// SPDX-License-Identifier: GPL-2.0-only
using System.Diagnostics;
using System.Text.Json.Nodes;

namespace NekoDrop.Service;

/// <summary>
/// Owns the card and the two daemons: the user-mode radio (nekodrop-radio for
/// Intel, nekodrop-radio-rtl for Realtek RTL8852CE) and awdld (winawdl's AWDL
/// daemon). Channel policy, after
/// ForbiddenItlwm 0.6.0: start on 6, where every country allows AWDL; when a
/// peer's channel sequence names 44 or 149 and its country is known, restart
/// there (the move waits until nothing is transferring); fall back to 6 if
/// the 5 GHz channel stays passive.
/// </summary>
public sealed class Supervisor : IDisposable
{
    public enum RadioState { Off, Starting, On, Error }

    readonly string _root = AppContext.BaseDirectory;
    readonly object _lock = new();
    readonly ServiceConfig _cfg = ServiceConfig.Load();
    /// <summary>The card being driven, for the settings ("Intel AX200"), "" when none was found.</summary>
    string _cardLabel = "";
    Process? _radio, _awdld;
    int _channel;
    /// <summary>The network last read from Windows; once the card is ours the WLAN service sees nothing.</summary>
    Wlan.Network? _net;
    string _wifi = "";
    /* Wi-Fi management (the list, connect, forget, auto-connect) */
    readonly KnownNetworks _known = KnownNetworks.Load();
    List<WifiSelection.AccessPoint> _scan = [], _scanPending = [];
    bool _scanBusy, _wantBootScan, _channelOpen;
    int _launchGen;
    DateTime _scanAt = DateTime.MinValue, _wifiDownSince = DateTime.UtcNow;
    bool _userChose, _userOff;
    string _staSsid = "", _wifiError = "";
    enum JoinOrigin { Inherited, Manual, Automatic }
    (string Ssid, string? Password, bool? Auto, JoinOrigin Origin, bool Retry)? _pendingConnect;
    readonly Dictionary<(string Ssid, string Bssid), DateTime> _failedAccessPoints = [];
    Wlan.Network? _runningStation;
    Wlan.Network? _pendingManualSuccess;
    JoinOrigin _joinOrigin, _runningJoinOrigin;
    bool _wifiJoinQueued, _stationRecorded, _runningStationConnected, _runningUserChose;
    int _stationVersion;
    int _pendingScans; // scans that missed the pending network so far
    int _mtkBackgroundScans; // repeated MT792x off-channel scans while Wi-Fi is down
    string? _mtkBackgroundScanSsid;
    readonly HashSet<string> _hiddenProbed = [];
    static readonly TimeSpan StaleScan = TimeSpan.FromSeconds(20);
    string _detail = "";
    bool _wanted;
    /// <summary>The give-back task restores Windows' profiles and may retry through PnP.</summary>
    Task? _wifiReconnect;
    /// <summary>Invalidates a give-back task when a new start/stop begins.</summary>
    int _wifiEpoch;
    /// <summary>Cancels a give-back task's DHCP/connect wait when the card changes hands.</summary>
    CancellationTokenSource _wifiCancel = new();
    bool _earlyFirmwareError, _holdFirmwareRetry;
    DateTime _lastUse = DateTime.UtcNow, _lastData = DateTime.UtcNow, _startedAt;
    long _rxToUs;
    int _restarts;
    /// <summary>When the next restart may run after the radio found no contiguous DMA memory (#30).</summary>
    DateTime _retryAt = DateTime.MinValue;
    /// <summary>After giving up on DMA memory, automatic wake-ups wait until then (#94).</summary>
    DateTime _dmaHoldUntil = DateTime.MinValue;
    CancellationTokenSource _cts = new();

    public RadioState State { get; private set; } = RadioState.Off;
    public event Action<JsonObject>? Changed;
    public event Action<string>? Log;

    public NetMode Mode => _cfg.Mode;

    /// <summary>
    /// What the card can do, read when the service starts and whenever the card
    /// is taken: the 2 s watchdog and the 15 s gateway probe ask it, and must
    /// not enumerate PCI devices each time. Before a card is seen (or with none)
    /// it is the Intel set, so those paths behave exactly as before the catalog.
    /// </summary>
    AdapterProfile _profile = CardCatalog.ActiveProfiles[0];
    AdapterCapabilities _caps = AdapterCapabilities.IntelDefault;

    /// <summary>
    /// The mode the card can run: the user's choice, except Concurrent on a
    /// card whose radio has no station, which runs on demand. For every Intel
    /// card this is _cfg.Mode. The snapshot keeps reporting the user's choice.
    /// </summary>
    NetMode EffectiveMode()
    {
        var mode = CardCatalog.EffectiveMode(_cfg.Mode, _caps);
        /* Concurrent mode cannot join this Wi-Fi (#97): Windows keeps the card, AirDrop runs on demand. */
        return mode == NetMode.Concurrent && _fallbackReason != "" ? NetMode.OnDemand : mode;
    }

    /// <summary>Why Concurrent mode runs on demand this session ("" when it does not): an unsupported network's reason.</summary>
    string _fallbackReason = "";
    static readonly TimeSpan UnsupportedMemory = TimeSpan.FromDays(7);

    public JsonObject Snapshot()
    {
        lock (_lock)
        {
            return new JsonObject
            {
                ["event"] = "state",
                ["radio"] = State.ToString().ToLowerInvariant(),
                ["mode"] = _cfg.Mode.ToString().ToLowerInvariant(),
                // What the card actually runs (#32): an RTL8852CE set to Concurrent runs
                // on demand, and the client's Bluetooth wake-up has to know that.
                ["effectiveMode"] = EffectiveMode().ToString().ToLowerInvariant(),
                ["channel"] = _channel,
                ["country"] = _cfg.Country,
                ["fastUnlock"] = _cfg.FastUnlock,
                ["coexPlan"] = _cfg.CoexPlan,
                ["highThroughput"] = _cfg.HighThroughput,
                ["dataPlaneAccel"] = _cfg.DataPlaneAccel,
                ["stationWifi6"] = _cfg.StationWifi6,
                ["keepWindowsMac"] = _cfg.KeepWindowsMac,
                ["quickReleaseOnDemand"] = _cfg.QuickReleaseOnDemand,
                ["idleSeconds"] = _cfg.IdleSeconds,
                ["card"] = _cardLabel,
                ["manualCard"] = _cfg.ManualCard,
                ["manualModel"] = _cfg.ManualModel,
                ["wifi"] = _wifi,
                ["detail"] = _detail,
                ["ssid"] = _staSsid,
                ["scanning"] = _scanBusy,
                ["wifiError"] = _wifiError,
                ["concurrentFallback"] = _fallbackReason,
                ["networks"] = new JsonArray(_scan.GroupBy(ap => ap.Ssid, StringComparer.Ordinal)
                    .Select(g => g.FirstOrDefault(ap => _runningStationConnected && _runningStation is { } active &&
                                      ap.Ssid == active.Ssid && ap.Bssid.Equals(active.Bssid, StringComparison.OrdinalIgnoreCase))
                                 ?? WifiSelection.ForName(g, _known.SelectionProfile(g.Key)) ?? g.MaxBy(ap => ap.Dbm)!)
                    .OrderByDescending(ap => ap.Dbm).Select(ap =>
                    {
                        var known = _known.Find(ap.Ssid);
                        return (JsonNode)new JsonObject
                        {
                            ["ssid"] = ap.Ssid, ["bssid"] = ap.Bssid, ["channel"] = ap.Channel,
                            ["dbm"] = ap.Dbm, ["security"] = ap.Security,
                            ["saved"] = known is { HasKey: true }, ["auto"] = known?.AutoConnect ?? false,
                            ["connected"] = ap.Ssid == _staSsid && _wifi == "connected",
                        };
                    }).ToArray()),
                ["known"] = new JsonArray(_known.All().Select(k =>
                    (JsonNode)new JsonObject { ["ssid"] = k.Ssid, ["auto"] = k.AutoConnect, ["hidden"] = k.Hidden }).ToArray()),
            };
        }
    }

    void Publish() => Changed?.Invoke(Snapshot());

    static readonly object LogFileLock = new();
    /// <summary>service.log moves to service.log.1 at this size (the radio adds ~3 lines every 5 s).</summary>
    const long LogFileLimit = 8 * 1024 * 1024;

    void Say(string line)
    {
        Log?.Invoke(line);
        try
        {
            ServiceConfig.EnsureSecureDirectory();
            var logPath = Path.Combine(ServiceConfig.Folder, "service.log");
            ServiceConfig.RejectReparsePoint(logPath);
            // Several threads log at once (radio, awdld, watchdog): one writer at a time.
            lock (LogFileLock)
            {
                if (File.Exists(logPath) && new FileInfo(logPath).Length > LogFileLimit)
                {
                    var old = logPath + ".1";
                    ServiceConfig.RejectReparsePoint(old);
                    File.Move(logPath, old, true);
                }
                File.AppendAllText(logPath,
                    $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {line}{Environment.NewLine}");
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    /// <summary>
    /// The first lines of every service start: which build, on which Windows,
    /// in which mode, with which card and driver, so a log says what it came
    /// from without asking the reporter.
    /// </summary>
    void SayHeader()
    {
        var asm = typeof(Supervisor).Assembly;
        var version = asm.GetCustomAttributes(typeof(System.Reflection.AssemblyInformationalVersionAttribute), false)
            .OfType<System.Reflection.AssemblyInformationalVersionAttribute>().FirstOrDefault()?.InformationalVersion
            ?? asm.GetName().Version?.ToString() ?? "?";
        string windows = "?";
        try
        {
            using var cv = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
            var build = cv?.GetValue("CurrentBuild") as string ?? "";
            var name = cv?.GetValue("ProductName") as string ?? "Windows";
            // ProductName still says "Windows 10" on Windows 11.
            if (int.TryParse(build, out var b) && b >= 22000) name = name.Replace("Windows 10", "Windows 11");
            windows = $"{name} {cv?.GetValue("DisplayVersion")} (build {build}.{cv?.GetValue("UBR")}, {cv?.GetValue("EditionID")})";
        }
        catch (Exception) { }
        Say($"===== nekodrop service {version}; {windows}; mode {_cfg.Mode}, country \"{_cfg.Country}\" =====");
        try
        {
            Card.Manual = (_cfg.ManualCard, _cfg.ManualModel);
            var card = Card.Find();
            if (card is null) Say("card: no supported Wi-Fi card present" +
                                  (_cfg.ManualCard != "" ? $" (chosen in settings: {_cfg.ManualCard} as {_cfg.ManualModel}, not found)" : ""));
            else
            {
                var p = card.Profile;
                _profile = p;
                _caps = p.Capabilities;
                _cardLabel = $"{p.VendorName} {p.FamilyName}";
                // Same line as before the catalog for Intel cards ("Intel INF on record").
                Say($"card: {card.InstanceId}; driver service {card.Service}, INF {Path.GetFileName(card.InfPath)} ({card.Provider}), problem {card.Problem}" +
                    $"; {p.VendorName} INF on record {Path.GetFileName(_cfg.VendorInf)}");
                var c = p.Capabilities;
                Say($"card profile: {p.VendorName} {p.FamilyName} ({p.VendorId}:{p.DeviceId}), radio {p.Backend}, " +
                    $"concurrent {c.SupportsConcurrentWifi}, fast send {c.SupportsFastSend}, A-MPDU {c.SupportsAggregate}, " +
                    $"kernel driver {c.SupportsKernelDriver}, register BAR >= 0x{c.MinBarBytes:x}");
                if (EffectiveMode() != _cfg.Mode)
                    Say($"mode: {p.VendorName} {p.FamilyName} cannot keep Wi-Fi beside AirDrop; runs {EffectiveMode()} instead of {_cfg.Mode}");
            }
        }
        catch (Exception e) { Say("card: " + e.Message); }
    }

    /// <summary>Called once when the service starts.</summary>
    public void Boot()
    {
        // A config the radio never read (it deletes it once read) holds the
        // Wi-Fi key: never leave one behind across a restart.
        try { File.Delete(Path.Combine(ServiceConfig.Folder, "run", "sta.conf")); } catch (Exception) { }
        SayHeader();
        _ = Task.Run(SetTapMetric);
        if (KernelDriver) Start("boot (kernel driver: Windows keeps Wi-Fi)");
        else if (EffectiveMode() == NetMode.Concurrent) Start("boot (concurrent mode)");
        else
        {
            EnsureVendorDriver();
            // Networks lost by an earlier version or a card re-detected outside nekodrop.
            _ = Task.Run(() => WlanBackup.Restore(Say, TimeSpan.FromSeconds(30)));
        }
        _ = Task.Run(() => Watchdog(_cts.Token));
    }

    public void SetMode(NetMode mode)
    {
        lock (_lock)
        {
            _cfg.Mode = mode;
            /* Choosing a mode tries again: the router may have been changed to WPA2-AES. */
            _fallbackReason = "";
            _cfg.UnsupportedNetworks.Clear();
            _cfg.Save();
        }
        Say($"mode: {mode}");
        // Saved as chosen; a card without a station runs Concurrent on demand.
        if (EffectiveMode() == NetMode.Concurrent) Start("mode changed");
        else Stop("mode changed", giveBack: true);
        Publish();
    }

    /// <summary>
    /// Cleans up any legacy TAP NetworkAddress override left by v2.3.5/v2.3.6 (#96).
    /// Windows' Wi-Fi MAC is now carried in sta.conf (`mac=...`) and translated at
    /// the TAP boundary so factory OUI MACs work without tap-windows6 rejecting them.
    /// </summary>
    void EnsureTapMac()
    {
        bool applied;
        lock (_lock) applied = _cfg.TapMacApplied;
        if (!applied) return;
        var why = TapMac.Apply(null);
        if (why == "") lock (_lock) { _cfg.TapMacApplied = false; _cfg.Save(); }
        Say(why == "" ? "Wi-Fi: reset legacy TAP MAC override (air MAC translation is used instead)"
                      : "Wi-Fi: could not reset legacy TAP MAC override: " + why);
    }

    /// <summary>Concurrent mode shows the router Windows' Wi-Fi MAC address (#96); a running radio restarts.</summary>
    public void SetKeepWindowsMac(bool on)
    {
        lock (_lock)
        {
            if (_cfg.KeepWindowsMac == on) return;
            _cfg.KeepWindowsMac = on;
            _cfg.Save();
        }
        Say($"Wi-Fi: use Windows' MAC address: {(on ? "on" : "off")}");
        if (State is RadioState.On or RadioState.Starting)
            _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        Publish();
    }

    /// <summary>In on-demand mode, give the Wi-Fi card back a few seconds after a transfer completes (#96).</summary>
    public void SetQuickReleaseOnDemand(bool on)
    {
        lock (_lock)
        {
            if (_cfg.QuickReleaseOnDemand == on) return;
            _cfg.QuickReleaseOnDemand = on;
            _cfg.Save();
            if (!on) _quickReleaseAt = DateTime.MinValue;
        }
        Say($"on-demand quick release after transfer: {(on ? "on" : "off")}");
        Publish();
    }

    /// <summary>Seconds without AirDrop activity before on-demand mode gives the card back (#96).</summary>
    public void SetIdleSeconds(int seconds)
    {
        seconds = Math.Clamp(seconds, 15, 600);
        lock (_lock)
        {
            if (_cfg.IdleSeconds == seconds) return;
            _cfg.IdleSeconds = seconds;
            _cfg.Save();
        }
        Say($"on-demand idle timeout: {seconds} s");
        Publish();
    }

    /// <summary>Experimental fast unlock of a passive 5 GHz channel (#11); a running radio restarts to take it.</summary>
    public void SetFastUnlock(bool on)
    {
        lock (_lock)
        {
            if (_cfg.FastUnlock == on) return;
            _cfg.FastUnlock = on;
            _cfg.Save();
        }
        Say($"experimental fast passive-channel unlock: {(on ? "on" : "off")}");
        if (State is RadioState.On or RadioState.Starting)
            _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        Publish();
    }

    /// <summary>Experimental coexistence plan (ForbiddenItlwm PR #30); a running radio restarts to take it.</summary>
    public void SetCoexPlan(bool on)
    {
        lock (_lock)
        {
            if (_cfg.CoexPlan == on) return;
            _cfg.CoexPlan = on;
            _cfg.Save();
        }
        Say($"experimental coexistence plan: {(on ? "on" : "off")}");
        if (State is RadioState.On or RadioState.Starting)
            _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        Publish();
    }

    /// <summary>Experimental high-throughput mode (AWDL HE80 TLV 12 + MTU 1500, #65); a running daemon restarts to take it.</summary>
    public void SetHighThroughput(bool on)
    {
        lock (_lock)
        {
            if (_cfg.HighThroughput == on) return;
            _cfg.HighThroughput = on;
            _cfg.Save();
        }
        Say($"experimental high-throughput mode (HE80 + MTU 1500): {(on ? "on" : "off")}");
        if (State is RadioState.On or RadioState.Starting)
            _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        Publish();
    }

    /// <summary>
    /// Experimental station data-plane acceleration: RX Block Ack reordering
    /// (sta.conf ba=1) and two TAP writer threads (tap_writers=2). Off makes
    /// the AP fall back to single frames and writes TAP on the RX thread;
    /// a running radio restarts to take the change.
    /// </summary>
    public void SetDataPlaneAccel(bool on)
    {
        lock (_lock)
        {
            if (_cfg.DataPlaneAccel == on) return;
            _cfg.DataPlaneAccel = on;
            _cfg.Save();
        }
        Say($"experimental data-plane acceleration: {(on ? "on" : "off")}");
        if (State is RadioState.On or RadioState.Starting)
            _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        Publish();
    }

    /// <summary>
    /// Experimental Wi-Fi 6 for nekodrop's own Wi-Fi connection (sta.conf he=1).
    /// A running radio restarts so the station rejoins with or without HE.
    /// </summary>
    public void SetStationWifi6(bool on)
    {
        lock (_lock)
        {
            if (_cfg.StationWifi6 == on) return;
            _cfg.StationWifi6 = on;
            _cfg.Save();
        }
        Say($"experimental Wi-Fi 6 for the station: {(on ? "on" : "off")}");
        if (State is RadioState.On or RadioState.Starting)
            _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        Publish();
    }

    /// <summary>
    /// The user's manual card choice (Settings): a PCI hardware ID and a
    /// CardCatalog.ManualModels key, or "" and "" for automatic. Used only for a
    /// card the catalog does not recognize by its ID. Returns "" or why not.
    /// </summary>
    public string SetManualCard(string hardwareId, string model)
    {
        hardwareId = hardwareId.Trim();
        if (hardwareId == "") model = "";
        else if (CardCatalog.ManualProfile(hardwareId, model) is null) return "this model does not fit this card";
        lock (_lock)
        {
            _cfg.ManualCard = hardwareId;
            _cfg.ManualModel = model;
            _cfg.Save();
            Card.Manual = (hardwareId, model);
        }
        Say(hardwareId == "" ? "card: automatic detection" : $"card: {hardwareId} chosen in settings as {model}");
        if (Card.Find() is { } c) { _profile = c.Profile; _caps = c.Profile.Capabilities; _cardLabel = $"{c.Profile.VendorName} {c.Profile.FamilyName}"; }
        else _cardLabel = "";
        if (_wanted) _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        Publish();
        return "";
    }

    DateTime _quickReleaseAt = DateTime.MinValue;
    DateTime _quickReleaseCooldownUntil = DateTime.MinValue;

    /// <summary>The client wants AirDrop now (opened the window, a phone nearby is sharing).</summary>
    public void Wake(bool retry = false, bool fromBle = false)
    {
        lock (_lock)
        {
            if (_holdFirmwareRetry && !retry) return;
            var now = DateTime.UtcNow;
            /* After quick release in on-demand mode, ignore passive BLE wake-ups
             * briefly so a nearby unlocked iPad does not take the card right back (#96). */
            if (fromBle && !retry && now < _quickReleaseCooldownUntil) return;
            /* Out of DMA memory (#94): a wake-up (a phone sharing nearby, every second
             * or so) restarted the radio at once and reset the back-off, so it retried
             * several times a second, each try locking gigabytes of pages to look for
             * low ones. The watchdog restarts when the back-off is over. */
            if (!retry && ((_retryAt != DateTime.MinValue && now < _retryAt) || now < _dmaHoldUntil))
            {
                _lastUse = now;
                return;
            }
            if (!fromBle) _quickReleaseAt = DateTime.MinValue;
        }
        _lastUse = DateTime.UtcNow;
        if (_cfg.Mode != NetMode.Off) Start("woken");
    }

    public void Touch()
    {
        lock (_lock) _quickReleaseAt = DateTime.MinValue;
        _lastUse = DateTime.UtcNow;
    }

    /// <summary>An AirDrop transfer finished: release the card soon when on-demand quick release is on (#96).</summary>
    public void TransferDone()
    {
        lock (_lock)
        {
            if (EffectiveMode() == NetMode.OnDemand && State == RadioState.On && _cfg.QuickReleaseOnDemand)
                _quickReleaseAt = DateTime.UtcNow + TimeSpan.FromSeconds(5);
        }
    }

    public void Start(string why)
    {
        lock (_lock)
        {
            if (_cts.IsCancellationRequested) return;
            if (_wanted && State is RadioState.On or RadioState.Starting) return;
            _wanted = true;
            _wifiEpoch++;
            _wifiCancel.Cancel();
            _wifiCancel = new();
            _holdFirmwareRetry = false;
            _restarts = 0;
            _retryAt = DateTime.MinValue;
        }
        Say($"starting: {why}");
        // The peer's last channel even without a country (#4: the country only
        // arrives after the move, so every start sat on channel 6 first). On a
        // passive channel the radio waits for a beacon before transmitting, and
        // the 30 s check below falls back to the start channel.
        _ = Task.Run(() => Launch(_cfg.PeerChannel != 0 ? _cfg.PeerChannel : _cfg.StartChannel));
    }

    public void Stop(string why, bool giveBack)
    {
        int epoch;
        lock (_lock)
        {
            _wanted = false;
            _wifiJoinQueued = false;
            _runningStation = null;
            _runningStationConnected = false;
            _pendingManualSuccess = null;
            _stationVersion++;
            epoch = ++_wifiEpoch;
            _wifiCancel.Cancel();
            _wifiCancel = new();
        }
        Say($"stopping: {why}");
        // Serialized with launches (round 2 #7): a launch in progress sees
        // _wanted false at its next check and returns; then the card goes back.
        _launchGate.Wait();
        try
        {
            // A give-back task from the previous session may still be waiting
            // for DHCP or its PnP fallback. Do not let it touch the card while
            // this stop is binding the vendor driver: that used to add a second
            // restart and a full extra 12 s to the hand-back.
            WaitForWifiReconnect("before stopping");
            var sw = Stopwatch.StartNew();
            bool cleanRadio = KillDaemons();
            Say($"timing: radio stopped in {sw.ElapsedMilliseconds} ms (clean={cleanRadio})");
            if (giveBack)
            {
                // The disconnected TAP used to keep its router-assigned IPv4
                // address and the only default route. Windows then joined Wi-Fi
                // with APIPA only (no Internet, and sometimes a password
                // prompt after the stale lease expired). Release it before the
                // vendor driver comes back, so Windows' Wi-Fi gets the lease.
                // It is independent of the PCI bind; run both concurrently.
                var releaseWatch = Stopwatch.StartNew();
                var releaseTask = Task.Run(() =>
                {
                    var result = TapNet.Release();
                    return (Result: result, ElapsedMs: releaseWatch.ElapsedMilliseconds);
                });
                EnsureVendorDriver(reconnect: true, resetCard: !cleanRadio, epoch: epoch);
                var release = releaseTask.GetAwaiter().GetResult();
                Say(release.Result == ""
                    ? $"Wi-Fi: released the TAP address and default route before giving the card back ({release.ElapsedMs} ms)"
                    : "Wi-Fi: could not release the TAP address: " + release.Result);
            }
            lock (_lock) { State = RadioState.Off; _wifi = ""; _channel = 0; }
        }
        finally { _launchGate.Release(); }
        Publish();
    }

    /// <summary>Waits for the previous give-back task before taking or releasing the card.</summary>
    void WaitForWifiReconnect(string why)
    {
        Task? task;
        lock (_lock) task = _wifiReconnect;
        if (task is null || task.IsCompleted) return;
        Say($"Wi-Fi: waiting for the previous reconnect before {why}");
        try
        {
            if (!task.Wait(TimeSpan.FromSeconds(20)))
                Say("Wi-Fi: the previous reconnect is still running after 20 s; continuing with the card");
        }
        catch (AggregateException e)
        {
            Say("Wi-Fi: the previous reconnect failed: " + e.GetBaseException().Message);
        }
    }

    /// <summary>One launch at a time: the watchdog, a wake, a mode change and a channel move
    /// used to start radios concurrently and kill each other's (hardware, 2026-11-17).</summary>
    readonly SemaphoreSlim _launchGate = new(1, 1);

    void Launch(int channel)
    {
        _launchGate.Wait();
        try { LaunchLocked(channel); }
        finally { _launchGate.Release(); }
    }

    void LaunchLocked(int channel)
    {
        if (!_wanted) return;
        WaitForWifiReconnect("taking the card");
        if (!_wanted) return;
        channel = Mt76RadioBackend.ChannelFor(_profile, channel, _cfg.Country);
        lock (_lock)
        {
            State = RadioState.Starting; _channel = channel; _detail = "";
            _earlyFirmwareError = false;
            // A scan the old radio never finished must not block auto-connect forever (Codex review #29).
            _scanBusy = false;
            _wantBootScan = false;
            _mtkBackgroundScanSsid = null;
            _channelOpen = false;
            _hiddenProbed.Clear();
            _launchGen++;
            _runningStation = null;
            _runningStationConnected = false;
            _stationRecorded = false;
            _stationVersion++;
            _scanPending = [];
        }
        Publish();
        try
        {
            KillDaemons();
            string? staConf = null;
            Wlan.Network? stationNet = null;
            var kernel = KernelDriver;
            if (!kernel && EffectiveMode() == NetMode.Concurrent)
            {
                var why = "";
                var cardFree = !(Card.Find() is { } c && Card.TakenByProxy(c));
                if (cardFree)
                {
                    try
                    {
                        var imported = _known.ImportFromWindows();
                        if (imported > 0) Say($"Wi-Fi: imported {imported} saved network(s) from Windows");
                    }
                    catch (Exception e) { Say("Wi-Fi: could not read Windows' saved networks: " + e.Message); }
                }
                if (cardFree && TapMac.WindowsWifiMac() is { } windowsMac && windowsMac != _cfg.WindowsMac)
                    lock (_lock) { _cfg.WindowsMac = windowsMac; _cfg.Save(); }
                // The user's choice wins over what Windows was connected to.
                var fresh = cardFree && !_userChose && !_userOff ? Wlan.Current(out why) : null;
                // Not beside a wired connection: that left two default routes (issue #1).
                var wired = fresh is null && _net is null && !_userChose && Wlan.WiredUp();
                string? knownBad = null;
                lock (_lock)
                    if (fresh is not null && _cfg.UnsupportedNetworks.TryGetValue(fresh.Ssid, out var bad))
                    {
                        if (DateTime.UtcNow - bad.SeenUtc < UnsupportedMemory) knownBad = bad.Reason;
                        else { _cfg.UnsupportedNetworks.Remove(fresh.Ssid); _cfg.Save(); }
                    }
                if (knownBad is not null)
                {
                    Say($"Wi-Fi: \"{fresh!.Ssid}\" cannot be joined beside AirDrop ({knownBad}); Windows keeps the card and AirDrop runs on demand");
                    lock (_lock) { _fallbackReason = knownBad; _wanted = false; State = RadioState.Off; }
                    Publish();
                    return;
                }
                if (wired) Say("Wi-Fi: a wired connection is up; not joining a saved network on its own");
                // If Windows is already in the middle of connecting at boot, wait briefly
                // instead of cutting it off; never call WlanConnect across saved profiles
                // (blocked cold start for 6 minutes with 23 profiles, #4).
                if (cardFree && !wired && !_userChose && !_userOff && fresh is null && _net is null && Wlan.IsConnecting())
                {
                    var end = DateTime.UtcNow + TimeSpan.FromSeconds(3);
                    while (DateTime.UtcNow < end && _wanted && fresh is null)
                    {
                        Thread.Sleep(250);
                        fresh = Wlan.Current(out why);
                        if (fresh is null && !Wlan.IsConnecting()) break;
                    }
                }
                if (fresh is not null)
                {
                    lock (_lock) { _net = fresh; _joinOrigin = JoinOrigin.Inherited; _pendingManualSuccess = null; }
                    _known.Remember(fresh.Ssid, fresh.Passphrase, fresh.PskHex, fresh.Open);
                    // A card whose station joins only 5 GHz (MT7902): Windows' 2.4 GHz access
                    // point is no use, so the radio looks for the network's 5 GHz one by name.
                    if (!_caps.StationOn24GHz && fresh.Channel <= 14)
                    {
                        Say($"Wi-Fi: Windows was on \"{fresh.Ssid}\" at 2.4 GHz; this card joins 5 GHz only, so nekodrop scans for its 5 GHz access point");
                        _net = null;
                        lock (_lock) { _pendingConnect = (fresh.Ssid, null, null, JoinOrigin.Inherited, false); _wantBootScan = true; }
                    }
                }
                var net = _userOff ? null : _net;
                lock (_lock) _staSsid = net?.Ssid ?? "";
                if (net is not null && (net.Open || net.Passphrase is not null || net.PskHex is not null))
                {
                    string? staMac;
                    lock (_lock) staMac = _cfg.KeepWindowsMac ? TapMac.FormatColon(_cfg.WindowsMac) : null;
                    staConf = Wlan.WriteStaConfig(net, _cfg.DataPlaneAccel, _cfg.StationWifi6, staMac);
                    stationNet = net;
                    Say($"Wi-Fi: joining \"{net.Ssid}\" ({net.Security}, channel {net.Channel}) beside AirDrop" +
                        (staMac is not null ? " (using Windows' Wi-Fi MAC address)" : ""));
                }
                else if (net is null && !wired && !_userOff && !_userChose &&
                         why is "Wi-Fi is not connected" or "no WLAN interface" or "" &&
                         _known.All().Any(k => k.HasKey && k.AutoConnect))
                {
                    // Saved networks were imported into _known: start AirDrop at once and
                    // let the radio scan and join the strongest one in range (#4).
                    Say("Wi-Fi: Windows is not connected; AirDrop starts now and nekodrop will scan for a saved network");
                    lock (_lock) { _wifi = ""; _wantBootScan = true; }
                }
                else if (!string.IsNullOrEmpty(why))
                {
                    Say($"Wi-Fi: {why}; AirDrop runs without it");
                    var notConnected = why is "Wi-Fi is not connected" or "no WLAN interface" or "WLAN service unavailable";
                    lock (_lock) _wifi = notConnected ? "" : "unsupported: " + why;
                }
            }
            // With nekodrop-wlan.sys the card stays where it is: Windows runs
            // the station, the radio only drives AWDL through the driver.
            // Stop may have run meanwhile: never take the card or start a radio for it (Codex review #10).
            if (!_wanted) { lock (_lock) State = RadioState.Off; Publish(); return; }
            if (!kernel) TakeCard();
            else if (Card.Find() is { } kc) { _profile = kc.Profile; _caps = kc.Profile.Capabilities; }
            if (!_wanted) { lock (_lock) State = RadioState.Off; Publish(); return; }
            if (!kernel && EffectiveMode() == NetMode.Concurrent) EnsureTapMac();
            channel = Mt76RadioBackend.ChannelFor(_profile, channel, _cfg.Country);
            lock (_lock) _channel = channel;
            var launch = CardCatalog.BuildRadioLaunch(_profile, channel, kernel, _cfg.Country, staConf, true, _cfg.FastUnlock,
                                                      _cfg.CoexPlan, btCoexBalanced: true, highThroughput: _cfg.HighThroughput);
            _startedAt = DateTime.UtcNow;
            int gen;
            lock (_lock)
            {
                gen = _launchGen;
                _runningStation = stationNet;
                _runningJoinOrigin = ReferenceEquals(stationNet, _net) ? _joinOrigin : JoinOrigin.Inherited;
                _runningUserChose = _userChose;
            }
            var radio = _radio = Spawn(Path.Combine(_root, "radio", launch.ExeName), launch.Args, line => OnRadioLine(line, gen));
            // awdld connects to the radio's pipe; give the firmware time to boot.
            for (int i = 0; i < 100 && _wanted && State == RadioState.Starting && !radio.HasExited; i++) Thread.Sleep(100);
            if (!_wanted) return;
            if (radio.HasExited) throw new InvalidOperationException("the radio exited during bring-up: " + _detail);
            bool useHe80 = Mt76RadioBackend.IsMt7925(_profile) ? launch.Args.Contains("--he80") : _cfg.HighThroughput;
            string[] awdlArgs = useHe80
                ? ["-c", channel.ToString(), "-H", "-M", "1500"]
                : ["-c", channel.ToString()];
            // An awdld built from the pinned winawdl source predates -H/-M (the
            // released binary has them). Launched with flags it does not know it
            // prints its usage and exits, which reads as a radio that keeps
            // failing: the card goes back to Windows and AirDrop disappears with
            // no hint why. Start it without them instead.
            string awdldExe = Path.Combine(_root, "awdl", "awdld.exe");
            if (useHe80 && !AwdldTakesHighThroughput(awdldExe))
            {
                awdlArgs = ["-c", channel.ToString()];
                Say("awdld does not take -H/-M (built from source?): starting without high throughput");
            }
            _awdld = Spawn(awdldExe, awdlArgs, OnAwdldLine);
        }
        catch (Exception e)
        {
            try { File.Delete(Path.Combine(ServiceConfig.Folder, "run", "sta.conf")); } catch (Exception) { }
            if (Mt76RadioBackend.IsMt7925(_profile))
            {
                KillDaemons();
                try { EnsureVendorDriver(); }
                catch (Exception restoreError) { Say("MT7925 driver restore failed: " + restoreError.Message); }
            }
            var bind = e as CardBindException ?? e.InnerException as CardBindException;
            Say("start failed: " + (bind?.Message ?? e.Message));
            lock (_lock)
            {
                // Windows refusing the driver is not cured by retrying: no restart loop
                // (it ran five times, each time leaving the card without a driver).
                if (bind is not null) _wanted = false;
                State = _wanted || bind is not null ? RadioState.Error : RadioState.Off;
                _detail = bind?.Message ?? e.Message;
            }
            Publish();
        }
    }

    /// <summary>Whether the awdld at <paramref name="exe"/> accepts the high-throughput
    /// flags. The released binary does; one built from the pinned winawdl source does
    /// not, and launched with -H it prints its usage and exits. An unknown option makes
    /// awdld print that usage before it touches the card, so the first start probes once
    /// and caches the answer.</summary>
    static bool AwdldTakesHighThroughput(string exe)
    {
        lock (_probeLock)
        {
            if (_awdldTakesHighThroughput is { } known) return known;
            bool takes = false;
            try
            {
                var info = new ProcessStartInfo(exe, "-Z")
                {
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                };
                using var probe = Process.Start(info);
                if (probe is not null)
                {
                    var err = probe.StandardError.ReadToEndAsync();
                    var outp = probe.StandardOutput.ReadToEndAsync();
                    if (!probe.WaitForExit(2000))
                    {
                        try { probe.Kill(true); } catch (Exception) { }
                    }
                    takes = (err.GetAwaiter().GetResult() + outp.GetAwaiter().GetResult()).Contains("-H");
                }
            }
            catch (Exception) { }
            _awdldTakesHighThroughput = takes;
            return takes;
        }
    }
    static readonly object _probeLock = new();
    static bool? _awdldTakesHighThroughput;
    /* awdld warns for every frame it does not parse, several a second (issue #1):
       the first is logged, then one count a minute. */
    readonly object _noiseLock = new();
    DateTime _noiseAt = DateTime.MinValue;
    int _noiseCount;

    void OnAwdldLine(string line)
    {
        if (line.Contains("unhandled frame", StringComparison.Ordinal))
        {
            string? summary = null;
            lock (_noiseLock)
            {
                var now = DateTime.UtcNow;
                if (_noiseAt == DateTime.MinValue) { _noiseAt = now; summary = line + " (repeats are counted once a minute)"; }
                else
                {
                    _noiseCount++;
                    if (now - _noiseAt >= TimeSpan.FromMinutes(1))
                    {
                        summary = $"{_noiseCount} more unhandled frames";
                        _noiseCount = 0;
                        _noiseAt = now;
                    }
                }
            }
            if (summary is not null) Say("awdld: " + summary);
            return;
        }
        Say("awdld: " + line);
        // "peers 1 | rx action 21, data 84 (15 joined) | ...": AWDL data from peers. The
        // RTL8852CE and MT7902 radios print no "rx:" line, so without this a channel move
        // (now reported by the RTL8852CE radio too, #32) never waited for a transfer to end.
        var m = AwdldRxData.Match(line);
        if (m.Success && long.TryParse(m.Groups[1].Value, out var data))
        {
            if (data != _awdldRxData) { _lastData = DateTime.UtcNow; _lastUse = DateTime.UtcNow; }
            _awdldRxData = data;
        }
    }

    static readonly System.Text.RegularExpressions.Regex AwdldRxData =
        new(@"\| rx action \d+, data (\d+)", System.Text.RegularExpressions.RegexOptions.CultureInvariant);
    long _awdldRxData = -1;

    internal static string NormalizeRadioLine(string line)
    {
        var at = line.IndexOf("radio: ", StringComparison.Ordinal);
        var text = at >= 0 ? line[(at + 7)..] : line;
        if (text.StartsWith("[+", StringComparison.Ordinal) &&
            text.IndexOf("] ", StringComparison.Ordinal) is > 0 and var close)
            text = text[(close + 2)..];
        return text;
    }

    void OnRadioLine(string line, int generation)
    {
        Say("radio: " + line);
        if (generation != Volatile.Read(ref _launchGen)) return;
        var text = NormalizeRadioLine(line);
        if (text.StartsWith("ready on channel", StringComparison.Ordinal))
        {
            bool bootScan, bootBackground;
            lock (_lock)
            {
                State = RadioState.On;
                bootScan = _wantBootScan;
                bootBackground = _pendingConnect is null;
                _wantBootScan = false;
                if (bootScan) _wifiDownSince = DateTime.MinValue;
            }
            Publish();
            if (bootScan) WifiScan(background: bootBackground);
        }
        else if (text.StartsWith("bring-up failed: ", StringComparison.Ordinal))
        {
            lock (_lock) _detail = text[17..];
        }
        else if (text.StartsWith("firmware/DMA error:", StringComparison.Ordinal))
        {
            // BE200 #53 reports this as soon as it says ready. AX201 #50
            // reached the same fatal path during its first Wi-Fi association.
            lock (_lock)
                if (_profile.Backend == RadioBackendKind.Iwx &&
                    DateTime.UtcNow - _startedAt <= TimeSpan.FromSeconds(30))
                    _earlyFirmwareError = true;
        }
        else if (text.StartsWith("rx: ", StringComparison.Ordinal))
        {
            // "... N addressed to us": transfers keep a channel move waiting.
            var parts = text.Split(' ');
            var i = Array.IndexOf(parts, "addressed");
            if (i > 0 && long.TryParse(parts[i - 1], out var n))
            {
                if (n != _rxToUs) { _lastData = DateTime.UtcNow; _lastUse = DateTime.UtcNow; }
                _rxToUs = n;
            }
        }
        else if (text.Contains("transmitting allowed here", StringComparison.Ordinal))
        {
            // A beacon or AWDL peer unlocked a passive 5 GHz channel (#11):
            // do not bounce back to channel 6 after 30 s.
            lock (_lock) _channelOpen = true;
        }
        else if (text.StartsWith("event: ", StringComparison.Ordinal))
        {
            OnEvent(text[7..].Split(' ', 2), generation);
        }
    }

    void OnEvent(string[] e, int generation)
    {
        var name = e[0];
        var value = e.Length > 1 ? e[1] : "";
        switch (name)
        {
            case "peer-country":
            case "country":
                if (value.Length == 2 && value != _cfg.Country)
                {
                    lock (_lock) { _cfg.Country = value; _cfg.Save(); }
                    Publish();
                }
                break;
            case "peer-channel":
                if (int.TryParse(value, out var ch) && (ch == 44 || ch == 149))
                {
                    lock (_lock) { _cfg.PeerChannel = ch; _cfg.Save(); }
                    if (_channel != ch) _ = Task.Run(() => MoveWhenIdle(ch));
                }
                break;
            case "channel":
                // "44 passive": no transmitting until a beacon or AWDL peer is heard, or
                // the firmware unlocks the channel; if it stays passive, go back to 6 (#11).
                if (value.EndsWith("active", StringComparison.Ordinal))
                {
                    lock (_lock) _channelOpen = true;
                }
                else if (value.EndsWith("passive", StringComparison.Ordinal))
                {
                    int gen, passiveCh;
                    lock (_lock) { gen = _launchGen; passiveCh = _channel; }
                    _ = Task.Run(async () =>
                    {
                        await Task.Delay(TimeSpan.FromSeconds(30));
                        bool bounce;
                        lock (_lock)
                            bounce = State == RadioState.On && !Mt76RadioBackend.IsMt7925(_profile) &&
                                     _launchGen == gen && _channel == passiveCh &&
                                     _channel > 14 && !_channelOpen && _rxToUs == 0 && _wifi != "connected";
                        if (bounce)
                        {
                            Say($"channel {passiveCh} stayed passive: back to {_cfg.StartChannel}");
                            lock (_lock) _cfg.PeerChannel = 0;
                            Relaunch(_cfg.StartChannel);
                        }
                    });
                }
                break;
            case "wifi-unsupported":
            {
                /* The network's security cannot be joined (#97). If it is the one
                 * Windows was on (not one the user picked in nekodrop), give the
                 * card back so the PC stays online, and remember the network. */
                string ssid;
                bool fallback;
                lock (_lock)
                {
                    ssid = _staSsid;
                    _wifiError = "unsupported-network:" + value;
                    fallback = !_userChose && ssid != "" && EffectiveMode() == NetMode.Concurrent;
                    if (fallback)
                    {
                        _cfg.UnsupportedNetworks[ssid] = new UnsupportedNetwork { Reason = value, SeenUtc = DateTime.UtcNow };
                        _cfg.Save();
                        _fallbackReason = value;
                    }
                }
                Say($"Wi-Fi: \"{ssid}\" cannot be joined beside AirDrop ({value})" +
                    (fallback ? "; giving the card back to Windows, AirDrop runs on demand" : ""));
                if (fallback) _ = Task.Run(() => Stop("unsupported Wi-Fi security", giveBack: true));
                Publish();
                break;
            }
            case "wifi-yield":
                // The radio pauses the station during AirDrop transfers: the
                // gateway cannot answer then, and the probe must not rejoin.
                // A pause lasts at most 20 s, so "on" needs no "off" to expire.
                lock (_lock) _yieldUntil = DateTime.UtcNow + (value == "on" ? YieldGrace * 2 : YieldGrace);
                break;
            case "wifi":
                string? rescan = null;
                lock (_lock)
                {
                    if (generation != _launchGen || !_wanted) break;
                    _runningStationConnected = value == "connected";
                    if (_wifi != value)
                    {
                        _stationVersion++;
                        _stationRecorded = false;
                    }
                    if (value == "connected")
                    {
                        _wifiDownSince = DateTime.MaxValue;
                        _wifiError = "";
                        _mtkBackgroundScans = 0;
                    }
                    else if (_wifi == "connected" || _wifiDownSince == DateTime.MaxValue) _wifiDownSince = DateTime.UtcNow;
                    _wifi = value;
                    if (value == "failed" && _runningStation is { } failed && ReferenceEquals(failed, _net))
                    {
                        var now = DateTime.UtcNow;
                        var key = (failed.Ssid, failed.Bssid.ToUpperInvariant());
                        if (failed.Bssid != "" && (!_failedAccessPoints.TryGetValue(key, out var until) || until <= now))
                            _failedAccessPoints[key] = now + WifiSelection.FailedApCooldown;
                        // Let the next background scan look at other SSIDs after
                        // an automatic attempt. A manual/Windows choice can still
                        // look for a new BSSID for the same SSID (e.g. hotspot #27).
                        if (_runningJoinOrigin != JoinOrigin.Automatic && !_userOff && !_scanBusy &&
                            _pendingConnect is null && now - _scanAt > StaleScan)
                        {
                            _pendingConnect = (failed.Ssid, null, null, _runningJoinOrigin, true);
                            rescan = failed.Ssid;
                        }
                    }
                }
                Publish();
                if (rescan is { } retry)
                {
                    Say($"Wi-Fi: joining \"{retry}\" failed; scanning for it again");
                    WifiScan(retry);
                }
                break;
            case "bss":
            {
                // "<bssid> <channel> <dBm> <security> <ssid hex>"
                var f = value.Split(' ');
                if (f.Length == 5 && int.TryParse(f[1], out var bch) && int.TryParse(f[2], out var dbm))
                {
                    string ssid;
                    try { ssid = System.Text.Encoding.UTF8.GetString(Convert.FromHexString(f[4])); }
                    catch (FormatException) { break; }
                    lock (_lock)
                        if (generation == _launchGen && _wanted)
                            _scanPending.Add(new(ssid, f[0], bch, dbm, f[3]));
                }
                break;
            }
            case "scan-done":
                lock (_lock)
                {
                    if (generation != _launchGen || !_wanted) break;
                    // Keep all eligible BSSIDs for retries and band selection.
                    // The client's snapshot still shows one row per network name.
                    var on24 = _caps.StationOn24GHz;
                    _scan = _scanPending.Where(ap => WifiSelection.Supports(ap, on24)).ToList();
                    _scanPending = [];
                    _scanBusy = false;
                    _mtkBackgroundScanSsid = null;
                    _scanAt = DateTime.UtcNow;
                    foreach (var key in _failedAccessPoints.Where(p => p.Value <= _scanAt).Select(p => p.Key).ToArray())
                        _failedAccessPoints.Remove(key);
                }
                Publish();
                _ = Task.Run(AfterScan);
                break;
            case "scan-interrupted":
                string? pendingSsid = null;
                lock (_lock)
                {
                    _scanPending = [];
                    _scanBusy = false;
                    _scanAt = DateTime.UtcNow;
                    if (_mtkBackgroundScanSsid is { } hidden) _hiddenProbed.Remove(hidden);
                    _mtkBackgroundScanSsid = null;
                    if (value == "air-drop")
                    {
                        pendingSsid = _pendingConnect?.Ssid;
                    }
                    else if (_pendingConnect is not null)
                    {
                        _pendingConnect = null;
                        _pendingScans = 0;
                        _wifiError = "could not return to the AirDrop channel during the scan";
                    }
                }
                Publish();
                if (pendingSsid is not null) WifiScan(pendingSsid);
                break;
        }
    }

    int _moveGen;

    async Task MoveWhenIdle(int channel)
    {
        // Upstream waits 10 s without data before a move (kChannelMoveIdleUS).
        // No time limit: it gave up waiting after 120 s and moved anyway, which
        // restarted the radio in the middle of a send that was going fine on the
        // old channel (#9: 1.8 MB/s on 44, cut with TLS EOF 120 s after the
        // iPhone first named 149). One mover at a time, and none once the peer
        // names our channel again (#9's iPhone alternated 44 and 149).
        var gen = Interlocked.Increment(ref _moveGen);
        while (true)
        {
            if (!_wanted || _channel == channel || Volatile.Read(ref _moveGen) != gen || _cfg.PeerChannel != channel) return;
            if (DateTime.UtcNow - _lastData > TimeSpan.FromSeconds(10)) break;
            await Task.Delay(1000);
        }
        if (_wanted && _channel != channel && Volatile.Read(ref _moveGen) == gen)
        {
            Say($"peer uses channel {channel}: moving there (country {_cfg.Country})");
            Relaunch(channel);
        }
    }

    void Relaunch(int channel, Wlan.Network? requested = null)
    {
        _launchGate.Wait();
        try
        {
            // A queued scan result must not restart the radio after a newer
            // manual selection, disconnect or stop superseded its target.
            if (requested is not null)
                lock (_lock)
                {
                    if (!_wanted || !ReferenceEquals(requested, _net)) return;
                    // The old station can reconnect on its own while this
                    // background choice is waiting for the launch gate.
                    if (_joinOrigin == JoinOrigin.Automatic && _runningStationConnected &&
                        _runningStation is { } recovered)
                    {
                        _net = recovered;
                        _joinOrigin = _runningJoinOrigin;
                        _userChose = _runningUserChose;
                        _staSsid = recovered.Ssid;
                        _wifiJoinQueued = false;
                        return;
                    }
                }
            KillDaemons();
            LaunchLocked(channel);
        }
        finally
        {
            if (requested is not null)
                lock (_lock)
                    if (ReferenceEquals(requested, _net)) _wifiJoinQueued = false;
            _launchGate.Release();
        }
    }

    async Task Watchdog(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            await Task.Delay(2000, ct).ContinueWith(_ => { });
            if (!_wanted) continue;
            // Snapshots: Relaunch swaps the processes on another thread (Codex review #11).
            var radio = _radio;
            var awdld = _awdld;
            var radioDown = radio is null || radio.HasExited;
            var awdldDown = State is RadioState.On or RadioState.Error && (awdld is null || awdld.HasExited);
            // A radio that never says "ready" would hold Starting for good (Codex review #12).
            var startStuck = State == RadioState.Starting && _launchGate.CurrentCount > 0 &&
                             DateTime.UtcNow - _startedAt > TimeSpan.FromSeconds(60);
            if (_scanBusy && DateTime.UtcNow - _scanStartedAt > TimeSpan.FromSeconds(30))
                lock (_lock) { _scanBusy = false; _scanPending = []; }
            if ((radioDown || awdldDown || startStuck) && (State != RadioState.Starting || startStuck) &&
                _launchGate.CurrentCount > 0)
            {
                bool firmwareFault;
                lock (_lock)
                {
                    firmwareFault = _earlyFirmwareError;
                    if (firmwareFault) _holdFirmwareRetry = true;
                }
                if (firmwareFault)
                {
                    Stop("firmware error during startup; restoring the Windows driver", giveBack: true);
                    lock (_lock)
                    {
                        State = RadioState.Error;
                        _detail = "The Wi-Fi firmware stopped during startup. Select Retry to try again.";
                    }
                    Publish();
                    continue;
                }
                // Out of contiguous DMA memory (#30): six restarts 2.7 s apart all failed
                // the same way, while a start 16 s or 90 s later worked. Back off 5, 10, 20,
                // 30, 30 s before each restart, so Windows has time to free memory; every
                // other failure restarts at once as before.
                // The MT7902 reports the same transient condition using a different
                // diagnostic (#52): it needs lockable pages below 4 GiB.
                var dmaShortage = _detail.Contains("out of DMA memory", StringComparison.OrdinalIgnoreCase) ||
                                  _detail.Contains("DMA memory below 4 GiB: only", StringComparison.OrdinalIgnoreCase);
                if (dmaShortage && _restarts < 5)
                {
                    if (_retryAt == DateTime.MinValue)
                    {
                        var wait = TimeSpan.FromSeconds(Math.Min(30, 5 << _restarts));
                        _retryAt = DateTime.UtcNow + wait;
                        Say($"the radio found no contiguous DMA memory; trying again in {wait.TotalSeconds:0} s");
                        continue;
                    }
                    if (DateTime.UtcNow < _retryAt) continue;
                }
                _retryAt = DateTime.MinValue;
                if (++_restarts > 5)
                {
                    if (dmaShortage) lock (_lock) _dmaHoldUntil = DateTime.UtcNow + TimeSpan.FromSeconds(60);
                    Say("the radio keeps failing; giving the card back");
                    Stop("too many failures", giveBack: true);
                    lock (_lock) { State = RadioState.Error; _detail = "the radio keeps failing"; }
                    Publish();
                    continue;
                }
                Say(startStuck ? "the radio never became ready; restarting" : "a daemon stopped; restarting");
                Relaunch(_channel == 0 ? _cfg.StartChannel : _channel);
            }
            else if (State == RadioState.On && DateTime.UtcNow - _startedAt > TimeSpan.FromMinutes(2))
            {
                _restarts = 0;
            }
            // Wi-Fi down for a while: look for a known network to join.
            var now = DateTime.UtcNow;
            var mtk = _profile.Backend == RadioBackendKind.MtkRadio;
            // A full MT792x scan takes the radio away from AWDL for ~10 seconds
            // (#54, #56). Don't start automatic retries during a transfer; if
            // no network joins, back off instead of breaking sync every 40 s.
            // An explicit Wi-Fi scan or connect is still available in the UI.
            var scanDelay = mtk ? Math.Min(300, 30 << Math.Min(_mtkBackgroundScans, 4)) : 30;
            if (EffectiveMode() == NetMode.Concurrent && State == RadioState.On && !_userOff && _wifi != "connected" &&
                !_scanBusy && now - _wifiDownSince > TimeSpan.FromSeconds(20) &&
                now - _scanAt > TimeSpan.FromSeconds(scanDelay) &&
                (!mtk || (now - _lastUse > TimeSpan.FromSeconds(10) &&
                          _known.All().Any(k => k is { HasKey: true, AutoConnect: true }))) &&
                !Wlan.WiredUp())
            {
                if (mtk && _mtkBackgroundScans < 4) _mtkBackgroundScans++;
                WifiScan(background: true);
            }
            bool doQuickRelease = false;
            lock (_lock)
            {
                if (_quickReleaseAt != DateTime.MinValue && DateTime.UtcNow >= _quickReleaseAt)
                {
                    _quickReleaseAt = DateTime.MinValue;
                    doQuickRelease = EffectiveMode() == NetMode.OnDemand && State == RadioState.On && _cfg.QuickReleaseOnDemand;
                    if (doQuickRelease) _quickReleaseCooldownUntil = DateTime.UtcNow + TimeSpan.FromSeconds(15);
                }
            }
            if (doQuickRelease)
                Stop("transfer done (quick release)", giveBack: true);
            else if (EffectiveMode() == NetMode.OnDemand && State == RadioState.On &&
                DateTime.UtcNow - _lastUse > TimeSpan.FromSeconds(_cfg.IdleSeconds))
                Stop("idle", giveBack: true);
            if (DateTime.UtcNow - _probeAt >= TimeSpan.FromSeconds(15))
            {
                _probeAt = DateTime.UtcNow;
                try { await ProbeGateway(); } catch (Exception e) { Say("gateway probe: " + e.Message); }
            }
        }
    }

    /* ----------------------------------------------------------- gateway probe */

    DateTime _probeAt = DateTime.MinValue, _connectedAt = DateTime.MaxValue, _scanStartedAt = DateTime.MinValue;
    System.Net.IPAddress? _probeGateway;
    DateTime _rejoinAt = DateTime.MinValue;
    /// <summary>Until when the station is paused for AirDrop ("event: wifi-yield"), plus a grace period.</summary>
    DateTime _yieldUntil = DateTime.MinValue;
    static readonly TimeSpan YieldGrace = TimeSpan.FromSeconds(30);
    int _probeFails;
    bool _gatewayAnswered, _rejoinAsked;

    /// <summary>
    /// The link can look connected while nothing comes back from the access
    /// point (hardware, 2026-11-17: Windows lost the internet with the station
    /// still "connected"). Every 15 s the gateway on the "nekodrop" adapter is
    /// pinged; four misses in a row (a minute) make the station join again, and
    /// four more after that restart the radio. A gateway that never answered a
    /// ping since the join is not judged (some routers drop ICMP).
    /// </summary>
    async Task ProbeGateway()
    {
        bool connected, wanted;
        Wlan.Network? station;
        int stationVersion;
        lock (_lock)
        {
            // Not judged when the user turned Wi-Fi off or no station is set up (round 2 #11).
            wanted = EffectiveMode() == NetMode.Concurrent && State == RadioState.On && !_userOff && _staSsid != "";
            connected = wanted && _wifi == "connected";
            station = _runningStationConnected ? _runningStation : null;
            stationVersion = _stationVersion;
        }
        if (!wanted)
        {
            _connectedAt = DateTime.MaxValue;
            _probeFails = 0;
            _gatewayAnswered = _rejoinAsked = false;
            return;
        }
        if (!connected)
        {
            _connectedAt = DateTime.MaxValue;
            // A rejoin that never gets the link back escalates on its own clock (round 2 #10).
            if (_rejoinAsked && DateTime.UtcNow - _rejoinAt > TimeSpan.FromSeconds(90))
            {
                Say("Wi-Fi did not come back after joining again; restarting the radio");
                _probeFails = 0;
                _rejoinAsked = false;
                _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
                return;
            }
            // The rejoin we asked for disconnects briefly: keep the episode, so a
            // join that stays broken still escalates (Codex review #15).
            if (!_rejoinAsked)
            {
                _probeFails = 0;
                _gatewayAnswered = false;
            }
            return;
        }
        if (_connectedAt == DateTime.MaxValue) _connectedAt = DateTime.UtcNow;
        if (DateTime.UtcNow - _connectedAt < TimeSpan.FromSeconds(20)) return; /* DHCP first */
        lock (_lock)
            if (DateTime.UtcNow < _yieldUntil) return; /* paused for AirDrop, not judged */
        var adapter = System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces()
            .FirstOrDefault(n => n.Name == "nekodrop" && n.OperationalStatus == System.Net.NetworkInformation.OperationalStatus.Up);
        var props = adapter?.GetIPProperties();
        static bool V4(System.Net.IPAddress a) =>
            a.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork && !a.Equals(System.Net.IPAddress.Any);
        var gateway = props?.GatewayAddresses.Select(g => g.Address).FirstOrDefault(V4);
        var source = props?.UnicastAddresses.Select(u => u.Address).FirstOrDefault(V4);
        if (gateway is null || source is null)
        {
            // No DHCP gateway long after the join is a failure too (Codex review #18).
            if (DateTime.UtcNow - _connectedAt > TimeSpan.FromSeconds(90)) Failed("no gateway from DHCP");
            return;
        }
        if (!gateway.Equals(_probeGateway))
        {
            // Another network or router: its ICMP behaviour is learned afresh (Codex review #19).
            _probeGateway = gateway;
            _gatewayAnswered = false;
            _probeFails = 0;
        }
        var answered = false;
        for (int i = 0; i < 3 && !answered; i++)
            answered = await Task.Run(() => Icmp.Ping(source, gateway, 1000));
        if (answered)
        {
            if (_probeFails >= 4) Say($"the gateway {gateway} answers again");
            _probeFails = 0;
            _gatewayAnswered = true;
            _rejoinAsked = false;
            if (station is not null)
                lock (_lock)
                    if (!_stationRecorded && stationVersion == _stationVersion && _wanted && !_userOff &&
                        _runningStationConnected && _wifi == "connected" &&
                        ReferenceEquals(station, _net) && ReferenceEquals(station, _runningStation))
                    {
                        // Authentication alone is not success: DHCP/TAP and the
                        // source-bound IPv4 gateway also have to work. Routers
                        // that filter ICMP keep working but do not get a new rank.
                        bool manual = ReferenceEquals(station, _pendingManualSuccess);
                        _known.ConfirmConnection(station.Ssid, manual, DateTime.UtcNow);
                        if (manual) _pendingManualSuccess = null;
                        _stationRecorded = true;
                        _failedAccessPoints.Remove((station.Ssid, station.Bssid.ToUpperInvariant()));
                    }
            return;
        }
        if (!_gatewayAnswered) return; /* a router that drops ICMP is not judged */
        Failed($"the gateway {gateway} does not answer");
    }

    /// <summary>One more failed probe: four in a row rejoin, four more restart the radio.</summary>
    void Failed(string why)
    {
        _probeFails++;
        if (_probeFails == 4)
        {
            Say($"{why} for a minute; joining Wi-Fi again");
            _rejoinAsked = true;
            _rejoinAt = DateTime.UtcNow;
            RadioCommand("rejoin");
        }
        else if (_probeFails >= 8)
        {
            Say($"{why} after joining again; restarting the radio");
            _probeFails = 0;
            _rejoinAsked = false;
            _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        }
    }

    void TakeCard()
    {
        var card = Card.Find() ?? throw new InvalidOperationException(
            "no supported Wi-Fi card found (Settings > Wi-Fi card lets you choose the model by hand)");
        _profile = card.Profile;
        _cardLabel = $"{card.Profile.VendorName} {card.Profile.FamilyName}";
        _caps = card.Profile.Capabilities;
        if (Card.TakenByProxy(card)) return;
        // Only the vendor's INF is the one to give the card back to, never the proxy's (issue #12).
        // Not checked against the vendor's provider names: OEM repackages of
        // Intel's driver carry other names, and refusing them would leave the
        // card with no driver to go back to.
        if (card.InfPath != "" && !Card.BoundToProxy(card))
        {
            lock (_lock) { _cfg.VendorInf = card.InfPath; _cfg.Save(); }
        }
        // Windows' saved networks live under its WLAN interface, which goes away
        // with the card and may come back with a new GUID: put back any that
        // went missing, then back them all up before taking the card.
        if (!Card.BoundToProxy(card))
        {
            WlanBackup.Restore(Say, TimeSpan.Zero);
            WlanBackup.Save(Say);
        }
        var inf = Path.Combine(_root, "driver", card.Profile.ProxyInfName);
        Say($"taking the card from {Path.GetFileName(card.InfPath)}");
        try { Card.Bind(card.HardwareId, inf); }
        catch (CardBindException e)
        {
            // A failed bind can leave the card with no driver at all, and so
            // Windows without Wi-Fi (issue #3): give it straight back.
            Say("taking the card failed: " + e.Message);
            EnsureVendorDriver();
            throw;
        }
        // The proxy's device interface appears once the UMDF host has started.
        for (int i = 0; i < 300; i++)
        {
            var now = Card.Find();
            if (now is not null && Card.TakenByProxy(now)) return;
            Thread.Sleep(100);
        }
        // Bound but never started (issue #12: Windows 10, then "register proxy not
        // openable (error 259)" in a loop with the card held): say why, give it back.
        var after = Card.Find();
        var state = after is null ? "the card disappeared"
            : $"driver service '{after.Service}', provider '{after.Provider}', INF {Path.GetFileName(after.InfPath)}, " +
              $"Device Manager problem code {after.Problem}";
        Say("the register proxy did not start: " + state);
        EnsureVendorDriver();
        throw new CardBindException($"the register proxy was bound but did not start ({state})", new TimeoutException());
    }

    /* ------------------------------------------------------- Wi-Fi management */

    bool IsCooling(WifiSelection.AccessPoint ap, DateTime now) =>
        _failedAccessPoints.TryGetValue(ap.Key, out var until) && now < until;

    void RadioCommand(string line)
    {
        try
        {
            if (_radio is { HasExited: false } r)
            {
                r.StandardInput.WriteLine(line);
                r.StandardInput.Flush();
            }
        }
        catch (Exception) { }
    }

    /// <summary>Refreshes the list; `ssid` makes it a directed scan (hidden networks).</summary>
    public void WifiScan(string? ssid = null, bool background = false)
    {
        if (State != RadioState.On) return;
        var mtkBackground = background && _profile.Backend == RadioBackendKind.MtkRadio;
        lock (_lock)
        {
            _scanPending = [];
            _scanBusy = true;
            _scanStartedAt = DateTime.UtcNow;
            _mtkBackgroundScanSsid = mtkBackground ? ssid : null;
        }
        var cmd = mtkBackground ? "scan-bg" : "scan";
        RadioCommand(string.IsNullOrEmpty(ssid) ? cmd : cmd + " " + Convert.ToHexString(System.Text.Encoding.UTF8.GetBytes(ssid)));
        Publish();
    }

    /// <summary>Joins `ssid` (from the last scan). Returns "" or why not.</summary>
    public string WifiConnect(string ssid, string? password, bool? autoConnect, bool hidden) =>
        WifiConnect(ssid, password, autoConnect, hidden, JoinOrigin.Manual);

    string WifiConnect(string ssid, string? password, bool? autoConnect, bool hidden, JoinOrigin origin,
                       bool retry = false, WifiSelection.AccessPoint? automaticTarget = null)
    {
        // Only Concurrent runs a station beside AirDrop. In On demand / Off the
        // relaunch below joined nothing and only restarted the radio, which
        // reset discovery each time the user tried again (#11).
        if (EffectiveMode() != NetMode.Concurrent) return "needs concurrent mode";
        WifiSelection.AccessPoint? target;
        bool hadName;
        bool stale;
        lock (_lock)
        {
            var now = DateTime.UtcNow;
            stale = now - _scanAt > StaleScan;
            var aps = _scan.Where(ap => ap.Ssid == ssid).ToArray();
            hadName = aps.Length != 0;
            if (automaticTarget is not null)
            {
                if (stale || !aps.Contains(automaticTarget) || IsCooling(automaticTarget, now)) return "";
                target = automaticTarget;
            }
            else
                target = WifiSelection.ForName(retry ? aps.Where(ap => !IsCooling(ap, now)) : aps,
                                               _known.SelectionProfile(ssid), password);
        }
        // An iPhone hotspot comes back with a new BSSID: joining the address
        // from an old scan sent every authentication into the void until a later
        // scan happened to catch the new one (#27). Look by name again first.
        var isHidden = hidden || _known.Find(ssid)?.Hidden == true;
        if (target is not null && stale && !_scanBusy && State == RadioState.On)
        {
            lock (_lock) _pendingConnect = (ssid, password, autoConnect, origin, retry);
            WifiScan(ssid);
            return "";
        }
        if (target is null)
        {
            if (hadName)
                return retry ? "no compatible access point ready to retry yet" : "no compatible access point for this network";
            if (_scanBusy)
            {
                if (!isHidden) return "still scanning; try again in a moment";
                // Queue directed scan after the active wildcard scan finishes (#50).
                lock (_lock) _pendingConnect = (ssid, password, autoConnect, origin, retry);
                return "";
            }
            // Not in the list (hidden, or out of the last scan): look for it by name first.
            lock (_lock) _pendingConnect = (ssid, password, autoConnect, origin, retry);
            WifiScan(ssid);
            return "";
        }
        var security = target.Security;
        if (!WifiSelection.IsJoinable(security))
            return security switch
            {
                "802.1x" => "enterprise (802.1X) networks are not supported yet",
                "wep" => "WEP networks are not supported",
                _ => "this network's security is not supported",
            };
        // Enhanced Open (OWE) has no password: the radio sees it in the beacon and encrypts.
        var open = WifiSelection.IsOpen(security);
        string? pass = null, psk = null;
        if (!open)
        {
            if (!string.IsNullOrEmpty(password))
            {
                if (password.Length == 64 && password.All(Uri.IsHexDigit)) psk = password;
                else if (password.Length is >= 8 and <= 63) pass = password;
                else return "the password must be 8 to 63 characters";
            }
            else if (_known.Find(ssid) is { HasKey: true } k)
            {
                pass = k.Passphrase;
                psk = k.PskHex;
            }
            else return "password needed";
            // SAE works from the password itself; a stored 64-digit key cannot do WPA3.
            if (security == "wpa3-sae" && pass is null) return "WPA3 needs the Wi-Fi password itself, not a 64-digit key";
        }
        var net = new Wlan.Network(ssid, target.Bssid, target.Channel, security, pass, psk, open);
        lock (_lock)
        {
            if (origin == JoinOrigin.Automatic &&
                (!CanAutoJoin() || !_scan.Contains(target) || IsCooling(target, DateTime.UtcNow) ||
                 _known.SelectionProfile(ssid) is not { AutoConnect: true })) return "";
            if (origin != JoinOrigin.Automatic)
            {
                _known.Remember(ssid, pass, psk, open, autoConnect, hidden: hidden ? true : null);
                bool awaitManualSuccess = origin == JoinOrigin.Manual &&
                    (!retry || _pendingManualSuccess is not null && ReferenceEquals(_pendingManualSuccess, _net));
                _pendingManualSuccess = awaitManualSuccess ? net : null;
            }
            _net = net;
            _joinOrigin = origin;
            _wifiJoinQueued = true;
            _pendingConnect = null;
            _userChose = true;
            _userOff = false;
            _wifiError = "";
            _staSsid = ssid;
        }
        Say($"Wi-Fi: joining \"{ssid}\" ({(origin == JoinOrigin.Automatic ? "auto-connect" : origin == JoinOrigin.Manual ? "chosen in nekodrop" : "inherited from Windows")}, channel {net.Channel})");
        _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel, net));
        return "";
    }

    public void WifiDisconnect()
    {
        lock (_lock)
        {
            _userOff = true;
            _userChose = false;
            _net = null;
            _pendingManualSuccess = null;
            _wifiJoinQueued = false;
            _pendingConnect = null;
            _staSsid = "";
            _wifi = "";
        }
        Say("Wi-Fi: disconnected by the user");
        Publish();
        _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
    }

    public void WifiForget(string ssid)
    {
        _known.Forget(ssid);
        if (ssid == _staSsid) WifiDisconnect();
        else Publish();
    }

    public void WifiAuto(string ssid, bool on)
    {
        _known.SetAutoConnect(ssid, on);
        if (on) lock (_lock) _hiddenProbed.Remove(ssid);
        Publish();
    }

    bool CanAutoJoin() => _wanted && State == RadioState.On && !_userOff && _wifi != "connected" &&
                          !_wifiJoinQueued && _pendingConnect is null && EffectiveMode() == NetMode.Concurrent;

    /// <summary>After a scan: a pending connect by name, or auto-connect when Wi-Fi is down.</summary>
    void AfterScan()
    {
        (string Ssid, string? Password, bool? Auto, JoinOrigin Origin, bool Retry)? pending;
        lock (_lock)
        {
            pending = _pendingConnect;
            _pendingConnect = null;
        }
        if (pending is { } pc)
        {
            bool found;
            lock (_lock) found = _scan.Any(ap => ap.Ssid == pc.Ssid);
            // A scan hears a DFS channel's access point only by its beacons, and
            // one scan in three missed the MT7902's 5 GHz one: look twice more.
            if (!found && _pendingScans < 2)
            {
                _pendingScans++;
                lock (_lock) _pendingConnect = pc;
                WifiScan(pc.Ssid);
                return;
            }
            _pendingScans = 0;
            var why = found ? WifiConnect(pc.Ssid, pc.Password, pc.Auto, false, pc.Origin, pc.Retry)
                            : $"\"{pc.Ssid}\" was not found";
            if (why != "") lock (_lock) _wifiError = why;
            Publish();
            return;
        }
        lock (_lock) if (!CanAutoJoin()) return;
        // Auto-join can restart the radio. Don't do that to an AirDrop transfer
        // that started during the scan's home windows (#54/#56).
        if (_profile.Backend == RadioBackendKind.MtkRadio &&
            DateTime.UtcNow - _lastUse <= TimeSpan.FromSeconds(10)) return;
        if (DateTime.UtcNow - _wifiDownSince < TimeSpan.FromSeconds(20)) return;
        // Beside a wired connection Wi-Fi is joined only from the Wi-Fi window (issue #1).
        if (Wlan.WiredUp()) return;
        WifiSelection.AccessPoint? best;
        lock (_lock)
        {
            if (!CanAutoJoin()) return;
            var now = DateTime.UtcNow;
            best = WifiSelection.Automatic(_scan, _known.SelectionProfile, ap => IsCooling(ap, now));
        }
        if (best is null)
        {
            string? nextHidden = null;
            lock (_lock)
            {
                if (!CanAutoJoin()) return;
                nextHidden = _known.All()
                    .FirstOrDefault(k => k is { HasKey: true, AutoConnect: true, Hidden: true } && !_hiddenProbed.Contains(k.Ssid))
                    ?.Ssid;
                if (nextHidden is not null) _hiddenProbed.Add(nextHidden);
            }
            if (nextHidden is not null) WifiScan(nextHidden, background: true);
            return;
        }
        WifiConnect(best.Ssid, null, null, false, JoinOrigin.Automatic, automaticTarget: best);
    }

    /// <summary>
    /// The "nekodrop" adapter after any wired one: with the automatic metric both
    /// got 25 and traffic could leave through Wi-Fi (issue #1).
    /// </summary>
    void SetTapMetric()
    {
        foreach (var family in new[] { "ipv4", "ipv6" })
        {
            try
            {
                var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "netsh.exe"))
                {
                    UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardOutput = true, RedirectStandardError = true,
                };
                foreach (var a in new[] { "interface", family, "set", "interface", "nekodrop", $"metric={TapMetric}" })
                    psi.ArgumentList.Add(a);
                using var p = Process.Start(psi);
                if (p is null) continue;
                // Read before waiting: a full pipe would block netsh.
                var output = p.StandardOutput.ReadToEndAsync();
                var errors = p.StandardError.ReadToEndAsync();
                p.WaitForExit(10000);
                if (p is { HasExited: true, ExitCode: not 0 })
                {
                    // What netsh said, so the log tells a missing adapter from a refusal (issue #3).
                    var why = string.Join(" ", (output.Wait(1000) ? output.Result : "") + " " + (errors.Wait(1000) ? errors.Result : ""))
                        .Replace("\r", " ").Replace("\n", " ").Trim();
                    Say($"could not set the nekodrop adapter's {family} metric (netsh exit {p.ExitCode}): {why}");
                }
            }
            catch (Exception e) { Say("could not set the nekodrop adapter's metric: " + e.Message); }
        }
    }

    /// <summary>
    /// High enough that wired Ethernet always wins the default route: Windows
    /// often assigns RouteMetric 256 + InterfaceMetric 25 = 281 to Ethernet,
    /// which lost to nekodrop's earlier 75 + 75 = 150 (issue #11).
    /// </summary>
    const int TapMetric = 500;

    /// <summary>The card runs nekodrop's kernel Wi-Fi driver (nekodrop-wlan.sys).</summary>
    static bool KernelDriver => Card.Find() is { } k && Card.ByKernelDriver(k);

    void EnsureVendorDriver(bool reconnect = false, bool resetCard = true, int epoch = -1)
    {
        var card = Card.Find();
        // Ours (running or not), or left with no driver by a failed bind: the vendor's driver goes back on.
        var ours = card is not null && (Card.BoundToProxy(card) || Card.ByKernelDriver(card));
        if (card is null || (!ours && card.InfPath != ""))
        {
            if (reconnect)
            {
                var instance = card?.InstanceId ?? "";
                if (card is { InfPath: not "" })
                {
                    if (resetCard)
                    {
                        try
                        {
                            Card.Restart(card.InstanceId);
                            Say($"Wi-Fi: restarted the Wi-Fi device ({Path.GetFileName(card.InfPath)})");
                        }
                        catch (Exception e) { Say("Wi-Fi: " + e.Message); }
                    }
                    else
                    {
                        Say("Wi-Fi: skipped the PnP restart after a clean radio stop");
                    }
                }
                ReconnectWindowsWifi(instance, resetOnFailure: true, epoch: epoch);
            }
            return;
        }
        var vendor = card.Profile.VendorName;
        if (_cfg.VendorInf == "" || !File.Exists(_cfg.VendorInf))
        {
            Say($"cannot give the card back: {vendor}'s INF is not known; run give-card.ps1");
            return;
        }
        var fullInf = Path.GetFullPath(_cfg.VendorInf);
        var infDir = Path.GetFullPath(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "INF"));
        var infRoot = infDir.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (!fullInf.StartsWith(infRoot, StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(Path.GetDirectoryName(fullInf), infDir.TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase) ||
            !System.Text.RegularExpressions.Regex.IsMatch(Path.GetFileName(fullInf), @"^[a-z0-9][a-z0-9._-]*\.inf$", System.Text.RegularExpressions.RegexOptions.IgnoreCase) ||
            Path.GetFileName(fullInf).Contains("..", StringComparison.Ordinal) ||
            (File.GetAttributes(fullInf) & FileAttributes.ReparsePoint) != 0)
        {
            Say($"refusing untrusted VendorInf path: {_cfg.VendorInf}");
            return;
        }
        // An earlier build recorded the proxy's own INF as Intel's (issue #12):
        // binding it again would keep Windows without Wi-Fi.
        try
        {
            if (File.ReadAllText(fullInf).Contains(card.Profile.ProxyInfStem, StringComparison.OrdinalIgnoreCase))
            {
                Say($"{Path.GetFileName(fullInf)} on record is nekodrop's own driver, not {vendor}'s; " +
                    $"run Start menu > nekodrop - restore Windows Wi-Fi to get {vendor}'s driver back");
                lock (_lock) { _cfg.VendorInf = ""; _cfg.Save(); }
                return;
            }
        }
        catch (IOException) { }
        try
        {
            Say($"giving the card back to {Path.GetFileName(fullInf)}");
            var bindWatch = Stopwatch.StartNew();
            Card.Bind(card.HardwareId, fullInf);
            Say($"Wi-Fi: driver binding returned after {bindWatch.ElapsedMilliseconds} ms");
            // Once the vendor driver's WLAN interface is up, put back the saved networks it lacks.
            // On a user-requested give-back, also join the default network instead of leaving
            // Windows disconnected until the user clicks the Wi-Fi icon.
            if (reconnect) ReconnectWindowsWifi(card.InstanceId, resetOnFailure: true, epoch: epoch);
            else _ = Task.Run(() => WlanBackup.Restore(Say, TimeSpan.FromSeconds(60)));
        }
        catch (Exception e) { Say("giving the card back failed: " + e.Message); }
    }

    /// <summary>Gives the card back, then asks Windows to join its default saved Wi-Fi.</summary>
    void ReconnectWindowsWifi(string instanceId = "", bool resetOnFailure = false, int epoch = -1)
    {
        var preferred = _net?.Ssid;
        if (string.IsNullOrWhiteSpace(preferred))
        {
            // Taking the card can make Wlan.Current fail on some adapters even
            // though Windows was connected a moment ago (the log then says
            // "cannot read the current connection"). Without a preferred SSID
            // ConnectDefault picks the first saved auto-connect profile, which
            // can be an out-of-range network instead of the one the user was
            // actually using. Fall back to the most recently used saved
            // network instead, which is what Windows would have chosen.
            var remembered = _known.All();
            preferred = remembered.FirstOrDefault(e => e.AutoConnect && !e.Hidden)?.Ssid
                     ?? remembered.FirstOrDefault(e => e.AutoConnect)?.Ssid;
            if (!string.IsNullOrEmpty(preferred))
                Say($"Wi-Fi: no current network read; preferring the last used \"{preferred}\"");
        }
        if (_userOff)
        {
            Say("Wi-Fi: not reconnecting after AirDrop: Wi-Fi was disconnected in nekodrop");
            return;
        }
        CancellationToken token;
        lock (_lock) token = _wifiCancel.Token;
        var task = Task.Run(() =>
        {
            var ok = WlanBackup.RestoreAndConnect(Say, preferred, token);
            // A newer start/stop has taken over the card. Never run a PnP
            // restart from an old session: it would reset the card while the
            // next session is binding it.
            if (token.IsCancellationRequested || (epoch >= 0 && epoch != Volatile.Read(ref _wifiEpoch))) return;
            if (ok || !resetOnFailure || string.IsNullOrEmpty(instanceId)) return;
            // Fast path failed to reach IPv4 Internet. Keep the clean-stop
            // shortcut for the common case, but do not leave the card in a
            // degraded state: retry once through the full PnP reset.
            Say("Wi-Fi: fast path did not reach IPv4 Internet; retrying with a PnP device restart");
            try
            {
                var card = Card.Find();
                if (card is { InfPath: not "" } && !Card.BoundToProxy(card) && !Card.ByKernelDriver(card))
                {
                    Card.Restart(card.InstanceId);
                    if (token.IsCancellationRequested || (epoch >= 0 && epoch != Volatile.Read(ref _wifiEpoch))) return;
                    WlanBackup.RestoreAndConnect(Say, preferred, token);
                }
            }
            catch (Exception e) { Say("Wi-Fi: PnP fallback failed: " + e.Message); }
        });
        lock (_lock) _wifiReconnect = task;
    }

    Process Spawn(string exe, IEnumerable<string> args, Action<string> onLine)
    {
        var psi = new ProcessStartInfo(exe)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            RedirectStandardInput = true,
            WorkingDirectory = Path.GetDirectoryName(exe)!,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        var p = Process.Start(psi) ?? throw new InvalidOperationException("cannot start " + exe);
        p.ErrorDataReceived += (_, e) => { if (e.Data is not null) onLine(e.Data); };
        p.OutputDataReceived += (_, e) => { if (e.Data is not null) onLine(e.Data); };
        p.BeginErrorReadLine();
        p.BeginOutputReadLine();
        return p;
    }

    /// <summary>
    /// Stops the radio and awdld. Returns true only when the Intel radio
    /// exited through `quit` with a clean IwxDevice::stop(); a hard kill, a
    /// timeout or a non-zero radio exit means the caller must keep the PnP
    /// restart as the fallback.
    /// </summary>
    bool KillDaemons()
    {
        bool clean = true;
        foreach (var p in new[] { _awdld, _radio })
        {
            try
            {
                if (p is { HasExited: false })
                {
                    // MT7921/MT7922 must finish MtkDevice::close() -> dma_disable(true) +
                    // wfsys_reset() BEFORE the process exits and frees its AWE physical pages,
                    // otherwise live WFDMA or stale firmware state can corrupt memory or wedge
                    // the vendor driver on reboot (#54, #72).
                    // IwxDevice::stop() similarly resets the Intel firmware and DMA. A hard
                    // kill skips it and leaves the card in a degraded AWDL state; Windows then
                    // joins Wi-Fi but transfers at a fraction of the link rate.
                    if (ReferenceEquals(p, _radio) &&
                        _profile.Backend is RadioBackendKind.MtkRadio or RadioBackendKind.Iwx)
                    {
                        try
                        {
                            p.StandardInput.WriteLine("quit");
                            p.StandardInput.Flush();
                            p.StandardInput.Close();
                        }
                        catch (Exception) { }
                        if (p.WaitForExit(6000))
                        {
                            if (ReferenceEquals(p, _radio) && _profile.Backend == RadioBackendKind.Iwx)
                                clean &= p.ExitCode == 0;
                            continue;
                        }
                        clean = false;
                        Say($"WARNING: {_profile.Backend} radio did not exit within 6 s after quit; forcing termination");
                    }
                    p.Kill();
                    p.WaitForExit(5000);
                    if (ReferenceEquals(p, _radio)) clean = false;
                }
                else if (ReferenceEquals(p, _radio) && p is not null)
                {
                    // It exited before Stop() reached it. A clean radio exit is
                    // only trustworthy when the process reported success.
                    clean &= p.ExitCode == 0;
                }
            }
            catch (InvalidOperationException) { if (ReferenceEquals(p, _radio)) clean = false; }
        }
        _awdld = _radio = null;
        return clean;
    }

    public void Dispose()
    {
        _cts.Cancel();
        Stop("service stopped", giveBack: EffectiveMode() != NetMode.Concurrent);
    }
}
