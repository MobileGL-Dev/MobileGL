#!/bin/bash
# Wi-Fi TCP throughput WSL -> phone, 3 reps, sink to /dev/null, nc restarted per rep.
set -u
PORT=5213
# Through a TCP-terminating proxy this measures the proxy, not Wi-Fi (the 09-28 run did).
bash "$(dirname "$0")/tcp_path_check.sh" || exit 1
for rep in 1 2 3; do
  adb -s 2f7cbe2e shell "pkill -f '^toybox nc' 2>/dev/null; nohup toybox nc -4 -l -p $PORT -q 180 >/dev/null 2>/data/local/tmp/nc.err </dev/null & echo up_$rep"
  sleep 1
  wsl -d Ubuntu -- bash -c "
python3 - <<'EOF'
import socket, time
MIB=1024*1024
data=b'\0'*MIB
total=256
try:
    s=socket.create_connection(('192.168.21.181',$PORT),timeout=10)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4*1024*1024)
    s.settimeout(120)
    t0=time.time(); sent=0
    while sent<total:
        s.sendall(data); sent+=1
    dt=time.time()-t0
    s.shutdown(socket.SHUT_WR); s.close()
    print(f'rep: {sent} MiB in {dt:.3f}s -> {sent/dt:.1f} MiB/s ({sent*8/dt/1000:.0f} Mbps)')
except Exception as e:
    print(f'rep: FAIL {e}')
EOF
"
  adb -s 2f7cbe2e shell "pkill -f '^toybox nc' 2>/dev/null" >/dev/null
  sleep 1
done
echo NETBENCH_DONE
