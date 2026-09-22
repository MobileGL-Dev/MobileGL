#!/usr/bin/env python3
"""Start/stop the loopback supervisor used by the integration-tcp CTest fixture."""
import argparse
import errno
import ipaddress
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time
from urllib.parse import urlsplit


def process_start(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[19]
    except FileNotFoundError:
        return None


def stop(state):
    if not state.exists():
        return
    saved = json.loads(state.read_text())
    pid = saved['pid']
    # PID reuse must never let a stale fixture file kill an unrelated process.
    if process_start(pid) == saved['start']:
        os.killpg(pid, signal.SIGTERM)
        for _ in range(50):
            if process_start(pid) != saved['start']:
                break
            time.sleep(.05)
        else:
            try:
                os.killpg(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    state.unlink()


def start(args):
    endpoint = urlsplit(args.endpoint)
    if endpoint.scheme != 'tcp' or not ipaddress.ip_address(endpoint.hostname).is_loopback:
        raise ValueError('the local fixture requires a numeric loopback TCP endpoint')
    family = socket.AF_INET6 if ':' in endpoint.hostname else socket.AF_INET
    address = (endpoint.hostname, endpoint.port)
    # Do not connect as a readiness probe: it would consume the first half of
    # AcceptPair, so the next real client would be paired with the probe.
    with socket.socket(family) as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(address)
    if args.state.exists():
        raise RuntimeError(f'stale fixture state; run stop first: {args.state}')
    env = dict(os.environ, MOBILEGL_IPC_ROLE='server', MOBILEGL_IPC_DIAL='no',
               MOBILEGL_IPC_TOKEN='', MOBILEGL_IPC_LOG_FORWARD='1')
    for key in ('MOBILEGL_IPC_CONTROL', 'MOBILEGL_IPC_ENDPOINT', 'MOBILEGL_BACKEND_TYPE'):
        env.pop(key, None)
    log_path = args.state.with_suffix('.log')
    with log_path.open('wb') as log:
        child = subprocess.Popen([args.server, args.endpoint, '--serve'], env=env,
                                 stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                 start_new_session=True)
    args.state.write_text(json.dumps({'pid': child.pid, 'start': process_start(child.pid)}))
    try:
        for _ in range(100):
            if child.poll() is not None:
                raise RuntimeError(f'supervisor exited {child.returncode}: {log_path.read_text(errors="replace")}')
            with socket.socket(family) as probe:
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                try:
                    probe.bind(address)
                except OSError as error:
                    if error.errno == errno.EADDRINUSE:
                        print(f'TCP supervisor pid={child.pid} endpoint={args.endpoint}')
                        return
                    raise
            time.sleep(.05)
        raise RuntimeError(f'supervisor did not listen: {log_path}')
    except BaseException:
        stop(args.state)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('start', 'stop'))
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--server')
    parser.add_argument('--endpoint')
    args = parser.parse_args()
    if args.command == 'start':
        if not args.server or not args.endpoint:
            parser.error('start requires --server and --endpoint')
        start(args)
    else:
        stop(args.state)


if __name__ == '__main__':
    main()
