#!/bin/bash
# Phone-side: attach strace (as app uid) to the session child for 6s, dump head.
set -u
PID=$(adb -s 2f7cbe2e shell "ps -A -o PID,NAME,ELAPSED | awk '\$2==\"libMobileGLServer.so\" && \$1>15000 {print \$1}'" | tr -d '\r' | head -1)
echo "target child pid=$PID"
[ -z "$PID" ] && { echo NO_CHILD; exit 1; }
adb -s 2f7cbe2e shell "run-as top.mobilegl.plugin.trace sh -c '/system/bin/strace -p $PID -f -tt -e trace=recvfrom,recvmsg,read,write,writev,futex,ppoll,pselect6,epoll_wait,epoll_pwait -s 80 -o /data/data/top.mobilegl.plugin.trace/files/st.txt 2>/data/data/top.mobilegl.plugin.trace/files/st.err & S=\$!; sleep 6; kill \$S 2>/dev/null; wait \$S 2>/dev/null; echo ===ERR===; cat /data/data/top.mobilegl.plugin.trace/files/st.err; echo ===ST===; wc -l /data/data/top.mobilegl.plugin.trace/files/st.txt; head -150 /data/data/top.mobilegl.plugin.trace/files/st.txt'" 2>&1
