# 为了在天玑 1100 + Mali（内核 4.14）上跑起来，改了什么、为什么

> 本文只记**改动与动机**。参考机（Lenovo Y700 / Adreno 750 / 内核 6.1 / KernelSU / 有 systemd 的
> Arch 容器）的完整操作手册见 [`runbook-plasma.md`](runbook-plasma.md)；本文的改动都在分支
> `feat/disaggregated-fix-d1100-mali` 上，两个在 MobileGL 里（本仓库），两个在 anland 的
> consumer app 里。
>
> 判据：改之前 `screencap` 是 37 KB 的纯黑；改之后是 1.4 MB 的完整 Plasma 桌面
> （壁纸 + 面板 + 时钟），client 日志的验收项从 `presented through wl_shm` 变成
> `presented through linux-dmabuf shared images`。

## 这台机器与参考机的差异（每条改动都从这里来）

| 项 | 参考机 | 本机 |
|---|---|---|
| GPU / 驱动 | Adreno 750 | **Mali**（`/dev/mali0`） |
| 内核 | 6.1 | **4.14.186**（cgroup v1） |
| 容器 | Arch + **systemd** | Fedora 44，**无 systemd**（`custom_init=ds-init.sh`） |
| 容器里的编译器 | 有 | **没有**，且无 DNS → 全部主机交叉编译 |
| `/dev/dri` | 有 `renderD*` | 只有 `card0` |

---

# MobileGL 仓库（本分支）

## 改动 1：dma-buf 身份认不出来时，用导入自己的 extent + format 认领

**文件**：`MobileGL/MG_Remote/Server/SharedImageRegistry.{h,cpp}`、`MobileGL/MG_Remote/Server/PipeApplier.cpp`

**改了什么**：`Identify(int fd, …)` 增加 `width/height/fourcc` 三个参数（导入记录本来就有它们）。
内核身份 `(st_dev, st_ino)` 仍然优先；只有当注册表已经发现身份**不唯一**时，才退化为：在该身份
下已导出的活镜像里，挑 **extent 与 fourcc 都相同、且尚未被认领** 的那个，按 `Id` 从小到大
（= 分配顺序 = 生产者交给合成器的顺序），认领一次。

**为什么必须改**：零拷贝的整条链是"server 分配 AHardwareBuffer → 导出 dma-buf fd → client 把它做成
`wl_buffer` → 合成器 `eglCreateImageKHR` 导回 → server 认出是自己那块"。认的依据只有
`(st_dev, st_ino)`。本机内核给**所有** dma-buf descriptor 发同一个身份（实测三个不同的 buffer）：

```
bo0: fd=15 dev=12 ino=10658 size=0  fdinfo[pos: 0 flags: 02000000 mnt_id: 12 ]
bo1: fd=17 dev=12 ino=10658 size=0  …
bo2: fd=19 dev=12 ino=10658 size=0  …
```

`fdinfo` 里没有 name、没有别的唯一量——用户态拿不到任何能区分 buffer 的东西。注册表检测到
"两块活镜像同一个身份"后**按设计 fail-closed**：

```
[ERROR] two live shared images have one dma-buf identity (dev 12 ino 10658); this kernel does not
        keep dma-buf inodes unique …
[WARN]  a dma-buf import was refused: dma-buf identities are not unique on this kernel
```

于是**每一个** linux-dmabuf 窗口都被拒：client 全退回 `wl_shm`（`8 presented through wl_shm`），
合成器也没法采样它上不了屏的东西。fail-closed 本身是对的（猜错会把别人的像素当自己的），
缺的只是**内核给不出身份时还有别的可用依据**——导入声明的尺寸和格式，加上 server 自己的分配顺序。

**验证**：自包含探针 `eglprobe4.c`（主机 clang+lld 编，容器里以 Gold 跑）——`gbm_bo_create`
取一块 server shared image 的 fd，再走合成器的导入路径：修前
`eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) -> (nil)`，修后 `-> 0x1`；会话里
refusal 归零、client 日志变成 `presented through linux-dmabuf shared images`。

## 改动 2：damage 变体被驱动拒绝时，回落普通 `eglSwapBuffers`

**文件**：`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp` 的 `Present()`

**改了什么**：`eglSwapBuffersWithDamageEXT` 返回 EGL_FALSE 时，用 `eglSwapBuffers` 再试一次，
并 `MGLOG_W_ONCE` 说明一次（没有该扩展的驱动照旧走普通入口）。

**为什么必须改**：server 的 `Present()` 原本优先用 WithDamage 变体（注释说"驱动提供哪个拼写就用
哪个"）。本机的 Mali/4.14 vendor EGL **暴露了这个入口，却对窗口面返回 EGL_FALSE 且不设 EGL 错误**：

```
server window: 1080x2194 fmt=0x1 (pointer=0xb4000070d902ad00)   ← ANativeWindow 是活的、尺寸对
swap entry points: withDamage=0x71a0887040 plain=0x71a087c62c
withDamage → swap=0 err=0x3000        ← 同一块 surface、同一线程
plain      → swap=1 err=0x3000
```

排除了绑定问题（`session.Draw == eglGetCurrentSurface(EGL_DRAW)`、context 也 current），也不是窗口
问题（`ANativeWindow_getWidth/Height/Format` 都正常）——就是驱动拒。信任那个指针的后果是
**一帧都不入队**：SurfaceView 的 BLAST 层 `frame events` 全 0、`geomLayerBounds` 0×0，屏幕上只有
`Background for SurfaceView` 那块纯黑占位；合成器又能采样但因为上不了屏，client 也跟着退回
`wl_shm`。这条是"屏幕全黑"的最后一环。

**验证**：修后 `frame events` 出现真实时间戳，`screencap` 从 37 KB 变 1.4 MB 的完整桌面。

---

# anland 仓库（consumer app）

这两条不在本仓库里，但缺任何一条桌面都上不了屏。

## 改动 3：`:mobilegl` worker 设置 `MOBILEGL_IPC_SURFACE=server`

**文件**：`consumers/anland_v5/android_consumer/app/src/main/java/com/anland/consumer/MobileGLWorker.java`

**改了什么**：在 `Os.setenv("MOBILEGL_IPC_ROLE", "server", …)` 旁边加
`Os.setenv("MOBILEGL_IPC_SURFACE", "server", true)`。

**为什么必须改**：`Config.h` 里 `IpcTable::Surface` 的默认值是 `IpcSurface::Offscreen`。
client 侧（容器里的 `mobilegl-startup.sh`/`start-mobilegl-session.sh`）按 `server` 去要
server 的窗口，而 **server 自己的进程没有任何 `MOBILEGL_IPC_SURFACE`**，于是它把 app 递过来的
窗口 attach 了、却**从不创建 server-owned 窗口面、也从不往上面 swap**。日志上表现为只有一句
`a ServerOwned window surface is served on its window`，没有真正的
`surface=window … owner=server`。补上之后这一行立刻出现。

## 改动 4：几何回调不再把 Surface 塌成 0×0

**文件**：`…/MainActivity.java`（`MobileGLConnection` 的几何回调）

**改了什么**：回调收到 `(0,0)`（"不指定尺寸"）时，只有在视图**确实已有尺寸**时才
`holder.setSizeFromLayout()`；否则什么都不做，保留现有 surface。

**为什么必须改**：这个回调可能在视图第一次布局之前被调用，此时 `setSizeFromLayout()` 会把
`SurfaceHolder` 定成 0×0；0×0 的 surface 上任何生产者都无法 queue，之后每一帧
`eglSwapBuffers` 都会失败——正是"BLAST 层 `geomLayerBounds` 0×0 且永远没有 buffer"的成因。

---

# 环境侧（不是代码，但缺了就起不来）

1. **`libatomic.so.1`**：主机交叉编出来的 KWin 需要它，而这个 Fedora 运行时镜像没有
   （`kwin_wayland: error while loading shared libraries: libatomic.so.1`）。从 sysroot 把
   `libatomic.so.1.2.0` + 软链补进 rootfs（容器停着时直接写
   `/data/local/Droidspaces/fedora-kde/usr/lib64/`）。`ldd` 其余 Qt6/KF6 依赖都齐。
2. **无 systemd 的会话**：容器里 systemd 259 因"内核 4.14 + cgroup v1"直接拒绝启动，会话由
   `ds-init.sh` 自己拉起。等价物是把 `mobilegl-startup.sh plasma` 的那套 env 写进
   `/opt/mobilegl/bin/start-mobilegl-session.sh`（PATH 前置 `/opt/mobilegl/kwin/bin` 让
   `kwin_wayland_wrapper` 起我们的 KWin），再让 `ds-init.sh` 的最后一句 exec 它。**同时必须去掉
   `LIBGL_ALWAYS_SOFTWARE=1`/`GALLIUM_DRIVER=llvmpipe`**：MobileGL 对"要求软件渲染的进程"主动
   让位，留着它整个会话就还是 llvmpipe。
3. **GBM 后端的路径**：Fedora 的 loader 搜 `/usr/lib64/gbm`（上游脚本写的是 Arch 的
   `/usr/lib/gbm`）；`libGLX_mobilegl.so.0` 同理放 `/usr/lib64`。
4. **`/dev/dri` 只有 `card0`**：`MOBILEGL_GBM_NODE` 退到 `/dev/null` —— 那个节点只当身份、
   后端从不对它 ioctl，这是 `mobilegl-startup.sh` 自己的回退路径，不需要改设备节点权限。
5. **KWin 的构建**：pristine `kwin-6.7.5` + `Arch_v5/kwin.patch` + `anland_backend_Arch_v5` +
   顶层 `libdisplay_producer/`（后者的 `protocol.h`/`socket_utils.c` 是指向 `common/` 的符号链接，
   **拷贝时要 -L 解引用**；也**不要**把 `common/` 整个拷进 backend 目录，会让 `kde_clang_format`
   生成重名 target）。6.7.5 上只有 `src/opengl/eglcontext.cpp` 的 Hunk #2 打不上，按补丁原意手工把
   no-arg `makeCurrent()` 里的 `EGL_NO_SURFACE, EGL_NO_SURFACE` 换成
   `m_display->defaultSurface()`。

---

# 已知但未处理

- daemon 的 producer/consumer 每 ~5 s 抖一次（`producer connected/disconnected`、
  `consumer re-deposited 5 fds`，kwin 侧 `consumer disconnected, entering fallback`）。MobileGL
  模式下合成不受影响（窗口与默认 framebuffer 属于 KWin），桌面照常上屏，成因未查。
- 桌面静止时两次 `screencap` 的 md5 相同（时钟只到分钟、无动画）；判断"活着"要看 server 的
  `P65ServerFrame` 计数或 SurfaceView 的 frame events。
- **Magma（DirectVulkan）未复验**。上面三条代码改动都与后端无关，两个后端都会撞到。
