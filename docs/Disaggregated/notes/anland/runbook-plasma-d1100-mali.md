# Runbook：D1100 + Mali、内核 4.14 的设备上起 MobileGL + anland + KDE Plasma（2026-10-03）

> 第二台设备的实测记录。参考机（Lenovo Y700 / Adreno 750 / 内核 6.1 / KernelSU）见
> [`runbook-plasma.md`](runbook-plasma.md)；本文只写**与它不同的地方**、以及那台机器上不会
> 遇到的坑。台面上的三个修复随分支 `feat/disaggregated-fix-d1100-mali` 提交：两个在 MobileGL
> 里（本仓库），两个在 anland 的 consumer app 里。

## 设备与容器（与参考机的全部差异）

| 项 | 参考机 | 本机 |
|---|---|---|
| SoC / GPU | 骁龙 8 Gen 3 / **Adreno 750** | 天玑 1100 / **Mali**（droidspaces 镜像 `/dev/mali0`） |
| 内核 | 6.1 | **4.14.186**（+ cgroup v1） |
| root | KernelSU | **Magisk**（`su -c` 可用；`/data/adb/magisk`） |
| 系统 | Android 15 | **TrebleDroid GSI, Android 14 (SDK 34)** |
| adb | USB `HA27Q3LQ` | USB `aikjs4s8cys8ba7l` |
| 容器 | `arch-kde-mgl`（Arch，**systemd**） | `fedora-kde`（Fedora 44 Container Image，**无 systemd**） |
| 容器 init | `desktop-session.service` | `custom_init=/usr/local/sbin/ds-init.sh`（`exec setpriv … dbus-run-session -- startplasma-wayland`） |
| 桌面用户 | `swung0x48` | `Gold`（uid 1000） |
| 容器里的编译器 | 有（gcc/cmake/ninja） | **没有**（运行时镜像，且容器无 DNS） |
| `/dev/dri` | 有 renderD\* | **只有 `card0`（600 root:root）** |
| 容器截图/构建 | 容器内构建 client 与 KWin | **全部在主机（PC）交叉编译**，只把产物送进容器 |

容器里 systemd 起不来的原因很直接（`/data/local/Droidspaces/Logs/fedora-kde/console`）：

```
systemd 259.9-1.fc44 … Warning! Reported kernel version 4.14.186 is older than systemd's required
baseline kernel version 5.7.
Detected cgroup v1 hierarchy at /sys/fs/cgroup/, which is no longer supported…
[!!!!!!] Detected unsupported legacy cgroup hierarchy, refusing execution.
```

所以本机**没有** `desktop-session.service`/`plasma-kwin_wayland.service` 这条链路，
`mobilegl-startup.sh plasma` 里那几行 `systemctl --user` 也没有意义。等价物见下文"会话"。

## 一次性准备

1. **容器 bind**：`container.config` 的 `bind_mounts` 要有两条——
   `/data/local/tmp/display_daemon.sock:/run/display.sock` 与
   `/data/local/tmp/anland-mobilegl:/run/anland-mobilegl`（后者镜像里默认没有，缺了客户端就看不到
   daemon socket）。`net_mode=host` 必须有（`@anland-mobilegl` 是抽象 socket）。
   Android 侧 `mkdir -p /data/local/tmp/anland-mobilegl && chmod 777`。
2. **client 库**（主机交叉编译）：容器运行时是 glibc 2.43 / GLIBCXX 3.4.36，和 Fedora 44 aarch64
   sysroot 一致，所以用 `scripts/xsysroot/aarch64-arch.cmake` 的等价物（clang + lld +
   `--gcc-install-dir=<sysroot>/usr/lib/gcc/aarch64-redhat-linux/16`）编
   `libMobileGL.so` + `mobilegl_gbm.so`，参数与 `xbuild.sh` 相同。**注意 64 位发行版的路径**：
   `/usr/lib64/gbm/mobilegl_gbm.so`、`/usr/lib64/libGLX_mobilegl.so.0`（`xdeploy.sh` 写的是 Arch 的
   `/usr/lib`，两条都放最省事）。
3. **KWin**：pristine `kwin-6.7.5`（`download.kde.org/stable/plasma/6.7.5/kwin-6.7.5.tar.xz`）
   + `producers/kde/Arch_v5/kwin.patch` + `anland_backend_Arch_v5` + 顶层 `libdisplay_producer/`
   （后者里的 `protocol.h`/`socket_utils.c` 是指向 `common/` 的符号链接，**拷的时候要 -L 解引用**，
   否则 configure 报 "Cannot find source file … socket_utils.c"；也**不要**把 `common/` 整个拷进
   backend 目录，那会让 `kde_clang_format` 生成重名的 clang-format target 而 configure 失败）。
   7.7.5 的树上 `kwin.patch` 只有 `src/opengl/eglcontext.cpp` 的 Hunk #2 打不上（6.7.5 里
   `makeCurrent()` 的上下文不同），按补丁原意手工改：no-arg `makeCurrent()` 里把
   `EGL_NO_SURFACE, EGL_NO_SURFACE` 换成 `m_display->defaultSurface()`。
   装到 `/opt/mobilegl/kwin/{bin,lib}`，screencast 插件就地替换
   `/usr/lib64/qt6/plugins/kwin/plugins/screencast.so`（留 `.orig`）。
4. **容器缺的运行时库**：交叉编出来的 KWin 需要 `libatomic.so.1`，这个运行时镜像没有。
   从 sysroot 把 `libatomic.so.1.2.0` + 软链补进 rootfs（容器停着时直接写
   `/data/local/Droidspaces/fedora-kde/usr/lib64/` 最方便）。`ldd` 其余依赖 Qt6/KF6 都齐。

## 会话（无 systemd 的等价物）

镜像原来的 `ds-init.sh` 用 **llvmpipe 软件渲染**起 Plasma，且 export 了
`LIBGL_ALWAYS_SOFTWARE=1` —— MobileGL 对"要求软件渲染的进程"主动让位，所以那一行必须去掉。
把这个脚本装到容器 `/opt/mobilegl/bin/start-mobilegl-session.sh`：

```sh
# 复刻 mobilegl-startup.sh plasma 的 env（KWin 用 server，其余 client 用 offscreen），
# 然后 exec startplasma-wayland —— PATH 前置 /opt/mobilegl/kwin/bin，
# kwin_wayland_wrapper 就会 exec 我们的 KWin。
export KWIN_BIN_DIR=/opt/mobilegl/kwin/bin KWIN_LIB_DIR=/opt/mobilegl/kwin/lib
export __EGL_VENDOR_LIBRARY_FILENAMES=/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json
export __GLX_VENDOR_LIBRARY_NAME=mobilegl GBM_BACKEND=mobilegl ANLAND_MOBILEGL=1
export ANLAND_SOCKET=/run/anland-mobilegl/display.sock
export MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_DATA=shm MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl
export MOBILEGL_IPC_SURFACE=server MOBILEGL_BACKEND_TYPE=$(cat /etc/mobilegl/backend)
export MOBILEGL_GBM_NODE=/dev/null MOBILEGL_DEVICE_DRM_NODE=/dev/null
export KWIN_DISABLE_VULKAN=1 KWIN_NO_TIMER_QUERY=1 KWIN_PERSISTENT_VBO=0 KWIN_DISABLE_UDMABUF_IMPORT=1
export PATH="$KWIN_BIN_DIR:$PATH" LD_LIBRARY_PATH="$KWIN_LIB_DIR"
unset LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE
exec /usr/bin/startplasma-wayland
```

`/dev/dri` 里没有 `renderD*` 时 `MOBILEGL_GBM_NODE` 取 `/dev/null`：那个节点只是身份，
后端从不对它 ioctl（`mobilegl-startup.sh` 自己也是这么退的），不需要改设备节点权限。

`ds-init.sh` 最后的 `exec … startplasma-wayland` 换成 `dbus-run-session -- /opt/mobilegl/bin/start-mobilegl-session.sh`，
并去掉 `LIBGL_ALWAYS_SOFTWARE`/`GALLIUM_DRIVER`。**会话在容器启动时就起**，所以 bring-up 顺序是
**app → display daemon → 容器重启**（不是 runbook 里那条 systemd 顺序）。

## 本机独有的三个坑（都已修，随分支提交）

### 1. 内核不保证 dma-buf inode 唯一 → 零拷贝被整体拒绝

参考机靠 `(st_dev, st_ino)` 认"这块 dma-buf 是不是 server 自己发的"。本机**所有** dma-buf
descriptor 报同一个身份：

```
bo0: fd=15 dev=12 ino=10658 size=0  fdinfo[pos: 0 flags: 02000000 mnt_id: 12 ]
bo1: fd=17 dev=12 ino=10658 size=0  …
bo2: fd=19 dev=12 ino=10658 size=0  …
```

`/proc/self/fdinfo` 里也没有 name / 任何唯一量，也就是用户态**拿不到**能区分 buffer 的东西。
`SharedImageRegistry` 正确地检测到冲突并按设计 fail-closed：

```
[ERROR] two live shared images have one dma-buf identity (dev 12 ino 10658); this kernel does not
        keep dma-buf inodes unique …
[WARN]  a dma-buf import was refused: dma-buf identities are not unique on this kernel
```

后果是**所有** linux-dmabuf 窗口都被拒，client 全部退回 `wl_shm`（`8 presented through wl_shm`），
合成器也就没法采样它上不了屏的东西。

**修复**（`feat/disaggregated-fix-d1100-mali` 的第一个提交）：身份仍然优先由内核给；不唯一时按
**导入记录自己带的 extent + fourcc**，在该身份下已导出的活镜像里取"最早且未被认领"的那个
（分配顺序 = 生产者交给合成器的顺序）。`Identify()` 多收 `width/height/fourcc` 三个参数，
`PipeApplier` 的导入分支本来就有它们。

修好后 client 日志变成验收项要的那句，且 refusal 归零：

```
8 presented through linux-dmabuf shared images
```

自包含复现（主机用 clang+lld 编，容器里以 Gold 跑）：`eglprobe4.c`——`gbm_bo_create` 拿一块
server shared image 的 dma-buf fd，再走 KWin 的导入路径 `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)`。
修前 `-> (nil)`，修后 `-> 0x1`。

### 2. 驱动暴露 `eglSwapBuffersWithDamageEXT` 却对它返回 EGL_FALSE

这条是"屏幕全黑"的最后一环。server 的 `Present()` 在两个入口里优先用 WithDamage 变体；
本机驱动（Mali / 4.14 vendor EGL）对**窗口面**返回 EGL_FALSE 且**不设 EGL 错误**：

```
server window: 1080x2194 fmt=0x1 (pointer=0xb4000070d902ad00)   ← ANativeWindow 是活的
swap entry points: withDamage=0x71a0887040 plain=0x71a087c62c
withDamage → swap=0 err=0x3000 ; plain → swap=1 err=0x3000       ← 同一块 surface、同一个线程
```

surface 在当前线程上是 current、context 也是 current、窗口尺寸格式都对，所以不是绑定问题——
就是驱动拒绝。信任那个指针等于**一帧都不入队**：SurfaceView 的 BLAST 层 `frame events` 全 0、
`geomLayerBounds` 0×0，屏幕上只有 `Background for SurfaceView` 那块纯黑占位。

**修复**（第二个提交）：damage 变体返回 EGL_FALSE 时回落普通 `eglSwapBuffers`，并
`MGLOG_W_ONCE` 说明一次。修好后 `frame events` 出现真实时间戳，`screencap` 从 37 KB（纯黑）
变成 1.4 MB 的完整桌面。

### 3. app 侧两处（在 anland 仓库，不在本仓库）

- **`:mobilegl` worker 从没设 `MOBILEGL_IPC_SURFACE=server`**。`IpcTable::Surface` 的默认是
  `Offscreen`，而 client 侧（会话脚本）按 `server` 去要 server 的窗口，于是 server **把窗口
  attach 了却从不创建 server-owned 窗口面**——日志里只有
  `a ServerOwned window surface is served on its window`，没有真正的
  `surface=window … owner=server`。在 `MobileGLWorker.java` 的 `MOBILEGL_IPC_ROLE` 旁边加一行
  `Os.setenv("MOBILEGL_IPC_SURFACE", "server", true)` 即可。
- **`MobileGLConnection` 的几何回调**在收到 (0,0) 时会 `setSizeFromLayout()`；若此时视图还没
  第一次布局，surface 会被定成 **0×0**，之后任何生产者都无法 queue。改成"视图确实有尺寸时才
  回到布局"。

## 验收（本机实测结论）

- 会话：`kwin_wayland`（`/proc/<pid>/exe -> /opt/mobilegl/kwin/bin/kwin_wayland`）+ `plasmashell`
  常驻；client 日志 `Renderer Name: Espryt`、`CapsMirror generation N adopted`、
  `P65ClientPace present=N`；server 日志 `P65ServerFrame frame=N`（空闲桌面约 1 帧/分钟，
  只在有 damage 时合成）。
- 上屏：`screencap` 得到 1080×2400 的完整 Plasma 桌面（壁纸 + 面板 + 时钟），不再是纯黑。
  SurfaceView 的 BLAST 层有真实 `frame events`。
- 验收项 `presented through linux-dmabuf shared images` **达成**（8/8，0 refusal）。

## 仍未解决 / 未验证

- **daemon 的 producer/consumer 每 ~5 s 抖一次**（`daemon: producer connected/disconnected`、
  `consumer re-deposited 5 fds`，kwin 侧 `kwin_backend_anland: consumer disconnected, entering
  fallback`）。在 MobileGL 模式下合成不受影响（窗口和默认 framebuffer 属于 KWin），所以桌面
  照常上屏，但这条抖动本身的成因没查。
- 桌面静止时两次截图 md5 相同（时钟只到分钟、无动画），要证明"活着"看 server 的 frame 计数
  或 `testImportBuffer`；参考机的 `shot.sh` 两帧哈希法在这台机器上需要先制造 damage。
- **Magma（DirectVulkan）未复验**：`setprop debug.mobilegl.backend DirectVulkan` +
  `/etc/mobilegl/backend=DirectVulkan` 后重启 app 与容器即可。上面三个坑与后端无关，
  两个后端都会撞到（都已在 DirectGLES 上验证过修复路径）。
- 容器无 DNS、无编译器，所以"容器内构建"这条上游路线在本机不适用；本机全部交叉编译。
