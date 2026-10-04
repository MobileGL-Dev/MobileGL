# Handoff: anland 5.x + MobileGL unified server

Updated 2026-10-04. This replaces the 2026-10-02 (post-P14) handoff; git history keeps the old text.

## What this is

MobileGL runs disaggregated on a rooted Android tablet:
- **Client:** the glibc library in the Droidspaces container `arch-kde-mgl`, where it is the system-wide glvnd EGL/GLX vendor and the GBM backend.
- **Transport:** the client streams pipe records over abstract socket `@anland-mobilegl`.
- **Server:** runs in-process in the anland APK's `:mobilegl` foreground service.
- **Backends:** Espryt (DirectGLES) or Magma (DirectVulkan), chosen in the app settings.

KDE Plasma runs on top with zero-copy windows (server-allocated AHB dma-bufs over linux-dmabuf). Everything is vendor-agnostic: AHB, sync_file, EGL_ANDROID_*, VK_ANDROID_*; no kgsl or qcom specifics.

## Remotes and heads

- MobileGL: <https://github.com/MobileGL-Dev/MobileGL> `feat/disaggregated`, head `2d424146`. Integration worktree: `.claude/worktrees/anland-unified`.
- anland: <https://github.com/MobileGL-Dev/anland> `legacy-mobilegl-unified`, head `41ec066`. Integration worktree: `anland/.claude/worktrees/mgl-unified`.
- Version: 26.10 (`MobileGL/Config.h`, `android-plugin/app/build.gradle.kts`).

## Where to start reading

| Topic | Doc |
|---|---|
| Reproduce the whole setup on a new machine or phone | `.claude/skills/anland-mobilegl-plasma/reproduce.md` (and `SKILL.md`) |
| Day-to-day bring-up and checks | `runbook-plasma.md` |
| Zero-copy windows | `plan-ahb-dmabuf.md` |
| X11 on the GPU (glamor, DRI3/Present GLX) | `plan-x11-gpu.md` |
| glmark2 performance: baseline and the optimization handoff | `../../../Performance/glmark2-baseline.md`, `../../../Performance/glmark2-handoff.md` |
| VA-API driver on MediaCodec (plan only, nothing implemented) | `../../../MobileVA/plan.md` |

## State of the desktop (verified on the device, 2026-10-04)

- **App-driven bring-up:** opening the anland app starts daemon → server → container → Plasma. A foreground service keeps it alive, the server idles while the app is hidden (KWin DPMS off), and the notification has a "Stop desktop" action.
- **Both backends** run the desktop: KWin, plasmashell, Kickoff, notifications, the lock screen, resize on extra-keys-bar toggle, and suspend/resume on window loss.
- **Buffer age and damage** work end to end on both backends.
- **glvnd vendor:** MobileGL is the system-wide vendor (`10_mobilegl.json`, `/etc/mobilegl/client.conf`). It steps aside for `LIBGL_ALWAYS_SOFTWARE`, foreign GBM devices, and when no server answers (falls through in about 0.1 s).
- **X11:** Xwayland glamor on MobileGL. GLX presents through DRI3 + Present shared images, with a MIT-SHM readback fallback.
- **Chrome:**
  - runs its GPU in a separate process with real EGL native fences, with no flicker;
  - recovers from device loss (as do KWin and plasmashell);
  - Shadertoy-style GPU faults are contained per session;
  - a hanging client is cut off on Magma (watchdog `MOBILEGL_GPU_HANG_BUDGET_MS`) and on Espryt (GUILTY reset).
- **Chrome video:** hardware decode works through the container's VA-API driver. NV12/P010 dma-bufs are sampled via `GL_OES_EGL_image_external` on both backends. Foreign YUV buffers are CPU-copied into server YUV images, and 4K60 is fine. `MOBILEGL_CHROME_VIDEO_DECODE=software` opts out.
- **Device state at handoff:**
  - MobileGL anland on Magma, Plasma up, server/client build `bb31f374`; later commits are docs, CI and a DirectGLES multi-session fix that is not yet deployed;
  - clocks NOT pinned (restored); the original anland APK is stopped;
  - pin and restore helpers: `su -c "sh /data/local/tmp/anl/pbclk.sh pin|restore"`.

## Work streams and where each one stopped

### 1. glmark2 performance (stopped at a handoff)
Details: `docs/Performance/glmark2-handoff.md`.
- **Now:** MobileGL beats the stock kgsl stack in windowed glmark2 on both backends (es2 800x600: Espryt ~1887, Magma ~1213, stock ~1100). Fullscreen is still about half of stock (Espryt ~561, Magma ~404, stock ~1177).
- **Next, ranked:**
  1. render straight into the window's buffers (removes the fullscreen copy);
  2. non-blocking present;
  3. merge Magma's per-frame submissions;
  4. Magma descriptor and pipeline caching;
  5. profile terrain;
  6. Espryt per-draw state sync;
  7. non-blocking fences.
- Bench scripts: `tools/device_bench/anland_glmark2/`.

### 2. GitHub Actions CI (stopped mid-way, no handoff from its agent)
- **Background:** CI had been red since about 2026-09-26 (Test workflow) and since 2026-10-02 (APK workflow retraces). Most runs show `cancelled` only because each newer push cancels them.
- **Landed:**

| Commit | What |
|---|---|
| `eb71b669` | The pull and verify builds compile again |
| `c1e957cb` | Retired the P3a/P4a untouched-region gates; those phases closed and later work legitimately changed the regions |
| `78c5f8da` | Census records `Refuse{Recover}` as a local refusal |
| `32d27b14` | Real bug: a session's backend registration no longer trips the strict apply-thread pin while another session is applying (DirectGLES) |
| `2d424146` | Failed ctest entries are annotated with their failure lines, so failures can be read from the check-run annotations without a signed-in log reader |

- **Run on `2d424146`:** in progress at handoff. Gates, include-closure and flatc are green; the builds and integration jobs were still running.
- **Still open:**
  - the APK workflow's **retrace failures** (Retrace and validate), on DirectGLES for minecraft-1.21.11-main-menu, 1.21.4 startup/in-world/main-menu, 1.17-main-menu-854 and OpenRA, and on DirectVulkan for 1.17-main-menu-854;
  - they first appeared around `5accb098` / `8da082f8` (2026-10-02); last green APK run `2f64ede8`;
  - not yet bisected. Candidates:
    - the clear-colour default (0,0,0,0), which is GL-correct, so judge any golden against the spec;
    - anonymous-struct sampler handling in glslang;
    - sampler CSO publishing;
    - the scissor-shadow fix;
    - the inactive-fragment-output location fix;
    - the external-texture and YUV changes;
    - the perf commits.
- **User guidance on judging checks:**
  - phase-era process gates may be overly strict, and superseded ones may be removed with a reason;
  - retraces and OpenGL-level checks must pass;
  - a feature a backend does not implement (e.g. iterationRP) may be excluded narrowly per backend.
- **Reading CI without `gh`:** `curl https://api.github.com/repos/MobileGL-Dev/MobileGL/actions/runs?branch=feat/disaggregated`, then `/actions/runs/<id>/jobs` and `/check-runs/<job>/annotations`. Raw logs need sign-in.

### 3. Chrome hardware video (landed) → vendor-agnostic VA-API (planned only)
- **Container driver:** `/usr/lib/dri/msm_drm_drv_video.so` is an unpackaged third-party, Qualcomm-only driver. It drives V4L2 at `/dev/video32` with Qualcomm modes and copies frames on the CPU itself. libva picks it because the render node's DRM driver name is `msm_drm`.
- **Plan in `docs/MobileVA/plan.md`:** a new `MobileVA/` directory; MediaCodec decode in the `:mobilegl` process; H.264 first, then VP9, AV1, HEVC/10-bit; the old driver kept as A/B fallback.
- **Prerequisites the plan found:**
  - per-plane R8/GR88 dma-buf import, which mpv's GL path and KWin's NV12 path need;
  - YUV sampling paths that wait on the shared image's write fence (they appear not to).

### 4. MobileGL work landed since the previous handoff (newest first, grouped)
- **Perf:**
  - `bb31f374` Magma keeps render passes open across draws;
  - `e5ee31b0` android-30 for native TLS;
  - `915882b2` record timing only with stats on;
  - `963f4d81` Magma command buffer reuse;
  - `d627fca3` the server wakes a parked client only when its sequence is applied;
  - `2de0d6dd` client-array buffer reuse;
  - `66b47a10` glFinish waits for the GPU, plus ES OES strings.
- **YUV:** `7218bacb` NV12/P010 shared images, `GL_OES_EGL_image_external`, Espryt `GL_EXT_YUV_target`, Magma Ycbcr conversion.
- **Device loss and hangs:**
  - `287e1df8` cut off the session that hangs the GPU;
  - `7823bcd9` client fresh-session recovery;
  - `90e63c9e` robustness extensions;
  - `58c3d9bd` per-session device-loss latch;
  - `36190b7c`, `cf763b40`, `1dd888cb`, `f22ae625`, `f52a5467` loss and teardown edge cases.
- **Multi-thread and multi-context fixes:** `a9f22ebb`, `25a76c1b`, `393002a4`, `a84104a4`, `2acc6c41`, `8024b645`.
- **Chrome:**
  - `4f6f4125` real EGL native fences;
  - `30977c18` opt-in ES identity for ES contexts (the GPU process on ANGLE);
  - `5accb098` glslang anonymous-struct samplers (Shadertoy page fault).
- **X11:**
  - `198d6fea` GLX via DRI3/Present;
  - `08f23543` glFlush implicit sync for glamor;
  - `5de6ecd4`, `7fd965ce` shared-image upload and publish;
  - `cd58f048` inactive fragment output location;
  - `316ffe02` never dial its own X display.
- **glvnd:** `69e96f5b`, `4095624f`, `4f679d66`, `3d69c375`, `552dd3d3`.
- **Present:**
  - `74919a3b` / `7090e31d` buffer age and damage;
  - Espryt fixes `815217cc` (scissor shadow is per-session), `c822a9fe`, `228bd24c`;
  - `1ff7ca53` / `caa00995` frame-callback pacing;
  - `5a7b23f6` / `258b9460` / `16b07cf4` suspend, resume and resize.
- **Shared images:** `38842985`, `3d17e8c1`, `350fc699`, `67226176`, `b5cab441`, `7520e53b`.
- **Misc:**
  - `8da082f8` clear colour starts at (0,0,0,0);
  - `51c37bcb` sampler param conversion;
  - `15f9b27a` sampler CSO per share group;
  - `239fc228` gl_FragCoord access chains;
  - `bfa7987c` Magma declines invalid SPIR-V;
  - `6125f941` surfaceless/no-config EGL;
  - `4633ca22` frame-stats log gated (`MOBILEGL_SERVER_FRAME_STATS`);
  - `2a1889b8` gtest discovery at ctest start.

### 5. anland work landed (newest first)
- `41ec066` / `f60b261`: Chrome video decode modes.
- `e48874c`: KWin recovers from its own session loss.
- `b9f8162` / `cf07bc7`: Chrome GPU process by default.
- `3d8c4dd` / `5d30340`: Xwayland glamor.
- `89894f1` / `a55e975`: DPMS off while hidden.
- `74bd2d3`: the app starts the desktop, foreground service.
- `0fd5d01` / `144f55c`: APK build guards.
- `90c44d7`: system-wide vendor.
- `23fc73f`: buffer age and damage in KWin.
- `9773d1e` / `2dfda4a`: resize follow.
- `f9e41df`: GBM device for linux-dmabuf.
- `5f15240`: backend choice.

## Known open issues (rough priority)

1. **CI:** retrace failures (see stream 2) and whatever the `2d424146` run still shows red.
2. **Fullscreen performance** at about half of stock (see stream 1).
3. **Espryt's ~10 s Preemption-Fault hang path:** the driver doesn't report the reset to the server's contexts, so a hanging client stalls the desktop for 10 s per hang there.
4. **YUV gaps:**
   - YUV sampling doesn't wait on the write fence (suspected);
   - no single-channel (R8/GR88) dma-buf import, so KWin advertises no YUV formats and mpv's GL interop won't work;
   - the CPU copy for foreign YUV buffers is a fallback, not zero-copy.
5. **Chrome** doesn't use buffer age (full-frame repaints). The Magma text-caret frame costs about 24 ms.
6. **A rare Magma X11 rectangle glitch.** The readback GLX fallback is slower with glamor.
7. **Two GoldControllerTest failures** on Windows hosts. MSVC builds of this branch are broken; build tests in WSL with clang.
8. **Leftovers:**
   - container probe files under `~swung0x48/yuvtest` and the local `v.html` tabs in the Chrome profile;
   - an empty directory `.claude/worktrees/quirky-poincare-df3f3e` (locked by Windows);
   - WSL `~/mgl-espryt*` / `~/mgl-magma*` directories of unknown owner (kept).

## Rules this work follows

- **Commits:** `--no-verify`, one short sentence, no Co-Authored-By trailer. No external project names in code or comments.
- **Tests:** never run the DirectVulkan integration tests (`ctest -E DirectVulkan`); never run two ctest suites at once.
- **Worktrees:** one worktree and build directory per task; remove the worktree, branch and build directories right after merging. Keep `anland-unified`, `mgl-unified`, WSL `~/mgl-xbuild-a64`, `~/mgl-anl-bld` and `~/sysroots`. Don't touch `wgl-host` or `~/repos/mobilegl-cts-gl33`.
- **Core profile only:** don't use fixed-function test apps such as glxgears.
- **Device:**
  - stop device work while the user is using the phone;
  - on USB power, watch the battery (stop below 40%);
  - wireless adb needs re-enabling after a reboot.
