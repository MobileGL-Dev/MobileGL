# Server-side surface path and both backends' window surfaces (commit 9f669e52)

Everything below was read in code unless it is marked **[INFER]**. Nothing was edited or built.

## 1. How the control/surface ops travel today [VERIFIED]

**Wire schema**
- `SurfaceOpKind` is 0..11 (`protocol.fbs:218-237`). `WindowKind` is `None=0, AndroidNativeWindow=1, X11=2, Win32Hwnd=3, Surfaceless=4, Pbuffer=5, MetalLayer=6` (`:239-250`). Both are append-only.
- `SurfaceOp` carries `seq, kind, display, surface, windowKind, nativeToken, width, height, swapInterval, readSurface, context` (`:252-268`). The comment says "Android transfers the window out of band" (`:258`).
- `SurfaceReply.defaultFb` still exists in the schema (`:275`), but the encoder writes 0 (`SurfaceOpCodec.cpp:192-193`) and CONTRACT-P6.md:75 calls the slot burned.
- `SurfaceProgress` is at `:351-354`.

**Client emission**
- Every `Server*` forwarder builds a `SurfaceControlFrame` (`ServerLoop.cpp:1289-1404`) and calls `RunSurfaceControlFrame`.
- Under spawn or tcp, `m_remoteSink` routes the frame to `ClientSession::RunRemoteSurfaceControlFrame` (`ServerLoop.cpp:793-795`, `ClientSession.cpp:1205-1249`). An encode error there comes back as `MOBILEGL_ERR_UNSUPPORTED` (`:1217-1226`).
- `BackendObject_Remote::CreateEGLWindowSurface` always sends `ServerSetWindowHandle(handle)` first and then `ServerCreateEGLWindowSurface` (`BackendObject_Remote.cpp:215-216`). There is no split check.
- SwapBuffers is never forwarded (`:279-288`). Present travels as a class-B record.
- The frame is values only, and a compile-time binding enforces it (`SurfaceControlFrame.h:75-100`, `:113-114`). The four inproc-only kinds are 12..14 plus `InitCapabilities=11` on the wire (`:46-73`, `SurfaceControlFrame.cpp:34-54`).

**Server intake**
- Path: `ServerMain.cpp:594-612` → `ServerApplyWireSurfaceOp` (`SurfaceOpCodec.cpp:207-246`) → `DecodeWireSurfaceOp` (`:150-188`) → `ServerLoop::RunSurfaceControlFrame` (`:785-807`), which posts to the apply thread or runs inline if already on it (`:803`) → `ApplySurfaceControlFrame` (`:1097-1283`).
- `OpNeedsAWindowBackend` is true only for CreateWindowSurface and SetWindowHandle (`SurfaceOpCodec.cpp:60-62`).
- For a pbuffer, the encoder fills in `WindowKind::Pbuffer` itself (`:133-135`).

**Dispatch in `ApplySurfaceControlFrame`**
- A null backend answers `NOT_INITIALIZED` for every op (`:1110-1111`).
- **CreateWindowSurface**: `UnpackWindowHandle` (`:1050-1058`) turns the token back into a `void*`, then calls `backend->CreateEGLWindowSurface` and `ForgetCurrentTuple` (`:1126-1138`).
- **ResizeWindowSurface** (`:1139-1143`) and **CreatePbufferSurface** (`:1144-1151`) call the matching backend method.
- **MakeCurrent**: `ApplyMakeCurrent`, then a CapsSnapshot republish on a native bind (`:1152-1188`).
- **ReleaseCurrent**: recorded, not forwarded (`:1189-1200`).
- **SetSwapInterval**: `:1201-1204`.
- **ReleaseSurface**: `:1205-1216`.
- **ReleaseResources**: `ReleaseEGLResources` plus `SetContextLive(false)` (`:1217-1231`).
- **SetWindowHandle**: `backend->SetWindowHandle` (`:1232-1241`).
- **InitCapabilities**: `:1242-1254`.
- **Present**: `ServerVerbSink::OnPresent` → `table->Present()` (`PipeApplier.cpp:313-331`), then the present credit is returned (`:357-359`).

**Refusals, by name**
- `Fatal{UnmigratedSurface,"AndroidNativeWindow@P12"}` for AndroidNativeWindow on a window op (`SurfaceOpCodec.cpp:166-171` and `:217-223`).
- `Fatal{UnmigratedSurface,"MetalLayer@P12"}` (`:172-175`, `:224-228`).
- `WindowKindNamesNoBackend` for Surfaceless/Pbuffer on a window op, and `UnknownWindowKind`; both become `Fatal{ProtocolCorruption,"SurfaceOp"}` (`:176-182`, `:229-235`).
- `SurfaceOp.windowBackend` for an out-of-range tag (`ServerLoop.cpp:1039-1048`).
- `SurfaceOp.kind` for an unknown kind (`:1279-1282`).
- `ApplyThreadNotRunning` (`:815-831`).
- In an armed session child all of these latch the session instead of aborting (`SurfaceOpCodec.cpp:211`, `:215-216`).
- **The X11 refusal is not in the code.** X11, Win32Hwnd and None are accepted, and their token is copied through (`:183-184`). CONTRACT-P6.md:438-446 specifies `X11@P12` and `Win32Hwnd@P12`; they never landed.
- The client-side mirror of the Android refusal (CONTRACT-P6.md:451-456) is also missing: there are no `@P12` hits under `MG_Remote/Client`.

## 2. Backend window surfaces [VERIFIED]

**Base class, `BackendObject`**
- `WindowBackend {Android, X11, MetalLayer, Win32, Count, Unknown=-1}` (`BackendObject.h:569-577`); `WindowHandle` (`:579-584`); `SurfaceKind {None, Window, Pbuffer}` (`:616-620`).
- `CreateEGLWindowSurface` = `RegisterEGLWindowSurface` + `ActivateEGLSurface` (`BackendObject.cpp:191-194`).
- Register refuses Unknown/null (`:227-230`) and stores `surfaceState.Window = handle` (`:232-238`).
- Activate calls `SetWindowHandle(surfaceState->Window)` (`:283`), then `InitWindowSurface()` (`:284`). For a pbuffer it calls `InitPbufferSurface(w,h)` (`:288-292`).
- MakeCurrent activates lazily (`:318-323`, `:332`).
- **Stores to `m_windowHandle`:** `:475` (SetWindowHandle), `:365` (ResetEGLRuntimeState), and `:206-207` (Resize, width/height only; nothing reads those).
- `SwapEGLBuffers` → `Present()` (`:396`).

**DirectGLES (Espryt)**
- `InitWindowSurface` casts `m_windowHandle.Handle` → `DirectGLES::InitWindowSurface` (`BackendObject_DirectGLES.cpp:899-907`).
- `CreateEGLWindowSurface` checks the window backend (`:955-960`). The **sameHandle** dedup key (`:962-967`) requires the same surface, kind Window, and the same Backend and Handle. Otherwise it runs `DestroyEGLContext` + `ResetEGLRuntimeState` (`:969-972`).
- Process globals `g_Display/g_Context/g_Surface/g_Config` (`DirectGLES.cpp:15814-15817`).
- `InitDisplayAndContext` destroys the old context first (`:16159`), then `eglGetDisplay(EGL_DEFAULT_DISPLAY)` (`:16177`). The config has a single surface bit (`:16085`).
- `InitWindowSurface` → `eglCreateWindowSurface` (`:16230`) → MakeCurrent → publish (`:16225-16241`). The pbuffer twin is `:16243-16258`.
- `Present` → `eglSwapBuffers(g_Display, g_Surface)`, **return value ignored** (`:16842`).
- `DestroyEGLContext` destroys the surface and context and calls `eglTerminate` (`:16907-16919`).
- Resize is not overridden (`BackendObject_DirectGLES.h:39-44`), so it only updates bookkeeping.
- **Width/Height gap:** `PublishDefaultFramebufferDepthStencilFormat` posts `MGPSurfaceInfo` with the format only, W/H = 0 (`DirectGLES.cpp:15850-15852`, `:15943-15950`). `QueryCurrentSurfaceSize` exists (`:15819-15837`) but is not used for publishing.

**DirectVulkan (Magma)**
- `CreateEGLWindowSurface` only registers (`BackendObject_DirectVulkan.cpp:418-431`). The real creation happens at the client's MakeCurrent: `Activate` → `InitWindowSurface` → `VulkanRenderer(nativeWindow)` (`:337-352`).
- `InitPbufferSurface` uses a config with `SurfaceWidth/Height` and a null window (`:354-365`).
- Resize → `RequestSwapchainResize` (`:433-446`, `VulkanRenderer.cpp:16533-16542`).
- Window: `vkCreateAndroidSurfaceKHR` (`VulkanRenderer.cpp:16196-16202`).
- Offscreen on Android: headless surface (`:16112-16121`), otherwise **an AImageReader ANativeWindow** (`:16129-16158`).
- Swapchain extent = `currentExtent`, or the clamped desired extent when currentExtent is UINT32_MAX (`SwapchainObject.cpp:194-202`). Quarter-turn swap (`:204-207`). `preTransform = currentTransform` (`:234`).
- `OnSurfaceChanged` carries **Width/Height** plus format (`:323-339`).
- On present: OUT_OF_DATE, or an extent/transform change (`VulkanRenderer.cpp:16501-16531`), → `RecreateSwapchain` (`:14381-14418`) → re-publish. Zero area sets `m_presentSuspended` (`:14386`). `VK_VERIFY` aborts on any other present or acquire result (`:14394`, `:14456`).

**How the size and format reach the client**
- `ServerOnSurfaceChanged` → `kEventSurfaceChanged` (`ServerSession.cpp:483-499`, installed at `:827-828`).
- `ApplySurfaceChangedToClient` (`ClientSession.cpp:306-347`): if W/H ≠ 0 it reallocates all three attachments (`:316-338`); otherwise it changes the format only (`:339-346`).
- The client drains after create, resize and make-current (`BackendObject_Remote.cpp:224`, `:233`, `:272`).

## 3. What the device split path renders into today [VERIFIED]

- The trace app forces a pbuffer for spawn (`trace_replay_jni.cpp:173-188`). apitrace creates a pbuffer sized `ResolveWidth/Height` (`apitrace_glws_android.cpp:198-219`).
- `EGLImpl.cpp:458-469` passes `EGL_WIDTH/EGL_HEIGHT` (default 1) → frame width/height → server `InitPbufferSurface(w,h)`.
  - Espryt: `eglCreatePbufferSurface`.
  - Magma: a headless surface or an AImageReader swapchain at w×h.
- Present on Espryt is `eglSwapBuffers` on a pbuffer (a no-op). On Magma it is `vkQueuePresent` to a surface nobody sees.
- The service exec's `libMobileGLServer.so --serve` (`MobileGLServerService.java:60-61`). The supervisor forks one child per session (`ServerMain.cpp:985`, `:1123`). The child runs `RunSession` (`:268`, `[[noreturn]]`), sets `Transport=Spawn` (`:356-358`) and calls `InitServerRoleForSpawn` (`:377`).
- This whole pbuffer chain is what must stay.

## 4. What `WindowKind::ServerOwned` needs on the server

- **Injection point [VERIFIED as the natural seam].** Substitute the server's window token in `ServerApplyWireSurfaceOp`, after decode and before `RunSurfaceControlFrame` (`SurfaceOpCodec.cpp:243`). Downstream nothing changes: `UnpackWindowHandle` → Register (`:232`) → Activate → SetWindowHandle (`:283`), which is exactly the inproc path today.
- **sameHandle** (`BackendObject_DirectGLES.cpp:962-964`) then compares the server's pointer with the server's pointer and dedups correctly. The concern a6 §6(d) raised disappears.
- **Present:** unchanged. The Present record → `DirectGLES::Present` `eglSwapBuffers` on the window surface, or Magma `vkQueuePresentKHR` on the real swapchain.
- **Size and rotation**
  - Magma already reports window resize and rotation (`SwapchainObject.cpp:323-339`); rotation is compensated inside the server via preTransform.
  - Espryt reports nothing (§2 gap).
  - Resize requests from the client do not move the extent on either backend. Espryt only updates bookkeeping; Magma uses currentExtent. A client-requested size therefore has to be applied to the window itself: `ANativeWindow_setBuffersGeometry` or Java `SurfaceHolder.setFixedSize`, which is the lever the code comment names (`VulkanRenderer.cpp:14402-14405`).
- **Window absent:** an absent window must be checked both when CreateWindowSurface is decoded and when the surface is activated, because on Magma activation is deferred to MakeCurrent (`BackendObject.cpp:319`, `:332`).
- **Existing precedent:** the codebase refuses by name and never falls back silently (`ServerLoop.cpp:180-186`, `:824-831`).

## OPEN QUESTIONS / RISKS

1. **Process topology is the main blocker [INFER].** An ANativeWindow from a SurfaceView is a Binder proxy living in the APK's JVM process. The server is an exec'd binary that forks one child per session, and neither exec nor fork carries a usable window (Binder does not survive fork). The options:
   - (a) Run the session in-process via JNI, in a dedicated `android:process` that is restarted per session. `RunSession` exits the process when it ends, and `ServerLoopInstance` is a leaked singleton (`ServerLoop.cpp:980-984`).
   - (b) Pass a Parcel to the exec'd child. This needs API 34 and a binder route to a native process; impractical.
   - (c) Keep the server offscreen and send `AHardwareBuffer`s over the existing AF_UNIX socket (API 26) to an APK-side presenter.
2. **Selection knob.** Should the mode be client-driven (the client sends ServerOwned) or a server environment policy? Should a client pbuffer be allowed to go on-screen?
3. **Espryt Width/Height publishing.** Publishing W/H through the transport arm switches the client to the reallocating arm (`ClientSession.cpp:316-338`), which is a divergence from monolith. It should be gated to ServerOwned only.
4. **Initial viewport and client default-FB extent.** Does the frontend's initial viewport or the default-FB extent depend on the 512×512 placeholder? That is another slice.
5. **Losing the window mid-session.**
   - Espryt: destroying `g_Surface` destroys the context as well (`DirectGLES.cpp:16907-16919`). Parking on a pbuffer would need `g_*` de-globalisation and a config with both WINDOW and PBUFFER bits (`:16085`).
   - Magma: it aborts on `SURFACE_LOST` (`:14394`), and `vkCreateAndroidSurfaceKHR` failure is `VK_VERIFY` rather than a `false` return.
   - Espryt ignores the `eglSwapBuffers` error (`:16842`).
6. **`surfaceDestroyed` threading.** Teardown must be serialized onto the apply thread before the Java callback returns.
7. **Unrelated gap.** The X11/Win32/None hole in CONTRACT-P6 §7.2 is still open.

## MINIMAL CHANGE LIST

- **`protocol.fbs:249`**: append `ServerOwned = 7` and regenerate. **`SurfaceOpCodec.cpp`**:
  - Add a static_assert for the new value; widen the `IsOutRange` bound (`:178-179`).
  - In the decoder, add a ServerOwned arm before the Android refusal. It requires `nativeToken==0` (otherwise `Fatal{ProtocolCorruption,"SurfaceOp.nativeToken"}`) and sets `windowBackend=Android`.
  - In `ServerApplyWireSurfaceOp`, fill the token from the provider. Add a new `SurfaceWireError::ServerWindowAbsent` that answers `ok=false`, is logged by name, and does not latch; the client then gets `EGL_BAD_NATIVE_WINDOW` (`EGLImpl.cpp:153-156`).
  - Encoder (client side): under spawn/tcp, map `WindowBackend::Android` → ServerOwned with token 0, and send the client's desired w/h.
- **New `Server/ServerWindowProvider.{h,cpp}`**:
  - A process-wide holder, `SetServerOwnedWindow(ANativeWindow*)` with acquire/release, a generation counter, and `ApplyGeometry(w,h)`.
  - It is set by the APK's JNI before a session starts; no window means the ServerOwned path is refused.
  - This is the "both paths" switch: pbuffer ops are untouched.
- **`SurfaceControlFrame.h/.cpp`, `ServerLoop.cpp`**:
  - Append an inproc-only `ServerWindowLost` kind (15), posted by the provider on `surfaceDestroyed`.
  - Its dispatch releases the surface and latches through a new `FatalFamilies.def` row (e.g. `ServerWindowLost`).
  - Recheck the provider at activation (`BackendObject.cpp:283-284`) or in `UnpackWindowHandle` (`ServerLoop.cpp:1050-1058`).
- **`DirectGLES.cpp`**:
  - `:15947-15950`: fill W/H from `QueryCurrentSurfaceSize` when the surface is a server-owned window.
  - `:16842`: check the `eglSwapBuffers` result, re-query the size after the swap, and call `OnSurfaceChanged` when it changed.
  - `g_*` de-globalisation is not needed while there is one backend per session process.
- **`VulkanRenderer.cpp:14394`, `:14456`, `:16202`**: turn `SURFACE_LOST` and a failed surface creation into named latches instead of aborts. The on-screen path itself needs nothing new, because `InitWindowSurface` already takes a real window.
- **`BackendObject_DirectGLES.cpp:962-967`**: no change once the token is the server's.
- **`ServerMain.cpp`, `MobileGLServerService.java`, JNI**: decide the topology in risk 1. Option (a) needs an in-process entry point without fork; the service is gated by environment or intent extras.
- **Tests**:
  - `SurfaceControlFrameTest.cpp:125-163`, `:236-249`, `:323`, `:376` (enum bounds).
  - `PeerLatchTest.cpp:1250-1257`: add ServerOwned rows (absent, nonzero token).
  - `ServerLoopTest`: a lost-window control.
- **Client (another slice)**: `BackendObject_Remote.cpp:215` currently sends the client pointer unconditionally. With the encoder mapping above it becomes harmless.