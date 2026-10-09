---
name: anland-mobilegl-plasma
description: Bring up, rebuild, deploy, switch backends on, test and debug the "MobileGL + anland + KDE Plasma" stack - a rooted Android tablet whose anland APK embeds the MobileGL server, plus a Droidspaces Arch Linux ARM container running KWin/plasmashell/Chrome through the MobileGL client library and GBM backend - driven from this Windows machine (Git Bash, WSL archlinux, adb). Use it after a phone reboot, when changing server (NDK) or client (container glibc) MobileGL code or anland's KWin backend/patch, when switching Espryt/Magma, when validating zero-copy dma-buf presentation, and when chasing crashes, black windows, noise or software-rendering fallbacks in the Plasma session.
---

# MobileGL on anland KDE Plasma

**Setting this up on a new machine or phone from source: `reproduce.md` next to this file.**
Human-readable runbook (Chinese, same content): `docs/Disaggregated/notes/anland/runbook-plasma.md`.
Background: `docs/Disaggregated/notes/anland/handoff-legacy-mobilegl-unified.md` (state, commits, known
issues) and `plan-ahb-dmabuf.md` (zero-copy design).  All scripts are in `scripts/` next to this file;
parameters are documented at the top of each script and in `scripts/env.sh`.

**The phone may be shared with another session.** Before anything that restarts the app, the daemon,
the container session or replaces a library, run the read-only `scripts/device/status.sh` and make sure
nobody else is mid-test.  Use your own `ct.sh` channel names (others use c, c2, c3, xd, xr, gbm).

## Architecture (10 lines)

1. Phone: Lenovo TB321FU (Y700, Adreno 750, Android 15, kernel 6.1), rooted with **KernelSU** (`/data/adb/ksu`, `ksud`; `su -c` works). USB serial `HA27Q3LQ`, or wireless adb `ip:port`.
2. anland APK `com.anland.consumer.mobilegl` (debuggable build of anland branch `legacy-mobilegl-unified`): UI process + private service process `:mobilegl` (`MobileGLWorker`), which loads the **server** `libMobileGL.so` (NDK, from MobileGL `feat/disaggregated`) and listens on abstract socket `@anland-mobilegl` from the first attached window on. One process serves every client (KWin, plasmashell, ksplash, Chrome...), one backend per process. `MobileGLWorker` is a **foreground service** (notification "Linux desktop is running" / action "Stop desktop"), so the server and the session outlive a hidden, locked or swiped-away window.
3. Backend = the app setting "Renderer backend" (Settings > Connection > MobileGL desktop; file `/data/data/<pkg>/files/mobilegl-backend`, default DirectGLES; also `am start ... --es mobilegl_backend X`). Espryt = DirectGLES, Magma = DirectVulkan. The server reads it when its process starts; the app publishes it as `/data/local/tmp/anland-mobilegl/backend` and the session copies that into the container's `/etc/mobilegl/backend` (what every client reports; mismatch = "Hello.backendType disagrees with pinned backend"). `debug.mobilegl.backend`, if someone sets it, still overrides the setting until the next reboot.
4. Experiment display daemon `/data/adb/modules/anland-daemon/display_daemon /data/local/tmp/anland-mobilegl/display.sock` (display/input broker), started by the app (below). The module only starts the original one on `/data/local/tmp/display_daemon.sock` at boot; leave that alone.
5. Container `arch-kde-mgl` (Droidspaces 6.4.5, Arch Linux ARM aarch64 glibc, host net, `rootfs.img` mounted at `/mnt/Droidspaces/arch-kde-mgl` on the Android side). Bind mounts: `/data/local/tmp/anland-mobilegl` -> `/run/anland-mobilegl`, `/data/local/tmp/display_daemon.sock` -> `/run/display.sock`.
6. `desktop-session.service` (User=swung0x48, **enabled**: the container boots straight into it, as the original image does) has drop-in `/etc/systemd/system/desktop-session.service.d/mobilegl.conf` (anland `producers/kde/Arch_v5/desktop-session-mobilegl.conf`, installed + enabled by `sync-build-kwin.sh`): `ExecStartPre=mobilegl-startup.sh wait` (until `@anland-mobilegl` listens in `/proc/net/unix` and the daemon socket exists), `ExecStartPre=+... sync-backend`, `Restart=always`, then `ExecStart=/opt/mobilegl/bin/mobilegl-startup.sh plasma`, which deletes the sticky software-renderer key, sets `MOBILEGL_IPC_PROBE_TIMEOUT_MS=0` session-wide (MobileGL never declines, so glvnd never falls through to another GL stack), writes a user drop-in for `plasma-kwin_wayland.service` (kwin_wayland_wrapper -> `/opt/mobilegl/kwin/bin/kwin_wayland` + `/opt/mobilegl/kwin/lib/libkwin.so.6.7.4`, `ANLAND_MOBILEGL=1`, `MOBILEGL_IPC_SURFACE=server`) and runs `startplasma-wayland`.
7. KWin 6.7.4 with anland's `anland` backend renders straight into the anland Android Surface through the server. While the window is not on screen (consumer disconnected) the backend powers its output down (DPMS Off + RenderLoop inhibited: no compositing, no frame callbacks) and back up when the window returns. Every other GL process is `MOBILEGL_IPC_SURFACE=offscreen` and presents through KWin.
8. Client = `/opt/mobilegl/lib/libMobileGL.so` (glibc aarch64), the container's **system-wide** GL vendor (section below): glvnd EGL vendor `/usr/share/glvnd/egl_vendor.d/10_mobilegl.json` (sorts before the distribution's `50_*.json`, which serves whenever MobileGL declines), GLX vendor `/usr/lib/libGLX_mobilegl.so.0`, settings from `/etc/mobilegl/client.conf` (`MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_DATA=shm MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl`) + `/etc/mobilegl/backend`. No per-process environment is needed.
9. Zero-copy windows: client `wl_egl_window` frames are server-allocated AHardwareBuffer "shared images" exported as dma-buf fds over `zwp_linux_dmabuf_v1`; KWin imports them with EGL and the server recognises its own buffer by inode. KWin gets a DRM device via `GBM_BACKEND=mobilegl` -> `/usr/lib/gbm/mobilegl_gbm.so` (node is only an identity, never ioctl'd). Without linux-dmabuf the client falls back to wl_shm readback.
10. X11 (`docs/Disaggregated/notes/anland/plan-x11-gpu.md`): Xwayland draws with **glamor on MobileGL** (an offscreen session; `GBM_BACKEND=mobilegl` on KWin's render node; window pixmaps are shared images handed to KWin as linux-dmabuf buffers; its `glFlush` publishes them through the server, `/tmp/mgl-xwayland.client.log`); `ANLAND_XWAYLAND_GLAMOR=0` in KWin's drop-in brings back the software `-shm` Xwayland. GLX windows present their frames as shared images through **DRI3 + Present** (log `presented through DRI3+Present shared images`), falling back to readback + MIT-SHM / `xcb_put_image` when the X server has no DRI3 (`MOBILEGL_GLX_PRESENT=readback` forces it per app). Chrome runs ANGLE-GLES over MobileGL EGL with the GPU in its own process (`mobilegl-startup.sh chrome`; `MOBILEGL_CHROME_GPU=in-process` opts out), presenting GBM dma-bufs over linux-dmabuf; after a device loss Chrome restarts that process and repaints.

## First-time prerequisites (Windows host)

- Git Bash, `adb` (SDK platform-tools), CMake >= 3.22 and Ninja on PATH (seen: CMake 3.27.6, Ninja 1.13.2), Python 3 + Pillow (only `rec.sh`).
- Android NDK **27.2.12479018** at `%LOCALAPPDATA%/Android/Sdk/ndk/27.2.12479018` (override `ANDROID_NDK`).
- WSL distro **archlinux** with clang, lld, llvm, cmake, ninja. **lld's major version must equal clang/llvm-libs'** (lld 23 on llvm 22 broke `ld.lld`; working set: all 22.1.6). Then export the sysroot once: `bash scripts/xsysroot/refresh-sysroot.sh` (-> `~/sysroots/arch-kde-mgl` + `~/sysroots/aarch64-arch.cmake`; re-run after container package upgrades, then `xbuild.sh --reconfigure`).
- MobileGL worktree (never build in the shared main checkout): `GIT_LFS_SKIP_SMUDGE=1 git worktree add ...`, `git submodule update --init --recursive`, and copy `3rdparty/glslang/External/spirv-tools` from the main checkout (the submodule leaves it empty).
- anland worktree on `legacy-mobilegl-unified` (KWin patch/backend, startup script, `mobilegl-tools/`).
- Phone side that already exists and is NOT recreated by this skill: the APK (build: anland `consumers/anland_v5/android_consumer/MOBILEGL.md`, gradle `:app:assemblePlainDebug` with `MOBILEGL_DIST`, `-PmobileglApplicationId=com.anland.consumer.mobilegl -PanlandDefaultSocket=/data/local/tmp/anland-mobilegl/display.sock`, from Git Bash with `MSYS_NO_PATHCONV=1`; signed with this machine's `~/.android/debug.keystore`, so `adb install -r` works; reinstalling with another key changes the UID and needs the KernelSU grant again - `mobilegl-tools/ksu_grant.c`), the anland KernelSU modules (`anland-daemon`, `anland-awl`), the container with KWin's configured build tree `/root/mobilegl-build/kwin-6.7.4/build` and pristine `/root/gbm-dev/kwin-a/src`.
- Install the helpers once (container must be running): `bash scripts/push-tools.sh` (device/*.sh -> `/data/local/tmp/anl`, `mgrun` -> container `/usr/local/bin`, `kwprep.sh` -> container `/root`).

Run host scripts from Git Bash as `bash scripts/<x>.sh`.  Raw adb with device paths needs `MSYS_NO_PATHCONV=1` and the whole root command in one quoted string: `adb shell 'su -c "..."'`.  A device script can also be run without pushing: `MSYS_NO_PATHCONV=1 adb shell 'su -c sh' < scripts/device/status.sh`.

## Starting, idling, stopping - no manual steps

Nothing starts at phone boot (container `run_at_boot=0`). **Opening the app** (launcher icon = `am start -n com.anland.consumer.mobilegl/com.anland.consumer.MainActivity`, no extras; the APK's default socket is the experiment's, `-PanlandDefaultSocket`) calls `MobileGLDesktop.ensureStarted()` - the single trigger - which starts the foreground service and runs the APK's `assets/mobilegl-desktop.sh up` as root (`su`): display daemon if absent (the script leaves the app's cgroup first, so it survives the app) -> backend published -> container started if stopped -> `systemctl start --no-block desktop-session`. The window waits for the daemon socket; the server listens once the Surface is attached; the session's `wait` step then lets Plasma start. A reopen with everything up only reattaches the window. Log: `/data/local/tmp/anland-mobilegl/desktop.log`, logcat `AnlandMobileGL` ("MobileGL desktop up done in N ms").

Hidden window, screen off, lock screen or swiped-away task: server and session stay (foreground service); KWin turns the workspace's DPMS off (logs "viewer gone: output powered down"; windows get xdg_toplevel `suspended`, no frame callbacks) and back on ("viewer back"). Client swaps are paced by frame callbacks (`MG_Impl/EGLImpl/FrameThrottle.h`): interval >= 1 waits like any Wayland EGL; interval 0 (Chrome) drops to ~1 fps once a callback goes unanswered for 200 ms.

Measured 2026-10-03 (Espryt, Chrome on webglsamples aquarium; % of one core, phone = all of Android): open-to-desktop cold 7.5-12.5 s (daemon 0.5, server 1.0, container 1.5, kwin ~5, plasmashell ~6-7), after a phone reboot 8.5 s, reopen of a running desktop 0.2 s; visible phone 249% / kwin 13 / chrome 99 / server 57 / GPU 22%, hidden 54 / 0 / 6 / 2 / 0%, locked 38 / 0 / 8 / 3 / 0%. Stop desktop 1.7 s. Wireless adb does not come back by itself after a reboot (re-enable it on the phone). **Stop desktop** (notification action, or `am startservice -n <pkg>/com.anland.consumer.MobileGLWorker -a com.anland.consumer.mobilegl.STOP`) runs `mobilegl-desktop.sh down` (session + container stopped, daemon stays), closes the windows, ends `:mobilegl`.

```sh
adb connect <ip>:<port>                        # only for wireless debugging
export MSYS_NO_PATHCONV=1
adb shell 'su -c "sh /data/local/tmp/anl/bringup.sh"'     # after a reboot: wake, dismiss keyguard, open the app, print the timeline
bash scripts/desktop-verify.sh push cold reopen idle      # timed cold start, swipe-away + reopen, CPU/GPU visible/hidden/locked
adb shell 'su -c "sh /data/local/tmp/anl/status.sh"'
```

`droidspaces` has no `status` command; `droidspaces show` lists running containers. `device/run-plasma.sh` is a developer restart (session + logind user, then the app), needed after a rebuild because KWin does not reconnect to a new server process. `device/desktop-reset.sh` takes everything down to the post-reboot state. The old manual sequence (setprop, start container, `restart-daemon.sh`, `run-plasma.sh`) is gone; `restart-daemon.sh` remains for daemon debugging only.

## Build and deploy loops

**Server (Android, NDK) -> APK lib**
```sh
bash scripts/build-android.sh          # -> <worktree>/build-android/libMobileGL.so (RelWithDebInfo, unstripped)
bash scripts/cyc.sh s1                 # clear sticky SW-renderer key, push, swap into APK, restart session, screenshot $ANL_DIR/s1.png, print fatals
```
`device/swap.sh` overwrites `/data/app/.../lib/arm64/libMobileGL.so` keeping owner/mode/SELinux context (original saved as `/data/local/tmp/anland-mobilegl/libMobileGL.so.orig`); no re-signing. To swap without the session restart: push to `/data/local/tmp/anland-mobilegl/libMobileGL.new.so`, `su -c "sh /data/local/tmp/anl/swap.sh"`, then `run-plasma.sh` anyway (KWin does not reconnect to a new server process).

**Client + GBM backend (container glibc) via WSL cross-build**
```sh
bash scripts/xbuild.sh [--reconfigure]  # WSL ~/mgl-xbuild-a64; ~150 s full, seconds incremental
bash scripts/xdeploy.sh                 # rename-into-place install of libMobileGL.so + mobilegl_gbm.so, aliases, vendor JSONs, client.conf, GLX/GBM env drop-ins
```
Running clients keep the old inode; restart the client, or `run-plasma.sh` for KWin/plasmashell.  Build server and client from the **same commit** (both stamp `git rev-parse HEAD`), else the logs WARN "compatible wire with different build".  Fallback in-container build (slow): `scripts/sync-src.sh` + anland `mobilegl-tools/anland-build-client.sh`.

**KWin (anland backend / kwin.patch) - built inside the container**
```sh
bash scripts/kwin-stage.sh build        # stage CR-stripped patch+backend+startup script, kwprep.sh, background sync-build-kwin.sh
printf 'tail -5 /root/kwin-build.log\n' | bash scripts/ct.sh kw     # wait for "== ALL DONE =="
adb shell 'su -c "sh /data/local/tmp/anl/run-plasma.sh"'
```
`sync-build-kwin.sh` (anland `producers/kde/Arch_v5/`) copies only the backend sources, then `ninja -j4`, installs `kwin_wayland`, `libkwin.so.6.7.4`, the patched screencast plugin and `/opt/mobilegl/bin/mobilegl-startup.sh` by atomic rename.  `kwprep.sh` is what applies changed `kwin.patch` hunks to core KWin files.  Repo-root `libdisplay_producer/` is not synced by either.  `kwin-stage.sh` is assembled from those two scripts and was not run as one unit yet.

**Unit tests (no device)** - in WSL (the MSVC build of this branch is broken):
```sh
MSYS2_ARG_CONV_EXCL='*' wsl -d archlinux -- bash /mnt/c/<worktree>/.claude/skills/anland-mobilegl-plasma/scripts/wsl-test.sh MultiSessionTest TwinOwnershipTest
MSYS2_ARG_CONV_EXCL='*' wsl -d archlinux -- bash -lc 'cd ~/mgl-anl-bld && ctest -R "MultiSession|TwinOwnership" -E DirectVulkan --output-on-failure'
```
`wsl-all.sh` builds and runs every unit test.  Never run DirectVulkan integration tests; never two ctest runs at once.

## Switching backends

In the app: Settings > Connection > MobileGL desktop > Renderer backend, then Stop desktop and open the app. Or `adb shell 'su -c "sh /data/local/tmp/anl/switch.sh DirectGLES"'`, which does the same (saves the setting through the launch extra, STOPs the service, reopens; clears a stale `debug.mobilegl.backend`). No setprop, no file to edit in the container: the session copies the published backend into `/etc/mobilegl/backend` on its next start. `cyc.sh` keeps whatever is set.

## System-wide vendor and fallback

Installed by `xdeploy.sh` (and anland's `anland-build-client.sh`), so any process in the container gets MobileGL through the stock libglvnd/libgbm:

| File | Does |
|---|---|
| `/usr/share/glvnd/egl_vendor.d/10_mobilegl.json` | EGL vendor, asked before the system's `50_*.json` |
| `/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json` | same JSON, for per-process forcing with `__EGL_VENDOR_LIBRARY_FILENAMES` |
| `/etc/mobilegl/client.conf` | `KEY=value` defaults for every client (transport, data, endpoint); rewritten on deploy |
| `/etc/mobilegl/backend` | the backend (the session's `sync-backend` step copies the app's choice in); wins over a `MOBILEGL_BACKEND_TYPE` line in client.conf |
| `/etc/environment.d/10-mobilegl.conf` | `__GLX_VENDOR_LIBRARY_NAME=mobilegl`, `GBM_BACKEND=mobilegl` for the systemd user manager (all Plasma services and what they start, terminals included) |
| `/etc/profile.d/mobilegl.sh` | the same two for login shells (`bash -l`, `su -l`, ssh, `droidspaces run bash -l`) |

Precedence is environment > `/etc/mobilegl/backend` (backend only) > client.conf > built-in; `MOBILEGL_CONFIG_FILE` / `MOBILEGL_BACKEND_FILE` name other files (empty = off); a server process never reads them.

**Fallback.** Before bringing anything up the client probes the configured endpoint (one non-blocking connect, `MOBILEGL_IPC_PROBE_TIMEOUT_MS`, default 250, 0 = off; a refused abstract socket answers in ~0.2 ms). With no server: the EGL vendor returns `EGL_NO_DISPLAY` and no devices (glvnd moves on to the other vendor), the GLX vendor fails `__glx_Main` (libGLX falls back to the vendor Xwayland names), and `mobilegl_gbm` fails `create_device` (libgbm falls back to the device's own backend). Client queries (`eglQueryString(EGL_NO_DISPLAY)`, `eglGetProcAddress`) never bring a session up. MobileGL also steps aside for a GBM device another backend created. It no longer steps aside for `LIBGL_ALWAYS_SOFTWARE=1` (918d40dd): that is Mesa's software-renderer selection, not a reason to disable MobileGL's entry points, so a process that sets it (a software Xwayland included) is still served by MobileGL while a server answers. MobileGL never opens an X connection to a display its own process serves (`MG_Util/X11/DisplayGuard.h`; `MOBILEGL_X11_DIAL=0` forbids all), which is what made a glamor Xwayland deadlock on itself before. Show it without stopping the real server:
```sh
sed 's|^MOBILEGL_IPC_CONTROL=.*|MOBILEGL_IPC_CONTROL=unix:@nobody|' /etc/mobilegl/client.conf > /tmp/down.conf
sudo -u swung0x48 env -i HOME=/home/swung0x48 PATH=/usr/bin XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
  MOBILEGL_CONFIG_FILE=/tmp/down.conf eglinfo -B -p wayland     # vendor = the other one, ~0.1 s
```

## Test clients and how to verify

X11 on the GPU has its own driver, `scripts/x11-verify.sh` (preflight, measure before/after with glxgears at `vblank_mode=0` on both GLX paths (presentation throughput only: glxgears is fixed-function GL, which MobileGL does not implement, so its window is black by design), deploy-client/-kwin/-server, check, apps, stop); `scripts/device/x11-measure.sh` refuses to load the phone below 40% battery.

Inside the container as root (`printf '...' | bash scripts/ct.sh <chan>`), `mgrun offscreen <cmd>` runs a client as swung0x48 in the live session through a login shell, with nothing but the surface mode and the log path set (log `/tmp/mgl-run.client.log`, override base with `MGLOG=/tmp/x.log`); a plain `env -i ... bash -l -c '<cmd>'` as swung0x48 gets MobileGL the same way:
```sh
mgrun offscreen eglinfo -B ; mgrun offscreen glxinfo -B
timeout 25 mgrun offscreen glmark2-es2-wayland -s 1280x720 --run-forever > /tmp/gm.out 2>&1 &
sleep 8; grep -a "presented through" /tmp/mgl-run.client.log
```
Chrome: `bash scripts/chrome-launch.sh <tag> [flags]` (log `/tmp/mgl-chrome-<tag>.client.log`, screenshot `ch-<tag>.png`; `CHROME_GPU=in-process` puts the GPU back in the browser, which cannot recover from a device loss).
Video: Chrome decodes in hardware. The GPU process's render node has a VA-API driver in the container (`/usr/lib/dri/msm_drm_drv_video.so`, DroidSpaces' V4L2 driver): Chrome allocates its frames with `vaCreateSurfaces` and exports them (`vaExportSurfaceHandle`, one fd, R8 + GR88 layers) - the driver's own buffers, never GBM ones - and imports each as an NV12 (P010 for 10-bit) EGLImage bound to `GL_TEXTURE_EXTERNAL_OES`. MobileGL takes such a FOREIGN YUV dma-buf through its labelled CPU-copy fallback: the server maps it and copies it into a YUV AHardwareBuffer of its own (`AHardwareBuffer_lockPlanes`) at the reader's first use in each frame (any present, publishing flush or native fence), then converts that into the texture's RGBA level on the GPU (Espryt: `GL_EXT_YUV_target` with the import's colour hints, `MOBILEGL_YUV_DRIVER_CONVERSION=1` for the driver's external sampler; Magma: `VkSamplerYcbcrConversion` on the AHB's external format). A server-allocated NV12/P010 buffer (GBM `gbm_bo_create(NV12)`) needs no copy. `MOBILEGL_CHROME_VIDEO_DECODE=software` in `mobilegl-startup.sh chrome` brings back `--disable-accelerated-video-decode`. Check a video with `scripts/probes/cdpmedia.py <port> <s>` in the container (CDP `Media.enable`: `kVideoDecoderName = VaapiVideoDecoder`, `kIsPlatformVideoDecoder = true`; `CDP_NAVIGATE=<url>` / `CDP_EXPR=<js>`), which needs Chrome started with `--remote-debugging-port` and a non-default `--user-data-dir` (a fresh profile also needs `--password-store=basic`, else page loads wait forever on KWallet), and grep Chrome's log for `exit_code` (a GPU-process restart). `scripts/probes/yuvprobe.c` pixel-checks foreign NV12/P010 imports, their per-frame refresh and a GBM NV12 buffer (`mgrun offscreen mgl-yuvprobe`, exit 0 = pass). KWin's own NV12 path (it splits a YUV buffer into R8 + GR88 imports) is not served: MobileGL imports no single-channel dma-bufs, so KWin advertises no YUV format over linux-dmabuf. A vendor-agnostic replacement driver (MobileVA: VA-API on Android MediaCodec, decoding into server YUV images with no CPU copy, the current driver kept as an A/B fallback) is planned, not built: `docs/MobileVA/plan.md`.
Device loss: `setprop debug.mobilegl.inject_device_lost_pid <pid>` (as root) loses the server session of the client whose pid - as it sees itself in the container - is `<pid>`, once per value, on any backend and whether or not it presents; `debug.mobilegl.inject_device_lost_at N` instead loses whichever session makes the N-th device check (Espryt presents and read-backs only). `bash scripts/chrome-recover.sh <in-process|process> [rounds]` checks that Chrome repaints after such a loss without a restart (battery-guarded, stops below 40%). `bash scripts/kwin-recover.sh [rounds]` loses KWin's own session (default twice) and checks that KWin restarts compositing on a fresh session - same KWin, plasmashell and client processes, the screen still animating (glmark2 on Wayland, glxgears on X11, `CHROME=1` adds Chrome) - which needs the anland backend + kwin.patch reset handling deployed (`kwin-stage.sh build`).
Probes: `scripts/probes/texfmt.c` (texture formats keep alpha?), `dmahash.c` (hash a process's dma-bufs), `yuvprobe.c` (YUV dma-buf sampling, `gcc -O1 /root/yuvprobe.c -o /usr/local/bin/mgl-yuvprobe -lEGL -lGLESv2 -lgbm -lm`), `cdpmedia.py` (which decoder a Chrome page uses); build in the container, e.g. `gcc -O1 /root/texfmt.c -o /usr/local/bin/mgl-texfmt -lEGL`, run with `mgrun offscreen`.
GPU hangs: a shader that never ends stops the WHOLE GPU (every process, MobileGL or not) until the kernel resets it - ~2 s, or ~10 s ("Preemption Fault" in dmesg) when a higher-priority context waits; nothing in user space shortens one stall. What the server does is end the guilty session so it cannot hang the GPU again (Magma: event-bracketed submissions + `mgl-gpu-watch`, budget `MOBILEGL_GPU_HANG_BUDGET_MS` / `debug.mobilegl.gpu_hang_budget_ms`, default 2000; Espryt: the driver's GUILTY reset status, which it reports after the ~2 s reset only). Repro: `scripts/probes/hangclient.c` (Wayland + GL 3.3 core; `-H 60 -N 1 -E 60` hangs every 60 frames, `-H -1` is a frame-gap observer; build with wayland-scanner's xdg-shell files, `-lwayland-client -lwayland-egl -lEGL -lOpenGL -lm`), and `gpuprobe.c` (NDK, plain adb shell; `gpuprobe 30 high` is a high-priority GLES context outside MobileGL - it both measures the device-wide stall and forces the 10 s preemption path).

A result counts only if ALL hold:
- client log says `presented through linux-dmabuf shared images` (not `wl_shm`; `could not be presented through a linux-dmabuf shared image` = fell back);
- `bash scripts/shot.sh g 2 1` gives two different hashes while the client animates (a "fast" score can mean frames are not shown);
- `adb logcat -b crash -d` and `adb logcat -d | grep -E "Fatal signal|Abort message|E MobileGL"` are clean (a frozen window is usually a dead server);
- for plasmashell verdicts, the sticky software renderer is off (below) and `/tmp/mgl-plasmashell.client.log` is freshly written.
Reference (glmark2-es2-wayland 1280x720, zero-copy vs wl_shm): Espryt 187 vs 49, Magma 158 vs 34.  Rerun any surprising pass.

## Logs

| What | Where |
|---|---|
| KWin client side | container `/tmp/mobilegl-compositor.client.log` |
| plasmashell / ksplash | `/tmp/mgl-plasmashell.client.log`, `/tmp/mgl-ksplash.client.log` (user drop-ins `~/.config/systemd/user/plasma-{plasmashell,ksplash}.service.d/mobilegl-log.conf`) |
| mgrun / Chrome | `/tmp/mgl-run.client.log`, `/tmp/mgl-chrome-<tag>.client.log` (+ `/tmp/chrome-<tag>-out.log`) |
| Server | `adb logcat -s MobileGL`; file `/data/data/com.anland.consumer.mobilegl/files/mobilegl-server.server.log` |
| Native crashes | `adb logcat -b crash -d`; container `coredumpctl list` |
| Session / KWin stderr | container `journalctl -u desktop-session --no-pager`, `journalctl --no-pager --since -2min \| grep kwin_wayland` |
| Display daemon | `/data/local/tmp/anland-mobilegl/display-daemon.log` |
| Desktop start/stop (app's su script) | `/data/local/tmp/anland-mobilegl/desktop.log`, `container-start.log`; logcat `-s AnlandMobileGL` |
| Session wait / backend sync | container `journalctl -u desktop-session --no-pager` ("waiting for the MobileGL server ...") |

`MOBILEGL_LOG_FILE_PATH=/tmp/x.log` produces `/tmp/x.client.log` (and `.server.log` for forwarded lines); files are truncated on open, so one base per process.  Client builds log at INFO.  Symbolize server frames with `$ANDROID_NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-addr2line.exe -Cfe build-android/libMobileGL.so <offset>`.

## Pitfalls

- **pkill -f self-match**: a `ct.sh`/`droidspaces run bash -lc "<script>"` process carries the script in its argv; `pkill -f chrome` kills the runner. Use `pkill -x`.
- **Root-owned /tmp files**: a log created by a root-run client cannot be reopened by swung0x48 later (no log appears). `rm -f` it as root first.
- **Sticky Plasma software renderer**: after any server crash Plasma writes `[QtQuickRendererSettings] SceneGraphBackend=software` into `~swung0x48/.config/kdeglobals`; plasmashell then renders on the CPU and screenshots still look fine (tray icon "正在使用的软件渲染器"). `mobilegl-startup.sh plasma` deletes it at every session start; within a running session that hit a crash, restart the session (`run-plasma.sh`) before any verdict.
- **Lock screen**: autolock is off (`kscreenlockerrc [Daemon] Autolock=false`); a real lock can only be left by typing the password (logind lock/unlock is not supported for this session type - `device/unlock.sh` is unverified). Test the greeter with `/usr/lib/kscreenlocker_greet --testing` and kill it.
- **CRLF**: `core.autocrlf=true` here; anland's backend sources and `sync-build-kwin.sh` are CRLF in the Windows checkout. Strip CR before anything runs on Linux/Android (`tr -d '\r'`; `ct.sh`, `push-tools.sh`, `kwin-stage.sh` do). This skill has a `.gitattributes` forcing LF.
- **Display daemon**: the app starts it; the session's `wait` step keeps KWin from starting without it (a KWin that cannot reach it logs "failed to connect to display daemon at /run/anland-mobilegl/display.sock" and exits in ~3 s). Anything started from the app's `su` must leave the app's cgroup (`echo $$ > /sys/fs/cgroup/cgroup.procs`) or a force-stop kills it.
- **Device power/connection**: low battery or the screen turning off (30 s default) can kill the SurfaceView - `svc power stayon true`; wireless adb `ip:port` changes per pairing - never pin `-s HA27Q3LQ`, use `ANDROID_SERIAL` if several devices are attached.
- **Container vs phone paths**: container `/tmp` is a different place from the phone's `/data/local/tmp`; move files via `/mnt/Droidspaces/arch-kde-mgl/root` (container `/root`) or `/data/local/tmp/anland-mobilegl` (= container `/run/anland-mobilegl`).
- **Never overwrite a mapped .so in place** (container or APK while running): copy to a new inode and `mv -f`; the APK swap force-stops the app first.
- **Input**: plain `adb shell input` is blocked - use `su -c input`; anland grabs the touchscreen, so if `input tap` does nothing use `input mouse tap x y`. Never `debuggerd -b` a container process (signal 35 kills glibc processes).
- **linux-dmabuf**: the client must use `create` + roundtrip, never `create_immed` (KWin answers a failed immediate import with a protocol error that kills the client).
- **Cross-session bugs**: noise / another window's pixels after some client starts or exits = process-global state in the multi-session server (handles are per client, FBO/VAO per context). Repro with `kscreenlocker_greet --testing`.
- `/etc/environment` leaks the baseline session's driver variables through pam_env (and, via `/usr/lib/environment.d/99-environment.conf`, into the user manager); `mobilegl-startup.sh` unsets them - keep that when editing it.
- **Shared log path**: Xwayland inherits KWin's `MOBILEGL_LOG_FILE_PATH`; any line a process logs opens (truncates) that file. MobileGL stays silent in processes it does not serve, but give any extra client its own `MOBILEGL_LOG_FILE_PATH`.
- **Stale per-process overrides**: an `__EGL_VENDOR_LIBRARY_FILENAMES` / `MOBILEGL_*` left in a unit drop-in or the user manager's environment (`systemctl --user show-environment`) still wins over the system files; `run-plasma.sh` (terminate-user) clears the manager's.
- `handoff` mentions a `kwin_wayland` logging shim and `/tmp/mgl-kwin-<pid>.log`: stale - `/opt/mobilegl/kwin/bin/kwin_wayland` is now the real binary and logs to `/tmp/mobilegl-compositor.client.log`.

## House rules

- Work in a git worktree, never in the shared main checkout or its build dirs; worktrees need `GIT_LFS_SKIP_SMUDGE=1`.
- Commit with `git commit --no-verify`, one short sentence, **no** `Co-Authored-By` trailer.
- Never run the DirectVulkan integration tests (`ctest -E DirectVulkan`).
- No external project names ("Mesa", "Zink", ...) in source code or comments; describe the technique.
- Vendor-agnostic only on the zero-copy path: AHB, EGL_ANDROID_*, VK_ANDROID AHB import, sync_file, dma-buf, libgbm ABI - no kgsl/gralloc layouts/QCOM modifiers.
- Identity caches key on monotonic lifetime ids, never GL names or pointers.
