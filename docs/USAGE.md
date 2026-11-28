# Using nekodrop

English · [简体中文](USAGE.zh-CN.md) · [Back to README](../README.en.md)

## What it does

- **Receive from iPhone, iPad and Mac.** nekodrop shows up in the AirDrop sheet like any Apple device. Files land in a folder you choose (default `Downloads\nekodrop`), and you can accept or decline each transfer from a notification.
- **Send to Apple devices.** Right-click a file in Explorer and choose **Send with nekodrop**. This is on the Windows 11 first-level menu, the classic menu and *Send to*. Then pick a nearby device.
- **Stay online.** In *Wi-Fi and AirDrop together* mode nekodrop drives the card itself. It joins your Wi-Fi on one virtual interface and runs AWDL (Apple Wireless Direct Link) on the other. Windows sees your internet connection as a wired adapter called **nekodrop**.
- **Manage Wi-Fi.** Scanning, connecting, hidden networks and saved networks with auto-connect all work. Saved Windows Wi-Fi profiles are imported, and captive portals (hotel, airport and campus sign-in pages) are detected and opened in your browser.
- **Recover by itself.** A service watches the radio, the AWDL daemon and the AirDrop engine and restarts whatever stops. It starts with Windows.
- **A small panel by the tray**, in Chinese and English, light and dark. It slides up from the tray icon or your shortcut (Win+Shift+D unless you record your own) and goes away when you click elsewhere. Nearby devices sit on a radar (or a list), with motion on Windows' own Fluent curves: a blue wave, a steady ring, and live transfer speed (`MB/s` / `KB/s`, toggleable in Settings) while sending or receiving, then green with a check mark.
- **Who can send to you, from the AirDrop mark.** Click the mark at the top left: **Receiving Off**, **Contacts Only** or **Everyone**, as on an iPhone. Supports configurable auto-accept (**Off**, **Contacts**, or **Everyone**), media format preference (**Compatible** JPEG/H.264 or **Original** HEIC/HEVC with optional sibling JPG conversion), toast image previews with one-click copy to clipboard, and automatic post-transfer BLE re-pulse so iPhones re-appear cleanly after large sends.
- **Contacts Only (experimental).** Sign in to your Apple ID on the PC (with automatic `-27952` / 2FA 503 retries and redacted sign-in log export), or import a Mac's AirDrop certificate, and the PC shows up on your own iPhone like one of your Macs and works with iPhones set to Contacts Only. See [Contacts Only](#contacts-only-apple-id-experimental).
- **Windows Share sheet.** nekodrop is in Explorer's and other apps' Share panel.

## Screenshots

A small panel that slides up beside the tray (click the tray icon, or press the shortcut) and goes away when you click elsewhere: nearby iPhones and Macs on a radar (or as a list), the AirDrop mark that switches who can send to you, the status and quick controls in the title bar, settings in five tabs, one accent colour you can switch (or colourful), light and dark. While files come in, a progress ring turns around "This PC", which turns green with a check mark when they are saved. The global shortcut is Win+Shift+D unless you record your own in Settings.

<table>
  <tr>
    <td align="center"><img src="screenshots/main-radar.png" width="280" alt="Main window: radar"><br><sub>Radar: a request to answer, sending, sent, declined</sub></td>
    <td align="center"><img src="screenshots/main-list-dark.png" width="280" alt="Main window: list, dark"><br><sub>List, dark</sub></td>
    <td align="center"><img src="screenshots/send.png" width="230" alt="Send window"><br><sub>Right-click a file → Send with nekodrop</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="screenshots/settings.png" width="280" alt="Settings"><br><sub>Settings: accent colour, your own shortcut</sub></td>
    <td align="center"><img src="screenshots/settings-appleid.png" width="280" alt="Settings: Apple ID"><br><sub>Apple ID and Contacts Only (experimental)</sub></td>
    <td align="center"><img src="screenshots/wifi.png" width="250" alt="Wi-Fi window"><br><sub>nekodrop's own Wi-Fi</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="screenshots/tray.png" width="260" alt="Tray menu"><br><sub>Tray menu</sub></td>
    <td align="center" colspan="2"><img src="screenshots/motion-send.png" width="560" alt="Sending, frame by frame"><br><sub>Sending, frame by frame: the ring fills, turns green, a check is drawn</sub></td>
  </tr>
</table>

## Install

1. Download `nekodrop-<version>-setup.exe` from [Releases](../../../releases) and run it.
2. The installer:
   - copies the files to `C:\Program Files\nekodrop`;
   - creates a code-signing certificate **on your PC**, signs the small register-proxy driver with it, trusts it, and **deletes the private key**, so nothing else can ever be signed with it;
   - creates the TAP adapter **nekodrop** (TAP-Windows, signed by OpenVPN);
   - adds two inbound firewall rules, link-local addresses only (`TCP 8771-8779, 18771-18779, 28771-28779` to avoid Hyper-V excluded port blocks, `UDP 5353`);
   - installs and starts the **NekoDropService** service.
3. The tray icon (the purple cat) appears. Open it and turn on **Allow receiving**.

On first start nekodrop takes the Wi-Fi card away from the Intel driver and reconnects you to the network Windows was using. The Windows Wi-Fi icon disappears; use nekodrop's **Wi-Fi…** window instead.

### Updates

nekodrop checks GitHub for a newer release a minute after it starts and then every 6 hours, resolving your current Windows system proxy dynamically and falling back to browser-style `github.com` web endpoints if `api.github.com` is blocked or rate-limited ([#62](../../../issues/62)). When it finds one, a notification offers **Update**. nekodrop downloads the installer with live download speed (`KB/s` / `MB/s`, [#64](../../../pull/64)), checks it against the SHA-256 GitHub publishes for it, and runs it (Windows asks for administrator rights). It never installs anything on its own. *Settings → About & updates* lets you toggle update checks, choose whether preview releases count, delete the downloaded installer after a successful install ([#44](../../../pull/44)), pick whether to open the **Main panel** or start in the **Tray** after an update ([#71](../../../pull/71)), or click **Check now**. There are two channels: **Stable** releases (currently `v2.2.8`), and **Preview** releases built as fixes land. Previews are included by default; untick *Include preview releases* to get only stable ones.

### Uninstall

Use *Settings → Apps → nekodrop → Uninstall*, or run `uninstall.ps1` in the install folder. Uninstalling gives the card back to the Intel driver, then removes the service, the driver package, the certificate trust and the firewall rules.

## Usage

| I want to… | Do this |
|---|---|
| Receive | Keep receiving on (the AirDrop mark at the top left is coloured, not grey). On the iPhone, tap Share → AirDrop and pick your PC's name |
| Choose who can send to me | Click the AirDrop mark: **Receiving Off**, **Contacts Only** (experimental, Settings → Apple ID) or **Everyone** |
| Send | Right-click files → **Send with nekodrop** → pick a device. Or select files in Explorer and press the shortcut (Win+Shift+D by default) |
| Change Wi-Fi | Tray → **Wi-Fi…**: pick a network, enter the password, Connect |
| Hidden network | **Wi-Fi…** → *Hidden network*: type the SSID and password |
| Sign in to hotel Wi-Fi | Connect; the sign-in page opens in your browser by itself |
| Give the card back to Windows | Tray → *Wi-Fi and AirDrop* → **Take the card on demand** or **AirDrop off** |
| Change name, folder or language | Tray → **Settings…** |
| Change the shortcut | Settings → General: click the shortcut, press the new keys |

### Modes

| Mode | Internet | AirDrop |
|---|---|---|
| **Wi-Fi and AirDrop together** (default) | Through nekodrop's own Wi-Fi connection (adapter "nekodrop") | Always available |
| **Take the card on demand** | The normal Intel driver | nekodrop takes the card when an iPhone nearby opens AirDrop or you send. Internet drops during the transfer |
| **AirDrop off** | The normal Intel driver | Off |

### Wi-Fi security

| Network | Together mode |
|---|---|
| Open | ✅ |
| WPA/WPA2-Personal | ✅ |
| WPA2/WPA3 transition | ✅ (joins with WPA2) |
| WPA2 with protected management frames required, WPA2-PSK-SHA256 | 🧪 0.1.12, untested on hardware |
| Captive portal | ✅ detected, sign-in page opened |
| Hidden SSID | ✅ |
| WPA3-Personal only (SAE) | 🧪 0.1.12, untested on hardware (hash-to-element-only networks, mostly 6 GHz, not yet) |
| Enhanced Open (OWE) | 🧪 0.1.12, untested on hardware |
| WPA2/WPA3-Enterprise (802.1X, PEAP…) | ❌ not yet. nekodrop switches to *on demand* |
| WEP, WPA/TKIP | ❌ (insecure; not planned) |

### Contacts Only (Apple ID, experimental)

Settings → **Apple ID**: turn on the experimental switch at the top, then choose who can send to this PC, **Everyone** or **Contacts Only**. Contacts Only needs an AirDrop identity like a Mac's, which you get one of two ways:

- **Sign in with Apple ID** on this PC. nekodrop asks Apple for an AirDrop certificate the way a Mac does (WirDrop's `appleid`, MIT), keeps it renewed and reads your iCloud contacts. The first sign-in downloads a component of iCloud for Windows from Apple (about 160 MB; iCloud isn't installed). The password is used once and never stored.
- **Import from a Mac** signed in to your Apple ID: export `certificate.pem`, `key.pem` and `validation_record.cms` with [airdrop-keychain-extractor](https://github.com/seemoo-lab/airdrop-keychain-extractor) (it needs SIP partly off), pick their folder, then import your contacts as a `.vcf`.

With an identity, your own iPhone lists this PC like one of your Macs, and iPhones set to Contacts Only see it when your Apple ID's e-mail or phone number is in their contacts. The receiver's Contacts Only filter reads the sender's record without checking Apple's signature, so it is a filter, not proof. Confirmed working by the maintainer on 2.1.0.

### Experimental features

In *Settings → Experimental*, all off by default; turning one off restores the old behaviour exactly. (Graduated from here and always on: aggregated sending, A-MPDU, in 0.1.35; balanced Bluetooth coexistence on Intel cards in 0.1.37, which lets the Bluetooth beacon through so the PC finds iPhones and Macs in seconds, thanks to [@XXD051030](https://github.com/XXD051030), [#26](../../../pull/26).)

- **Android Share (MTA v2 over LAN, `v2.3.3`+)**: send and receive files with Mutual Transmission Alliance phones (OnePlus / OPPO / Xiaomi / vivo / Honor / realme, etc.) on the same Wi-Fi, found over Bluetooth. In Concurrent mode Android → PC is fast; PC → Android drops to about 2 MB/s on routers that ignore `ADDBA`, so switch to On Demand or use Ethernet / a second card for big uploads.
- **LocalSend (`v2.3.5`+)**: send to and receive from devices running [LocalSend](https://localsend.org/) (Android, iPhone, Mac, Windows, Linux) on the same Wi-Fi or local network. They show up on the radar and in Explorer's **Send with nekodrop**. Uses TCP / UDP port 53317, so the official LocalSend app cannot run on the same PC. Receivers that ask for a PIN are not supported yet.
- **Relay (rules from `v2.3.5`)**: add rules in *Settings → Sharing & relay*; each says whose files go on to which device, in any mix of iPhone, Android (Android Share) and LocalSend devices, and a pair can be added both ways at once. A diagram above the rules shows where each device's files go, with arrows into the same device in the same colour. Each source has one destination; only the sources in the rules are accepted and passed on automatically, and anyone else's files are received as usual. If the destination is not around, the files stay on the PC. AirDrop and Android Share devices are recognised by name (add them again after renaming a phone), LocalSend devices by their certificate fingerprint. After upgrading from an older version, add rules before the relay works.
- **Bluetooth diagnostic scan**: one click lists nearby MTA advertisements with MAC addresses and other private fields redacted, for reporting phones that don't show up.
- **Experimental Wi-Fi 6 internet (`v2.3.4`+, Intel only, not the 9560)**: nekodrop's own Wi-Fi joins a Wi-Fi 6 router with Wi-Fi 6 (802.11ax); other routers are joined with Wi-Fi 5 as before. A conservative profile for now: single-user, up to two streams, 80 MHz, no 6 GHz (Wi-Fi 6E). Speed is still mostly limited by time sharing with AirDrop, so gains are modest; turn it off if the network won't connect or drops.
- **High-throughput mode (Wi-Fi 6 HE80 + MTU 1500)** (inspired by [`YiHoooong/owly`](https://github.com/YiHoooong/owly/tree/feat/windows-mtkradio)): advertises Apple AWDL Wi-Fi 6 (HE80) in 5 GHz 80 MHz and raises `awdl0` MTU from `1450` to `1500`. Combined with the lock-free 4+4 MiB SHM data plane, 8 KiB RX DMA, 8 MiB Wintun ring, fair-polled `ev_win.c`, and streaming receive, AX201 reaches **50+ MiB/s (~53 MB/s)** in both directions.
- **Time sharing when Wi-Fi and AirDrop use different channels**: tells the iPhone when the PC is away with the router. With nekodrop's Wi-Fi on another 5 GHz channel, one AX211 heard no AirDrop frame without it ([#35](../../../issues/35)); on one AX200 speeds went both up and down ([#4](../../../issues/4)).
- **Faster unlock of a passive 5 GHz channel**: scans sooner so cards that refuse the country code can transmit earlier ([#11](../../../issues/11)).
- **Standby BLE beacon (`NEKODROP_BLE_STANDBY=1`)**: optional environment variable contributed in [#69](../../../pull/69) (off by default) to keep the BLE AirDrop advertisement active while receiving on standby.

### A device does not appear in the list

The Apple device has to be listening for AirDrop before it can list anyone, and
its AWDL stack can get stuck: the phone keeps spinning and never shows the PC,
while the PC shows the phone and transfers *from* the phone still work.

On an iPhone 14 Pro (iOS 18.7.8, 2026-11-28) the only thing that fixed it was to
**restart the phone**. Nothing on the PC side changed: the MIF nekodrop sends is
the same one a Mac sends, and the very same build listed the PC on other iOS
versions.

Try these first, in this order:

1. restart the Apple device;
2. switch its Bluetooth off and on, then its Wi-Fi off and on;
3. set AirDrop to "Everyone for 10 minutes" and leave the share sheet open;
4. unlock the device and turn off any Focus mode.

Only then collect data from the PC; `docs/SUPPORT.md` says what helps.