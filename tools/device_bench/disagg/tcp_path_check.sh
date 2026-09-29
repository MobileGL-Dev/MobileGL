#!/bin/bash
# Is the WSL -> phone TCP path end to end? Run before any cross-host measurement.
#
# A TUN-mode proxy on the Windows host (v2rayN's xray_tun here, WSL's default route in mirrored
# mode) terminates every TCP connection itself and relays it: it ACKs locally (sub-ms "RTT",
# PMTU 9000), and on 2026-09-28 its relay stalled the MobileGL control stream at the same byte
# every run, which read as a server hang at eglMakeCurrent. Probe: connect to a port nothing
# listens on. End to end the phone answers RST (refused); a terminating middlebox accepts.
# Exit 0 = end to end, 1 = middlebox, 2 = cannot tell (filtered).
set -u
export MSYS_NO_PATHCONV=1
PHONE="${PHONE:-192.168.21.181}"
PROBE_PORT="${PROBE_PORT:-1}"
wsl.exe -d "${WSL_DISTRO:-Ubuntu}" -- bash -c "
ip -o route get $PHONE | head -1
python3 - <<'PY'
import socket, sys
s = socket.socket(); s.settimeout(3)
try:
    s.connect(('$PHONE', $PROBE_PORT))
except ConnectionRefusedError:
    print('OK: end to end (closed port $PROBE_PORT refused)'); sys.exit(0)
except socket.timeout:
    print('UNKNOWN: no answer in 3 s (filtered)'); sys.exit(2)
print('FAIL: closed port $PROBE_PORT accepted - something between WSL and $PHONE terminates TCP '
      '(TUN proxy?). Route around it as root, e.g. ip route replace $PHONE/32 via <LAN gateway> dev eth0')
sys.exit(1)
PY"
