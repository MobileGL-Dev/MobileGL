# CI cleanup log

Every removal or replacement of a CI test, gate or lane is logged here with a one-line reason;
decisions to keep a red case (and fix the code or the tooling instead) are logged too, so the
next reader can see why a check still exists. Started in P13 (2026-10-06). The rule: a check is
removed only when it is superseded or no longer relevant (tied to the pull build, or a finished
migration-phase audit), never to hide a real bug. Goldens change only when the GL spec proves
them wrong.

## Removals

| Date | Check | Workflow / job | Reason | Successor |
|---|---|---|---|---|
| 2026-10-06 | `build explicit pull runtime control` (the pull `libMobileGL.so`, `-DMOBILEGL_PIPE_PUSH=OFF`) | Test / `build-linux-monolith-control` | P13 W2 retires the pull build: CMake defaults to push and nothing ships pull (ID-P13-1) | the same job builds the push monolith without MG_Remote (the FCL shape); its `runtime_mode_proof --mode monolith` is the zero-MG_Remote-symbol check |
| 2026-10-06 | `Negative control (pull library)` | Test / `retrace-split` (every leg) | its library was the pull build above | `Negative control (library without a transport)`: same script (renamed `retrace_transport_control.sh`) and evidence sentence, with the push library without MG_Remote; W1 ran both side by side on every leg |

## Kept after investigation

### `minecraft-1.17-main-menu-854` (Test retrace / retrace-split / retrace-verify, APK retrace), 2026-10-06

- **Symptom**: every lane, both backends, SSIM 0.0186 against the golden; the frame is Minecraft's
  red loading-screen colour. Local repro on WSL llvmpipe: same SSIM, monolith and inproc.
- **Root cause**: the trace was captured on an implementation that gave shaders and programs
  **overlapping names** (shader 1 and program 1 live at once, likewise shader 5 / program 5,
  calls 159-209). GL 4.6 core and ES 3.2 §7.1 put shader and program objects in one shared name
  space, so a conformant implementation never hands out such a pair. Since
  `5a0f13fdf` / `168808d1a` (2026-10-01/02) MobileGL advertises `GL_ARB_shader_objects`, and
  apitrace's glretrace then resolves every shader and program name through one ARB handle map
  (`_handleARB_map`): program 1 overwrote shader 1's entry, `glAttachShader(1, 1)` attached a
  program name (GL_INVALID_OPERATION at call 170), the blit program linked without a vertex
  shader, and nothing after it drew.
- **Classification**: (b) retrace tooling - not a MobileGL bug and not a bad golden. The core
  entry points are typed (`glAttachShader(program, shader)`), so mapping each argument through
  its own type's map is lossless even when the capture's names collide; only the untyped
  `*ObjectARB` handle calls need the shared map.
- **Fix**: MobileGL-Dev/apitrace `45843686` (branch `mobilegl/android-trace-replay`): typed
  `GLprogram` / `GLshader` arguments resolve through `_program_map` / `_shader_map` first and fall
  back to the ARB handle map only for names created by `glCreate*ObjectARB`; typed creators record
  both maps. Submodule bumped in the same MobileGL commit. Local: 1.17 passes on DirectGLES
  (monolith, inproc) and DirectVulkan (lavapipe).
- **Decision**: keep the case. With typed mapping it checks GL-level correctness of MobileGL like
  any other fixture; the namespace overlap is a property of the capture that a spec-portable
  replay can absorb.
- **Side note (not changed)**: MobileGL advertises `GL_ARB_shader_objects` while its
  `glCreateShaderObjectARB` / `glCreateProgramObjectARB` entry points are stubs
  (`MG_Impl/GLImpl/Exporting/Definitions.cpp`). The string was added on purpose for KWin; whether
  to implement the ARB handle entry points is a separate decision.

## Infrastructure fixes (no check removed)

| Date | What was red | Cause | Fix |
|---|---|---|---|
| 2026-10-06 | every `ctest` invocation in integration / verify / split jobs exited 8 after the tests passed (`integration-verify`, `integration-split*`, `integration (inproc)`, `spawn_lane_parity.py`) | `PipeCatalogueSourceRoot.cmake` (a CTest include file) used `if(... IN_LIST ...)`; CTest scripts run without policy CMP0057 on the pinned CMake 3.31.10, so the include errored | use `list(FIND)` instead (`MobileGL/MG_Test/Pipe/CMakeLists.txt`) |
| 2026-10-06 | `TcpServer.Start.P8aUnlocatedIoBlocks` failed in 0.07 s on `integration (monolith)` | the loopback TCP endpoints (40613, knob lanes 41613-41617) sat inside Linux's ephemeral port range; with thousands of loopback client connections in the lane, an outgoing socket can own the port the supervisor binds | default endpoint moved to 30613 (knob lanes 31613-31617), below 32768 |
