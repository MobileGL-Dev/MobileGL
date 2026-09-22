#!/usr/bin/env python3
"""Start or stop the trace APK's foreground TCP supervisor through adb."""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess


def shell(adb, command):
    return subprocess.run(adb + ['shell', shlex.join(command)], check=True,
                          capture_output=True, text=True).stdout


def idle_whitelisted(adb, package):
    entries = shell(adb, ['dumpsys', 'deviceidle', 'whitelist'])
    return any(package in line.strip().split(',') for line in entries.splitlines())


def allow_idle(adb, serial, package, state_path):
    if state_path.exists():
        saved = json.loads(state_path.read_text())
        if saved['serial'] != serial or saved['package'] != package:
            raise ValueError('idle state belongs to a different device or package')
    else:
        saved = {'serial': serial, 'package': package,
                 'originally_whitelisted': idle_whitelisted(adb, package)}
        state_path.parent.mkdir(parents=True, exist_ok=True)
        # Save the original state before the device mutation, including if the
        # command's response is lost and a later stop has to recover it.
        state_path.write_text(json.dumps(saved, indent=2))
    if not idle_whitelisted(adb, package):
        shell(adb, ['dumpsys', 'deviceidle', 'whitelist', '+' + package])
    if not idle_whitelisted(adb, package):
        raise RuntimeError('the package was not added to the device idle whitelist')
    print(f'Idle exemption recorded in {state_path}; stop restores the original package setting')


def restore_idle(adb, serial, package, state_path):
    if not state_path.exists():
        return
    saved = json.loads(state_path.read_text())
    if saved['serial'] != serial or saved['package'] != package:
        raise ValueError('idle state belongs to a different device or package')
    if not saved['originally_whitelisted']:
        shell(adb, ['dumpsys', 'deviceidle', 'whitelist', '-' + package])
        if idle_whitelisted(adb, package):
            raise RuntimeError('the package idle exemption could not be restored')
    state_path.unlink()
    print('Restored the original device idle whitelist setting for ' + package)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('start', 'stop'))
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial', default='2f7cbe2e')
    parser.add_argument('--package', default='top.mobilegl.plugin.trace')
    parser.add_argument('--listen', default='tcp://127.0.0.1:40613')
    parser.add_argument('--token', default='')
    parser.add_argument('--env', action='append', default=[], help='KEY=VALUE;KEY overrides (repeatable)')
    parser.add_argument('--forward', action='store_true', help='Forward local adb port 40613 to the device')
    parser.add_argument('--allow-idle', action='store_true',
                        help='Temporarily exempt this package from Doze; stop restores its previous setting')
    parser.add_argument('--state-file', type=Path,
                        help='Saved original idle setting (default: ~/.cache/mobilegl/tcp-server/<serial>-<package>.json)')
    args = parser.parse_args()
    adb = [args.adb, '-s', args.serial]
    state_name = re.sub(r'[^A-Za-z0-9_.-]', '_', args.serial + '-' + args.package) + '.json'
    state_path = args.state_file or (Path.home() / '.cache/mobilegl/tcp-server' / state_name)
    if args.action == 'stop':
        command = ['am', 'force-stop', args.package]
    else:
        if args.allow_idle:
            allow_idle(adb, args.serial, args.package, state_path)
        command = ['am', 'start-foreground-service', '-n', args.package + '/top.mobilegl.plugin.MobileGLServerService',
                   '--es', 'listen', args.listen, '--es', 'token', args.token,
                   '--es', 'env', ';'.join(args.env)]
    print(shell(adb, command), end='')
    if args.action == 'stop':
        restore_idle(adb, args.serial, args.package, state_path)
    if args.action == 'start' and args.forward:
        subprocess.run(adb + ['forward', 'tcp:40613', 'tcp:40613'], check=True)
    if args.action == 'start':
        print('Read readiness: adb -s ' + args.serial + ' logcat -s MobileGLServer')
        print('Client: MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_DATA=stream '
              'MOBILEGL_IPC_REQUIRE_SAME_BUILD=1 MOBILEGL_IPC_CONTROL=tcp://<device-address>:40613')


if __name__ == '__main__':
    main()
