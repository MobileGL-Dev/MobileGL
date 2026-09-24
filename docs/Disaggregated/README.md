# MGPipe：MobileGL 前后端拆分

> **状态（2026-09-24）**：P0 … P7 已收官；P6.5 第一波已落地；**当前阶段 P12**「server 自有屏幕窗口」子集，已实现、未收官——[`CURRENT_STAGE_PROGRESS.md`](CURRENT_STAGE_PROGRESS.md)。阶段表见 [`ROADMAP.md`](ROADMAP.md)。
>
> 性能纪律（2026-09-08 起）：逐线程 CPU 对着 pull 臂**只记录**、不作阻塞门；专门的优化阶段排在路线图推完之后。

## 是什么

MGPipe 是 MobileGL 前端（`MG_State` + `MG_Impl`）与后端（`MG_Backend`：Espryt = DirectGLES、Magma = DirectVulkan）之间的一份**显式接口**：gallium 形状、句柄寻址、只推不拉。它取代后端每 draw 直接读 `MG_State::pGLContext` 的做法，让后端拥有自己的状态机，并在此之上把前后端拆到**两个线程**（`inproc`）、**两个进程**（`spawn`），乃至经 TCP 的**两台机器**。

接口本身是可独立交付的产物：即使 IPC 永不上线，`inproc` 就是 monolith 的渲染线程。

## 架构（一段）

```
应用 GL 调用
  → MG_Impl（GL 语义、错误、shadow）
  → MG_Impl/Pipe/Tracker：在每条 verb 之前 validate，把变化推成 MGPipe 调用
  → MGPipeScreen / MGPipeContext（两张函数指针表，单一真相源 PipeCalls.def）
      monolith：直调后端      split：发射器写记录 → 数据面（共享段 / 字节流）→ server 的 applier（apply 线程）
  → server 对象表（按 {slot, gen} 句柄索引）+ PipeInputs（后端被推送的状态块）
  → MG_Backend（Espryt / Magma），两个后端的 ring / pool / memo / lowering 原样不动
  ← MGPipeCallbacks（9 个具名反向回调 + 1 个正向终止符，split 下走 SEG_EVENT）
```

三种拓扑共用**同一份后端实现**：`monolith`（默认）、`inproc`（同进程两线程，CI 主验收；client 在 draw 路径上 run-ahead——Espryt 自 P5e、Magma 自 P5f 之后）、`spawn`（两进程；控制面 `fork` / `unix:` / `tcp://` × 数据面共享段 / 字节流，可跨机）。设计全文见 [`ARCHITECTURE.md`](ARCHITECTURE.md)。

## 构建与运行

四个 flavour：`build-linux`（pull，默认）、`build-push`（`-DMOBILEGL_PIPE_PUSH=ON`）、`build-verify`（+`MOBILEGL_PIPE_VERIFY`，影子比对）、`build-split`（+`MOBILEGL_BUILD_DISAGGREGATED` +`MOBILEGL_BUILD_DISAGGREGATED_INPROC`）。split 构建必须显式选拓扑，不设时是 monolith 对照臂：

```text
MOBILEGL_TRANSPORT=inproc MOBILEGL_ITEST_REQUIRE_GPU=1 ctest --test-dir build-split -L integration-split --output-on-failure
ctest --test-dir build-split -L 'integration-(spawn|tcp)' --output-on-failure   # 条目自带 spawn / tcp 环境与私有日志
```

常用旋钮（全表见 [`ARCHITECTURE.md`](ARCHITECTURE.md) 附 A）：`MOBILEGL_TRANSPORT`（拓扑）、`MOBILEGL_IPC_CONTROL` / `MOBILEGL_IPC_DATA`（控制面 / 数据面）、`MOBILEGL_IPC_TOKEN`（非 loopback 必需）、`MOBILEGL_IPC_RUN_AHEAD`、`MOBILEGL_IPC_STAGE_MB`（默认 32，1/4 是内容分块预算）、`MOBILEGL_IPC_SURFACE=server`（使用 server 自有窗口，P12）、`MOBILEGL_PIPE_STATS=1`（边界计数器）。

Android：三份 APK flavour（pull / push / split，Gradle 属性 `mobilegl.pipePush`、`mobilegl.buildDisaggregated`、`mobilegl.buildDisaggregatedInproc`）。设备侧 server 有离屏的前台 Service 与上屏的 `MobileGLDisplayActivity` 两种形态。设备 A/B 只用 Redmi `2f7cbe2e`，定频协议见 [`devices/pin-verification-2026-09-07.md`](devices/pin-verification-2026-09-07.md)。

## 文件地图

| 文件 | 内容 |
|---|---|
| [`CURRENT_STAGE_PROGRESS.md`](CURRENT_STAGE_PROGRESS.md) | 当前阶段：落地了什么、门、下一步、阻塞（摘要；细节链到 `notes/<当前阶段>/`） |
| [`ROADMAP.md`](ROADMAP.md) | 纪律、两条跑道、阶段表（每阶段一行摘要 + 细节链接）、里程碑、开放的债务与问题 |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | 已定稿的设计（§1–§16）、阶段落地形状索引（§17）、开关（附 A）与计数器（附 B） |
| [`MEASUREMENTS.md`](MEASUREMENTS.md) | 每阶段的头条实测数字，全文在各阶段 notes |
| [`notes/`](notes/README.md) | **阶段细节**：每阶段一个目录，`README.md` 是该阶段的汇总（阶段表原行、逐门数字、落地形状），其余是计划、契约草稿、裁定、审计、报告与证据 |
| [`devices/`](devices/pin-verification-2026-09-07.md) | 设备定频档案与核验（跨阶段共用） |

代码地图：

| 路径 | 作用 |
|---|---|
| `MobileGL/MG_Pipe/` | 调用目录与各 `.def` 表、payload / 句柄 / 回调头、`PipeApply`、`PipeRoute`、`generated/*.inc`（G1–G8 产物，提交进树） |
| `MobileGL/MG_Impl/Pipe/` | Tracker、`PipeFill`、`SlotAllocator`、`CsoCache`、`CompositeResolver`、`ResourceTracker`、各族 `*Emit.h` |
| `MobileGL/MG_Backend/MGPipe/` | `PipeInputs`（后端被推送的状态块） |
| `MobileGL/MG_Remote/` | 仅 `MOBILEGL_BUILD_DISAGGREGATED`：`Protocol/`、`Transport/`（`ITransport` / `ILink` 两轴）、`Wire/`、`Client/`、`Server/`；各阶段 wire 契约 `CONTRACT-*.md` |
| `scripts/` | `gen_pipe.py`（G1–G7）、`gen_pipe_field_ownership.py`（G8）、`gen_pipe_dirty_surface.py`、`gen_protocol.py`、`symbol_report.py`（G1）、`check_include_closure.py`、`check_doc_citations.py`（本目录 `file:line` lint）、`link_ratchet.py`、G5 保护区脚本、`ci/` |
| `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}` | 边界计数器，`MOBILEGL_PIPE_STATS=1` 开启 |
| `MobileGL/MG_Test/Pipe/`、`MG_Test/Wire/`、`MG_IntegrationTest/Harness/*Peek` | 目录 / tracker / emitter 单元、wire 与 remote 单元、白盒断言 |
| `tools/trace_replay/`、`tools/device_bench/` | retrace 跑器（含 TCP 矩阵）、设备定频与配对 A/B 工具 |

## 术语

- **client / server**：前端角色 / 后端角色；`inproc` 下是两个线程，`spawn` 下是两个进程（可在两台机器上）。
- **verb**：会让 server 做事的命令（draw、dispatch、clear、blit、readback、XFB 跨度、query、纹理操作）；推送只发生在 verb 之前的 validate 时刻。
- **class A / B / C**：后端槽的三类——A 从 caps mirror 本地回答，B 发射记录，C 具名拒绝（`Fatal{UnmigratedVerb}`），永不回落到 monolith。
- **CSO**：常量状态对象（render state、vertex elements、sampler、sampler view、shader），client 侧内容寻址，server 侧按句柄缓存。
- **Track V / Track H**：值类读点的迁移（整块 POD 过线）/ 对象类读点的迁移（`SharedPtr<前端对象>` → 句柄）。
- **lockstep / run-ahead**：每条 verb 等 server 应用完 / client 发布完即走，只有 barriered 记录才等（P5e）。
- **`MGGen`**：server 私有的"我重铸了驱动对象"纪元，永不过线；与句柄里的 client 世代严格分开。
- **G1 / G2 / G5 / G14**：pull 符号恒等 / pull == push 测试名 / 保护区字节一致 / 测试名只增不删。**barrier tax**：split − push 的逐线程 CPU 增量。

## 历史

本目录最早是一份 328 KB 的实施计划加 135 KB 的设计竞赛与评审记录；设计定稿后只保留设计与架构，评审与早期草案在 git 历史（`8b31de2f`、`1794ac94`、`8349babe`、`87ee17c6`），以及 `notes/recovered/` 里捞回的独有稿。2026-09-24 再次整理：四份索引文档只留摘要，各阶段细节（阶段表原行、逐门数字、落地形状、当时的进度页）移入 `notes/<阶段>/README.md`，散在本层的阶段报告与计划（P5d / P5e / P5f / P6）移入各自阶段目录；整理前的版本见 `1fb18d9`。
