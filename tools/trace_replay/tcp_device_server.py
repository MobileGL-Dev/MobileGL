#!/usr/bin/env python3
"""Start or stop the trace APK's foreground TCP supervisor through adb."""
import argparse
import shlex
import subprocess


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
    args = parser.parse_args()
    adb = [args.adb, '-s', args.serial]
    if args.action == 'stop':
        command = ['am', 'force-stop', args.package]
    else:
        command = ['am', 'start-foreground-service', '-n', args.package + '/top.mobilegl.plugin.MobileGLServerService',
                   '--es', 'listen', args.listen, '--es', 'token', args.token,
                   '--es', 'env', ';'.join(args.env)]
    subprocess.run(adb + ['shell', shlex.join(command)], check=True)
    if args.action == 'start' and args.forward:
        subprocess.run(adb + ['forward', 'tcp:40613', 'tcp:40613'], check=True)
    if args.action == 'start':
        print('Read readiness: adb -s ' + args.serial + ' logcat -s MobileGLServer')
        print('Client: MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_DATA=stream '
              'MOBILEGL_IPC_REQUIRE_SAME_BUILD=1 MOBILEGL_IPC_CONTROL=tcp://<device-address>:40613')


if __name__ == '__main__':
    main()
