# Reproducing the MobileGL + anland + KDE Plasma setup from source

This is the from-scratch path for a NEW development machine and/or a NEW phone.  `SKILL.md` covers the
day-to-day loop once everything below exists.  Paths and names that are specific to the original
machine are listed in step 9: set them in `scripts/env.sh` (or the environment) before running anything.

## 0. What you need

**Phone.**
- arm64 Android 11+ with **root**. KernelSU is what this was built on; the scripts only need `su -c`.
- A Vulkan 1.1 driver with `VK_ANDROID_external_memory_android_hardware_buffer` for Magma, and/or GLES 3.2 + EGL_ANDROID_native_fence_sync for Espryt. Nothing vendor-specific is used: no kgsl and no vendor ioctls, only AHardwareBuffer, sync_file and the vendor's public GLES/Vulkan.
- About 20 GB free for the container image.

**Development host.**
- The scripts are written for **Windows + Git Bash** with WSL (`archlinux` distro) for the unit tests and the cross build.
- A Linux host works too: drop the `cygpath`/`MSYS_NO_PATHCONV` handling in `env.sh`, and run the WSL parts natively.
- Tools:
  - `adb`, CMake >= 3.22, Ninja, Python 3 (+ Pillow for screenshots/recording).
  - Android NDK **27.2.12479018**.
  - JDK 17 and Android SDK platform 36 (for the APK).
  - In the Linux environment: clang/lld/llvm of ONE major version (a mismatched lld breaks `ld.lld`), cmake, ninja.

## 1. Sources

```sh
# MobileGL: branch feat/disaggregated (everything here is on it)
git clone https://github.com/MobileGL-Dev/MobileGL.git && cd MobileGL
git checkout feat/disaggregated
GIT_LFS_SKIP_SMUDGE=1 git submodule update --init --recursive
#   3rdparty/glslang is the MobileGL-Dev fork (main); its External/spirv-tools must be populated
#   (glslang's own update_glslang_sources.py, or copy it from another checkout).
# Keep builds out of a shared checkout: work in a worktree (GIT_LFS_SKIP_SMUDGE=1 git worktree add ...).

# anland: branch legacy-mobilegl-unified (KWin backend + patch, startup script, APK, daemon module)
git clone https://github.com/MobileGL-Dev/anland.git && cd anland   # the fork that carries this branch
git checkout legacy-mobilegl-unified
```

Build the server (APK) and the client (container) from the **same MobileGL commit**: both stamp
`git rev-parse HEAD`, and a mismatch logs "compatible wire with different build".

## 2. Phone base: root, Droidspaces, display daemon

1. **Root** with KernelSU (or another `su` provider).
2. **Droidspaces:** install the module and its app. Containers live under `/data/local/Droidspaces/Containers/<name>/`, and the CLI is `/data/local/Droidspaces/bin/droidspaces`.
3. **anland display daemon** (KernelSU/Magisk module `anland-daemon`):
   - Build it from the anland repo with `build_daemon_android.sh`, then package it with `build_magisk_module.sh`, which uses `magisk_module/`. Flash it and reboot.
   - The module's `service.sh` starts the original daemon on `/data/local/tmp/display_daemon.sock`.
   - The MobileGL session uses a SECOND instance of the same binary on `/data/local/tmp/anland-mobilegl/display.sock`: see `scripts/device/restart-daemon.sh`, which also chmods the directory and socket.

## 3. The container

Use an **Arch Linux ARM (aarch64, glibc) KDE Plasma Wayland rootfs** that has a systemd `desktop-session.service` running the desktop as a normal user with `PAMName=login`. A ready-made one is the Arch KDE "Wayland" image from Droidspaces RootFS Desktop Builder (see anland `doc/UserManual/anland_guide.md`, "Use prebuilt packages or a RootFS"). Or install `plasma-desktop`, `plasma-workspace`, `libglvnd`, `xorg-xwayland`, `base-devel`, `cmake` and `ninja` on plain Arch Linux ARM and write the service yourself.

Import it into Droidspaces as `arch-kde-mgl`. The default name is in `env.sh`. Reference `container.config` keys:
```
enable_gpu_mode=1
net_mode=host
bind_mounts=/data/local/tmp/display_daemon.sock:/run/display.sock,/data/local/tmp/anland-mobilegl:/run/anland-mobilegl
run_at_boot=0
```
- **Host networking:** it is required. The MobileGL server listens on the ABSTRACT socket `@anland-mobilegl`, which crosses into the container only when the network namespace is shared.
- **The anland-mobilegl directory:** create `/data/local/tmp/anland-mobilegl` on the phone before the first start, mode 777.
- **Desktop user:** the default is `swung0x48` (`MGL_DESKTOP_USER`). Create your own and point `desktop-session.service` at it.
- **Starting it:** `droidspaces -C .../container.config start`. Android root sees the rootfs at `/mnt/Droidspaces/<name>`, which `CT_ROOT` in `env.sh` relies on.
- **Container tools:** install them with `bash scripts/push-tools.sh` once the container runs.

## 4. MobileGL server and the anland APK

```sh
bash scripts/build-android.sh          # -> <worktree>/build-android/libMobileGL.so
```
The script runs, with the NDK toolchain:
```
-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=RelWithDebInfo
-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON
-DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF
```

APK, from anland `consumers/anland_v5/android_consumer` (details in its `MOBILEGL.md`):
```sh
mkdir -p dist/arm64-v8a && cp <worktree>/build-android/libMobileGL.so dist/arm64-v8a/
MOBILEGL_DIST=$PWD/dist ./gradlew :app:assemblePlainDebug \
  -PanlandNdkVersion=27.2.12479018 -PmobileglApplicationId=com.anland.consumer.mobilegl \
  -PanlandDefaultSocket=/data/local/tmp/anland-mobilegl/display.sock
adb install -r app/build/outputs/apk/plain/debug/app-plain-debug.apk
```
- **Grant root:** the app needs root for its helper; approve it in KernelSU.
- **Reinstall with another signing key:** that changes the UID, so grant root again (anland `mobilegl-tools/ksu_grant.c` does it from a shell). Don't uninstall to fix a signature clash: that wipes the app data.
- **Later server-only changes:** swap just the library into the installed APK (`scripts/cyc.sh` / `device/swap.sh`) instead of rebuilding the APK.

## 5. MobileGL client in the container

Pick one of these two:
- **Cross build** (fast, from WSL or Linux):
  1. `bash scripts/xsysroot/refresh-sysroot.sh` exports the container's headers and libraries to `~/sysroots/arch-kde-mgl` and installs `~/sysroots/aarch64-arch.cmake`. Re-run it after container package upgrades.
  2. `bash scripts/xbuild.sh`, then `bash scripts/xdeploy.sh`.
- **Native build in the container** (slow, no WSL): `scripts/sync-src.sh` copies the worktree to `/root/mobilegl-unified-src`, then run anland `producers/kde/Arch_v5/mobilegl-tools/anland-build-client.sh` inside (g++, `-DMOBILEGL_BUILD_DISAGGREGATED=ON`, Release).

Either route installs the same files, renamed into place so running clients keep their old mapping:

| File | What it is |
|---|---|
| `/opt/mobilegl/lib/libMobileGL.so` (+ `libEGL`/`libGL` aliases) | the client library |
| `/usr/share/glvnd/egl_vendor.d/10_mobilegl.json` | system-wide EGL vendor; sorts before the distribution's `50_*.json`, which serves whenever MobileGL declines (no server reachable) |
| `/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json` | per-process forcing |
| `/usr/lib/libGLX_mobilegl.so.0` | GLX vendor |
| `/usr/lib/gbm/mobilegl_gbm.so` | GBM backend (zero-copy window buffers) |
| `/etc/mobilegl/client.conf` | `MOBILEGL_TRANSPORT=spawn`, `MOBILEGL_IPC_DATA=shm`, `MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl` |
| `/etc/mobilegl/backend` | `DirectVulkan` (Magma) or `DirectGLES` (Espryt) |
| `/etc/environment.d/10-mobilegl.conf`, `/etc/profile.d/mobilegl.sh` | `__GLX_VENDOR_LIBRARY_NAME=mobilegl`, `GBM_BACKEND=mobilegl` |

## 6. KWin 6.7.4 with the anland backend, and Xwayland

The MobileGL session runs a rebuilt KWin from `/opt/mobilegl/kwin`, so the packaged one stays untouched.

1. **Source tree:** in the container, unpack KWin 6.7.4 to `/root/mobilegl-build/kwin-6.7.4`, apply anland `producers/kde/Arch_v5/kwin.patch`, and overlay `producers/kde/anland_backend_Arch_v5/src/backends/anland` into `src/backends/anland`. Also take the current `common/` and `libdisplay_producer/`, as `mobilegl.md` there says.
2. **Configure:**
   ```sh
   cmake -S kwin-6.7.4 -B kwin-6.7.4/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTING=OFF
   ```
3. **Build and install:** run `producers/kde/Arch_v5/sync-build-kwin.sh` (`ninja -j4`). It installs `kwin_wayland` + `libkwin.so.6.7.4` into `/opt/mobilegl/kwin/{bin,lib}`, the patched screencast plugin in place (the original is kept beside it), and `/opt/mobilegl/bin/mobilegl-startup.sh`.
   - From the dev host, `bash scripts/kwin-stage.sh build` does steps 1–3 incrementally: `kwprep.sh` applies changed patch hunks.
4. **Xwayland:** the anland-patched Xwayland 24.1.13 comes from `producers/kde/Arch_v5/build.sh` (makepkg, as a normal user; it builds the anland KWin and Xwayland packages). A prebuilt anland rootfs already has it.
5. **Session hook:** add `/etc/systemd/system/desktop-session.service.d/mobilegl.conf`:
   ```ini
   [Service]
   ExecStart=
   ExecStart=/opt/mobilegl/bin/mobilegl-startup.sh plasma
   Environment=ANLAND_SOCKET=/run/anland-mobilegl/display.sock
   Environment=QT_LOGGING_RULES=kwin_*.info=true
   ```
   `mobilegl-startup.sh plasma` writes the user drop-in for `plasma-kwin_wayland.service` (the `/opt/mobilegl/kwin` binary, `ANLAND_MOBILEGL=1`, `MOBILEGL_IPC_SURFACE=server`, `GBM_BACKEND=mobilegl`) and runs `startplasma-wayland`.

## 7. Bring it up

Nothing has to be started by hand, and the order no longer matters: **open the app** (its launcher
icon, or `am start -n com.anland.consumer.mobilegl/com.anland.consumer.MainActivity` with no extras).
That is the single trigger. The app starts its foreground service and runs its bundled
`mobilegl-desktop.sh up` as root, which:
1. starts the display daemon if it is not running;
2. publishes the backend (the app setting "Renderer backend", default DirectGLES) to
   `/data/local/tmp/anland-mobilegl/backend`;
3. starts the container if it is stopped;
4. starts `desktop-session`, which waits for the server and the daemon socket, copies the backend into
   `/etc/mobilegl/backend` and runs Plasma.

Cold start to a desktop takes about 8-12 s. `scripts/device/bringup.sh` wakes the phone, dismisses the
keyguard, opens the app and prints the timeline. To change the backend, use the app setting and then
Stop desktop and reopen, or run `scripts/device/switch.sh DirectGLES|DirectVulkan`. Do not use
`setprop debug.mobilegl.backend`: it overrides the setting until the next reboot. **Stop desktop**
(the notification action) stops the session and the container.

**After a phone reboot:** wireless adb does not come back by itself. Re-enable it on the phone
(Developer options > Wireless debugging), then `adb connect <ip>:<port>`; the port changes. After
that, open the app as above. See SKILL.md "Starting, idling, stopping".

## 8. Verify

- **Plasma:** it appears in the app. The tray must NOT show the "software renderer" icon; if it does, delete `SceneGraphBackend=software` from the user's `kdeglobals` and restart.
- **Plain shell:** in a plain container shell as the desktop user, `eglinfo -B` names MobileGL, and `glxinfo -B` from a login shell does too.
- **Wayland client:** `timeout 25 mgrun offscreen glmark2-es2-wayland -s 1280x720`. The log must say `presented through linux-dmabuf shared images`.
- **Chrome:** `bash scripts/chrome-launch.sh t1` gives windows with correct icons.
- **Resize and lock:** BACK toggles the extra-keys bar (resize); lock/unlock resumes the desktop. `rtest.sh` in the scratch dir checks both.
- **Unit tests:** `bash scripts/wsl-all.sh` in WSL. NEVER run the DirectVulkan integration tests.

## 9. Machine-specific values to change

| Value | Original | Where |
|---|---|---|
| MobileGL worktree | this skill's worktree | `MGL_WORKTREE` (auto) |
| scratch dir | `%LOCALAPPDATA%/Temp/anl` | `ANL_DIR` |
| NDK | `<SDK>/ndk/27.2.12479018` | `ANDROID_NDK` |
| anland worktree | `%USERPROFILE%/AndroidStudioProjects/anland/.claude/worktrees/mgl-unified` | `ANLAND_WORKTREE` (`kwin-stage.sh`) |
| APK id | `com.anland.consumer.mobilegl` | `MGL_PKG` |
| container | `arch-kde-mgl` | `MGL_CONTAINER` |
| desktop user | `swung0x48` | `MGL_DESKTOP_USER` + `desktop-session.service` |
| WSL distro | `archlinux` | `WSL_DISTRO` (`xbuild.sh`, `wsl-*.sh`) |
| cross build dir | WSL `~/mgl-xbuild-a64` | `MGL_XBUILD_DIR` |
| phone | Lenovo TB321FU, Adreno 750 | nothing: no serial is pinned; with several devices set `ANDROID_SERIAL` |

**Thermals:** a phone vendor's thermal daemon may throttle charging while the screen is on during long sessions. That is not part of this setup.
