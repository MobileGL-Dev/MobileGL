#!/usr/bin/env python3
"""host-inproc-server.py <libMobileGL.so> <endpoint>: the in-process display server entry
(mobilegl_server_serve_inprocess) hosted by a plain host process, no display installed - the host
twin of MobileGLDisplayActivity's server thread, for reproducing sequential-session behaviour."""
import ctypes, os, sys, signal, threading
lib_path, endpoint = sys.argv[1], sys.argv[2]
for k in ("MOBILEGL_TRANSPORT", "MOBILEGL_IPC_SERVER_PATH", "MOBILEGL_IPC_RING_MB", "MOBILEGL_IPC_STAGE_MB",
          "MOBILEGL_IPC_CONTROL", "DISPLAY", "WAYLAND_DISPLAY"):
    os.environ.pop(k, None)
os.environ.update(MOBILEGL_IPC_ROLE="server", MOBILEGL_IPC_DIAL="no", MOBILEGL_IPC_LOG_FORWARD="1")
os.environ.setdefault("EGL_PLATFORM", "surfaceless")
lib = ctypes.CDLL(lib_path)
lib.mobilegl_server_serve_inprocess.argtypes = [ctypes.c_char_p]
lib.mobilegl_server_serve_inprocess.restype = ctypes.c_int
def stop(*_):
    lib.mobilegl_server_stop_inprocess()
signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
rc = [None]
t = threading.Thread(target=lambda: rc.__setitem__(0, lib.mobilegl_server_serve_inprocess(endpoint.encode())))
t.start()
while t.is_alive():
    t.join(0.2)
print("serve returned", rc[0], flush=True)
sys.exit(rc[0] or 0)
