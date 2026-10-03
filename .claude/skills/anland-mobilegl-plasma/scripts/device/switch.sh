# switch.sh <DirectGLES|DirectVulkan> - (Android root shell) change the MobileGL backend the anland
# app's server runs (Espryt = DirectGLES, Magma = DirectVulkan). The choice is the app's setting
# (Settings > Connection > MobileGL desktop, saved in the app's files/mobilegl-backend); this does
# what a user does there: save it, Stop desktop, open the app again. The new server publishes the
# backend and the container's session copies it to /etc/mobilegl/backend before it starts, so
# server and clients agree. ~30-60 s. No setprop: debug.mobilegl.backend, if set, still overrides
# the setting until the next reboot - this clears it.
# Env: PKG, SOCK.
B=${1:?usage: switch.sh DirectGLES|DirectVulkan}
P=${PKG:-com.anland.consumer.mobilegl}
[ -n "$(getprop debug.mobilegl.backend)" ] && setprop debug.mobilegl.backend ""
if pidof $P:mobilegl > /dev/null; then
    am startservice -n $P/com.anland.consumer.MobileGLWorker -a com.anland.consumer.mobilegl.STOP > /dev/null
    i=0; while pidof $P:mobilegl > /dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i + 1)); done
fi
am start -n $P/com.anland.consumer.MainActivity --es mobilegl_backend $B > /dev/null
echo "backend $B saved; the desktop is starting (watch: sh $(dirname $0)/status.sh)"
