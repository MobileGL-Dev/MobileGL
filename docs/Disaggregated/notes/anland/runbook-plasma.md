# Runbook：在这台 Windows 机器 + 平板上起 / 迭代 MobileGL + anland + KDE Plasma（2026-10-02）

> 操作手册，不是设计笔记。配套 Claude Code skill：`.claude/skills/anland-mobilegl-plasma/`（`SKILL.md` 是本文的精简版，`scripts/` 是本文引用的全部脚本，每个脚本开头注明了参数）。现状、提交与已知问题见 [`handoff-legacy-mobilegl-unified.md`](handoff-legacy-mobilegl-unified.md)，零拷贝的设计见 [`plan-ahb-dmabuf.md`](plan-ahb-dmabuf.md)。下文路径与命令按 2026-10-02 设备上的实际状态核对过；标"未验证"的是没跑通过或只从旧记录推出来的。

**设备可能正被别的会话使用。** 凡是会重启 app / daemon / 容器会话、或替换库的操作，先跑只读的 `scripts/device/status.sh` 看一眼，确认没人在测；`ct.sh` 的通道名用自己的（别人在用 c、c2、c3、xd、xr、gbm）。

## 架构

- **平板**：Lenovo TB321FU（Y700，Adreno 750，Android 15，内核 6.1），**KernelSU** root（`/data/adb/ksu`、`ksud`；不是 Magisk），`su -c` 可用。USB 序列号 `HA27Q3LQ`，也常走无线调试 `ip:port`。
- **anland APK** `com.anland.consumer.mobilegl`（anland 分支 `legacy-mobilegl-unified` 的 debuggable 构建）：UI 进程 + 私有 Service 进程 `:mobilegl`（`MobileGLWorker`）。后者加载 **server** `libMobileGL.so`（NDK 构建，MobileGL 分支 `feat/disaggregated`），从第一个 attach 上来的窗口起监听抽象 socket `@anland-mobilegl`。一个进程服务所有 client（KWin、plasmashell、ksplash、Chrome……），一个进程只服务一种后端。`MobileGLWorker` 是**前台服务**（通知"Linux 桌面正在运行"，带"停止桌面"按钮），窗口隐藏、锁屏、从最近任务划掉之后 server 和会话都还在。
- **后端**：app 设置"渲染后端"（设置 > 连接 > MobileGL 桌面；存在 `/data/data/<包名>/files/mobilegl-backend`，默认 DirectGLES；启动参数 `--es mobilegl_backend X` 也会写它）。Espryt = DirectGLES，Magma = DirectVulkan。server 进程启动时读一次；app 把它发布成 `/data/local/tmp/anland-mobilegl/backend`，会话启动前拷进容器的 `/etc/mobilegl/backend`——所有 client 按这个文件报后端，不一致 server 拒绝（"Hello.backendType disagrees with pinned backend"）。如果有人设了 `debug.mobilegl.backend`，它在下次重启前仍然覆盖设置。
- **实验用 display daemon**：`/data/adb/modules/anland-daemon/display_daemon /data/local/tmp/anland-mobilegl/display.sock`（显示 / 输入中介），由 app 拉起（见下）。模块的 `service.sh` 开机只起原版那个（`/data/local/tmp/display_daemon.sock`），原版不要动。
- **容器** `arch-kde-mgl`：Droidspaces 6.4.5，Arch Linux ARM（aarch64 glibc），host 网络；Android 侧 `rootfs.img` 挂在 `/mnt/Droidspaces/arch-kde-mgl`。bind mount：`/data/local/tmp/anland-mobilegl` → `/run/anland-mobilegl`，`/data/local/tmp/display_daemon.sock` → `/run/display.sock`。
- **会话**：`desktop-session.service`（User=swung0x48，**已 enable**：容器一启动就进会话，和原版镜像一样）。drop-in `/etc/systemd/system/desktop-session.service.d/mobilegl.conf`（anland 的 `producers/kde/Arch_v5/desktop-session-mobilegl.conf`，由 `sync-build-kwin.sh` 安装并 enable）：先 `mobilegl-startup.sh wait`（等 `/proc/net/unix` 里 `@anland-mobilegl` 在监听、daemon socket 存在），再以 root 跑 `sync-backend`，`Restart=always`，ExecStart 是 `/opt/mobilegl/bin/mobilegl-startup.sh plasma`——它删掉粘滞的软件渲染键，整个会话设 `MOBILEGL_IPC_PROBE_TIMEOUT_MS=0`（MobileGL 永不拒绝，glvnd 不会把 KWin / plasmashell 交给别的 GL 栈），它每次写 `plasma-kwin_wayland.service` 的用户 drop-in（经 `kwin_wayland_wrapper` 起 `/opt/mobilegl/kwin/bin/kwin_wayland` + `/opt/mobilegl/kwin/lib/libkwin.so.6.7.4`，`ANLAND_MOBILEGL=1`、`MOBILEGL_IPC_SURFACE=server`），然后 `startplasma-wayland`。
- **KWin 6.7.4** 带 anland 的 `anland` 后端，经 server 直接画进 anland 的 Android Surface。窗口不在屏幕上（consumer 断开）时后端把输出关掉（DPMS Off + RenderLoop inhibit：不合成、不发 frame callback），窗口回来再打开。其余 GL 进程都是 `MOBILEGL_IPC_SURFACE=offscreen`，经 KWin 合成上屏。
- **client 库** `/opt/mobilegl/lib/libMobileGL.so`（glibc aarch64），是容器里**全系统**的 GL vendor（见下节）：EGL vendor `/usr/share/glvnd/egl_vendor.d/10_mobilegl.json`（排在发行版的 `50_*.json` 前面，MobileGL 拒绝时由它接手）、GLX vendor `/usr/lib/libGLX_mobilegl.so.0`，配置来自 `/etc/mobilegl/client.conf`（`MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_DATA=shm MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl`）+ `/etc/mobilegl/backend`。进程不需要任何专门的环境变量。
- **零拷贝窗口**：client 的 `wl_egl_window` 帧是 server 分配的 AHardwareBuffer（shared image），以 dma-buf fd 经 `zwp_linux_dmabuf_v1` 交给 KWin；KWin 用 EGL 导入，server 按 inode 认出是自己发出的那块。KWin 的 DRM 设备来自 `GBM_BACKEND=mobilegl` → `/usr/lib/gbm/mobilegl_gbm.so`（节点只当身份，从不 ioctl）。没有 linux-dmabuf 时退回 wl_shm 回读。
- **X11**（Xwayland）走 MobileGL 的 GLX vendor，每帧回读 + `xcb_put_image`。Chrome 用 `mobilegl-startup.sh chrome`：ANGLE GLES 跑在 MobileGL EGL 上，GPU 在浏览器进程内。

## 首次准备（Windows 主机）

- Git Bash、`adb`（SDK platform-tools）、PATH 上有 CMake ≥ 3.22 与 Ninja（本机 CMake 3.27.6、Ninja 1.13.2）；`rec.sh` 另需 Python 3 + Pillow。
- Android NDK **27.2.12479018**，默认 `%LOCALAPPDATA%/Android/Sdk/ndk/27.2.12479018`（`ANDROID_NDK` 可覆盖）。
- WSL 发行版 **archlinux**：clang、lld、llvm、cmake、ninja。**lld 的大版本必须等于 clang / llvm-libs**（曾经 lld 23 配 llvm 22，`ld.lld` 直接跑不起来；现在全是 22.1.6）。然后导出一次 sysroot：`bash scripts/xsysroot/refresh-sysroot.sh`（得到 WSL 里的 `~/sysroots/arch-kde-mgl` 与 `~/sysroots/aarch64-arch.cmake`，glibc 2.43、gcc 16；容器升级 glibc / gcc / vulkan / libdrm / gbm 后要重跑，再 `xbuild.sh --reconfigure`）。
- MobileGL 用独立 worktree（主 checkout 和它的构建目录是共用的，别动）：`GIT_LFS_SKIP_SMUDGE=1 git worktree add ...`，`git submodule update --init --recursive`，再从主 checkout 拷 `3rdparty/glslang/External/spirv-tools`（submodule 初始化后是空的，configure 会报找不到 SPIR-V tools）。
- anland worktree，分支 `legacy-mobilegl-unified`（KWin patch / 后端源码、启动脚本、`mobilegl-tools/`）。
- 设备侧已经存在、本 runbook **不负责从零搭**的：APK（按 anland `consumers/anland_v5/android_consumer/MOBILEGL.md`，gradle `:app:assemblePlainDebug`，`MOBILEGL_DIST` 指向 server 库，`-PmobileglApplicationId=com.anland.consumer.mobilegl`；换签名重装会换 UID，要重做 KernelSU 授权，见 `mobilegl-tools/ksu_grant.c`）、anland 的 KernelSU 模块（`anland-daemon`、`anland-awl`）、容器本身，以及容器里配置好的 KWin 构建目录 `/root/mobilegl-build/kwin-6.7.4/build` 和原版 KWin 源码 `/root/gbm-dev/kwin-a/src`。从零重建这些未验证。
- 装一次辅助脚本（容器要在跑）：`bash scripts/push-tools.sh`——`device/*.sh` 到 `/data/local/tmp/anl`，`mgrun` 到容器 `/usr/local/bin`，`kwprep.sh` 到容器 `/root`。

主机脚本都在 Git Bash 里 `bash scripts/<x>.sh` 运行。直接敲 adb 时，带设备路径要加 `MSYS_NO_PATHCONV=1`（否则 `/data/...` 被改写成 Git 安装目录），root 命令整个包进一层引号：`adb shell 'su -c "..."'`。设备脚本也可以不推送直接跑：`MSYS_NO_PATHCONV=1 adb shell 'su -c sh' < scripts/device/status.sh`。

## 启动、闲置、停止——没有手动步骤

开机什么都不起（容器 `run_at_boot=0`）。**打开 app**（桌面图标，等同 `am start -n com.anland.consumer.mobilegl/com.anland.consumer.MainActivity`，不带参数；APK 的默认 socket 是实验 daemon 那个，见 `-PanlandDefaultSocket`）会调 `MobileGLDesktop.ensureStarted()`——唯一的触发点——启动前台服务，并以 root（`su`）跑 APK 里的 `assets/mobilegl-desktop.sh up`：daemon 不在就起（脚本先离开 app 的 cgroup，app 被杀时 daemon 和容器不受牵连）→ 发布后端 → 容器没跑就启动 → `systemctl start --no-block desktop-session`。窗口等 daemon socket 出现；Surface attach 后 server 开始监听；会话的 `wait` 放行，Plasma 起来。全都在跑时再打开只是重新挂上窗口。日志：`/data/local/tmp/anland-mobilegl/desktop.log`，logcat `AnlandMobileGL`（"MobileGL desktop up done in N ms"）。

窗口隐藏、熄屏、锁屏、从最近任务划掉：server 与会话都留着（前台服务）；KWin 记 "viewer gone: output powered down" 然后闲置，回来时记 "viewer back"。**停止桌面**（通知按钮，或 `am startservice -n <包名>/com.anland.consumer.MobileGLWorker -a com.anland.consumer.mobilegl.STOP`）跑 `mobilegl-desktop.sh down`（停会话、停容器，daemon 留着），关窗口，结束 `:mobilegl` 进程。

```sh
adb connect <ip>:<port>        # 只有无线调试才需要
export MSYS_NO_PATHCONV=1
adb shell 'su -c "sh /data/local/tmp/anl/bringup.sh"'     # 重启后：点亮、解开锁屏、打开 app、打印各步耗时
bash scripts/desktop-verify.sh push cold reopen idle      # 冷启动计时、划掉再打开、可见/隐藏/锁屏的 CPU 与 GPU
adb shell 'su -c "sh /data/local/tmp/anl/status.sh"'
```

`droidspaces` 没有 `status` 子命令，`droidspaces show` 列出在跑的容器。`device/run-plasma.sh` 是开发用的重启（会话 + logind 用户，再重开 app），重新构建之后需要——KWin 不会重连新的 server 进程。`device/desktop-reset.sh` 把一切停到"刚重启"的状态。以前的手动序列（setprop、起容器、`restart-daemon.sh`、`run-plasma.sh`）不再需要；`restart-daemon.sh` 只留作调试 daemon。

## 构建与部署循环

### server（Android，NDK）→ 替换 APK 里的库

```sh
bash scripts/build-android.sh     # → <worktree>/build-android/libMobileGL.so（RelWithDebInfo，不 strip，约 320 MB，留着给 addr2line）
bash scripts/cyc.sh s1            # 删粘滞软件渲染键 → 推库 → 换进 APK → 重启会话 → 截图 $ANL_DIR/s1.png → 打印 fatal
```

`device/swap.sh` 直接 root 覆盖 `/data/app/.../lib/arm64/libMobileGL.so`，保留 owner / 权限 / SELinux context，第一次运行把原件存为 `/data/local/tmp/anland-mobilegl/libMobileGL.so.orig`；不重签 APK（重签要重装，UID 会变，授权要重做）。只换库不跑 cyc：推到 `/data/local/tmp/anland-mobilegl/libMobileGL.new.so`，`su -c "sh /data/local/tmp/anl/swap.sh"`，之后照样要 `run-plasma.sh`——server 进程换了，KWin 不会重连。

### client + GBM 后端（容器 glibc），WSL 交叉编译

```sh
bash scripts/xbuild.sh [--reconfigure]   # WSL ~/mgl-xbuild-a64；全量约 150 秒，增量几秒
bash scripts/xdeploy.sh                  # 新 inode 改名就位安装 libMobileGL.so + mobilegl_gbm.so，刷新别名、GLX 链接、vendor JSON、client.conf 和 GLX/GBM 环境 drop-in
```

已经在跑的 client 继续映射旧 inode：重启那个 client，KWin / plasmashell 就 `run-plasma.sh`。server 与 client **同一个 commit** 构建（两边 stamp 都取 `git rev-parse HEAD`），否则日志里 WARN "compatible wire with different build"。容器内构建是备用路径（慢）：`scripts/sync-src.sh` 同步改动的文件，再在容器里跑 anland 的 `mobilegl-tools/anland-build-client.sh`。

### KWin（anland 后端 / kwin.patch），在容器里编

```sh
bash scripts/kwin-stage.sh build                                  # 去 CR 后暂存 patch + 后端 + 启动脚本，跑 kwprep.sh，后台起 sync-build-kwin.sh
printf 'tail -5 /root/kwin-build.log\n' | bash scripts/ct.sh kw   # 等到 "== ALL DONE =="
adb shell 'su -c "sh /data/local/tmp/anl/run-plasma.sh"'
```

两处暂存是因为两个容器脚本读的地方不同：`kwprep.sh` 读容器 `/root/kwsync.tgz`，把 `kwin.patch` 重新打到原版 KWin 源码的副本上，只把内容变了的文件拷进活树（保持 ninja 增量）；anland 的 `sync-build-kwin.sh`（`producers/kde/Arch_v5/`）读 `/run/anland-mobilegl/kwin-sync/`，只同步后端源码，`ninja -j4`，再以原子改名安装 `kwin_wayland`、`libkwin.so.6.7.4`、打过补丁的 screencast 插件和 `/opt/mobilegl/bin/mobilegl-startup.sh`，不重启合成器。仓库根的 `libdisplay_producer/` 两个脚本都不同步。`kwin-stage.sh` 是把这两步拼起来的，还没整体跑过。

### 单元测试（不用设备）

这个分支的 MSVC 构建是坏的，在 WSL 里编：

```sh
MSYS2_ARG_CONV_EXCL='*' wsl -d archlinux -- bash /mnt/c/<worktree>/.claude/skills/anland-mobilegl-plasma/scripts/wsl-test.sh MultiSessionTest TwinOwnershipTest
MSYS2_ARG_CONV_EXCL='*' wsl -d archlinux -- bash -lc 'cd ~/mgl-anl-bld && ctest -R "MultiSession|TwinOwnership" -E DirectVulkan --output-on-failure'
```

`wsl-all.sh` 编并跑全部单元测试。DirectVulkan 集成测试永远不要跑；两个 ctest 不要同时跑。

## 切换后端

在 app 里：设置 > 连接 > MobileGL 桌面 > 渲染后端，然后通知里"停止桌面"，再打开 app。或者 `adb shell 'su -c "sh /data/local/tmp/anl/switch.sh DirectGLES"'`，做的是同一件事（用启动参数保存设置、STOP 服务、重新打开；顺手清掉残留的 `debug.mobilegl.backend`）。不用 setprop，也不用改容器里的文件：会话下次启动时把发布的后端拷进 `/etc/mobilegl/backend`。`cyc.sh` 不改后端。

## 全系统 vendor 与回退

`xdeploy.sh`（以及 anland 的 `anland-build-client.sh`）安装下面这些，容器里任何进程经原版 libglvnd / libgbm 就用上 MobileGL：

| 文件 | 作用 |
|---|---|
| `/usr/share/glvnd/egl_vendor.d/10_mobilegl.json` | EGL vendor，先于系统的 `50_*.json` 被问 |
| `/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json` | 同一份 JSON，留给 `__EGL_VENDOR_LIBRARY_FILENAMES` 单进程强制 |
| `/etc/mobilegl/client.conf` | 所有 client 的 `KEY=value` 默认值（transport、data、endpoint），每次部署重写 |
| `/etc/mobilegl/backend` | 后端（`switch.sh` 写）；比 client.conf 里的 `MOBILEGL_BACKEND_TYPE` 优先 |
| `/etc/environment.d/10-mobilegl.conf` | `__GLX_VENDOR_LIBRARY_NAME=mobilegl`、`GBM_BACKEND=mobilegl`，给 systemd 用户管理器（所有 Plasma 服务及其子进程，包括终端） |
| `/etc/profile.d/mobilegl.sh` | 同样两项，给登录 shell（`bash -l`、`su -l`、ssh、`droidspaces run bash -l`） |

优先级：环境变量 > `/etc/mobilegl/backend`（只管后端）> client.conf > 内置默认；`MOBILEGL_CONFIG_FILE` / `MOBILEGL_BACKEND_FILE` 可指向别的文件（设为空即关闭）；server 进程不读这些文件。

**回退**：真正拉起之前 client 先探一次配置的 endpoint（一次非阻塞 connect，`MOBILEGL_IPC_PROBE_TIMEOUT_MS` 默认 250，0 关闭；被拒的抽象 socket 约 0.2 ms 就有结果）。没有 server 时：EGL vendor 返回 `EGL_NO_DISPLAY`、设备数 0（glvnd 换下一个 vendor），GLX vendor 的 `__glx_Main` 失败（libGLX 回退到 Xwayland 报的 vendor），`mobilegl_gbm` 的 `create_device` 失败（libgbm 回退到设备自己的后端）。客户端查询（`eglQueryString(EGL_NO_DISPLAY)`、`eglGetProcAddress`）从不拉起会话。对别的后端创建的 GBM 设备和 `LIBGL_ALWAYS_SOFTWARE=1` 的进程，MobileGL 同样让开——Xwayland（KWin 补丁给它设了这个）因此继续走软件栈。不停真 server 的演示：
```sh
sed 's|^MOBILEGL_IPC_CONTROL=.*|MOBILEGL_IPC_CONTROL=unix:@nobody|' /etc/mobilegl/client.conf > /tmp/down.conf
sudo -u swung0x48 env -i HOME=/home/swung0x48 PATH=/usr/bin XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
  MOBILEGL_CONFIG_FILE=/tmp/down.conf eglinfo -B -p wayland     # vendor 变成另一个，约 0.1 s
```

## 跑测试 client 与验证

在容器里以 root 执行（`printf '...' | bash scripts/ct.sh <通道>`），`mgrun offscreen <命令>` 以 swung0x48 身份在当前会话里起一个 client（日志 `/tmp/mgl-run.client.log`，`MGLOG=/tmp/x.log` 可换）：

```sh
mgrun offscreen eglinfo -B ; mgrun offscreen glxinfo -B
timeout 25 mgrun offscreen glmark2-es2-wayland -s 1280x720 --run-forever > /tmp/gm.out 2>&1 &
sleep 8; grep -a "presented through" /tmp/mgl-run.client.log
```

Chrome：`bash scripts/chrome-launch.sh <tag> [参数]`（日志 `/tmp/mgl-chrome-<tag>.client.log`，截图 `ch-<tag>.png`）。探针：`scripts/probes/texfmt.c`（各纹理格式是否保住 alpha）、`dmahash.c`（对某进程的 dma-buf 取样哈希）；放进容器 `/root` 编，例如 `gcc -O1 /root/texfmt.c -o /usr/local/bin/mgl-texfmt -lEGL`，用 `mgrun offscreen` 跑（之前的确切编译参数未验证）。

结论成立要同时满足：

- client 日志是 `presented through linux-dmabuf shared images`，不是 `wl_shm`；出现 `could not be presented through a linux-dmabuf shared image` 说明退回了；
- client 在动的时候 `bash scripts/shot.sh g 2 1` 两张截图哈希不同——dma-buf 下分数"很快"可能是帧根本没上屏；
- `adb logcat -b crash -d` 与 `adb logcat -d | grep -E "Fatal signal|Abort message|E MobileGL"` 干净——窗口冻住多半是 server 死了；
- 对 plasmashell 下结论前，粘滞软件渲染已关（见下），且 `/tmp/mgl-plasmashell.client.log` 是新写的。

参考（glmark2-es2-wayland 1280x720，零拷贝 vs wl_shm）：Espryt 187 vs 49，Magma 158 vs 34。出乎意料的通过要复跑。

## 日志

| 内容 | 位置 |
|---|---|
| KWin 的 client 侧 | 容器 `/tmp/mobilegl-compositor.client.log` |
| plasmashell / ksplash | `/tmp/mgl-plasmashell.client.log`、`/tmp/mgl-ksplash.client.log`（用户 drop-in `~/.config/systemd/user/plasma-{plasmashell,ksplash}.service.d/mobilegl-log.conf`） |
| mgrun / Chrome | `/tmp/mgl-run.client.log`、`/tmp/mgl-chrome-<tag>.client.log`（+ `/tmp/chrome-<tag>-out.log`） |
| server | `adb logcat -s MobileGL`；文件 `/data/data/com.anland.consumer.mobilegl/files/mobilegl-server.server.log` |
| native 崩溃 | `adb logcat -b crash -d`；容器里 `coredumpctl list` |
| 会话 / KWin stderr | 容器 `journalctl -u desktop-session --no-pager`、`journalctl --no-pager --since -2min \| grep kwin_wayland` |
| display daemon | `/data/local/tmp/anland-mobilegl/display-daemon.log` |

`MOBILEGL_LOG_FILE_PATH=/tmp/x.log` 实际写 `/tmp/x.client.log`（server 转发的行进 `.server.log`），打开时截断，所以每个进程一个 base。client 构建的日志级别是 INFO。server 栈帧用 `$ANDROID_NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-addr2line.exe -Cfe build-android/libMobileGL.so <偏移>` 符号化。

## 常见坑

- **pkill -f 误杀自己**：`ct.sh` / `droidspaces run bash -lc "<脚本>"` 的进程参数里带着整段脚本，`pkill -f chrome` 会先把执行者杀掉。用 `pkill -x`。
- **/tmp 里 root 的文件**：root 跑 client 留下的日志，之后 swung0x48 打不开（日志不出现）。先以 root `rm -f`。
- **Plasma 软件渲染是粘滞的**：server 一崩，Plasma 就把 `[QtQuickRendererSettings] SceneGraphBackend=software` 写进 `~swung0x48/.config/kdeglobals`，之后 plasmashell 全用 CPU 渲染，截图照样正常（托盘里有"正在使用的软件渲染器"图标）。`mobilegl-startup.sh plasma` 每次会话启动都会删掉它；一个已经崩过的会话里，下结论前先重启会话（`run-plasma.sh`）。
- **锁屏**：自动锁屏已关（`kscreenlockerrc` 的 `[Daemon] Autolock=false`）；真锁上只能输密码——这种 logind 会话不支持 lock-session，`device/unlock.sh` 的 unlock-session 能否解开未验证。测 greeter 用 `/usr/lib/kscreenlocker_greet --testing` 再杀掉。
- **CRLF**：本机 `core.autocrlf=true`，anland 的后端源码和 `sync-build-kwin.sh` 在 Windows checkout 里是 CRLF。凡是要在 Linux / Android 上执行的先 `tr -d '\r'`（`ct.sh`、`push-tools.sh`、`kwin-stage.sh` 已处理）；skill 目录有 `.gitattributes` 强制 LF。
- **display daemon**：由 app 拉起；会话的 `wait` 步骤保证 KWin 不会在它之前启动（连不上时 KWin 报 "failed to connect to display daemon at /run/anland-mobilegl/display.sock"，约 3 秒退出）。从 app 的 `su` 拉起的东西必须先离开 app 的 cgroup（`echo $$ > /sys/fs/cgroup/cgroup.procs`），否则 force-stop 会一并杀掉。
- **电量 / 连接**：电量低或熄屏（默认 30 秒）可能让 SurfaceView 被回收——`svc power stayon true`；无线调试的端口每次配对都变——脚本里不要写死 `-s HA27Q3LQ`，多设备时设 `ANDROID_SERIAL`。
- **容器与 Android 路径**：容器的 `/tmp` 和平板的 `/data/local/tmp` 不是一个地方；文件经 `/mnt/Droidspaces/arch-kde-mgl/root`（= 容器 `/root`）或 `/data/local/tmp/anland-mobilegl`（= 容器 `/run/anland-mobilegl`）中转。
- **正在映射的 .so 不要原地覆盖**（容器里或运行中的 APK）：拷成新 inode 再 `mv -f`；APK 换库前先 force-stop。
- **输入注入**：普通 `adb shell input` 被拦，要 `su -c input`；anland 抓了触摸屏，`input tap` 无效时用 `input mouse tap x y`。不要对容器进程用 `debuggerd -b`（信号 35 会直接杀掉 glibc 进程）。
- **linux-dmabuf**：client 必须 `create` + roundtrip，不能 `create_immed`（KWin 对失败的立即导入回协议错误，直接断开 client）。
- **跨会话 bug**：某个 client 起来或退出后出现噪点 / 别的窗口的残影，先怀疑多会话 server 里的进程级状态（handle 是每 client 各自的，FBO / VAO 是每 context 的）。复现用 `kscreenlocker_greet --testing`。
- `/etc/environment` 经 pam_env（以及 `/usr/lib/environment.d/99-environment.conf` 进用户管理器）带进基线会话的驱动变量，`mobilegl-startup.sh` 里把它们 unset 了，改脚本时保留。
- **共用日志路径**：Xwayland 继承 KWin 的 `MOBILEGL_LOG_FILE_PATH`，进程写第一行日志就会打开（截断）那个文件。MobileGL 在不服务的进程里不写日志，但额外起的 client 要给自己的 `MOBILEGL_LOG_FILE_PATH`。
- **残留的单进程覆盖**：unit drop-in 或用户管理器环境（`systemctl --user show-environment`）里遗留的 `__EGL_VENDOR_LIBRARY_FILENAMES` / `MOBILEGL_*` 仍比系统文件优先；`run-plasma.sh`（terminate-user）会清掉管理器里的。
- handoff 里提到的 `kwin_wayland` 日志 shim 与 `/tmp/mgl-kwin-<pid>.log` 已过时：`/opt/mobilegl/kwin/bin/kwin_wayland` 现在就是真二进制，KWin 的 client 日志在 `/tmp/mobilegl-compositor.client.log`。

## 规矩

- 在 git worktree 里干活，不碰共用的主 checkout 和它的构建目录；worktree 要 `GIT_LFS_SKIP_SMUDGE=1`。
- 提交用 `git commit --no-verify`，一句话的短消息，**不加** `Co-Authored-By`。
- DirectVulkan 集成测试不跑（`ctest -E DirectVulkan`）。
- 源码和注释里不写外部项目名（"Mesa"、"Zink" 等），只描述手法。
- 零拷贝路径只用厂商中立的设施：AHB、EGL_ANDROID_*、VK_ANDROID 的 AHB 导入、sync_file、dma-buf、libgbm ABI；不碰 kgsl / gralloc 私有布局 / 高通修饰符。
- 身份缓存用单调递增的生命期 id 作键，不用 GL 名字或指针。
