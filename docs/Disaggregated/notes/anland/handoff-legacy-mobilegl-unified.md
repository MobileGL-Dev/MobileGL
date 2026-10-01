# Handoff：anland 5.x + 内置 MobileGL SHM server

更新时间：2026-10-01（用户时区 America/Cuiaba）。用户要求暂停实现，先写交接文档，由新的 agent 继续。

## 先读：用户最新纠正与当前完成度

**任务没有完成。KWin、Konsole、eglinfo、glxinfo、Plasma 均未通过 MobileGL 真机验收。构建成功不能当成验收成功。**

用户原始目标：

1. 在 anland 新建 `legacy-mobilegl-unified`，让 anland 5.x 使用 MobileGL。
2. SHM split server **必须是 anland APK 的一部分，由 anland 管理**，client 在 Droidspaces 容器内。
3. 在联想 Y700 TB321FU 上复制 `arch-kde` 为 `arch-kde-mgl`，作为实验环境。
4. 依次调通 KWin → Konsole → eglinfo → glxinfo → Plasma。
5. 阶段成果形成可二分的 commits，推送 anland fork 和 MobileGL `feat/disaggregated`。
6. 开始前阅读此目录调研，尤其 [`kgsl-as-server.md`](kgsl-as-server.md)，技术路线接近 B：anland 自己的 Surface 交给 MobileGL server 直接渲染。

用户已明确排除两种路线：

> anland fork 的 `legacy-mobilegl` 路径是配合 MobileGL feat/disaggregated-container-gbm 的错误路径。MobileGL server 不应该活在独立的 plugin app 里，应该成为 anland 的一部分。

> 说起来，为什么会有实现一个应用离屏 Service？如果 MobileGL 有全局状态需要解决，那应该处理的是 MobileGL 自己的全局状态，而不是想办法制造多个 MobileGL peer。

第二条纠正针对本轮草稿：曾添加 `MobileGLAppsService`，exec 一个离屏 supervisor，给每个应用 fork 一个独立 worker，以隔离 MobileGL 全局状态。**用户否定了这个绕法；不要把它继续打包、部署或宣称为最终设计。**

应在 MobileGL 内解决 session/context/surface 状态归属与共享资源问题。anland 内置的 renderer 可以由私有 Service 管理，但不能用额外“应用离屏 Service”或按应用 fork 渲染进程来代替这个工作。多应用的正常 client 连接与人为制造 worker 来绕过全局状态，要区分清楚。具体统一 server 的设计尚未完成。

已发给各子 agent 的最新指令是暂停实现与部署，仅报告状态。**纠正后的架构还没有实现；草稿仍包含被否定代码。** 本文的状态说明优先于草稿中的旧启动说明。

## 工作区、分支、保留事项

### MobileGL

```text
C:\Users\Swung0x48\repos\FoldCraftLauncher\MobileGL
origin: https://github.com/MobileGL-Dev/MobileGL.git
显示分支: feat/disaggregated
当前实现 HEAD: 2f64ede8c65e9577676e658e82017672ad58f03e
```

Windows 上真实本地 ref 的大小写是 `refs/heads/Feat/disaggregated`。推送使用：

```powershell
git push origin HEAD:refs/heads/feat/disaggregated
```

不要用失败的 `git push origin feat/disaggregated` 重复排查网络。

用户原有 dirty submodule `3rdparty/glslang` 必须保留，不能提交、重置或当成本任务成果。为 split 构建初始化过缺失的 `3rdparty/flatbuffers`，没有修改 gitlink。

本任务未提交改动仅有：

| 文件 | 改动与状态 |
|---|---|
| `CMakeLists.txt` | Linux 增加 `libGL.so`、`libGL.so.1` 别名 |
| `MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp` | Wayland `Present()` 失败返回 `EGL_BAD_SURFACE`；原代码忽略失败 |
| `MobileGL/MG_Impl/GLXImpl/GLXImpl.cpp` | spawn 模式 X drawable 使用远端 pbuffer，避免把 XID 当 Android ANativeWindow；**没有 X11 像素上屏、没有远端 resize** |

DirectGLES/BackendObject 的“保留一个 native context、切换多个 surface”实现曾被讨论，但**没有产生代码改动**，也没有产生测试成果。不要误以为它已经实现。

### anland

原工作区：

```text
C:\Users\Swung0x48\repos\anland
origin: https://github.com/MobileGL-Dev/anland.git
原分支: main
```

原工作区用户 dirty submodules `pulseaudio`、`wayland`、`xserver` 没有修改。继续保留。

本任务使用独立 worktree，**后续 anland 编辑必须在这里进行**：

```text
C:\Users\Swung0x48\repos\anland-legacy-mobilegl-unified
branch: legacy-mobilegl-unified
base: origin/legacy = 9ab13eb146bc9f3dd3cc44cabc268659d9eb0a46
当前本地/远端 HEAD: 6e1fa653314956f57948b93abff08724c97c779c
```

不要重新 checkout 远端分支来替代工作区；大量实现仍在未提交文件中。

## 已推送 commits 与适用性

### MobileGL `feat/disaggregated`

| Commit | 内容 | 适用性 |
|---|---|---|
| `57781688` | GLVND EGL vendor ABI headers，来自 `6a702afa` | 可复用的独立 client 能力 |
| `75f83b20` | EGL extension strings，来自 `dfbda7cc` | 可复用 |
| `0dad3fca` | EGL GLVND vendor，来自 `c9206dd9` | 可复用 |
| `f4589243` | multiple display/vendor lookup，来自 `2860d090` | 可复用 |
| `32c79f41` | Wayland wl_shm WSI，来自 `7fd7f71d` | 初步 client 能力；有下述限制 |
| `a5b463a5` | package-neutral embedded display C ABI | 14 项 ownership/lifecycle 测试通过，可复用 |
| `2f64ede8` | Unix `--serve --max-sessions 1..64`，fork worker 并发 | **已推送，但用户随后否定用它解决 anland 全局状态的方案。最终架构需移除/替换这个依赖，不能把它当任务完成。** |

这些独立 client 改动来自旧分支，但没有 cherry-pick 独立 plugin/GBM/host-frame 整体架构。`32c79f41` 冲突处理中移除了尚未实现的 device/GBM 扩展广告。

### anland `legacy-mobilegl-unified`

`6e1fa65 Add a no-fd MobileGL Surface mode to the display protocol` 已推送。

它添加 `DATA_MSG_MOBILEGL_SURFACE=201`、`ANLAND_FORMAT_MOBILEGL_SURFACE=0x4d474c53`。metadata 没有 fd，保留一 pacing slot（fd=-1、idx=0）。真实 daemon + producer + consumer 测试验证 metadata、frame ack、输入事件。没有假造 dma-buf。

其他 APK、JNI、KWin、launcher 草稿都还没有提交。

## 未提交 anland 草稿：保留有用部分，移除被否定部分

### Android APK/JNI

改动文件：

```text
consumers/anland_v5/android_consumer/app/build.gradle
consumers/anland_v5/android_consumer/app/src/main/AndroidManifest.xml
consumers/anland_v5/android_consumer/app/src/main/java/com/anland/consumer/MainActivity.java
consumers/anland_v5/android_consumer/app/src/main/java/com/anland/consumer/Native.java
consumers/anland_v5/android_consumer/app/src/main/jni/CMakeLists.txt
consumers/anland_v5/android_consumer/app/src/main/jni/native_consumer.c
```

未跟踪文件：

```text
consumers/anland_v5/android_consumer/MOBILEGL.md
consumers/anland_v5/android_consumer/app/src/main/java/com/anland/consumer/MobileGLWorker.java
consumers/anland_v5/android_consumer/app/src/main/java/com/anland/consumer/MobileGLConnection.java
consumers/anland_v5/android_consumer/app/src/main/java/com/anland/consumer/MobileGLAppsService.java
consumers/anland_v5/android_consumer/app/src/main/jni/mobilegl_worker.c
```

有用设计：

- `MobileGLWorker` 是同 APK、同 UID 的私有 `:mobilegl` Service（不是 isolatedProcess），接收 anland 自己的 Surface，JNI 通过 `dlopen/dlsym` 使用中立 ABI。
- 主 UI 进程不加载 MobileGL 的 GL 导出；避免影响 Android framework/UI 的 EGL。
- `MainActivity` 等待 worker attach 后才启动 native display consumer。
- MobileGL 模式 native consumer **不连接 ANW_API_CPU、不 dequeue/queue Surface buffer**，EGL 是唯一生产者；输入、音频、显示协议继续保留。
- 新 Surface parcel 每次会产生不同 native wrapper。最新草稿保留同一 window owner/generation 的 Java Surface，尺寸更新使用保留的 ANativeWindow，避免 resume/resize 把活动 session detach。
- native geometry callback → one-way Binder → UI SurfaceHolder；`surfaceChanged` 先报告尺寸，再等待 native transport 停止，避免 geometry/ack 互等。
- `Surface.getGenerationId()` 是隐藏 API，曾导致编译失败；改为 NDK native identity token + 本地生命周期 generation。
- CMake 使用仓库根下的 authoritative common/libdisplay 源码，避免 Windows 把 symlink checkout 成 target-text 文件。
- `mobileglDist` staging 需在切回 unbundled 时清除旧 libs；**Gradle Sync 没有 sources 时会 NO-SOURCE，不自动删除旧文件**，草稿已改为显式 cleanup。

必须撤掉/重设计：`MobileGLAppsService`、manifest/binding/health check、第二个应用 endpoint、`libMobileGLServer.so --serve --max-sessions 16` 依赖以及相关文档。

当前 `MobileGLConnection`、APK 文档仍包含这条被否定路径。不要只读注释就继续部署。

### KWin 6.7.4 后端

**anland 5.x 是宿主协议版本，容器当前 KWin 实际为 6.7.4。**

草稿：

```text
producers/kde/Arch_v5/kwin.patch
producers/kde/anland_backend_Arch_v5/src/backends/anland/anland_backend.cpp
producers/kde/anland_backend_Arch_v5/src/backends/anland/anland_backend.h
producers/kde/anland_backend_Arch_v5/src/backends/anland/anland_egl_backend.cpp
producers/kde/anland_backend_Arch_v5/src/backends/anland/anland_egl_backend.h
producers/kde/Arch_v5/mobilegl-startup.sh        # untracked
producers/kde/Arch_v5/mobilegl.md                # untracked
```

`CMakeLists.txt` 也可能显示 dirty（曾尝试 PUBLIC include，已撤销语义改动；检查实际 diff/行尾）。

`ANLAND_MOBILEGL=1` 路径：跳过 GPU/DRM probing，不广告 dma-buf/syncobj，不要求 GBM/no_config/surfaceless。先选择 RGBA window config，创建 server-owned EGL window，再创建 RenderDevice/shared contexts。默认 makeCurrent 使用该 Surface。output layer marker 走 default FBO 0、full damage 和 eglSwapBuffers，不导入 consumer dma-buf。Mesa 路径保留。

full repaint 是因为 Android BufferQueue 自己轮换 buffers，而 client 暂未提供 buffer age。RenderTarget transform 保持原 Anland FlipY/output transform 语义。

native 构建已通过。最后修了 public backend headers 的 include 路径：`main_wayland.cpp` 包含它们时也必须找到 producer/protocol header，不能靠 backend PRIVATE include dirs。两个 header 已改为自足的相对 include。

launcher 有 `compositor`/`plasma` modes，原先按两个 endpoints 分配环境，**这部分要随新统一 server 设计修改**。提交新 shell helper 时须设置 git executable bit `100755`。

## MobileGL 状态问题：已知事实与未完成设计

不能用“每进程隔离”掩盖同一应用的多 context/share-group 缺陷：

- `MG_State::pGLContext` 是进程全局，bindings/errors/object registries 被不同 EGLContext 共用。
- EGL `shareCtx` 当前只是 metadata，不能据此声称真正支持 shared contexts。
- DirectGLES `g_Display/g_Context/g_Surface` 和 Managers/cache/twin 状态是全局；不能只把三个 handle 改 TLS 就声称完成隔离。
- 多个虚拟 EGLContext 在**同一个已创建 Surface**上切换，不会直接销毁 native context；ServerLoop 检测 context token 变化后重新绑定同一 native triple、失效 caches。因此简单串行 KWin 单 Surface 有可能工作，但没有真机证据。
- 创建第二个 pbuffer/window 会 `DestroyEGLContext()`，`InitDisplayAndContext()` 又销毁 context/terminate，原资源丢失。Qt 的 offscreen probe + 实际窗口、多窗口会碰到这个问题。
- 只给 native `eglCreateContext` 添加 share 参数，不会自动解决 frontend object registries、context-local bindings 或 wire 上的 context/share-group 归属。
- `ServerDisplayInstance` 当前只有一个 display/window lease；现有 inprocess server 接一个活动 session；多 client 真正共存需要重新审视其状态与路由。
- ServerLoop 当前把 server-owned window 与 pbuffer session mode 闩定，混用会返回 `SurfaceModeMismatch`。新架构要在明确的 session/context/surface 所有权下处理 compositor 与应用。

后续应先审计 `MG_State`、`MG_Backend`、`MG_Pipe`、DirectGLES Managers、Init/Destroy、ServerSession/ServerLoop 的全局状态。确定 per-session/per-context 的 state owner、GL share group、原生 EGLContext/Surface 生命周期、apply-thread 绑定，再实施和测试。这个设计审计只开始了，没有完成代码。

以下是 runtime agent 补充的源码定位（行号针对当前实现 HEAD；以后可能移动）：

| 定位 | 问题 |
|---|---|
| `MG_State/GLState/Core.cpp:1565` | 全局 `pGLContext`，EGL CreateContext 不创建独立 GLContext |
| `MG_State/EGLState/Core.cpp:1496` | 全局 `pEGLContext`；已有 threadCurrents/contextOwners/互斥，但 ContextObject 只是元数据，没有 GL 状态/share group |
| `MG_Backend/BackendObjects.h:16` | 全局 `pActiveBackendObject`，Init 的 `gBackendFunctionsTable` 也全局 |
| `MG_Remote/Server/ServerSession.cpp`、`ServerLoop.h:129` | singleton session/loop、全局 active/sessionOwner、唯一 g_applyThreadKey |
| `MG_Pipe/MGPipe.h:264`、`MGPipeCallbacks.h:77,97`、`MGPipeHostSpan.h` | screen/context、callbacks、event capacity hook、segment resolver 全局 |
| `MG_Pipe/PipeApply.cpp:484,490` | 全局 g_applier 与 g_resourceOps，会覆盖不同 session 的表、绑定与后端 ops |
| `DirectGLES.cpp:16183,16568` | 唯一 native tuple，无条件 context teardown |
| `DirectGLES/Managers.h`、`Managers.cpp` | resource/twin registries、memo、texture units、context generations 混在全局 |

建议的真正状态边界：

- **Session**：SHM rings/segments、staged stores、reply/event 路由、failure、资源 handle空间、backend/Pipe runtime；关闭一个 session 不得破坏另一个。
- **Context**：独立 GLContext、bindings/render state/errors、VAO/FBO/query/pipeline/transform-feedback 等局部状态、native EGLContext、cache/scratch/owner/generation。
- **Share group**：buffer/texture/renderbuffer/sampler/shader/program/sync 的共享对象 registry 与名称分配、native object映射。不能直接共享整个 BufferState/TextureState，因为它们还包含局部 bindings。
- **Thread current**：TLS 指向 context/session，**owner 本身不应简单放 TLS**；context 移交线程仍应保留状态。server apply显式设置 runtime scope。

当前 wire SurfaceOp 没有真正 Create/DestroyContext、share-group语义。多client线程的GL records可能交错，提交记录或不可拆分批次必须有准确 context身份，不能只靠最后一个 MakeCurrent决定归属。统一 embedded server可以顺序schedule多个session/context，但仍要创建正确的native contexts并遵守share-group，不等于把所有contexts别名到一个native context。

需要的真实验证：相同handle值的两个session隔离与独立关闭；shared contexts访问同一buffer/texture；nonshared contexts同名资源隔离；每context的viewport/FBO/bindings/error独立；跨线程移交；pbuffer→window→pbuffer保留资源。

### WSI 现有限制

- Wayland：server pbuffer → readback RGBA → flip/swizzle → 三槽 wl_shm buffer → wl_surface；有独立 release event queue。
- 未跟随 `wl_egl_window` resize；也没有隐藏窗口/pacing 完整验收。
- 当前 GLES frontend 的版本/precision 等能力尚不能当完整 GLES 支持；`GL_VERSION` 仍 desktop 4.6，precisionformat 有 stub。KWin 优先 desktop GL，但 Plasma QtQuick 必须实际验证。
- GLX 草稿仅让 glxinfo 的 X drawable 可以用远端 pbuffer查询；**没有 X11 readback/present，所以不能声称通用 GLX 窗口上屏**。
- Surface loss 当前会结束活动 session；无损挂起/重连尚未实现。

## 设备与容器：只操作 HA27Q3LQ

```text
ADB serial: HA27Q3LQ
型号: Lenovo Y700 TB321FU
root: 可用，su context u:r:su:s0
SELinux: Enforcing
Droidspaces: v6.4.5
CLI: /data/local/Droidspaces/bin/droidspaces
clone: /data/local/Droidspaces/Containers/arch-kde-mgl
mount root: /mnt/Droidspaces/arch-kde-mgl
desktop user: swung0x48，uid1000
clone UUID: bc34a4c0a69e424a8890836ed61e4a1d
clone init PID: 27123（继续前重新确认）
boot_id: 3f4027fc-50de-400a-8751-cc925d61e99d
```

有另外三台 ADB 设备，所有命令必须带 `-s HA27Q3LQ`。设备多次短暂 offline，`adb -s HA27Q3LQ reconnect` 后恢复；uptime 与 boot_id 证实没有重启。不重启全部 adb server、不操作其他设备。

原 `arch-kde` 保持停止、配置未改。clone 从其 128GiB sparse ext4 image 拷贝约 7.7GB allocated extents；因为 Toybox/Busybox 不支持 `conv=sparse`，用 NDK SEEK_DATA/HOLE helper。不要重复全量复制。

clone Mesa 基线曾实测：freedreno FD750，OpenGL 4.6 Mesa 26.3.0，1280×800、scale2、120Hz。这是 **Mesa 基线，不是 MobileGL 成果**。

仅 clone 安装过构建依赖 GCC16.1.1/cmake/ninja/base-devel；没有 clang。clone 的 patched Xwayland `24.1.13-1.1` 为安装依赖降到仓库 `-1`，原容器未动。实验 backend 对 Xwayland 使用 `-shm` 并清除其 MobileGL loader 环境。

### 当前停止状态与目录 bind

clone 仍运行，`desktop-session.service`、`mobilegl-kwin-build.service` 均 inactive/dead（构建 ExecMainStatus=0），没有实验 KWin/Plasma/Konsole/MobileGL server 运行。

已在 clone config 追加：

```text
bind_mounts=/data/local/tmp/display_daemon.sock:/run/display.sock,/data/local/tmp/anland-mobilegl:/run/anland-mobilegl
```

配置备份：`/data/local/Droidspaces/Containers/arch-kde-mgl/container.config.mobilegl-before`。

**追加的目录 bind 尚未生效：clone 没有重启。** 使用目录 bind 是为了 daemon unlink/recreate socket 时不留下 file-bind 的旧 inode。

clone service override 已准备、尚未启动：

```text
/etc/systemd/system/desktop-session.service.d/mobilegl.conf
[Service]
ExecStart=
ExecStart=/opt/mobilegl/bin/mobilegl-startup.sh compositor
Environment=ANLAND_SOCKET=/run/anland-mobilegl/display.sock
```

原 service 的 `User=swung0x48`、`PAMName=login` 保留。单纯 `droidspaces --user=... run` 不会建立正常 logind/user D-Bus session，Plasma mode 要借此 service 或等效正常登录。

实验 display daemon 已启动（不是 MobileGL renderer，只是原有协议 broker）：

```text
host socket: /data/local/tmp/anland-mobilegl/display.sock
binary: /data/adb/modules/anland-daemon/display_daemon
log: /data/local/tmp/anland-mobilegl/display-daemon.log
实验 PID: 13993（继续前确认）
原 daemon PID: 2315，socket /data/local/tmp/display_daemon.sock（未停止）
```

实验 APK 启动时 `--es socket_path /data/local/tmp/anland-mobilegl/display.sock`；KWin 使用容器 `/run/anland-mobilegl/display.sock`，在目录 bind 生效后才成立。

### Android 安装与尚未解决的权限

原有 `com.anland.consumer`（uid10253）、`com.anlandnext`、`com.droidspaces.app` 未覆盖。

已并排安装实验 APK `com.anland.consumer.mobilegl`（uid10269）。安装的是**早期双 Service 草稿**，不是最新 source 的生命周期修正版。它启动后因没有 KernelSU root grant，socket root helper 探测失败，转到 Settings；PID12420。**没有 MobileGL renderer 启动、没有 GPU 连接测试。**

KernelSU `ksud profile` CLI 仅支持 sepolicy/template，没有直接 root allow grant。已查 manager 对应源码，GET/SET_APP_PROFILE ioctl 是 manager UID 专用，root 本身也不能直接调用。授予实验包权限仍未解决；没有编辑 opaque allowlist，没有改变 manager 注册，没有完成 grant。

对应 upstream SHA `4d396fda53e510d75f2608a184df56ccd33aac9c`。临时源在 `C:\Users\Swung0x48\AppData\Local\Temp\anland-mobilegl-device`，包括 `manager-ksu.h`、`manager-ksu.cc`、`kernel-supercalls.c`、`kernel-manager.h`、`kernel-allowlist.c`、`ksucalls.rs`、`app_profile.h`。普通 `ksu.h` 被 kernel版本覆盖，用户态ABI应读 `manager-ksu.h`。ioctl11/12分别 GET/SET_APP_PROFILE，only_manager 检查 current UID。尚未编译/执行profile mutation helper。

Android 全局 Enforcing，但已安装 Droidspaces module 的 `sepolicy.rule` 包含 `permissive droidspacesd`。这不是本任务设置的。该 module 已有双向 fd use 与部分 unix_stream read/write 规则。容器 → APK abstract socket、SCM_RIGHTS/memfd 的实际 AVC 尚未采集。不要先全局 setenforce0；待统一 server 上线后精确实测。

## 构建产物、日志、复用方法

### WSL

发行版名称 **`archlinux`**，16CPU、约14GiB RAM。

```powershell
wsl -d archlinux -- bash -lc '...'
```

Linux NDK `/root/builds/android-ndk-r27c`（27.2.12479018），Linux Android SDK `/root/anland-mgl-sdk`，JDK17 task-specific 使用（默认 JDK25 未改）。

Windows Gradle 在 Zulu21 和 Android Studio jbr 都因 `PipeImpl/WEPollSelector` AF_UNIX “Invalid argument” 在 daemon 前失败，属于本机 Java 环境。**已用 WSL JDK17 成功构建，不要再次浪费时间试 Windows flags。**

WSL 直接联网曾超时，Windows 下载可用。task-local CONNECT relay：

```text
脚本: C:\Users\Swung0x48\repos\anland-legacy-mobilegl-unified\build\windows_build_proxy.py
Windows listener: 127.0.0.1:18081
Python PID at snapshot: 29096
exec session originally: 75964
```

Gradle/sdkmanager 配置 HTTP/HTTPS proxy `127.0.0.1:18081`，没有改变主机全局设置。复用前查进程；任务结束可停止这个明确的 helper。

### Android MobileGL

```text
build: /root/anland-mgl-build/android
configure log: /root/anland-mgl-configure.log
build log: /root/anland-mgl-android-build.log
arm64-v8a / android-26 / c++_static / Release / split ON
targets: MobileGL MobileGLServer
```

493 actions 构建成功。stripped `libMobileGL.so` 约17.6MB（unstripped约329MB）。8 个 embedded ABI exports 经 dynamic nm 验证。

anland 打包 dist：

```text
C:\Users\Swung0x48\repos\FoldCraftLauncher\MobileGL\build\anland-dist
/mnt/c/Users/Swung0x48/repos/FoldCraftLauncher/MobileGL/build/anland-dist
```

当前 dist 含 libMobileGL.so、libMobileGLServer.so、EmbeddedServer.h；它是旧架构构建，**后续需重新构建统一 runtime**，不要盲用旧二进制。

最新读取 Android generated/MGGitHash.h：stamp `32c79f41350186274b6d545083656d564c0ea80f`；native client stamp `a5b463a5`。build 时曾包含未提交源码，stamp 不能证明实际源状态。最终两端使用相同确认 commit 显式 `-DMOBILEGL_BUILD_STAMP=<commit>` 重新构建，并验证 wireFingerprint，不要忽略 build drift。

一次 WSL shell quote 失误曾留下 `/libMobileGL.so`、`/libMobileGLServer.so`、`/EmbeddedServer.h` 任务残留，正确定向产物已复制到 dist。若清理只清这些明确文件，不广泛清理 `/`。

### Android anland APK

```text
C:\Users\Swung0x48\repos\anland-legacy-mobilegl-unified\consumers\anland_v5\android_consumer
artifact: app\build\outputs\apk\plain\debug\app-plain-debug.apk
```

bundled APK 已构建成功，ZIP 验证包含 MobileGL 与 Anland JNI（APK压缩约7.38MB）。最新 session96573 已结束：BUILD SUCCESSFUL，28秒，42tasks。较新的 build artifact 包含生命周期修正，但**没有装到设备**；仍含被否定 AppsService。Android source没有比该APK更新的改动，AppsService removal补丁没有生效。

最新 APK：2026-10-01 13:07:18，7,381,976 bytes，SHA256 `CD15C203226AFB95D1464DDB0B52DD232FFFA22392794E3C086E8F09EB4D7772`。JVM测试11suites、98tests、0failure/error/skipped；这是现有逻辑测试，不能验证统一Service或GPU。

已有可复用构建脚本及日志：

```powershell
wsl.exe -d archlinux -- bash /mnt/c/Users/Swung0x48/repos/anland-legacy-mobilegl-unified/build/build_bundled_apk.sh
```

脚本固定JDK17/SDK/proxy/gitpaths，调用GradleWrapperMain。WSL日志 `/root/.gradle/daemon/8.11.1/daemon-256044.out.log`，HTML报告 `app/build/reports/tests/testPlainDebugUnitTest/index.html`。无activeGradle/Java build。

生命周期仍待复核：Worker.onDestroy没有join server thread；detach/uninstall 3秒超时返回值处理弱；load/install部分失败的清理未验证。随统一server设计处理，不能认为上述Surface修复已涵盖完整shutdown。

Gradle properties 是：`-PmobileglDist=...`、`-PanlandNdkVersion=27.2.12479018`、`-PmobileglApplicationId=com.anland.consumer.mobilegl`。**不是 `anlandApplicationId`**。

```sh
MOBILEGL_DIST=/mnt/c/Users/Swung0x48/repos/FoldCraftLauncher/MobileGL/build/anland-dist \
./gradlew :app:assemblePlainDebug :app:testPlainDebugUnitTest \
  -PanlandNdkVersion=27.2.12479018 \
  -PmobileglApplicationId=com.anland.consumer.mobilegl
```

WSL 读 Windows worktree `.git` 文件的 `C:/...` path 有问题。Gradle版本追踪用明确 `GIT_DIR`/`GIT_WORK_TREE` 指向 `/mnt/c/...`，不要把运行成功但版本为 unknown 的结果当最终可追溯产物。

### 容器 client

```text
/root/mobilegl-unified-src
/root/mobilegl-unified-build
/root/anland-build-client.sh
/root/mobilegl-unified-client-build.log
```

GCC Release split ON、LTO OFF、`ninja -j4` 构建完成并安装：

```text
/opt/mobilegl/lib/libMobileGL.so       # 约21MB
/opt/mobilegl/lib/libEGL.so(.1)        # aliases
/opt/mobilegl/lib/libGL.so(.1)         # aliases
/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json
```

JSON library_path 指向 `/opt/mobilegl/lib/libMobileGL.so`。当前 source tar 不是完整 git checkout，源码更新需要同步；此前 tar 过宽排除 `build`/`test` 曾漏 xxHash/build/cmake 与 spirv-tools/test，已经补齐。

client 曾在 ADB offline 时中止，之后 nohup 恢复；**当前构建完成，不要重复全量 build**。最终统一状态改动需要增量同步与重构建。

诊断程序 `/opt/mobilegl/bin/anland-egl-smoke` 已编译，source 在 clone `/root/anland-egl-smoke.c` 和 Windows MobileGL `build/anland-egl-smoke.c`。测试 EGL pbuffer、GL renderer、clear/readback RGBA预期64,128,191,255。**从未运行**，不能算通过。

### 容器 KWin

```text
source/build: /root/mobilegl-build/kwin-6.7.4/build
log: /root/mobilegl-build/kwin-build.log
installed binary: /opt/mobilegl/kwin/bin/kwin_wayland
installed library: /opt/mobilegl/kwin/lib/libkwin.so*
helper: /opt/mobilegl/bin/mobilegl-startup.sh
```

full build 1358 actions 通过；uid1000 `--version` 为6.7.4，ldd 验证加载 `/opt/mobilegl/kwin/lib/libkwin.so.6`。实际 build lib 在 **`build/bin`**，不是 `build/lib`。

执行位曾因 Windows re-tar 丢失：上游 `src/plugins/strip-effect-metadata.py` 已恢复755；不是 C++失败。

KWin 使用**系统 GLVND + vendor JSON**。只把 rebuilt libkwin目录放 LD_LIBRARY_PATH；不要把 `/opt/mobilegl/lib` 放入 KWin LD_LIBRARY_PATH，抢走 GLVND EGL库。glxinfo 的直接 libGL facade 可单独 scope `/opt/mobilegl/lib`。

### Host tests

```text
/root/anland-mgl-build/host
/root/anland-mgl-build/host-build.log
/root/anland-mgl-build/host-server-tests.log
/root/anland-mgl-build/test-embedding
/root/anland-mgl-build/test-embedding.log
```

20 项测试通过：14 display/ABI、2 Unix pool、3 inprocess、1 parent-death。验证真实 SHM connections/fork/lifetime，**不是实际 GPU 渲染测试**。

CMake test 子目录早于 server target，显式构建 `MobileGLServer`，不能依赖 test target自动构建它。host clang22构建已完成，无 active cmake/ninja/test/server任务。

Host configure 使用本地GoogleTest：`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/root/mobilegl-build/_deps/googletest-src`，避免网络clone。已经通过的suite复跑：

```sh
ctest --test-dir /root/anland-mgl-build/host \
  -R '^(EmbeddedServerTest|ServerDisplayTest|UnixSupervisor|InProcessServer|SupervisorChildren)\.' \
  --output-on-failure
```

协议测试复跑命令：

```powershell
wsl -d archlinux -- bash /mnt/c/Users/Swung0x48/repos/anland-legacy-mobilegl-unified/tests/run_mobilegl_surface_exchange.sh
```

## 命令与续接注意事项

Android/client env（以后统一 endpoint 名称需据新设计修改）：

```sh
MOBILEGL_TRANSPORT=spawn
MOBILEGL_IPC_DATA=shm
MOBILEGL_IPC_CONTROL=unix:@<embedded-endpoint>
# compositor: MOBILEGL_IPC_SURFACE=server
# applications: MOBILEGL_IPC_SURFACE=offscreen（或清除）
```

`MOBILEGL_TRANSPORT=shm` **无效，会落回 monolith**；`MOBILEGL_IPC_SURFACE=client` 也无效。Listen/serve endpoint是 `@name`，client必须 `unix:@name`。

PowerShell 的 adb compound shell command 要保持 su quoting。以下错误写法只让第一条命令成为root：`adb shell su -c 'id; id -Z'`（adb flatten后分号跑到外层）。正确例子：

```powershell
adb -s HA27Q3LQ shell "su -c 'id; id -Z'"
```

更深的嵌套用 literal here-string或 push script，别使用 bash 的 `\"` 去转义 PowerShell字符串：

```powershell
$mglCmd = @'
su -c '/data/local/Droidspaces/bin/droidspaces --name=arch-kde-mgl run bash -lc "ls -l /run/display.sock; id"'
'@
adb -s HA27Q3LQ shell $mglCmd
```

## 推荐续接顺序

1. 阅读本文、用户原话、kgsl-as-server与调研；检查两个本地 workspaces，保护用户原有dirtysubmodules。不要重复 clone或基线build。
2. 首先完成 MobileGL 真正的 session/context/share-group/global-state设计，并移除 anland 应用离屏 Service/forkworker路径与对应旧文档。不要先把旧 APK再次部署来制造进展。
3. 保留有用的 embedded ABI、Surface生命周期、无fd显示协议、KWinB路线改动；新增状态/资源生命周期测试要真实覆盖多session、多surface、多context及share-group。
4. 增量构建统一 server与client，明确同一源码revision；构建APK并审查再安装。解决实验包 KernelSU rootgrant，待clone安全重启后使目录bind生效，保持原环境。
5. Enforcing下采集真实 socket/SCM_RIGHTS/memfd AVC并精确处理；确认 renderer仍在anland自己的进程/UID。
6. 先KWin单Surface真正上屏（renderer信息、GPU像素/截图、输入与帧节奏）；然后Konsole、eglinfo、glxinfo、Plasma。**不能只用进程存在或版本字符串判成功。**
7. 按实际失败修WSI、context共享、resize/Surface loss；记录截图与日志。阶段代码commit、推两个用户指定分支，最终写可复现步骤与真实限制。

目前没有合入/部署/最终验收请求被拒绝的自动审批。用户已授权实验clone、构建安装、修改、commit/push；除新的明确破坏性动作或确实缺失信息，不要重新索要相同授权。
