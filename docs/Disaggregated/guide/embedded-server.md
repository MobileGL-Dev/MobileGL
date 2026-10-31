# Embedding the split server in a host APK

A host application can load `libMobileGL.so` directly and hand the server an `ANativeWindow` it owns.
The window, input and the Android Service lifecycle stay with the host APK; no MobileGL plugin APK is
needed. The public interface is
[`EmbeddedServer.h`](../../../MobileGL/MG_Remote/Server/EmbeddedServer.h), uses only C types, and can
also be called through `dlsym`. The build must enable `MOBILEGL_BUILD_DISAGGREGATED`.

## Compositor service with a window

Before loading the library, set `MOBILEGL_IPC_ROLE=server` and `MOBILEGL_IPC_DIAL=no`, and clear
`MOBILEGL_TRANSPORT`, `MOBILEGL_IPC_SERVER_PATH`, `MOBILEGL_IPC_RING_MB` and
`MOBILEGL_IPC_STAGE_MB`. Running in a separate service process of the host APK is recommended, so that
a rendering fault does not take down the UI process.

1. Call `mobilegl_server_display_install_android(request_geometry, user)`.
   The callback may be null; when present it runs on the apply thread, must return quickly, and hands
   size changes to the host thread.
2. Take an `ANativeWindow` from the host's `Surface` and call
   `mobilegl_server_display_attach_android(window, width, height)`.
   The server holds its own reference; the caller then releases its own. Attaching the same window
   again only updates the size.
3. On the service thread, call `mobilegl_server_serve_inprocess("@anland-mobilegl-compositor")`.
   This accepts one client session in the current process. The AF_UNIX control connection passes the
   shared-memory segment with `SCM_RIGHTS`, and data travels over a SHM ring. The client sets
   `MOBILEGL_TRANSPORT=spawn`, `MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl-compositor`,
   `MOBILEGL_IPC_DATA=shm` and `MOBILEGL_IPC_SURFACE=server`.
4. When the Surface is destroyed, call `mobilegl_server_display_detach(3000)`.
   The return values `0/1/2/3` mean no window, released directly, completed after the session
   released it, and timed out. On timeout the reference is kept until the apply thread stops using the
   window; losing the window currently ends the active session.
5. When the service stops, call `mobilegl_server_stop_inprocess()`, wait for the serve thread to
   return, then call `mobilegl_server_display_uninstall(3000)`. The geometry callback's user object
   must live at least until then.

When `width/height` are zero, the Android attach queries the size from the `ANativeWindow`. A positive
size in the geometry callback requests fixed buffer dimensions; `0/0` means restore the host's layout
size, and the host reports the actual result through a later attach. Install, attach, detach and
uninstall are serialized by the host. The library refuses to replace an installed display or an old
window lease that has not ended, so that a late release cannot use another set of reference callbacks.

## Concurrent off-screen application service

A process still has only one set of backend / GL state. A service with a window accepts a single
compositor; other EGL clients can connect to the `libMobileGLServer.so` executable packaged by the
host APK, which another service process starts and stops:

```text
<nativeLibraryDir>/libMobileGLServer.so @anland-mobilegl --serve --max-sessions 16
```

`--max-sessions` is the limit on active sessions; it accepts `1..64` and still defaults to `1`. Values
above `1` apply only to the AF_UNIX `--serve` off-screen supervisor. Each client forks its own worker
with an independent backend, isolated from other sessions in shared memory; a worker returning frees
its slot, and all workers end when the supervisor ends. These workers have no display; the client
should clear `MOBILEGL_IPC_SURFACE` and submit window images through its own WSI.

Process isolation solves multiple applications connecting at the same time; true multi-context /
share-group support inside one application still needs separate work.
