# F1 — 占位 CapsMirror 回答「server 消费这个族吗」，并丢掉整整一个族的记录（P7 wave 2）

> 代码：`MobileGL/Init.cpp`、`MobileGL/MG_Backend/Init.cpp`、`MobileGL/MG_Remote/Client/CapsMirror.{h,cpp}`、
> `MobileGL/MG_Remote/Client/ClientSession.{h,cpp}`、`MobileGL/ConfigLoader.cpp`、`MobileGL/Config.h`、
> `MobileGL/MG_Remote/Server/ServerSession.cpp`。
> 用例：`MG_Test/Wire/RemoteClientTest.cpp` 的 `CapsMirrorTest.*`（四条新的）。
> Fatal 族：`CapsBeforeFirstSnapshot`（`MG_Remote/FatalFamilies.def`，投影 `ProtocolCorruption`）。

## 0 发射者：不是竞态，是两条语句的顺序

设备日志里那行 R-8 警告

```
MG_Remote client: the server does not consume MGPipe subsystem 0x400 - this family emits
NOTHING and the legacy pull path runs for it (R-8). callMask=0x0, caps generation 0
```

的来源是一条**确定的调用链**，不是「主机上第一份 CapsSnapshot 赢了竞态、手机上输了」：

```
MobileGL::Initialize()                        Init.cpp:118  MG_ConfigLoader::Init()  → "Config loaded"
  └ MG_State::Init()                          Init.cpp:124
      └ GLState::GLContext::GLContext()       MG_State/GLState/Core.cpp:1291
          └ TextureState::TextureState()      MG_State/GLState/TextureState/TextureState.cpp:62-71
              └ ×11（每个 TextureTarget 一个默认纹理，GL 3.3 core 3.8）
                  TextureObjectBase::TextureObjectBase()
                      MG_State/GLState/TextureState/TextureObject.cpp:168-169
                    ├ MGPipeMintTextureHandle(*this)          ← 无条件，槽位照拿
                    └ MGPipeEmitTextureResourceCreate(*this)  ← 被丢弃
                        └ FamilyIsLive → P4aFamilyHasItsConsumer
                            MG_Impl/Pipe/PipeFill.cpp:1607
                          └ CapsMirror::ServerConsumes(0x400)
                              MG_Remote/Client/CapsMirror.cpp:137（修改前）
  └ MG_Backend::Init()                        Init.cpp:126   ← 会话在这里才开始握手
```

`MG_State::Init()` 在 `MG_Backend::Init()` **之前**，而启动会话的是后者。所以每一次 split 启动，
这 11 个默认纹理的 `resource_create` 都在 `callMask=0`、`generation 0` 上被判成「server 不消费」，
一条 WARN 之后再无痕迹。

**主机上同样发生。** lavapipe 上重跑 OpenRA / DirectVulkan / inproc（`rt-before-inproc.client.log:6`）
与 DirectGLES 集成用例（`redA-gles-inproc.client.log:6`）都逐字带这一行，位置也一样（`Config loaded` 的下一行）。
计划 §3 与简报里「主机上第一份快照赢了竞态」的读法**不成立**：这从来不是竞态，两臂都输，
只是主机上的 OpenRA 仍然 ssim 1.000000 / mismatch 0。

另外两条与简报不同的事实：

1. **P6.5 的「第一份真 CapsSnapshot 落地前不算会话启动」已经覆盖 inproc**。它在
   `ClientSession::FinishStartup` 第 7 步（`ClientSession.cpp:1284-1311`），而 `FinishStartup`
   是 inproc / spawn / unix / tcp 四条路径共用的唯一漏斗。所以计划里的选项 (b)「把 P6.5 修复推广到 inproc」
   **已经是树上的现状**，它修不了 F1：发射发生在会话**还没开始**的时候。
2. **选项 (a)「第一次 `Consumes()` 有界等待到 generation ≥ 1」单独用会死锁**：`MG_State::Init()`
   执行时没有任何握手在飞，将来那次握手是**同一个线程**稍后去做的。等待只会烧掉预算然后 Fatal。

## 1 修在哪个缝上

**顺序 + 规则，两件事各修一半。**

- **顺序（真正的修复）**：`MobileGL::Initialize()` 在 `MG_State::Init()` **之前**通过
  `MG_Backend::InitSplitRolesBeforeState()` 把 split 两角色带起来（`MobileGL/Init.cpp`）。
  `MG_Backend::Init()` 的 split 臂整体抽成 `InitSplitBackend()`，两个调用点共用一份；
  monolith 臂原地不动（它必须留在 `MG_State::Init()` 之后，挪它会动 pull `.text`）。
  整段在 `#if MOBILEGL_BUILD_DISAGGREGATED` 里，pull 构建里连语句都不存在。
- **规则（让回归变响）**：`CapsMirror::ServerConsumes()` 不再从占位镜像回答。
  `RequireFirstSnapshot()` 先排除两种「问错了对象」的情形——server 臂（`MGPipeServerArm()`：
  apply 线程与 spawn server 进程）和「本进程的配置根本没要过 split 传输」
  （`MG_Config::SplitTransportRequestedByConfig`，由 `MG_ConfigLoader::InitTransport()` 置位）——
  然后做一次**有界等待**，只有在**别的线程**确实在握手时才真的等
  （`ClientSession` 的 `BringUpScope` + `PublishedCapsGeneration()`），否则立刻
  `Fatal{CapsBeforeFirstSnapshot, "<族名>"}`。

没有做的两件事，按简报的禁令：**没有**把警告调轻，**没有**给 callMask 一个全 1 的默认值。
读取型访问器（`Renderer()` / `Dynamic()` / `Formats()` / `ApiVersion()` / `Backend()`）的 P5 裁定
**原样保留**——`LogBackendInfo()` 在 `MG_Backend::Init()` 里读 `GetRendererInfo()`，在那里 abort
不是更严格的镜像而是起不来的进程。改的只有那个**决定**。

## 2 red-once

三组，都在 lavapipe / Arch WSL 上，`build-split`（Release、clang、DISAGGREGATED+INPROC+PIPE_PUSH=ON）。

| # | 内容 | 结果 |
|---|---|---|
| A | **基线行为**（把 `Init.cpp`、`CapsMirror.{h,cpp}`、`ClientSession.cpp` 退回 HEAD） | `redA-gles-inproc.client.log:6` 与 `rt-before-{inproc,inproc-delay200,spawn-delay200}.client.log:6` 都带 R-8 那一行；OpenRA 仍 **ssim 1.000000 / mismatch 0** |
| B | **只退回顺序修复**（`git checkout HEAD -- MobileGL/Init.cpp`，保留新门） | 进程在 `Config loaded` 的下一行死于 `Fatal{CapsBeforeFirstSnapshot, "TextureResources"}`（`redB2.client.log:6`）——门是承重的 |
| C | **修复后** | R-8 那一行消失，`CapsMirror generation 1 adopted` 变成 `Config loaded` 之后的第一条 MG_Remote 行；OpenRA inproc / inproc+延迟 / inproc lockstep / spawn+延迟 / monolith 全部 ssim 1.000000、mismatch 0、`Fatal{` 计数 0 |

**A 组是这个包最重要的一条记录**：主机上 OpenRA 的 ssim 与那 11 条丢掉的 `resource_create`
**无关**——丢掉它们，lavapipe 上的 OpenRA 依然 1.000000。所以 F1 是一个真缺陷、现在关掉了，
但**它不能被 lavapipe 的图像差异证明**，也就**不能**据此断言它就是真机 0.976494 / 0.988998 的原因。
真机验证仍然欠着（见 §4）。

## 3 `MOBILEGL_TEST_DELAY_FIRST_CAPS_MS`

`ServerSession::Accept` 第 6 步之前的一次性延迟（`ServerSession.cpp`），只在
`MOBILEGL_BUILD_DISAGGREGATED` 下存在，不设就完全不动；只延迟**第一次**发布，R-12 的重发不受影响。

它证明的东西分两臂：

- **spawn / unix: / tcp://**：发布在**另一个进程**里，所以这是一次货真价实的「快照迟到」。
  实测 `MOBILEGL_TEST_DELAY_FIRST_CAPS_MS=200` 的 spawn 臂，server 日志里有那行 WARN
  （`rt-after-spawn-delay200.server.log:4`，进程名 `libMobileGLServ`），client 仍然在
  `Config loaded` 之后第一条就 `CapsMirror generation 1 adopted`——P6.5 第 7 步的等待在 spawn 上成立。
- **inproc**：`Accept` 就在 `ClientSession::Start` 里、同一个线程上，所以延迟只是让 `Start` 变慢。
  **这本身就是结论**：inproc 下第一份快照相对会话启动**不可能迟到**，因此客户端能读到占位镜像的窗口
  只有一个，就是 `Start` 被调用**之前**那一段——F1 的缺陷正是它，而它只能靠顺序关掉。

## 4 欠集成者的两件事

1. **真机复跑**：Redmi 2f7cbe2e / Adreno 830 / DirectVulkan / `--use-pbuffer` 上的 OpenRA，
   inproc（run-ahead ARMED 与 `MOBILEGL_IPC_RUN_AHEAD=0` 两臂）。预期是 R-8 那一行消失；
   ssim 是否从 0.976494 / 0.988998 回到 1.0 **本包不预测**——主机上这两者无关（§2 A 组）。
   如果真机 ssim 不动，0.976494 / 0.988998 的病因仍未定位，wave 1 的 E4–E6 还得走。
2. **一条裁定**：`MG_Config::SplitTransportRequestedByConfig` 是新的「本进程配置过 split」闩。
   它现在是门的武装条件；如果以后有哪条产品路径不经 `MG_ConfigLoader::Init()` 就手设
   `MG_Config::Transport` 并建前端对象，那条路径会静默回到占位回答。树上只有单测这么做
   （`ServerLoopTest` 的 `main`，已在那里写明原因）。
