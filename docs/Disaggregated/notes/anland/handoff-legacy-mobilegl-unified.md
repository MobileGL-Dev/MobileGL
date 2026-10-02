# Handoff：anland 5.x + 内置 MobileGL SHM server

更新时间：2026-10-01（用户时区 America/Cuiaba）。**本文档是 anland 统一 server 工作的多会话权威状态文档**：动手前先通读，动手后更新。当前这版已刷新到 P14（MobileGL 库内的状态归属）落地之后的真实状态。

## 先读：当前完成度与状态

**架构已按用户纠正落地；真机验收仍未完成。KWin、Konsole、eglinfo、glxinfo、Plasma 都没有通过 MobileGL 真机验收。构建成功、进程存在、版本字符串都不能当成验收成功。**

用户原始目标：

1. 在 anland 新建 `legacy-mobilegl-unified`，让 anland 5.x 使用 MobileGL。
2. SHM split server **必须是 anland APK 的一部分，由 anland 管理**，client 在 Droidspaces 容器内。
3. 在联想 Y700 TB321FU 上复制 `arch-kde` 为 `arch-kde-mgl`，作为实验环境。
4. 依次调通 KWin → Konsole → eglinfo → glxinfo → Plasma。
5. 阶段成果形成可二分的 commits，推送 anland fork 和 MobileGL `feat/disaggregated`。
6. 开始前阅读此目录调研，尤其 [`kgsl-as-server.md`](kgsl-as-server.md)，技术路线接近 B：anland 自己的 Surface 交给 MobileGL server 直接渲染。

用户已明确排除两种路线，这两条纠正**仍然有效**：

> anland fork 的 `legacy-mobilegl` 路径是配合 MobileGL feat/disaggregated-container-gbm 的错误路径。MobileGL server 不应该活在独立的 plugin app 里，应该成为 anland 的一部分。

> 说起来，为什么会有实现一个应用离屏 Service？如果 MobileGL 有全局状态需要解决，那应该处理的是 MobileGL 自己的全局状态，而不是想办法制造多个 MobileGL peer。

**当前状态**（本轮刷新：2026-10-01 约 20:40 America/Cuiaba，对应设备本地 2026-10-02 08:4x CST）：

- 被否定的 fork-worker / `MobileGLAppsService` 路径**已移除**：anland APK 不再 fork 每应用 worker、不再声明第二个 endpoint（`33e496c`）。MobileGL 侧 `--serve --max-sessions` supervisor 只作为独立工具保留代码，anland 流程不再依赖它。
- **统一 server 设计已在 MobileGL 库内实现（P14）**：设计文档 [`../design/11-state-ownership.md`](../design/11-state-ownership.md)（`d52c031c`），主体 S1–S5 `4ed1c9ce`、S6 `ff212061`、KWin 需要的 ARB 扩展串 `5a0f13fd`。单进程多 session、per-EGLContext 独立 GLContext、真实 share group 都已在库内落地；未做完的工程欠债见下文「P14 状态归属：已落地与已知欠债」。
- **真机验收仍在进行**（用户有并行会话在推进）：KWin 的 `ANLAND_MOBILEGL` 后端与 APK 内置 embedded server 都已上设备，server 端已观察到真实 client session 建连、native context 创建与探测日志；但 KWin 真正上屏还没有验收，Konsole、eglinfo、glxinfo、Plasma 未开始。
- **存在并行会话同时操作同一台设备/容器与 Windows 仓库**（本文刷新时最近的 KWin 尝试就在几分钟前）。动手前先做并发检查，见下文「并发警告」。

## 工作区、分支、保留事项

### MobileGL

```text
C:\Users\Swung0x48\repos\FoldCraftLauncher\MobileGL
origin: https://github.com/MobileGL-Dev/MobileGL.git
显示分支: feat/disaggregated
当前实现 HEAD: 5a0f13fd（P14 最后一个实现 commit；本文档 commit 在其之上）
```

Windows 上真实本地 ref 的大小写是 `refs/heads/Feat/disaggregated`。推送使用：

```powershell
git push origin HEAD:refs/heads/feat/disaggregated
```

不要用失败的 `git push origin feat/disaggregated` 重复排查网络。

用户原有 dirty submodule `3rdparty/glslang` 必须保留，不能提交、重置或当成本任务成果。为 split 构建初始化过缺失的 `3rdparty/flatbuffers`，没有修改 gitlink。本地 glslang 工作区实际在 `6f125987`（比记录的 gitlink `d89cf443` 新若干 commit，就是用户自己的 dirty 内容）。

工作区当前未提交改动**只剩**用户原有的 dirty submodule `3rdparty/glslang`。原「本任务未提交改动」三件已全部提交：

| 原改动 | 落在 |
|---|---|
| `CMakeLists.txt`：Linux 增加 `libGL.so`、`libGL.so.1` 别名 | `6fb880cd` |
| `EGLImpl.cpp`：Wayland `Present()` 失败返回 `EGL_BAD_SURFACE`；`GLXImpl.cpp`：spawn 模式 X drawable 用远端 pbuffer，避免把 XID 当 Android ANativeWindow | `ebc552a0`（**GLX 仍无 X11 读回/present、无远端 resize**） |

DirectGLES/BackendObject「保留一个 native context、切换多个 surface」在 P14 S4 **已经真正实现**：原生 EGL tuple 按 `(session, context token)` 键控，`InitDisplayAndContext()` 首句的无条件 `DestroyEGLContext()` 已移除，建第二个 context/surface 不再销毁第一个。证据是真实 headless EGL 的 `ServerLoopEglTest.CreatingASecondContextLeavesTheFirstNativeContextAlive` 与 `CreatingAnotherSurfaceKeepsTheSameNativeContext`。

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
当前本地/远端 HEAD: 33e496c9564e34b48185d7dd81c0c8956a397f3d（与 origin/legacy-mobilegl-unified 一致）
```

工作区已与远端一致，只剩 3 个 KWin 验收相关文件有未提交小改动（见「KWin 6.7.4 后端」一节）。不要重新 checkout 远端分支来替代工作区。

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
| `2f64ede8` | Unix `--serve --max-sessions 1..64`，fork worker 并发 | **用户否定用它解决 anland 全局状态。anland 已不依赖它；代码只作为独立工具保留，不是最终架构，不能当任务完成。** |
| `4156ca6e` | 上一版 anland 交接文档 | 文档 |
| `d52c031c` | **P14 设计文档**：state ownership for multi-session/context/share-group | 设计权威：`design/11-state-ownership.md` |
| `6fb880cd` | Linux 增加 `libGL.so`/`libGL.so.1` 别名 | 已推送 |
| `ebc552a0` | Wayland `Present()` 失败上报 + GLX drawable 不拿 XID | 已推送 |
| `4ed1c9ce` | **P14 主体 S1–S5**：`bind_context` wire 记录（opcode 84）+ CreateContext/DestroyContext 控制帧、control revision 5→6；SessionRuntime 按 session 实例化（singleton 降级为 fallback）、in-process supervisor 最多 16 并发 session；前端 `ShareGroupState` + per-EGLContext `GLContext`、`pGLContext` 变 thread-local；DirectGLES 原生 tuple 按 `(session, token)` 键控、不再拆 context、`eglCreateContext` share 接到真驱动；applier 拆成 `MGPipeObjectRecords`（per share group）与 `MGPipeWorkingState`（per context） | 已合入并推送 |
| `5c6399e2` | S5 文档：记录已落地形状与剩余 twin-key 欠债 | 文档 |
| `ff212061` | **P14 S6**：八个 DirectGLES twin 表按 `{SessionKey, ShareGroupKey}` 重键（`TwinKey` + `ResolveThreadTwinKey`），`DropEveryTwinForEndedServerSession` 变 per-session | 已合入并推送 |
| `5a0f13fd` | KWin 所需的 GLSL 1.00 时代 ARB 扩展串（`GL_ARB_shader_objects`/`vertex_shader`/`fragment_shader`/`texture_non_power_of_two`）；KWin 6 的 `EglContext::checkSupported()` 要这些串才起 compositing | 已合入；本文档 commit 之前它是 branch HEAD |

这些独立 client 改动来自旧分支，但没有 cherry-pick 独立 plugin/GBM/host-frame 整体架构。`32c79f41` 冲突处理中移除了尚未实现的 device/GBM 扩展广告。

### anland `legacy-mobilegl-unified`

三笔 commit 都已推送到 `origin/legacy-mobilegl-unified`：

| Commit | 内容 |
|---|---|
| `6e1fa65` | no-fd MobileGL Surface 显示协议模式：`DATA_MSG_MOBILEGL_SURFACE=201`、`ANLAND_FORMAT_MOBILEGL_SURFACE=0x4d474c53`。metadata 没有 fd，保留一 pacing slot（fd=-1、idx=0）。真实 daemon + producer + consumer 测试验证 metadata、frame ack、输入事件。没有假造 dma-buf。 |
| `2ffa1f5` | KWin `ANLAND_MOBILEGL` 后端：跳过 GPU/DRM probing、不广告 dma-buf/syncobj、渲染进 server-owned EGL window（default FBO 0、full damage、`eglSwapBuffers`）；未设该变量时保留 Mesa producer。 |
| `33e496c` | **APK 内置统一 embedded server**：私有同 UID `:mobilegl` Service 加载 libMobileGL，为 compositor 和所有容器 client 提供**单个 endpoint `@anland-mobilegl`**；session 调度留在 MobileGL 库内，Anland 不 fork 每应用 worker、不传 session budget。每应用离屏 worker Service `MobileGLAppsService` **已删除**。 |

## anland 草稿：已落地与已移除

### Android APK/JNI：已落地（`33e496c`）

下列文件（含 `MobileGLWorker.java`、`MobileGLConnection.java`、`mobilegl_worker.c`、`MOBILEGL.md`）都已提交：

```text
consumers/anland_v5/android_consumer/MOBILEGL.md
consumers/anland_v5/android_consumer/app/build.gradle
consumers/anland_v5/android_consumer/app/src/main/AndroidManifest.xml
consumers/anland_v5/android_consumer/app/src/main/java/com/anland/consumer/MainActivity.java
consumers/anland_v5/android_consumer/app/src/main/java/com/anland/consumer/MobileGLConnection.java
consumers/anland_v5/android_consumer/app/src/main/java/com/anland/consumer/MobileGLWorker.java
consumers/anland_v5/android_consumer/app/src/main/java/com/anland/consumer/Native.java
consumers/anland_v5/android_consumer/app/src/main/jni/CMakeLists.txt
consumers/anland_v5/android_consumer/app/src/main/jni/mobilegl_worker.c
consumers/anland_v5/android_consumer/app/src/main/jni/native_consumer.c
```

`MobileGLAppsService.java` 已删除；manifest/binding/health check、第二个应用 endpoint、`libMobileGLServer.so --serve --max-sessions 16` 依赖与旧文档一并移除。**现在的 anland 侧只有「统一 embedded server + 正常多 client 连接」一条路径。**

仍有效的设计要点：

- `MobileGLWorker` 是同 APK、同 UID 的私有 `:mobilegl` Service（不是 isolatedProcess），接收 anland 自己的 Surface，JNI 通过 `dlopen/dlsym` 使用中立 ABI。
- 主 UI 进程不加载 MobileGL 的 GL 导出；避免影响 Android framework/UI 的 EGL。
- `MainActivity` 等待 worker attach 后才启动 native display consumer。
- MobileGL 模式 native consumer **不连接 ANW_API_CPU、不 dequeue/queue Surface buffer**，EGL 是唯一生产者；输入、音频、显示协议继续保留。
- 新 Surface parcel 每次会产生不同 native wrapper。保留同一 window owner/generation 的 Java Surface，尺寸更新使用保留的 ANativeWindow，避免 resume/resize 把活动 session detach。
- native geometry callback → one-way Binder → UI SurfaceHolder；`surfaceChanged` 先报告尺寸，再等待 native transport 停止，避免 geometry/ack 互等。
- `Surface.getGenerationId()` 是隐藏 API，曾导致编译失败；改为 NDK native identity token + 本地生命周期 generation。
- CMake 使用仓库根下的 authoritative common/libdisplay 源码，避免 Windows 把 symlink checkout 成 target-text 文件。
- `onDestroy` 先 join serve thread 再拆 display，runtime 不会被从活着的 apply 下面抽走。
- `mobileglDist` staging 在切回 unbundled 时显式 cleanup；**Gradle Sync 没有 sources 时会 NO-SOURCE，不自动删除旧文件**。

worker 里 `MOBILEGL_BACKEND_TYPE` 当前工作区改成了 `DirectGLES`（未提交，见下），设备上装的 APK 仍是提交版（`Espryt`）。

### KWin 6.7.4 后端

**anland 5.x 是宿主协议版本，容器当前 KWin 实际为 6.7.4。**

已提交（`2ffa1f5`；工作区另有未提交小修，见下）：

```text
producers/kde/Arch_v5/kwin.patch
producers/kde/anland_backend_Arch_v5/src/backends/anland/anland_backend.cpp
producers/kde/anland_backend_Arch_v5/src/backends/anland/anland_backend.h
producers/kde/anland_backend_Arch_v5/src/backends/anland/anland_egl_backend.cpp
producers/kde/anland_backend_Arch_v5/src/backends/anland/anland_egl_backend.h
producers/kde/Arch_v5/mobilegl-startup.sh        # 100755
producers/kde/Arch_v5/mobilegl.md
```

`CMakeLists.txt` 也可能显示 dirty（曾尝试 PUBLIC include，已撤销语义改动；检查实际 diff/行尾）。

`ANLAND_MOBILEGL=1` 路径：跳过 GPU/DRM probing，不广告 dma-buf/syncobj，不要求 GBM/no_config/surfaceless。先选择 RGBA window config，创建 server-owned EGL window，再创建 RenderDevice/shared contexts。默认 makeCurrent 使用该 Surface。output layer marker 走 default FBO 0、full damage 和 eglSwapBuffers，不导入 consumer dma-buf。Mesa 路径保留。

full repaint 是因为 Android BufferQueue 自己轮换 buffers，而 client 暂未提供 buffer age。RenderTarget transform 保持原 Anland FlipY/output transform 语义。

native 构建已通过。public backend headers 的 include 路径已改为自足的相对 include：`main_wayland.cpp` 包含它们时也必须找到 producer/protocol header，不能靠 backend PRIVATE include dirs。

launcher 有 `compositor`/`plasma` modes；统一 server 只有 `@anland-mobilegl` 一个 endpoint，两个 mode 不再按 endpoint 分配环境。`mobilegl-startup.sh` 已带 executable bit `100755`。

**当前工作区还有 3 个未提交的 KWin 验收相关小改动**（未推送；动手前先 `git diff` 确认，不要覆盖）：

| 文件 | 改动 |
|---|---|
| `.../com/anland/consumer/MobileGLWorker.java` | worker 的 `MOBILEGL_BACKEND_TYPE` 由 `Espryt` 改为 `DirectGLES`（**APK 侧，需重打包才生效；设备上装的仍是提交版**） |
| `producers/kde/Arch_v5/mobilegl-startup.sh` | compositor 侧 `MOBILEGL_LOG_FILE_PATH` 默认 `/tmp/mobilegl-compositor.log`（**容器里已安装的副本还是旧版，没有这个变量**） |
| `.../backends/anland/anland_backend.cpp` | `eglChooseConfig` 判定由 `count != 1` 放宽为 `count < 1` |

## P14 状态归属：已落地与已知欠债

统一 server 的状态归属已经在 MobileGL 库内实现，不再依赖“每进程隔离”绕开问题。设计与逐切片的形状、逐项源码定位（行号随实现移动）见 [`../design/11-state-ownership.md`](../design/11-state-ownership.md)；本节只记交接需要的结论。

**已落地（`4ed1c9ce` S1–S5、`ff212061` S6）**：

- **Wire**：`bind_context` ring record（opcode 84）在 make-current 边沿宣告当前 context；`CreateContext`/`DestroyContext` 进入 SurfaceOp 控制帧；control revision 5→6；server 保存 per-session token 表，未知 token 按名字拒绝。
- **Session**：`SessionRuntime` 按 session 实例化，线程作用域解析，singleton 降级为 fallback；per-thread `gPipeInputs`、per-session latch domain、per-thread segment resolver；in-process unix supervisor 最多 16 个并发 session；surface mode 按 session 声明，display lease 拒绝第二个 on-screen session（不再靠 `SurfaceModeMismatch`）。
- **Context**：前端 `pGLContext` 变 thread-local（进程默认 context 逐字保留单 context 世界），`eglCreateContext` 建真实 `GLContext` 并加入共享 context 的 share group；原生 EGL tuple 按 `(session, context token)` 键控，`bind_context` 是原生切换点；建第二个 context/surface 不再 teardown 第一个。
- **Share group**：`ShareGroupState` 持有共享对象 store 与名字生成器（buffer/texture/renderbuffer/sampler/program），`GLContext` 持有 per-context bindings/errors/container 对象。
- **Applier**：`MGPipeApplierState` 拆成 `MGPipeObjectRecords`（per share group）与 `MGPipeWorkingState`（per context），经 `{session, token}` registry 解析，带 thread-local 快路径。
- **Twin**：八个 DirectGLES twin 表按 `{SessionKey, ShareGroupKey}` 重键（`TwinKey`，group 由 `MGPipeApplierShareGroupKeyFor` 派生，记录与 twin 不可能落进不同桶）；`DropEveryTwinForEndedServerSession` 变 per-session（无 session 的线程保持旧的全量清理形状）。

**已知欠债（已写进设计文档，下一刀要补的）**：

- **单元影子仍是进程级**：`g_activeTextureUnit`、`g_boundTexturesCache`、`g_boundSamplersCache` 与 sampler cache（`Managers.cpp` 约 11031-11032）还是全局（S6 报告记为 P14b/c）。残留风险是“同一个 twin 在一次原生 context 切换之后被缓存命中而跳过绑定”。
- **`g_backendContextGeneration` / `g_syncContextGeneration` 仍是进程级**：销毁单个 context 会推进全局世代，其他 context 的旧 fence/query 句柄因此读作已 signal。方向安全（最坏是多等），但不是 per-context 世代。
- **逐记录 `ContextSlot` 未做**：wire 仍走 S1 的“边沿宣告 + `bind_context`”，没有在 71 个 context 族记录的 header 与 payload 之间插 `Uint32 ContextSlot`；多线程交错下的逐记录身份只在 client 按 make-current 顺序发射时成立。
- **驱动级 `glIsBuffer` 共享探针未通**：S4/S5/S6 都没得到可用控制（同 context 内 `glGenBuffers` 后 `glIsBuffer` 即返回 false）。S6 改走 twin 身份这条替代证据（twin 就是持有 driver id 的对象），驱动级 shared/nonshared 证据仍然是缺的。
- **pbuffer→window 的 window 半边未验**：`pbuffer→pbuffer` 只到 `ServerLoopEglTest.CreatingAnotherSurfaceKeepsTheSameNativeContext` 的程度；真正的三 pbuffer 来回、以及 window surface 那一半需要真机 window，headless 验不了。

**还需要的真实验证**：相同 handle 值的两个 session 隔离并独立关闭；shared contexts 访问同一 buffer/texture；nonshared contexts 同名资源隔离；每 context 的 viewport/FBO/bindings/error 独立；跨线程移交；pbuffer→window→pbuffer 保留资源。无头/单元测试（`MultiSessionTest`、`ServerLoopEglTest` 等）已经覆盖其中一部分，**但它们都不等于真机 GPU 渲染验收**。

### WSI 现有限制

- Wayland：server pbuffer → readback RGBA → flip/swizzle → 三槽 wl_shm buffer → wl_surface；有独立 release event queue。`Present()` 失败现在返回 `EGL_BAD_SURFACE`（`ebc552a0`），不再静默忽略。
- 未跟随 `wl_egl_window` resize；也没有隐藏窗口/pacing 完整验收。
- 当前 GLES frontend 的版本/precision 等能力尚不能当完整 GLES 支持；`GL_VERSION` 仍 desktop 4.6，precisionformat 有 stub。KWin 优先 desktop GL，但 Plasma QtQuick 必须实际验证。
- GLX 只让 glxinfo 的 X drawable 可以用远端 pbuffer 查询（`ebc552a0` 已提交）；**没有 X11 readback/present，所以不能声称通用 GLX 窗口上屏，也没有远端 resize**。
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
clone init PID: 31212（继续前重新确认；宿主机侧是 `droidspaces --name=arch-kde-mgl start`，PID 31210/31211）
boot_id: 3f4027fc-50de-400a-8751-cc925d61e99d（设备 host boot_id，本轮未变）
```

有另外三台 ADB 设备，所有命令必须带 `-s HA27Q3LQ`。设备多次短暂 offline，`adb -s HA27Q3LQ reconnect` 后恢复；uptime 与 boot_id 证实设备本身没有重启（本轮 uptime 20:24）。

原 `arch-kde` 保持停止、配置未改。clone 从其 128GiB sparse ext4 image 拷贝约 7.7GB allocated extents；因为 Toybox/Busybox 不支持 `conv=sparse`，用 NDK SEEK_DATA/HOLE helper。不要重复全量复制。

clone Mesa 基线曾实测：freedreno FD750，OpenGL 4.6 Mesa 26.3.0，1280×800、scale2、120Hz。这是 **Mesa 基线，不是 MobileGL 成果**。

仅 clone 安装过构建依赖 GCC16.1.1/cmake/ninja/base-devel；没有 clang。clone 的 patched Xwayland `24.1.13-1.1` 为安装依赖降到仓库 `-1`，原容器未动。实验 backend 对 Xwayland 使用 `-shm` 并清除其 MobileGL loader 环境。

### 当前状态与目录 bind

clone 已**重启过**（init PID 31212），**目录 bind 已生效**：容器内 `/run/anland-mobilegl` 的 inode 与宿主机 `/data/local/tmp/anland-mobilegl` 一致（本轮实测都是 59897）。使用目录 bind 是为了 daemon unlink/recreate socket 时不留下 file-bind 的旧 inode。

`egl_vendor.d` 的文件名问题（部分已修，**还有残留**）：

- 正确名 `50_mobilegl.json` 已就位（内容 `library_path=/opt/mobilegl/lib/libMobileGL.so`）。
- 但同目录里**旧的 `50_mobilegl.json\r`（inode 999692）仍然存在**——构建脚本只写正确名、不删旧文件。建议清理，避免 GLVND vendor 扫描时读到带 `\r` 的坏文件名。

`desktop-session.service` 已 `disable`，`mobilegl-kwin-build.service` 为 inactive。**注意：本文刷新时该 service 处于 `failed` / `Result=start-limit-hit`**——并行会话在设备本地 08:19–08:26 跑起过一次 KWin（约 8 分钟），停止时 `kwin_wayland` SEGV（status=11），之后 08:27 的连续重启全部立即 SEGV、触发 start-limit。**下次手动验收前先 `systemctl reset-failed desktop-session.service`**（disable 状态下手动 start 仍可用）。

已在 clone config 追加（已生效）：

```text
bind_mounts=/data/local/tmp/display_daemon.sock:/run/display.sock,/data/local/tmp/anland-mobilegl:/run/anland-mobilegl
```

配置备份：`/data/local/Droidspaces/Containers/arch-kde-mgl/container.config.mobilegl-before`。

clone service override：

```text
/etc/systemd/system/desktop-session.service.d/mobilegl.conf
[Service]
ExecStart=
ExecStart=/opt/mobilegl/bin/mobilegl-startup.sh compositor
Environment=ANLAND_SOCKET=/run/anland-mobilegl/display.sock
```

原 service 的 `User=swung0x48`、`PAMName=login` 保留。单纯 `droidspaces --user=... run` 不会建立正常 logind/user D-Bus session，Plasma mode 要借此 service 或等效正常登录。

实验 display daemon 在跑（不是 MobileGL renderer，只是原有协议 broker）：

```text
host socket: /data/local/tmp/anland-mobilegl/display.sock
binary: /data/adb/modules/anland-daemon/display_daemon
log: /data/local/tmp/anland-mobilegl/display-daemon.log
实验 PID: 13993（本轮实测仍在）
原 daemon PID: 2315，socket /data/local/tmp/display_daemon.sock（未停止）
```

实验 APK 启动时 `--es socket_path /data/local/tmp/anland-mobilegl/display.sock`；KWin 使用容器 `/run/anland-mobilegl/display.sock`，目录 bind 已生效。

### Android 安装与 root 授权（已解决）

原有 `com.anland.consumer`（uid10253）、`com.anlandnext`、`com.droidspaces.app` 未覆盖。

**KernelSU root grant 已解决，不再是阻塞项**：

- `com.anland.consumer.mobilegl` 重装后 uid 变成 **10270**（`u0_a270`）；KernelSU manager 里原来的 10269 是过期条目，所以一直拨不动开关。
- 处理办法：`force-stop` manager 让它重新枚举应用，再拨开关授权，成功。allowlist 现在有该包的有效条目。
- 早先关于 `ksud profile` 没有 grant 子命令、`GET/SET_APP_PROFILE` ioctl 仅 manager UID 可调的调研结论仍然成立，但**不再需要自己写 profile mutation helper**。临时源仍在 `C:\Users\Swung0x48\AppData\Local\Temp\anland-mobilegl-device`（对应 upstream `4d396fda53e510d75f2608a184df56ccd33aac9c`），只作参考。

实验 APK 当前状态（本轮实测）：

- 装的是 **`5a0f13fd` 构建**：`app-plain-debug.apk`，7,702,188 B，sha256 `26d70bac8dd3ec1ef5f7bb79eb063ae5bbaa3b21bddc0ff60256e4f4e0f5384b`。
- root helper 探测成功（不再跳 Settings）；`:mobilegl` worker 进程内 server 在 abstract socket **`@anland-mobilegl` 上 LISTEN**；server window attached 2560x1412。
- 已观察到 **6 个真实 client session 完成**（server log 最后一条 `in-process unix session #6 pid=13811 reaped exit=0 sessionsFaulted=0`，设备本地 08:28）。
- server 日志在 `/data/data/com.anland.consumer.mobilegl/files/mobilegl-server.server.log`（注意 `.server.log` 后缀；`mobilegl-server.log` 不存在）。这份日志里有完整的 GL 能力探测输出，是查 KWin 失败原因的第一现场。
- **app 必须保持前台**：回 HOME 会让 Surface detach、worker 停止。

Android 全局 Enforcing，但已安装 Droidspaces module 的 `sepolicy.rule` 包含 `permissive droidspacesd`。这不是本任务设置的。该 module 已有双向 fd use 与部分 unix_stream read/write 规则。**容器 → APK abstract socket、SCM_RIGHTS/memfd 的真实 AVC 尚未采集**（KWin 已能连上并建 context，说明现有规则放行到了这一步）。不要先全局 `setenforce 0`；按下面「推荐续接顺序」精确实测再加规则。

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

dist 目录（`build/anland-dist`）当前只有 `libMobileGL.so`（17,789,480 B，2026-10-01 20:22 Cuiaba 产出）与 `EmbeddedServer.h`；`libMobileGLServer.so` 已被 20:28 的 clean-dist 脚本清掉。**这个 dist 是 APK 打包用的，二进制实测带 `5a0f13fd` 的 stamp（已装 APK 里那份 libMobileGL.so 也是同一个 17,789,480 B、同一个 stamp）**。但它是"某会话在当时的工作区上构建"的产物，目录本身不能证明源状态；重打 APK 前仍先确认 dist 与目标 commit 一致。

另一处 Android 构建（`/root/anland-mgl-build/android`）的 generated/MGGitHash.h 历史上记录过 stamp `32c79f41350186274b6d545083656d564c0ea80f`、native client stamp `a5b463a5`——那是旧构建。**无论哪一份，build 时都可能包含未提交源码，stamp 不能证明源状态**：两端最终要用同一个确认 commit、显式 `-DMOBILEGL_BUILD_STAMP=<commit>` 重新构建，并验证 wireFingerprint，不要忽略 build drift。

一次 WSL shell quote 失误曾留下 `/libMobileGL.so`、`/libMobileGLServer.so`、`/EmbeddedServer.h` 任务残留，正确定向产物已复制到 dist。若清理只清这些明确文件，不广泛清理 `/`。

### Android anland APK

```text
C:\Users\Swung0x48\repos\anland-legacy-mobilegl-unified\consumers\anland_v5\android_consumer
artifact: app\build\outputs\apk\plain\debug\app-plain-debug.apk
```

当前 artifact 就是**已装到设备的 `5a0f13fd` 构建**：2026-10-01（Cuiaba）产出，**7,702,188 bytes**，SHA256 `26d70bac8dd3ec1ef5f7bb79eb063ae5bbaa3b21bddc0ff60256e4f4e0f5384b`。它是「统一 embedded server」版本，**不含 `MobileGLAppsService`**。

更早那版 7,381,976 bytes / SHA256 `CD15C203...7772`（含被否定路径，11 suites / 98 tests 全绿）已作废，只作历史参考。**JVM 单元测试只是逻辑测试，不能验证统一 server，更不能验证 GPU。**

可复用构建脚本及日志：

```powershell
wsl.exe -d archlinux -- bash /mnt/c/Users/Swung0x48/repos/anland-legacy-mobilegl-unified/build/build_bundled_apk.sh
```

脚本固定 JDK17/SDK/proxy/gitpaths，调用 GradleWrapperMain。WSL 日志 `/root/.gradle/daemon/8.11.1/daemon-256044.out.log`，HTML 报告 `app/build/reports/tests/testPlainDebugUnitTest/index.html`。

生命周期：`33e496c` 的 `onDestroy` 已经先 join serve thread 再拆 display，堵住了上一版的 shutdown 竞态；**detach/uninstall 的 3 秒超时返回值处理与 load/install 部分失败的清理仍未真机验证。**

Gradle properties 是：`-PmobileglDist=...`、`-PanlandNdkVersion=27.2.12479018`、`-PmobileglApplicationId=com.anland.consumer.mobilegl`。**不是 `anlandApplicationId`**。

```sh
MOBILEGL_DIST=/mnt/c/Users/Swung0x48/repos/FoldCraftLauncher/MobileGL/build/anland-dist \
./gradlew :app:assemblePlainDebug :app:testPlainDebugUnitTest \
  -PanlandNdkVersion=27.2.12479018 \
  -PmobileglApplicationId=com.anland.consumer.mobilegl
```

WSL 读 Windows worktree `.git` 文件的 `C:/...` path 有问题。Gradle 版本追踪用明确 `GIT_DIR`/`GIT_WORK_TREE` 指向 `/mnt/c/...`，不要把运行成功但版本为 unknown 的结果当最终可追溯产物。

### 容器 client

```text
/root/mobilegl-unified-src        # tar 同步的源码树，不是 git checkout
/root/mobilegl-unified-build
/root/anland-build-client.sh      # 配置 + 构建 + 安装
/root/mobilegl-unified-client-build.log
```

GCC Release split ON、LTO OFF、`ninja -j4`。当前安装：

```text
/opt/mobilegl/lib/libMobileGL.so        # 22,170,048 B，设备本地 2026-10-02 08:36
/opt/mobilegl/lib/libEGL.so(.1)         # aliases
/opt/mobilegl/lib/libGL.so(.1)          # aliases
/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json
```

**build stamp 与内容不一致（已知、非阻塞）**：`MGGitHash.h` 里 `MOBILEGL_BUILD_STAMP_VALUE=ff212061...`，但源码树内容已经包含 `5a0f13fd`（`BackendObject_DirectGLES.cpp` 有 ARB 扩展串、`DirectGLES.h` 有 `ShareGroupKey`）。原因是 `/root/anland-build-client.sh` 不传 `-DMOBILEGL_BUILD_STAMP`，而 `CMakeCache.txt` 里留着上一次的 `ff212061`。wire fingerprint 兼容，client log 只提示 "compatible wire with different build"。**下次重建要显式 `-DMOBILEGL_BUILD_STAMP=<实际构建 commit>`。**

**警告：容器源码树的 glslang 曾被覆盖过，重建前先验证 `External/`。** `build/anland-overlay-glslang.sh` 把容器内 `3rdparty/glslang` 重置成「记录版 gitlink `d89cf443` 的干净拷贝 **+ 保留 `External/`**」；`External/`（嵌套的 SPIRV-Tools / googletest 检出）不在那个 tar 里，而 MobileGL 顶层 `CMakeLists.txt` 无条件链 `SPIRV-Tools-opt`/`SPIRV-Tools`（`CMakeLists.txt:734`），External 一旦被删，配置/构建就失败。某会话确实制造过这个状态；脚本已改为保留 External，本轮实测容器树 `External/spirv-tools`（28 项，含 `source/`、`include/`）与 `External/googletest` 在位，并在 08:36 成功做了一次增量重链。**续接时仍先 `ls /root/mobilegl-unified-src/3rdparty/glslang/External` 确认；要干净重建就清掉旧 `.o`/构建目录再配。** 注意 overlay 用的是记录版 `d89cf443`，与 Windows 工作区里用户 dirty glslang（`6f125987`）内容不同，两端 glslang 版本有差异。

源码树是 tar 同步（没有 `.git`），源码更新需要手动同步；此前 tar 过宽排除 `build`/`test` 曾漏 xxHash/build/cmake 与 spirv-tools/test，已经补齐。

诊断程序 `/opt/mobilegl/bin/anland-egl-smoke`（另有 `anland-egl-win-smoke`）已编译，source 在 clone `/root/anland-egl-smoke.c` 和 Windows MobileGL `build/anland-egl-smoke.c`。测试 EGL pbuffer、GL renderer、clear/readback 预期 RGBA=64,128,191,255。**尚未记录到运行通过，不能算通过。**

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

Android/client env（统一 server 只有 `@anland-mobilegl` 一个 endpoint，compositor 与所有容器 client 都指向它）：

```sh
MOBILEGL_TRANSPORT=spawn
MOBILEGL_IPC_DATA=shm
MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl
# compositor: MOBILEGL_IPC_SURFACE=server
# applications: MOBILEGL_IPC_SURFACE=offscreen（或清除）
# worker 侧 backend: MOBILEGL_BACKEND_TYPE=DirectGLES
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

## 并发警告

**存在并行会话同时操作同一台设备、同一个容器和这个 Windows 仓库**，已经互相覆盖过 `build/anland-dist` 与容器源码 tar。动手前的检查清单：

1. 设备上是否有 app / KWin 在跑：`adb -s HA27Q3LQ shell "su -c 'ps -A -o PID,USER,ARGS | grep -iE \"anland|kwin|display_daemon\"'"`；容器内 `systemctl is-active desktop-session.service`、`pgrep -a kwin`。
2. 仓库 HEAD 是否已经变过：MobileGL 与 anland 两个工作区各 `git rev-parse HEAD` 一次，并看有没有新的未提交改动。
3. 容器里是否正在构建：`pgrep -af 'ninja|cmake'`，以及 `/root/mobilegl-unified-client-build.log` 的 mtime。
4. 拿不准就先只读、不动文件；确实要覆盖前先和用户确认，别把别人 in-flight 的改动当残留删掉。

## 推荐续接顺序

1. **先确认没有并行会话在动设备/容器**（见上一节）。读本文、`design/11-state-ownership.md`、用户原话与 [`kgsl-as-server.md`](kgsl-as-server.md)；检查两个本地 workspace，保护用户原有 dirty submodules。不要重复 clone 或基线 build。
2. **修容器源码树的可构建性**（本轮若不重建 client 可跳过）：确认 `3rdparty/glslang/External/{spirv-tools,googletest}` 在位，必要时重新同步完整源码树；清旧 `.o`/构建目录后重建，并显式 `-DMOBILEGL_BUILD_STAMP=<实际 commit>`；重建后核对 `MGGitHash.h` 的 stamp 与 wire fingerprint。
3. **KWin 验收**：`systemctl reset-failed desktop-session.service` 后 `start`（app 保持前台、display daemon 在跑），看 KWin 是否真上屏——渲染器字符串（`Renderer:` 应为 MobileGL 而不是 freedreno/Mesa）、GPU 像素/截图、帧节奏。同时取三处证据：容器内 `journalctl -u desktop-session.service`、server 侧 `/data/data/com.anland.consumer.mobilegl/files/mobilegl-server.server.log`、Android `logcat`。上一轮 `kwin_wayland` 是 SEGV（status=11），先从 server log 尾部与 KWin 自身输出里找第一个失败点，不要靠猜。
4. **Enforcing 下采真实 AVC**：容器 → APK abstract socket、`SCM_RIGHTS`/memfd 路径，按实际 denial 精确加规则，**不要 `setenforce 0`**；确认 renderer 仍在 anland 自己的进程/UID。
5. Konsole、eglinfo、glxinfo、Plasma 依次验收；每项都要真实像素/渲染证据，不能只看进程存在或版本字符串。
6. 成果 commit/push（MobileGL `feat/disaggregated` + anland `legacy-mobilegl-unified`），并把这篇文章更新成最终可复现文档。

目前没有合入/部署/最终验收请求被拒绝的自动审批。用户已授权实验clone、构建安装、修改、commit/push；除新的明确破坏性动作或确实缺失信息，不要重新索要相同授权。

## 更新记录

- **2026-10-01（America/Cuiaba）**：由 agent 刷新为 **P14 落地后**的状态。改动：先读节改为「架构已落地 / 真机验收未完成」；MobileGL 节 HEAD 更新到 `5a0f13fd` 并补 P14 各 commit 与已知欠债；anland 节补 `2ffa1f5`/`33e496c`，把原「未提交草稿」节标记为已落地/已移除；设备节记录 KernelSU grant 已解决（重装后 uid 10270）、clone 重启后目录 bind 生效、APK 已装 `5a0f13fd` 构建（7,702,188 B）且 worker 在 `@anland-mobilegl` LISTEN；容器 client 节记录 22,170,048 B 的 lib 与落后一个 commit 的 stamp、glslang overlay 的 `External/` 注意事项；新增并发警告并重写续接顺序。
