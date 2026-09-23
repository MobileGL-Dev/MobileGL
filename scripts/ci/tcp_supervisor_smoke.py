#!/usr/bin/env python3
"""Exercise a real TCP supervisor's LISTEN, Busy, token, version, build-policy and REAP controls.

P7 wave 0 registered this script as `TcpLane.SupervisorProtocolControls` in
MG_IntegrationTest/CMakeLists.txt. Until then it was referenced by NOTHING in the tree - no
ctest entry, no workflow step - so the four controls it asserts had been green or red for an
unknown number of commits and nobody would have known which.

P7 wave 2-F, PH-7 (1)(2)(3) (ID-P7-3), added the two LISTEN controls at the top of main(). They
are the negative controls for the policy AuthToken.h now owns, and both are end-to-end against a
real supervisor process because that is the only place the policy is reachable: an in-process
unit test of `SocketTransport::Listen` can assert the return code but not that an operator who
misconfigured a token gets a dead server with a named reason rather than a live one that silently
serves loopback only.

P7 F2, PH-7 (5): the supervisor now reads and authenticates every first frame before it forks, so
the controls here assert that an unauthenticated peer learns nothing of ours
(`unauthenticated_wrong_layout`), that a silent one costs no fork and is answered at the pre-auth
deadline (`silent_peer`), that Busy is the answer to an AUTHENTICATED second client (`busy`), the
pending cap and the backoff (`pending_cap`, `auth_backoff`), and ID-P7-44's queued-DataBind
refusal (`data_bind_queued_before_bound`). The f2-auth fix round added that the pending queue is
shared between addresses (`pending_queue_is_shared`) and that an address holding slots with silent
connections is backed off (`silent_slot_holder_backed_off`). The five malformed-frame shapes are
fuzz arm 1's own entry, scripts/ci/ph_fuzz_control_frames.py.

It needs flatc, which is deliberately absent from the default build graph (gen_protocol.py's
header block says why). Resolution order is gen_protocol.py's, minus the build-it-for-you arm:
--flatc, then $MOBILEGL_FLATC_EXECUTABLE, then <repo>/../flatc-build/flatc. With none of them
the script exits kSkipExit and ctest reports SKIP - loudly absent rather than quietly passing,
which is the failure mode a gate nothing references already had.
"""
import argparse
from contextlib import contextmanager
import importlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]

# ctest's SKIP_RETURN_CODE for this entry. 77 is the automake convention the rest of the world
# uses and ctest has no opinion of its own.
kSkipExit = 77

# PH-7 (3): >= AuthToken.h's kMinimumAuthTokenBytes (16). The old value, `p65-smoke-token`, was
# fifteen bytes - one short - which is a pleasing accident and exactly the kind of token the
# minimum exists to refuse. The value is arbitrary; its LENGTH is the fixture.
kToken = 'p7-ph-f-supervisor-token'
assert len(kToken) >= 16, kToken
# A token that is a credential in every respect except length.
kShortToken = 'p7-ph-f-short'
assert len(kShortToken) < 16, kShortToken

# ServerMain's `SocketTransport::Listen(...) != MOBILEGL_OK` arm.
kListenRefusedExit = 72


def resolve_flatc(explicit):
    """gen_protocol.py's order, without its build-it-for-you arm. None when there is none."""
    for candidate in (explicit, os.environ.get('MOBILEGL_FLATC_EXECUTABLE', ''),
                      str(ROOT.parent / 'flatc-build' / 'flatc')):
        if not candidate:
            continue
        found = candidate if os.path.isfile(candidate) else shutil.which(candidate)
        if found:
            return found
    return None


def read_exact(peer, size):
    result = bytearray()
    while len(result) < size:
        chunk = peer.recv(size - len(result))
        if not chunk:
            raise RuntimeError('control channel closed before a complete frame')
        result.extend(chunk)
    return result


def welcome_terms(welcome):
    """The four byte counts a Welcome states, or {} when it carries no LinkTerms."""
    terms = welcome.LinkTerms()
    if terms is None:
        return {}
    return {'maxReply': terms.MaxReplyBytes(), 'cmdWindow': terms.CmdWindowBytes(),
            'stageWindow': terms.StageWindowBytes(), 'eventWindow': terms.EventWindowBytes()}


def receive(peer, schema):
    magic, size = struct.unpack('<4sI', read_exact(peer, 8))
    if magic != b'MGLF' or size > 64 * 1024 * 1024:
        raise RuntimeError('invalid control frame')
    data = read_exact(peer, size)
    envelope = schema['CtrlEnvelope'].CtrlEnvelope.GetRootAs(data, 0)
    table = envelope.Msg()
    kind = envelope.MsgType()
    if kind == schema['CtrlMsg'].CtrlMsg.Refuse:
        refusal = schema['Refuse'].Refuse()
        refusal.Init(table.Bytes, table.Pos)
        return {'code': refusal.Code(), 'expected': refusal.Expected(),
                'detail': (refusal.Detail() or b'').decode()}
    if kind == schema['CtrlMsg'].CtrlMsg.Welcome:
        welcome = schema['Welcome'].Welcome()
        welcome.Init(table.Bytes, table.Pos)
        return {'welcome': welcome.ServerPid(), 'fingerprint': welcome.WireFingerprint(),
                'nonce': bytes(welcome.DataNonce(j) for j in range(welcome.DataNonceLength())).hex(),
                'terms': welcome_terms(welcome)}
    raise RuntimeError(f'unexpected first control reply {kind}')


def hello(schema, flatbuffers, fingerprint=0, major=1, token=kToken, identifier=b'MGLC', ask=0):
    builder = flatbuffers.Builder(512)
    build = builder.CreateString('intentionally-different-build-for-p65-control')
    auth = builder.CreateString(token)
    terms = schema['LinkTerms']
    terms.Start(builder)
    terms.AddDataPlane(builder, 1)
    if ask:
        # PH-8: a client ASKING for windows. The server sizes the session itself; see
        # `hello_asks_64_gib` below.
        terms.AddWireForm(builder, 0)
        terms.AddMaxReplyBytes(builder, ask)
        terms.AddCmdWindowBytes(builder, ask)
        terms.AddStageWindowBytes(builder, ask)
        terms.AddEventWindowBytes(builder, ask)
    link = terms.End(builder)
    message = schema['Hello']
    message.Start(builder)
    message.AddAbiMajor(builder, major)
    message.AddBuildFingerprint(builder, build)
    message.AddBackendType(builder, 0)  # DirectGLES
    message.AddPid(builder, os.getpid())
    message.AddWireFingerprint(builder, fingerprint)
    message.AddLinkTerms(builder, link)
    message.AddToken(builder, auth)
    message.AddDialMode(builder, 2)  # Connect
    body = message.End(builder)
    envelope = schema['CtrlEnvelope']
    envelope.Start(builder)
    envelope.AddMsgType(builder, schema['CtrlMsg'].CtrlMsg.Hello)
    envelope.AddMsg(builder, body)
    # `identifier` is a parameter for ONE caller: the steady-state loop's identifier check.
    # flatbuffers' VerifyCtrlEnvelopeBuffer passes a nullptr identifier to VerifyBuffer, so a
    # frame of some other schema that happens to verify used to reach GetCtrlEnvelope here.
    builder.Finish(envelope.End(builder), identifier)
    payload = bytes(builder.Output())
    return struct.pack('<4sI', b'MGLF', len(payload)) + payload


def data_bind(schema, flatbuffers, nonce):
    """PH-7 (4): the framed DataBind a client writes as its data connection's first bytes."""
    builder = flatbuffers.Builder(64)
    vector = builder.CreateByteVector(nonce)
    message = schema['DataBind']
    message.Start(builder)
    message.AddNonce(builder, vector)
    body = message.End(builder)
    envelope = schema['CtrlEnvelope']
    envelope.Start(builder)
    envelope.AddMsgType(builder, schema['CtrlMsg'].CtrlMsg.DataBind)
    envelope.AddMsg(builder, body)
    builder.Finish(envelope.End(builder), b'MGLC')
    payload = bytes(builder.Output())
    return struct.pack('<4sI', b'MGLF', len(payload)) + payload


def open_data(port, schema, flatbuffers, nonce):
    """A data connection that has presented `nonce`. Left open: the server owns what happens next."""
    data = socket.create_connection(('127.0.0.1', port), timeout=3)
    data.sendall(data_bind(schema, flatbuffers, nonce))
    return data


def supervisor_environment(log, token, same_build=False):
    """The env a supervisor is started with, minus everything ServerMain's scrub catches."""
    env = dict(os.environ, MOBILEGL_IPC_ROLE='server', MOBILEGL_IPC_DIAL='no',
               MOBILEGL_IPC_REQUIRE_SAME_BUILD=str(int(same_build)),
               MOBILEGL_IPC_LOG_FORWARD='0', MOBILEGL_LOG_FILE_PATH=str(log))
    if token is None:
        env.pop('MOBILEGL_IPC_TOKEN', None)
    else:
        env['MOBILEGL_IPC_TOKEN'] = token
    for name in ('MOBILEGL_TRANSPORT', 'MOBILEGL_IPC_SERVER_PATH', 'MOBILEGL_IPC_RING_MB', 'MOBILEGL_IPC_STAGE_MB',
                 'MOBILEGL_IPC_CONTROL', 'MOBILEGL_IPC_ENDPOINT', 'MOBILEGL_BACKEND_TYPE'):
        env.pop(name, None)
    return env


def free_loopback_port():
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        return reserve.getsockname()[1]


def refused_listen(server, log, endpoint, token):
    """Start a supervisor that must NOT come up, and return (exit code, everything it said).

    PH-7 (2)(3), ID-P7-3. Both listen refusals are asserted this way rather than through
    SocketTransport::Listen's return code, because the property is not "the function returns
    PROTOCOL_MISMATCH" - it is that the PROCESS dies, says which policy killed it, and does not
    leave a half-configured server listening. A supervisor that degraded to loopback-only after a
    misconfigured token would pass a return-code test and fail an operator.
    """
    base = log.with_suffix('.library.log')
    env = supervisor_environment(base, token)
    with log.open('wb') as output:
        child = subprocess.Popen([server, endpoint, '--serve'], env=env,
                                 stdout=output, stderr=output, start_new_session=True)
    try:
        code = child.wait(timeout=30)
    except subprocess.TimeoutExpired:
        # THE RED SHAPE OF THIS CONTROL IS A SUPERVISOR THAT CAME UP, so it is still listening
        # and would answer the next control's peers. Killed here rather than left for the
        # assertion below, which is what makes the red a red and not a cascade.
        os.killpg(child.pid, signal.SIGKILL)
        child.wait(timeout=10)
        code = None
    text = log.read_text(errors='replace')
    role = base.with_name(base.stem + '.server' + base.suffix)
    if role.is_file():
        text += role.read_text(errors='replace')
    return code, text


@contextmanager
def supervisor(server, log, same_build=False, serve=True, extra_env=None):
    """A supervisor on its own free loopback port. `serve=False` is the single-session shape
    (no fork; the process IS the session and exits with it), which `single_session_listener`
    below needs and nothing else does. `extra_env` sets supervisor knobs (PH-7 (5)'s pre-auth
    deadline, pending cap and backoff) for the controls that need non-default ones."""
    port = free_loopback_port()
    base = log.with_suffix('.library.log')
    env = supervisor_environment(base, kToken, same_build=same_build)
    env.update(extra_env or {})
    with log.open('wb') as output:
        child = subprocess.Popen([server, f'tcp://127.0.0.1:{port}'] + (['--serve'] if serve else []),
                                 env=env, stdout=output, stderr=output, start_new_session=True)
    try:
        # Log.cpp's RoleLogPath: the base name is a BASE, and the server role writes
        # `<stem>.server<ext>`. Yielded rather than recomputed at the call site, so the one
        # place that knows the rule is the one place that sets MOBILEGL_LOG_FILE_PATH.
        yield port, child, base.with_name(base.stem + '.server' + base.suffix)
    finally:
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        child.wait(timeout=10)


def connect_control(port, seconds=5, source=None):
    """A connection to the supervisor's port, retried on ECONNREFUSED for `seconds`: a supervisor
    just started may not have reached listen() yet, and a refused connect before it has is
    startup, not a finding. `source` binds the local end to another loopback address (127.0.0.2,
    ...), which is how the pre-auth controls stand in for a second host: the supervisor keys its
    backoff and its share of the pending queue by peer address."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            return socket.create_connection(('127.0.0.1', port), timeout=3,
                                            source_address=(source, 0) if source else None)
        except ConnectionRefusedError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.02)


@contextmanager
def held_session(port, schema, flatbuffers, message):
    """A welcomed session whose sockets STAY OPEN, so its child is still alive to be killed.

    `exchange` closes the connection before returning, which is what every other control in this
    file wants and is exactly wrong for the reap case: a child that has already exited cannot be
    made to fault. Busy is retried here for the same reason it is there - a previous child may
    still be reaching _exit.

    PH-7 (4): the data connection is opened AFTER the Welcome and presents its nonce, which is
    what gets the child out of the handshake and into its control loop.
    """
    control = None
    data = None
    reply = None
    for _ in range(30):
        control = connect_control(port)
        control.sendall(message)
        reply = receive(control, schema)
        if reply.get('code') != 7:
            break
        control.close()
        control = None
        time.sleep(.05)
    if control is None:
        raise RuntimeError('supervisor remained Busy while a held session was wanted')
    if reply.get('welcome', 0):
        data = open_data(port, schema, flatbuffers, bytes.fromhex(reply['nonce']))
    try:
        yield control, reply
    finally:
        control.close()
        if data is not None:
            data.close()


def last_faulted(path):
    """The supervisor's lifetime fault count as of the last reap line, or 0 before the first."""
    if not path.is_file():
        return 0
    values = re.findall(r'sessionsFaulted=(\d+)', path.read_text(errors='replace'))
    return int(values[-1]) if values else 0


def wait_for_log(path, needle, seconds=15):
    """The supervisor reaps on its next accept timeout (250 ms), so this polls rather than sleeps."""
    deadline = time.monotonic() + seconds
    while True:
        if path.is_file():
            text = path.read_text(errors='replace')
            if needle in text:
                return text
        if time.monotonic() >= deadline:
            return None
        time.sleep(.05)


@contextmanager
def pair(port):
    """A session's CONTROL connection. The name is historical: until PH-7 (4) a TCP session was
    two connections paired by arrival order, and this opened both. The data connection is now
    opened after Welcome with the nonce it carries (open_data), so a control that stops at the
    handshake - every one but held_session's - never opens one."""
    control = connect_control(port)
    try:
        yield control
    finally:
        control.close()


def exchange(port, schema, message):
    # A prior rejected child may still be reaching _exit when the next pair
    # arrives. Only Busy may be retried here, and never in the Busy assertion.
    for _ in range(30):
        with pair(port) as control:
            control.sendall(message)
            reply = receive(control, schema)
        if reply.get('code') != 7:
            return reply
        time.sleep(.05)
    raise RuntimeError('supervisor remained Busy after the previous peer closed')


# PH-7 (5): the refusal details MG_Remote/Server/PreAuthGate.h names. Mirrored, not parsed out of
# the header: a rename there must fail these controls rather than be followed by them silently.
kDetailNotAFrame = 'first frame is not a control frame'
kDetailTruncated = 'first frame ended before it was complete'
kDetailNoIdentifier = 'first frame carries no CtrlEnvelope identifier'
kDetailUnverifiable = 'first frame did not verify as a CtrlEnvelope'
kDetailNotAHello = 'first control frame is not a verifiable Hello'
kDetailDeadline = 'no authenticated first frame within the pre-auth deadline'
kDetailPendingFull = 'too many connections are awaiting authentication'
kDetailDisplaced = 'displaced from the pre-auth queue by a connection from another address'
kDetailBackoff = 'too many failed authentications from this address; retry later'
kDetailHandoff = 'data connection could not be handed to the live session'


def child_pids(pid):
    """Every process whose parent is `pid` - the supervisor's session children, including one that
    has exited and is not reaped yet. Read from /proc directly, because the property PH-7 (5)
    asserts is that NO child exists for a peer that has not authenticated, and the log only speaks
    of a child once it is reaped."""
    children = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / 'stat').read_text()
        except OSError:
            continue
        fields = stat.rsplit(')', 1)[1].split()
        if int(fields[1]) == pid:
            children.append(int(entry.name))
    return children


def wait_for_no_session(pid, seconds=10):
    """Until the supervisor has no session child (a previous control's session reaped), so the
    next control starts from "no live session" by construction rather than by timing."""
    deadline = time.monotonic() + seconds
    while child_pids(pid):
        if time.monotonic() >= deadline:
            raise RuntimeError(f'supervisor pid={pid} still has session children {child_pids(pid)}')
        time.sleep(.05)


def load_schema(flatc, directory):
    """The protocol's Python bindings, generated by the pinned flatc into `directory`."""
    subprocess.run([flatc, '--python', '-o', directory,
                    str(ROOT / 'MobileGL/MG_Remote/Protocol/protocol.fbs')], check=True)
    sys.path[:0] = [directory, str(ROOT / '3rdparty/flatbuffers/python')]
    import flatbuffers
    schema = {name: importlib.import_module('MobileGL.Wire.' + name)
              for name in ('CtrlEnvelope', 'CtrlMsg', 'DataBind', 'Hello', 'LinkTerms', 'LogFlush', 'Refuse',
                           'Welcome')}
    return flatbuffers, schema


def silent_peer_control(port, process, schema):
    """PH-7 (5) (ph-f.md §6.4 (a)): A SILENT PEER COSTS NO FORK.

    The supervisor forked a session child the moment it accepted, and that child waited up to
    10 s for a Hello before exiting 67 without a word. Now nothing is forked for a peer that has
    not authenticated: while the silent connection is pending the supervisor has NO child (read
    from /proc, not from the log, which only names a child once it is reaped), and at the pre-auth
    deadline (default 2000 ms) the peer is answered by name. RED before the fix: a child exists
    at the first check, and the peer's read ends with EOF after 10 s instead of a frame. A function
    of its own so the red-once probe can run exactly this control against an older server."""
    wait_for_no_session(process.pid)
    silent = connect_control(port)
    try:
        time.sleep(.5)
        forked = child_pids(process.pid)
        silent.settimeout(15)
        began = time.monotonic()
        try:
            answer = receive(silent, schema)
        except (RuntimeError, OSError) as error:
            answer = {'error': repr(error)}
        waited = time.monotonic() - began
    finally:
        silent.close()
    assert not forked, f'the supervisor forked {forked} for a peer that has not sent a byte'
    assert answer.get('code') == 4 and answer.get('detail') == kDetailDeadline, answer
    assert waited < 5, f'the silent peer was answered after {waited:.1f} s'
    assert process.poll() is None, 'a silent peer killed the supervisor'
    return {'refusal': answer, 'childrenWhilePending': forked, 'answeredAfterSeconds': round(waited + .5, 2)}


def learn_fingerprint(port, schema, flatbuffers):
    """The supervisor's wire fingerprint, from the one peer allowed to learn it: an AUTHENTICATED
    Hello with fingerprint 0 is answered Refuse{WireFingerprint} carrying ours (PH-7 (5): an
    unauthenticated one is not)."""
    layout = exchange(port, schema, hello(schema, flatbuffers))
    assert layout.get('code') == 2 and layout.get('expected', 0), layout
    return layout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True)
    parser.add_argument('--flatc', default='')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    flatc = resolve_flatc(args.flatc)
    if flatc is None:
        print('tcp_supervisor_smoke: no flatc found (--flatc, $MOBILEGL_FLATC_EXECUTABLE, '
              f'{ROOT.parent / "flatc-build" / "flatc"}); skipping. Build one once with '
              '`python3 scripts/gen_protocol.py` and this lane runs.', flush=True)
        return kSkipExit
    if not os.path.isfile(args.server):
        print(f'tcp_supervisor_smoke: no server image at {args.server}; skipping', flush=True)
        return kSkipExit
    with tempfile.TemporaryDirectory(prefix='mgl-tcp-schema-') as directory:
        flatbuffers, schema = load_schema(flatc, directory)
        evidence = {}

        # ---- PH-7 (2)(3), ID-P7-3: THE TWO LISTEN REFUSALS -------------------------------
        #
        # Both are the SAME policy from two directions: a listener that can be reached from off
        # this machine has to have a credential, and a credential has to be one.
        #
        # (a) No token, non-loopback bind. The mechanism has been in the tree since P6.5
        #     (SocketTransport.cpp's ListenTcp) and the plan's own gate row called it "done
        #     (mechanism)" with the caveat that nothing referenced it. This is the reference.
        # (b) A token shorter than 16 bytes, on LOOPBACK, where a short token is otherwise
        #     harmless. It still fails, and that is the ruling: a configured token is either a
        #     credential or a refusal, never a warning. Before this package a one-byte token
        #     unlocked `tcp://0.0.0.0`.
        code, said = refused_listen(args.server, args.out / 'wildcard-no-token.log',
                                    'tcp://0.0.0.0:40699', None)
        assert code == kListenRefusedExit, (code, said)
        assert 'Refuse{Authentication}' in said, said
        evidence['wildcard_listen_without_a_token'] = {'exit': code, 'line': next(
            l.strip() for l in said.splitlines() if 'Refuse{Authentication}' in l)}
        short = f'tcp://127.0.0.1:{free_loopback_port()}'
        code, said = refused_listen(args.server, args.out / 'short-token.log', short, kShortToken)
        assert code == kListenRefusedExit, (code, said)
        assert 'Refuse{Authentication}' in said and 'minimum is 16' in said, said
        assert kShortToken not in said, 'the refusal printed the token'
        evidence['short_token_listen'] = {'exit': code, 'line': next(
            l.strip() for l in said.splitlines() if 'Refuse{Authentication}' in l)}
        with supervisor(args.server, args.out / 'supervisor.log') as (port, process, serverLog):
            layout = learn_fingerprint(port, schema, flatbuffers)
            fingerprint = layout['expected']
            evidence['wrong_layout'] = layout
            token = exchange(port, schema, hello(schema, flatbuffers, fingerprint=fingerprint, token='wrong-token'))
            assert token.get('code') == 4, token
            evidence['wrong_token'] = token

            # ---- PH-7 (5) (ph-f.md §6.4 (b)): AN UNAUTHENTICATED PEER LEARNS NOTHING ABOUT US ----
            #
            # The same wrong layout as `wrong_layout` above, WITHOUT the token. The session child
            # used to check the wire before the token, so this peer was answered
            # Refuse{WireFingerprint} with our fingerprint in `expected` (and with major 99,
            # Refuse{ProtocolVersion} with our version). The token is asked first now, by the
            # supervisor, and the answer is Refuse{Authentication} carrying nothing of ours.
            # RED before the fix: code 2 with expected == our fingerprint.
            unauthenticated = exchange(port, schema, hello(schema, flatbuffers, token='wrong-token'))
            assert unauthenticated.get('code') == 4 and unauthenticated.get('expected') == 0, (
                f'an unauthenticated Hello with a wrong layout was answered {unauthenticated}; '
                f'our fingerprint is {fingerprint}')
            unauthenticated_version = exchange(port, schema, hello(schema, flatbuffers, major=99, token='wrong-token'))
            assert unauthenticated_version.get('code') == 4 and unauthenticated_version.get('expected') == 0, (
                unauthenticated_version)
            evidence['unauthenticated_wrong_layout'] = unauthenticated
            evidence['unauthenticated_wire_major_99'] = unauthenticated_version

            version = exchange(port, schema, hello(schema, flatbuffers, major=99))
            assert version.get('code') == 1, version
            evidence['wire_major_99'] = version
            accepted = exchange(port, schema, hello(schema, flatbuffers, fingerprint=fingerprint))
            assert accepted.get('welcome', 0) and accepted['welcome'] != os.getpid(), accepted
            assert accepted.get('fingerprint') == fingerprint, accepted
            evidence['different_build_connect'] = accepted
            assert process.poll() is None, 'a rejected session killed the supervisor'

            # ---- Busy is the answer to an AUTHENTICATED second client (PH-7 (5)) ---------------
            #
            # This control used to hold a connection that never sent a Hello and read Busy on a
            # second silent one: the supervisor forked for the first and refused the second after
            # waiting 2 s for its frame. Neither is a client now - both are unauthenticated peers
            # and are answered at the pre-auth deadline (`silent_peer` below). One session at a
            # time is unchanged, so a WELCOMED session plus a second authenticated Hello is Busy.
            with held_session(port, schema, flatbuffers,
                              hello(schema, flatbuffers, fingerprint=fingerprint)) as (control, welcome):
                assert welcome.get('welcome', 0), welcome
                with pair(port) as second:
                    second.sendall(hello(schema, flatbuffers, fingerprint=fingerprint))
                    busy = receive(second, schema)
                assert busy.get('code') == 7, busy
                evidence['busy'] = busy

            evidence['silent_peer'] = silent_peer_control(port, process, schema)

            # P7 wave 0: the steady-state control loop asks for the file identifier, which
            # ServerSession::ParseEnvelope has asked since P5 and ServerMain's loop never did.
            # flatbuffers verifies a table's offsets, not the four identifier bytes, so until now
            # a frame of a DIFFERENT schema that happened to verify was read here as a
            # CtrlEnvelope and its union tag believed. This is the cheap half of Ph's fuzz arm 1;
            # the arm itself (five malformed shapes, its own ctest lane) is scheduled after P7.
            with held_session(port, schema, flatbuffers,
                              hello(schema, flatbuffers, fingerprint=fingerprint)) as (control, welcome):
                assert welcome.get('welcome', 0), welcome
                control.sendall(hello(schema, flatbuffers, fingerprint=fingerprint,
                                      identifier=b'XXXX'))
                named = wait_for_log(serverLog, 'carries no CtrlEnvelope identifier')
                assert named is not None, (
                    'the session read a frame with the wrong file identifier without saying so; '
                    f'see {serverLog}')
            evidence['wrong_identifier_frame'] = next(
                l.strip() for l in named.splitlines() if 'carries no CtrlEnvelope identifier' in l)
            assert process.poll() is None, 'a malformed steady-state frame killed the supervisor'

            # P7 wave 0, Ph slice (1) (ID-P7-1). THE SUPERVISOR NAMES A FAULTED SESSION, AND THE
            # NEXT CONNECTION IS STILL SERVED.
            #
            # The fault is delivered as SIGABRT to the session child, which is the signal every
            # death through SessionFail raises - so this exercises the reap path a real Fatal
            # takes, without needing a scenario that can reach one. The child's pid comes from
            # the Welcome it just sent (`serverPid`), which is the only honest way to name it:
            # the supervisor's own pid is the parent's.
            #
            # Before this package the three `waitpid(active, nullptr, WNOHANG)` calls discarded
            # the status, so the log carried no line at all for this event and the assertion
            # below could not have been written.
            held = hello(schema, flatbuffers, fingerprint=fingerprint)
            with held_session(port, schema, flatbuffers, held) as (control, welcome):
                faulted = welcome.get('welcome', 0)
                assert faulted and faulted != os.getpid(), welcome
                # The count is a supervisor LIFETIME figure, so what is asserted is the DELTA, not
                # an absolute. (Until PH-7 (5) the controls above had already moved it: the old
                # Busy probe held a pair that never sent a Hello, and the child forked for it exited
                # 67. Nothing is forked for such a peer now.)
                prior = last_faulted(serverLog)
                os.kill(faulted, signal.SIGABRT)
            reaped = wait_for_log(serverLog, f'pid={faulted} reaped signal={int(signal.SIGABRT)}')
            assert reaped is not None, (
                f'the supervisor did not name session pid={faulted} as killed by '
                f'signal={int(signal.SIGABRT)} in {serverLog}; a faulted session is '
                f'indistinguishable from a finished one again (ID-P7-1)')
            line = next(l for l in reaped.splitlines() if f'pid={faulted} reaped' in l)
            assert f'sessionsFaulted={prior + 1}' in line, (line, prior)
            # A refused handshake is a normal outcome and its child exits 0, so the counter must
            # NOT have moved for the three refusals above. Stated as an assertion rather than
            # left to the delta: a counter that counted refusals would make the number useless.
            refusals = sum(1 for l in reaped.splitlines() if 'reaped exit=0' in l)
            assert refusals >= 3, f'expected the three refusal children to exit 0: {refusals}'
            evidence['faulted_session'] = {'pid': faulted, 'line': line.strip(),
                                           'priorFaults': prior, 'cleanExits': refusals}
            assert process.poll() is None, 'the supervisor died with its session child'

            # And the point of fork-per-session: the NEXT Hello is welcomed as usual.
            after = exchange(port, schema, hello(schema, flatbuffers, fingerprint=fingerprint))
            assert after.get('welcome', 0) and after['welcome'] != faulted, after
            evidence['connect_after_fault'] = after

            # ---- PH-7 (4), ID-P7-3: THE DATA CONNECTION IS BOUND BY NONCE ---------------------
            #
            # (a) THE FIELD FAILURE. Over a Windows `adb forward` the two connections of one
            #     session arrived reordered, the server's AcceptPair read the data connection as
            #     control, and every child exited 67. Reproduced here without adb: the data
            #     connection (a DataBind with a nonce no live session minted) is presented FIRST,
            #     the control connection second. It is refused by name, as a clean exit, and the
            #     control connection behind it is welcomed.
            # "No live session" by construction: the previous control's child is reaped first.
            wait_for_no_session(process.pid)
            stray = open_data(port, schema, flatbuffers, os.urandom(16))
            try:
                refused = receive(stray, schema)
            finally:
                stray.close()
            assert refused.get('code') == 4 and refused.get('detail') == 'data connection names no live session', refused
            reordered = exchange(port, schema, hello(schema, flatbuffers, fingerprint=fingerprint))
            assert reordered.get('welcome', 0), reordered
            assert len(bytes.fromhex(reordered['nonce'])) == 16, reordered
            evidence['data_connection_first'] = refused

            # (b) A STALE NONCE while a session is live: a data connection from an earlier session
            #     (or raced in by anybody who can reach the port) is refused by name ON THAT
            #     CONNECTION, and the session it failed to join still binds its own and comes up.
            held = hello(schema, flatbuffers, fingerprint=fingerprint)
            for _ in range(30):
                control = socket.create_connection(('127.0.0.1', port), timeout=3)
                control.sendall(held)
                welcome = receive(control, schema)
                if welcome.get('code') != 7:
                    break
                control.close()
                time.sleep(.05)
            try:
                assert welcome.get('welcome', 0) and len(bytes.fromhex(welcome['nonce'])) == 16, welcome
                nonce = bytes.fromhex(welcome['nonce'])
                stale = bytes(b ^ 0xFF for b in nonce)
                wrong = open_data(port, schema, flatbuffers, stale)
                try:
                    mismatch = receive(wrong, schema)
                finally:
                    wrong.close()
                assert mismatch.get('code') == 4 and mismatch.get('detail') == 'data connection nonce mismatch', mismatch
                right = open_data(port, schema, flatbuffers, nonce)
                try:
                    ready = wait_for_log(serverLog, f'pid={welcome["welcome"]} transport=spawn role=server ready')
                    assert ready is not None, (
                        f'session pid={welcome["welcome"]} did not come up on its own data connection '
                        f'after a stale one was refused; see {serverLog}')
                finally:
                    right.close()
            finally:
                control.close()
            named = wait_for_log(serverLog, 'Refuse{Authentication} data connection nonce mismatch')
            assert named is not None, f'the stale data connection was not refused by name in {serverLog}'
            evidence['stale_nonce'] = {'refusal': mismatch, 'line': next(
                l.strip() for l in named.splitlines() if 'data connection nonce mismatch' in l)}
            assert process.poll() is None, 'a refused data connection killed the supervisor'

            # ---- PH-8: THE SERVER SIZES THE SESSION; THE CLIENT ONLY ASKS -------------------
            #
            # A Hello that asks for 64 GiB in every window is welcomed with the server's own
            # terms, and the session child that answered it - named by the Welcome's serverPid -
            # never had 64 GiB of address space at any point (VmPeak, read after its data
            # connection bound, i.e. after every segment of the session exists).
            ask = 64 << 30
            with held_session(port, schema, flatbuffers,
                              hello(schema, flatbuffers, fingerprint=fingerprint, ask=ask)) as (control, welcome):
                assert welcome.get('welcome', 0), welcome
                terms = welcome['terms']
                assert terms and all(0 < value < (1 << 30) for value in terms.values()), (
                    f'the Welcome did not clamp a 64 GiB ask to the server\'s own terms: {terms}')
                ready = wait_for_log(serverLog, f'pid={welcome["welcome"]} transport=spawn role=server ready')
                assert ready is not None, f'the 64 GiB-asking session never came up; see {serverLog}'
                status = Path(f'/proc/{welcome["welcome"]}/status').read_text()
                peak = int(re.search(r'VmPeak:\s+(\d+) kB', status).group(1)) * 1024
                assert peak < ask, f'session pid={welcome["welcome"]} reached VmPeak={peak} for a 64 GiB ask'
            evidence['hello_asks_64_gib'] = {'asked': ask, 'terms': terms, 'childVmPeakBytes': peak}

            # ---- F fix round: A BOUND SESSION'S SUPERVISOR REFUSES EVERY LATER DataBind BY NAME --
            #
            # The session child reads its hand-off socketpair only while binding; after that it
            # never reads it again. Until this control the supervisor kept forwarding every later
            # DataBind into it anyway: each descriptor sat in the child's receive queue with NO
            # reply, its TCP connection was held open until the session ended, and once the queue
            # reached net.unix.max_dgram_qlen (10 on the phone's kernel, 512 on this host's) the
            # supervisor blocked in sendmsg for the rest of the session - no accepts, no Busy, no
            # reaping. Now the child closes its end the moment Accept returns and the send is
            # non-blocking, so each of these twelve is answered on its own connection with
            # Refuse{Authentication} "data connection could not be handed to the live session",
            # and a fresh control connection afterwards is still answered Busy. RED before the fix
            # at the very first `receive`: a 3 s socket timeout, because nothing ever answered.
            with held_session(port, schema, flatbuffers,
                              hello(schema, flatbuffers, fingerprint=fingerprint)) as (control, welcome):
                assert welcome.get('welcome', 0), welcome
                # Bound, not merely welcomed: an extra DataBind that arrived while the child was
                # still in BindDataConnection would be forwarded and refused as a nonce mismatch,
                # which is the other control's line, not this one's.
                ready = wait_for_log(serverLog, f'pid={welcome["welcome"]} transport=spawn role=server ready')
                assert ready is not None, f'the held session never came up; see {serverLog}'
                answered = []
                for _ in range(12):
                    extra = open_data(port, schema, flatbuffers, os.urandom(16))
                    try:
                        answered.append(receive(extra, schema))
                    finally:
                        extra.close()
                for answer in answered:
                    assert answer.get('code') == 4 and answer.get('detail') == (
                        'data connection could not be handed to the live session'), answer
                with pair(port) as fresh:
                    fresh.sendall(hello(schema, flatbuffers, fingerprint=fingerprint))
                    still_busy = receive(fresh, schema)
                assert still_busy.get('code') == 7, still_busy
            evidence['data_bind_after_bound'] = {'refused': len(answered), 'last': answered[-1],
                                                 'fresh_control': still_busy}
            assert process.poll() is None, 'the refused DataBinds killed the supervisor'

            # ---- ID-P7-44: A DataBind QUEUED TO THE CHILD BEFORE IT LET GO IS REFUSED, NOT DROPPED
            #
            # The F fix round closed the child's end of the hand-off the moment Accept returned;
            # between the bind and that close the supervisor could still queue a DataBind, and a
            # close with descriptors queued drops them in the kernel - that peer's connection just
            # ended, with no frame. The window is microseconds in the wild, so it is held open
            # here: the session child is SIGSTOPped right after its Welcome, its own data
            # connection and three extra DataBinds are then routed to it (the supervisor forwards
            # to a stopped child as readily as to a running one), and it is continued. It binds
            # its own connection and lets go of the hand-off with the three still queued. Each
            # extra must be answered by name - "nonce mismatch" if the child happened to read it
            # before its own, the supervisor's hand-off words if it was still queued when the
            # child let go - and the session must come up. RED before the fix: the extras' reads
            # end with EOF and no frame ("control channel closed before a complete frame").
            held = hello(schema, flatbuffers, fingerprint=fingerprint)
            for _ in range(30):
                control = connect_control(port)
                control.sendall(held)
                welcome = receive(control, schema)
                if welcome.get('code') != 7:
                    break
                control.close()
                time.sleep(.05)
            extras = []
            right = None
            try:
                assert welcome.get('welcome', 0), welcome
                child = welcome['welcome']
                os.kill(child, signal.SIGSTOP)
                try:
                    right = open_data(port, schema, flatbuffers, bytes.fromhex(welcome['nonce']))
                    time.sleep(.3)  # routed first, so it is first in the child's queue
                    for _ in range(3):
                        extras.append(open_data(port, schema, flatbuffers, os.urandom(16)))
                    time.sleep(.5)  # every one of them routed into the stopped child's queue
                finally:
                    os.kill(child, signal.SIGCONT)
                queued = []
                for extra in extras:
                    extra.settimeout(10)
                    try:
                        queued.append(receive(extra, schema))
                    except (RuntimeError, OSError) as error:
                        queued.append({'error': repr(error)})
                ready = wait_for_log(serverLog, f'pid={child} transport=spawn role=server ready')
            finally:
                for extra in extras:
                    extra.close()
                if right is not None:
                    right.close()
                control.close()
            for answer in queued:
                assert answer.get('code') == 4 and answer.get('detail') in (
                    'data connection nonce mismatch', kDetailHandoff), (
                    f'a DataBind queued to session pid={child} before it let go of the hand-off was '
                    f'answered {answer}, not refused by name (ID-P7-44); all: {queued}')
            assert ready is not None, f'session pid={child} did not come up; see {serverLog}'
            evidence['data_bind_queued_before_bound'] = {'refused': queued}
            assert process.poll() is None, 'the queued DataBinds killed the supervisor'

            # ---- F fix round: A GARBAGE FIRST FRAME WHILE BUSY IS MalformedHello, NOT Busy ------
            #
            # While a session is live the supervisor reads a new connection's first frame to route
            # it. A frame with the wrong magic (or a length over 1 MiB) was refused Busy, naming a
            # condition the connection does not have: it is not a control frame at all, and it now
            # gets the word ServerSession::Accept uses for a first frame that is not a verifiable
            # Hello. Busy stays the answer for a well-formed frame that is not a DataBind (the
            # `busy` control above). RED before the fix: code 7.
            with held_session(port, schema, flatbuffers,
                              hello(schema, flatbuffers, fingerprint=fingerprint)) as (control, welcome):
                assert welcome.get('welcome', 0), welcome
                garbage = connect_control(port)
                try:
                    garbage.sendall(b'XXXX' + bytes(60))
                    malformed = receive(garbage, schema)
                finally:
                    garbage.close()
                assert malformed.get('code') == 8 and malformed.get('detail') == (
                    'first frame is not a control frame'), malformed
            evidence['garbage_first_frame_while_busy'] = malformed
            assert process.poll() is None, 'a garbage first frame killed the supervisor'

        with supervisor(args.server, args.out / 'same-build-supervisor.log', same_build=True) as (port, _, _log):
            strict = exchange(port, schema, hello(schema, flatbuffers, fingerprint=fingerprint))
            assert strict.get('code') == 3, strict
            evidence['same_build_required'] = strict

        # ---- PH-7 (5): THE PENDING CAP AND THE AUTHENTICATION BACKOFF ---------------------------
        #
        # A supervisor with small knobs, so both limits are reached in a few connections: at most
        # two connections pending authentication, and an address backed off after two failures,
        # for 1500 ms. Each control below speaks from its OWN loopback address (connect_control's
        # `source`), because the failures one control provokes are counted against the address it
        # used, and a silent connection held past the probe grace is one of them.
        knobs = {'MOBILEGL_IPC_PREAUTH_MAX': '2', 'MOBILEGL_IPC_PREAUTH_MS': '3000',
                 'MOBILEGL_IPC_AUTH_BACKOFF_AFTER': '2', 'MOBILEGL_IPC_AUTH_BACKOFF_MS': '1500'}
        with supervisor(args.server, args.out / 'preauth-knobs.log', extra_env=knobs) as (port, process, serverLog):
            # (a) THE CAP. Two silent connections from 127.0.0.3 fill it; a third from the SAME
            #     address is refused Busy AT ACCEPT, at once, instead of joining them - its address
            #     already holds the whole queue. RED with the cap disabled: the third is not
            #     answered until its own deadline, and then as Authentication.
            first, second = connect_control(port, source='127.0.0.3'), connect_control(port, source='127.0.0.3')
            try:
                time.sleep(.3)  # both accepted and pending
                third = connect_control(port, source='127.0.0.3')
                try:
                    third.settimeout(10)
                    began = time.monotonic()
                    full = receive(third, schema)
                    waited = time.monotonic() - began
                finally:
                    third.close()
            finally:
                first.close()
                second.close()
            assert full.get('code') == 7 and full.get('detail') == kDetailPendingFull, full
            assert waited < 1, f'the connection over the pending cap waited {waited:.2f} s for its answer'
            evidence['pending_cap'] = {'refusal': full, 'answeredAfterSeconds': round(waited, 3)}
            time.sleep(.3)

            # (a2) THE QUEUE IS SHARED (fix round). The first cut had one pool: one address that
            #     held every slot kept every other address out, for as long as it cared to. Now two
            #     silent connections from 127.0.0.2 fill the queue and a Hello from 127.0.0.1 is
            #     still JUDGED - it displaces 127.0.0.2's oldest connection, which is told so by
            #     name - and reaches a session child (an authenticated Hello with fingerprint 0 is
            #     answered Refuse{WireFingerprint} by the child, i.e. it was forked for). RED with a
            #     single pool: the Hello is refused Busy "too many connections are awaiting
            #     authentication".
            holders = [connect_control(port, source='127.0.0.2') for _ in range(2)]
            try:
                time.sleep(.3)  # both accepted and pending
                # Not `exchange`: its Busy retry would turn the red shape into a timeout. No
                # session is live on this supervisor, so a Busy here can only be the queue's.
                with pair(port) as newcomer:
                    newcomer.settimeout(10)
                    newcomer.sendall(hello(schema, flatbuffers))
                    shared = receive(newcomer, schema)
                holders[0].settimeout(5)
                try:
                    displaced = receive(holders[0], schema)
                except (RuntimeError, OSError) as error:
                    displaced = {'error': repr(error)}
            finally:
                for holder in holders:
                    holder.close()
            assert shared.get('code') == 2 and shared.get('expected', 0), (
                f'a Hello from 127.0.0.1 while 127.0.0.2 held the whole pre-auth queue was answered {shared}')
            assert displaced.get('code') == 7 and displaced.get('detail') == kDetailDisplaced, displaced
            named = wait_for_log(serverLog, 'pending connection from 127.0.0.2 displaced by one from 127.0.0.1')
            assert named is not None, f'the displacement was not named in {serverLog}'
            evidence['pending_queue_is_shared'] = {'newcomer': shared, 'displaced': displaced, 'line': next(
                l.strip() for l in named.splitlines() if 'displaced by one from 127.0.0.1' in l)}
            time.sleep(.3)

            # (b) THE BACKOFF. Two wrong tokens from this address, and the THIRD connection - with
            #     the right token - is refused at accept, before its Hello is read, with the backoff's
            #     own words; once the window has passed the right token is welcomed again (and that
            #     clears the address). RED with the backoff disabled: the third is welcomed.
            fingerprint = learn_fingerprint(port, schema, flatbuffers)['expected']
            for _ in range(2):
                wrong = exchange(port, schema, hello(schema, flatbuffers, fingerprint=fingerprint, token='wrong-token'))
                assert wrong.get('code') == 4 and wrong.get('detail') == 'token mismatch', wrong
            backed = exchange(port, schema, hello(schema, flatbuffers, fingerprint=fingerprint))
            assert backed.get('code') == 4 and backed.get('detail') == kDetailBackoff, (
                f'two failed authentications from one address and the next Hello was answered {backed}')
            named = wait_for_log(serverLog, 'refused at accept: authentication backoff')
            assert named is not None, f'the backoff refusal was not named in {serverLog}'
            time.sleep(1.6)
            after = exchange(port, schema, hello(schema, flatbuffers, fingerprint=fingerprint))
            assert after.get('welcome', 0), f'the address was still refused after its window: {after}'
            evidence['auth_backoff'] = {'refusal': backed, 'line': next(
                l.strip() for l in named.splitlines() if 'refused at accept: authentication backoff' in l),
                'afterWindow': after}

            # (c) A SILENT SLOT HOLDER IS BACKED OFF (fix round). The critic's reproduction, with
            #     this supervisor's small knobs: 127.0.0.4 holds the whole queue with connections
            #     that never send a byte and closes them just before the deadline would have
            #     counted them. The first cut counted no close without a byte, so that address
            #     could repeat this for ever. A close after the probe grace (250 ms) is a failure
            #     now: after two, 127.0.0.4 is refused at accept - even with the right token.
            #     RED without the rule: that Hello is judged and forked for (Refuse{WireFingerprint}
            #     from the child, code 2).
            holders = [connect_control(port, source='127.0.0.4') for _ in range(2)]
            time.sleep(.6)  # past the grace, well inside the 3000 ms deadline
            for holder in holders:
                holder.close()
            time.sleep(.2)  # the supervisor sees both closes before the next connection
            with socket.create_connection(('127.0.0.1', port), timeout=3, source_address=('127.0.0.4', 0)) as held:
                held.settimeout(10)
                held.sendall(hello(schema, flatbuffers))
                try:
                    silent = receive(held, schema)
                except (RuntimeError, OSError) as error:
                    silent = {'error': repr(error)}
            assert silent.get('code') == 4 and silent.get('detail') == kDetailBackoff, (
                f'127.0.0.4 held the pre-auth queue twice without a byte and its next Hello was answered {silent}')
            reason = '127.0.0.4 failed pre-auth 2 times (last: held a pre-auth slot and closed without a byte)'
            named = wait_for_log(serverLog, reason)
            assert named is not None, f'the silent holder\'s failures were not named ("{reason}") in {serverLog}'
            evidence['silent_slot_holder_backed_off'] = {'refusal': silent, 'line': next(
                l.strip() for l in named.splitlines() if reason in l)}
            assert process.poll() is None, 'the pre-auth limits killed the supervisor'

        # ---- F fix round: THE SINGLE-SESSION SERVER CLOSES ITS LISTENER ONCE THE SESSION IS BOUND
        #
        # Without --serve the process is the session; it keeps the listener open only so the
        # session's data connection can arrive on it (ListenerSource). Until this control nothing
        # closed it afterwards, so the kernel went on completing TCP handshakes into a backlog no
        # accept() would ever drain: a second client, or a retry, hung for its whole connect budget
        # (kSpawnConnectTimeoutMs, 20 s) instead of the ECONNREFUSED it got before PH-7 (4). Now
        # RunSession closes the listener the moment Accept returns, and a connect after `ready`
        # is refused at once. RED before the fix: the connect below succeeds.
        with supervisor(args.server, args.out / 'single-session.log', serve=False) as (port, process, serverLog):
            with held_session(port, schema, flatbuffers,
                              hello(schema, flatbuffers, fingerprint=fingerprint)) as (control, welcome):
                assert welcome.get('welcome', 0), welcome
                ready = wait_for_log(serverLog, f'pid={welcome["welcome"]} transport=spawn role=server ready')
                assert ready is not None, f'the single session never came up; see {serverLog}'
                try:
                    second = socket.create_connection(('127.0.0.1', port), timeout=3)
                except ConnectionRefusedError as refused:
                    second = None
                    at_connect = repr(refused)
                else:
                    second.close()
                assert second is None, (
                    'the single-session server still completed a TCP handshake after its data '
                    'connection bound; the listener was not closed')
            evidence['single_session_listener'] = {'second_connect': at_connect}
        (args.out / 'protocol-controls.json').write_text(json.dumps(evidence, indent=2))
        print(json.dumps(evidence, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
