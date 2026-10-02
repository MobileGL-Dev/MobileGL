# restart-daemon.sh - (Android root shell) (re)start the EXPERIMENT's anland display daemon on $SOCK.
# It is not started at boot (the anland-daemon module's service.sh only starts the original daemon on
# /data/local/tmp/display_daemon.sock, which must be left alone).  Without it KWin logs
# "failed to connect to display daemon at /run/anland-mobilegl/display.sock" and the session dies in ~3 s.
# The container sees $SOCK as /run/anland-mobilegl/display.sock (bind mount in container.config);
# uid 1000 in the container must connect, hence 777 dir / 666 socket.
# Env: SOCK, DAEMON (/data/adb/modules/anland-daemon/display_daemon).
SOCK=${SOCK:-/data/local/tmp/anland-mobilegl/display.sock}
DAEMON=${DAEMON:-/data/adb/modules/anland-daemon/display_daemon}
DIR=$(dirname $SOCK)
for p in $(ps -A -o PID,ARGS | grep "display_daemon $SOCK" | grep -v grep | awk '{print $1}'); do kill $p; done
sleep 1
rm -f $SOCK
mkdir -p $DIR
nohup $DAEMON $SOCK > $DIR/display-daemon.log 2>&1 &
sleep 1
chmod 777 $DIR
chmod 666 $SOCK
echo "daemon restarted:"
ps -A -o PID,ARGS | grep "display_daemon $SOCK" | grep -v grep
