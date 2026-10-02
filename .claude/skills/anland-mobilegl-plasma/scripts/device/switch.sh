# switch.sh <DirectGLES|DirectVulkan> - (Android root shell) select the server backend: Espryt =
# DirectGLES, Magma = DirectVulkan.  Sets the system property the server reads (lost on reboot), writes
# the container's /etc/mobilegl/backend (what every client and the KWin drop-in report; it must match or
# the server refuses the client: "Hello.backendType disagrees with pinned backend"), restarts the app.
# The running Plasma session still speaks the old backend: run run-plasma.sh afterwards.
# Env: PKG, CONTAINER (arch-kde-mgl), SOCK (display daemon socket).
B=${1:?usage: switch.sh DirectGLES|DirectVulkan}
P=${PKG:-com.anland.consumer.mobilegl}
C=${CONTAINER:-arch-kde-mgl}
SOCK=${SOCK:-/data/local/tmp/anland-mobilegl/display.sock}
setprop debug.mobilegl.backend $B
echo $B > /mnt/Droidspaces/$C/etc/mobilegl/backend
am force-stop $P
am start -n $P/com.anland.consumer.MainActivity --es socket_path $SOCK >/dev/null
sleep 4
