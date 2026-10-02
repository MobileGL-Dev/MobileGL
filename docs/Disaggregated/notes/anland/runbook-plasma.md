# Runbook：在这台 Windows 机器 + 平板上起 / 迭代 MobileGL + anland + KDE Plasma（2026-10-02）

> 操作手册，不是设计笔记。配套 Claude Code skill：`.claude/skills/anland-mobilegl-plasma/`（`SKILL.md` 是本文的精简版，`scripts/` 是本文引用的全部脚本，每个脚本开头注明了参数）。现状、提交与已知问题见 [`handoff-legacy-mobilegl-unified.md`](handoff-legacy-mobilegl-unified.md)，零拷贝的设计见 [`plan-ahb-dmabuf.md`](plan-ahb-dmabuf.md)。下文路径与命令按 2026-10-02 设备上的实际状态核对过；标"未验证"的是没跑通过或只从旧记录推出来的。

**设备可能正被别的会话使用。** 凡是会重启 app / daemon / 容器会话、或替换库的操作，先跑只读的 `scripts/device/status.sh` 看一眼，确认没人在测；`ct.sh` 的通道名用自己的（别人在用 c、c2、c3、xd、xr、gbm）。

## 架构

- **平板**：Lenovo TB321FU（Y700，Adreno 750，Android 15，内核 6.1），**KernelSU** root（`/data/adb/ksu`、`ksud`；不是 Magisk），`su -c` 可用。USB 序列号 `HA27Q3LQ`，也常走无线调试 `ip:port`。
- **anland APK** `com.anland.consumer.mobilegl`（anland 分支 `legacy-mobilegl-unified` 的 debuggable 构建）：UI 进程 + 私有 Service 进程 `:mobilegl`（`MobileGLWorker`）。后者加载 **server** `libMobileGL.so`（NDK 构建，MobileGL 分支 `feat/disaggregated`），监听抽象 socket `@anland-mobilegl`。一个进程服务所有 client（KWin、plasmashell、ksplash、Chrome……），一个进程只服务一种后端。
- **后端**：`setprop debug.mobilegl.backend DirectGLES|DirectVulkan`（Espryt = DirectGLES，Magma = DirectVulkan；重启丢失），必须与容器里的 `/etc/mobilegl/backend` 一致——所有 client 按这个文件报后端，不一致 server 拒绝（"Hello.backendType disagrees with pinned backend"）。
- **实验用 display daemon**：`/data/adb/modules/anland-daemon/display_daemon /data/local/tmp/anland-mobilegl/display.sock`（显示 / 输入中介）。**开机不会自动起**——模块的 `service.sh` 只起原版那个（`/data/local/tmp/display_daemon.sock`），原版不要动。
- **容器** `arch-kde-mgl`：Droidspaces 6.4.5，Arch Linux ARM（aarch64 glibc），host 网络；Android 侧 `rootfs.img` 挂在 `/mnt/Droidspaces/arch-kde-mgl`。bind mount：`/data/local/tmp/anland-mobilegl` → `/run/anland-mobilegl`，`/data/local/tmp/display_daemon.sock` → `/run/display.sock`。
- **会话**：`desktop-session.service`（User=swung0x48）的 drop-in 把 ExecStart 换成 `/opt/mobilegl/bin/mobilegl-startup.sh plasma`；它每次写 `plasma-kwin_wayland.service` 的用户 drop-in（经 `kwin_wayland_wrapper` 起 `/opt/mobilegl/kwin/bin/kwin_wayland` + `/opt/mobilegl/kwin/lib/libkwin.so.6.7.4`，`ANLAND_MOBILEGL=1`、`MOBILEGL_IPC_SURFACE=server`），然后 `startplasma-wayland`。
- **KWin 6.7.4** 带 anland 的 `anland` 后端，经 server 直接画进 anland 的 Android Surface。其余 GL 进程都是 `MOBILEGL_IPC_SURFACE=offscreen`，经 KWin 合成上屏。
- **client 库** `/opt/mobilegl/lib/libMobileGL.so`（glibc aarch64）：glvnd 的 EGL vendor（`/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json`）+ GLX vendor（`/usr/lib/libGLX_mobilegl.so.0`，`__GLX_VENDOR_LIBRARY_NAME=mobilegl`）；环境 `MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_DATA=shm MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl`。
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

## 平板重启后的拉起

重启后整套会话一样都不在。可以一条 `adb shell 'su -c "sh /data/local/tmp/anl/bringup.sh [DirectGLES|DirectVulkan]"'`（按下面的步骤拼出来的，还没整体跑过），或者按顺序手敲：

```sh
adb connect <ip>:<port>        # 只有无线调试才需要（比如 USB 供不上电时）
export MSYS_NO_PATHCONV=1
adb shell 'su -c "svc power stayon true"'
adb shell 'su -c "setprop debug.mobilegl.backend DirectVulkan"'      # 或 DirectGLES，与 /etc/mobilegl/backend 一致
adb shell 'su -c "am start -n com.anland.consumer.mobilegl/com.anland.consumer.MainActivity --es socket_path /data/local/tmp/anland-mobilegl/display.sock"'
adb shell 'su -c "/data/local/Droidspaces/bin/droidspaces -C /data/local/Droidspaces/Containers/arch-kde-mgl/container.config start"'
adb shell 'su -c "sh /data/local/tmp/anl/restart-daemon.sh"'        # 实验 daemon；目录 777、socket 666（容器里 uid 1000 要连）
adb shell 'su -c "sh /data/local/tmp/anl/run-plasma.sh"'            # 重启 app 与 desktop-session，约 90 秒
```

`droidspaces` 没有 `status` 子命令，`droidspaces show` 列出在跑的容器。不要用旧的 `/data/local/tmp/restart-daemon.sh`（里面 kill 的是写死的 PID）。起完用 `device/status.sh` 检查：两个进程、daemon、会话 active、kwin_wayland 与 plasmashell 在跑、kdeglobals 里没有软件渲染键。

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
bash scripts/xdeploy.sh                  # 新 inode 改名就位安装 libMobileGL.so + mobilegl_gbm.so，刷新别名、GLX 链接、vendor JSON
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

`adb shell 'su -c "sh /data/local/tmp/anl/switch.sh DirectGLES"'`（设属性、写 `/etc/mobilegl/backend`、重启 app），然后 `run-plasma.sh`——正在跑的会话里各 client 还钉在旧后端上。`cyc.sh` 不改后端。

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
- **Plasma 软件渲染是粘滞的**：server 一崩，Plasma 就把 `[QtQuickRendererSettings] SceneGraphBackend=software` 写进 `~swung0x48/.config/kdeglobals`，之后 plasmashell 全用 CPU 渲染，截图照样正常（托盘里有"正在使用的软件渲染器"图标）。下结论前删掉：`sudo -u swung0x48 kwriteconfig6 --file kdeglobals --group QtQuickRendererSettings --key SceneGraphBackend --delete`（cyc.sh 会做），再重启会话。
- **锁屏**：自动锁屏已关（`kscreenlockerrc` 的 `[Daemon] Autolock=false`）；真锁上只能输密码——这种 logind 会话不支持 lock-session，`device/unlock.sh` 的 unlock-session 能否解开未验证。测 greeter 用 `/usr/lib/kscreenlocker_greet --testing` 再杀掉。
- **CRLF**：本机 `core.autocrlf=true`，anland 的后端源码和 `sync-build-kwin.sh` 在 Windows checkout 里是 CRLF。凡是要在 Linux / Android 上执行的先 `tr -d '\r'`（`ct.sh`、`push-tools.sh`、`kwin-stage.sh` 已处理）；skill 目录有 `.gitattributes` 强制 LF。
- **display daemon 开机不自启**：KWin 报 "failed to connect to display daemon at /run/anland-mobilegl/display.sock"，会话约 3 秒退出。
- **电量 / 连接**：电量低或熄屏（默认 30 秒）可能让 SurfaceView 被回收——`svc power stayon true`；无线调试的端口每次配对都变——脚本里不要写死 `-s HA27Q3LQ`，多设备时设 `ANDROID_SERIAL`。
- **容器与 Android 路径**：容器的 `/tmp` 和平板的 `/data/local/tmp` 不是一个地方；文件经 `/mnt/Droidspaces/arch-kde-mgl/root`（= 容器 `/root`）或 `/data/local/tmp/anland-mobilegl`（= 容器 `/run/anland-mobilegl`）中转。
- **正在映射的 .so 不要原地覆盖**（容器里或运行中的 APK）：拷成新 inode 再 `mv -f`；APK 换库前先 force-stop。
- **输入注入**：普通 `adb shell input` 被拦，要 `su -c input`；anland 抓了触摸屏，`input tap` 无效时用 `input mouse tap x y`。不要对容器进程用 `debuggerd -b`（信号 35 会直接杀掉 glibc 进程）。
- **linux-dmabuf**：client 必须 `create` + roundtrip，不能 `create_immed`（KWin 对失败的立即导入回协议错误，直接断开 client）。
- **跨会话 bug**：某个 client 起来或退出后出现噪点 / 别的窗口的残影，先怀疑多会话 server 里的进程级状态（handle 是每 client 各自的，FBO / VAO 是每 context 的）。复现用 `kscreenlocker_greet --testing`。
- `/etc/environment` 经 pam_env 带进 Mesa / KGSL 变量，`mobilegl-startup.sh` 里把它们 unset 了，改脚本时保留。
- handoff 里提到的 `kwin_wayland` 日志 shim 与 `/tmp/mgl-kwin-<pid>.log` 已过时：`/opt/mobilegl/kwin/bin/kwin_wayland` 现在就是真二进制，KWin 的 client 日志在 `/tmp/mobilegl-compositor.client.log`。

## 规矩

- 在 git worktree 里干活，不碰共用的主 checkout 和它的构建目录；worktree 要 `GIT_LFS_SKIP_SMUDGE=1`。
- 提交用 `git commit --no-verify`，一句话的短消息，**不加** `Co-Authored-By`。
- DirectVulkan 集成测试不跑（`ctest -E DirectVulkan`）。
- 源码和注释里不写外部项目名（"Mesa"、"Zink" 等），只描述手法。
- 零拷贝路径只用厂商中立的设施：AHB、EGL_ANDROID_*、VK_ANDROID 的 AHB 导入、sync_file、dma-buf、libgbm ABI；不碰 kgsl / gralloc 私有布局 / 高通修饰符。
- 身份缓存用单调递增的生命期 id 作键，不用 GL 名字或指针。
