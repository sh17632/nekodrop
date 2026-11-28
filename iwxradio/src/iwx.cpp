// SPDX-License-Identifier: GPL-2.0-only
/*
 * User-mode Intel 22000-family driver. Derived from itlwm's hal_iwx
 * (ItlIwx.cpp, GPL-2.0, by zxystd, extended by retropineaple's ForbiddenItlwm),
 * which is itself a port of OpenBSD's if_iwx (ISC, Stefan Sperling et al.),
 * which follows Linux iwlwifi (dual BSD/GPL). Function names keep the iwx_
 * vocabulary where a step corresponds one to one, so the two can be read side
 * by side.
 */
#include "iwx.h"

#include "plat.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <intrin.h>

#include "iwxreg.h"
#include "iwx_channel.h"

namespace {

/* The Bz firmware validates the peripheral scratch against the layout Linux
 * uses: control 84 bytes, the DRAM map at 124, and the fseq tail included in
 * version.size. */
static_assert(sizeof(iwx_prph_scratch_ctrl_cfg) == 84, "prph scratch ctrl_cfg");
static_assert(offsetof(iwx_prph_scratch, dram.common) == 124, "prph scratch DRAM map");
static_assert(sizeof(iwx_prph_scratch) == 1724, "prph scratch total");

/* struct ieee80211_frame is only used as a flexible array in the header. */
/* The 22000 firmware walks every TFD queue as 256 entries whatever size it is
 * told: a 64-entry ring made it transmit the 192 "frames" of memory behind
 * the ring at every wrap, and assert on one of them (0x4216). The AX210
 * family's pointers run to 65536 and its rings may be smaller: 128, Linux's
 * min_txq_size there, halves the adjacent pages its rings need (issue #11). */
constexpr unsigned kTxQueueSize22000 = IWX_TFD_QUEUE_SIZE_MAX;
constexpr unsigned kTxQueueSizeAx210 = 128;

/* Depth of the A-MPDU ring: as deep as the firmware can fill in one PPDU. The
 * AX210 family accepts 256 frames per aggregate (IWX_TFD_QUEUE_SIZE_MAX, the
 * same size the reference driver allocates its BA queues with). At 128 entries
 * the firmware never combined our submissions: every TX response reported
 * frame_count 1, no Block Ack was ever received, and throughput was bounded by
 * the peer's per-frame ACK latency (0.3-1.1 MB/s to an iPhone 14 Pro). At 256
 * the same link aggregates up to 63 frames per PPDU and moves 6.6-20 MB/s. */
constexpr unsigned kAggQueueSize = IWX_TFD_QUEUE_SIZE_MAX;
constexpr unsigned kTxPayloadSlot = 4096;
/* Every PNVM section shipped is at most 14 KiB (4 pages). */
constexpr size_t kPnvmPagesMax = 4;
/* More pages are locked, a batch at a time, until the buffers fit; always
 * leaves at least 1 GiB (or 25% of pre-lock free RAM) for Windows. */
constexpr int kDmaGrowRounds = 12;
/* Pages locked while carving: enough that runs of 8 adjacent frames turn up
 * for the firmware's 32 KiB sections; the rest are returned once carved. */
constexpr size_t kDmaScanPages = 262144; /* 1 GiB, locked for well under a second */
constexpr size_t kDmaMinGrowPages = 16384; /* 64 MiB minimum partial grow step */
constexpr size_t kDmaMinPages = 8192;

const uint8_t kNvmChannels[] = {
    1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  36,  40,
    44,  48,  52,  56,  60,  64,  68,  72,  76,  80,  84,  88,  92,  96,  100, 104,
    108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165, 169,
    173, 177, 181};

std::string hex(uint32_t v) {
  char b[16];
  plat_snprintf(b, sizeof(b), "0x%08x", v);
  return b;
}

/* Header length of an 802.11 frame, as net80211's ieee80211_get_hdrlen. */
size_t hdrlen_80211(const uint8_t *f, size_t len) {
  if (len < 10) return 0;
  const uint8_t type = f[0] & 0x0c;
  if (type == 0x04) { /* control */
    const uint8_t sub = f[0] & 0xf0;
    return (sub == 0xc0 || sub == 0xd0) ? 10 : 16; /* CTS/ACK : RTS etc. */
  }
  size_t h = 24;
  if ((f[1] & 0x03) == 0x03) h += 6;       /* four addresses */
  if (type == 0x08 && (f[0] & 0x80)) h += 2; /* QoS data */
  if ((f[1] & 0x80) && type == 0x08 && (f[0] & 0x80)) h += 4; /* +HTC */
  return h <= len ? h : 0;
}

uint16_t offload_header(unsigned hdrlen) {
  return static_cast<uint16_t>(((hdrlen / 2) << 8) | ((hdrlen % 4) ? (1u << 13) : 0));
}

uint8_t ofdm_plcp(unsigned mbps_x2) {
  switch (mbps_x2) {
  case 12: return 0xd;
  case 18: return 0xf;
  case 24: return 0x5;
  case 36: return 0x7;
  case 48: return 0x9;
  case 72: return 0xb;
  case 96: return 0x1;
  case 108: return 0x3;
  default: return 0;
  }
}

int ofdm_index(unsigned mbps_x2) {
  static const unsigned rates[] = {12, 18, 24, 36, 48, 72, 96, 108};
  for (int i = 0; i < 8; i++)
    if (rates[i] == mbps_x2) return i;
  return -1;
}

int cck_index(unsigned mbps_x2) {
  static const unsigned rates[] = {2, 4, 11, 22};
  for (int i = 0; i < 4; i++)
    if (rates[i] == mbps_x2) return i;
  return -1;
}

uint8_t cck_plcp(unsigned mbps_x2) {
  switch (mbps_x2) {
  case 2: return 10;
  case 4: return 20;
  case 11: return 55;
  case 22: return 110;
  default: return 0;
  }
}

uint32_t le32_at(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint16_t le16_at(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

/* Per-chunk maximum SEC_RT sizes across all PNVM_SKU blocks matching (mac, rf),
 * used to pre-allocate fragmented PNVM buffers before DmaPool::trim(). */
std::vector<size_t> pnvm_max_chunks(const uint8_t *d, size_t len, uint32_t mac, uint32_t rf) {
  std::vector<size_t> best, cur;
  bool in_block = false, hw = false;
  const auto finish = [&] {
    if (!in_block || !hw) return;
    if (cur.size() > best.size()) best.resize(cur.size(), 0);
    for (size_t i = 0; i < cur.size(); i++) best[i] = std::max(best[i], cur[i]);
  };
  while (len >= 8) {
    const uint32_t type = le32_at(d), tl = le32_at(d + 4);
    d += 8;
    len -= 8;
    if (len < tl) break;
    if (type == IWX_UCODE_TLV_PNVM_SKU) {
      finish();
      in_block = true;
      hw = false;
      cur.clear();
    } else if (in_block && type == IWX_UCODE_TLV_HW_TYPE && tl >= 4) {
      hw = hw || (le16_at(d) == mac && le16_at(d + 2) == rf);
    } else if (in_block && type == IWX_UCODE_TLV_SEC_RT && tl >= 4 && le32_at(d) != 0xddddeeee) {
      if (cur.size() < IWX_IPC_DRAM_MAP_ENTRY_NUM_MAX) cur.push_back(tl - 4);
    }
    const size_t step = roundup(tl, 4);
    if (step > len) break;
    d += step;
    len -= step;
  }
  finish();
  return best;
}

} // namespace

IwxDevice::~IwxDevice() { stop(); }

/* ---------------------------------------------------------------- registers */

bool IwxDevice::poll_bit(uint32_t reg, uint32_t bits, uint32_t mask, unsigned timeout_us) {
  const uint64_t end = now_us() + timeout_us;
  for (;;) {
    const uint32_t v = rd(reg);
    if (v != 0xffffffffu && (v & mask) == (bits & mask)) return true;
    if (now_us() >= end || _proxy.failed()) return false;
    delay_us(10);
  }
}

bool IwxDevice::nic_lock() {
  if (_nic_locks > 0) {
    _nic_locks++;
    return true;
  }
  if (bz()) {
    setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ);
    delay_us(2);
    if (poll_bit(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_STATUS,
                 IWX_CSR_GP_CNTRL_REG_FLAG_MAC_STATUS, 150000)) {
      _nic_locks++;
      return true;
    }
    _log("acquiring the device (Bz MAC access) failed");
    return false;
  }
  setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
  delay_us(2);
  if (poll_bit(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN,
               IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY |
                   IWX_CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP,
               150000)) {
    _nic_locks++;
    return true;
  }
  _log("acquiring the device (MAC access) failed");
  return false;
}

void IwxDevice::nic_unlock() {
  if (_nic_locks > 0 && --_nic_locks == 0)
    clrb(IWX_CSR_GP_CNTRL,
         bz() ? IWX_CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ : IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
}

void IwxDevice::sw_reset() {
  if (bz()) {
    setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_SW_RESET);
    delay_us(20000);
  } else {
    setb(IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_SW_RESET);
    delay_us(5000);
  }
}

void IwxDevice::ring_rx_doorbell(unsigned idx) {
  if (bz()) wr(IWX_HBUS_TARG_WRPTR, idx | IWX_HBUS_TARG_WRPTR_RX_Q(0));
  else wr(IWX_RFH_Q0_FRBDCB_WIDX_TRG, idx);
}

/* The two accesses of a peripheral read go in one proxy call. */
uint32_t IwxDevice::read_prph(uint32_t addr) {
  std::vector<RegOp> ops{
      {IWX_HBUS_TARG_PRPH_RADDR, (addr & (gen3() ? 0x00ffffffu : 0x000fffffu)) | (3u << 24), 1},
      {IWX_HBUS_TARG_PRPH_RDAT, 0, 0}};
  return _proxy.batch(ops) ? ops[1].value : 0xffffffffu;
}

void IwxDevice::write_prph(uint32_t addr, uint32_t value) {
  std::vector<RegOp> ops{
      {IWX_HBUS_TARG_PRPH_WADDR, (addr & (gen3() ? 0x00ffffffu : 0x000fffffu)) | (3u << 24), 1},
      {IWX_HBUS_TARG_PRPH_WDAT, value, 1}};
  _proxy.batch(ops);
}

void IwxDevice::set_bits_prph(uint32_t addr, uint32_t bits) {
  if (nic_lock()) {
    write_prph(addr, read_prph(addr) | bits);
    nic_unlock();
  }
}

void IwxDevice::clear_bits_prph(uint32_t addr, uint32_t bits) {
  if (nic_lock()) {
    write_prph(addr, read_prph(addr) & ~bits);
    nic_unlock();
  }
}

/* ----------------------------------------------------------------- bring-up */

bool IwxDevice::identify(std::string &error) {
  if (_proxy.vendor() != 0x8086) {
    error = "the card bound to the proxy is not an Intel device";
    return false;
  }
  _hw_rev_raw = rd(IWX_CSR_HW_REV);
  if (_hw_rev_raw == 0xffffffffu) {
    error = "the card does not answer (CSR_HW_REV reads all ones)";
    return false;
  }
  /* 8000 family and later: the step is in bits 0-1; keep the old layout. */
  _hw_rev = (_hw_rev_raw & 0xfff0) | (IWX_CSR_HW_REV_STEP(_hw_rev_raw << 2) << 2);
  _hw_rf_id = rd(IWX_CSR_HW_RF_ID);
  /* The card's family, transport parameters and firmware (iwx_attach's
   * iwx_devices / iwl_dev_info_table walk, cards.h). */
  const uint16_t device = _opt.family_device ? _opt.family_device : static_cast<uint16_t>(_proxy.device());
  _card = cards::select(device, _hw_rev, _hw_rf_id);
  _log("card " + hex(_proxy.vendor()) + ":" + hex(_proxy.device()) + " subsystem " +
       hex(_proxy.subsystem()) + ", hw_rev " + hex(_hw_rev_raw) + " rf_id " + hex(_hw_rf_id) +
       (_opt.family_device ? ", run as " + hex(_opt.family_device) + " (model chosen in settings)" : ""));
  if (!_card.known) {
    error = "unsupported card: " + _card.why;
    return false;
  }
  _fw_name = _card.firmware;
  _integrated = _card.integrated;
  _log(_card.name + (_card.bz ? " (Bz family" : _card.ax210 ? " (AX210 family" : " (22000 family") +
       (_integrated ? ", integrated)" : ", discrete)") + ", firmware " + _fw_name +
       (_card.pnvm.empty() ? "" : " + " + _card.pnvm) +
       (_card.tested ? "" : "; this card has not been tested with nekodrop"));
  return true;
}

bool IwxDevice::alloc_rings(std::string &error) {
  if (!_dma.init(kDmaScanPages, kDmaMinPages, error)) return false;
  /* What needs physically adjacent frames, in pages: each multi-page
   * firmware section, the descriptor rings, the receive lists and the PNVM. */
  const auto pages_of = [](size_t bytes) { return (bytes + DmaPool::kPageBytes - 1) / DmaPool::kPageBytes; };
  /* Data rings stay 256 entries on Bz (its min_txq_size is 128, so 256 is
   * accepted and is what the AX203 ran with on hardware); AX210 keeps 128. */
  const unsigned tx_count = bz() ? kTxQueueSize22000 : gen3() ? kTxQueueSizeAx210 : kTxQueueSize22000;
  const unsigned cmd_count = gen3() ? IWX_CMD_QUEUE_SIZE_GEN3 : IWX_CMD_QUEUE_SIZE;
  const size_t rx_used_elem = bz()     ? sizeof(iwx_rx_completion_desc_bz)
                              : gen3() ? sizeof(iwx_rx_completion_desc)
                                       : sizeof(uint32_t);
  const size_t bc_tbl_size = bz() ? sizeof(iwx_bz_bc_tbl)
                                  : std::max(sizeof(iwx_agn_scd_bc_tbl), sizeof(iwx_gen3_bc_tbl));
  std::vector<size_t> need;
  for (const auto &sec : _fw.regular) need.push_back(pages_of(sec.len));
  need.push_back(pages_of(_fw.iml_len));
  need.push_back(pages_of(cmd_count * sizeof(iwx_tfh_tfd)));
  need.push_back(pages_of(tx_count * sizeof(iwx_tfh_tfd)));
  need.push_back(pages_of(tx_count * sizeof(iwx_tfh_tfd)));
  need.push_back(pages_of(tx_count * sizeof(iwx_tfh_tfd))); /* _sta_aggq */
  if (_opt.aggregate) need.push_back(pages_of(kAggQueueSize * sizeof(iwx_tfh_tfd)));
  if (bz()) {
    for (int i = 0; i < (_opt.aggregate ? 5 : 4); i++) need.push_back(pages_of(bc_tbl_size));
  }
  need.push_back(pages_of(IWX_RX_MQ_RING_COUNT * rx_used_elem));
  need.push_back(pages_of(IWX_RX_MQ_RING_COUNT * (gen3() ? sizeof(iwx_rx_transfer_desc) : sizeof(uint64_t))));
  if (gen3()) {
    if (_fw.has_capa(IWX_UCODE_TLV_CAPA_FRAGMENTED_PNVM_IMG) && _fw.pnvm && _fw.pnvm_len) {
      for (size_t sz : pnvm_max_chunks(_fw.pnvm, _fw.pnvm_len, cards::mac_type(_hw_rev), cards::rf_type(_hw_rf_id)))
        need.push_back(pages_of(sz));
    } else {
      need.push_back(kPnvmPagesMax);
    }
  }
  const auto report = [&](const char *what) {
    _log(std::string("DMA: ") + what + " " + std::to_string(_dma.pages_locked()) + " pages, longest physical run " +
         std::to_string(_dma.longest_run()) + " pages, " + std::to_string(_dma.runs_of(8)) + " runs of 8+; " +
         (_dma.fits(need) ? "enough" : "not enough") + " for this card's " +
         std::to_string(std::count_if(need.begin(), need.end(), [](size_t n) { return n > 1; })) +
         " multi-page buffers");
  };
  report("locked");
  /* A long-running PC's free memory (especially with Hyper-V/WSL/VMware, #30)
   * can be so scattered that 1-2 GiB holds few runs of adjacent frames. Lock
   * more while always keeping max(1 GiB, 25% of pre-lock free RAM) for Windows;
   * trim() returns ~99.5% of these pages within a second. */
  for (int round = 0; round < kDmaGrowRounds && !_dma.fits(need); round++) {
    const uint64_t locked = uint64_t(_dma.pages_locked()) * DmaPool::kPageBytes;
    const uint64_t avail = DmaPool::available_bytes();
    const uint64_t pre_lock_free = locked + avail;
    const uint64_t reserve_for_os = std::max<uint64_t>(1024ULL << 20, pre_lock_free / 4);
    const uint64_t headroom = avail > reserve_for_os ? avail - reserve_for_os : 0;
    const size_t step_pages = static_cast<size_t>(
        std::min<uint64_t>(kDmaScanPages, headroom / DmaPool::kPageBytes));
    if (step_pages < kDmaMinGrowPages) {
      _log("DMA: not locking more; Windows has " + std::to_string(avail >> 20) + " MiB free");
      break;
    }
    std::string why;
    if (!_dma.grow(step_pages, why)) {
      _log("DMA: " + why);
      break;
    }
    report("grew to");
  }
  _rx_buf_bytes = IWX_RBUF_SIZE;
  bool use_8k_rx = false;
  if (_opt.he80) {
    std::vector<size_t> probe = need;
    probe.insert(probe.end(), IWX_RX_MQ_RING_COUNT, size_t(2));
    use_8k_rx = _dma.fits(probe);
    if (use_8k_rx) {
      need = std::move(probe);
      _rx_buf_bytes = 8192;
      _log("rx: 8 KiB DMA buffers enabled (A-MSDU high-throughput)");
    } else {
      _log("rx: keeping 4 KiB DMA buffers (insufficient 2-page contiguous runs)");
    }
  }
#ifndef NEKO_KERNEL
  if (!_dma.reserve_runs(need))
    _log("DMA: not enough free runs to reserve all rings and firmware sections");
#endif

  _ctxt_info = _dma.alloc(std::max(sizeof(iwx_context_info), sizeof(iwx_context_info_gen3)), 4096);
  /* Receive: free list, used list, status word, 512 buffers (4 KiB default, 8 KiB with --he80). */
  _rx_free = _dma.alloc(IWX_RX_MQ_RING_COUNT * (gen3() ? sizeof(iwx_rx_transfer_desc) : sizeof(uint64_t)), 256);
  _rx_used = _dma.alloc(IWX_RX_MQ_RING_COUNT * rx_used_elem, 256);
  _rx_stat = _dma.alloc(sizeof(iwx_rb_status), 16);
  if (!_ctxt_info || !_rx_free || !_rx_used || !_rx_stat) {
    error = "out of DMA memory for the rings";
    return false;
  }
  _rx_bufs.resize(IWX_RX_MQ_RING_COUNT);
  for (unsigned i = 0; i < IWX_RX_MQ_RING_COUNT; i++) {
    _rx_bufs[i] = _rx_buf_bytes == 8192 ? _dma.alloc(8192, 4096) : _dma.alloc_page();
    if (!_rx_bufs[i]) {
      error = "out of DMA memory for receive buffers";
      return false;
    }
    post_rx_buf(i);
  }

  const auto make_ring = [&](TxRing &ring, unsigned count, unsigned qid, size_t payload) {
    ring.count = count;
    ring.qid = qid;
    ring.desc = _dma.alloc(count * sizeof(iwx_tfh_tfd), bz() ? 4096 : 256);
    ring.bc_tbl = _dma.alloc(bc_tbl_size, bz() ? 4096 : 1024);
    bool ok = ring.desc && ring.bc_tbl;
    if (payload) {
      /* Data ring: a page per slot for the TX command and one for the
       * payload, so only the descriptors need adjacent frames. */
      ring.payload.resize(count);
      ring.cmd_slot.resize(count);
      for (auto &slot : ring.payload) ok = ok && (slot = _dma.alloc_page());
      for (auto &slot : ring.cmd_slot) ok = ok && (slot = _dma.alloc_page());
    }
    /* The command ring's commands each take a page of _cmd_big: 128 commands
     * side by side needed 11 adjacent pages (issue #11). */
    return ok;
  };
  /* The AX210 family's command ring is 128 entries (IWX_CMD_QUEUE_SIZE_GEN3,
   * the size the gen3 context info announces); upstream's 1024-entry data
   * rings would need 64 physically adjacent pages for the descriptors alone. */
  if (!make_ring(_cmdq, cmd_count, IWX_DQA_CMD_QUEUE, 0) ||
      !make_ring(_dataq, tx_count, 0, kTxPayloadSlot) ||
      !make_ring(_staq, tx_count, 0, kTxPayloadSlot)) {
    error = "out of DMA memory for the transmit rings (longest remaining run of adjacent pages: " +
            std::to_string(_dma.longest_run()) + "; " + std::to_string(pages_of(tx_count * sizeof(iwx_tfh_tfd))) +
            " needed)";
    return false;
  }
  /* Station TX A-MPDU ring (TID 0): optional, falls back to _staq if DMA is tight. */
  if (!make_ring(_sta_aggq, tx_count, 0, kTxPayloadSlot)) {
    _log("station: no DMA memory for TX aggregation ring; sending unaggregated");
    _sta_aggq = TxRing{};
  }
  /* Experimental A-MPDU sending: its own ring for the peer's TID 0. Only when
   * asked for, so the default memory need is unchanged; without it the radio
   * just sends as before. */
  if (_opt.aggregate && !make_ring(_aggq, kAggQueueSize, 0, kTxPayloadSlot)) {
    _log("aggregate: no DMA memory for its ring; sending without A-MPDU");
    _aggq = TxRing{};
  }
  _cmd_slots.resize(cmd_count);
  _cmd_big.resize(cmd_count);
  for (auto &big : _cmd_big) {
    big = _dma.alloc_page();
    if (!big) {
      error = "out of DMA memory for command buffers";
      return false;
    }
  }
  return true;
}

bool IwxDevice::set_hw_ready() {
  setb(IWX_CSR_HW_IF_CONFIG_REG, IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);
  const bool ready = poll_bit(IWX_CSR_HW_IF_CONFIG_REG, IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,
                              IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY, 50);
  if (ready) setb(IWX_CSR_MBOX_SET_REG, IWX_CSR_MBOX_SET_REG_OS_ALIVE);
  return ready;
}

bool IwxDevice::prepare_card_hw() {
  if (set_hw_ready()) return true;
  setb(IWX_CSR_DBG_LINK_PWR_MGMT_REG, IWX_CSR_RESET_LINK_PWR_MGMT_DISABLED);
  delay_us(1000);
  for (int tries = 0; tries < 10; tries++) {
    setb(IWX_CSR_HW_IF_CONFIG_REG, IWX_CSR_HW_IF_CONFIG_REG_PREPARE);
    const uint64_t end = now_us() + 150000;
    do {
      if (rd(IWX_CSR_HW_IF_CONFIG_REG) == 0xffffffffu) return false;
      if (set_hw_ready()) return true;
      delay_us(200);
    } while (now_us() < end);
    delay_us(25000);
  }
  return false;
}

void IwxDevice::clear_persistence_bit() {
  if (gen3()) return;
  const uint32_t hpm = read_prph(IWX_HPM_DEBUG);
  if (hpm != 0xa5a5a5a0 && (hpm & IWX_PERSISTENCE_BIT)) {
    if (read_prph(IWX_PREG_PRPH_WPROT_22000) & IWX_PREG_WFPM_ACCESS) {
      _log("cannot clear the persistence bit");
      return;
    }
    write_prph(IWX_HPM_DEBUG, hpm & ~IWX_PERSISTENCE_BIT);
  }
}

void IwxDevice::force_power_gating() {
  set_bits_prph(IWX_HPM_HIPM_GEN_CFG, IWX_HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE);
  delay_us(20);
  set_bits_prph(IWX_HPM_HIPM_GEN_CFG,
                IWX_HPM_HIPM_GEN_CFG_CR_PG_EN | IWX_HPM_HIPM_GEN_CFG_CR_SLP_EN);
  delay_us(20);
  clear_bits_prph(IWX_HPM_HIPM_GEN_CFG, IWX_HPM_HIPM_GEN_CFG_CR_FORCE_ACTIVE);
}

bool IwxDevice::apm_init() {
  setb(IWX_CSR_GIO_CHICKEN_BITS, IWX_CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);
  setb(IWX_CSR_DBG_HPET_MEM_REG, IWX_CSR_DBG_HPET_MEM_REG_VAL);
  setb(IWX_CSR_HW_IF_CONFIG_REG, IWX_CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A);
  /* iwx_apm_config also clears ASPM in PCI config space, which a UMDF
   * driver cannot reach; the L0s bit here is the part within the BAR. */
  setb(IWX_CSR_GIO_REG, IWX_CSR_GIO_REG_VAL_L0S_DISABLED);
  if (bz()) {
    setb(IWX_CSR_GP_CNTRL,
         IWX_CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ | IWX_CSR_GP_CNTRL_REG_FLAG_MAC_INIT);
    if (!poll_bit(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_STATUS,
                  IWX_CSR_GP_CNTRL_REG_FLAG_MAC_STATUS, 25000)) {
      _log("timeout waiting for the Bz MAC clock to stabilise");
      return false;
    }
    return true;
  }
  setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
  if (!poll_bit(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, 25000)) {
    _log("timeout waiting for the clock to stabilise");
    return false;
  }
  return true;
}

void IwxDevice::apm_stop() {
  setb(IWX_CSR_DBG_LINK_PWR_MGMT_REG, IWX_CSR_RESET_LINK_PWR_MGMT_DISABLED);
  setb(IWX_CSR_HW_IF_CONFIG_REG,
       IWX_CSR_HW_IF_CONFIG_REG_PREPARE | IWX_CSR_HW_IF_CONFIG_REG_ENABLE_PME);
  delay_us(1000);
  clrb(IWX_CSR_DBG_LINK_PWR_MGMT_REG, IWX_CSR_RESET_LINK_PWR_MGMT_DISABLED);
  delay_us(5000);
  if (bz()) {
    setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_REQ);
    if (!poll_bit(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_STATUS,
                  IWX_CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_STATUS, 100))
      _log("timeout waiting for the Bz bus master to stop");
    delay_us(20000);
    clrb(IWX_CSR_GP_CNTRL,
         IWX_CSR_GP_CNTRL_REG_FLAG_MAC_INIT | IWX_CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ);
    return;
  }
  setb(IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_STOP_MASTER);
  if (!poll_bit(IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED,
                IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED, 100))
    _log("timeout waiting for the bus master to stop");
  clrb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
}

/* Nothing here takes interrupts: every cause stays masked, so the card never
 * raises a line no handler listens to. CSR_INT still latches the causes. */
void IwxDevice::mask_interrupts() {
  wr(IWX_CSR_INT_MASK, 0);
  wr(IWX_CSR_INT, ~0u);
  wr(IWX_CSR_FH_INT_STATUS, ~0u);
  wr(IWX_CSR_MSIX_FH_INT_MASK_AD, ~0u);
  wr(IWX_CSR_MSIX_HW_INT_MASK_AD, ~0u);
}

bool IwxDevice::start_hw() {
  if (!prepare_card_hw()) {
    _log("the card did not become ready");
    return false;
  }
  clear_persistence_bit();
  if (bz() && _integrated) {
    /* Integrated Bz (CNVi, e.g. AX203) needs MAC_INIT before nic_lock, so
     * apm_init runs once before the reset and once after. The AUX bus is
     * claimed for the MAC before the reset (Linux get_crf_id leaves it held),
     * and CNVI_AUX_MISC_CHIP reports the MAC step. Discrete BE200 (Gl,
     * !_integrated) has no CNVi AUX block and carries its step in CSR_HW_REV. */
    if (!apm_init()) return false;
    if (!nic_lock()) {
      _log("Bz: MAC access failed before the reset");
      return false;
    }
    const uint32_t wfpm_before = read_umac_prph(IWX_WFPM_CTRL_REG);
    write_umac_prph(IWX_WFPM_CTRL_REG, wfpm_before | IWX_WFPM_AUX_CTL_AUX_IF_MAC_OWNER_MSK);
    const uint32_t crf = read_prph(IWX_SD_REG_VER_GEN2);
    const uint32_t cnv = read_prph(IWX_CNVI_AUX_MISC_CHIP);
    const uint32_t otp = read_umac_prph(IWX_WFPM_OTP_CFG1_ADDR);
    const uint32_t ps = read_umac_prph(IWX_WFPM_PS_CTL_CLR);
    const uint32_t req4 = read_umac_prph(IWX_WFPM_RSRCS_4PHS_REQ_STTS);
    const uint32_t ack4 = read_umac_prph(IWX_WFPM_RSRCS_4PHS_ACK_STTS);
    const uint32_t wfpm_owned = read_umac_prph(IWX_WFPM_CTRL_REG);
    const std::string boot_before = bz_boot_regs();
    nic_unlock();
    _log("Bz before reset: " + boot_before);
    _hw_rev_step = IWX_CNVI_AUX_MISC_CHIP_MAC_STEP(cnv);
    if (IWX_CNVI_AUX_MISC_CHIP_PROD_TYPE(cnv) == IWX_CNVI_AUX_MISC_CHIP_PROD_TYPE_BZ_U &&
        _hw_rev_step == IWX_SILICON_A_STEP)
      _hw_rev_step = IWX_SILICON_B_STEP;
    _log("Bz before reset: WFPM_CTRL " + hex(wfpm_before) + " -> " + hex(wfpm_owned) +
         ", otp " + hex(otp) + ", ps " + hex(ps) + ", 4phs req " + hex(req4) + " ack " +
         hex(ack4));
    _log("Bz: crf-id " + hex(crf) + ", cnv-id " + hex(cnv) + ", MAC step " +
         std::to_string(_hw_rev_step) + " (CSR_HW_REV step " +
         std::to_string(IWX_CSR_HW_REV_STEP(_hw_rev)) + ")");

    setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_SW_RESET);
    delay_us(10000);
    if (!prepare_card_hw()) {
      _log("the card did not come back after the Bz reset");
      return false;
    }
    if (!apm_init()) return false;
    if (nic_lock()) {
      const uint32_t wfpm_after = read_umac_prph(IWX_WFPM_CTRL_REG);
      const uint32_t req4_after = read_umac_prph(IWX_WFPM_RSRCS_4PHS_REQ_STTS);
      const uint32_t ack4_after = read_umac_prph(IWX_WFPM_RSRCS_4PHS_ACK_STTS);
      const std::string boot_after = bz_boot_regs();
      nic_unlock();
      _log("Bz after reset: " + boot_after);
      _log("Bz after reset: WFPM_CTRL " + hex(wfpm_after) +
           (wfpm_after & IWX_WFPM_AUX_CTL_AUX_IF_MAC_OWNER_MSK ? " (owner still set)"
                                                                : " (owner cleared)") +
           ", 4phs req " + hex(req4_after) + " ack " + hex(ack4_after));
    }
  } else {
    sw_reset();
    if (_integrated && !gen3()) {
      setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
      delay_us(20);
      if (!poll_bit(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                    IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, 25000)) {
        _log("timeout waiting for the clock before power gating");
        return false;
      }
      force_power_gating();
      setb(IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_SW_RESET);
      delay_us(5000);
    }
    if (!apm_init()) return false;
  }
  /* MSI rather than MSI-X causes, so CSR_INT reports them (iwx_conf_msix_hw
   * with sc_msix = 0). */
  if (nic_lock()) {
    write_umac_prph(IWX_UREG_CHICK, IWX_UREG_CHICK_MSI_ENABLE);
    nic_unlock();
  }
  mask_interrupts();
  setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_RFKILL_WAKE_L1A_EN);
  const bool rfkill = (rd(IWX_CSR_GP_CNTRL) & IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW) == 0;
  if (rfkill) _log("warning: the hardware radio switch reports the radio off");
  return true;
}

void IwxDevice::nic_config() {
  const uint32_t cfg = _fw.phy_config;
  const uint32_t radio_type = (cfg & IWX_FW_PHY_CFG_RADIO_TYPE) >> IWX_FW_PHY_CFG_RADIO_TYPE_POS;
  const uint32_t radio_step = (cfg & IWX_FW_PHY_CFG_RADIO_STEP) >> IWX_FW_PHY_CFG_RADIO_STEP_POS;
  const uint32_t radio_dash = (cfg & IWX_FW_PHY_CFG_RADIO_DASH) >> IWX_FW_PHY_CFG_RADIO_DASH_POS;
  uint32_t reg = 0;
  reg |= IWX_CSR_HW_REV_STEP(_hw_rev) << IWX_CSR_HW_IF_CONFIG_REG_POS_MAC_STEP;
  reg |= IWX_CSR_HW_REV_DASH(_hw_rev) << IWX_CSR_HW_IF_CONFIG_REG_POS_MAC_DASH;
  reg |= radio_type << IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE;
  reg |= radio_step << IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_STEP;
  reg |= radio_dash << IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_DASH;
  const uint32_t mask =
      IWX_CSR_HW_IF_CONFIG_REG_MSK_MAC_DASH | IWX_CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP |
      IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP | IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH |
      IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE | IWX_CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI |
      IWX_CSR_HW_IF_CONFIG_REG_BIT_MAC_SI;
  wr(IWX_CSR_HW_IF_CONFIG_REG, (rd(IWX_CSR_HW_IF_CONFIG_REG) & ~mask) | reg);
}

void IwxDevice::set_ltr() {
  if (bz()) return; /* Linux iwl_pcie_set_ltr skips Bz */
  const uint32_t ltr =
      IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_REQ |
      (IWX_CSR_LTR_LONG_VAL_AD_SCALE_USEC << 26) /* NO_SNOOP_SCALE */ |
      (250u << 16) /* NO_SNOOP_VAL */ | IWX_CSR_LTR_LONG_VAL_AD_SNOOP_REQ |
      (IWX_CSR_LTR_LONG_VAL_AD_SCALE_USEC << 10) /* SNOOP_SCALE */ | 250u;
  /* Discrete cards take it in a CSR, integrated 22000 parts in the HPM;
   * integrated AX210 parts leave it to the firmware (iwx_set_ltr). */
  if (!_integrated) {
    wr(IWX_CSR_LTR_LONG_VAL_AD, ltr);
  } else if (!gen3()) {
    if (nic_lock()) {
      write_prph(IWX_HPM_MAC_LTR_CSR, IWX_HPM_MAC_LRT_ENABLE_ALL);
      write_prph(IWX_HPM_UMAC_LTR, ltr);
      nic_unlock();
    }
  }
}

/* iwx_start_fw + iwx_ctxt_info_init + iwx_load_firmware, waiting for ALIVE
 * by polling instead of sleeping on the interrupt. */
namespace {
/* The largest PNVM section (SEC_RT data of one PNVM_SKU block) for this MAC
 * and radio type, to reserve its buffer before the SKU is known. */
size_t pnvm_max_section(const std::vector<uint8_t> &raw, uint32_t mac, uint32_t rf) {
  const auto u32 = [](const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
  const auto u16 = [](const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; };
  size_t best = 0, cur = 0;
  bool in_block = false, hw = false;
  const uint8_t *d = raw.data();
  size_t len = raw.size();
  while (len >= 8) {
    const uint32_t type = u32(d), tl = u32(d + 4);
    d += 8;
    len -= 8;
    if (len < tl) break;
    if (type == IWX_UCODE_TLV_PNVM_SKU) {
      if (in_block && hw) best = std::max(best, cur);
      in_block = true;
      hw = false;
      cur = 0;
    } else if (in_block && type == IWX_UCODE_TLV_HW_TYPE && tl >= 4) {
      hw = hw || (u16(d) == mac && u16(d + 2) == rf);
    } else if (in_block && type == IWX_UCODE_TLV_SEC_RT && tl >= 4 && u32(d) != 0xddddeeee) {
      cur += tl - 4;
    }
    const size_t step = roundup(tl, 4);
    if (step > len) break;
    d += step;
    len -= step;
  }
  if (in_block && hw) best = std::max(best, cur);
  return best;
}
} // namespace

bool IwxDevice::load_firmware(std::string &error) {
  if (!prepare_card_hw()) {
    error = "card not ready before firmware load";
    return false;
  }
  wr(IWX_CSR_INT, 0xffffffffu);
  mask_interrupts();
  wr(IWX_CSR_UCODE_DRV_GP1_CLR, IWX_CSR_UCODE_SW_BIT_RFKILL);
  wr(IWX_CSR_UCODE_DRV_GP1_CLR, IWX_CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);
  wr(IWX_CSR_INT, ~0u);

  /* iwx_nic_init */
  if (!apm_init()) {
    error = "APM init failed";
    return false;
  }
  /* Not on the AX210 family: OpenBSD's AX210 support (commit 4735940a10,
   * tested on AX210 hardware) stopped calling iwx_nic_config there. The
   * MAC/PHY step and radio type it writes into CSR_HW_IF_CONFIG_REG follow
   * the 22000 layout; on AX210-family cards the firmware asserted at
   * NVM_ACCESS_COMPLETE (issue #7). */
  if (!gen3()) nic_config();
  _proxy.write8(IWX_CSR_INT_COALESCING, IWX_HOST_INT_TIMEOUT_DEF);
  setb(IWX_CSR_MAC_SHADOW_REG_CTRL, 0x800fffff);

  /* Firmware sections into DMA memory: LMAC, separator, UMAC, separator,
   * paging (iwx_init_fw_sec). The 22000 family finds them through the
   * context info, the AX210 family through the peripheral scratch. */
  iwx_context_info *ci = nullptr;
  iwx_prph_scratch *scratch = nullptr;
  iwx_context_info_dram *dram = nullptr;
  if (gen3()) {
    _prph_scratch = _dma.alloc(sizeof(iwx_prph_scratch), 4096);
    _prph_info = _dma.alloc_page();
    if (!_prph_scratch || !_prph_info) {
      error = "out of DMA memory for the peripheral scratch";
      return false;
    }
    scratch = reinterpret_cast<iwx_prph_scratch *>(_prph_scratch.va);
    std::memset(scratch, 0, sizeof(*scratch));
    dram = &scratch->dram.common;
  } else {
    ci = reinterpret_cast<iwx_context_info *>(_ctxt_info.va);
    std::memset(ci, 0, sizeof(*ci));
    dram = &ci->dram;
  }
  const auto &secs = _fw.regular;
  const auto count_until_sep = [&](size_t start) {
    size_t n = 0;
    while (start < secs.size() && secs[start].devoff != IWX_CPU1_CPU2_SEPARATOR_SECTION &&
           secs[start].devoff != IWX_PAGING_SEPARATOR_SECTION) {
      start++;
      n++;
    }
    return n;
  };
  const size_t lmac = count_until_sep(0);
  const size_t umac = count_until_sep(lmac + 1);
  const size_t paging = count_until_sep(lmac + umac + 2);
  if (lmac > nitems(dram->lmac_img) || umac > nitems(dram->umac_img) ||
      paging > nitems(dram->virtual_img)) {
    error = "firmware has more sections than the context info holds";
    return false;
  }
  const auto place = [&](const FwSection &sec, uint64_t &slot) {
    DmaBuf b = _dma.alloc(sec.len, 4096);
    if (!b) return false;
    std::memcpy(b.va, sec.data, sec.len);
    slot = htole64(b.pa);
    _fw_sections.push_back(b);
    return true;
  };
  bool placed = true;
  for (size_t i = 0; placed && i < lmac; i++) placed = place(secs[i], dram->lmac_img[i]);
  for (size_t i = 0; placed && i < umac; i++) placed = place(secs[lmac + 1 + i], dram->umac_img[i]);
  for (size_t i = 0; placed && i < paging; i++)
    placed = place(secs[lmac + umac + 2 + i], dram->virtual_img[i]);
  if (gen3() && placed) {
    if (!_fw.iml || !_fw.iml_len) {
      error = "the AX210-family firmware has no image loader (IML)";
      return false;
    }
    _iml_buf = _dma.alloc(_fw.iml_len, 4096);
    placed = static_cast<bool>(_iml_buf);
    if (placed) std::memcpy(_iml_buf.va, _fw.iml, _fw.iml_len);
  }
  if (!placed) {
    error = "out of DMA memory for the firmware image";
    return false;
  }
  /* The PNVM section is chosen after ALIVE (by the SKU the firmware reports),
   * but its buffer has to come from the pool before trim() hands the rest
   * back: allocated afterwards it never fit ("PNVM: out of DMA memory"), and
   * the firmware asserted at NVM_ACCESS_COMPLETE (issues #2, #7, #8). */
  if (gen3() && (_fw.pnvm || !_card.pnvm.empty())) {
    const uint8_t *pnvm_data = _fw.pnvm;
    size_t pnvm_len = _fw.pnvm_len;
    if (!pnvm_data && !_card.pnvm.empty()) {
      std::string path = _opt.firmware_dir, err;
      if (!path.empty() && path.back() != '\\' && path.back() != '/') path += '\\';
      if (!plat_read_file(path + _card.pnvm, _pnvm_raw, err)) {
        _log("PNVM: " + err);
        _pnvm_raw.clear();
      } else {
        pnvm_data = _pnvm_raw.data();
        pnvm_len = _pnvm_raw.size();
      }
    }
    if (pnvm_data && pnvm_len) {
      if (_fw.has_capa(IWX_UCODE_TLV_CAPA_FRAGMENTED_PNVM_IMG)) {
        const auto chunks = pnvm_max_chunks(pnvm_data, pnvm_len, cards::mac_type(_hw_rev),
                                            cards::rf_type(_hw_rf_id));
        if (!chunks.empty()) {
          _pnvm_desc = _dma.alloc(sizeof(iwx_prph_scratch_mem_desc_addr_array), 4096);
          _pnvm_chunks.clear();
          _pnvm_chunks.reserve(chunks.size());
          bool ok = static_cast<bool>(_pnvm_desc);
          for (size_t sz : chunks) {
            DmaBuf b = _dma.alloc(sz, 4096);
            ok = ok && static_cast<bool>(b);
            _pnvm_chunks.push_back(b);
          }
          if (!ok) {
            error = "out of DMA memory for the fragmented platform NVM (PNVM)";
            return false;
          }
        }
      } else if (const size_t max_section = pnvm_max_section(_pnvm_raw, cards::mac_type(_hw_rev),
                                                             cards::rf_type(_hw_rf_id))) {
        /* Only this card's largest section (at most 4 pages), not the file. */
        _pnvm_buf = _dma.alloc(std::max(max_section, kPnvmPagesMax * DmaPool::kPageBytes), 4096);
        if (!_pnvm_buf) {
          error = "out of DMA memory for the platform NVM (PNVM)";
          return false;
        }
      }
    }
  }
  _dma.trim();
  _log("DMA: " + std::to_string(_dma.pages_used()) + " pages in use, the rest returned");
  _log("firmware " + _fw.version + ": " + std::to_string(lmac) + " LMAC, " +
       std::to_string(umac) + " UMAC, " + std::to_string(paging) + " paging sections");

  if (gen3()) {
    /* iwx_ctxt_info_gen3_init */
    auto &ctrl = scratch->ctrl_cfg;
    ctrl.version.version = 0;
    uint16_t mac_id = static_cast<uint16_t>(rd(IWX_CSR_HW_REV));
    if (bz() && _integrated) /* Linux info.hw_rev: CNVi MAC step from CNVI_AUX_MISC_CHIP */
      mac_id = static_cast<uint16_t>((mac_id & ~0x3u) | (_hw_rev_step & 0x3u));
    ctrl.version.mac_id = htole16(mac_id);
    ctrl.version.size = htole16(sizeof(*scratch) / 4);
    const uint32_t rb_flag =
        _rx_buf_bytes == 8192 ? (IWX_PRPH_SCRATCH_RB_SIZE_4K | IWX_PRPH_SCRATCH_RB_SIZE_EXT_8K)
                              : IWX_PRPH_SCRATCH_RB_SIZE_4K;
    uint32_t control_flags = rb_flag | IWX_PRPH_SCRATCH_MTR_MODE |
                             (IWX_PRPH_MTR_FORMAT_256B & IWX_PRPH_SCRATCH_MTR_FORMAT);
    if (_card.imr) control_flags |= kPrphScratchImrDebugEn;
    ctrl.control.control_flags = htole32(control_flags);
    ctrl.rbd_cfg.free_rbd_addr = htole64(_rx_free.pa);
    auto *g3 = reinterpret_cast<iwx_context_info_gen3 *>(_ctxt_info.va);
    std::memset(g3, 0, sizeof(*g3));
    g3->prph_info_base_addr = htole64(_prph_info.pa);
    g3->prph_scratch_base_addr = htole64(_prph_scratch.pa);
    g3->prph_scratch_size = htole32(static_cast<uint32_t>(
        offsetof(iwx_prph_scratch, dram.common) + sizeof(iwx_context_info_dram)));
    g3->cr_head_idx_arr_base_addr = htole64(_rx_stat.pa);
    g3->tr_tail_idx_arr_base_addr = htole64(_prph_info.pa + DmaPool::kPageBytes / 2);
    g3->cr_tail_idx_arr_base_addr = htole64(_prph_info.pa + 3 * DmaPool::kPageBytes / 4);
    g3->mtr_base_addr = htole64(_cmdq.desc.pa);
    g3->mcr_base_addr = htole64(_rx_used.pa);
    g3->mtr_size = htole16(static_cast<uint16_t>(IWX_TFD_QUEUE_CB_SIZE(IWX_CMD_QUEUE_SIZE_GEN3)));
    g3->mcr_size = htole16(static_cast<uint16_t>(IWX_RX_QUEUE_CB_SIZE(IWX_RX_MQ_RING_COUNT)));
  } else {
    ci->version.version = 0;
    ci->version.mac_id = htole16(static_cast<uint16_t>(rd(IWX_CSR_HW_REV)));
    ci->version.size = htole16(sizeof(*ci) / 4);
    const uint32_t rb_sz =
        _rx_buf_bytes == 8192 ? IWX_CTXT_INFO_RB_SIZE_8K : IWX_CTXT_INFO_RB_SIZE_4K;
    ci->control.control_flags =
        htole32(IWX_CTXT_INFO_TFD_FORMAT_LONG |
                (IWX_RX_QUEUE_CB_SIZE(IWX_MQ_RX_TABLE_SIZE) << IWX_CTXT_INFO_RB_CB_SIZE_POS) |
                (rb_sz << IWX_CTXT_INFO_RB_SIZE_POS));
    ci->rbd_cfg.free_rbd_addr = htole64(_rx_free.pa);
    ci->rbd_cfg.used_rbd_addr = htole64(_rx_used.pa);
    ci->rbd_cfg.status_wr_ptr = htole64(_rx_stat.pa);
    ci->hcmd_cfg.cmd_queue_addr = htole64(_cmdq.desc.pa);
    ci->hcmd_cfg.cmd_queue_size = IWX_TFD_QUEUE_CB_SIZE(IWX_CMD_QUEUE_SIZE);
  }

  /* From here the card may write into our memory: arm the stop the proxy
   * replays if this process dies (iwx_disable_rx_dma + stop master). */
  {
    std::vector<RegOp> close_ops =
        bz() ? std::vector<RegOp>{{IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_REQ, 2},
                                  {IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_SW_RESET, 2}}
             : std::vector<RegOp>{{IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_STOP_MASTER, 2},
                                  {IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_SW_RESET, 2}};
    if (!_proxy.set_close_ops(close_ops)) {
      error = "could not arm the DMA stop the driver replays on exit; refusing to start DMA";
      return false;
    }
  }

  _alive_seen = _alive_ok = false;
  if (gen3()) {
    wr(IWX_CSR_CTXT_INFO_ADDR, static_cast<uint32_t>(_ctxt_info.pa));
    wr(IWX_CSR_CTXT_INFO_ADDR + 4, static_cast<uint32_t>(_ctxt_info.pa >> 32));
    wr(IWX_CSR_IML_DATA_ADDR, static_cast<uint32_t>(_iml_buf.pa));
    wr(IWX_CSR_IML_DATA_ADDR + 4, static_cast<uint32_t>(_iml_buf.pa >> 32));
    wr(IWX_CSR_IML_SIZE_ADDR, static_cast<uint32_t>(_fw.iml_len));
    setb(IWX_CSR_CTXT_INFO_BOOT_CTRL, IWX_CSR_AUTO_FUNC_BOOT_ENA);
  } else {
    wr(IWX_CSR_CTXT_INFO_BA, static_cast<uint32_t>(_ctxt_info.pa));
    wr(IWX_CSR_CTXT_INFO_BA + 4, static_cast<uint32_t>(_ctxt_info.pa >> 32));
  }
  set_ltr();
  if (bz()) {
    wr(IWX_CSR_INT_MASK, IWX_CSR_INT_BIT_ALIVE | IWX_CSR_INT_BIT_FH_RX);
    wr(IWX_CSR_MSIX_HW_INT_MASK_AD,
       ~(IWX_MSIX_HW_INT_CAUSES_REG_ALIVE | IWX_MSIX_HW_INT_CAUSES_REG_IML_BZ));
  }
  if (bz()) {
    /* Bz's ROM takes the IML after a FUNC_SCRATCH handshake and ROM_START,
     * and BOOT_CTRL must be set bit by bit: the ROM preloads bits 11/14/15
     * (0xc800) and a plain write clears them, which the ROM then reports as
     * SW_ERR_BZ instead of handing over to the image loader. Leave nothing
     * that would keep its DMA engine stopped from an earlier boot. */
    clrb(IWX_CSR_RESET, IWX_CSR_RESET_REG_FLAG_STOP_MASTER);
    wr(IWX_CSR_FUNC_SCRATCH, IWX_CSR_FUNC_SCRATCH_INIT_VALUE);
    /* Make every CPU-written boot buffer visible before the device reads it. */
    const auto flush = [](const DmaBuf &b) {
      const auto *p = reinterpret_cast<const char *>(b.va);
      for (size_t off = 0; off < b.size; off += 64) _mm_clflush(p + off);
    };
    flush(_iml_buf);
    flush(_ctxt_info);
    flush(_prph_scratch);
    flush(_prph_info);
    flush(_rx_free);
    flush(_rx_stat);
    for (const auto &sec : _fw_sections) flush(sec);
    _mm_mfence();
    /* Hand the MAC to the ROM before it runs. */
    clrb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ);
    setb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_ROM_START);
    setb(IWX_CSR_CTXT_INFO_BOOT_CTRL, IWX_CSR_AUTO_FUNC_BOOT_ENA);
    for (unsigned i = 0; i < 20; i++) {
      if (rd(IWX_CSR_MSIX_HW_INT_CAUSES_AD) & IWX_MSIX_HW_INT_CAUSES_REG_IML_BZ) break;
      delay_us(10000);
    }
  } else {
    if (!nic_lock()) {
      error = "MAC access failed before the firmware kick";
      return false;
    }
    write_umac_prph(IWX_UREG_CPU_INIT_RUN, 1);
    nic_unlock();
  }

  /* The ALIVE cause latches in CSR_INT; the firmware has then set up the
   * receive engine, so hand it the free buffers (iwx_intr's ALIVE branch).
   * On Bz also check CSR_MSIX_HW_INT_CAUSES_AD in case silicon routes causes
   * there without PCI MSI-X (see docs/BE200.md). */
  {
    const uint64_t end = now_us() + (bz() ? 2000000u : 1000000u);
    bool alive_cause = false;
    while (now_us() < end) {
      const uint32_t cause = rd(IWX_CSR_INT);
      if (cause != 0xffffffffu && (cause & IWX_CSR_INT_BIT_ALIVE)) {
        wr(IWX_CSR_INT, cause);
        alive_cause = true;
        break;
      }
      if (cause != 0xffffffffu && (cause & ((bz() ? 0u : IWX_CSR_INT_BIT_SW_ERR) | IWX_CSR_INT_BIT_HW_ERR))) {
        wr(IWX_CSR_INT, cause);
        break;
      }
      if (bz()) {
        const uint32_t hw_cause = rd(IWX_CSR_MSIX_HW_INT_CAUSES_AD);
        if (hw_cause != 0xffffffffu && (hw_cause & IWX_MSIX_HW_INT_CAUSES_REG_ALIVE)) {
          wr(IWX_CSR_MSIX_HW_INT_CAUSES_AD, hw_cause);
          alive_cause = true;
          break;
        }
        if (hw_cause != 0xffffffffu &&
            (hw_cause & (IWX_MSIX_HW_INT_CAUSES_REG_SW_ERR_BZ | IWX_MSIX_HW_INT_CAUSES_REG_HW_ERR))) {
          wr(IWX_CSR_MSIX_HW_INT_CAUSES_AD, hw_cause);
          break;
        }
      }
      delay_us(1000);
    }
    if (!alive_cause) {
      if (nic_lock()) {
        _log("SecBoot CPU1 " + hex(read_umac_prph(IWX_UMAG_SB_CPU_1_STATUS)) + " CPU2 " +
             hex(read_umac_prph(IWX_UMAG_SB_CPU_2_STATUS)) + " UMAC PC " +
             hex(read_umac_prph(IWX_UREG_UMAC_CURRENT_PC)) + " LMAC PC " +
             hex(read_umac_prph(IWX_UREG_LMAC1_CURRENT_PC)));
        nic_unlock();
      }
      error = "the firmware did not come alive";
      return false;
    }
    for (unsigned i = 0; i < IWX_RX_MQ_RING_COUNT; i++) post_rx_buf(i);
    /* Leave one empty slot, tell the card in multiples of 8. Bz takes the free
     * index through HBUS_TARG_WRPTR; FRBDCB_WIDX_TRG is ignored there. */
    ring_rx_doorbell(bz() ? ((IWX_RX_MQ_RING_COUNT - 1) & ~7u) : 8);
  }
  if (!wait_for([&] { return _alive_seen; }, 1000)) {
    error = "ALIVE interrupt seen but no ALIVE notification arrived";
    return false;
  }
  if (!_alive_ok) {
    error = "the firmware reported a failed ALIVE";
    return false;
  }
  _log("firmware alive");
  if (gen3() && !load_pnvm()) {
    error = "the firmware did not take its platform NVM (PNVM)";
    return false;
  }
  if (bz()) {
    /* Confirm PNVM kept the firmware alive before host commands resume. */
    for (unsigned i = 0; i < 50; i++) {
      const uint32_t msix = rd(IWX_CSR_MSIX_HW_INT_CAUSES_AD);
      const uint32_t csr = rd(IWX_CSR_INT);
      const bool bad = (msix != 0xffffffffu &&
                        (msix & (IWX_MSIX_HW_INT_CAUSES_REG_SW_ERR_BZ | IWX_MSIX_HW_INT_CAUSES_REG_HW_ERR))) ||
                       (csr != 0xffffffffu && (csr & IWX_CSR_INT_BIT_HW_ERR));
      if (bad) {
        _log("Bz idle after PNVM step +" + std::to_string(i * 2) + " ms: MSIX_HW " + hex(msix) +
             ", CSR_INT " + hex(csr));
        dump_error_tables();
        error = "the firmware reported an error after ALIVE";
        return false;
      }
      delay_us(2000);
    }
  }
  return true;
}

/* iwx_load_pnvm + iwx_read_pnvm + iwx_pnvm_handle_section: the platform NVM
 * section for this SKU and MAC/radio (from embedded TLV 74 or iwlwifi-*.pnvm)
 * goes to the firmware through the peripheral scratch (flat buffer on AX210,
 * or descriptor pointer array when FRAGMENTED_PNVM_IMG is set); then the
 * doorbell, and the firmware answers PNVM_INIT_COMPLETE. */
bool IwxDevice::load_pnvm() {
  if (!_sku_id[0] && !_sku_id[1] && !_sku_id[2]) return true; /* nothing to do */
  const uint8_t *pnvm_data = (_fw.pnvm && _fw.pnvm_len) ? _fw.pnvm : _pnvm_raw.data();
  const size_t pnvm_len = (_fw.pnvm && _fw.pnvm_len) ? _fw.pnvm_len : _pnvm_raw.size();
  std::vector<std::vector<uint8_t>> chunks;
  if (pnvm_data && pnvm_len) {
    const uint8_t *data = pnvm_data;
    size_t len = pnvm_len;
    while (len >= 8 && chunks.empty()) {
      const uint32_t type = le32_at(data), tlv_len = le32_at(data + 4);
      len -= 8;
      data += 8;
      if (len < tlv_len || roundup(tlv_len, 4) > len) break;
      if (type == IWX_UCODE_TLV_PNVM_SKU && tlv_len >= 12 && le32_at(data) == _sku_id[0] &&
          le32_at(data + 4) == _sku_id[1] && le32_at(data + 8) == _sku_id[2]) {
        /* This SKU's section: the TLVs up to the next SKU. */
        const uint8_t *s = data + roundup(tlv_len, 4);
        size_t slen = len - roundup(tlv_len, 4);
        bool hw_match = false;
        std::vector<std::vector<uint8_t>> collected;
        while (slen >= 8) {
          const uint32_t st = le32_at(s), sl = le32_at(s + 4);
          slen -= 8;
          s += 8;
          if (slen < sl) break;
          if (st == IWX_UCODE_TLV_PNVM_SKU) break;
          if (st == IWX_UCODE_TLV_HW_TYPE && sl >= 4 && !hw_match)
            hw_match = le16_at(s) == cards::mac_type(_hw_rev) && le16_at(s + 2) == cards::rf_type(_hw_rf_id);
          if (st == IWX_UCODE_TLV_SEC_RT && sl >= 4 && le32_at(s) != 0xddddeeee)
            collected.emplace_back(s + 4, s + sl);
          if (roundup(sl, 4) > slen) break;
          slen -= roundup(sl, 4);
          s += roundup(sl, 4);
        }
        if (hw_match && !collected.empty()) chunks = std::move(collected);
        else _log("PNVM: this SKU's section is for another MAC/radio");
      }
      data += roundup(tlv_len, 4);
      len -= roundup(tlv_len, 4);
    }
  }
  if (!chunks.empty()) {
    if (_fw.has_capa(IWX_UCODE_TLV_CAPA_FRAGMENTED_PNVM_IMG)) {
      bool ok = static_cast<bool>(_pnvm_desc) && chunks.size() <= _pnvm_chunks.size() &&
                chunks.size() <= IWX_IPC_DRAM_MAP_ENTRY_NUM_MAX;
      size_t total = 0;
      for (size_t i = 0; ok && i < chunks.size(); i++) {
        ok = _pnvm_chunks[i] && chunks[i].size() <= _pnvm_chunks[i].size;
        total += chunks[i].size();
      }
      if (!ok) {
        _log("PNVM: no fragmented buffer for " + std::to_string(chunks.size()) + " chunks");
      } else {
        auto *arr = reinterpret_cast<iwx_prph_scratch_mem_desc_addr_array *>(_pnvm_desc.va);
        std::memset(arr, 0, sizeof(*arr));
        for (size_t i = 0; i < chunks.size(); i++) {
          std::memcpy(_pnvm_chunks[i].va, chunks[i].data(), chunks[i].size());
          arr->mem_descs[i] = htole64(_pnvm_chunks[i].pa);
        }
        auto &ctrl = reinterpret_cast<iwx_prph_scratch *>(_prph_scratch.va)->ctrl_cfg;
        ctrl.pnvm_cfg.pnvm_base_addr = htole64(_pnvm_desc.pa);
        ctrl.pnvm_cfg.pnvm_size = htole32(static_cast<uint32_t>(total));
        _log("PNVM: " + std::to_string(total) + " bytes in " + std::to_string(chunks.size()) +
             " fragmented chunks for this SKU");
      }
    } else {
      std::vector<uint8_t> section;
      for (const auto &c : chunks) section.insert(section.end(), c.begin(), c.end());
      if (!_pnvm_buf || section.size() > _pnvm_buf.size) {
        _log("PNVM: no buffer for " + std::to_string(section.size()) + " bytes");
      } else {
        std::memcpy(_pnvm_buf.va, section.data(), section.size());
        auto &ctrl = reinterpret_cast<iwx_prph_scratch *>(_prph_scratch.va)->ctrl_cfg;
        ctrl.pnvm_cfg.pnvm_base_addr = htole64(_pnvm_buf.pa);
        ctrl.pnvm_cfg.pnvm_size = htole32(static_cast<uint32_t>(section.size()));
        _log("PNVM: " + std::to_string(section.size()) + " bytes for this SKU");
      }
    }
  } else {
    _log("PNVM: none for this SKU; the firmware goes on without it");
  }
  _pnvm_raw.clear();
  _pnvm_done = false;
  if (nic_lock()) {
    write_umac_prph(IWX_UREG_DOORBELL_TO_ISR6, IWX_UREG_DOORBELL_TO_ISR6_PNVM);
    nic_unlock();
  }
  if (!wait_for([&] { return _pnvm_done; }, 2000)) {
    _log("PNVM: no PNVM_INIT_COMPLETE from the firmware");
    return false;
  }
  return true;
}

/* iwx_run_init_mvm_ucode(readnvm = 1) followed by the parts of iwx_init_hw
 * a monitor needs. */
bool IwxDevice::run_init_sequence(std::string &error) {
  /* Bz firmware only speaks the MLD command set, whatever capability word it
   * reports (the AX203's hr firmware is API 97 and carries it; BE200 the
   * same). */
  _use_mld = bz();
  if (_use_mld) _log("firmware MLD API enabled (experimental)");
  iwx_init_extended_cfg_cmd init_cfg{};
  /* MLD firmware marks the PHY configuration (not NVM_ACCESS) before
   * INIT_COMPLETE, as iwl_mld_run_fw_init_sequence does. */
  init_cfg.init_flags = htole32(bz() ? IWX_INIT_PHY : IWX_INIT_NVM);
  _init_complete = false;
  /* What the firmware sends while it initialises goes to the log, so a card
   * that never answers INIT_COMPLETE can be told apart (issue #2). */
  _trace_packets = 24;
  if (send_cmd(IWX_WIDE_ID(IWX_SYSTEM_GROUP, IWX_INIT_EXTENDED_CFG_CMD), &init_cfg,
               sizeof(init_cfg), false, nullptr)) {
    error = "INIT_EXTENDED_CFG failed";
    return false;
  }
  if (bz()) {
    /* MLD firmware takes the PHY configuration before INIT_COMPLETE and has
     * no NVM_ACCESS_COMPLETE (iwl_mld_run_fw_init_sequence). */
    iwx_phy_cfg_cmd_v3 phy{};
    phy.phy_cfg = htole32(_fw.phy_config);
    phy.calib_control.event_trigger = htole32(_fw.calib_event_trigger);
    phy.calib_control.flow_trigger = htole32(_fw.calib_flow_trigger);
    if (send_cmd(IWX_PHY_CONFIGURATION_CMD, &phy, sizeof(phy), false, nullptr)) {
      error = "PHY_CONFIGURATION failed";
      return false;
    }
  } else {
    iwx_nvm_access_complete_cmd nvm_complete{};
    if (send_cmd(IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP, IWX_NVM_ACCESS_COMPLETE),
                 &nvm_complete, sizeof(nvm_complete), false, nullptr)) {
      error = "NVM_ACCESS_COMPLETE failed";
      return false;
    }
  }
  const bool init_done = wait_for([&] { return _init_complete; }, 5000);
  _trace_packets = 0;
  if (!init_done) {
    error = "no INIT_COMPLETE from the firmware";
    return false;
  }
  if (int err = nvm_get()) {
    error = "reading the NVM failed (" + std::to_string(err) + ")";
    return false;
  }

  if (!nic_lock()) {
    error = "MAC access failed after init";
    return false;
  }
  int err = 0;
  {
    iwx_tx_ant_cfg_cmd ant{};
    ant.valid = htole32(valid_tx_ant());
    err = send_cmd(IWX_TX_ANT_CONFIGURATION_CMD, &ant, sizeof(ant), false, nullptr);
  }
  if (!err) {
    /* Arbitration with the card's Bluetooth half: Wi-Fi first, as itlwm,
     * unless --bt-coex asks for another mode. Windows sends the AirDrop
     * beacon that wakes Apple devices through that Bluetooth half, and Wi-Fi
     * first can starve it while AWDL listens (mode 1 found them in seconds on
     * an AX200 where Wi-Fi first often never did). */
    iwx_bt_coex_cmd bt{};
    const uint32_t mode = _opt.bt_coex >= IWX_BT_COEX_NW && _opt.bt_coex <= IWX_BT_COEX_WIFI
                              ? _opt.bt_coex
                              : uint32_t(IWX_BT_COEX_WIFI);
    bt.mode = htole32(mode);
    err = send_cmd(IWX_BT_CONFIG, &bt, sizeof(bt), false, nullptr);
    if (!err)
      _log(std::string("bluetooth coexistence: mode ") + std::to_string(mode) +
           (mode == IWX_BT_COEX_NW ? " (network)" : mode == IWX_BT_COEX_BT ? " (Bluetooth first)" : " (Wi-Fi first)"));
  }
  if (!err) {
    /* iwx_send_soc_conf with the card's transport parameters (cards.h):
     * discrete cards say so (VER_1); integrated ones give the LTR delay and
     * low-latency crystal (VER_2) and their crystal latency. */
    iwx_soc_configuration_cmd soc{};
    uint32_t flags = 0;
    if (!_integrated) {
      flags = IWX_SOC_CONFIG_CMD_FLAGS_DISCRETE;
      if (bz() && _card.low_latency_xtal) flags |= IWX_SOC_CONFIG_CMD_FLAGS_LOW_LATENCY;
    } else {
      flags = _card.ltr_delay & IWX_SOC_FLAGS_LTR_APPLY_DELAY_MASK;
      const uint8_t scan_ver = cmd_version(IWX_LONG_GROUP, IWX_SCAN_REQ_UMAC);
      if (scan_ver != IWX_FW_CMD_VER_UNKNOWN && scan_ver >= 2 && _card.low_latency_xtal)
        flags |= IWX_SOC_CONFIG_CMD_FLAGS_LOW_LATENCY;
    }
    soc.flags = htole32(flags);
    soc.latency = htole32(_card.xtal_latency);
    err = send_cmd(iwx_cmd_id(IWX_SOC_CONFIGURATION_CMD, IWX_SYSTEM_GROUP, 0), &soc,
                   sizeof(soc), false, nullptr);
  }
  if (!err && !bz() && _fw.has_capa(IWX_UCODE_TLV_CAPA_DQA_SUPPORT)) {
    iwx_dqa_enable_cmd dqa{};
    dqa.cmd_queue = htole32(IWX_DQA_CMD_QUEUE);
    err = send_cmd(iwx_cmd_id(IWX_DQA_ENABLE_CMD, IWX_DATA_PATH_GROUP, 0), &dqa, sizeof(dqa),
                   false, nullptr);
  }
  if (!err) {
    /* Monitor: no power save (iwx_power_update_device). */
    iwx_device_power_cmd power{};
    err = send_cmd(IWX_POWER_TABLE_CMD, &power, sizeof(power), false, nullptr);
  }
  if (!err && _lar_enabled) err = update_mcc("ZZ", -1, nullptr);
  nic_unlock();
  if (!err && _opt.country != "ZZ" && !set_country(_opt.country))
    _log("the firmware did not take country " + _opt.country);
  if (err) {
    error = "firmware init commands failed (" + std::to_string(err) + ")";
    return false;
  }
  return true;
}

bool IwxDevice::open(const Options &options, std::string &error) {
  _opt = options;
  if (!_proxy.open(error)) return false;
  if (!identify(error)) return false;
  std::string path = _opt.firmware_dir;
  if (!path.empty() && path.back() != '\\' && path.back() != '/') path += '\\';
  if (!_fw.load(path + _fw_name, error)) return false;
  if (!alloc_rings(error)) return false;
  if (!start_hw()) {
    error = "hardware start failed";
    return false;
  }
  if (!load_firmware(error)) {
    dump_error_tables();
    stop();
    return false;
  }
  _open = true;
  if (!run_init_sequence(error)) {
    dump_error_tables();
    stop();
    return false;
  }
  return true;
}

bool IwxDevice::stop() {
  if (!_proxy.is_open()) return true;
  bool clean = true;
  if (_open || !_fw_sections.empty()) {
    /* iwx_stop_device: stop receive DMA, then the bus master, then reset. */
    if (nic_lock()) {
      if (gen3()) write_umac_prph(IWX_RFH_RXF_DMA_CFG_GEN3, 0);
      else write_prph(IWX_RFH_RXF_DMA_CFG, 0);
      for (int i = 0; i < 100; i++) {
        const uint32_t v = gen3() ? read_umac_prph(IWX_RFH_GEN_STATUS_GEN3) : read_prph(IWX_RFH_GEN_STATUS);
        if (v == 0xffffffffu || (v & IWX_RXF_DMA_IDLE)) break;
        delay_us(10);
      }
      nic_unlock();
    } else {
      clean = false;
    }
    if (bz()) {
      /* Linux Bz: drop MAC_INIT and the AUX ownership, then reset; the old
       * STOP_MASTER/INIT_DONE handshake does not exist here. */
      clrb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ |
                                 IWX_CSR_GP_CNTRL_REG_FLAG_MAC_INIT);
    } else {
      clrb(IWX_CSR_GP_CNTRL, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
      _nic_locks = 0;
      apm_stop();
    }
    _nic_locks = 0;
    sw_reset();
    mask_interrupts();
    if (!prepare_card_hw()) clean = false;
  }
  _proxy.set_close_ops({});
  _open = false;
  _phy_added = _mac_added = _binding_added = _link_added = _link_active = _sta_added = _queue_enabled = false;
  _sta_link_added = _sta_link_active = _sta_paused = false;
  _use_mld = false;
  _ba_station = _agg_queue_enabled = _agg_active = false;
  _ba_tids = 0;
  for (auto &p : _bz_peers) p = BzPeer{};
  _reorder.clear();
  _deferred_rx.clear();
  return clean;
}

/* ---------------------------------------------------------------- commands */

bool IwxDevice::wait_for(const std::function<bool()> &done, unsigned timeout_ms) {
  const uint64_t end = now_us() + uint64_t(timeout_ms) * 1000;
  while (!done()) {
    if (_fw_error || _proxy.failed() || now_us() >= end) return done();
    if (poll() == 0) delay_us(100);
  }
  return true;
}

/* iwx_send_cmd, synchronous only. Returns 0, or an errno-style code. */
/* A name for the commands a log is most likely to show (#16: which one hung). */
static std::string cmd_name(uint32_t id) {
  switch (id & 0xffff) {
  case IWX_PHY_CONTEXT_CMD: return " (PHY_CONTEXT)";
  case IWX_ADD_STA: return " (ADD_STA)";
  case IWX_REMOVE_STA: return " (REMOVE_STA)";
  case IWX_SCD_QUEUE_CFG: return " (SCD_QUEUE_CFG)";
  case IWX_MAC_CONTEXT_CMD: return " (MAC_CONTEXT)";
  case IWX_BINDING_CONTEXT_CMD: return " (BINDING_CONTEXT)";
  case IWX_TIME_QUOTA_CMD: return " (TIME_QUOTA)";
  case IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_SESSION_PROTECTION_CMD): return " (SESSION_PROTECTION)";
  case IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_MAC_CONFIG_CMD): return " (MAC_CONFIG)";
  case IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_LINK_CONFIG_CMD): return " (LINK_CONFIG)";
  case IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_STA_CONFIG_CMD): return " (STA_CONFIG)";
  case IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_STA_REMOVE_CMD): return " (STA_REMOVE)";
  case IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_RX_BAID_ALLOCATION_CONFIG_CMD): return " (RX_BAID_CFG)";
  case IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_SCD_QUEUE_CONFIG_CMD): return " (SCD_QUEUE_CONFIG)";
  case IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_SEC_KEY_CMD): return " (SEC_KEY)";
  default: return "";
  }
}

int IwxDevice::send_cmd(uint32_t id, const void *data, size_t len, bool want_resp,
                        std::vector<uint8_t> *resp, unsigned timeout_ms) {
  if (_fw_error || _proxy.failed()) return ENXIO;
  TxRing &ring = _cmdq;
  const unsigned idx = ring.cur & (ring.count - 1);
  CmdSlot &slot = _cmd_slots[idx];
  if (slot.pending || ring.queued >= ring.count - 1) return ENOSPC;
  if (len > IWX_MAX_CMD_PAYLOAD_SIZE) return EINVAL;

  uint32_t code = id;
  slot.narrow = iwx_cmd_groupid(code) == 0;
  if (slot.narrow) code = IWX_WIDE_ID(IWX_LONG_GROUP, code);

  /* Every command in its own page (no adjacent pages needed for the ring). */
  auto *cmd = reinterpret_cast<iwx_device_cmd *>(_cmd_big[idx].va);
  const uint64_t paddr = _cmd_big[idx].pa;
  const size_t hdrlen = sizeof(cmd->hdr_wide);
  cmd->hdr_wide.cmd = iwx_cmd_opcode(code);
  cmd->hdr_wide.group_id = iwx_cmd_groupid(code);
  cmd->hdr_wide.qid = static_cast<uint8_t>(ring.qid);
  cmd->hdr_wide.idx = static_cast<uint8_t>(idx);
  cmd->hdr_wide.length = htole16(static_cast<uint16_t>(len));
  cmd->hdr_wide.reserved = 0;
  cmd->hdr_wide.version = iwx_cmd_version(code);
  if (len) std::memcpy(cmd->data_wide, data, len);

  auto *desc = reinterpret_cast<iwx_tfh_tfd *>(ring.desc.va) + idx;
  std::memset(desc, 0, sizeof(*desc));
  const size_t total = hdrlen + len;
  desc->tbs[0].tb_len = htole16(static_cast<uint16_t>(std::min<size_t>(total, IWX_FIRST_TB_SIZE)));
  uint64_t a = htole64(paddr);
  std::memcpy(&desc->tbs[0].addr, &a, sizeof(a));
  if (total > IWX_FIRST_TB_SIZE) {
    desc->tbs[1].tb_len = htole16(static_cast<uint16_t>(total - IWX_FIRST_TB_SIZE));
    a = htole64(paddr + IWX_FIRST_TB_SIZE);
    std::memcpy(&desc->tbs[1].addr, &a, sizeof(a));
    desc->num_tbs = htole16(2);
  } else {
    desc->num_tbs = htole16(1);
  }
  slot.pending = true;
  slot.want_resp = want_resp;
  slot.resp.clear();
  ring.queued++;
  ring.cur = (ring.cur + 1) % wrap(); /* the pointer wraps at 256 (65536 on AX210), not at the ring */
  plat_fence();
  wr(IWX_HBUS_TARG_WRPTR, (ring.qid << 16) | ring.cur);

  _cmd_depth++;
  const bool answered = wait_for([&] { return !slot.pending; }, timeout_ms);
  _cmd_depth--;
  if (!answered) {
    _log("firmware command " + hex(id) + cmd_name(id) + " timed out");
    slot.pending = false;
    return _fw_error ? EIO : ETIMEDOUT;
  }
  if (want_resp) {
    if (slot.resp.empty()) return EIO; /* failed, or the response was too long */
    if (resp) *resp = std::move(slot.resp);
  }
  return 0;
}

int IwxDevice::send_cmd_status(uint32_t id, const void *data, size_t len, uint32_t *status) {
  std::vector<uint8_t> resp;
  const int err = send_cmd(id, data, len, true, &resp);
  if (err) return err;
  const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(resp.data());
  if (resp.size() < sizeof(*pkt) + sizeof(iwx_cmd_response) ||
      (pkt->hdr.group_id & IWX_CMD_FAILED_MSK) ||
      iwx_rx_packet_payload_len(pkt) != sizeof(iwx_cmd_response))
    return EIO;
  *status = le32toh(reinterpret_cast<const iwx_cmd_response *>(pkt->data)->status);
  return 0;
}

uint8_t IwxDevice::valid_tx_ant() const {
  uint8_t ant = (_fw.phy_config & IWX_FW_PHY_CFG_TX_CHAIN) >> IWX_FW_PHY_CFG_TX_CHAIN_POS;
  if (_nvm_tx_ant) ant &= _nvm_tx_ant;
  return ant;
}

uint8_t IwxDevice::valid_rx_ant() const {
  uint8_t ant = (_fw.phy_config & IWX_FW_PHY_CFG_RX_CHAIN) >> IWX_FW_PHY_CFG_RX_CHAIN_POS;
  if (_nvm_rx_ant) ant &= _nvm_rx_ant;
  return ant;
}

int IwxDevice::nvm_get() {
  iwx_nvm_get_info cmd{};
  std::vector<uint8_t> resp;
  const bool v4 = _fw.has_api(IWX_UCODE_TLV_API_REGULATORY_NVM_INFO);
  const int err = send_cmd(IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP, IWX_NVM_GET_INFO), &cmd,
                           sizeof(cmd), true, &resp);
  if (err) return err;
  const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(resp.data());
  const size_t want = v4 ? sizeof(iwx_nvm_get_info_rsp) : sizeof(iwx_nvm_get_info_rsp_v3);
  if (iwx_rx_packet_payload_len(pkt) != want) return EIO;
  const auto *rsp = reinterpret_cast<const iwx_nvm_get_info_rsp *>(pkt->data);
  _nvm_tx_ant = static_cast<uint8_t>(le32toh(rsp->phy_sku.tx_chains));
  _nvm_rx_ant = static_cast<uint8_t>(le32toh(rsp->phy_sku.rx_chains));
  _lar_enabled = le32toh(rsp->regulatory.lar_enabled) && _fw.has_capa(IWX_UCODE_TLV_CAPA_LAR_SUPPORT);

  /* The MAC address strapped by the platform (iwx_set_mac_addr_from_csr). */
  const uint32_t addr_base = bz() ? IWX_CSR_ADDR_BASE_BZ : IWX_CSR_ADDR_BASE;
  const uint32_t a0 = rd(addr_base + 8), a1 = rd(addr_base + 12);
  _hw_addr = {uint8_t(a0 >> 24), uint8_t(a0 >> 16), uint8_t(a0 >> 8), uint8_t(a0),
              uint8_t(a1 >> 24), uint8_t(a1 >> 16)};
  if ((_hw_addr[0] & 1) || std::all_of(_hw_addr.begin(), _hw_addr.end(), [](uint8_t b) { return b == 0; })) {
    const uint32_t b0 = rd(addr_base), b1 = rd(addr_base + 4);
    _hw_addr = {uint8_t(b0 >> 24), uint8_t(b0 >> 16), uint8_t(b0 >> 8), uint8_t(b0),
                uint8_t(b1 >> 24), uint8_t(b1 >> 16)};
  }
  char line[160];
  plat_snprintf(line, sizeof(line),
                "NVM: tx antennas 0x%x, rx antennas 0x%x, LAR %s, address %02x:%02x:%02x:%02x:%02x:%02x",
                valid_tx_ant(), valid_rx_ant(), _lar_enabled ? "on" : "off", _hw_addr[0],
                _hw_addr[1], _hw_addr[2], _hw_addr[3], _hw_addr[4], _hw_addr[5]);
  _log(line);
  return 0;
}

int IwxDevice::update_mcc(const std::string &alpha2, int source, std::string *got) {
  iwx_mcc_update_cmd cmd{};
  const char c0 = alpha2.size() > 0 ? alpha2[0] : 'Z', c1 = alpha2.size() > 1 ? alpha2[1] : 'Z';
  cmd.mcc = htole16(static_cast<uint16_t>(c0 << 8 | c1));
  if (source >= 0)
    cmd.source_id = static_cast<uint8_t>(source);
  else if (_fw.has_api(IWX_UCODE_TLV_API_WIFI_MCC_UPDATE) || _fw.has_capa(IWX_UCODE_TLV_CAPA_LAR_MULTI_MCC))
    cmd.source_id = (c0 == 'Z' && c1 == 'Z') ? IWX_MCC_SOURCE_GET_CURRENT : IWX_MCC_SOURCE_ME;
  else
    cmd.source_id = IWX_MCC_SOURCE_OLD_FW;
  std::vector<uint8_t> resp;
  const int err = send_cmd(IWX_MCC_UPDATE_CMD, &cmd, sizeof(cmd), true, &resp);
  if (err) return err;
  const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(resp.data());
  if (pkt->hdr.group_id & IWX_CMD_FAILED_MSK) return EIO;
  const size_t plen = iwx_rx_packet_payload_len(pkt);
  const uint8_t mcc_notif_ver = _fw.notif_version(IWX_LONG_GROUP, IWX_MCC_UPDATE_CMD);
  const bool v8 = bz() && mcc_notif_ver != IWX_FW_CMD_VER_UNKNOWN && mcc_notif_ver >= 8;
  uint32_t status = 0, n = 0;
  uint16_t mcc = 0;
  uint8_t resp_source = 0;
  const uint32_t *channels = nullptr;
  if (v8) {
    if (plen < sizeof(iwx_mcc_update_resp_v8)) return EIO;
    const auto *r = reinterpret_cast<const iwx_mcc_update_resp_v8 *>(pkt->data);
    n = le32toh(r->n_channels);
    if (plen != sizeof(*r) + n * sizeof(uint32_t)) return EIO;
    status = le32toh(r->status);
    mcc = le16toh(r->mcc);
    resp_source = r->source_id;
    channels = r->channels;
  } else {
    if (plen < sizeof(iwx_mcc_update_resp)) return EIO;
    const auto *r = reinterpret_cast<const iwx_mcc_update_resp *>(pkt->data);
    n = le32toh(r->n_channels);
    if (plen != sizeof(*r) + n * sizeof(uint32_t)) return EIO;
    status = le32toh(r->status);
    mcc = le16toh(r->mcc);
    resp_source = r->source_id;
    channels = r->channels;
  }
  const std::string domain{char(mcc >> 8), char(mcc & 0xff)};
  if (got) *got = domain;
  _current_mcc = domain; /* what the firmware holds now, refused or not (current_country) */
  std::string summary = "regulatory: asked " + alpha2 + " (source " + std::to_string(cmd.source_id) +
                        "), firmware says " + domain + " (source " + std::to_string(resp_source) +
                        ", status " + std::to_string(status) + "):";
  for (uint32_t i = 0; i < n && i < nitems(kNvmChannels); i++) {
    const uint8_t ch = kNvmChannels[i];
    if (ch != 6 && ch != 36 && ch != 44 && ch != 149) continue;
    const uint32_t flags = le32toh(channels[i]);
    _chan_flags[ch] = flags;
    summary += " ch" + std::to_string(ch) + "=" +
               (!(flags & IWX_NVM_CHANNEL_VALID) ? "off"
                : (flags & IWX_NVM_CHANNEL_ACTIVE) ? "active"
                                                    : "passive") +
               ((flags & IWX_NVM_CHANNEL_80MHZ) ? "/80" : "");
  }
  _log(summary);
  return 0;
}

bool IwxDevice::answer_country_notice(std::string *got) {
  if (!_notice_pending) return false;
  _notice_pending = false;
  const uint16_t mcc = _notice_mcc;
  const uint8_t source = _notice_source;
  const std::string alpha2{char(mcc >> 8), char(mcc & 0xff)};
  const bool letters = alpha2[0] >= 'A' && alpha2[0] <= 'Z' && alpha2[1] >= 'A' && alpha2[1] <= 'Z';
  const bool world = alpha2 == "00" || alpha2 == "ZZ";
  _log("regulatory: firmware notice " + (letters || world ? alpha2 : hex(uint32_t(mcc))) + " (source " +
       std::to_string(source) + ")");
  /* The firmware repeats its notice; each country and source is answered once.
   * A withdrawal ("00") is answered too, so the channel map never stays more
   * permissive than the firmware. */
  if (!_lar_enabled || (!letters && !world) || (mcc == _answered_mcc && source == _answered_source)) return false;
  if (!nic_lock()) return false;
  std::string now;
  const int err = update_mcc(alpha2, source, &now);
  nic_unlock();
  if (err) return false;
  _answered_mcc = mcc;
  _answered_source = source;
  if (got) *got = now;
  return true;
}

bool IwxDevice::set_promiscuous(bool on) {
  if (on == _promisc) return true;
  _promisc = on;
  if (!_mac_added || bz()) return true; /* Bz never runs promiscuous */
  if (!nic_lock()) return false;
  const int err = use_mld() ? mld_mac_cmd(IWX_FW_CTXT_ACTION_MODIFY) : mac_ctxt_cmd(IWX_FW_CTXT_ACTION_MODIFY);
  nic_unlock();
  if (err) _log("promiscuous change failed (" + std::to_string(err) + ")");
  return err == 0;
}

/* ---- Bz receive Block Ack: one firmware station per AWDL peer ---- */

void IwxDevice::bz_recount_ba_tids() {
  _ba_tids = 0;
  for (const auto &p : _bz_peers)
    if (p.used) _ba_tids |= p.valid;
}

/* Takes one peer's station down: its sessions, then the entry (Linux removes
 * the queues before the station). */
void IwxDevice::bz_peer_drop(unsigned slot) {
  BzPeer &p = _bz_peers[slot];
  if (!p.used) return;
  const unsigned sta = IWX_BZ_PEER_STA_ID + slot;
  for (unsigned t = 0; t < 8; t++)
    if ((p.valid >> t) & 1) {
      /* The session's buffered frames go up before the firmware can reuse
       * the BAID for someone else. */
      if (!bz_baid_remove(p.baid[t], t, sta)) _reorder.close(p.baid[t]);
    }
  iwx_remove_sta_cmd rm{};
  rm.sta_id = htole32(sta);
  send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_STA_REMOVE_CMD), &rm, sizeof(rm), false, nullptr);
  p = BzPeer{};
  bz_recount_ba_tids();
}

/* Slot of `peer` in _bz_peers, adding its station (evicting the least
 * recently used peer if all are taken); -1 on failure. nic_lock() held. */
int IwxDevice::bz_peer_station(const uint8_t peer[6]) {
  int slot = -1, free_slot = -1, oldest = 0;
  for (unsigned i = 0; i < kBzPeers; i++) {
    const BzPeer &p = _bz_peers[i];
    if (p.used && std::memcmp(p.addr, peer, 6) == 0) slot = static_cast<int>(i);
    else if (!p.used && free_slot < 0) free_slot = static_cast<int>(i);
    if (p.used && _bz_peers[oldest].used && p.last_us < _bz_peers[oldest].last_us)
      oldest = static_cast<int>(i);
  }
  if (slot < 0) {
    if (free_slot < 0) {
      const uint8_t *a = _bz_peers[oldest].addr;
      char b[120];
      plat_snprintf(b, sizeof(b),
                    "Block Ack: all %u peer stations taken, dropping %02x:%02x:%02x:%02x:%02x:%02x "
                    "(station %u)",
                    kBzPeers, a[0], a[1], a[2], a[3], a[4], a[5], IWX_BZ_PEER_STA_ID + oldest);
      _log(b);
      bz_peer_drop(static_cast<unsigned>(oldest));
      free_slot = oldest;
    }
    const unsigned sta = IWX_BZ_PEER_STA_ID + static_cast<unsigned>(free_slot);
    iwx_sta_cfg_cmd_v2 cmd{};
    cmd.sta_id = htole32(sta);
    cmd.link_id = htole32(0);
    std::memcpy(cmd.peer_mld_address, peer, 6);
    std::memcpy(cmd.peer_link_address, peer, 6);
    cmd.station_type = htole32(IWX_STATION_TYPE_PEER);
    const uint8_t ver = cmd_version(IWX_MAC_CONF_GROUP, IWX_STA_CONFIG_CMD);
    const size_t sz = (ver != IWX_FW_CMD_VER_UNKNOWN && ver > 1) ? sizeof(iwx_sta_cfg_cmd_v2)
                                                                 : sizeof(iwx_sta_cfg_cmd_v1);
    const int err = send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_STA_CONFIG_CMD), &cmd, sz,
                             false, nullptr);
    if (err) {
      _log("Block Ack: peer station " + std::to_string(sta) + " refused (" +
           std::to_string(err) + ")");
      return -1;
    }
    slot = free_slot;
    BzPeer &p = _bz_peers[slot];
    p = BzPeer{};
    p.used = true;
    std::memcpy(p.addr, peer, 6);
  }
  _bz_peers[slot].last_us = now_us();
  return slot;
}

/* RX_BAID_ALLOCATION_CONFIG_CMD: the firmware reorders for this station+TID
 * and answers with the BAID; no ADD_STA modify is involved. */
int IwxDevice::bz_baid_add(unsigned sta_id, unsigned tid, uint16_t ssn, uint16_t window,
                           uint32_t &baid) {
  iwx_rx_baid_cfg_cmd cmd{};
  cmd.action = htole32(IWX_RX_BAID_ACTION_ADD);
  cmd.alloc.sta_id_mask = htole32(1u << sta_id);
  cmd.alloc.tid = static_cast<uint8_t>(tid);
  cmd.alloc.ssn = htole16(ssn);
  cmd.alloc.win_size = htole16(window);
  std::vector<uint8_t> resp;
  const int err = send_cmd(IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_RX_BAID_ALLOCATION_CONFIG_CMD),
                           &cmd, sizeof(cmd), true, &resp);
  if (err) return err;
  const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(resp.data());
  if ((pkt->hdr.group_id & IWX_CMD_FAILED_MSK) ||
      iwx_rx_packet_payload_len(pkt) != sizeof(uint32_t))
    return EIO;
  uint32_t value = 0;
  std::memcpy(&value, pkt->data, sizeof(value));
  baid = le32toh(value);
  return 0;
}

int IwxDevice::bz_baid_remove(uint32_t baid, unsigned tid, unsigned sta_id) {
  iwx_rx_baid_cfg_cmd cmd{};
  cmd.action = htole32(IWX_RX_BAID_ACTION_REMOVE);
  /* iwl_mvm_fw_baid_op_cmd: only version 1 (or an unlisted command) removes
   * by BAID; version 2+ names station mask and TID. A BAID read as a station
   * mask asserts the firmware (ADVANCED_SYSASSERT 0x2010351e). */
  const uint8_t ver = cmd_version(IWX_DATA_PATH_GROUP, IWX_RX_BAID_ALLOCATION_CONFIG_CMD);
  if (ver >= 2 && ver != IWX_FW_CMD_VER_UNKNOWN) {
    cmd.remove.sta_id_mask = htole32(1u << sta_id);
    cmd.remove.tid = htole32(tid);
  } else {
    cmd.remove_v1.baid = htole32(baid);
  }
  std::vector<uint8_t> resp;
  int err = send_cmd(IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_RX_BAID_ALLOCATION_CONFIG_CMD), &cmd,
                     sizeof(cmd), true, &resp);
  bool failed = false;
  if (!err && resp.size() >= sizeof(iwx_rx_packet)) {
    const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(resp.data());
    failed = (pkt->hdr.group_id & IWX_CMD_FAILED_MSK) != 0;
    if (failed) err = EIO;
  }
  if (_ba_rm_logged < 40) {
    _ba_rm_logged++;
    _log("Block Ack: remove BAID " + std::to_string(baid) + " tid " + std::to_string(tid) +
         " station " + std::to_string(sta_id) +
         (err ? (failed ? ": firmware refused" : ": error " + std::to_string(err)) : ": ok"));
  }
  return err;
}

/* SESSION_PROTECTION_CMD: an unassociated BSS_STA link gets no air time of
 * its own (Linux schedules this before authenticating). Version 2+ names the
 * link, version 1 the MAC (iwl_mvm_get_session_prot_id). */
int IwxDevice::bz_session_protect(unsigned link_id, unsigned mac_id, uint32_t duration_tu) {
  iwx_session_prot_cmd sp{};
  const uint8_t ver = cmd_version(IWX_MAC_CONF_GROUP, IWX_SESSION_PROTECTION_CMD);
  const bool by_link = ver >= 2 && ver != IWX_FW_CMD_VER_UNKNOWN;
  sp.id_and_color = htole32(by_link ? link_id : mac_id);
  sp.action = htole32(IWX_FW_CTXT_ACTION_ADD);
  sp.conf_id = htole32(IWX_SESSION_PROTECT_CONF_ASSOC);
  sp.duration_tu = htole32(duration_tu);
  return send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_SESSION_PROTECTION_CMD), &sp, sizeof(sp),
                  false, nullptr);
}

bool IwxDevice::keep_on_channel() {
  if (!bz() || _fw_error) return false;
  const uint64_t now = now_us();
  bool did_work = false;
  if (_sta_mac_added && _sta_join_us == 0) _sta_join_us = now;
  /* The station beside the listener: until association, at most 10 s per
   * join so a silent AP cannot starve AWDL. A new ADD only once the last one
   * ended (1000 TU is 1.024 s), never on top of a running one, and none once
   * associated (as Linux): overlapping ADDs on the associated link while AWDL
   * kept the other PHY busy asserted the LMAC (0x3706, 2026-11-25). */
  const bool sta_session_due =
      !_sta_session_on ? now - _sta_session_sent_us >= 200000 : now - _sta_session_sent_us >= 1100000;
  if (_sta_mac_added && !_sta_assoc && now - _sta_join_us < 10000000 && sta_session_due) {
    _sta_session_sent_us = now;
    _sta_session_on = true; /* until the firmware says it ended or was not scheduled */
    if (nic_lock()) {
      const int err = bz_session_protect(1, 1, 1000);
      nic_unlock();
      if (err && _session_logged++ < 12)
        _log("station: session protection refused (" + std::to_string(err) + ")");
      did_work = true;
    }
  }
  if (!_mac_added || _opt.mac_type != MacType::Sta) return did_work;
  if (_session_on && now - _session_sent_us < 700000) return did_work;
  if (now - _session_sent_us < 200000) return did_work;
  _session_sent_us = now;
  if (!nic_lock()) return did_work;
  const int err = bz_session_protect(0, 0, 1000);
  nic_unlock();
  if (err && _session_logged++ < 12)
    _log("session protection refused (" + std::to_string(err) + ")");
  return true;
}

std::string IwxDevice::bz_boot_regs() {
  return "CSR_INT " + hex(rd(IWX_CSR_INT)) + ", FH_INT " + hex(rd(IWX_CSR_FH_INT_STATUS)) +
         ", MSIX_HW " + hex(rd(IWX_CSR_MSIX_HW_INT_CAUSES_AD)) + ", FSEQ ERROR_CODE " +
         hex(read_prph(0xa340c8)) + ", ALIVE_TOKEN " + hex(read_prph(0xa340f0)) + ", SB_CPU_2 " +
         hex(read_umac_prph(IWX_UMAG_SB_CPU_2_STATUS)) + ", SB_CPU_1 " +
         hex(read_umac_prph(IWX_UMAG_SB_CPU_1_STATUS)) + ", AUTH_KEY_0 " +
         hex(read_umac_prph(0xa03088)) + ", func scratch " + hex(rd(IWX_CSR_FUNC_SCRATCH));
}

bool IwxDevice::add_rx_ba(const uint8_t peer[6], unsigned tid, uint16_t ssn, uint16_t window,
                          uint8_t token) {
  if (tid >= 8 || !_mac_added) return false;
  if (bz()) {
    if (!nic_lock()) return false;
    int known = -1;
    for (unsigned i = 0; i < kBzPeers; i++)
      if (_bz_peers[i].used && std::memcmp(_bz_peers[i].addr, peer, 6) == 0)
        known = static_cast<int>(i);
    const int slot = bz_peer_station(peer);
    if (slot < 0) {
      nic_unlock();
      return false;
    }
    BzPeer &p = _bz_peers[slot];
    const unsigned sta = IWX_BZ_PEER_STA_ID + slot;
    const bool open = (p.valid >> tid) & 1;
    if (_ba_logged < 40) {
      _ba_logged++;
      char b[220];
      plat_snprintf(b, sizeof(b),
                    "Block Ack: ADDBA from %02x:%02x:%02x:%02x:%02x:%02x tid %u token %u ssn %u "
                    "window %u; station %u (%s), its open tids 0x%02x, this tid %s (BAID %u, token %u)",
                    peer[0], peer[1], peer[2], peer[3], peer[4], peer[5], tid, token, ssn, window, sta,
                    known < 0 ? "new" : "kept", p.valid, open ? "open" : "closed", p.baid[tid],
                    p.token[tid]);
      _log(b);
    }
    int err = 0;
    /* A new token is a new session with its own start sequence. */
    if (open && p.token[tid] != token) {
      bz_baid_remove(p.baid[tid], tid, sta);
      _reorder.close(p.baid[tid]);
      p.valid &= static_cast<uint8_t>(~(1u << tid));
    }
    if (!((p.valid >> tid) & 1)) {
      uint32_t baid = 0;
      err = bz_baid_add(sta, tid, ssn, window, baid);
      if (!err && baid >= RxReorder::kMaxBaid) {
        _log("Block Ack: BAID " + std::to_string(baid) + " out of range, declining (station " +
             std::to_string(sta) + ", tid " + std::to_string(tid) + ")");
        bz_baid_remove(baid, tid, sta);
        err = EIO;
      }
      if (!err) {
        p.baid[tid] = baid;
        p.valid |= static_cast<uint8_t>(1u << tid);
        p.token[tid] = token;
        _reorder.open(baid, ssn, window);
        _log("Block Ack: Bz receive session open (BAID " + std::to_string(baid) + "), station " +
             std::to_string(sta) + ", tid " + std::to_string(tid) + ", window " +
             std::to_string(window));
      } else {
        _log("Block Ack: BAID add refused (" + std::to_string(err) + "), station " +
             std::to_string(sta) + ", tid " + std::to_string(tid));
      }
    }
    bz_recount_ba_tids();
    nic_unlock();
    return err == 0;
  }
  if (!nic_lock()) return false;
  constexpr uint8_t kBaStation = 3; /* ForbiddenItlwm's kAwdlBaStationId */
  int err = 0;
  uint32_t status = 0;
  if (!ensure_ba_station(peer)) {
    nic_unlock();
    return false;
  }
  /* A new token is a new session with its own start: drop the old one first. */
  if (((_ba_tids >> tid) & 1) && _ba_token[tid] != token) {
    iwx_add_sta_cmd cmd{};
    cmd.sta_id = kBaStation;
    cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
    cmd.add_modify = IWX_STA_MODE_MODIFY;
    cmd.remove_immediate_ba_tid = static_cast<uint8_t>(tid);
    cmd.modify_mask = IWX_STA_MODIFY_REMOVE_BA_TID;
    status = IWX_ADD_STA_SUCCESS;
    send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
    _ba_tids &= static_cast<uint8_t>(~(1u << tid));
  }
  if (!((_ba_tids >> tid) & 1)) {
    iwx_add_sta_cmd cmd{};
    cmd.sta_id = kBaStation;
    cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
    cmd.add_modify = IWX_STA_MODE_MODIFY;
    cmd.add_immediate_ba_tid = static_cast<uint8_t>(tid);
    cmd.add_immediate_ba_ssn = htole16(ssn);
    cmd.rx_ba_window = htole16(window);
    cmd.modify_mask = IWX_STA_MODIFY_ADD_BA_TID;
    status = IWX_ADD_STA_SUCCESS;
    err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
    if (err || (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS ||
        !(status & IWX_ADD_STA_BAID_VALID_MASK)) {
      nic_unlock();
      _log("Block Ack: session refused (err " + std::to_string(err) + ", status " + hex(status) + ")");
      return false;
    }
    _ba_tids |= static_cast<uint8_t>(1u << tid);
    _ba_token[tid] = token;
    _log("Block Ack: receive session open, tid " + std::to_string(tid) + ", window " +
         std::to_string(window));
  }
  nic_unlock();
  return true;
}

/* The peer's firmware station, bound to the listener; replaces another
 * peer's (with its sessions and aggregation queue). nic_lock() held. */
bool IwxDevice::ensure_ba_station(const uint8_t peer[6]) {
  constexpr uint8_t kBaStation = 3;
  if (_ba_station && std::memcmp(_ba_peer, peer, 6) == 0) return true;
  if (_ba_station) {
    /* A new peer: the old station and its sessions go; the firmware frees
     * its queues with it (new TX API). */
    iwx_rm_sta_cmd rm{};
    rm.sta_id = kBaStation;
    send_cmd(IWX_REMOVE_STA, &rm, sizeof(rm), false, nullptr);
    _ba_station = false;
    _ba_tids = 0;
    _agg_queue_enabled = false;
    _agg_active = false;
    _aggq.queued = 0;
    _ba_epoch++;
  }
  iwx_add_sta_cmd cmd{};
  cmd.sta_id = kBaStation;
  cmd.station_type = IWX_STA_LINK;
  cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
  std::memcpy(cmd.addr, peer, 6);
  cmd.tid_disable_tx = htole16(0xffff);
  const bool wide = _width >= 80 && _channel > 14;
  const uint8_t rx_ant = valid_rx_ant();
  const bool mimo2 = (rx_ant & (rx_ant - 1)) != 0; /* 1x1 cards (e.g. 9462) stay SISO (#79) */
  uint32_t flags = (wide ? IWX_STA_FLG_FAT_EN_80MHZ : IWX_STA_FLG_FAT_EN_20MHZ) |
                   (mimo2 ? IWX_STA_FLG_MIMO_EN_MIMO2 : 0u);
  uint32_t mask = IWX_STA_FLG_FAT_EN_MSK | IWX_STA_FLG_MIMO_EN_MSK;
  if (_aggq.count) {
    /* As ForbiddenItlwm adds the peer for sending: the largest aggregate the
     * width allows. Only with A-MPDU sending on; the default is unchanged. */
    flags |= wide ? IWX_STA_FLG_MAX_AGG_SIZE_1024K : IWX_STA_FLG_MAX_AGG_SIZE_64K;
    mask |= IWX_STA_FLG_MAX_AGG_SIZE_MSK | IWX_STA_FLG_AGG_MPDU_DENS_MSK;
  }
  cmd.station_flags = htole32(flags);
  cmd.station_flags_msk = htole32(mask);
  uint32_t status = IWX_ADD_STA_SUCCESS;
  const int err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
  if (err || (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS) {
    _log("Block Ack: peer station refused (err " + std::to_string(err) + ", status " + hex(status) + ")");
    return false;
  }
  _ba_station = true;
  std::memcpy(_ba_peer, peer, 6);
  _ba_epoch++;
  return true;
}

bool IwxDevice::agg_setup(const uint8_t peer[6], bool vht80, bool mimo) {
  if (use_mld()) return false;
  if (!_aggq.count || !_mac_added) return false;
  if (!nic_lock()) return false;
  bool ok = ensure_ba_station(peer);
  if (ok && !_agg_queue_enabled) {
    const int err = enable_queue(_aggq, 3, 0);
    if (err) {
      _log("aggregate: queue refused (" + std::to_string(err) + ")");
      ok = false;
    } else {
      _agg_queue_enabled = true;
      /* Our sequence numbers follow the ring, as upstream starts them. */
      _agg_seq = static_cast<uint16_t>(_aggq.cur & 0xfff);
    }
  }
  if (ok) {
    /* Firmware rate control for the peer: VHT80 when the listener is 80 MHz,
     * two streams when both antennas can send. Intel 22000/QuZ firmware asserts
     * in UMAC (0x20101a0d on TLC_MNG_CONFIG_CMD) if HE mode is requested on a
     * LISTENER MAC context, so TX aggregation stays on VHT80 2SS MCS 4..9 SGI
     * (866.7 Mbps PHY) while --he80 enables 8 KiB RX DMA + RX A-MSDU + AWDL HE80. */
    const uint8_t ant = valid_tx_ant();
    const bool two = mimo && (ant & 3) == 3;
    const bool vht = vht80 && _channel > 14;
    const uint32_t id = iwx_cmd_id(IWX_TLC_MNG_CONFIG_CMD, IWX_DATA_PATH_GROUP, 0);
    const uint8_t ver = cmd_version(IWX_DATA_PATH_GROUP, IWX_TLC_MNG_CONFIG_CMD);

    iwx_tlc_config_cmd_v4 c{};
    c.sta_id = 3;
    c.non_ht_rates = htole16(static_cast<uint16_t>(_channel <= 14 ? 0x0fff : 0x0ff0));
    c.chains = static_cast<uint8_t>(((ant & 1) ? IWX_TLC_MNG_CHAIN_A_MSK : 0) |
                                    ((ant & 2) ? IWX_TLC_MNG_CHAIN_B_MSK : 0));
    c.mode = static_cast<uint8_t>(vht ? IWX_TLC_MNG_MODE_VHT : IWX_TLC_MNG_MODE_HT);
    /* High rates only, as ForbiddenItlwm's iwx_rs_init (MCS 4..9 VHT / 4..7 HT):
     * the peer leaves the channel often and rate control would otherwise drop
     * all the way down to MCS 0 (#4). */
    const uint16_t mcs = vht ? 0x03f0 : 0x00f0;
    c.ht_rates[IWX_TLC_NSS_1][IWX_TLC_MCS_PER_BW_80] = htole16(mcs);
    if (two) c.ht_rates[IWX_TLC_NSS_2][IWX_TLC_MCS_PER_BW_80] = htole16(mcs);
    c.max_ch_width =
        static_cast<uint8_t>(vht && _width >= 80 ? IWX_TLC_MNG_CH_WIDTH_80MHZ : IWX_TLC_MNG_CH_WIDTH_20MHZ);
    c.flags = htole16(static_cast<uint16_t>(IWX_TLC_MNG_CFG_FLAGS_LDPC_MSK |
                                            (two ? IWX_TLC_MNG_CFG_FLAGS_STBC_MSK : 0u)));
    c.sgi_ch_width_supp = static_cast<uint8_t>((1u << IWX_TLC_MNG_CH_WIDTH_20MHZ) |
                                               (1u << IWX_TLC_MNG_CH_WIDTH_40MHZ) |
                                               (1u << IWX_TLC_MNG_CH_WIDTH_80MHZ));
    _agg_he80_active = false;
    int err;
    if (ver == 4) {
      err = send_cmd(id, &c, sizeof(c), false, nullptr);
    } else {
      iwl_tlc_config_cmd_v3 v3{};
      v3.sta_id = c.sta_id;
      v3.max_ch_width = c.max_ch_width;
      v3.mode = c.mode;
      v3.chains = c.chains;
      v3.flags = c.flags;
      v3.non_ht_rates = c.non_ht_rates;
      v3.ht_rates[0][0] = c.ht_rates[0][0];
      v3.ht_rates[1][0] = c.ht_rates[1][0];
      v3.sgi_ch_width_supp = c.sgi_ch_width_supp;
      size_t sz = sizeof(v3);
      if (ver == IWX_FW_CMD_VER_UNKNOWN || ver < 3) sz -= 4;
      err = send_cmd(id, &v3, sz, false, nullptr);
    }
    if (err) _log("aggregate: rate control refused (" + std::to_string(err) + ")");
    ok = err == 0;
  }
  nic_unlock();
  return ok;
}

bool IwxDevice::agg_enable(bool on) {
  if (!_ba_station || !_agg_queue_enabled) return false;
  iwx_add_sta_cmd cmd{};
  cmd.sta_id = 3;
  cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
  cmd.add_modify = IWX_STA_MODE_MODIFY;
  /* iwlmvm enables aggregation for a TID by clearing its tid_disable_tx bit. */
  cmd.tid_disable_tx = htole16(static_cast<uint16_t>(on ? 0xfffe : 0xffff));
  cmd.modify_mask = IWX_STA_MODIFY_TID_DISABLE_TX;
  uint32_t status = IWX_ADD_STA_SUCCESS;
  if (!nic_lock()) return false;
  const int err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
  const bool ok = !err && (status & IWX_ADD_STA_STATUS_MASK) == IWX_ADD_STA_SUCCESS;
  /* Switch the listener MAC context atomically with the aggregation state:
   * - on: enter ForbiddenItlwm's BA listener state (AWDL BSSID, ACK rates,
   *   group-only filter, promiscuous off) BEFORE the first A-MPDU is queued.
   * - off: leave BA listener state and restore full receive filters at once
   *   so ADDBA/discovery/management frames arrive again (#4). */
  if (on && ok) {
    _agg_active = true;
    _promisc = false;
    if (_mac_added) mac_ctxt_cmd(IWX_FW_CTXT_ACTION_MODIFY);
  } else if (!on && _agg_active) {
    _agg_active = false;
    _promisc = true;
    if (_mac_added) mac_ctxt_cmd(IWX_FW_CTXT_ACTION_MODIFY);
  }
  nic_unlock();
  return ok;
}

bool IwxDevice::agg_pause(bool on) {
  if (!_ba_station) return false;
  iwx_add_sta_cmd cmd{};
  cmd.sta_id = 3;
  cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
  cmd.add_modify = IWX_STA_MODE_MODIFY;
  cmd.station_flags = htole32(on ? IWX_STA_FLG_DISABLE_TX : 0u);
  cmd.station_flags_msk = htole32(IWX_STA_FLG_DISABLE_TX);
  uint32_t status = IWX_ADD_STA_SUCCESS;
  if (!nic_lock()) return false;
  const int err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
  nic_unlock();
  return !err && (status & IWX_ADD_STA_STATUS_MASK) == IWX_ADD_STA_SUCCESS;
}

bool IwxDevice::agg_flush() {
  if (!_ba_station || !_agg_queue_enabled) return false;
  iwx_tx_path_flush_cmd cmd{};
  cmd.sta_id = htole32(3u);
  cmd.tid_mask = htole16(1u);
  if (!nic_lock()) return false;
  const int err = send_cmd(IWX_TXPATH_FLUSH, &cmd, sizeof(cmd), false, nullptr);
  nic_unlock();
  if (!err) {
    _aggq.tail = _aggq.cur & (_aggq.count - 1);
    _aggq.queued = 0;
    _agg_seq = static_cast<uint16_t>(_aggq.cur & 0xfff);
  }
  return err == 0;
}

int IwxDevice::transmit_agg(const uint8_t *frame, size_t len, bool kick) {
  if (!_agg_queue_enabled || _fw_error) return -1;
  TxRate rate;
  rate.kind = TxRate::Auto; /* the firmware's rate control picks */
  const int slot = transmit_on(_aggq, frame, len, rate, false, kick);
  if (slot >= 0) {
    _agg_seq = static_cast<uint16_t>((_agg_seq + 1) & 0xfff);
    _agg_stats.frames++;
  }
  return slot;
}

void IwxDevice::kick_agg() {
  if (!_agg_queue_enabled || _fw_error) return;
  plat_fence();
  wr(IWX_HBUS_TARG_WRPTR, (_aggq.qid << 16) | _aggq.cur);
}

/* Compressed Block Ack notification (new TX API): the aggregation queue's
 * progress. Frees the ring up to the index the firmware gives. */
void IwxDevice::handle_ba_notif(const uint8_t *payload, size_t len) {
  if ((!_agg_queue_enabled && !_sta_agg_queue_enabled) || len < offsetof(iwx_compressed_ba_notif, tfd)) return;
  const auto *r = reinterpret_cast<const iwx_compressed_ba_notif *>(payload);
  const unsigned n = le16toh(r->tfd_cnt);
  if (len < offsetof(iwx_compressed_ba_notif, tfd) + n * sizeof(iwx_compressed_ba_tfd)) return;
  /* #39/#43: AX210 and AX201 logged "0 Block Acks" for every burst. Say
   * which queue, station and TID a notification names when none is ours,
   * so a report tells a queue mismatch from notifications never arriving. */
  bool ours = false;
  for (unsigned i = 0; i < n; i++) {
    const unsigned q = le16toh(r->tfd[i].q_num);
    if ((_agg_queue_enabled && q == _aggq.qid) || (_sta_agg_queue_enabled && q == _sta_aggq.qid)) ours = true;
  }
  if (!ours && _ba_foreign_logged < 8) {
    _ba_foreign_logged++;
    std::string queues;
    for (unsigned i = 0; i < n && i < 4; i++) queues += (i ? "," : "") + std::to_string(le16toh(r->tfd[i].q_num));
    _log("Block Ack notice for queue(s) [" + queues + "] (ours " + std::to_string(_aggq.qid) + "/" +
         std::to_string(_sta_aggq.qid) + "), station " + std::to_string(r->sta_id) + ", flags " +
         hex(le32toh(r->flags)) + ", " + std::to_string(n) + " TFD entries, txed " +
         std::to_string(le16toh(r->txed)) + ", done " + std::to_string(le16toh(r->done)));
  }
  for (unsigned i = 0; i < n; i++) {
    const iwx_compressed_ba_tfd &t = r->tfd[i];
    const unsigned q = le16toh(t.q_num);
    if (_agg_queue_enabled && q == _aggq.qid) {
      _agg_stats.notifs++;
      _agg_stats.txed += le16toh(r->txed);
      _agg_stats.acked += le16toh(r->done);
      _agg_stats.last_rate = le32toh(r->tx_rate);
      _agg_stats.peak_frames = std::max<uint32_t>(_agg_stats.peak_frames, le16toh(r->txed));
      const unsigned idx = le16toh(t.tfd_index) & (_aggq.count - 1);
      const unsigned freed = (idx + _aggq.count - _aggq.tail) % _aggq.count;
      if (freed <= _aggq.queued) {
        _aggq.tail = idx;
        _aggq.queued -= freed;
      }
    } else if (_sta_agg_queue_enabled && q == _sta_aggq.qid) {
      _sta_agg_notifs++;
      _sta_tx_acked += le16toh(r->done);
      const unsigned txed = le16toh(r->txed);
      const unsigned done = le16toh(r->done);
      if (txed > done) _sta_tx_failed += (txed - done);
      const unsigned idx = le16toh(t.tfd_index) & (_sta_aggq.count - 1);
      const unsigned freed = (idx + _sta_aggq.count - _sta_aggq.tail) % _sta_aggq.count;
      if (freed <= _sta_aggq.queued) {
        _sta_aggq.tail = idx;
        _sta_aggq.queued -= freed;
      }
    }
  }
}

bool IwxDevice::remove_rx_ba(unsigned tid, const uint8_t *peer) {
  if (bz()) {
    if (tid >= 8 || !((_ba_tids >> tid) & 1)) return true;
    if (!nic_lock()) return false;
    bool ok = true;
    uint32_t closed[8];
    unsigned nclosed = 0;
    for (unsigned i = 0; i < kBzPeers; i++) {
      BzPeer &p = _bz_peers[i];
      if (!p.used || !((p.valid >> tid) & 1)) continue;
      if (peer && std::memcmp(p.addr, peer, 6) != 0) continue;
      if (bz_baid_remove(p.baid[tid], tid, IWX_BZ_PEER_STA_ID + i)) {
        ok = false;
      } else {
        p.valid &= static_cast<uint8_t>(~(1u << tid));
        closed[nclosed++] = p.baid[tid];
      }
    }
    bz_recount_ba_tids();
    nic_unlock();
    /* Held frames out before the BAID can be handed to another session. */
    for (unsigned i = 0; i < nclosed; i++) _reorder.close(closed[i]);
    return ok;
  }
  if (tid >= 8 || !((_ba_tids >> tid) & 1)) return true;
  if (!nic_lock()) return false;
  iwx_add_sta_cmd cmd{};
  cmd.sta_id = 3;
  cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
  cmd.add_modify = IWX_STA_MODE_MODIFY;
  cmd.remove_immediate_ba_tid = static_cast<uint8_t>(tid);
  cmd.modify_mask = IWX_STA_MODIFY_REMOVE_BA_TID;
  uint32_t status = IWX_ADD_STA_SUCCESS;
  const int err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
  nic_unlock();
  _ba_tids &= static_cast<uint8_t>(~(1u << tid));
  return err == 0;
}

bool IwxDevice::set_country(const std::string &alpha2) {
  if (!_lar_enabled || alpha2.size() != 2) return false;
  if (!nic_lock()) return false;
  /* As ForbiddenItlwm answers a country learned from Wi-Fi: with the Wi-Fi
   * source. Other sources are tried after it. */
  bool ok = false;
  /* Measured on an AX201 (firmware 68): only the BIOS source is taken; the
   * Wi-Fi, ME and MCC-API sources answer status 4 and change nothing. An
   * AX200 (cc-a0-68) answered "00" to BIOS and Wi-Fi (issue #1): ME and
   * MCC-API are tried too, each logged with what the firmware answered. */
  for (int source : {int(IWX_MCC_SOURCE_BIOS), int(IWX_MCC_SOURCE_WIFI), int(IWX_MCC_SOURCE_ME),
                     int(IWX_MCC_SOURCE_MCC_API)}) {
    std::string got;
    if (update_mcc(alpha2, source, &got) == 0 && got == alpha2) {
      ok = true;
      break;
    }
  }
  nic_unlock();
  return ok;
}

int IwxDevice::phy_ctxt_cmd(uint32_t action) { return phy_cmd(0, _channel, _width, action); }

int IwxDevice::phy_cmd(unsigned phy_id, uint8_t channel, unsigned width_mhz, uint32_t action) {
  const bool is_2ghz = channel <= 14;
  uint8_t width = (width_mhz >= 80 && !is_2ghz) ? IWX_PHY_VHT_CHANNEL_MODE80
                        : (width_mhz >= 40)            ? IWX_PHY_VHT_CHANNEL_MODE40
                                                    : IWX_PHY_VHT_CHANNEL_MODE20;
  /* The station can use more 80 MHz blocks than the legacy AWDL choices. */
  uint8_t pos = IWX_PHY_VHT_CTRL_POS_1_BELOW;
  if (width == IWX_PHY_VHT_CHANNEL_MODE80) {
    const int primary = iwx_primary80_position(channel);
    if (primary >= 0) pos = static_cast<uint8_t>(primary);
    else width = IWX_PHY_VHT_CHANNEL_MODE20;
  } else if (width == IWX_PHY_VHT_CHANNEL_MODE40) {
    pos = (channel % 8 == 4 || channel == 149 || channel == 157 || channel <= 7)
              ? IWX_PHY_VHT_CTRL_POS_1_BELOW
              : IWX_PHY_VHT_CTRL_POS_1_ABOVE;
  }
  const uint8_t rx_ant = valid_rx_ant();
  const uint8_t chains = (rx_ant & (rx_ant - 1)) ? 2 : 1;
  const uint32_t rxchain = (uint32_t(rx_ant) << IWX_PHY_RX_CHAIN_VALID_POS) |
                           (uint32_t(chains) << IWX_PHY_RX_CHAIN_CNT_POS) |
                           (uint32_t(chains) << IWX_PHY_RX_CHAIN_MIMO_CNT_POS);
  const uint32_t id = IWX_FW_CMD_ID_AND_COLOR(phy_id, 0);
  const uint32_t lmac = (is_2ghz || !_fw.has_capa(IWX_UCODE_TLV_CAPA_CDB_SUPPORT))
                            ? IWX_LMAC_24G_INDEX
                            : IWX_LMAC_5G_INDEX;
  const uint8_t phy_ver = cmd_version(IWX_LONG_GROUP, IWX_PHY_CONTEXT_CMD);
  if (phy_ver == 3 || (bz() && phy_ver != IWX_FW_CMD_VER_UNKNOWN && phy_ver >= 3)) {
    const uint8_t rlc_ver = bz() ? cmd_version(IWX_DATA_PATH_GROUP, IWX_RLC_CONFIG_CMD) : IWX_FW_CMD_VER_UNKNOWN;
    const bool rlc_separate = bz() && rlc_ver != IWX_FW_CMD_VER_UNKNOWN && rlc_ver >= 2;
    iwx_phy_context_cmd cmd{};
    cmd.id_and_color = htole32(id);
    cmd.action = htole32(action);
    cmd.lmac_id = htole32(lmac);
    cmd.ci.band = is_2ghz ? IWX_PHY_BAND_24 : IWX_PHY_BAND_5;
    cmd.ci.channel = htole32(channel);
    cmd.ci.width = width;
    cmd.ci.ctrl_pos = pos;
    cmd.rxchain_info = htole32(rlc_separate ? 0u : rxchain);
    int err = send_cmd(IWX_PHY_CONTEXT_CMD, &cmd, sizeof(cmd), false, nullptr);
    if (!err && bz() && rlc_ver == 2 && action != IWX_FW_CTXT_ACTION_REMOVE) {
      iwx_rlc_config_cmd rlc{};
      rlc.phy_id = htole32(phy_id);
      rlc.rlc.rx_chain_info = htole32(rxchain);
      err = send_cmd(IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_RLC_CONFIG_CMD), &rlc, sizeof(rlc), false, nullptr);
    }
    return err;
  }
  if (_fw.has_capa(IWX_UCODE_TLV_CAPA_ULTRA_HB_CHANNELS)) {
    iwx_phy_context_cmd_uhb cmd{};
    cmd.id_and_color = htole32(id);
    cmd.action = htole32(action);
    cmd.ci.band = is_2ghz ? IWX_PHY_BAND_24 : IWX_PHY_BAND_5;
    cmd.ci.channel = htole32(channel);
    cmd.ci.width = width;
    cmd.ci.ctrl_pos = pos;
    cmd.rxchain_info = htole32(rxchain);
    cmd.txchain_info = htole32(valid_tx_ant());
    return send_cmd(IWX_PHY_CONTEXT_CMD, &cmd, sizeof(cmd), false, nullptr);
  }
  iwx_phy_context_cmd_v1 cmd{};
  cmd.id_and_color = htole32(id);
  cmd.action = htole32(action);
  cmd.ci.band = is_2ghz ? IWX_PHY_BAND_24 : IWX_PHY_BAND_5;
  cmd.ci.channel = channel;
  cmd.ci.width = width;
  cmd.ci.ctrl_pos = pos;
  cmd.rxchain_info = htole32(rxchain);
  cmd.txchain_info = htole32(valid_tx_ant());
  return send_cmd(IWX_PHY_CONTEXT_CMD, &cmd, sizeof(cmd), false, nullptr);
}

/* The AWDL listener: ForbiddenItlwm's monitor-mode MAC context. Its node
 * address is the AWDL address, which is what makes the firmware acknowledge
 * frames sent to us; the filters let everything else through as well. */
int IwxDevice::mac_ctxt_cmd(uint32_t action) {
  static const uint8_t kAwdlBssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
  static const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  iwx_mac_ctx_cmd cmd{};
  cmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
  cmd.action = htole32(action);
  if (action == IWX_FW_CTXT_ACTION_REMOVE)
    return send_cmd(IWX_MAC_CONTEXT_CMD, &cmd, sizeof(cmd), false, nullptr);
  uint32_t type = IWX_FW_MAC_TYPE_LISTENER;
  if (_opt.mac_type == MacType::Ibss) type = IWX_FW_MAC_TYPE_IBSS;
  if (_opt.mac_type == MacType::Sta) type = IWX_FW_MAC_TYPE_BSS_STA;
  cmd.mac_type = htole32(type);
  cmd.tsf_id = htole32(IWX_TSF_ID_A);
  std::memcpy(cmd.node_addr, _address.data(), 6);
  /* The listener keeps the broadcast BSSID even with experimental A-MPDU
   * sending: 0.1.22 gave it AWDL's BSSID and ACK rates there, and on an AX200
   * discovery stopped (PSF count frozen, 0 frames addressed to us, no peer for
   * minutes) while 0.1.20 with the broadcast BSSID still found the iPhone (#4). */
  /* Experimental A-MPDU only, ONLY once the peer has agreed (_agg_active, set
   * in agg_enable): ForbiddenItlwm's listener in its active BA state
   * (awdlCoexBaFilterOff, filter 3): AWDL's BSSID, ACK rates, group frames
   * only. Never apply this during discovery or while asking for Block Ack:
   * 0.1.28 stripped IN_CONTROL_AND_MGMT / IN_PROMISC during asking and the
   * firmware dropped every ADDBA Response (asks 80, agreed 0, #4).
   *
   * When BaFilter clears IN_PROMISC during an RX Block Ack session without TX
   * aggregation (!_promisc && _ba_station && !_agg_active), the MAC context
   * must still carry kAwdlBssid and basic ACK rates — leaving bssid_addr as
   * kBroadcast while IN_PROMISC is off made AX211/AX210 firmware drop unicast
   * AWDL frames (ICMP, TCP ACKs, mDNS QU replies) (#30, #118, #122). */
  const bool ba_listener =
      _opt.mac_type == MacType::Listener && _opt.aggregate && _agg_active && !_promisc && _ba_station;
  const bool rx_ba_filtered =
      _opt.mac_type == MacType::Listener && !_promisc && _ba_station;
  std::memcpy(cmd.bssid_addr,
              (_opt.mac_type == MacType::Listener && !ba_listener && !rx_ba_filtered) ? kBroadcast : kAwdlBssid, 6);
  if (_opt.mac_type != MacType::Listener || ba_listener || rx_ba_filtered) {
    /* ACK rates: the OFDM basics on 5 GHz, CCK and OFDM on 2.4 GHz. */
    cmd.cck_rates = htole32(_channel <= 14 ? 0x0f : 0);
    cmd.ofdm_rates = htole32(0x15); /* 6, 12, 24 */
    if (_opt.mac_type != MacType::Listener) cmd.qos_flags = htole32(IWX_MAC_QOS_FLG_TGN);
  }
  cmd.filter_flags = htole32(ba_listener ? IWX_MAC_FILTER_ACCEPT_GRP
                                         : (_promisc ? IWX_MAC_FILTER_IN_PROMISC : 0u) |
                                               IWX_MAC_FILTER_IN_CONTROL_AND_MGMT | IWX_MAC_FILTER_ACCEPT_GRP |
                                               IWX_MAC_FILTER_IN_BEACON | IWX_MAC_FILTER_IN_PROBE_REQUEST |
                                               IWX_MAC_FILTER_IN_CRC32);
  if (_opt.mac_type == MacType::Ibss) {
    cmd.ibss.bi = htole32(100);
    cmd.ibss.bi_reciprocal = htole32(iwx_reciprocal(100));
  } else if (_opt.mac_type == MacType::Sta) {
    cmd.sta.is_assoc = htole32(action != IWX_FW_CTXT_ACTION_ADD);
    cmd.sta.bi = htole32(100);
    cmd.sta.bi_reciprocal = htole32(iwx_reciprocal(100));
    cmd.sta.dtim_interval = htole32(100);
    cmd.sta.dtim_reciprocal = htole32(iwx_reciprocal(100));
    cmd.sta.listen_interval = htole32(10);
    cmd.sta.assoc_id = htole32(1);
  }
  return send_cmd(IWX_MAC_CONTEXT_CMD, &cmd, sizeof(cmd), false, nullptr);
}

int IwxDevice::binding_cmd(uint32_t action) {
  /* With the station on the listener's channel, both MACs share binding 0. */
  uint32_t macs[2] = {IWX_FW_CMD_ID_AND_COLOR(0, 0), IWX_FW_CMD_ID_AND_COLOR(1, 0)};
  const unsigned n = (_sta_mac_added && _sta_shared_binding) ? 2 : 1;
  return binding_for(0, 0, macs, n, _channel, action);
}

int IwxDevice::binding_for(unsigned id, unsigned phy, const uint32_t *macs, unsigned nmacs,
                           uint8_t channel, uint32_t action) {
  iwx_binding_cmd cmd{};
  cmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(id, 0));
  cmd.action = htole32(action);
  cmd.phy = htole32(IWX_FW_CMD_ID_AND_COLOR(phy, 0));
  for (unsigned i = 0; i < IWX_MAX_MACS_IN_BINDING; i++)
    cmd.macs[i] = htole32(i < nmacs ? macs[i] : IWX_FW_CTXT_INVALID);
  cmd.lmac_id = htole32((channel <= 14 || !_fw.has_capa(IWX_UCODE_TLV_CAPA_CDB_SUPPORT))
                            ? IWX_LMAC_24G_INDEX
                            : IWX_LMAC_5G_INDEX);
  uint32_t status = 0;
  const int err = send_cmd_status(IWX_BINDING_CONTEXT_CMD, &cmd, sizeof(cmd), &status);
  return err ? err : (status ? EIO : 0);
}

int IwxDevice::add_monitor_sta() {
  iwx_add_sta_cmd cmd{};
  cmd.sta_id = IWX_MONITOR_STA_ID;
  cmd.station_type = IWX_STA_GENERAL_PURPOSE;
  cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
  std::memset(cmd.addr, 0, sizeof(cmd.addr)); /* etheranyaddr */
  cmd.add_modify = 0;
  cmd.station_flags_msk = htole32(IWX_STA_FLG_FAT_EN_MSK | IWX_STA_FLG_MIMO_EN_MSK);
  cmd.tid_disable_tx = htole16(0xffff);
  uint32_t status = IWX_ADD_STA_SUCCESS;
  const int err = send_cmd_status(IWX_ADD_STA, &cmd, sizeof(cmd), &status);
  if (err) return err;
  return (status & IWX_ADD_STA_STATUS_MASK) == IWX_ADD_STA_SUCCESS ? 0 : EIO;
}

/* The firmware picks the queue number (TVQM); iwx_tvqm_enable_txq. */
/* Linux's MLD monitor station queues on IWL_MAX_TID_COUNT (8), not the legacy
 * MGMT TID 15. */
int IwxDevice::enable_monitor_queue() {
  return enable_queue(_dataq, IWX_MONITOR_STA_ID,
                      static_cast<uint8_t>(bz() ? IWX_MAX_TID_COUNT : IWX_MGMT_TID));
}

int IwxDevice::enable_queue(TxRing &ring, uint8_t sta_id, uint8_t tid) {
  std::vector<uint8_t> resp;
  int err;
  if (use_mld() && cmd_version(IWX_DATA_PATH_GROUP, IWX_SCD_QUEUE_CONFIG_CMD) == 3) {
    /* MLD / API 100: SCD_QUEUE_CONFIG_CMD v3 (0x05:0x17) replaces legacy
     * SCD_QUEUE_CFG; TX_QUEUE_CFG_ENABLE_QUEUE is not valid in v3 flags. */
    iwx_scd_queue_cfg_cmd cmd{};
    cmd.operation = htole32(IWX_SCD_QUEUE_ADD);
    cmd.u.add.sta_mask = htole32(1u << sta_id);
    cmd.u.add.tid = tid;
    cmd.u.add.flags = 0;
    cmd.u.add.cb_size = htole32(IWX_TFD_QUEUE_CB_SIZE(ring.count));
    cmd.u.add.bc_dram_addr = htole64(ring.bc_tbl.pa);
    cmd.u.add.tfdq_dram_addr = htole64(ring.desc.pa);
    err = send_cmd(IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_SCD_QUEUE_CONFIG_CMD), &cmd, sizeof(cmd), true, &resp);
  } else {
    iwx_tx_queue_cfg_cmd cmd{};
    cmd.sta_id = sta_id;
    cmd.tid = tid;
    cmd.flags = htole16(IWX_TX_QUEUE_CFG_ENABLE_QUEUE);
    cmd.cb_size = htole32(IWX_TFD_QUEUE_CB_SIZE(ring.count));
    cmd.byte_cnt_addr = htole64(ring.bc_tbl.pa);
    cmd.tfdq_addr = htole64(ring.desc.pa);
    err = send_cmd(IWX_SCD_QUEUE_CFG, &cmd, sizeof(cmd), true, &resp);
  }
  if (err) return err;
  const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(resp.data());
  if ((pkt->hdr.group_id & IWX_CMD_FAILED_MSK) ||
      iwx_rx_packet_payload_len(pkt) != sizeof(iwx_tx_queue_cfg_rsp))
    return EIO;
  const auto *r = reinterpret_cast<const iwx_tx_queue_cfg_rsp *>(pkt->data);
  ring.qid = le16toh(r->queue_number);
  /* The pointer as the firmware keeps it; the slot is it modulo the ring. */
  ring.cur = le16toh(r->write_pointer) % wrap();
  ring.tail = ring.cur & (ring.count - 1);
  ring.queued = 0;
  _log("transmit queue " + std::to_string(ring.qid) + " assigned for station " +
       std::to_string(sta_id) + ", write pointer " + std::to_string(ring.cur));
  return 0;
}

int IwxDevice::disable_queue(uint8_t sta_id, uint8_t tid) {
  if (!use_mld()) return 0;
  const uint8_t ver = cmd_version(IWX_DATA_PATH_GROUP, IWX_SCD_QUEUE_CONFIG_CMD);
  if (ver == IWX_FW_CMD_VER_UNKNOWN || ver == 0) return 0;
  iwx_scd_queue_cfg_cmd cmd{};
  cmd.operation = htole32(IWX_SCD_QUEUE_REMOVE);
  cmd.u.remove.sta_mask = htole32(1u << sta_id);
  cmd.u.remove.tid = htole32(tid);
  return send_cmd(IWX_WIDE_ID(IWX_DATA_PATH_GROUP, IWX_SCD_QUEUE_CONFIG_CMD), &cmd, sizeof(cmd), false, nullptr);
}

int IwxDevice::mld_mac_cmd(uint32_t action, const uint8_t *address) {
  iwx_mac_config_cmd cmd{};
  cmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
  cmd.action = htole32(action);
  if (action != IWX_FW_CTXT_ACTION_REMOVE) {
    /* MLD firmware has no IBSS (Linux mld dropped it): --mac-type ibss stays a
     * listener; sta gives the BSS_STA context Linux sends unassociated
     * authentication frames from. */
    if (action == IWX_FW_CTXT_ACTION_ADD && _opt.mac_type != MacType::Listener)
      _log(_opt.mac_type == MacType::Sta ? "Bz listener MAC as BSS_STA (--mac-type sta)"
                                         : "Bz: --mac-type ibss is not supported, using listener");
    cmd.mac_type = htole32(_opt.mac_type == MacType::Sta ? IWX_FW_MAC_TYPE_BSS_STA
                                                         : IWX_FW_MAC_TYPE_LISTENER);
    std::memcpy(cmd.local_mld_addr, address ? address : _address.data(), 6);
    /* No PROMISC on Bz: with it the firmware hands every ACK to the host and
     * neither matches them to its own unicast nor acknowledges frames for us
     * (every unicast ends in LONG_LIMIT). CONTROL_AND_MGMT is what lets the
     * AWDL action frames (foreign BSSID) through. */
    cmd.filter_flags = htole32(IWX_MAC_CFG_FILTER_ACCEPT_CONTROL_AND_MGMT |
                               IWX_MAC_CFG_FILTER_ACCEPT_GRP |
                               IWX_MAC_CFG_FILTER_ACCEPT_BEACON);
    cmd.he_support = htole16(1);
  }
  return send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_MAC_CONFIG_CMD), &cmd, sizeof(cmd), false, nullptr);
}

int IwxDevice::mld_link_add() {
  iwx_link_config_cmd cmd{};
  cmd.action = htole32(IWX_FW_CTXT_ACTION_ADD);
  cmd.link_id = htole32(0);
  cmd.mac_id = htole32(0);
  cmd.phy_id = htole32(IWX_FW_CTXT_INVALID);
  std::memcpy(cmd.local_link_addr, _address.data(), 6);
  cmd.spec_link_id = 0;
  return send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_LINK_CONFIG_CMD), &cmd, sizeof(cmd), false, nullptr);
}

int IwxDevice::mld_link_modify(bool active, uint32_t modify_mask, const uint8_t *address) {
  /* EDCA defaults for a link (802.11-2016 table 9-137), firmware AC order.
   * The listener needs them as much as a station link: with a Wi-Fi station
   * time-sharing the radio, an AWDL link without EDCA/QoS parameters loses
   * its transmit opportunities (Concurrent mode). */
  struct Edca {
    uint16_t cw_min, cw_max;
    uint8_t aifsn;
    uint16_t txop_32us;
    uint8_t fifo;
  };
  static constexpr Edca kEdca[IWX_AC_NUM] = {
      {15, 1023, 7, 0, IWX_GEN2_EDCA_TX_FIFO_BK},
      {15, 1023, 3, 0, IWX_GEN2_EDCA_TX_FIFO_BE},
      {7, 15, 2, 94, IWX_GEN2_EDCA_TX_FIFO_VI},
      {3, 7, 2, 47, IWX_GEN2_EDCA_TX_FIFO_VO},
  };
  iwx_link_config_cmd cmd{};
  cmd.action = htole32(IWX_FW_CTXT_ACTION_MODIFY);
  cmd.link_id = htole32(0);
  cmd.mac_id = htole32(0);
  cmd.phy_id = htole32(0);
  std::memcpy(cmd.local_link_addr, address ? address : _address.data(), 6);
  /* Apply the EDCA values when the listener link is activated. */
  if (active) modify_mask |= IWX_LINK_CONTEXT_MODIFY_QOS_PARAMS;
  cmd.modify_mask = htole32(modify_mask);
  cmd.active = htole32(active ? 1u : 0u);
  cmd.cck_rates = htole32(_channel <= 14 ? 0x0fu : 0u);
  cmd.ofdm_rates = htole32(0x15u); /* 6, 12, 24 */
  cmd.short_slot = htole32(_channel > 14 ? 1u : 0u);
  for (unsigned ac = 0; ac < IWX_AC_NUM; ac++) {
    cmd.ac[ac].cw_min = htole16(kEdca[ac].cw_min);
    cmd.ac[ac].cw_max = htole16(kEdca[ac].cw_max);
    cmd.ac[ac].aifsn = kEdca[ac].aifsn;
    cmd.ac[ac].fifos_mask = static_cast<uint8_t>(1u << kEdca[ac].fifo);
    cmd.ac[ac].edca_txop = htole16(static_cast<uint16_t>(kEdca[ac].txop_32us * 32));
  }
  cmd.qos_flags = htole32(IWX_MAC_QOS_FLG_UPDATE_EDCA |
                          (_width > 20 && _channel > 14 ? IWX_MAC_QOS_FLG_TGN : 0u));
  cmd.bi = htole32(100);
  cmd.dtim_interval = htole32(100);
  cmd.spec_link_id = 0;
  return send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_LINK_CONFIG_CMD), &cmd, sizeof(cmd), false, nullptr);
}

int IwxDevice::mld_add_monitor_sta() {
  iwx_sta_cfg_cmd_v2 cmd{};
  cmd.sta_id = htole32(IWX_MONITOR_STA_ID);
  cmd.link_id = htole32(0);
  cmd.station_type = htole32(IWX_STATION_TYPE_BCAST_MGMT);
  if (_fw.has_capa(IWX_UCODE_TLV_CAPA_STA_EXP_MFP_SUPPORT)) cmd.mfp = htole32(1);
  const uint8_t ver = cmd_version(IWX_MAC_CONF_GROUP, IWX_STA_CONFIG_CMD);
  const size_t sz =
      (ver != IWX_FW_CMD_VER_UNKNOWN && ver > 1) ? sizeof(iwx_sta_cfg_cmd_v2) : sizeof(iwx_sta_cfg_cmd_v1);
  return send_cmd(IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_STA_CONFIG_CMD), &cmd, sz, false, nullptr);
}

int IwxDevice::monitor_smart_fifo(uint32_t state) {
  iwx_sf_cfg_cmd sf{};
  sf.state = htole32(state);
  sf.watermark[IWX_SF_LONG_DELAY_ON] = htole32(IWX_SF_W_MARK_SCAN);
  sf.watermark[IWX_SF_FULL_ON] = htole32(state == IWX_SF_FULL_ON ? IWX_SF_W_MARK_LEGACY : IWX_SF_W_MARK_MIMO2);
  for (unsigned i = 0; i < IWX_SF_NUM_SCENARIO; i++)
    for (unsigned j = 0; j < IWX_SF_NUM_TIMEOUT_TYPES; j++)
      sf.long_delay_timeouts[i][j] = htole32(IWX_SF_LONG_DELAY_AGING_TIMER);
  if (state == IWX_SF_FULL_ON) {
    static const uint32_t full_on[IWX_SF_NUM_SCENARIO][IWX_SF_NUM_TIMEOUT_TYPES] = {
        {IWX_SF_SINGLE_UNICAST_AGING_TIMER, IWX_SF_SINGLE_UNICAST_IDLE_TIMER},
        {IWX_SF_AGG_UNICAST_AGING_TIMER, IWX_SF_AGG_UNICAST_IDLE_TIMER},
        {IWX_SF_MCAST_AGING_TIMER, IWX_SF_MCAST_IDLE_TIMER},
        {IWX_SF_BA_AGING_TIMER, IWX_SF_BA_IDLE_TIMER},
        {IWX_SF_TX_RE_AGING_TIMER, IWX_SF_TX_RE_IDLE_TIMER}};
    std::memcpy(sf.full_on_timeouts, full_on, sizeof(full_on));
  } else {
    static const uint32_t defaults[IWX_SF_NUM_SCENARIO][IWX_SF_NUM_TIMEOUT_TYPES] = {
        {IWX_SF_SINGLE_UNICAST_AGING_TIMER_DEF, IWX_SF_SINGLE_UNICAST_IDLE_TIMER_DEF},
        {IWX_SF_AGG_UNICAST_AGING_TIMER_DEF, IWX_SF_AGG_UNICAST_IDLE_TIMER_DEF},
        {IWX_SF_MCAST_AGING_TIMER_DEF, IWX_SF_MCAST_IDLE_TIMER_DEF},
        {IWX_SF_BA_AGING_TIMER_DEF, IWX_SF_BA_IDLE_TIMER_DEF},
        {IWX_SF_TX_RE_AGING_TIMER_DEF, IWX_SF_TX_RE_IDLE_TIMER_DEF}};
    std::memcpy(sf.full_on_timeouts, defaults, sizeof(defaults));
  }
  return send_cmd(IWX_REPLY_SF_CFG_CMD, &sf, sizeof(sf), false, nullptr);
}

bool IwxDevice::start_monitor(uint8_t channel, unsigned width_mhz, const uint8_t address[6]) {
  _channel = channel;
  _width = width_mhz;
  std::memcpy(_address.data(), address, 6);
  if (!nic_lock()) return false;
  auto bz_step_check = [&](const char *step) -> bool {
    if (!bz()) return true;
    std::string what;
    if (!fw_error_state(what)) return true;
    _log(std::string("Bz error latched after ") + step + ": " + what);
    return false;
  };
  if (!bz_step_check("init")) {
    nic_unlock();
    check_health();
    return false;
  }
  int err = phy_ctxt_cmd(IWX_FW_CTXT_ACTION_ADD);
  if (err) _log("PHY context add failed (" + std::to_string(err) + ")");
  else _phy_added = true;
  if (!err && !bz_step_check("PHY_CONTEXT_CMD")) err = EIO;
  if (use_mld()) {
    if (!err) {
      err = mld_mac_cmd(IWX_FW_CTXT_ACTION_ADD);
      if (err) _log("MLD MAC config add failed (" + std::to_string(err) + ")");
      else _mac_added = true;
    }
    if (!err && !bz_step_check("MAC_CONFIG_CMD")) err = EIO;
    if (!err && !_fw.has_api(IWX_UCODE_TLV_API_SMART_FIFO_OFFLOAD)) {
      /* iwlwifi's MLD monitor has no associated station: leave Smart FIFO
       * uninitialized before binding. BE200 API 100 owns Smart FIFO itself. */
      err = monitor_smart_fifo(IWX_SF_UNINIT);
      if (err) _log("MLD monitor Smart FIFO config failed (" + std::to_string(err) + ")");
    }
    if (!err) {
      err = mld_link_add();
      if (err) _log("MLD link add failed (" + std::to_string(err) + ")");
      else _link_added = true;
    }
    if (!err && !bz_step_check("LINK_CONFIG_CMD(ADD)")) err = EIO;
    if (!err) {
      /* Bind link 0 to PHY 0 while inactive, then activate with basic rates. */
      err = mld_link_modify(false, 0);
      if (err) _log("MLD link PHY bind failed (" + std::to_string(err) + ")");
    }
    if (!err && !bz_step_check("LINK_CONFIG_CMD(BIND)")) err = EIO;
    if (!err) {
      err = mld_link_modify(true, IWX_LINK_CONTEXT_MODIFY_ACTIVE | IWX_LINK_CONTEXT_MODIFY_RATES_INFO);
      if (err) _log("MLD link activate failed (" + std::to_string(err) + ")");
      else _link_active = true;
    }
    if (!err && !bz_step_check("LINK_CONFIG_CMD(ACTIVATE)")) err = EIO;
    if (!err) {
      err = mld_add_monitor_sta();
      if (err) _log("MLD monitor station add failed (" + std::to_string(err) + ")");
      else _sta_added = true;
    }
    if (!err && !bz_step_check("STA_CONFIG_CMD")) err = EIO;
    if (!err) {
      err = enable_monitor_queue();
      if (err) _log("transmit queue setup failed (" + std::to_string(err) + ")");
      else _queue_enabled = true;
    }
    if (!err && !bz_step_check("SCD_QUEUE_CONFIG_CMD")) err = EIO;
  } else {
    if (!err) {
      err = mac_ctxt_cmd(IWX_FW_CTXT_ACTION_ADD);
      if (err) _log("MAC context add failed (" + std::to_string(err) + ")");
      else _mac_added = true;
    }
    if (!err) {
      err = binding_cmd(IWX_FW_CTXT_ACTION_ADD);
      if (err) _log("binding failed (" + std::to_string(err) + ")");
      else _binding_added = true;
    }
    if (!err) {
      err = add_monitor_sta();
      if (err) _log("monitor station add failed (" + std::to_string(err) + ")");
      else _sta_added = true;
    }
    if (!err) {
      err = enable_monitor_queue();
      if (err) _log("transmit queue setup failed (" + std::to_string(err) + ")");
      else _queue_enabled = true;
    }
    /* The rest of iwx_run for a monitor: the MAC context again, now "running",
     * smart FIFO fully on, every multicast let through, no MAC power save, and
     * the whole air time for this binding where the firmware wants quotas. */
    if (!err) {
      err = mac_ctxt_cmd(IWX_FW_CTXT_ACTION_MODIFY);
      if (err) _log("MAC context modify failed (" + std::to_string(err) + ")");
    }
  }
  if (!err && !use_mld()) {
    err = monitor_smart_fifo(IWX_SF_FULL_ON);
    if (err) _log("smart FIFO config failed (" + std::to_string(err) + ")");
  }
  if (!err && !use_mld()) {
    /* MLD listeners already accept group frames in MAC_CONFIG. On MLD,
     * MCAST_FILTER_CMD is for an associated station with a real BSSID. */
    /* A byte buffer: the struct ends in a zero-size array. */
    alignas(4) uint8_t buf[roundup(sizeof(iwx_mcast_filter_cmd), 4)] = {};
    auto *mc = reinterpret_cast<iwx_mcast_filter_cmd *>(buf);
    mc->filter_own = 1;
    mc->pass_all = 1;
    std::memset(mc->bssid, 0xff, sizeof(mc->bssid));
    err = send_cmd(IWX_MCAST_FILTER_CMD, buf, sizeof(buf), false, nullptr);
    if (err) _log("multicast filter failed (" + std::to_string(err) + ")");
  }
  if (!err && !use_mld()) {
    /* MLD listener MACs have no BSS power context and gl-c0-fm-c0-100 hangs on
     * legacy MAC_PM_POWER_TABLE (0xa9); device-wide POWER_TABLE_CMD already
     * disabled power save during init. */
    iwx_mac_power_cmd pm{};
    pm.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
    pm.keep_alive_seconds = htole16(25);
    err = send_cmd(IWX_MAC_PM_POWER_TABLE, &pm, sizeof(pm), false, nullptr);
    if (err) _log("MAC power table failed (" + std::to_string(err) + ")");
  }
  if (!err && !use_mld() && !_fw.has_capa(IWX_UCODE_TLV_CAPA_DYNAMIC_QUOTA)) {
    iwx_time_quota_cmd q{};
    for (unsigned i = 0; i < IWX_MAX_BINDINGS; i++)
      q.quotas[i].id_and_color = htole32(IWX_FW_CTXT_INVALID);
    q.quotas[0].id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(0, 0));
    q.quotas[0].quota = htole32(IWX_MAX_QUOTA);
    err = send_cmd(IWX_TIME_QUOTA_CMD, &q, sizeof(q), false, nullptr);
    if (err) _log("time quota failed (" + std::to_string(err) + ")");
  }
  nic_unlock();
  if (bz()) {
    /* A firmware assertion may arrive during or right after monitor bring-up.
     * Always dump error tables if a fault latched before returning failure. */
    check_health();
    if (_fw_error) return false;
  }
  if (!err)
    _log("listening on channel " + std::to_string(_channel) + " at " + std::to_string(_width) +
         " MHz");
  return err == 0;
}

bool IwxDevice::set_channel(uint8_t channel, unsigned width_mhz) {
  if (!_phy_added) return false;
  if (channel == _channel && width_mhz == _width) return true;
  const uint8_t old_channel = _channel;
  const unsigned old_width = _width;
  _channel = channel;
  _width = width_mhz;
  /* Changing band under a live binding is what itlwm refuses (it asserts the
   * firmware on CDB parts); a modify within one band is safe. */
  if (_sta_shared_binding && channel != _sta.channel) {
    _log("channel change skipped while station shares binding on channel " + std::to_string(_sta.channel));
    _channel = old_channel;
    _width = old_width;
    return false;
  }
  if ((old_channel <= 14) != (channel <= 14)) {
    _log("channel change across bands is not supported live");
    _channel = old_channel;
    _width = old_width;
    return false;
  }
  if (!nic_lock()) return false;
  int err;
  if (bz()) {
    /* MLD: the link holds the PHY, so a PHY modify under a live link fails
     * (138) and then takes the firmware down. Let the link go, move the PHY,
     * bind and activate again (Linux iwl_mld_change_chanctx). */
    err = mld_link_modify(false, IWX_LINK_CONTEXT_MODIFY_ACTIVE);
    if (!err) err = phy_ctxt_cmd(IWX_FW_CTXT_ACTION_MODIFY);
    if (!err) err = mld_link_modify(false, IWX_LINK_CONTEXT_MODIFY_RATES_INFO);
    if (!err) {
      err = mld_link_modify(true, IWX_LINK_CONTEXT_MODIFY_ACTIVE | IWX_LINK_CONTEXT_MODIFY_RATES_INFO);
      if (!err) _link_active = true;
    }
  } else {
    err = phy_ctxt_cmd(IWX_FW_CTXT_ACTION_MODIFY);
  }
  nic_unlock();
  if (err) {
    _log("channel change failed (" + std::to_string(err) + ")");
    _channel = old_channel;
    _width = old_width;
    return false;
  }
  _log("now on channel " + std::to_string(channel));
  return true;
}

bool IwxDevice::set_address(const uint8_t address[6]) {
  if (std::memcmp(address, _address.data(), 6) == 0) return true;
  if (!_mac_added) {
    std::memcpy(_address.data(), address, 6);
    return true;
  }
  if (!nic_lock()) return false;
  int err = 0;
  if (use_mld()) {
    /* awdld picks its own address. Change both firmware contexts with the
     * link inactive, and commit our address only once firmware has accepted it. */
    const bool reactivate = _link_added && _link_active;
    if (reactivate) {
      err = mld_link_modify(false, IWX_LINK_CONTEXT_MODIFY_ACTIVE);
      if (!err) _link_active = false;
    }
    if (!err) err = mld_mac_cmd(IWX_FW_CTXT_ACTION_MODIFY, address);
    if (!err && _link_added) err = mld_link_modify(false, 0, address);
    if (!err && reactivate) {
      err = mld_link_modify(true, IWX_LINK_CONTEXT_MODIFY_ACTIVE | IWX_LINK_CONTEXT_MODIFY_RATES_INFO, address);
      if (!err) _link_active = true;
    }
    if (!err) std::memcpy(_address.data(), address, 6);
  } else {
    std::memcpy(_address.data(), address, 6);
    err = mac_ctxt_cmd(IWX_FW_CTXT_ACTION_MODIFY);
  }
  nic_unlock();
  if (err) _log("address change failed (" + std::to_string(err) + ")");
  return err == 0;
}

/* ---------------------------------------------------------------- transmit */

int IwxDevice::transmit_slot(const uint8_t *frame, size_t len, const TxRate &rate, bool kick) {
  if (!_queue_enabled || _fw_error) return -1;
  return transmit_on(_dataq, frame, len, rate, false, kick);
}

void IwxDevice::kick_data() {
  if (!_queue_enabled || _fw_error) return;
  plat_fence();
  wr(IWX_HBUS_TARG_WRPTR, (_dataq.qid << 16) | _dataq.cur);
}

int IwxDevice::sta_transmit(const uint8_t *frame, size_t len, const TxRate &rate, bool encrypt) {
  if (!_sta_queue_enabled || _fw_error) return -1;
  if (_sta_paused) {
    _sta_pause_dropped++;
    return -1;
  }
  return transmit_on(_staq, frame, len, rate, encrypt);
}

int IwxDevice::transmit_on(TxRing &ring, const uint8_t *frame, size_t len, const TxRate &rate,
                           bool encrypt, bool kick) {
  const size_t hdrlen = hdrlen_80211(frame, len);
  if (!hdrlen || len - hdrlen > kTxPayloadSlot || hdrlen > 64) return -1;
  if (ring.queued >= ring.count - 2) {
    if (&ring == &_dataq) _tx_stats.dropped_full++;
    return -1;
  }
  const unsigned idx = ring.cur & (ring.count - 1);
  auto *cmd = reinterpret_cast<iwx_device_cmd *>(ring.cmd_slot[idx].va);
  const uint64_t cmd_pa = ring.cmd_slot[idx].pa;
  cmd->hdr.cmd = IWX_TX_CMD;
  cmd->hdr.group_id = 0;
  cmd->hdr.qid = static_cast<uint8_t>(ring.qid);
  cmd->hdr.idx = static_cast<uint8_t>(idx);

  /* Rate: always fixed by us (IWX_TX_FLAGS_CMD_RATE), on the first valid
   * antenna, in the encoding the TX command version expects. */
  const uint8_t tx_ant = valid_tx_ant();
  const unsigned ant_idx = (tx_ant & 1) ? 0 : (tx_ant & 2) ? 1 : 0;
  uint32_t rnf = (1u << ant_idx) << IWX_RATE_MCS_ANT_POS;
  const uint8_t tx_ver = cmd_version(IWX_LONG_GROUP, IWX_TX_CMD);
  const bool new_format = tx_ver > 8 && tx_ver != IWX_FW_CMD_VER_UNKNOWN;
  if (rate.kind == TxRate::Vht && new_format) {
    const unsigned mcs = std::min(rate.mcs, 9u);
    rnf = (rate.nss > 1 ? IWX_RATE_MCS_ANT_AB_MSK : rnf) | IWX_RATE_MCS_VHT_MSK |
          IWX_RATE_MCS_LDPC_MSK | IWX_RATE_MCS_SGI_MSK |
          (rate.wide80 && _width >= 80 ? IWX_RATE_MCS_CHAN_WIDTH_80 : 0u) |
          (rate.nss > 1 ? IWX_RATE_MCS_NSS_MSK : 0u) | mcs;
  } else if (rate.kind == TxRate::Ht || rate.kind == TxRate::Vht) {
    const unsigned mcs = std::min(rate.mcs, 15u);
    if (mcs > 7) rnf = IWX_RATE_MCS_ANT_AB_MSK;
    if (new_format)
      rnf |= IWX_RATE_MCS_HT_MSK | (mcs > 7 ? IWX_RATE_MCS_NSS_MSK : 0u) | (mcs & 7u);
    else
      rnf |= IWX_RATE_MCS_HT_MSK_V1 | mcs;
  } else {
    unsigned r = rate.mbps_x2;
    const bool cck = cck_index(r) >= 0;
    if (cck && _channel > 14) r = 12; /* no CCK on 5 GHz */
    if (cck_index(r) >= 0) {
      rnf |= new_format ? (IWX_RATE_MCS_CCK_MSK | uint32_t(cck_index(r)))
                        : (IWX_RATE_MCS_CCK_MSK_V1 | cck_plcp(r));
    } else {
      if (ofdm_index(r) < 0) r = 12;
      rnf |= new_format ? (IWX_RATE_MCS_LEGACY_OFDM_MSK | uint32_t(ofdm_index(r))) : ofdm_plcp(r);
    }
  }

  /* TX command: version 7 (22000) or 8 (AX210 family, 16-bit flags and a
   * 32-bit offload word), the 802.11 header right behind it (iwx_tx). */
  const bool fixed = rate.kind != TxRate::Auto;
  const uint32_t txflags = (fixed ? IWX_TX_FLAGS_CMD_RATE : 0u) | (encrypt ? 0u : IWX_TX_FLAGS_ENCRYPT_DIS);
  size_t tx_size;
  if (gen3()) {
    auto *tx = reinterpret_cast<iwx_tx_cmd_gen3 *>(cmd->data);
    tx_size = sizeof(*tx);
    std::memset(tx, 0, tx_size);
    tx->len = htole16(static_cast<uint16_t>(len));
    tx->offload_assist = htole32(offload_header(static_cast<unsigned>(hdrlen)));
    tx->flags = htole16(static_cast<uint16_t>(txflags));
    tx->rate_n_flags = htole32(fixed ? rnf : 0u);
  } else {
    auto *tx = reinterpret_cast<iwx_tx_cmd_gen2 *>(cmd->data);
    tx_size = sizeof(*tx);
    std::memset(tx, 0, tx_size);
    tx->len = htole16(static_cast<uint16_t>(len));
    tx->offload_assist = htole16(offload_header(static_cast<unsigned>(hdrlen)));
    tx->flags = htole32(txflags);
    tx->rate_n_flags = htole32(fixed ? rnf : 0u);
  }
  std::memcpy(reinterpret_cast<uint8_t *>(cmd->data) + tx_size, frame, hdrlen);
  if (hdrlen % 4)
    std::memset(reinterpret_cast<uint8_t *>(cmd->data) + tx_size + hdrlen, 0, 4 - (hdrlen % 4));

  const size_t payload_len = len - hdrlen;
  uint8_t *payload = ring.payload[idx].va;
  const uint64_t payload_pa = ring.payload[idx].pa;
  if (payload_len) std::memcpy(payload, frame + hdrlen, payload_len);

  auto *desc = reinterpret_cast<iwx_tfh_tfd *>(ring.desc.va) + idx;
  std::memset(desc, 0, sizeof(*desc));
  const unsigned num_tbs = payload_len ? 3 : 2;
  desc->num_tbs = htole16(static_cast<uint16_t>(num_tbs));
  desc->tbs[0].tb_len = htole16(IWX_FIRST_TB_SIZE);
  uint64_t a = htole64(cmd_pa);
  std::memcpy(&desc->tbs[0].addr, &a, sizeof(a));
  const size_t tb1 = sizeof(iwx_cmd_header) + tx_size + hdrlen - IWX_FIRST_TB_SIZE;
  desc->tbs[1].tb_len = htole16(static_cast<uint16_t>((tb1 + 3) & ~size_t(3)));
  a = htole64(cmd_pa + IWX_FIRST_TB_SIZE);
  std::memcpy(&desc->tbs[1].addr, &a, sizeof(a));
  if (payload_len) {
    desc->tbs[2].tb_len = htole16(static_cast<uint16_t>(payload_len));
    a = htole64(payload_pa);
    std::memcpy(&desc->tbs[2].addr, &a, sizeof(a));
  }

  /* Byte count table: fetch chunks in the top bits, then dwords (22000)
   * or bytes (AX210 family) (iwx_tx_update_byte_tbl). */
  const unsigned filled = offsetof(iwx_tfh_tfd, tbs) + num_tbs * sizeof(iwx_tfh_tb);
  const unsigned chunks = howmany(filled, 64) - 1;
  if (gen3())
    reinterpret_cast<iwx_gen3_bc_tbl *>(ring.bc_tbl.va)->tfd_offset[idx] =
        htole16(static_cast<uint16_t>(len | (chunks << 14)));
  else
    reinterpret_cast<iwx_agn_scd_bc_tbl *>(ring.bc_tbl.va)->tfd_offset[idx] =
        htole16(static_cast<uint16_t>(howmany(len, 4) | (chunks << 12)));

  _tx_hist[_tx_hist_at++ % 8] = {static_cast<uint16_t>(len), static_cast<uint8_t>(hdrlen),
                                 frame[0], frame[1], frame[4], rnf, static_cast<uint8_t>(idx)};
  ring.cur = (ring.cur + 1) % wrap();
  ring.queued++;
  if (kick) {
    plat_fence();
    wr(IWX_HBUS_TARG_WRPTR, (ring.qid << 16) | ring.cur);
  }
  /* Per ring: tx_stats' done/failed come from the AWDL data ring only
   * (handle_tx_resp), so counting the station's and the A-MPDU ring's frames
   * here made "tx: N submitted" run far ahead of "done + failed" under
   * Concurrent, which read as loss (#11). The station has its own count; the
   * A-MPDU ring has agg_stats().frames. Counting only, no change to sending. */
  if (&ring == &_dataq) _tx_stats.submitted++;
  else if (&ring == &_staq || &ring == &_sta_aggq) _sta_tx_submitted++;
  return static_cast<int>(idx);
}

/* ------------------------------------------------------------------ receive */

void IwxDevice::deliver_deferred() {
  while (_cmd_depth == 0 && !_deferred_rx.empty()) {
    DeferredRx d = std::move(_deferred_rx.front());
    _deferred_rx.pop_front();
    d.meta.data = d.frame.data();
    d.meta.len = d.frame.size();
    if (d.raw) rx_in(d.meta, d.reorder_data, d.amsdu, d.last_subframe);
    else if (_on_rx) _on_rx(d.meta);
  }
}

int IwxDevice::poll() {
  if (!_rx_stat) return 0;
  if (_cmd_depth == 0) {
    deliver_deferred();
    if (bz()) _reorder.expire(now_us());
  }
  /* Bz publishes a 16-bit producer index in the status word and 4-byte
   * completions carrying the buffer's rbid; the older families use
   * iwx_rb_status and the slot holds the buffer. */
  const auto closed = [&] {
    if (bz())
      return static_cast<unsigned>(le16toh(*reinterpret_cast<volatile uint16_t *>(_rx_stat.va))) &
             (IWX_RX_MQ_RING_COUNT - 1);
    const auto *stat = reinterpret_cast<volatile iwx_rb_status *>(_rx_stat.va);
    return static_cast<unsigned>(le16toh(stat->closed_rb_num) & 0xfff) & (IWX_RX_MQ_RING_COUNT - 1);
  };
  int handled = 0;
  /* A receive handler may send a firmware command, whose wait polls again
   * (re-entering here). So (Codex review #1 and round 2 #2/#3):
   *  - the slot is claimed before it is handled, so no buffer is handled twice;
   *  - the card's position is read afresh every round, so after a nested
   *    poll has gone ahead the outer one stops instead of running on round
   *    the ring into buffers the card has not filled;
   *  - buffers still being handled further up the stack are not given back:
   *    the card is told only about those before the oldest one in use. */
  for (unsigned hw = closed(); _rx_cur != hw && handled < int(IWX_RX_MQ_RING_COUNT); hw = closed()) {
    const unsigned pos = _rx_cur;
    unsigned idx = pos;
    if (bz()) {
      const auto *cq = reinterpret_cast<const iwx_rx_completion_desc_bz *>(_rx_used.va);
      const iwx_rx_completion_desc_bz c = cq[pos];
      const unsigned rbid = le16toh(c.rbid);
      if (c.reserved[0] || rbid == 0 || rbid > IWX_RX_MQ_RING_COUNT) {
        _log("bad Bz receive completion in slot " + std::to_string(pos) + " (rbid " +
             std::to_string(rbid) + ", flags " + std::to_string(c.flags) + ")");
        _fw_error = true;
        break;
      }
      idx = rbid - 1;
    }
    _rx_cur = (_rx_cur + 1) % IWX_RX_MQ_RING_COUNT;
    _rx_inflight.push_back(pos);
    handle_rb(idx);
    _rx_inflight.pop_back();
    handled++;
  }
  if (handled) {
    if (bz()) {
      /* iwl_pcie_irq_handler acks the FH status along with CSR_INT. */
      const uint32_t fh = rd(IWX_CSR_FH_INT_STATUS);
      if (fh && fh != 0xffffffffu) wr(IWX_CSR_FH_INT_STATUS, fh);
    }
    /* The card wants the free index in multiples of 8 (iwx_notif_intr). */
    const unsigned frontier = _rx_inflight.empty() ? _rx_cur : _rx_inflight.front();
    const unsigned w = frontier == 0 ? IWX_RX_MQ_RING_COUNT - 1 : frontier - 1u;
    ring_rx_doorbell(w & ~7u);
  }
  return handled;
}

void IwxDevice::handle_rb(unsigned index) {
  /* The card fills the free buffers in order and closes them in order, so
   * slot `index` holds buffer `index`, as itlwm's iwx_notif_intr assumes. */
  const uint8_t *buf = _rx_bufs[index].va;
  const size_t minsz = sizeof(uint32_t) + sizeof(iwx_cmd_header);
  size_t offset = 0;
  while (offset + minsz < _rx_buf_bytes) {
    const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(buf + offset);
    const int qid = pkt->hdr.qid & ~0x80;
    if ((qid == 0 && pkt->hdr.idx == 0 && pkt->hdr.cmd == 0 && pkt->hdr.group_id == 0) ||
        pkt->len_n_flags == htole32(IWX_FH_RSCSR_FRAME_INVALID))
      break;
    const size_t len = sizeof(pkt->len_n_flags) + iwx_rx_packet_len(pkt);
    if (len < minsz || len > _rx_buf_bytes - offset) break;
    handle_packet(buf + offset, len);
    offset += roundup(len, IWX_FH_RSCSR_FRAME_ALIGN);
    if (gen3()) break; /* one packet per buffer on the AX210 family (iwx_rx_pkt) */
  }
  /* Clear only the consumed portion + next header so a stale buffer never
   * parses twice without zeroing unused tail bytes on every 8 KiB slot. */
  const size_t clear_bytes = std::min<size_t>(offset + minsz + IWX_FH_RSCSR_FRAME_ALIGN, _rx_buf_bytes);
  std::memset(_rx_bufs[index].va, 0, clear_bytes);
  post_rx_buf(index);
}

void IwxDevice::post_rx_buf(unsigned index) {
  if (gen3()) {
    auto *bd = reinterpret_cast<iwx_rx_transfer_desc *>(_rx_free.va) + index;
    bd->addr = htole64(_rx_bufs[index].pa);
    bd->rbid = htole16(static_cast<uint16_t>((bz() ? index + 1 : index) & 0x0fff));
  } else {
    reinterpret_cast<uint64_t *>(_rx_free.va)[index] = htole64(_rx_bufs[index].pa | (index & 0x0fff));
  }
}

void IwxDevice::handle_packet(const uint8_t *raw, size_t len) {
  const auto *pkt = reinterpret_cast<const iwx_rx_packet *>(raw);
  const int qid = pkt->hdr.qid;
  const int idx = pkt->hdr.idx;
  uint32_t code = IWX_WIDE_ID(pkt->hdr.group_id, pkt->hdr.cmd);
  const size_t plen = len - sizeof(pkt->len_n_flags) - sizeof(pkt->hdr);
  const uint8_t *payload = pkt->data;

  /* Narrow commands were sent in the long group; answers may come back in
   * either. */
  if (iwx_cmd_groupid(code) == IWX_LONG_GROUP && qid == IWX_DQA_CMD_QUEUE &&
      idx < int(_cmd_slots.size()) && _cmd_slots[idx].narrow)
    code = iwx_cmd_opcode(code);
  /* So may a notification with a legacy ID: Linux's notification wait
   * takes WIDE_ID(LONG_GROUP, x) for x (DEF_ID). Missing that loses
   * INIT_COMPLETE on AX210-family firmware (issue #2). */
  if ((qid & 0x80) && iwx_cmd_groupid(code) == IWX_LONG_GROUP) code = iwx_cmd_opcode(code);
  if (_trace_packets > 0) {
    _trace_packets--;
    _log("init: packet " + hex(IWX_WIDE_ID(pkt->hdr.group_id, pkt->hdr.cmd)) + " qid " + hex(uint32_t(qid)) +
         " idx " + std::to_string(idx) + " len " + std::to_string(plen));
  }

  switch (code) {
  case IWX_REPLY_RX_MPDU_CMD:
    handle_mpdu(payload, plen, (le32toh(pkt->len_n_flags) & IWX_FH_RSCSR_RADA_EN) != 0);
    break;
  /* Block Ack reordering (Bz only): the firmware lets out frames up to an NSSN
   * after a timeout of its own or a BAR (iwl_mvm_rx_frame_release). */
  case IWX_FRAME_RELEASE:
    if (bz() && plen >= sizeof(iwx_frame_release)) {
      const auto *r = reinterpret_cast<const iwx_frame_release *>(payload);
      _reorder.release(r->baid, static_cast<uint16_t>(le16toh(r->nssn) & 0xfff));
    }
    break;
  case IWX_BAR_FRAME_RELEASE:
    if (bz() && plen >= sizeof(iwx_bar_frame_release)) {
      const auto *r = reinterpret_cast<const iwx_bar_frame_release *>(payload);
      const uint32_t info = le32toh(r->ba_info);
      _reorder.release((info & IWX_BAR_FRAME_RELEASE_BAID_MASK) >> 24,
                       static_cast<uint16_t>(info & IWX_BAR_FRAME_RELEASE_NSSN_MASK));
    }
    break;
  case IWX_TX_CMD:
    handle_tx_resp(payload, plen, static_cast<unsigned>(idx));
    break;
  case IWX_ALIVE: {
    if (plen == sizeof(iwx_alive_resp_v4)) {
      const auto *r = reinterpret_cast<const iwx_alive_resp_v4 *>(payload);
      _alive_ok = le16toh(r->status) == IWX_ALIVE_STATUS_OK;
      _lmac_error_table = le32toh(r->lmac_data[0].dbg_ptrs.error_event_table_ptr);
      _lmac2_error_table = le32toh(r->lmac_data[1].dbg_ptrs.error_event_table_ptr);
      _umac_error_table = le32toh(r->umac_data.dbg_ptrs.error_info_addr);
    } else if (plen >= sizeof(iwx_alive_resp_v5)) {
      const auto *r = reinterpret_cast<const iwx_alive_resp_v5 *>(payload);
      _alive_ok = le16toh(r->status) == IWX_ALIVE_STATUS_OK;
      _lmac_error_table = le32toh(r->lmac_data[0].dbg_ptrs.error_event_table_ptr);
      _lmac2_error_table = le32toh(r->lmac_data[1].dbg_ptrs.error_event_table_ptr);
      _umac_error_table = le32toh(r->umac_data.dbg_ptrs.error_info_addr);
      for (int i = 0; i < 3; i++) _sku_id[i] = le32toh(r->sku_id.data[i]);
    }
    _alive_seen = true;
    break;
  }
  case IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP, IWX_PNVM_INIT_COMPLETE_NTFY):
    _pnvm_done = true;
    break;
  case IWX_INIT_COMPLETE_NOTIF:
    _init_complete = true;
    break;
  case IWX_SCAN_COMPLETE_UMAC:
  case IWX_WIDE_ID(IWX_LONG_GROUP, IWX_SCAN_COMPLETE_UMAC):
    if (_scan_running) {
      const uint8_t status = plen >= sizeof(iwx_umac_scan_complete)
          ? reinterpret_cast<const iwx_umac_scan_complete *>(payload)->status : 0;
      _scan_succeeded = status == IWX_SCAN_OFFLOAD_COMPLETED;
      _scan_running = false;
      _log("scan: firmware scan done, status " + std::to_string(status));
    }
    break;
  case IWX_MCC_CHUB_UPDATE_CMD:
  case IWX_WIDE_ID(IWX_LONG_GROUP, IWX_MCC_CHUB_UPDATE_CMD):
    if (plen >= sizeof(iwx_mcc_chub_notif)) {
      const auto *n = reinterpret_cast<const iwx_mcc_chub_notif *>(payload);
      _notice_mcc = le16toh(n->mcc);
      _notice_source = n->source_id;
      _notice_pending = true; /* answered from the main loop: no commands from here */
    }
    break;
  case IWX_WIDE_ID(IWX_MAC_CONF_GROUP, IWX_SESSION_PROTECTION_NOTIF):
    if (plen >= sizeof(iwx_session_prot_notif)) {
      const auto *n = reinterpret_cast<const iwx_session_prot_notif *>(payload);
      const bool start = le32toh(n->start) != 0, ok = le32toh(n->status) == 1;
      const uint32_t id = le32toh(n->mac_id);
      if (id == 0) _session_on = start && ok;
      else if (id == 1) _sta_session_on = start && ok;
      if (_session_logged++ < 12)
        _log(std::string("session protection ") + (start ? "started" : "ended") +
             (ok ? "" : " (not scheduled)") + " for id " + std::to_string(id));
    }
    break;
  case IWX_BA_NOTIF:
    handle_ba_notif(payload, plen);
    break;
  case IWX_REPLY_ERROR: {
    if (plen >= sizeof(iwx_error_resp)) {
      const auto *r = reinterpret_cast<const iwx_error_resp *>(payload);
      _log("firmware error " + hex(le32toh(r->error_type)) + " for command " +
           hex(r->cmd_id));
    }
    break;
  }
  default:
    /* #39/#43: a notification nothing here handles, once per ID, so a
     * Block Ack notification arriving under another group shows up. */
    if ((qid & 0x80) && _unhandled_notifs.size() < 32) {
      const uint32_t wide = IWX_WIDE_ID(pkt->hdr.group_id, pkt->hdr.cmd);
      if (std::find(_unhandled_notifs.begin(), _unhandled_notifs.end(), wide) == _unhandled_notifs.end()) {
        _unhandled_notifs.push_back(wide);
        _log("notification " + hex(wide) + " (" + std::to_string(plen) + " bytes) not handled");
      }
    }
    break;
  }

  /* A command's answer completes its slot; any answer of the right size is
   * kept for a caller that wants one. */
  if (!(qid & 0x80) && qid == IWX_DQA_CMD_QUEUE && idx < int(_cmd_slots.size())) {
    CmdSlot &slot = _cmd_slots[idx];
    if (slot.pending) {
      if (slot.want_resp && !(pkt->hdr.group_id & IWX_CMD_FAILED_MSK) && len <= IWX_CMD_RESP_MAX)
        slot.resp.assign(raw, raw + len);
      slot.pending = false;
      if (_cmdq.queued) _cmdq.queued--;
    }
  }
}

void IwxDevice::handle_mpdu(const uint8_t *payload, size_t len, bool rada) {
  /* The AX210 family's descriptor is the whole union (v3); the 22000's ends after v1. */
  const size_t desc_size = gen3() ? sizeof(iwx_rx_mpdu_desc) : IWX_RX_DESC_SIZE_V1;
  if (len < desc_size) return;
  const auto *desc = reinterpret_cast<const iwx_rx_mpdu_desc *>(payload);
  if (!(le16toh(desc->status) & IWX_RX_MPDU_RES_STATUS_CRC_OK) ||
      !(le16toh(desc->status) & IWX_RX_MPDU_RES_STATUS_OVERRUN_OK))
    return;
  size_t mpdu_len = le16toh(desc->mpdu_len);
  if (mpdu_len < 10 || mpdu_len > len - desc_size) return;
  const uint8_t *frame = payload + desc_size;

  /* The firmware pads two bytes after the header of some frames. */
  std::vector<uint8_t> &unpadded = _unpadded;
  if (desc->mac_flags2 & IWX_RX_MPDU_MFLG2_PAD) {
    const size_t h = hdrlen_80211(frame, mpdu_len);
    if (!h || h + 2 > mpdu_len) return;
    unpadded.assign(frame, frame + h);
    unpadded.insert(unpadded.end(), frame + h + 2, frame + mpdu_len);
    frame = unpadded.data();
    mpdu_len -= 2;
  }
  /* A trailing FCS some firmware appends is removed once, in radio_main
   * (awdl::strip_valid_fcs checks it against the frame's CRC). */
  /* Energy is reported as a positive number of dB below zero; 0 = none. */
  const uint8_t raw_a = gen3() ? desc->v3.energy_a : desc->v1.energy_a;
  const uint8_t raw_b = gen3() ? desc->v3.energy_b : desc->v1.energy_b;
  const int ea = raw_a ? -int(raw_a) : -256;
  const int eb = raw_b ? -int(raw_b) : -256;
  const int energy = std::max(ea, eb);
  const uint8_t channel = gen3() ? desc->v3.channel : desc->v1.channel;
  const uint32_t rnf = le32toh(gen3() ? desc->v3.rate_n_flags : desc->v1.rate_n_flags);
  RxFrame f{frame, mpdu_len, static_cast<int8_t>(energy < -127 ? -127 : energy), channel, rnf,
            le16toh(desc->status), rada};
  if (!bz()) {
    /* Non-Bz cards keep the direct v2.2.13 RX fast path without host reorder. */
    emit_rx(f);
    return;
  }
  const uint32_t reorder_data = le32toh(desc->reorder_data);
  const bool amsdu = (desc->mac_flags2 & IWX_RX_MPDU_MFLG2_AMSDU) != 0;
  const bool last = (desc->amsdu_info & IWX_RX_MPDU_AMSDU_LAST_SUBFRAME) != 0;
  if (_cmd_depth) {
    if (_deferred_rx.size() < 1024)
      _deferred_rx.push_back(
          {std::vector<uint8_t>(frame, frame + mpdu_len), f, true, reorder_data, amsdu, last});
    return;
  }
  rx_in(f, reorder_data, amsdu, last);
}

void IwxDevice::rx_in(const RxFrame &f, uint32_t reorder_data, bool amsdu, bool last_subframe) {
  if (!_reorder.frame(f, reorder_data, amsdu, last_subframe, now_us())) emit_rx(f);
}

void IwxDevice::emit_rx(const RxFrame &f) {
  if (_cmd_depth) {
    if (_deferred_rx.size() < 1024)
      _deferred_rx.push_back({std::vector<uint8_t>(f.data, f.data + f.len), f});
    return;
  }
  if (_on_rx) _on_rx(f);
}

void IwxDevice::handle_tx_resp(const uint8_t *payload, size_t len, unsigned slot) {
  if (len < sizeof(iwx_tx_resp)) return;
  const auto *r = reinterpret_cast<const iwx_tx_resp *>(payload);
  if (r->frame_count < 1 ||
      len < sizeof(*r) + size_t(r->frame_count - 1) * sizeof(r->status) + sizeof(uint32_t))
    return;
  const unsigned queue = le16toh(r->tx_queue);
  if (_agg_queue_enabled && queue == _aggq.qid) {
    /* A-MPDU TX responses (frame_count > 1) are reclaimed by handle_ba_notif
     * (IWX_BA_NOTIF), never here, matching Linux iwlwifi and ForbiddenItlwm.
     * Single-frame completions (frame_count == 1) reclaim the ring directly. */
    if (r->frame_count > 1) {
      _agg_stats.agg_resps++;
      return;
    }
    _agg_stats.singles++;
    const uint16_t st = static_cast<uint16_t>(le16toh(r->status.status) & IWX_TX_STATUS_MSK);
    _agg_stats.last_single_rate = le32toh(r->initial_rate);
    if (st == IWX_TX_STATUS_SUCCESS || st == IWX_TX_STATUS_DIRECT_DONE) {
      _agg_stats.singles_ok++;
    } else {
      _agg_stats.singles_fail++;
      _agg_stats.last_fail_status = st;
    }
    uint32_t ssn;
    std::memcpy(&ssn, reinterpret_cast<const uint8_t *>(&r->status) + r->frame_count * sizeof(r->status),
                sizeof(ssn));
    const unsigned idx = (le32toh(ssn) & 0xfff) & (_aggq.count - 1);
    const unsigned freed = (idx + _aggq.count - _aggq.tail) % _aggq.count;
    if (freed <= _aggq.queued) {
      _aggq.tail = idx;
      _aggq.queued -= freed;
    }
    return;
  }
  if (_sta_agg_queue_enabled && queue == _sta_aggq.qid) {
    /* Station TX A-MPDU responses (frame_count > 1) are reclaimed by handle_ba_notif. */
    if (r->frame_count > 1) return;
    const unsigned status = le16toh(r->status.status) & IWX_TX_STATUS_MSK;
    if (status != IWX_TX_STATUS_SUCCESS && status != IWX_TX_STATUS_DIRECT_DONE) _sta_tx_failed++;
    else _sta_tx_acked++;
    uint32_t ssn;
    std::memcpy(&ssn, reinterpret_cast<const uint8_t *>(&r->status) + r->frame_count * sizeof(r->status),
                sizeof(ssn));
    const unsigned idx = (le32toh(ssn) & 0xfff) & (_sta_aggq.count - 1);
    const unsigned freed = (idx + _sta_aggq.count - _sta_aggq.tail) % _sta_aggq.count;
    if (freed <= _sta_aggq.queued) {
      _sta_aggq.tail = idx;
      _sta_aggq.queued -= freed;
    }
    return;
  }
  if (_sta_queue_enabled && queue == _staq.qid) {
    /* The station's frames: the firmware retries them itself; only the ring
     * is reclaimed. Failures are logged, they mean the AP never acknowledged. */
    if (r->frame_count == 1) {
      const unsigned status = le16toh(r->status.status) & IWX_TX_STATUS_MSK;
      if (status != IWX_TX_STATUS_SUCCESS && status != IWX_TX_STATUS_DIRECT_DONE) {
        _sta_tx_failed++;
        if (_sta_tx_fail_logged++ < 20) {
          char b[128];
          plat_snprintf(b, sizeof(b), "station: frame not acknowledged, status 0x%04x, retries %u, rate 0x%08x",
                        le16toh(r->status.status), r->failure_frame, le32toh(r->initial_rate));
          _log(b);
        }
      } else {
        _sta_tx_acked++;
        if ((le16toh(r->frame_ctrl) & 0x000c) == 0x0008)
          _sta_tx_last_rate = le32toh(r->initial_rate);
      }
    }
    uint32_t ssn;
    std::memcpy(&ssn, reinterpret_cast<const uint8_t *>(&r->status) + r->frame_count * sizeof(r->status),
                sizeof(ssn));
    const unsigned idx = (le32toh(ssn) & 0xfff) & (_staq.count - 1);
    const unsigned freed = (idx + _staq.count - _staq.tail) % _staq.count;
    if (freed <= _staq.queued) {
      _staq.tail = idx;
      _staq.queued -= freed;
    }
    return;
  }
  if (queue != _dataq.qid || !_queue_enabled) return;
  if (r->frame_count == 1) {
    const unsigned status = le16toh(r->status.status) & IWX_TX_STATUS_MSK;
    if (status == IWX_TX_STATUS_SUCCESS || status == IWX_TX_STATUS_DIRECT_DONE)
      _tx_stats.acked++;
    else {
      _tx_stats.failed++;
      if (_opt.verbose && _tx_fail_logged++ < 40) {
        char b[160];
        plat_snprintf(b, sizeof(b),
                      "tx failed: status 0x%04x, retries %u, rts fails %u, bt kills %u, rate 0x%08x, "
                      "frame_ctrl 0x%04x, bytes %u",
                      le16toh(r->status.status), r->failure_frame, r->failure_rts, r->bt_kill_count,
                      le32toh(r->initial_rate), le16toh(r->frame_ctrl), le16toh(r->byte_cnt));
        _log(b);
      }
    }
    _tx_stats.retries += r->failure_frame;
    if (_on_tx)
      _on_tx(slot, status == IWX_TX_STATUS_SUCCESS || status == IWX_TX_STATUS_DIRECT_DONE);
  }
  uint32_t ssn;
  std::memcpy(&ssn, reinterpret_cast<const uint8_t *>(&r->status) + r->frame_count * sizeof(r->status),
              sizeof(ssn));
  const unsigned idx = (le32toh(ssn) & 0xfff) & (_dataq.count - 1);
  /* Reclaim everything from the tail up to the firmware's next index. */
  const unsigned n = _dataq.count;
  const unsigned freed = (idx + n - _dataq.tail) % n;
  if (freed <= _dataq.queued) {
    _dataq.tail = idx;
    _dataq.queued -= freed;
    _tx_stats.completed += freed;
  }
}

bool IwxDevice::read_mem(uint32_t addr, uint32_t *out, unsigned dwords) {
  if (!nic_lock()) return false;
  std::vector<RegOp> ops;
  ops.push_back({IWX_HBUS_TARG_MEM_RADDR, addr, 1});
  for (unsigned i = 0; i < dwords; i++) ops.push_back({IWX_HBUS_TARG_MEM_RDAT, 0, 0});
  const bool ok = _proxy.batch(ops);
  nic_unlock();
  if (!ok) return false;
  for (unsigned i = 0; i < dwords; i++) out[i] = ops[i + 1].value;
  return true;
}

namespace {
const char *error_name(uint32_t id) {
  static const struct { uint32_t num; const char *name; } names[] = {
      {0x34, "NMI_INTERRUPT_WDG"}, {0x35, "SYSASSERT"}, {0x37, "UCODE_VERSION_MISMATCH"},
      {0x38, "BAD_COMMAND"}, {0x39, "BAD_COMMAND"}, {0x3C, "NMI_INTERRUPT_DATA_ACTION_PT"},
      {0x3D, "FATAL_ERROR"}, {0x46, "NMI_TRM_HW_ERR"}, {0x4C, "NMI_INTERRUPT_TRM"},
      {0x54, "NMI_INTERRUPT_BREAK_POINT"}, {0x5C, "NMI_INTERRUPT_WDG_RXF_FULL"},
      {0x64, "NMI_INTERRUPT_WDG_NO_RBD_RXF_FULL"}, {0x66, "NMI_INTERRUPT_HOST"},
      {0x70, "NMI_INTERRUPT_LMAC_FATAL"}, {0x71, "NMI_INTERRUPT_UMAC_FATAL"},
      {0x73, "NMI_INTERRUPT_OTHER_LMAC_FATAL"}, {0x7C, "NMI_INTERRUPT_ACTION_PT"},
      {0x84, "NMI_INTERRUPT_UNKNOWN"}, {0x86, "NMI_INTERRUPT_INST_ACTION_PT"}};
  for (const auto &n : names)
    if (n.num == (id & ~0xf0000000u)) return n.name;
  return "ADVANCED_SYSASSERT";
}
} // namespace

/* 0: healthy, 1: firmware/DMA error, 2: the card stopped answering. On Bz the
 * firmware reports its own errors in the MSI-X hardware causes (Linux
 * iwl_pcie_irq_msix_handler: SW_ERR_BZ, TOP_FATAL_ERR, HW_ERR); the old SW_ERR
 * position (CSR_INT bit 25) rises by itself while the firmware keeps answering
 * and leaves no error record, so it is only noted and cleared. */
int IwxDevice::fw_error_state(std::string &what) {
  const uint32_t cause = rd(IWX_CSR_INT);
  if (cause == 0xffffffffu) {
    what = "CSR_INT reads all ones";
    return 2;
  }
  if (!bz()) {
    if (!(cause & (IWX_CSR_INT_BIT_SW_ERR | IWX_CSR_INT_BIT_HW_ERR))) return 0;
    what = "CSR_INT " + hex(cause) + ", FH_INT " + hex(rd(IWX_CSR_FH_INT_STATUS));
    return 1;
  }
  const uint32_t msix = rd(IWX_CSR_MSIX_HW_INT_CAUSES_AD);
  if (msix == 0xffffffffu) {
    what = "MSIX_HW causes read all ones";
    return 2;
  }
  if (cause & IWX_CSR_INT_BIT_SW_ERR) {
    if (!_bz_bit25_logged) {
      _log("Bz: CSR_INT bit 25 set (" + hex(cause) + ", FH_INT " +
           hex(rd(IWX_CSR_FH_INT_STATUS)) + ", MSIX_HW " + hex(msix) +
           "); not an error on Bz, clearing it");
      _bz_bit25_logged = true;
    }
    wr(IWX_CSR_INT, IWX_CSR_INT_BIT_SW_ERR);
  }
  constexpr uint32_t kTopFatal = 1u << 3; /* MSIX_HW_INT_CAUSES_REG_TOP_FATAL_ERR */
  const uint32_t bad = IWX_MSIX_HW_INT_CAUSES_REG_SW_ERR_BZ | IWX_MSIX_HW_INT_CAUSES_REG_HW_ERR | kTopFatal;
  if (!(msix & bad) && !(cause & IWX_CSR_INT_BIT_HW_ERR)) return 0;
  what = "MSIX_HW " + hex(msix) + " (" +
         (msix & IWX_MSIX_HW_INT_CAUSES_REG_SW_ERR_BZ ? "SW_ERR_BZ "
          : msix & kTopFatal                           ? "TOP_FATAL "
                                                       : "HW_ERR ") +
         "), CSR_INT " + hex(cause) + ", FH_INT " + hex(rd(IWX_CSR_FH_INT_STATUS));
  return 1;
}

void IwxDevice::check_health() {
  if (!_open || _fw_error) return;
  std::string what;
  const int st = fw_error_state(what);
  if (st == 0) return;
  if (st == 2) {
    _log("the card stopped answering (" + what + ")");
    _fw_error = true;
    return;
  }
  _log("firmware/DMA error: " + what +
       (bz() ? ", MSIX_FH_CAUSES " + hex(rd(IWX_CSR_MSIX_FH_INT_CAUSES_AD)) + ", GP_CNTRL " +
                   hex(rd(IWX_CSR_GP_CNTRL))
             : "") +
       ", FH_INT_STATUS " + hex(rd(IWX_CSR_FH_INT_STATUS)));
  _fw_error = true;
  dump_error_tables();
}

void IwxDevice::dump_error_tables() {
  if (!_proxy.is_open() || _proxy.failed()) return;
  const auto *stat = reinterpret_cast<volatile iwx_rb_status *>(_rx_stat.va);
  if (stat)
    _log("rx ring: card closed " + std::to_string(le16toh(stat->closed_rb_num) & 0xfff) +
         ", host at " + std::to_string(_rx_cur) + "; tx queue " + std::to_string(_dataq.qid) +
         " cur " + std::to_string(_dataq.cur) + " tail " + std::to_string(_dataq.tail) +
         " queued " + std::to_string(_dataq.queued));
  for (unsigned i = 0; i < 8; i++) {
    const TxHist &h = _tx_hist[(_tx_hist_at + i) % 8];
    if (!h.len) continue;
    char b[128];
    plat_snprintf(b, sizeof(b), "tx history: slot %u len %u hdr %u fc %02x %02x addr1[0] %02x rate 0x%08x",
                  h.slot, h.len, h.hdrlen, h.fc0, h.fc1, h.a1, h.rnf);
    _log(b);
  }
  const uint32_t min_tbl = bz() ? 0xD0000u : 0x400000u;
  auto dump_lmac = [&](const char *label, uint32_t addr) {
    if (addr < min_tbl) return;
    uint32_t t[38] = {};
    if (!read_mem(addr, t, 38)) {
      _log(std::string(label) + " error log unreadable (read_mem failed at " + hex(addr) + ")");
    } else if (t[0]) {
      _log(std::string(label) + " error " + hex(t[1]) + " " + error_name(t[1]) + ": blink2 " +
           hex(t[4]) + " ilink1 " + hex(t[5]) + " ilink2 " + hex(t[6]) + " data1 " + hex(t[7]) +
           " data2 " + hex(t[8]) + " data3 " + hex(t[9]) + " hcmd " + hex(t[23]) + " last_cmd " +
           hex(t[29]));
    } else {
      _log(std::string(label) + " error log valid=0 (at " + hex(addr) + ", raw " + hex(t[1]) +
           " " + hex(t[2]) + " " + hex(t[3]) + ")");
    }
  };
  dump_lmac("LMAC", _lmac_error_table);
  if (_lmac2_error_table >= min_tbl && _lmac2_error_table != _lmac_error_table)
    dump_lmac("LMAC1", _lmac2_error_table);
  if (_umac_error_table >= min_tbl) {
    uint32_t t[15] = {};
    if (!read_mem(_umac_error_table, t, 15)) {
      _log("UMAC error log unreadable (read_mem failed at " + hex(_umac_error_table) + ")");
    } else if (t[0]) {
      _log("UMAC error " + hex(t[1]) + " " + error_name(t[1]) + ": blink1 " + hex(t[2]) +
           " blink2 " + hex(t[3]) + " ilink1 " + hex(t[4]) + " ilink2 " + hex(t[5]) + " data1 " +
           hex(t[6]) + " data2 " + hex(t[7]) + " data3 " + hex(t[8]) + " cmd_header " + hex(t[13]));
    } else {
      _log("UMAC error log valid=0 (at " + hex(_umac_error_table) + ", raw " + hex(t[1]) + " " +
           hex(t[2]) + " " + hex(t[3]) + ")");
    }
  }
  if (nic_lock()) {
    _log("UMAC PC " + hex(read_umac_prph(IWX_UREG_UMAC_CURRENT_PC)) + ", LMAC PC " +
         hex(read_umac_prph(IWX_UREG_LMAC1_CURRENT_PC)));
    nic_unlock();
  }
}
