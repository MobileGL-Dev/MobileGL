# glmark2 performance work: handoff

Status on 2026-10-04 after the first optimization pass on top of the
[baseline](glmark2-baseline.md). Everything here was measured on the anland Plasma desktop of the
Lenovo TB321FU (Adreno 750) at **pinned clocks** (CPU 1.80 / 2.25 / 2.25 / 2.25 GHz, GPU 680 MHz;
see the baseline). Single runs or two at most per point, so the numbers are **approximate**
(light scenes vary about +-15% run to run). The tablet was in portrait for the later runs, so the
"after" fullscreen numbers are 1600x2560 and the "before" ones 2560x1600 (same pixel count).

## Headline

glmark2 score, `-b :duration=5`, default scene list:

| | stock kgsl stack | Espryt before | Espryt now | Magma before | Magma now |
|---|---|---|---|---|---|
| es2, 800x600 window | ~1076-1180 | ~1319 | **~1887** | ~778 | **~1213** |
| es2, fullscreen | ~1177 | ~572 | ~561 | ~354 | ~404 |
| desktop GL, 800x600 window | ~1161 | ~1283 | **~1809** | ~743 | **~1186** |

The "now" es2 window scores include the two `buffer update-method=map` scenes, which MobileGL now
runs (they were Unsupported before), so they are a fair comparison with stock's.

Per scene (es2 window unless marked F = fullscreen; FPS):

| scene | stock | Espryt before | Espryt now | Magma before | Magma now |
|---|---|---|---|---|---|
| texture | 1269 | 1672 | ~2210 | 915 | ~1580 |
| build use-vbo=false | 1095 | 467 | ~1215 | 472 | ~1040-1190 |
| build use-vbo=true | 1287 | 1675 | ~2450 | 1062 | ~1730 |
| desktop blur | 976 | 275 | **~1000** | 157 | ~680 |
| desktop shadow | 1044 | 234 | **~1000** | 125 | ~830 |
| ideas | 688 | 138 | ~550 | 66 | ~354 |
| terrain | 443 | 149 | ~220 | 115 | ~157 |
| shadow | 975 | 1139 | ~1840 | 644 | ~1170 |
| refract | 838 | 981 | ~1280 | 331 | ~520 |
| jellyfish | 1185 | 1438 | ~2170 | 841 | ~1155 |
| buffer map (2 scenes) | 330 / 408 | Unsupported | ~465 | Unsupported | ~431 |
| texture F | ~1430 | ~726 | ~597 | ~445 | ~398 |
| terrain F | - | - | ~142 | - | ~112 |

**Remaining gap:** Espryt is at or above stock in every windowed scene except **ideas (0.8x)** and
**terrain (0.5x)**. Magma is behind on most draw-heavy scenes (ideas 0.5x, terrain 0.35x, desktop
0.7-0.8x, refract 0.6x). **Fullscreen** is the biggest gap on both backends (0.4-0.5x of stock),
and nothing landed here addresses it.

## Done (all on origin/feat/disaggregated)

| commit | change | why / evidence | effect (approx) |
|---|---|---|---|
| `4722fa38` | the baseline report | - | - |
| `66b47a10` | **glFinish waits for the GPU** on the remote path (a server fence created, waited with `GL_SYNC_FLUSH_COMMANDS_BIT`, destroyed); **Magma submits** once 1024 retired per-clear objects pile up with nothing submitted; **ES contexts advertise** `GL_OES_rgb8_rgba8`, `GL_OES_depth24`, `GL_OES_mapbuffer` (with `glMapBufferOES` / `glUnmapBufferOES` / `glGetBufferPointervOES` exported as the core calls) | glFinish only waited for the server's apply, so off-screen rendering ran unbounded (glmark2 `--off-screen` "6846" / "12117" fps) and drove Magma's server to `VK_ERROR_OUT_OF_HOST_MEMORY` in `ClearWireFramebuffer`; es2 `--off-screen` could not start (glmark2 picked `GL_RGBA4` and no depth) | off-screen now real numbers (Espryt texture ~866, Magma ~800) and no crash; es2 `--off-screen` runs; the two buffer-map scenes run |
| `2de0d6dd` | **client-array buffers are reused** from a per-share-group pool (respecify, a non-blocking record, instead of a blocking ResourceCreate + destroy per array per draw); contiguous element ranges copied in one memcpy; the fetch plan skips sorting an already ascending range | desktop blur/shadow paid 28/52 blocking creates per frame, build use-vbo=false 2; `std::sort` was 27% of the client in build use-vbo=false | build use-vbo=false Espryt 467 -> ~1100, Magma 472 -> ~1040; blocking replies per frame in desktop 29/53 -> 1 (fps of desktop barely moved at this step: the next item was the limit) |
| `d627fca3` | **targeted wake-ups:** the client publishes the applied sequence it waits for (`RingControl::producerWakeSeq`, its own cache line); the server rings a parked client only once that sequence is applied (or the event ring filled); the parked flag is stored with release so the notifier sees the target | while the client waited for its present reply the server rang it after every record it applied: 18% of Espryt's apply thread was `SocketDoorbell::Notify`, and every ring was a futile wake-up and re-park on the client | Espryt desktop ~250 -> ~800, ideas 138 -> ~380, texture 1672 -> ~2190; Magma desktop ~150 -> ~550-650, ideas 45 -> ~137, refract 372 -> ~540 |
| `963f4d81` | Magma **reuses completed command buffers** (pool resets them at the next begin) instead of `vkFreeCommandBuffers` + `vkAllocateCommandBuffers` per submission | `FreeRetiredCommandBuffersCompletedUpTo` was 15% of Magma's apply thread in ideas | small: Magma texture / desktop +~10% |
| `915882b2` | the server **times records only when** `MOBILEGL_SERVER_FRAME_STATS` is on | two `steady_clock::now()` per record were ~5% of Espryt's apply thread | with the next row |
| `e5ee31b0` | the anland server build uses **`android-30`** (the APK's minSdk) in `build-android.sh` / `reproduce.md`: `thread_local` is native ELF TLS instead of emulated TLS | `__emutls_get_address` + `pthread_getspecific` were 7-12% of Espryt's apply thread. **An existing build dir keeps its API level: reconfigure (delete it) to get android-30** | Espryt (with the row above) desktop ~800 -> ~1000, ideas ~380 -> ~550, terrain 168 -> ~220; Magma unchanged |
| `bb31f374` | Magma **keeps the wire draw's render pass open** across draws to the same attachments. Every command that may not sit inside a pass (barriers, copies, clears, blits, dispatches, other passes' begins) in the resource managers and wire helpers first calls `EndActiveRenderPassOn(cb)` (`RenderPassGuard.h`); the per-draw "make image writes visible" barrier for sampled images is only recorded when an image may have been written since the last one (`WireImageWriteEpoch`, bumped by every pass end, out-of-pass work and storage-image draw); same-layout attachment barriers are skipped when the pass continues | `SetupWireDraw` ended and began a VkRenderPass for every draw (ideas: 305 passes per frame; End/Begin ~21% of the apply thread, plus `vkEndCommandBuffer` cost), and on a tiler each pass loads and stores its attachments | Magma ideas ~146 -> ~354, desktop shadow ~640 -> ~830; visually checked on ideas, terrain, shadow, refract, jellyfish, desktop, bump, conditionals, effect2d, Plasma, Chrome and the lock screen |

Each landed with the host unit tests green (WSL clang, `ctest -L unit -E DirectVulkan`, 2851
tests) and a device check on both backends (frames shown, Plasma, Chrome page + scroll input,
`kscreenlocker_greet --testing`).

## Findings

- **The common shape before the fixes: nothing was saturated.** GPU 7-40% busy, client and server
  threads each ~30-50% busy, both waiting for each other with a wake-up per handoff. Removing
  futile wake-ups (`d627fca3`) was worth more than any per-draw CPU saving.
- **Espryt after the fixes:** the remaining windowed gaps are ideas and terrain. Its present reply
  is still a synchronous round trip per frame, and the server's per-draw state sync
  (`DG::PrepareForDraw`, 28% of the apply thread in ideas before) is now the larger share.
- **Magma after the fixes:** CPU-bound on the server. Remaining per-draw costs in
  `SetupWireDraw` (descriptor sets, `GetOrCreateProgram` / pipeline hash, `BuildWireVertexInput`,
  vectors allocated per draw, scudo lock contention ~20% before), and per frame
  `BlitDefaultFramebufferToSharedImage` (~30% of the apply thread: frame flush + a second submit
  for the copy + `vkEndCommandBuffer`) plus `Present()` of the client's offscreen default surface
  (a swapchain acquire + submit that nobody looks at) and GPU progress markers per submit.
- **Fullscreen:** every frame is rendered into the server's default framebuffer and then copied
  into the window's shared image (Espryt `glBlitFramebuffer`, Magma `vkCmdCopyImage`); at 4 MP the
  GPU is ~98% busy on texture at ~600 fps while stock does ~1430 and is even faster fullscreen than
  windowed (it presumably skips composition).

**Dead end: an adaptive client spin.** Growing the doorbell spin up to 1 ms towards the measured
reply latency (and shrinking it on long waits) gave +30-80% on light scenes (texture, jellyfish,
shadow) on Magma but cost refract -35% (370 -> 236) and fullscreen terrain -35% (90 -> 58),
reproducibly; the present reply simply got slower while the client spun. Polling the shared line
less often (backoff up to 64 iterations) did not change that, so it is not cache-line ping-pong. A
static `MOBILEGL_IPC_SPIN_US=1000` on the client showed the same mixed picture on Espryt. Not
landed; the targeted wake-up removed most of what the spin bought without the regressions.

**Traps:**
- **Pin the clocks.** Unpinned, the stock stack's light scenes ran at GPU 310 MHz / big cores 672
  MHz and scored 425 instead of ~1100.
- **glFinish and `--off-screen`.** Before `66b47a10` off-screen MobileGL numbers were meaningless.
- **The stock desktop turns black mid-run** (a pure-black 21707-byte screenshot) while
  SurfaceFlinger keeps latching ~125 buffers/s, and its score then jumps to ~1420-1500. Probe the
  screen every ~15 s during stock runs and discard runs with a black probe; restart the stock
  container + app (`stfix.sh`) to get the picture back. A powerdevil inhibit did not prevent it.
- **The MobileGL app's extra-keys bar** shrinks its desktop to ~2560x1410; set
  `extra_keys_mode=with_keyboard` for fullscreen comparisons (it was restored afterwards).
- **The MobileGL session restores a System Settings window** at login; close it before measuring.
- **Rotation:** the stock app follows auto-rotate; check glmark2's "Surface Size" line.
- **Symbolizing:** the APK's server lib is stripped; a RelWithDebInfo build of the same tree was at
  +0x80 for every function (check with `llvm-nm -D` on both). Container clients: simpleperf prints
  offsets from the start of the executable mapping (add the r-x segment's page-aligned vaddr,
  0x4fb000 for the lib of 2026-10-04). Off-CPU (`sched_switch`) samples of container processes
  get their user frames attributed to Android's libs, so use the client's own `P65LinkMetrics` for
  its waits.
- `pkill -f <script>` inside `su -c "..."` kills the command itself; use pids.

## Yet to do (ranked)

1. **Fullscreen: render straight into the window's shared images** (both backends). The default
   framebuffer becomes the current back shared image (an AHB-backed EGLImage FBO on Espryt, the
   imported image on Magma); the bottom-up row order goes to linux-dmabuf's `Y_INVERT` flag instead
   of the flipping blit. Gain: fullscreen ~1.5-2x. Risk: default-framebuffer semantics (reads,
   blits, buffer age, resize, device loss), KWin's handling of `Y_INVERT`. Verify: fullscreen
   glmark2, Chrome, desktop-verify, the retrace CI.
2. **Asynchronous shared-image present.** Today `PresentToSharedImage` is `EmitAndWait`, so the
   client cannot record frame N+1 while the server finishes N. Commit the `wl_buffer` from the reply
   on the event ring (or order the compositor's acquire after the producer's present server-side),
   and allow present credit 2. Gain: light scenes 1.3-1.8x. Risk: frame ordering with the
   compositor, buffer reuse with 3 images, an app that draws one frame and idles. Verify: Chrome
   flicker barcode test, KWin, desktop-verify.
3. **Magma per-frame submissions:** append the shared-image copy to the frame's command buffer (one
   submit per frame), skip `Present()` for a surface whose frames only go to shared images, coalesce
   GPU progress markers. Gain: Magma light scenes 1.2-1.4x. Risk: the copy's fence/semaphore
   export (`SI::PublishWrite`), hang-watch coverage. Verify: Magma unit tests, shared-image tests.
4. **Magma per-draw CPU:** cache descriptor sets / pipeline lookups across draws when unchanged
   (keyed on monotonic lifetime ids), stop rebuilding the vertex input state per draw, replace the
   per-draw vectors with per-frame arenas. Gain: ideas/terrain 1.3-2x. Verify: retrace CI with
   Magma.
5. **Terrain** (both backends at 0.35-0.5x of stock): not profiled after the fixes; it has 43
   respecifies, several FBO passes and a GenerateMipmap per frame. Profile first.
6. **Espryt per-draw state sync** (`DG::PrepareForDraw`): the Minecraft roadmap's version-gated
   early-outs. Gain 10-20% on draw-heavy scenes. Risk: stale caches; retrace CI.
7. **Blocking fence create/destroy:** `EmitFenceSync` / `EmitDeleteSync` are `EmitAndWait`; an app
   that makes a fence per frame (Minecraft, Chrome) pays two round trips. Make them fire-and-forget
   (the client already mints the handle).
8. **Client-to-server doorbell:** the server parks between records and the client pays a socket
   write per record while it streams. Try a server-side spin sized to the inter-record gap of an
   actively streaming client (watch the spin regression above), or ring only at batch boundaries.
9. `CreateVertexElements` is re-sent per draw (ideas: 76 per frame); deduplicate by content on the
   client.

## How to reproduce

Helper scripts: `tools/device_bench/anland_glmark2/` (copied verbatim from the session; the phone
side expects them in `/data/local/tmp/anl/`, push with `adb push <file> /data/local/tmp/anl/`).

| script | where | does |
|---|---|---|
| `pbclk.sh save\|pin\|restore\|show` | phone, root | saves the governors/limits to `/data/local/tmp/anl/pb-clocks.orig`, pins CPU 1.80/2.25/2.25/2.25 GHz + GPU 680 MHz, restores |
| `pbbatch.sh <container> <stock\|mgl> <label> <cfg...>` | phone, root | per config (`es2w es2f glw glf es2o glo`): cool-down (`pbcool.sh`), samples (`pbsamp.sh`), screenshots + SurfaceFlinger frames (`pbshown.sh`), 15 s screen probes (`pbprobe.sh`), runs glmark2 through `pbrun.sh` in the container (`/root/pbrun.sh`), black-screen restart for stock (`stfix.sh`) |
| `ab.sh <container> <tag> <cmd...>` / `pbset.sh <tag> [env]` | phone, root | one run / the affected-scene set |
| `lm.sh <tag>` | phone, root | per-frame `P65LinkMetrics` averages of a run made with `MOBILEGL_PIPE_STATS=1` |
| `pbprof.sh`, `profrun.sh`, `gprof.sh` | phone, root | `simpleperf` (flat, per-thread `top -H`, GPU busy) and `--trace-offcpu -g` call graphs of the server apply thread + client |
| `visset.sh <tag>` | phone, root | mid-run screenshots of ten scenes |
| `dep.sh`, `smoke.sh`, `wtest.sh` | host | deploy a server + client build and restart on a backend; Chrome + lock screen smoke check; WSL unit tests |
| `pbparse.py`, `mktables.py` | host | glmark2 outputs -> per-scene tables |
| `symagg.py`, `incl.py`, `offcpu.py`, `callers.py`, `timeline.py` | host | symbolize and aggregate simpleperf output (flat by function, inclusive call graph, off-CPU waits, callers of a function, on/off-CPU timeline) |

```sh
# clocks (phone, root)
sh /data/local/tmp/anl/pbclk.sh save; sh /data/local/tmp/anl/pbclk.sh pin
sh /data/local/tmp/anl/pbclk.sh restore      # when done (also on abort)
# backends / stacks
sh /data/local/tmp/anl/switch.sh DirectGLES|DirectVulkan      # MobileGL anland (skill script)
am force-stop com.anland.consumer.mobilegl; droidspaces -C /data/local/Droidspaces/Containers/arch-kde/container.config start
monkey -p com.anland.consumer -c android.intent.category.LAUNCHER 1                     # stock
# one glmark2 run in the MobileGL container (root shell in the container)
MGLOG=/tmp/pb-x.log mgrun offscreen bash -c 'MOBILEGL_PIPE_STATS=1 glmark2-es2-wayland -b ideas:duration=10'
# profile the busiest server apply thread + the client
simpleperf record --trace-offcpu -e cpu-clock -f 2000 -g -t <apply tid>,<client pid> --duration 6 -o x.data
simpleperf report-sample -i x.data --show-callchain > x.samples
python incl.py x.samples <tid> <unstripped libMobileGL.so> <offset> com.anland.consumer.mobilegl
```
