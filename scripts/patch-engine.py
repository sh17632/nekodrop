# Applies nekodrop's changes to the staged copy of winawdl's AirDrop engine.
# Usage: python patch-engine.py <staged airdrop folder>
#
# - A stable mDNS instance name (NEKODROP_SERVICE_ID, kept by the client), so
#   an engine restart (a new awdl0 after the radio restarts) shows up on the
#   iPhone as the same device instead of a second one.
# - Spare receiver ports: when 8771 is held by another program the receiver
#   takes the next free one of 8772-8779 (advertised in the mDNS SRV record,
#   so the iPhone follows), instead of waiting for 8771 forever.
# - Sender presents no TLS client certificate.  An iPhone rejects a self-signed
#   one with SSLV3_ALERT_BAD_CERTIFICATE on both /Discover and /Ask; AirDrop
#   receivers accept connections without a client cert, and identity rides in
#   the plist record instead.  Without this the send direction cannot work.
import pathlib
import sys

NL = chr(10)
HELPER = NL.join([
    'SPARE_PORTS = range(8771, 8780)  # nekodrop: the firewall rule allows these',
    '',
    '',
    'def _bind_spare(settings, watcher):',
    '    ports = [settings.port] + [p for p in SPARE_PORTS if p != settings.port]',
    '    for port in ports:',
    '        try:',
    '            return receiver.Receiver(port, settings, watcher)',
    '        except OSError as error:',
    "            blocked = (error.errno in (errno.EADDRINUSE, errno.EACCES)",
    "                       or getattr(error, 'winerror', None) in (10013, 10048))",
    '            if not blocked or port == ports[-1]:',
    '                raise',
    "            logging.warning('port %d unavailable (%s), trying the next', port, error)",
    '',
    '',
    '',
])

engine = pathlib.Path(sys.argv[1]) / 'engine.py'
text = engine.read_text(encoding='utf-8')
changed = False

old = 'service_id = secrets.token_hex(6)'
new = "service_id = os.environ.get('NEKODROP_SERVICE_ID') or secrets.token_hex(6)"
if new not in text:
    if old not in text:
        sys.exit('engine.py: the service id line moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    if NL + 'import os' + NL not in text:
        text = text.replace(NL + 'import secrets' + NL, NL + 'import os' + NL + 'import secrets' + NL, 1)
    changed = True

old = 'server = receiver.Receiver(self.settings.port, self.settings, self.watcher)'
new = 'server = _bind_spare(self.settings, self.watcher)'
if new not in text:
    if old not in text:
        sys.exit('engine.py: the receiver bind line moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    text = text.replace(NL + 'class ', NL + HELPER + 'class ', 1)
    changed = True

# #45: the --outdir default called valid_dir('~/Downloads') while the parser was
# built, so with no Downloads folder in the profile (deleted after choosing
# another receive folder) the engine died at start every few seconds, even
# though the client always passes --outdir.  A string default goes through
# `type` only when --outdir is missing, so standalone runs behave as before.
old = "default=receiver.valid_dir('~/Downloads'))"
new = "default='~/Downloads')  # nekodrop: checked only when --outdir is missing (#45)"
if new not in text:
    if text.count(old) != 1:
        sys.exit('engine.py: the --outdir default moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    changed = True

if changed:
    engine.write_text(text, encoding='utf-8')
print('engine patched')

# sender.py: drop the client certificate from the TLS context.  The receiver
# rejects our self-signed cert; a sender without an Apple-issued identity is
# expected to present none at all.
sender = pathlib.Path(sys.argv[1]) / 'sender.py'
text = sender.read_text(encoding='utf-8')
old = NL.join([
    '    certificate, key = identity.ensure_self_signed(keys_dir)',
    '    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)',
    '    # A receiver visible to everyone presents a certificate nothing vouches',
    '    # for, and takes ours the same way.',
    '    context.check_hostname = False',
    '    context.verify_mode = ssl.CERT_NONE',
    '    context.load_cert_chain(certificate, key)',
])
new = NL.join([
    '    # Present no client certificate.  An iPhone in Everyone mode rejects a',
    '    # self-signed one with SSLV3_ALERT_BAD_CERTIFICATE on every connection.',
    '    # AirDrop receivers accept a connection with no client cert; identity',
    '    # is carried in the plist record, not in the TLS client certificate.',
    '    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)',
    '    context.check_hostname = False',
    '    context.verify_mode = ssl.CERT_NONE',
])
if new not in text:
    if old not in text:
        sys.exit('sender.py: the client_context body moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    sender.write_text(text, encoding='utf-8')
    print('sender patched')

# engine.py: follow a receiver that announces itself again.  After a transfer
# in either direction an iPhone withdraws its AirDrop service and comes back
# under a new instance (#4: peer_gone, then WinError 10061 on its old port,
# then every retry failed at once with "gone").  A send to a peer that is gone,
# or refused on its old port, now waits up to FOLLOW_SECONDS for a receiver
# with the same name and sends there.
FOLLOW = [
    (
        '        self._shy_said = 0' + NL,
        '        self._shy_said = 0' + NL +
        '        self._gone_names = {}  # nekodrop: key -> name of receivers that went away' + NL,
    ),
    (
        NL.join([
            '            for key in [key for key in self._peers if key not in located]:',
            '                if self._peers.pop(key).name:',
            '                    gone.append(key)',
        ]),
        NL.join([
            '            for key in [key for key in self._peers if key not in located]:',
            '                old = self._peers.pop(key)',
            '                if old.name:',
            '                    self._gone_names[key] = old.name',
            '                    gone.append(key)',
        ]),
    ),
    (
        '    def link(self, scope):' + NL,
        NL.join([
            '    def find(self, key, wait):',
            '        """nekodrop: the receiver under `key`, or the one that came back with',
            '        its name, waiting up to `wait` seconds for it."""',
            '        deadline = time.monotonic() + wait',
            '        while True:',
            '            with self._lock:',
            '                peer = self._peers.get(key)',
            '                if peer and peer.name:',
            '                    return peer',
            '                name = self._gone_names.get(key)',
            '                if name:',
            '                    for other in self._peers.values():',
            '                        if other.name == name:',
            '                            return other',
            '            if time.monotonic() >= deadline or not self.wanted.is_set():',
            '                return None',
            '            time.sleep(0.5)',
            '',
            '    def went_away(self, key):',
            '        with self._lock:',
            '            return key in self._gone_names',
            '',
            '    def link(self, scope):',
            '',
        ]),
    ),
    (
        NL.join([
            '        peer = self.looking.peer(peer_key)',
            '        if not peer or scope is None:',
            "            failed('gone')",
            '            return',
        ]),
        NL.join([
            '        peer = self.looking.peer(peer_key)',
            '        # None for a receiver that went away: looked for again in _run.',
            '        if (not peer and not self.looking.went_away(peer_key)) or scope is None:',
            "            failed('gone')",
            '            return',
        ]),
    ),
    (
        NL.join([
            '        watch = SendWatch(self.events, key, sender.archive_bytes(files))',
            '        with self._lock:',
        ]),
        NL.join([
            '        watch = SendWatch(self.events, key, sender.archive_bytes(files))',
            '        watch.peer_key = peer_key',
            '        with self._lock:',
        ]),
    ),
    (
        NL.join([
            "        watch.state('asking')",
            '        try:',
            '            outcome = sender.deliver(peer.address, peer.port, scope, self.context,',
            '                                     self.settings.name, files, watch)',
        ]),
        NL.join([
            "        watch.state('asking')",
            '        try:',
            '            if peer is None:',
            '                peer = self.looking.find(watch.peer_key, FOLLOW_SECONDS)',
            '                if peer is None:',
            "                    watch.state('failed', reason='gone')",
            '                    return',
            '            try:',
            '                outcome = sender.deliver(peer.address, peer.port, scope, self.context,',
            '                                         self.settings.name, files, watch)',
            '            except (ConnectionRefusedError, TimeoutError):',
            '                # Refused or silent before answering: the receiver may be',
            '                # announcing itself again on another port.',
            '                if watch.reached or watch.cancelled:',
            '                    raise',
            '                fresh = self.looking.find(watch.peer_key, FOLLOW_SECONDS)',
            '                if fresh is None or (fresh.address, fresh.port) == (peer.address, peer.port):',
            '                    raise',
            "                logging.info('receiver came back on port %d; sending again', fresh.port)",
            '                peer = fresh',
            '                outcome = sender.deliver(peer.address, peer.port, scope, self.context,',
            '                                         self.settings.name, files, watch)',
        ]),
    ),
    (
        'BIND_RETRY_SECONDS = 5' + NL,
        'BIND_RETRY_SECONDS = 5' + NL +
        'FOLLOW_SECONDS = 15  # nekodrop: wait for a receiver that announces itself again' + NL,
    ),
]
engine = pathlib.Path(sys.argv[1]) / 'engine.py'
text = engine.read_text(encoding='utf-8')
changed = False
for old, new in FOLLOW:
    if new in text:
        continue
    if text.count(old) != 1:
        sys.exit('engine.py: a line the receiver-follow patch needs moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    changed = True
if changed:
    engine.write_text(text, encoding='utf-8')
    print('engine: follows receivers that come back')

# transfer.py: throughput improvements for all cards (Intel, Realtek, ...):
# - 64 KiB DVZip blocks (4x fewer block headers / TLS records, matches AirDrop spec).
# - Adaptive incompressible-stream bypass: after 4 consecutive blocks do not shrink
#   under zlib level 1 (photos, HEIC, MP4, MOV, ZIP), emit DVZIP_STORED directly for
#   the next 32 blocks (2 MiB) before re-probing one block, cutting sender CPU ~90%
#   on media while still compressing text/mixed archives.
# - 0.5 s end-marker wait (down from 2 s) for iPhone videos/Live Photos that omit the
#   final chunked 0-length terminator.
transfer_path = pathlib.Path(sys.argv[1]) / 'transfer.py'
text = transfer_path.read_text(encoding='utf-8')
TRANSFER_PATCHES = [
    ('END_MARKER_WAIT_SECONDS = 2' + NL, 'END_MARKER_WAIT_SECONDS = 0.5  # nekodrop: faster video/Live Photo completion' + NL),
    ('IO_CHUNK_BYTES = 64 * KIB' + NL, 'IO_CHUNK_BYTES = 512 * KIB  # nekodrop: 512 KiB I/O chunks' + NL),
    ('DVZIP_BLOCK_BYTES = 16 * KIB' + NL, 'DVZIP_BLOCK_BYTES = 64 * KIB  # nekodrop: 64 KiB DVZip blocks' + NL),
    ("        return cand, os.fdopen(fd, 'wb')" + NL, "        return cand, os.fdopen(fd, 'wb', buffering=4 * MIB)  # nekodrop: 4 MiB output buffer" + NL),
    (
        NL.join([
            'def encode_dvzip(pieces):',
            '    """Yield the stream in `pieces` as DVZip blocks, each compressed only if',
            '    that makes it smaller."""',
            '    held = bytearray()',
            '    for piece in pieces:',
            '        held += piece',
            '        while len(held) >= DVZIP_BLOCK_BYTES:',
            '            yield _dvzip_block(bytes(held[:DVZIP_BLOCK_BYTES]))',
            '            del held[:DVZIP_BLOCK_BYTES]',
            '    if held:',
            '        yield _dvzip_block(bytes(held))',
        ]),
        NL.join([
            'def _dvzip_stored(data):',
            '    return (len(data) | DVZIP_STORED).to_bytes(4, \'big\') + data',
            '',
            '',
            'def encode_dvzip(pieces):',
            '    """Yield the stream in `pieces` as DVZip blocks, each compressed only if',
            '    that makes it smaller. Skips zlib on sustained incompressible runs (e.g.',
            '    photos/videos) and re-probes periodically so mixed archives still shrink."""',
            '    held = bytearray()',
            '    pos = 0',
            '    incomp_streak = 0',
            '    skip_left = 0',
            '    for piece in pieces:',
            '        held += piece',
            '        while len(held) - pos >= DVZIP_BLOCK_BYTES:',
            '            block = bytes(memoryview(held)[pos:pos + DVZIP_BLOCK_BYTES])',
            '            pos += DVZIP_BLOCK_BYTES',
            '            if skip_left > 0:',
            '                skip_left -= 1',
            '                yield _dvzip_stored(block)',
            '            else:',
            '                out = _dvzip_block(block)',
            '                if int.from_bytes(out[:4], \'big\') & DVZIP_STORED:',
            '                    incomp_streak += 1',
            '                    if incomp_streak >= 4:',
            '                        skip_left = 32',
            '                else:',
            '                    incomp_streak = 0',
            '                yield out',
            '        if pos >= 4 * DVZIP_BLOCK_BYTES:',
            '            del held[:pos]',
            '            pos = 0',
            '    if pos < len(held):',
            '        tail = bytes(memoryview(held)[pos:])',
            '        yield _dvzip_stored(tail) if skip_left > 0 else _dvzip_block(tail)',
        ]),
    ),
]
changed = False
for old, new in TRANSFER_PATCHES:
    if new in text:
        continue
    if text.count(old) != 1:
        sys.exit(f'transfer.py: expected line moved ({old[:48]!r}); update patch-engine.py')
    text = text.replace(old, new, 1)
    changed = True
if changed:
    transfer_path.write_text(text, encoding='utf-8')
    print('transfer: 64 KiB DVZip + adaptive stored bypass + 0.5 s tail wait')

# sender.py & receiver.py: 4 MiB socket buffers and TCP_NODELAY so bursty AWDL
# presence slots do not stall behind Winsock's 64 KiB default TCP window.
sender_path = pathlib.Path(sys.argv[1]) / 'sender.py'
text = sender_path.read_text(encoding='utf-8')
old_s = NL.join([
    '        try:',
    '            sock.settimeout(self.timeout)',
    '            sock.connect((self.host, self.port, 0, self.scope))',
])
new_s = NL.join([
    '        try:',
    '            sock.settimeout(self.timeout)',
    '            for opt in ((socket.IPPROTO_TCP, socket.TCP_NODELAY, 1),',
    '                        (socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024),',
    '                        (socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)):',
    '                try:',
    '                    sock.setsockopt(*opt)',
    '                except OSError:',
    '                    pass',
    '            sock.connect((self.host, self.port, 0, self.scope))',
])
if 'SO_SNDBUF, 4 * 1024 * 1024' not in text:
    if text.count(old_s) != 1:
        sys.exit('sender.py: Connection.connect moved; update patch-engine.py')
    text = text.replace(old_s, new_s, 1)
    sender_path.write_text(text, encoding='utf-8')
    print('sender: 4 MiB socket buffers + TCP_NODELAY')

text = sender_path.read_text(encoding='utf-8')
sender_io_pairs = [
    ('IO_BLOCK_BYTES = 64 * 1024' + NL, 'IO_BLOCK_BYTES = 512 * 1024  # nekodrop: 512 KiB send reads' + NL),
    ("    with open(path, 'rb') as source:" + NL, "    with open(path, 'rb', buffering=4 * MIB) as source:" + NL),
]
changed = False
for old_io, new_io in sender_io_pairs:
    if new_io in text:
        continue
    if text.count(old_io) != 1:
        sys.exit(f'sender.py: {old_io[:48]!r} moved; update patch-engine.py')
    text = text.replace(old_io, new_io, 1)
    changed = True
if changed:
    sender_path.write_text(text, encoding='utf-8')
    print('sender: 512 KiB reads + 4 MiB file buffer')

text = sender_path.read_text(encoding='utf-8')
if "self._phase = 'tcp'" not in text:
    phase_pairs = [
        (
            NL.join([
                '        self.scope = scope',
                '        self.context = context',
            ]),
            NL.join([
                '        self.scope = scope',
                '        self.context = context',
                "        self._phase = 'init'",
            ]),
        ),
        (
            NL.join([
                '            sock.connect((self.host, self.port, 0, self.scope))',
                '            self.sock = self.context.wrap_socket(sock)',
            ]),
            NL.join([
                "            self._phase = 'tcp'",
                '            sock.connect((self.host, self.port, 0, self.scope))',
                "            self._phase = 'tls'",
                '            self.sock = self.context.wrap_socket(sock)',
                "            self._phase = 'http'",
            ]),
        ),
        (
            '        raise Unreachable(str(error) or type(error).__name__) from None',
            "        raise Unreachable(f'{connection._phase}:{port} {error or type(error).__name__}') from None",
        ),
    ]
    for old_p, new_p in phase_pairs:
        if text.count(old_p) != 1:
            sys.exit(f'sender.py: {old_p[:50]!r} moved; update patch-engine.py')
        text = text.replace(old_p, new_p, 1)
    sender_path.write_text(text, encoding='utf-8')
    print('sender: phase-tagged Unreachable diagnostics')

receiver_path = pathlib.Path(sys.argv[1]) / 'receiver.py'
text = receiver_path.read_text(encoding='utf-8')
old_r = NL.join([
    '    def setup(self):',
    '        self.request.settimeout(transfer.READ_IDLE_TIMEOUT_SECONDS)',
])
new_r = NL.join([
    '    rbufsize = 512 * 1024  # nekodrop: 512 KiB buffered TLS reads',
    '',
    '    def setup(self):',
    '        self.request.settimeout(transfer.READ_IDLE_TIMEOUT_SECONDS)',
    '        for opt in ((socket.IPPROTO_TCP, socket.TCP_NODELAY, 1),',
    '                    (socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024),',
    '                    (socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)):',
    '            try:',
    '                self.request.setsockopt(*opt)',
    '            except OSError:',
    '                pass',
])
if new_r not in text:
    if text.count(old_r) != 1:
        sys.exit('receiver.py: Handler.setup moved; update patch-engine.py')
    text = text.replace(old_r, new_r, 1)
    receiver_path.write_text(text, encoding='utf-8')
    print('receiver: 4 MiB socket buffers + 512 KiB rbufsize + TCP_NODELAY')


# Contacts Only: nekodrop_identity.py (copied beside the engine by
# build-installer.ps1) gives the receiver and the sender an Apple ID identity
# when the client set NEKODROP_IDENTITY: the Apple-signed certificate on TLS,
# the validation record in /Discover and /Ask, and a Contacts Only filter on
# the receiver (NEKODROP_RECEIVE=contacts). Without one nothing changes.
# The hooks below import it, so the patch brings it along: engine-test ran the
# patched engine without it and every test module of receiver/sender/engine
# failed to import (ModuleNotFoundError) from 2.1.0 on.
_identity = pathlib.Path(__file__).resolve().parent.parent / 'installer' / 'nekodrop_identity.py'
(pathlib.Path(sys.argv[1]) / 'nekodrop_identity.py').write_bytes(_identity.read_bytes())


def _hook(name, pairs, mark):
    path = pathlib.Path(sys.argv[1]) / name
    text = path.read_text(encoding='utf-8')
    if mark in text:
        return
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f'{name}: {old[:60]!r} moved; update patch-engine.py')
        text = text.replace(old, new, 1)
    path.write_text(text, encoding='utf-8')
    print(f'{name}: Apple ID identity (Contacts Only)')


_hook('receiver.py', [
    (NL + 'import mdns' + NL, NL + 'import mdns' + NL + 'import nekodrop_identity' + NL),
    (NL.join([
        "        if self.path == '/Discover':",
        '            self.answer(self.identity_plist())',
        '        else:',
        '            self.handle_ask(body)',
    ]), NL.join([
        '        try:',
        "            sender_record = plistlib.loads(body).get('SenderRecordData') if body else None",
        '        except Exception:',
        '            sender_record = None',
        '        if not nekodrop_identity.allowed(sender_record):',
        "            self.reject(401, 'not one of the contacts (Contacts Only)')",
        '            return',
        "        if self.path == '/Discover':",
        '            self.answer(self.identity_plist())',
        '        else:',
        '            self.handle_ask(body)',
    ])),
    ("            'ReceiverModelName': settings.model," + NL,
     "            'ReceiverModelName': settings.model," + NL + '            **nekodrop_identity.receiver_extra(),' + NL),
    (NL.join([
        '    context.load_cert_chain(certificate, key)',
        '    # Senders present self-signed certificates, so none is asked for.',
    ]), NL.join([
        '    if not nekodrop_identity.load_into(context):',
        '        context.load_cert_chain(certificate, key)',
        '    # Senders present self-signed certificates, so none is asked for.',
    ])),
], 'import nekodrop_identity')

_hook('sender.py', [
    (NL + 'import mdns' + NL, NL + 'import mdns' + NL + 'import nekodrop_identity' + NL),
    (NL.join([
        '    context.verify_mode = ssl.CERT_NONE',
        '    return context',
    ]), NL.join([
        '    context.verify_mode = ssl.CERT_NONE',
        '    # With an Apple ID identity the Apple-signed certificate goes on: an',
        '    # iPhone takes that one (a self-signed one it refuses, see above).',
        '    nekodrop_identity.load_into(context)',
        '    return context',
    ])),
    ("post_plist(connection, '/Discover', {})", "post_plist(connection, '/Discover', nekodrop_identity.sender_body({}))"),
    ("        'SenderModelName': DEFAULT_MODEL," + NL,
     "        'SenderModelName': DEFAULT_MODEL," + NL + '        **nekodrop_identity.sender_extra(),' + NL),
], 'import nekodrop_identity')

# macOS 15 can follow the advertised SRV target with an IPv4 A query (#51,
# PR #55). awdl0 transports IPv6 only, so answer for the name we own with an
# explicit negative record, not a Windows APIPA address the peer cannot use.
# RFC 6762 §6.1: synthesized mDNS NSEC must not set the NSEC bit itself.
mdns = pathlib.Path(sys.argv[1]) / 'mdns.py'
text = mdns.read_text(encoding='utf-8')
if 'def address_response(self, types):' not in text:
    pairs = [
        (
            'TYPE_PTR, TYPE_TXT, TYPE_AAAA, TYPE_SRV, TYPE_NSEC = 12, 16, 28, 33, 47',
            'TYPE_A = 1\nTYPE_PTR, TYPE_TXT, TYPE_AAAA, TYPE_SRV, TYPE_NSEC = 12, 16, 28, 33, 47',
        ),
        (
            '# NSEC type bitmap, window 0, with bits 28 (AAAA) and 47 (NSEC) set.\n'
            'HOST_TYPES = bytes([0, 6, 0, 0, 0, 0x08, 0, 0x01])',
            '# Synthesized mDNS NSEC: window 0, only type 28 (AAAA) exists.\n'
            'HOST_TYPES = bytes([0, 4, 0, 0, 0, 0x08])',
        ),
        (
            """def question_names(packet):
    \"\"\"Lower-cased names a query asks about; empty for responses and for
    anything malformed.\"\"\"
    try:
        _, flags, questions = struct.unpack_from('!HHH', packet)
        if flags & FLAG_RESPONSE:
            return set()
        names = set()
        offset = 12
        for _ in range(questions):
            name, offset = decode_name(packet, offset)
            offset += 4
            names.add(name.lower())
        return names
    except (IndexError, struct.error, ValueError):
        return set()""",
            """def questions_in_with_qu(packet):
    \"\"\"((name, type) set, any_qu) from IN-class mDNS questions.\"\"\"
    try:
        _, flags, questions, _, _, _ = struct.unpack_from('!HHHHHH', packet)
        if flags & FLAG_RESPONSE:
            return set(), False
        found = set()
        any_qu = False
        offset = HEADER_BYTES
        for _ in range(questions):
            name, offset = decode_name(packet, offset)
            rtype, rclass = struct.unpack_from('!HH', packet, offset)
            offset += 4
            if rclass & ~CACHE_FLUSH == CLASS_IN:
                found.add((name.lower(), rtype))
                if rclass & CACHE_FLUSH:
                    any_qu = True
        return found, any_qu
    except (IndexError, struct.error, ValueError):
        return set(), False


def questions_in(packet):
    \"\"\"(name, type) from IN-class mDNS questions, including QU questions.\"\"\"
    return questions_in_with_qu(packet)[0]


def question_names(packet):
    return {name for name, _ in questions_in(packet)}""",
        ),
        (
            '    def response(self, alive=True):',
            """    def address_response(self, types):
        \"\"\"Reply to host address queries, with the requested result in Answer.\"\"\"
        unique = CLASS_IN | CACHE_FLUSH
        aaaa = _record(self.host_name, TYPE_AAAA, unique, TTL_HOST, self.address.packed)
        nsec = _record(self.host_name, TYPE_NSEC, unique, TTL_HOST,
                       encode_name(self.host_name) + HOST_TYPES)
        answers, additional = [], []
        (answers if TYPE_AAAA in types else additional).append(aaaa)
        (answers if TYPE_A in types else additional).append(nsec)
        header = struct.pack('!HHHHHH', 0, RESPONSE_FLAGS, 0, len(answers), 0, len(additional))
        return header + b''.join(answers + additional)

    def response(self, alive=True):""",
        ),
        (
            """    def _send(self, packet):
        try:
            self.sock.sendto(packet, (GROUP, PORT, 0, self.interface_index))
        except OSError as error:
            logging.warning('mdns: send failed: %s', error)""",
            """    def _send(self, packet):
        self._send_to(packet, (GROUP, PORT, 0, self.interface_index))

    def _send_to(self, packet, target):
        try:
            self.sock.sendto(packet, target)
        except OSError as error:
            logging.warning('mdns: send failed: %s', error)""",
        ),
        (
            """        asked = question_names(packet) & self.service.names()
        if asked:
            logging.info('mdns: %s asked for %s', sender[0].split('%')[0],
                         ', '.join(sorted(asked)))
        return bool(asked)""",
            """        names = self.service.names()
        raw_asked, any_qu = questions_in_with_qu(packet)
        asked = {(name, rtype) for name, rtype in raw_asked if name in names}
        self._last_qu_sender = (sender[0], sender[1], 0, self.interface_index) if (asked and any_qu) else None
        if asked:
            logging.info('mdns: %s asked for %s%s', sender[0].split('%')[0],
                         ', '.join(f'{name} (type {rtype})' for name, rtype in sorted(asked)),
                         ' (QU)' if any_qu else '')
        return asked""",
        ),
        (
            """        last_sent = 0.0
        while not self.stopping.is_set():
            asked = self._asked()
            since = time.monotonic() - last_sent
            if (asked and since >= MIN_REPLY_GAP_S) or since >= ANNOUNCE_PERIOD_S:
                self._send(announcement)
                last_sent = time.monotonic()""",
            """        last_announce = last_reply = float('-inf')
        while not self.stopping.is_set():
            now = time.monotonic()
            if now - last_announce >= ANNOUNCE_PERIOD_S:
                self._send(announcement)
                last_announce = now
            asked = self._asked()
            now = time.monotonic()
            if not asked or self.stopping.is_set() or now - last_reply < MIN_REPLY_GAP_S:
                continue
            host = self.service.host_name.lower()
            if all(name == host and rtype in (TYPE_A, TYPE_AAAA) for name, rtype in asked):
                reply = self.service.address_response({rtype for _, rtype in asked})
            else:
                reply = announcement
                last_announce = now
            qu_target = getattr(self, '_last_qu_sender', None)
            if qu_target is not None:
                self._send_to(reply, qu_target)
            self._send(reply)
            last_reply = now""",
        ),
    ]
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f'mdns.py: {old[:60]!r} moved; update patch-engine.py')
        text = text.replace(old, new, 1)
    mdns.write_text(text, encoding='utf-8')
    print('mdns.py: IPv6-only host answers for macOS address queries')

# Link sharing & media format support:
# - Extract http(s) URLs from AirDrop Ask Items even when an app (e.g. Bilibili,
#   Xiaohongshu, WeChat) wraps the link with title text or omits TransferType.links
#   when Files is empty, while keeping is_web_link() strict for safety.
# - Allow opting into original HEIC/HEVC media via NEKODROP_MEDIA_MODE=original
#   (default stays compatible JPEG/H.264).
transfer_path = pathlib.Path(sys.argv[1]) / 'transfer.py'
text = transfer_path.read_text(encoding='utf-8')
if 'def extract_web_links(ask):' not in text:
    old = NL.join([
        'def is_web_link(item):',
        "    return (isinstance(item, str) and item.startswith(('http://', 'https://'))",
        '            and not LINK_BREAKING_CHAR.search(item))',
    ])
    new = NL.join([
        'def is_web_link(item):',
        "    return (isinstance(item, str) and item.startswith(('http://', 'https://'))",
        '            and not LINK_BREAKING_CHAR.search(item))',
        '',
        '',
        "WEB_LINK_RE = re.compile(r'https?://[^\\s<>\"\\'\\x00-\\x1f\\x7f]+')",
        "LINK_TRAILING_PUNCT = '.,;:!?)]}）】》。，；：！？'",
        '',
        '',
        'def extract_web_links(ask):',
        '    """Return safe http(s) URLs carried in an Ask\'s Items (plain URLs, share',
        '    strings such as Bilibili shares containing a URL, or dict values)."""',
        "    items = ask.get('Items') or []",
        '    if not items:',
        '        return []',
        '    found = []',
        '    for item in items:',
        '        if is_web_link(item):',
        '            if item not in found:',
        '                found.append(item)',
        '            continue',
        '        candidates = []',
        '        if isinstance(item, str):',
        '            candidates = WEB_LINK_RE.findall(item)',
        '        elif isinstance(item, dict):',
        '            for val in item.values():',
        '                if isinstance(val, str):',
        '                    candidates.extend(WEB_LINK_RE.findall(val))',
        '        for raw in candidates:',
        '            cleaned = raw.rstrip(LINK_TRAILING_PUNCT)',
        '            if is_web_link(cleaned) and cleaned not in found:',
        '                found.append(cleaned)',
        '    return found',
        '',
        '',
        'def save_preview_icon(ask, key):',
        '    """Write ask[FileIcon] (JPEG/PNG/JP2 preview bytes) to a temp file for',
        '    Windows toast hero previews, pruning old temp previews."""',
        "    icon = ask.get('FileIcon')",
        '    if not isinstance(icon, (bytes, bytearray)) or len(icon) < 16 or len(icon) > 2 * 1024 * 1024:',
        "        return ''",
        "    if icon.startswith(b'\\xff\\xd8\\xff'):",
        "        ext = '.jpg'",
        "    elif icon.startswith(b'\\x89PNG\\r\\n\\x1a\\n'):",
        "        ext = '.png'",
        "    elif icon.startswith(b'\\x00\\x00\\x00\\x0cjP  ') or icon.startswith(b'\\xff\\x4f\\xff\\x51'):",
        "        ext = '.jp2'",
        '    else:',
        "        return ''",
        '    try:',
        "        import tempfile",
        "        root = os.path.join(tempfile.gettempdir(), 'nekodrop-previews')",
        '        os.makedirs(root, exist_ok=True)',
        '        entries = sorted((os.path.join(root, n) for n in os.listdir(root)),',
        '                         key=lambda p: os.path.getmtime(p), reverse=True)',
        '        for stale in entries[24:]:',
        '            with contextlib.suppress(OSError):',
        '                os.unlink(stale)',
        "        out = os.path.join(root, f'ask-{safe_name(str(key), \"icon\")}-{os.getpid()}{ext}')",
        "        with open(out, 'wb') as f:",
        '            f.write(icon)',
        '        return out',
        '    except OSError:',
        "        return ''",
    ])
    if text.count(old) != 1:
        sys.exit('transfer.py: is_web_link moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    transfer_path.write_text(text, encoding='utf-8')
    print('transfer.py: extract_web_links for app share strings')

receiver_path = pathlib.Path(sys.argv[1]) / 'receiver.py'
text = receiver_path.read_text(encoding='utf-8')
if 'def _media_capabilities():' not in text:
    pairs = [
        (
            "MEDIA_CAPABILITIES = json.dumps({'Version': 1}).encode()",
            NL.join([
                "MEDIA_CAPABILITIES = json.dumps({'Version': 1}).encode()",
                'MEDIA_CAPABILITIES_ORIGINAL = json.dumps({',
                "    'Version': 1,",
                "    'Codecs': {'hvc1': {'Profiles': {'HEVCMain': {}, 'HEVCMain10': {}}}},",
                "    'ContainerFormats': {'public.heic': {}, 'public.heif': {}, 'public.avci': {}},",
                '}).encode()',
                '',
                '',
                'def _media_capabilities():',
                "    if os.environ.get('NEKODROP_MEDIA_MODE') == 'original':",
                '        return MEDIA_CAPABILITIES_ORIGINAL',
                '    return MEDIA_CAPABILITIES',
            ]),
        ),
        (
            NL.join([
                '    def answer(self, body):',
                '        self.send_response(200)',
                "        self.send_header('Content-Length', str(len(body)))",
                '        self.end_headers()',
                '        self.wfile.write(body)',
            ]),
            NL.join([
                '    def answer(self, body, close=False):',
                '        self.send_response(200)',
                "        self.send_header('Content-Type', 'application/octet-stream')",
                "        self.send_header('Content-Length', str(len(body)))",
                '        if close:',
                "            self.send_header('Connection', 'close')",
                '            self.close_connection = True',
                '        self.end_headers()',
                '        self.wfile.write(body)',
            ]),
        ),
        (
            "        if self.path == '/Discover':" + NL + '            self.answer(self.identity_plist())',
            "        if self.path == '/Discover':" + NL
            + '            status_code = 200 if nekodrop_identity.is_contact(sender_record) else 100' + NL
            + '            self.answer(self.identity_plist(status_code=status_code))',
        ),
        (
            NL.join([
                '    def identity_plist(self):',
                '        settings = self.server.settings',
                '        return plistlib.dumps({',
                "            'ReceiverMediaCapabilities': MEDIA_CAPABILITIES,",
                "            'ReceiverComputerName': settings.name,",
                "            'ReceiverModelName': settings.model,",
                '            **nekodrop_identity.receiver_extra(),',
                '        }, fmt=plistlib.FMT_BINARY)',
            ]),
            NL.join([
                '    def identity_plist(self, status_code=None):',
                '        settings = self.server.settings',
                '        payload = {',
                "            'ReceiverMediaCapabilities': _media_capabilities(),",
                "            'ReceiverComputerName': settings.name,",
                "            'ReceiverModelName': settings.model,",
                '            **nekodrop_identity.receiver_extra(),',
                '        }',
                '        if status_code is not None:',
                "            payload['StatusCode'] = status_code",
                '        return plistlib.dumps(payload, fmt=plistlib.FMT_BINARY)',
            ]),
        ),
        (
            NL.join([
                "        if 'links' in kinds:",
                '            links = []',
                '            for item in items:',
                '                if not transfer.is_web_link(item):',
                "                    logging.warning('ask: ignoring non-web item %.80r', item)",
                '                    continue',
                '                stored = transfer.store_link(item, self.server.settings.dest)',
                "                logging.info('link stored as %s', stored)",
                '                links.append(stored)',
                '            watcher.stored(self.accepted, links)',
                '        self.answer(self.identity_plist())',
            ]),
            NL.join([
                '        if items:',
                "            logging.info('ask extras: keys=%s items=%d types=%s bundle=%s files=%d',",
                "                         sorted(ask.keys()), len(items), sorted(kinds), ask.get('BundleID'), len(files))",
                '        extracted = transfer.extract_web_links(ask)',
                '        links_stored = False',
                "        if 'links' in kinds or extracted:",
                '            links = []',
                '            for url in extracted:',
                '                stored = transfer.store_link(url, self.server.settings.dest)',
                "                logging.info('link stored as %s', stored)",
                '                links.append(stored)',
                '            if not extracted and items:',
                '                for item in items:',
                "                    logging.warning('ask: ignoring non-web item %.80r', item)",
                '            watcher.stored(self.accepted, links)',
                '            links_stored = bool(links)',
                '            if links_stored and hasattr(self.accepted, "__dict__"):',
                '                self.accepted.links_already_stored = True',
                '        self.answer(self.identity_plist(), close=(links_stored and not files))',
            ]),
        ),
        (
            '        watcher.stored(token, written)',
            NL.join([
                "        if not getattr(token, 'links_already_stored', False):",
                '            watcher.stored(token, written)',
                '        else:',
                "            logging.info('upload %s: sidecar file(s) accepted after mixed link ask: %s', tid, ', '.join(written))",
            ]),
        ),
    ]
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f'receiver.py: {old[:60]!r} moved; update patch-engine.py')
        text = text.replace(old, new, 1)
    receiver_path.write_text(text, encoding='utf-8')
    print('receiver.py: app link extraction + configurable media capabilities')

engine_path = pathlib.Path(sys.argv[1]) / 'engine.py'
text = engine_path.read_text(encoding='utf-8')
if 'contact=contact' not in text:
    pairs = [
        (NL + 'import mdns' + NL, NL + 'import mdns' + NL + 'import nekodrop_identity' + NL),
        (
            NL.join([
                '    def _open(self, sender_name, model, files, links, waits):',
                '        with self._lock:',
                '            key = next(self._keys)',
                '            if waits:',
                '                # Before the request is said: its answer may come back at once.',
                "                self._waiting[key] = {'made': threading.Event(), 'accept': False}",
                "        sizes = [file['size'] for file in files]",
                '        total = sum(sizes) if sizes and None not in sizes else None',
                "        self.events.say('asked', id=key, sender=sender_name, model=model, files=files,",
                '                        links=links, total=total, waits=waits)',
                '        return Accepted(key, total)',
                '',
                '    def asked(self, ask, address, left):',
                "        kinds = ask.get('TransferType') or {}",
                "        links = sum(1 for item in ask.get('Items') or [] if transfer.is_web_link(item)) \\",
                "            if 'links' in kinds else 0",
                '        waits = self.settings.ask',
                "        accepted = self._open(ask.get('SenderComputerName'), ask.get('SenderModelName'),",
                '                              announced_files(ask), links, waits)',
            ]),
            NL.join([
                '    def _open(self, sender_name, model, files, links, waits, urls=None, contact=False, icon=\'\'):',
                '        with self._lock:',
                '            key = next(self._keys)',
                '            if waits:',
                '                # Before the request is said: its answer may come back at once.',
                "                self._waiting[key] = {'made': threading.Event(), 'accept': False}",
                "        sizes = [file['size'] for file in files]",
                '        total = sum(sizes) if sizes and None not in sizes else None',
                "        self.events.say('asked', id=key, sender=sender_name, model=model, files=files,",
                '                        links=links, urls=urls or [], contact=contact, icon=icon, total=total, waits=waits)',
                '        return Accepted(key, total)',
                '',
                '    def asked(self, ask, address, left):',
                '        urls = transfer.extract_web_links(ask)',
                '        links = len(urls)',
                "        contact = nekodrop_identity.is_contact(ask.get('SenderRecordData'))",
                "        tid = ask.get('TransferID')",
        "        if isinstance(tid, dict):",
        "            tid = tid.get('id')",
        "        icon = transfer.save_preview_icon(ask, tid or secrets.token_hex(4))",
                '        waits = self.settings.ask',
                "        accepted = self._open(ask.get('SenderComputerName'), ask.get('SenderModelName'),",
                '                              announced_files(ask), links, waits, urls=urls, contact=contact, icon=icon)',
            ]),
        ),
    ]
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f'engine.py: {old[:60]!r} moved; update patch-engine.py')
        text = text.replace(old, new, 1)
    engine_path.write_text(text, encoding='utf-8')
    print('engine.py: include extracted link URLs and contact flag in asked events')

# cpio.py: 512 KiB extraction chunks (8x fewer read/write calls on large files).
cpio_path = pathlib.Path(sys.argv[1]) / 'cpio.py'
text = cpio_path.read_text(encoding='utf-8')
old_c = 'BLOCK_BYTES = 64 * 1024' + NL
new_c = 'BLOCK_BYTES = 512 * 1024  # nekodrop: 512 KiB CPIO extraction chunks' + NL
if new_c not in text:
    if text.count(old_c) != 1:
        sys.exit('cpio.py: BLOCK_BYTES moved; update patch-engine.py')
    text = text.replace(old_c, new_c, 1)
    cpio_path.write_text(text, encoding='utf-8')
    print('cpio.py: 512 KiB extraction chunks')

# transfer.py: streaming DVZip -> CPIO extraction (eliminates the second full-size
# temp file copy in store_upload) + AsyncBufferedWriter so TLS receive never stalls
# on synchronous NTFS writes.
transfer_path = pathlib.Path(sys.argv[1]) / 'transfer.py'
text = transfer_path.read_text(encoding='utf-8')
if 'class _DvzipStream:' not in text:
    stream_pairs = [
        (
            'import contextlib' + NL,
            'import collections' + NL + 'import contextlib' + NL + 'import queue' + NL,
        ),
        (
            NL.join([
                'def store_upload(raw, dest, tid, receive_budget):',
                '    """Decode and extract regular files, removing every output on failure."""',
                '    created = []',
                '    try:',
                '        with tempfile.TemporaryFile(dir=dest) as archive:',
                '            raw.seek(0)',
                "            logging.debug('upload %s: body starts with %s', tid, raw.read(8).hex())",
                '            decoded = decode_dvzip(raw, archive, receive_budget.byte_limit, dest)',
                '            # The wire copy is no longer needed. Release its blocks before the',
                '            # extracted files begin consuming space.',
                '            raw.seek(0)',
                '            raw.truncate(0)',
                '            archive.seek(0)',
                "            logging.info('upload %s: decoded to %d bytes', tid, decoded)",
                '            return _extract(archive, dest,',
                '                            ExtractionBudget(receive_budget.byte_limit), created)',
                '    except Exception:',
                '        for name in reversed(created):',
                '            with contextlib.suppress(FileNotFoundError):',
                '                os.unlink(os.path.join(dest, name))',
                '        raise',
            ]),
            NL.join([
                'class _DvzipStream:',
                '    """Pull-based gzip/DVZip decoder feeding cpio.read_entries directly,',
                '    eliminating the intermediate decoded temp file copy."""',
                '',
                '    def __init__(self, source, byte_limit):',
                '        self._source = source',
                '        self._limit = byte_limit',
                '        self.total = 0',
                '        self._chunks = collections.deque()',
                '        self._off = 0',
                '        self._avail = 0',
                '        self._eof = False',
                '        self._source.seek(0)',
                '        magic = self._source.read(2)',
                '        self._source.seek(0)',
                "        self._is_gzip = (magic == b'\\x1f\\x8b')",
                '        self._gz = zlib.decompressobj(zlib.MAX_WBITS | 16) if self._is_gzip else None',
                '        self._block_left = 0',
                '        self._block_stored = False',
                '        self._block_dec = None',
                '',
                '    def _emit(self, data):',
                '        if not data:',
                '            return',
                '        if self.total + len(data) > self._limit:',
                '            raise UploadLimitError(',
                "                f'decompressed archive exceeds {self._limit}-byte limit')",
                '        self.total += len(data)',
                '        self._chunks.append(data)',
                '        self._avail += len(data)',
                '',
                '    def _inflate_all(self, dec, data):',
                '        while data:',
                '            remaining = self._limit - self.total',
                '            output = dec.decompress(data, min(IO_CHUNK_BYTES, remaining + 1))',
                '            self._emit(output)',
                '            if dec.unconsumed_tail:',
                '                data = dec.unconsumed_tail',
                '            else:',
                '                return',
                '',
                '    def _fill_one(self):',
                '        if self._is_gzip:',
                '            data = self._source.read(IO_CHUNK_BYTES)',
                '            if not data:',
                '                self._emit(self._gz.flush())',
                '                if not self._gz.eof:',
                "                    raise ValueError('truncated gzip body')",
                '                self._eof = True',
                '                return',
                '            self._inflate_all(self._gz, data)',
                '            if self._gz.unused_data:',
                "                raise ValueError('gzip body has trailing data')",
                '            return',
                '        if self._block_left == 0:',
                '            if self._block_dec is not None:',
                '                self._emit(self._block_dec.flush())',
                '                if not self._block_dec.eof:',
                "                    raise ValueError('truncated compressed block')",
                '                self._block_dec = None',
                '                if self._avail:',
                '                    return',
                '            header = self._source.read(4)',
                '            if not header:',
                '                self._eof = True',
                '                return',
                '            if len(header) != 4:',
                "                raise ValueError(f'truncated DVZip block header: {len(header)} bytes')",
                "            value = int.from_bytes(header, 'big')",
                '            self._block_left = value & 0x7fffffff',
                '            self._block_stored = bool(value & 0x80000000)',
                '            self._block_dec = None if self._block_stored else zlib.decompressobj()',
                '            if self._block_left == 0:',
                '                return',
                '        data = self._source.read(min(self._block_left, IO_CHUNK_BYTES))',
                '        if not data:',
                "            kind = 'stored' if self._block_stored else 'compressed'",
                "            raise ValueError(f'truncated {kind} block: {self._block_left} bytes missing')",
                '        self._block_left -= len(data)',
                '        if self._block_stored:',
                '            self._emit(data)',
                '        else:',
                '            self._inflate_all(self._block_dec, data)',
                '            if self._block_dec.unused_data:',
                "                raise ValueError('compressed block has trailing data')",
                '            if self._block_left == 0:',
                '                self._emit(self._block_dec.flush())',
                '                if not self._block_dec.eof:',
                "                    raise ValueError('truncated compressed block')",
                '                self._block_dec = None',
                '',
                '    def read(self, size=-1):',
                '        if size == 0:',
                "            return b''",
                '        if size < 0:',
                '            while not self._eof:',
                '                self._fill_one()',
                '            size = self._avail',
                '        else:',
                '            while self._avail < size and not self._eof:',
                '                self._fill_one()',
                '        if self._avail == 0:',
                "            return b''",
                '        want = min(size, self._avail)',
                '        head = self._chunks[0]',
                '        head_rem = len(head) - self._off',
                '        if head_rem >= want:',
                '            if self._off == 0 and head_rem == want:',
                '                self._chunks.popleft()',
                '                self._avail -= want',
                '                return head',
                '            out = head[self._off:self._off + want]',
                '            self._off += want',
                '            self._avail -= want',
                '            if self._off == len(head):',
                '                self._chunks.popleft()',
                '                self._off = 0',
                '            return out',
                '        parts = []',
                '        left = want',
                '        while left:',
                '            head = self._chunks[0]',
                '            if self._off:',
                '                head = head[self._off:]',
                '                self._off = 0',
                '            if len(head) <= left:',
                '                self._chunks.popleft()',
                '                parts.append(head)',
                '                left -= len(head)',
                '            else:',
                '                parts.append(head[:left])',
                '                self._chunks[0] = head[left:]',
                '                left = 0',
                '        self._avail -= want',
                "        return b''.join(parts)",
                '',
                '    def drain(self):',
                '        """Consume trailing CPIO padding and verify clean gzip/DVZip EOF."""',
                '        while not self._eof:',
                '            self._chunks.clear()',
                '            self._off = 0',
                '            self._avail = 0',
                '            self._fill_one()',
                '        return self.total',
                '',
                '',
                'class AsyncBufferedWriter:',
                '    """Offloads raw temp-file writes to a daemon thread so TLS reads never',
                '    stall on synchronous NTFS writes."""',
                '',
                '    def __init__(self, target, max_pending=32):',
                '        self._target = target',
                '        self._queue = queue.Queue(maxsize=max_pending)',
                '        self._error = None',
                '        self._closed = False',
                "        self._thread = threading.Thread(target=self._run, name='nekodrop-rx-writer', daemon=True)",
                '        self._thread.start()',
                '',
                '    def _run(self):',
                '        while True:',
                '            item = self._queue.get()',
                '            if item is None:',
                '                return',
                '            if self._error is not None:',
                '                continue',
                '            try:',
                '                written = self._target.write(item)',
                '                if written != len(item):',
                "                    self._error = OSError(f'short async write: {written} of {len(item)} bytes')",
                '            except Exception as exc:',
                '                self._error = exc',
                '',
                '    def write(self, data):',
                '        if self._error is not None:',
                '            raise self._error',
                '        if not data:',
                '            return 0',
                '        self._queue.put(data)',
                '        if self._error is not None:',
                '            raise self._error',
                '        return len(data)',
                '',
                '    def close(self):',
                '        if not self._closed:',
                '            self._closed = True',
                '            self._queue.put(None)',
                '            self._thread.join()',
                '        if self._error is not None:',
                '            raise self._error',
                "        if hasattr(self._target, 'flush'):",
                '            self._target.flush()',
                '',
                '    def abort(self):',
                '        if not self._closed:',
                '            self._closed = True',
                '            while True:',
                '                try:',
                '                    self._queue.get_nowait()',
                '                except queue.Empty:',
                '                    break',
                '            self._queue.put(None)',
                '            self._thread.join()',
                '',
                '',
                'def store_upload(raw, dest, tid, receive_budget):',
                '    """Decode and extract regular files in a single pass, removing every output on failure."""',
                '    created = []',
                '    try:',
                '        raw.seek(0)',
                "        logging.debug('upload %s: body starts with %s', tid, raw.read(8).hex())",
                '        stream = _DvzipStream(raw, receive_budget.byte_limit)',
                '        written = _extract(stream, dest,',
                '                           ExtractionBudget(receive_budget.byte_limit), created)',
                '        decoded = stream.drain()',
                '        raw.seek(0)',
                '        raw.truncate(0)',
                "        logging.info('upload %s: decoded to %d bytes', tid, decoded)",
                '        return written',
                '    except Exception:',
                '        for name in reversed(created):',
                '            with contextlib.suppress(FileNotFoundError):',
                '                os.unlink(os.path.join(dest, name))',
                '        raise',
            ]),
        ),
    ]
    for old, new in stream_pairs:
        if text.count(old) != 1:
            sys.exit(f'transfer.py: {old[:60]!r} moved; update patch-engine.py')
        text = text.replace(old, new, 1)
    transfer_path.write_text(text, encoding='utf-8')
    print('transfer.py: streaming DVZip->CPIO extraction + AsyncBufferedWriter')

# receiver.py: 4 MiB temp-file buffering + AsyncBufferedWriter write-behind queue.
receiver_path = pathlib.Path(sys.argv[1]) / 'receiver.py'
text = receiver_path.read_text(encoding='utf-8')
if 'AsyncBufferedWriter' not in text:
    rx_io_pairs = [
        (
            NL.join([
                '        try:',
                '            wire_bytes = transfer.read_request_body(',
                '                self.rfile, self.headers,',
                '                transfer.EndWatchingFile(raw, expect_little_more, on_progress),',
                '                budget.byte_limit, disk=dest, plan=plan, deadline=deadline)',
                '        except TimeoutError as stall:',
                '            if not deadline.between_chunks:',
                '                raise',
                '            try:',
                '                written = transfer.store_upload(raw, dest, tid, budget)',
                '            except transfer.UploadLimitError:',
                '                raise',
                '            except ValueError:',
                '                raise stall from None',
                "            logging.info('upload %s: %d bytes received, a complete archive '",
                "                         'with no end marker', tid, deadline.transferred)",
                '            return written, False',
                "        logging.info('upload %s: %d bytes received', tid, wire_bytes)",
                '        return transfer.store_upload(raw, dest, tid, budget), True',
            ]),
            NL.join([
                '        sink = transfer.AsyncBufferedWriter(raw)',
                '        try:',
                '            try:',
                '                wire_bytes = transfer.read_request_body(',
                '                    self.rfile, self.headers,',
                '                    transfer.EndWatchingFile(sink, expect_little_more, on_progress),',
                '                    budget.byte_limit, disk=dest, plan=plan, deadline=deadline)',
                '                sink.close()',
                '            except TimeoutError as stall:',
                '                sink.close()',
                '                if not deadline.between_chunks:',
                '                    raise',
                '                try:',
                '                    written = transfer.store_upload(raw, dest, tid, budget)',
                '                except transfer.UploadLimitError:',
                '                    raise',
                '                except ValueError:',
                '                    raise stall from None',
                "                logging.info('upload %s: %d bytes received, a complete archive '",
                "                             'with no end marker', tid, deadline.transferred)",
                '                return written, False',
                '        finally:',
                '            sink.abort()',
                "        logging.info('upload %s: %d bytes received', tid, wire_bytes)",
                '        return transfer.store_upload(raw, dest, tid, budget), True',
            ]),
        ),
        (
            '            with tempfile.TemporaryFile(dir=settings.dest) as raw:',
            '            with tempfile.TemporaryFile(dir=settings.dest, buffering=4 * transfer.MIB) as raw:',
        ),
    ]
    for old, new in rx_io_pairs:
        if text.count(old) != 1:
            sys.exit(f'receiver.py: {old[:60]!r} moved; update patch-engine.py')
        text = text.replace(old, new, 1)
    receiver_path.write_text(text, encoding='utf-8')
    print('receiver.py: 4 MiB temp buffer + AsyncBufferedWriter')

# engine.py: keep the Bluetooth beacon running while receiving.  An Apple
# device's AirDrop list shows only peers whose Bluetooth advertisement it has
# seen; today the beacon starts only while the client looks for receivers
# (sender.Beacon in the looking path), so a PC that is just receiving is
# invisible to iOS 18 / macOS 15 (#51).  Off unless NEKODROP_BLE_STANDBY=1.
_hook('engine.py', [
    (
        '        self.announcer = None' + NL + '        self._said = None' + NL,
        '        self.announcer = None' + NL +
        '        self.beacon = None  # nekodrop: advertised while receiving' + NL +
        '        self._said = None' + NL,
    ),
    (
        "        logging.info('receiving as %s on port %d into %s', self.settings.name," + NL +
        "                     self.settings.port, self.settings.dest)" + NL +
        '        self._say(True)' + NL,
        "        logging.info('receiving as %s on port %d into %s', self.settings.name," + NL +
        "                     self.settings.port, self.settings.dest)" + NL +
        "        if os.environ.get('NEKODROP_BLE_STANDBY') == '1':" + NL +
        '            try:' + NL +
        '                self.beacon = sender.Beacon()' + NL +
        '                self.beacon.__enter__()' + NL +
        '            except Exception as error:  # noqa: BLE001' + NL +
        "                logging.warning('standby beacon failed: %s', error)" + NL +
        '                self.beacon = None' + NL +
        '        self._say(True)' + NL,
    ),
    (
        '    def stop(self, reason=None):' + NL + '        if self.announcer:' + NL,
        '    def stop(self, reason=None):' + NL +
        '        if self.beacon:' + NL +
        '            try:' + NL +
        '                self.beacon.__exit__(None, None, None)' + NL +
        '            except Exception:  # noqa: BLE001' + NL +
        '                pass' + NL +
        '            self.beacon = None' + NL +
        '        if self.announcer:' + NL,
    ),
], 'NEKODROP_BLE_STANDBY')
