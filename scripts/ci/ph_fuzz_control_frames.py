#!/usr/bin/env python3
"""Ph fuzz arm 1: malformed control frames against a live TCP supervisor, each refused by name.

PLAN-PH-P34B-P7.md §1.1, row "gate: fuzz arm 1 malformed control frames": the mechanism was in the
tree (Framing.h's latch, ControlInbox) and the end-to-end half was not. This is that half. It is
registered as `TcpLane.ControlFrameFuzz` (label integration-tcp) and required PASSED by CI the way
`TcpLane.SupervisorProtocolControls` is - a SKIP there is red.

Five malformations, delivered at the two places a control frame is read:

  bad_identifier           a well-formed Hello envelope finished with the file identifier 'XXXX'
  truncated_length_prefix  five bytes of the eight-byte frame header, then the peer's write side
                           closes
  oversize_length          'MGLF' and a length of 0xFFFFFFF0
  garbage_flatbuffer       a frame whose payload carries our identifier and a root offset that
                           points nowhere, so only the verifier can refuse it
  wrong_union_type         a VALID envelope of a type that is not expected there (a LogFlush as a
                           first frame; a DataBind in an established session)

(1) FIRST FRAME, read by the supervisor before it has forked anything (PH-7 (5)). Each must be
    answered with a Refuse frame naming the shape - never a bare close - with NO session child
    forked for it (/proc, and no reap line), and the supervisor must go on serving: after all of
    them, a well-formed client is welcomed and its session comes up. Before PH-7 (5) every one of
    these forked a child that `_exit(67)`ed without a word (or, for the oversize length, after
    the frame reader refused to allocate), counted as a faulted session. Three extra shapes ride
    along because they are cheap and each has its own words: a wrong magic, the 24-byte envelope
    whose tag says Hello and whose table is NULL, and a peer that says nothing at all.

(2) STEADY STATE, read by an authenticated session child after Welcome. Each must end the session
    with a named line in the server log - `WireLogError`, since a Refuse is a handshake answer and
    the session is past its handshake - and the child must exit 0 (the peer's malformation is not
    the server's fault); the next client is then welcomed. Before this package two of the five
    (the truncated frame and the wrong union type) ended the session without a word.

(3) SINGLE-SESSION SHAPE (no --serve), where the session process reads its own first frame
    through the same classification: a wrong identifier and a silent peer are each answered by
    name and the process exits 0, where it used to `_exit(67)` without a word.

The supervisor runs with the authentication backoff OFF (MOBILEGL_IPC_AUTH_BACKOFF_AFTER=0): every
malformation above is an authentication failure from 127.0.0.1, and with the default threshold the
closing well-formed client would be backed off - which TcpLane.SupervisorProtocolControls' own
`auth_backoff` control asserts on purpose, and which this arm is not about.

Exit 77 (SKIP) without flatc or without the server image, like the supervisor smoke.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tcp_supervisor_smoke as smoke  # noqa: E402  (the shared helpers and the mirrored details)


def framed(payload):
    return struct.pack('<4sI', b'MGLF', len(payload)) + payload


def envelope(schema, flatbuffers, kind, build_body=None, identifier=b'MGLC'):
    """A framed CtrlEnvelope of union type `kind`; `build_body(builder)` returns its table, or
    None for the NULL-table shape."""
    builder = flatbuffers.Builder(128)
    body = build_body(builder) if build_body else None
    root = schema['CtrlEnvelope']
    root.Start(builder)
    root.AddMsgType(builder, kind)
    if body is not None:
        root.AddMsg(builder, body)
    builder.Finish(root.End(builder), identifier)
    return framed(bytes(builder.Output()))


def log_flush(schema, flatbuffers):
    def body(builder):
        message = schema['LogFlush']
        message.Start(builder)
        message.AddSeq(builder, 1)
        return message.End(builder)
    return envelope(schema, flatbuffers, schema['CtrlMsg'].CtrlMsg.LogFlush, body)


def data_bind_envelope(schema, flatbuffers):
    def body(builder):
        vector = builder.CreateByteVector(os.urandom(16))
        message = schema['DataBind']
        message.Start(builder)
        message.AddNonce(builder, vector)
        return message.End(builder)
    return envelope(schema, flatbuffers, schema['CtrlMsg'].CtrlMsg.DataBind, body)


def garbage_flatbuffer():
    # Root offset 0xFFFFFF00 (far past the end), then OUR identifier: the identifier check passes
    # and the verifier is the one that has to say no. Deterministic, so a green run is not luck.
    return framed(struct.pack('<I', 0xFFFFFF00) + b'MGLC' + bytes(range(56)))


def malformations(schema, flatbuffers, fingerprint):
    """name -> (bytes to send, close the write side afterwards)."""
    return {
        'bad_identifier': (smoke.hello(schema, flatbuffers, fingerprint=fingerprint, identifier=b'XXXX'), False),
        'truncated_length_prefix': (b'MGLF\x10', True),
        'oversize_length': (b'MGLF' + struct.pack('<I', 0xFFFFFFF0) + bytes(56), False),
        'garbage_flatbuffer': (garbage_flatbuffer(), False),
        'wrong_union_type': (log_flush(schema, flatbuffers), False),
    }


def send_first_frame(port, schema, payload, half_close):
    """One connection, one malformed first frame, and whatever the supervisor answers."""
    peer = smoke.connect_control(port)
    try:
        peer.settimeout(10)
        if payload:
            peer.sendall(payload)
        if half_close:
            peer.shutdown(socket.SHUT_WR)
        began = time.monotonic()
        try:
            answer = smoke.receive(peer, schema)
        except (RuntimeError, OSError) as error:
            answer = {'error': repr(error)}
        answer['seconds'] = round(time.monotonic() - began, 3)
        return answer
    finally:
        peer.close()


def reap_lines(path):
    return path.read_text(errors='replace').count(' reaped ') if path.is_file() else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True)
    parser.add_argument('--flatc', default='')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    flatc = smoke.resolve_flatc(args.flatc)
    if flatc is None:
        print('ph_fuzz_control_frames: no flatc found (--flatc, $MOBILEGL_FLATC_EXECUTABLE, '
              '<repo>/../flatc-build/flatc); skipping', flush=True)
        return smoke.kSkipExit
    if not os.path.isfile(args.server):
        print(f'ph_fuzz_control_frames: no server image at {args.server}; skipping', flush=True)
        return smoke.kSkipExit
    evidence = {'first_frame': {}, 'steady_state': {}}
    with tempfile.TemporaryDirectory(prefix='mgl-fuzz-schema-') as directory:
        flatbuffers, schema = smoke.load_schema(flatc, directory)
        knobs = {'MOBILEGL_IPC_AUTH_BACKOFF_AFTER': '0', 'MOBILEGL_IPC_PREAUTH_MS': '1500'}
        with smoke.supervisor(args.server, args.out / 'fuzz-supervisor.log', extra_env=knobs) as (
                port, process, serverLog):
            fingerprint = smoke.learn_fingerprint(port, schema, flatbuffers)['expected']
            # The child that answered it is reaped before the phase starts counting reaps (and its
            # reap line, written just after the waitpid, has landed).
            smoke.wait_for_no_session(process.pid)
            time.sleep(.2)

            # ---- (1) FIRST FRAME: the supervisor, before any fork ---------------------------------
            expected = {
                'bad_identifier': (8, smoke.kDetailNoIdentifier),
                'truncated_length_prefix': (8, smoke.kDetailTruncated),
                'oversize_length': (8, smoke.kDetailNotAFrame),
                'garbage_flatbuffer': (8, smoke.kDetailUnverifiable),
                'wrong_union_type': (8, smoke.kDetailNotAHello),
                'bad_magic': (8, smoke.kDetailNotAFrame),
                'null_hello_table': (8, smoke.kDetailNotAHello),
                'silent': (4, smoke.kDetailDeadline),
            }
            cases = dict(malformations(schema, flatbuffers, fingerprint))
            cases['bad_magic'] = (b'XXXX' + bytes(60), False)
            cases['null_hello_table'] = (envelope(schema, flatbuffers, schema['CtrlMsg'].CtrlMsg.Hello), False)
            cases['silent'] = (b'', False)
            reaped_before = reap_lines(serverLog)
            for name, (payload, half_close) in cases.items():
                answer = send_first_frame(port, schema, payload, half_close)
                forked = smoke.child_pids(process.pid)
                code, detail = expected[name]
                assert answer.get('code') == code and answer.get('detail') == detail, (
                    f'first-frame {name}: expected Refuse code {code} "{detail}", got {answer}; see {serverLog}')
                assert not forked, f'first-frame {name}: the supervisor forked {forked} for it'
                assert process.poll() is None, f'first-frame {name} killed the supervisor'
                evidence['first_frame'][name] = answer
            text = serverLog.read_text(errors='replace')
            for name, (code, detail) in expected.items():
                word = 'MalformedHello' if code == 8 else 'Authentication'
                assert f'Refuse{{{word}}} {detail}' in text, (
                    f'first-frame {name}: the supervisor log does not name "Refuse{{{word}}} {detail}"')
            assert reap_lines(serverLog) == reaped_before, (
                'a session child was reaped during the first-frame phase, so one was forked for a '
                f'malformed peer; see {serverLog}')

            # A well-formed client after all of them: welcomed, and its session comes up.
            with smoke.held_session(port, schema, flatbuffers,
                                    smoke.hello(schema, flatbuffers, fingerprint=fingerprint)) as (control, welcome):
                assert welcome.get('welcome', 0), welcome
                ready = smoke.wait_for_log(serverLog, f'pid={welcome["welcome"]} transport=spawn role=server ready')
                assert ready is not None, f'the well-formed client after the first-frame phase never came up'
            evidence['first_frame']['then_well_formed'] = {'welcome': welcome['welcome']}

            # ---- (2) STEADY STATE: an authenticated session child, after Welcome ---------------
            steady = malformations(schema, flatbuffers, fingerprint)
            # A LogFlush is a type the steady-state loop DOES take, so the wrong union type there
            # is a DataBind - valid, verifiable, and not a message a session reads on control.
            steady['wrong_union_type'] = (data_bind_envelope(schema, flatbuffers), False)
            named = {
                'bad_identifier': 'control frame carries no CtrlEnvelope identifier',
                'truncated_length_prefix': 'control connection closed inside a frame',
                'oversize_length': 'frame length 4294967280 exceeds',
                'garbage_flatbuffer': 'control frame did not verify as a CtrlEnvelope',
                'wrong_union_type': 'is not a LogFlush or SurfaceOp',
            }
            for name, (payload, half_close) in steady.items():
                with smoke.held_session(port, schema, flatbuffers,
                                        smoke.hello(schema, flatbuffers, fingerprint=fingerprint)) as (control, welcome):
                    assert welcome.get('welcome', 0), (name, welcome)
                    child = welcome['welcome']
                    ready = smoke.wait_for_log(serverLog, f'pid={child} transport=spawn role=server ready')
                    assert ready is not None, f'steady-state {name}: session pid={child} never came up'
                    before = len(serverLog.read_text(errors='replace'))
                    control.sendall(payload)
                    if half_close:
                        control.shutdown(socket.SHUT_WR)
                    reaped = smoke.wait_for_log(serverLog, f'pid={child} reaped exit=')
                assert reaped is not None, f'steady-state {name}: session pid={child} did not end'
                said = reaped[before:]
                assert named[name] in said, (
                    f'steady-state {name}: session pid={child} ended without naming why '
                    f'(wanted "{named[name]}"); see {serverLog}')
                assert f'pid={child} reaped exit=0 ' in reaped, (
                    f'steady-state {name}: session pid={child} was counted as a fault')
                evidence['steady_state'][name] = next(
                    l.strip() for l in said.splitlines() if named[name] in l)
                assert process.poll() is None, f'steady-state {name} killed the supervisor'

            # And the supervisor still serves.
            with smoke.held_session(port, schema, flatbuffers,
                                    smoke.hello(schema, flatbuffers, fingerprint=fingerprint)) as (control, welcome):
                assert welcome.get('welcome', 0), welcome
                ready = smoke.wait_for_log(serverLog, f'pid={welcome["welcome"]} transport=spawn role=server ready')
                assert ready is not None, 'the well-formed client after the steady-state phase never came up'
            evidence['steady_state']['then_well_formed'] = {'welcome': welcome['welcome']}

        # ---- (3) THE SINGLE-SESSION SHAPE: the process that IS the session reads its own first frame
        #
        # Without --serve there is no supervisor to read for it: RunSession reads the first frame
        # itself (the unix endpoint's children do the same), through the same classification.
        # That read used to `_exit(67)` on every malformation without a word; now the peer is
        # answered by name and the process exits 0 - the malformation is the peer's, not a fault.
        evidence['single_session'] = {}
        for name, payload, code, detail in (
                ('bad_identifier', smoke.hello(schema, flatbuffers, fingerprint=fingerprint, identifier=b'XXXX'),
                 8, smoke.kDetailNoIdentifier),
                ('silent', b'', 4, smoke.kDetailDeadline)):
            with smoke.supervisor(args.server, args.out / f'single-session-{name}.log', serve=False,
                                  extra_env=knobs) as (port, process, serverLog):
                answer = send_first_frame(port, schema, payload, False)
                try:
                    exited = process.wait(timeout=10)
                except Exception:  # noqa: BLE001 - still running is the red shape here
                    exited = None
            assert answer.get('code') == code and answer.get('detail') == detail, (
                f'single-session {name}: expected Refuse code {code} "{detail}", got {answer}; see {serverLog}')
            assert exited == 0, f'single-session {name}: the session process exited {exited}, not 0'
            evidence['single_session'][name] = {'refusal': answer, 'exit': exited}
        (args.out / 'fuzz-control-frames.json').write_text(json.dumps(evidence, indent=2))
        print(json.dumps(evidence, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
