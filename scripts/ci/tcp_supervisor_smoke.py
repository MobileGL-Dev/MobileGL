#!/usr/bin/env python3
"""Exercise a real TCP supervisor's Busy, token, version, and build-policy controls."""
import argparse
from contextlib import contextmanager
import importlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def read_exact(peer, size):
    result = bytearray()
    while len(result) < size:
        chunk = peer.recv(size - len(result))
        if not chunk:
            raise RuntimeError('control channel closed before a complete frame')
        result.extend(chunk)
    return result


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
        return {'welcome': welcome.ServerPid(), 'fingerprint': welcome.WireFingerprint()}
    raise RuntimeError(f'unexpected first control reply {kind}')


def hello(schema, flatbuffers, fingerprint=0, major=1, token='p65-smoke-token'):
    builder = flatbuffers.Builder(512)
    build = builder.CreateString('intentionally-different-build-for-p65-control')
    auth = builder.CreateString(token)
    terms = schema['LinkTerms']
    terms.Start(builder)
    terms.AddDataPlane(builder, 1)
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
    builder.Finish(envelope.End(builder), b'MGLC')
    payload = bytes(builder.Output())
    return struct.pack('<4sI', b'MGLF', len(payload)) + payload


@contextmanager
def supervisor(server, log, same_build=False):
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    env = dict(os.environ, MOBILEGL_IPC_ROLE='server', MOBILEGL_IPC_DIAL='no',
               MOBILEGL_IPC_TOKEN='p65-smoke-token', MOBILEGL_IPC_REQUIRE_SAME_BUILD=str(int(same_build)),
               MOBILEGL_IPC_LOG_FORWARD='0', MOBILEGL_LOG_FILE_PATH=str(log.with_suffix('.library.log')))
    for name in ('MOBILEGL_TRANSPORT', 'MOBILEGL_IPC_SERVER_PATH', 'MOBILEGL_IPC_RING_MB', 'MOBILEGL_IPC_STAGE_MB',
                 'MOBILEGL_IPC_CONTROL', 'MOBILEGL_IPC_ENDPOINT', 'MOBILEGL_BACKEND_TYPE'):
        env.pop(name, None)
    with log.open('wb') as output:
        child = subprocess.Popen([server, f'tcp://127.0.0.1:{port}', '--serve'], env=env,
                                 stdout=output, stderr=output, start_new_session=True)
    try:
        yield port, child
    finally:
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        child.wait(timeout=10)


@contextmanager
def pair(port):
    deadline = time.monotonic() + 5
    while True:
        try:
            control = socket.create_connection(('127.0.0.1', port), timeout=3)
            break
        except ConnectionRefusedError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.02)
    try:
        data = socket.create_connection(('127.0.0.1', port), timeout=3)
        try:
            yield control
        finally:
            data.close()
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True)
    parser.add_argument('--flatc', default='flatc')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='mgl-tcp-schema-') as directory:
        subprocess.run([args.flatc, '--python', '-o', directory,
                        str(ROOT / 'MobileGL/MG_Remote/Protocol/protocol.fbs')], check=True)
        sys.path[:0] = [directory, str(ROOT / '3rdparty/flatbuffers/python')]
        import flatbuffers
        schema = {name: importlib.import_module('MobileGL.Wire.' + name)
                  for name in ('CtrlEnvelope', 'CtrlMsg', 'Hello', 'LinkTerms', 'Refuse', 'Welcome')}
        evidence = {}
        with supervisor(args.server, args.out / 'supervisor.log') as (port, process):
            with pair(port) as held:
                with pair(port) as second:
                    busy = receive(second, schema)
                    assert busy.get('code') == 7, busy
                    evidence['busy'] = busy
            layout = exchange(port, schema, hello(schema, flatbuffers))
            assert layout.get('code') == 2 and layout.get('expected', 0), layout
            fingerprint = layout['expected']
            evidence['wrong_layout'] = layout
            token = exchange(port, schema, hello(schema, flatbuffers, fingerprint=fingerprint, token='wrong-token'))
            assert token.get('code') == 4, token
            evidence['wrong_token'] = token
            version = exchange(port, schema, hello(schema, flatbuffers, major=99))
            assert version.get('code') == 1, version
            evidence['wire_major_99'] = version
            accepted = exchange(port, schema, hello(schema, flatbuffers, fingerprint=fingerprint))
            assert accepted.get('welcome', 0) and accepted['welcome'] != os.getpid(), accepted
            assert accepted.get('fingerprint') == fingerprint, accepted
            evidence['different_build_connect'] = accepted
            assert process.poll() is None, 'a rejected session killed the supervisor'
        with supervisor(args.server, args.out / 'same-build-supervisor.log', same_build=True) as (port, _):
            strict = exchange(port, schema, hello(schema, flatbuffers, fingerprint=fingerprint))
            assert strict.get('code') == 3, strict
            evidence['same_build_required'] = strict
        (args.out / 'protocol-controls.json').write_text(json.dumps(evidence, indent=2))
        print(json.dumps(evidence, indent=2))


if __name__ == '__main__':
    main()
