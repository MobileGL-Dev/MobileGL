# Runbook: bring up and iterate MobileGL + anland + KDE Plasma on this Windows machine and the tablet (2026-10-02)

> An operations manual, not design notes. Companion Claude Code skill: `tools/anland-mobilegl-plasma/` (`SKILL.md` is the condensed version of this document, and `scripts/` holds every script it references, each with its parameters at the top). For status, commits and known issues see [`handoff-legacy-mobilegl-unified.md`](handoff-legacy-mobilegl-unified.md); for the zero-copy design see [`plan-ahb-dmabuf.md`](plan-ahb-dmabuf.md). The paths and commands below were checked against the device state on 2026-10-02; items marked "unverified" have not been run through or are inferred only from older records.

**The device may be in use by another session.** For anything that restarts the app / daemon / container session or replaces a library, first run the read-only `scripts/device/status.sh` and check that nobody is testing; use your own `ct.sh` channel name (others use c, c2, c3, xd, xr, gbm).

## Architecture

- **Tablet**: Lenovo TB321FU (Y700, Adreno 750, Android 15, kernel 6.1), **KernelSU** root (`/data/adb/ksu`, `ksud`; not Magisk), `su -c` works. USB serial `HA27Q3LQ`, but wireless debugging `ip:port` is also common.
- **anland APK** `com.anland.consumer.mobilegl` (debuggable build of the anland branch `legacy-mobilegl-unified`): the UI process plus a private Service process `:mobilegl` (`MobileGLWorker`). The latter loads the **server** `libMobileGL.so` (NDK build, MobileGL branch `feat/disaggregated`) and listens on the abstract socket `@anland-mobilegl` from the first window that attaches. One process serves every client (KWin, plasmashell, ksplash, Chrome, ...), and one process serves only one backend. `MobileGLWorker` is a **foreground service** (notification "Linux desktop is running", with a "Stop desktop" button); after the window is hidden, the screen locks, or the app is swiped from recents, the server and the session are still there.
- **Backend**: the app's "render backend" setting (Settings > Connection > MobileGL desktop; stored in `/data/data/<package>/files/mobilegl-backend`, default DirectGLES; the launch argument `--es mobilegl_backend X` also writes it). Espryt = DirectGLES, Magma = DirectVulkan. The server process reads it once at startup; the app publishes it to `/data/local/tmp/anland-mobilegl/backend`, and it is copied into the container's `/etc/mobilegl/backend` before a session starts — every client reports the backend from that file, and the server refuses a mismatch ("Hello.backendType disagrees with pinned backend"). If someone set `debug.mobilegl.backend`, it still overrides the setting until the next restart.
- **Experimental display daemon**: `/data/adb/modules/anland-daemon/display_daemon /data/local/tmp/anland-mobilegl/display.sock` (the display / input intermediary), started by the app (see below). The module's `service.sh` only starts the original one at boot (`/data/local/tmp/display_daemon.sock`); leave the original alone.
- **Container** `arch-kde-mgl`: Droidspaces 6.4.5, Arch Linux ARM (aarch64 glibc), host networking; on the Android side `rootfs.img` is mounted at `/mnt/Droidspaces/arch-kde-mgl`. Bind mounts: `/data/local/tmp/anland-mobilegl` → `/run/anland-mobilegl`, `/data/local/tmp/display_daemon.sock` → `/run/display.sock`.
- **Session**: `desktop-session.service` (User=swung0x48, **enabled**: the container enters the session as soon as it starts, like the stock image). The drop-in `/etc/systemd/system/desktop-session.service.d/mobilegl.conf` (anland's `producers/kde/Arch_v5/desktop-session-mobilegl.conf`, installed and enabled by `sync-build-kwin.sh`): first `mobilegl-startup.sh wait` (wait for `@anland-mobilegl` to listen in `/proc/net/unix` and for the daemon socket), then run `sync-backend` as root; `Restart=always`; ExecStart is `/opt/mobilegl/bin/mobilegl-startup.sh plasma` — it deletes the sticky software-renderer key, sets `MOBILEGL_IPC_PROBE_TIMEOUT_MS=0` for the whole session (MobileGL never declines, so glvnd never hands KWin / plasmashell to another GL stack), writes the user drop-in for `plasma-kwin_wayland.service` on every start (starting `/opt/mobilegl/kwin/bin/kwin_wayland` + `/opt/mobilegl/kwin/lib/libkwin.so.6.7.4` through `kwin_wayland_wrapper`, with `ANLAND_MOBILEGL=1`, `MOBILEGL_IPC_SURFACE=server`), then runs `startplasma-wayland`.
- **KWin 6.7.4** with anland's `anland` backend draws straight into anland's Android Surface through the server. While the window is not on screen (consumer disconnected) the backend powers the output down (DPMS Off + RenderLoop inhibit: no compositing, no frame callbacks) and back up when the window returns. Every other GL process is `MOBILEGL_IPC_SURFACE=offscreen` and presents through KWin.
- **Client library** `/opt/mobilegl/lib/libMobileGL.so` (glibc aarch64) is the **system-wide** GL vendor in the container (next section): EGL vendor `/usr/share/glvnd/egl_vendor.d/10_mobilegl.json` (sorts before the distribution's `50_*.json`, which takes over when MobileGL declines), GLX vendor `/usr/lib/libGLX_mobilegl.so.0`, settings from `/etc/mobilegl/client.conf` (`MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_DATA=shm MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl`) + `/etc/mobilegl/backend`. Processes need no per-process environment variables.
- **Zero-copy windows**: the client's `wl_egl_window` frames are server-allocated AHardwareBuffers (shared images), handed to KWin as dma-buf fds over `zwp_linux_dmabuf_v1`; KWin imports them with EGL and the server recognizes its own buffer by inode. KWin's DRM device comes from `GBM_BACKEND=mobilegl` → `/usr/lib/gbm/mobilegl_gbm.so` (the node is only an identity and is never ioctl'd). Without linux-dmabuf the client falls back to wl_shm readback.
- **X11** (Xwayland) goes through MobileGL's GLX vendor, with per-frame readback + `xcb_put_image`. Chrome uses `mobilegl-startup.sh chrome`: ANGLE GLES on MobileGL EGL, with the GPU inside the browser process.

## First-time setup (Windows host)

- Git Bash, `adb` (SDK platform-tools), CMake ≥ 3.22 and Ninja on PATH (this machine has CMake 3.27.6, Ninja 1.13.2); `rec.sh` additionally needs Python 3 + Pillow.
- Android NDK **27.2.12479018**, default `%LOCALAPPDATA%/Android/Sdk/ndk/27.2.12479018` (`ANDROID_NDK` overrides).
- WSL distro **archlinux**: clang, lld, llvm, cmake, ninja. **lld's major version must match clang / llvm-libs** (once lld 23 was paired with llvm 22 and `ld.lld` would not run; everything is 22.1.6 now). Then export the sysroot once: `bash scripts/xsysroot/refresh-sysroot.sh` (produces `~/sysroots/arch-kde-mgl` and `~/sysroots/aarch64-arch.cmake` in WSL; glibc 2.43, gcc 16; rerun after the container upgrades glibc / gcc / vulkan / libdrm / gbm, then `xbuild.sh --reconfigure`).
- Use a separate worktree for MobileGL (the main checkout and its build directory are shared; do not touch them): `GIT_LFS_SKIP_SMUDGE=1 git worktree add ...`, `git submodule update --init --recursive`, then copy `3rdparty/glslang/External/spirv-tools` from the main checkout (the submodule is empty after initialization and configure reports missing SPIR-V tools).
- anland worktree, branch `legacy-mobilegl-unified` (KWin patch / backend sources, startup scripts, `mobilegl-tools/`).
- Already present on the device, and this runbook does **not** cover building from scratch: the APK (per anland's `consumers/anland_v5/android_consumer/MOBILEGL.md`, gradle `:app:assemblePlainDebug`, `MOBILEGL_DIST` pointing at the server library, `-PmobileglApplicationId=com.anland.consumer.mobilegl`; reinstalling with a different signature changes the UID and requires redoing the KernelSU grant, see `mobilegl-tools/ksu_grant.c`), anland's KernelSU modules (`anland-daemon`, `anland-awl`), the container itself, and the container's configured KWin build directory `/root/mobilegl-build/kwin-6.7.4/build` and stock KWin sources `/root/gbm-dev/kwin-a/src`. Rebuilding these from scratch is unverified.
- Install the helper scripts once (the container must be running): `bash scripts/push-tools.sh` — `device/*.sh` to `/data/local/tmp/anl`, `mgrun` to the container's `/usr/local/bin`, `kwprep.sh` to the container's `/root`.

Host scripts all run from Git Bash as `bash scripts/<x>.sh`. When typing adb commands directly, add `MSYS_NO_PATHCONV=1` for device paths (otherwise `/data/...` is rewritten to the Git installation directory), and wrap root commands in one more layer of quotes: `adb shell 'su -c "..."'`. Device scripts can also run without pushing: `MSYS_NO_PATHCONV=1 adb shell 'su -c sh' < scripts/device/status.sh`.

## Start, idle, stop — no manual steps

Nothing starts at boot (the container has `run_at_boot=0`). **Opening the app** (desktop icon; equivalent to `am start -n com.anland.consumer.mobilegl/com.anland.consumer.MainActivity` with no arguments; the APK's default socket is the experimental daemon one, see `-PanlandDefaultSocket`) calls `MobileGLDesktop.ensureStarted()` — the only trigger — which starts the foreground service and runs the APK's `assets/mobilegl-desktop.sh up` as root (`su`): start the daemon if absent (the script first leaves the app's cgroup, so killing the app does not take the daemon and container with it) → publish the backend → start the container if not running → `systemctl start --no-block desktop-session`. The window waits for the daemon socket; the server starts listening after the Surface attaches; the session's `wait` proceeds and Plasma comes up. Opening it again while everything runs only re-attaches the window. Logs: `/data/local/tmp/anland-mobilegl/desktop.log`, logcat `AnlandMobileGL` ("MobileGL desktop up done in N ms").

Hiding the window, screen off, lock screen, swiping from recents: the server and session stay (foreground service); KWin logs "viewer gone: output powered down" and idles, and "viewer back" on return. **Stop desktop** (notification button, or `am startservice -n <package>/com.anland.consumer.MobileGLWorker -a com.anland.consumer.mobilegl.STOP`) runs `mobilegl-desktop.sh down` (stops the session and container, leaves the daemon), closes the window, and ends the `:mobilegl` process.

```sh
adb connect <ip>:<port>        # only needed for wireless debugging
export MSYS_NO_PATHCONV=1
adb shell 'su -c "sh /data/local/tmp/anl/bringup.sh"'     # after a reboot: wake, unlock, open the app, print each step's time
bash scripts/desktop-verify.sh push cold reopen idle      # cold-start timing, swipe and reopen, CPU and GPU while visible/hidden/locked
adb shell 'su -c "sh /data/local/tmp/anl/status.sh"'
```

`droidspaces` has no `status` subcommand; `droidspaces show` lists the running containers. `device/run-plasma.sh` is the development restart (session + logind user, then reopen the app), needed after a rebuild — KWin does not reconnect to a new server process. `device/desktop-reset.sh` stops everything back to "just rebooted". The old manual sequence (setprop, start the container, `restart-daemon.sh`, `run-plasma.sh`) is no longer needed; `restart-daemon.sh` remains only for debugging the daemon.

## Build and deploy loop

### Server (Android, NDK) → replace the library in the APK

```sh
bash scripts/build-android.sh     # → <worktree>/build-android/libMobileGL.so (RelWithDebInfo, not stripped, ~320 MB, keep it for addr2line)
bash scripts/cyc.sh s1            # delete the sticky software-renderer key → push the library → swap it into the APK → restart the session → screenshot $ANL_DIR/s1.png → print fatals
```

`device/swap.sh` overwrites `/data/app/.../lib/arm64/libMobileGL.so` directly as root, preserving owner / mode / SELinux context, and saves the original as `/data/local/tmp/anland-mobilegl/libMobileGL.so.orig` on first run; it does not re-sign the APK (re-signing requires reinstalling, which changes the UID and requires redoing the grant). To replace only the library without running cyc: push to `/data/local/tmp/anland-mobilegl/libMobileGL.new.so`, `su -c "sh /data/local/tmp/anl/swap.sh"`, and still run `run-plasma.sh` afterwards — the server process changed and KWin will not reconnect.

### Client + GBM backend (container glibc), cross-compiled in WSL

```sh
bash scripts/xbuild.sh [--reconfigure]   # WSL ~/mgl-xbuild-a64; a full build takes about 150 seconds, incremental a few
bash scripts/xdeploy.sh                  # install libMobileGL.so + mobilegl_gbm.so under a new inode via atomic rename, refresh aliases, GLX links, vendor JSON, client.conf and the GLX/GBM environment drop-in
```

A running client keeps mapping the old inode: restart that client, and for KWin / plasmashell run `run-plasma.sh`. Build the server and client from the **same commit** (both stamps take `git rev-parse HEAD`); otherwise the log warns "compatible wire with different build". Building inside the container is the fallback (slow): `scripts/sync-src.sh` syncs the changed files, then run anland's `mobilegl-tools/anland-build-client.sh` in the container.

### KWin (anland backend / kwin.patch), built in the container

```sh
bash scripts/kwin-stage.sh build                                  # strip CR, stage the patch + backend + startup scripts, run kwprep.sh, start sync-build-kwin.sh in the background
printf 'tail -5 /root/kwin-build.log\n' | bash scripts/ct.sh kw   # wait for "== ALL DONE =="
adb shell 'su -c "sh /data/local/tmp/anl/run-plasma.sh"'
```

There are two staging areas because the two container scripts read different places: `kwprep.sh` reads the container's `/root/kwsync.tgz`, re-applies `kwin.patch` to a copy of the stock KWin sources, and copies only files whose content changed into the live tree (keeping ninja incremental); anland's `sync-build-kwin.sh` (`producers/kde/Arch_v5/`) reads `/run/anland-mobilegl/kwin-sync/`, syncs only the backend sources, runs `ninja -j4`, then installs `kwin_wayland`, `libkwin.so.6.7.4`, the patched screencast plugin and `/opt/mobilegl/bin/mobilegl-startup.sh` with an atomic rename, without restarting the compositor. Neither script syncs the repository-root `libdisplay_producer/`. `kwin-stage.sh` combines the two steps, but has not been run end to end.

### Unit tests (no device)

The MSVC build of this branch is broken; build in WSL:

```sh
MSYS2_ARG_CONV_EXCL='*' wsl -d archlinux -- bash /mnt/c/<repo>/tools/anland-mobilegl-plasma/scripts/wsl-test.sh MultiSessionTest TwinOwnershipTest
MSYS2_ARG_CONV_EXCL='*' wsl -d archlinux -- bash -lc 'cd ~/mgl-anl-bld && ctest -R "MultiSession|TwinOwnership" -E DirectVulkan --output-on-failure'
```

`wsl-all.sh` builds and runs every unit test. Never run the DirectVulkan integration tests; never run two ctests at once.

## Switching the backend

In the app: Settings > Connection > MobileGL desktop > render backend, then "Stop desktop" in the notification, then open the app again. Or `adb shell 'su -c "sh /data/local/tmp/anl/switch.sh DirectGLES"'`, which does the same thing (save the setting through the launch argument, STOP the service, reopen; it also clears a leftover `debug.mobilegl.backend`). No setprop and no editing files in the container: the next session copies the published backend into `/etc/mobilegl/backend`. `cyc.sh` does not change the backend.

## System-wide vendor and fallback

`xdeploy.sh` (and anland's `anland-build-client.sh`) installs the following, so any process in the container that goes through stock libglvnd / libgbm uses MobileGL:

| File | Role |
|---|---|
| `/usr/share/glvnd/egl_vendor.d/10_mobilegl.json` | EGL vendor, asked before the system's `50_*.json` |
| `/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json` | The same JSON, for forcing one process with `__EGL_VENDOR_LIBRARY_FILENAMES` |
| `/etc/mobilegl/client.conf` | `KEY=value` defaults for every client (transport, data, endpoint), rewritten on each deploy |
| `/etc/mobilegl/backend` | The backend (written by `switch.sh`); takes precedence over `MOBILEGL_BACKEND_TYPE` in client.conf |
| `/etc/environment.d/10-mobilegl.conf` | `__GLX_VENDOR_LIBRARY_NAME=mobilegl`, `GBM_BACKEND=mobilegl`, for the systemd user manager (all Plasma services and their children, including terminals) |
| `/etc/profile.d/mobilegl.sh` | The same two, for login shells (`bash -l`, `su -l`, ssh, `droidspaces run bash -l`) |

Precedence: environment > `/etc/mobilegl/backend` (backend only) > client.conf > built-in defaults; `MOBILEGL_CONFIG_FILE` / `MOBILEGL_BACKEND_FILE` can point at other files (empty disables them); the server process does not read these files.

**Fallback**: before actually bringing anything up, the client probes the configured endpoint once (one non-blocking connect, `MOBILEGL_IPC_PROBE_TIMEOUT_MS` defaults to 250 and 0 disables it; a refused abstract socket answers in about 0.2 ms). Without a server: the EGL vendor returns `EGL_NO_DISPLAY` with 0 devices (glvnd moves to the next vendor), the GLX vendor's `__glx_Main` fails (libGLX falls back to the vendor Xwayland reports), and `mobilegl_gbm`'s `create_device` fails (libgbm falls back to the device's own backend). Client queries (`eglQueryString(EGL_NO_DISPLAY)`, `eglGetProcAddress`) never bring up a session. MobileGL also steps aside for GBM devices created by another backend. `LIBGL_ALWAYS_SOFTWARE=1` no longer makes it step aside (918d40dd): that is only Mesa's software-renderer choice, not a reason to disable the MobileGL entry point; with a server present, processes carrying that variable (including Xwayland, which the KWin patch sets it for) are still served by MobileGL. A demonstration that does not stop the real server:
```sh
sed 's|^MOBILEGL_IPC_CONTROL=.*|MOBILEGL_IPC_CONTROL=unix:@nobody|' /etc/mobilegl/client.conf > /tmp/down.conf
sudo -u swung0x48 env -i HOME=/home/swung0x48 PATH=/usr/bin XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
  MOBILEGL_CONFIG_FILE=/tmp/down.conf eglinfo -B -p wayland     # the vendor becomes another one in about 0.1 s
```

## Running test clients and verifying

Run as root in the container (`printf '...' | bash scripts/ct.sh <channel>`); `mgrun offscreen <command>` starts a client as swung0x48 in the current session (log `/tmp/mgl-run.client.log`, override with `MGLOG=/tmp/x.log`):

```sh
mgrun offscreen eglinfo -B ; mgrun offscreen glxinfo -B
timeout 25 mgrun offscreen glmark2-es2-wayland -s 1280x720 --run-forever > /tmp/gm.out 2>&1 &
sleep 8; grep -a "presented through" /tmp/mgl-run.client.log
```

Chrome: `bash scripts/chrome-launch.sh <tag> [args]` (log `/tmp/mgl-chrome-<tag>.client.log`, screenshot `ch-<tag>.png`). Probes: `scripts/probes/texfmt.c` (whether texture formats preserve alpha), `dmahash.c` (hash a process's dma-bufs); compile them in the container's `/root`, for example `gcc -O1 /root/texfmt.c -o /usr/local/bin/mgl-texfmt -lEGL`, and run with `mgrun offscreen` (the exact older compiler flags are unverified).

Video: Chrome decodes in hardware (`VaapiVideoDecoder`). The container's VA-API driver (`msm_drm_drv_video.so`, V4L2 decode) allocates its own surfaces and exports NV12 dma-bufs (a single fd with R8 + GR88 layers), and Chrome binds them as NV12/P010 EGLImages to `GL_TEXTURE_EXTERNAL_OES`. The platform cannot import such **foreign** YUV dma-bufs, so MobileGL uses its explicitly-labeled CPU-copy fallback: the server maps the buffer, copies it into its own YUV AHB at first use in each frame (`AHardwareBuffer_lockPlanes`), then converts it on the GPU into the texture's RGBA level (Espryt: `GL_EXT_YUV_target` plus the import's color hints, with `MOBILEGL_YUV_DRIVER_CONVERSION=1` using the driver's external sampler; Magma: `VkSamplerYcbcrConversion` on the AHB's external format). A server-allocated NV12/P010 (GBM) is zero-copy. `MOBILEGL_CHROME_VIDEO_DECODE=software` restores `--disable-accelerated-video-decode`. To check: in the container `python3 scripts/probes/cdpmedia.py <port> <seconds>` (CDP `Media.enable`, `kVideoDecoderName`), and `scripts/probes/yuvprobe.c` pixel-checks foreign NV12/P010 imports, their per-frame refresh, and a GBM NV12 buffer. KWin's own NV12 path (split into two R8 + GR88 imports) is not supported, so KWin advertises no YUV formats.

A conclusion holds only when all of these hold:

- the client log says `presented through linux-dmabuf shared images`, not `wl_shm`; `could not be presented through a linux-dmabuf shared image` means it fell back;
- while the client animates, two `bash scripts/shot.sh g 2 1` screenshots hash differently — with dma-buf a "fast" score may mean frames never reached the screen;
- `adb logcat -b crash -d` and `adb logcat -d | grep -E "Fatal signal|Abort message|E MobileGL"` are clean — a frozen window usually means the server died;
- before concluding anything about plasmashell, the sticky software renderer is off (see below) and `/tmp/mgl-plasmashell.client.log` was freshly written.

Reference (glmark2-es2-wayland 1280x720, zero-copy vs wl_shm): Espryt 187 vs 49, Magma 158 vs 34. An unexpected pass must be rerun.

## Logs

| Content | Location |
|---|---|
| KWin client side | container `/tmp/mobilegl-compositor.client.log` |
| plasmashell / ksplash | `/tmp/mgl-plasmashell.client.log`, `/tmp/mgl-ksplash.client.log` (user drop-ins `~/.config/systemd/user/plasma-{plasmashell,ksplash}.service.d/mobilegl-log.conf`) |
| mgrun / Chrome | `/tmp/mgl-run.client.log`, `/tmp/mgl-chrome-<tag>.client.log` (plus `/tmp/chrome-<tag>-out.log`) |
| server | `adb logcat -s MobileGL`; file `/data/data/com.anland.consumer.mobilegl/files/mobilegl-server.server.log` |
| native crash | `adb logcat -b crash -d`; in the container `coredumpctl list` |
| session / KWin stderr | container `journalctl -u desktop-session --no-pager`, `journalctl --no-pager --since -2min \| grep kwin_wayland` |
| display daemon | `/data/local/tmp/anland-mobilegl/display-daemon.log` |

`MOBILEGL_LOG_FILE_PATH=/tmp/x.log` actually writes `/tmp/x.client.log` (lines forwarded by the server go to `.server.log`), truncating on open, so each process gets one base name. Client builds log at INFO. Symbolize server frames with `$ANDROID_NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-addr2line.exe -Cfe build-android/libMobileGL.so <offset>`.

## Common pitfalls

- **pkill -f kills itself**: the process arguments of `ct.sh` / `droidspaces run bash -lc "<script>"` contain the whole script, so `pkill -f chrome` kills the executor first. Use `pkill -x`.
- **Root-owned files in /tmp**: logs left by a root-run client cannot be opened later by swung0x48 (no log appears). Remove them as root first (`rm -f`).
- **Plasma software rendering is sticky**: once the server crashes, Plasma writes `[QtQuickRendererSettings] SceneGraphBackend=software` into `~swung0x48/.config/kdeglobals`, and plasmashell then renders entirely on the CPU while screenshots still look fine (the tray shows a "software renderer in use" icon). `mobilegl-startup.sh plasma` deletes it at every session start; in a session that has already crashed, restart the session (`run-plasma.sh`) before any verdict.
- **Lock screen**: automatic locking is off (`[Daemon] Autolock=false` in `kscreenlockerrc`); a real lock can only be left by typing the password — this logind session type does not support lock-session, and whether `device/unlock.sh`'s unlock-session works is unverified. Test the greeter with `/usr/lib/kscreenlocker_greet --testing` and then kill it.
- **CRLF**: this machine has `core.autocrlf=true`, so anland's backend sources and `sync-build-kwin.sh` are CRLF in the Windows checkout. Anything that runs on Linux / Android must go through `tr -d '\r'` first (`ct.sh`, `push-tools.sh` and `kwin-stage.sh` already do); the skill directory has a `.gitattributes` forcing LF.
- **display daemon**: started by the app; the session's `wait` step ensures KWin does not start before it (when it cannot connect, KWin reports "failed to connect to display daemon at /run/anland-mobilegl/display.sock" and exits after about 3 seconds). Anything started by the app's `su` must first leave the app's cgroup (`echo $$ > /sys/fs/cgroup/cgroup.procs`), otherwise force-stop kills it too.
- **Battery / connection**: low battery or the screen turning off (30 seconds by default) can reclaim the SurfaceView — `svc power stayon true`; the wireless debugging port changes on every pairing — never hardcode `-s HA27Q3LQ` in scripts, and set `ANDROID_SERIAL` when several devices are attached.
- **Container vs Android paths**: the container's `/tmp` and the tablet's `/data/local/tmp` are different places; move files through `/mnt/Droidspaces/arch-kde-mgl/root` (= container `/root`) or `/data/local/tmp/anland-mobilegl` (= container `/run/anland-mobilegl`).
- **Do not overwrite a mapped .so in place** (in the container or a running APK): copy to a new inode and `mv -f`; force-stop the APK before replacing its library.
- **Input injection**: plain `adb shell input` is blocked, use `su -c input`; anland grabs the touchscreen, so when `input tap` does nothing use `input mouse tap x y`. Do not use `debuggerd -b` on container processes (signal 35 kills glibc processes outright).
- **linux-dmabuf**: the client must `create` + roundtrip, not `create_immed` (KWin answers a failed immediate import with a protocol error and disconnects the client).
- **Cross-session bugs**: after a client starts or exits, noise or another window's remnants usually mean process-level state in the multi-session server (handles are per client, FBOs / VAOs are per context). Reproduce with `kscreenlocker_greet --testing`.
- `/etc/environment` enters the baseline session through pam_env (and `/usr/lib/environment.d/99-environment.conf` into the user manager) with driver variables; `mobilegl-startup.sh` unsets them, so keep that when editing the script.
- **Shared log paths**: Xwayland inherits KWin's `MOBILEGL_LOG_FILE_PATH`, and the first log line a process writes opens (truncates) that file. MobileGL does not log in processes it does not serve, but extra clients need their own `MOBILEGL_LOG_FILE_PATH`.
- **Leftover single-process overrides**: `__EGL_VENDOR_LIBRARY_FILENAMES` / `MOBILEGL_*` left in a unit drop-in or the user manager environment (`systemctl --user show-environment`) still beat the system files; `run-plasma.sh` (terminate-user) clears the manager ones.
- The `kwin_wayland` log shim and `/tmp/mgl-kwin-<pid>.log` mentioned in the handoff are obsolete: `/opt/mobilegl/kwin/bin/kwin_wayland` is the real binary now, and KWin's client log is `/tmp/mobilegl-compositor.client.log`.

## Rules

- Work in a git worktree, never in the shared main checkout or its build directories; worktrees need `GIT_LFS_SKIP_SMUDGE=1`.
- Commit as `CONTRIBUTING.md` describes (no trailer), with `git commit --no-verify`.
- Do not run the DirectVulkan integration tests (`ctest -E DirectVulkan`).
- Do not name external projects ("Mesa", "Zink", ...) in source code or comments; describe the technique.
- The zero-copy path uses only vendor-neutral facilities: AHB, EGL_ANDROID_*, VK_ANDROID AHB import, sync_file, dma-buf, the libgbm ABI; never kgsl / gralloc private layouts / Qualcomm modifiers.
- Key identity caches on a monotonic lifetime id, never a GL name or a pointer.
