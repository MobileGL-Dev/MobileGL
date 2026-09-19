# MGPipe：MobileGL 前后端拆分

> 状态：**P0–P5e 已收官**（P5e 2026-09-19 收官，附一条具名未决：E1 对照需按 ID-114 重新定义），**当前在 P6 spawn transport**（计划 [`P6-SPAWN-PLAN.md`](P6-SPAWN-PLAN.md)、契约草稿 [`P6-CONTRACT-DRAFT.md`](P6-CONTRACT-DRAFT.md)，均尚未开工）。P5e：run-ahead 已武装（十二包落地，strict 车道硬绿 179/179、`integration-gpu` 1357/1357、pull/push/split 三个构建 flavour 全绿；设备上渲染距离 32 下 `inproc` 已与 `monolith` 齐平，相对它取代的 lockstep p50 +69%。报告 [`P5E-RUNAHEAD.md`](P5E-RUNAHEAD.md)）（P5c 2026-09-17 `b88e8487`；P5d 三轮 2026-09-18，代码头 `1f8de61b`——`inproc` 性能专项，VD12 Minecraft 上 split 从 7-13 fps 拉到 103-106 fps、client 线程 CPU/帧从 15 ms 到 9.2 ms，报告 [`P5D-INPROC-PERFORMANCE.md`](P5D-INPROC-PERFORMANCE.md)）。目标负载（四条 Minecraft A/B trace）已在 Redmi Adreno 830 上以 `inproc`（独立 apply 线程）双后端渲染，barrier tax 首次实测；P5c 把审计出的 59 处不经 wire 的直接内存访问全部归零（契约 `MobileGL/MG_Remote/CONTRACT-P5C.md`），`inproc` 成为只经 wire 交换的诚实两角色。**P6 只是传输替换**——但这句话的证据是 P5c 时代的，P6 的第一个包是重新证明它的只读审计。当前头、逐门数字与开放项见 [`CURRENT_STAGE_PROGRESS.md`](CURRENT_STAGE_PROGRESS.md)。
>
> 性能纪律（2026-09-08 起）：逐线程 CPU 与 tracker 绝对 ns **对着 pull 臂基线记录**，不作阻塞门；专门的优化阶段排在路线图推完之后。

## 是什么

MGPipe 是 MobileGL 前端（`MG_State` + `MG_Impl`）与后端（`MG_Backend`：Espryt = DirectGLES、Magma = DirectVulkan）之间的一份**显式接口**：gallium 形状、句柄寻址、只推不拉。它取代后端每 draw 直接读 `MG_State::pGLContext` 的做法，让后端拥有自己的状态机，并在此之上把前后端拆到**两个线程**（`inproc`，已达成）与**两个进程**（`spawn`，P6）。

接口本身是可独立交付的产物：即使 IPC 永不上线，`inproc` 就是 monolith 的渲染线程。

## 架构（一段）

```
应用 GL 调用
  → MG_Impl（GL 语义、错误、shadow）
  → MG_Impl/Pipe/Tracker：在每条 verb 之前 validate，把变化推成 MGPipe 调用
  → MGPipeScreen / MGPipeContext（两张函数指针表，79 条调用，单一真相源 PipeCalls.def）
      monolith：直调 backend 函数          split：发射器写 SEG_CMD ring → server applier（apply 线程）
  → server 对象表（按 {slot, gen} 句柄索引的数组）+ PipeInputs（后端被推送的状态块）
  → MG_Backend（Espryt / Magma），两个后端的 ring / pool / memo / lowering pass 原样不动
  ← MGPipeCallbacks（10 个具名反向回调 + 1 个正向终止符）
```

三种形态共用**同一份 backend 实现**：`monolith`（默认，进程内直调）、`inproc`（同进程两个线程，CI 形态与渲染线程交付物；P5 起 lockstep verb barrier，P5b 起 71 个后端槽里 54 个发射、15 个具名拒绝）、`spawn`（P6：`fork`+`execve` 出 server 进程，SPSC 共享内存 ring + FlatBuffers 控制面）。

## 构建与运行

四个 flavour：`build-linux`（pull，默认）、`build-push`（`-DMOBILEGL_PIPE_PUSH=ON`）、`build-verify`（`+MOBILEGL_PIPE_VERIFY` 影子比对，`MOBILEGL_ITEST_REQUIRE_GPU=1`）、`build-split`（`+MOBILEGL_BUILD_DISAGGREGATED +MOBILEGL_BUILD_DISAGGREGATED_INPROC`）。split 运行时必须显式设置传输：

```text
MOBILEGL_TRANSPORT=inproc MOBILEGL_ITEST_REQUIRE_GPU=1 ctest --test-dir build-split -L integration-split --output-on-failure
MOBILEGL_TRANSPORT=inproc ctest --test-dir build-split -L integration-gpu --output-on-failure   # 普查车道，只记录
```

`integration-split`（`DirectGLES.Split.*` / `DirectVulkan.Split.*`）是硬门；`integration-gpu` 在 inproc 下是带已知 abort / wrong-answer 的普查车道。每条 split entry 带独立 `MOBILEGL_LOG_FILE_PATH`，阴性控制（`scripts/ci/split_negative_controls.sh`）只读被选 entry 的私有日志。常用旋钮（全表见 `ARCHITECTURE.md` 附 A）：

| 变量 | 默认 | 用途 |
|---|---:|---|
| `MOBILEGL_IPC_STAGE_MB` | `32` | `SEG_STAGE` 容量；目标负载 profile 显式 `256`（单次 128 MiB 上传），默认不改 |
| `MOBILEGL_IPC_VERB_BARRIER` | `1` | 每个 verb 等 `appliedSeq == emitSeq`；`0` 只作 E1 阴性控制 |
| `MOBILEGL_IPC_AUDIT` | `0` | apply 返回后以 `0xDD` 填退休 staging，查跨返回持针 |
| `MOBILEGL_IPC_STRICT_ERRORS` | `0` | BARRIER-PULLED residual input 提升为具名 Fatal |
| `MOBILEGL_IPC_ADOPT_TIER` | `2` | split 使用 emulated persistent-map 路径；`auto/0/1/2` |
| `MOBILEGL_IPC_PERSISTENT_BLOCK_KB` | `64` | persistent-map 块推送粒度；`0` 是 E3(a) 阴性控制 |

Android 三份 APK flavour：pull、push、split（Gradle 属性 `mobilegl.pipePush`、`mobilegl.buildDisaggregated`、`mobilegl.buildDisaggregatedInproc`；`~/w7/notes/tools/wsl_build_p5_apks.sh`）。split APK 需运行环境 `MOBILEGL_TRANSPORT=inproc`，不设置时是 split build 的 monolith control arm（splitctl）。设备 A/B 只用 Redmi `2f7cbe2e`，定频协议见 `devices/pin-verification-2026-09-07.md`。

## 文件地图

| 文件 | 内容 |
|---|---|
| `CURRENT_STAGE_PROGRESS.md` | 当前头实测、P5b 落地内容、开放项、下一步、证据位置、仍在生效的裁定；随每次落地更新 |
| `ARCHITECTURE.md` | 已定稿的设计：句柄与世代、调用目录与生成器、记录约定、tracker、纹理路径、shader 制品、反向通道、后端改造、传输、persistent map 分档、进程 / EGL / 平台、构建与门、P5 / P5b 落地形状、开关表 |
| `ROADMAP.md` | 纪律、两条跑道、P0…P13 阶段表、里程碑、债务表、开放问题 |
| `P6-SPAWN-PLAN.md` | P6 的包计划：树上已有什么（传输原语基本已就绪且有测试）、真正要造什么、P6 与 P12 的边界、七个包与依赖序、出口门与阴性对照 |
| `P6-CONTRACT-DRAFT.md` | P6 契约草稿（规则 G、令牌、控制面帧、死亡与 run-ahead、阶段修正）；`c6` 落地时移为 `MobileGL/MG_Remote/CONTRACT-P6.md` |
| `MEASUREMENTS.md` | 逐阶段实测：P0 spike 与基线、P1 verify、P2 门与 DriverBench、P3a / P4a 门与设备 A/B、P5 全门 / E1–E6 / Redmi 四臂、P5b 主机门 / 普查 / 审查 / Redmi 出口 |
| `devices/pin-verification-2026-09-07.md` | 设备定频档案与核验（Redmi 当前口径；小米 / Oppo 历史核验） |

代码地图：

| 路径 | 作用 |
|---|---|
| `MobileGL/MG_Pipe/` | `PipeCalls.def`（目录）、`PipeFields.def`、`Coverage.def`、`FieldOwnership.def`、`FillPoints.def`、`DirtySurface.def`；`MGPipeTypes.h`（payload POD）、`MGPipeValueTypes.h`、`MGPipeHandles.h`、`MGPipeHostSpan.h`、`MGPipeCallbacks.h`、`MGPipeRenderStateSpans`、`PipeApply`、`PipeRoute`；`generated/*.inc`（G1–G8 产物，提交进树） |
| `MobileGL/MG_Impl/Pipe/` | `Tracker.h`、`PipeFill`、`SlotAllocator`、`CsoCache`、`CompositeResolver`、`ResourceTracker`、`SetHashSuppressor`、各族 `*Emit.h` |
| `MobileGL/MG_Backend/MGPipe/` | `PipeInputs.{h,cpp}`（后端被推送的状态块，G5 / G8 产物的 include 点） |
| `MobileGL/MG_Remote/` | 仅 `MOBILEGL_BUILD_DISAGGREGATED=ON`：`Protocol/`（`protocol.fbs`）、`Transport/`（`Ring`、`Doorbell`、`ShmSegment`、`FdPassing`、`Framing`、`InProcessTransport`、`ReplySlot`、`EventRing`）、`Wire/PipeWireCodec`（记录编解码、`WireVerbSink`）、`Client/`（`BackendObject_Remote`、`ClientSession`、`EmitTables`、`CapsMirror`、`PersistentMapTracker`）、`Server/`（`PipeApplier` + `ServerVerbSink`、`ServerLoop`、`ServerSession`、`StagedShadow`）；`CONTRACT-P5.md`、`CONTRACT-P5B.md` |
| `scripts/` | `gen_pipe.py`（G1–G7）、`gen_pipe_field_ownership.py`（G8）、`gen_pipe_dirty_surface.py`、`gen_protocol.py`、`symbol_report.py`（G1）、`check_include_closure.py`、`check_doc_citations.py`（本目录 `file:line` lint）、`p3a_/p4a_untouched_regions.sh`（G5）、三个阴性对照脚本、`ci/`（split 阴性控制、普查 JUnit、smoke） |
| `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}` | 边界计数器，`MOBILEGL_PIPE_STATS=1` 开启 |
| `MobileGL/Config.h`、`MobileGL/ConfigLoader.cpp` | `MOBILEGL_PIPE_*`、`MOBILEGL_TRANSPORT`、`MOBILEGL_IPC_*` |
| `MobileGL/MG_Test/Pipe/`、`MG_Test/Wire/`、`MG_IntegrationTest/Harness/Pipe*Peek` | 目录 / tracker / emitter 单元、wire 层与 remote client/server 单元、白盒断言 |
| `tools/spikes/`、`android-plugin/app/src/trace/cpp/spawn_spike.cpp` | spike A（Android exec 第二个原生可执行文件）、spike B（跨进程外部内存分档） |

## 术语

- **client / server**：前端进程 / 后端进程；monolith 下是同一进程的两个角色，inproc 下是两个线程。
- **verb**：会让 server 做事的命令（draw、dispatch、clear、blit、readback、XFB 跨度、query、纹理操作）。推送只发生在 verb 之前的 validate 时刻。
- **class A / B / C**：后端槽的三类——A 从 caps mirror 本地回答，B 发射记录，C 具名拒绝（`Fatal{UnmigratedVerb}`），永不回落到 monolith applier。
- **CSO**：常量状态对象（render state、vertex elements、sampler、sampler view、shader），client 侧内容寻址，server 侧按句柄缓存。
- **Track V / Track H**：值类读点的迁移（整块 POD 过线）/ 对象类读点的迁移（`SharedPtr<前端对象>` → 句柄）。
- **`MGGen`**：server 私有的"我重铸了驱动对象"纪元，永不过线；与句柄里的 client 世代严格分开。
- **G1 / G2 / G5 / G14**：pull 符号恒等 / pull==push 测试名 / 保护区字节一致 / 测试名只增不删。**barrier tax**：split − push 的逐线程 CPU 增量（inproc 传输 + verb barrier 的代价）。

## 历史

本目录此前是一份 328 KB 的实施计划（`PLAN.md`）加 135 KB 的设计竞赛与评审记录（`REVIEW.md`）；设计定稿后只保留设计与架构本身，评审记录与早期草案留在 git 历史（`8b31de2f`、`1794ac94`、`8349babe`、`87ee17c6`）。P5b 收官后（`ea34bccb` 及之前）各文档的完整历史叙述、-O0 设备表与逐轮审查记录同样以 git 历史为准；集成者的逐条裁定长文（ID-1..75）在 `ef35ea0c` 之前版本的 `CURRENT_STAGE_PROGRESS.md`。
