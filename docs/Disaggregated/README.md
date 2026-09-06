# MGPipe：MobileGL 前后端拆分

> 状态：**P0 已落地**（`feat/disaggregated@458ccde1`，基线 `dev@81b17c0b`）。下一步 P0.5 → P1 → P2，第 43 天 GO/NO-GO。见 `ROADMAP.md`。

## 是什么

MGPipe 是 MobileGL 前端（`MG_State` + `MG_Impl`）与后端（`MG_Backend`：Espryt = DirectGLES、Magma = DirectVulkan）之间的一份**显式接口**：gallium 形状、句柄寻址、只推不拉。它取代今天后端每 draw 直接读 `MG_State::pGLContext` 的做法，让后端拥有自己的状态机，并在此之上把前后端拆到**两个进程**。

接口本身是可独立交付的产物：即使 IPC 永不上线，`inproc`（同进程第二个 apply 线程）就是 monolith 的渲染线程。

## 架构（一段）

```
应用 GL 调用
  → MG_Impl（GL 语义、错误、shadow）
  → MG_Impl/Pipe/Tracker：在每条 verb 之前 validate，把变化推成 MGPipe 调用
  → MGPipeScreen / MGPipeContext（两张函数指针表，71 条调用，单一真相源 PipeCalls.def）
      monolith：直调 backend 函数          split：发射器写 SEG_CMD ring → server applier
  → server 对象表（按 {slot, gen} 句柄索引的数组）+ PipeInputs（后端被推送的状态块）
  → MG_Backend（Espryt / Magma），两个后端的 ring / pool / memo / lowering pass 原样不动
  ← MGPipeCallbacks（10 个具名反向回调 + 1 个正向终止符）
```

三种构建/运行形态共用**同一份 backend 实现**：`monolith`（默认，接口在进程内直调）、`inproc`（同进程两个线程，CI 形态与渲染线程交付物）、`spawn`（`fork`+`execve` 出 server 进程，SPSC 共享内存 ring + FlatBuffers 控制面）。

## 文件地图

| 文件 | 内容 |
|---|---|
| `ARCHITECTURE.md` | 已定稿的设计与架构：句柄与世代、调用目录、记录约定、tracker、纹理路径、shader 制品、反向通道、后端改造、传输、persistent map 分档、进程/EGL/平台、构建与纯度门、验证策略 |
| `ROADMAP.md` | P0…P13 阶段表、两条跑道、GO/NO-GO 清单、再基线检查点、仍然开放的问题 |
| `MEASUREMENTS.md` | P0 实测：spike A/B 结论、双设备四条 trace 的边界计数器基线、桌面数据点、语料事实、复现命令 |

代码地图（P0 已落地的部分）：

| 路径 | 作用 |
|---|---|
| `MobileGL/MG_Pipe/` | `PipeCalls.def`（目录）、`PipeFields.def`（比对器字段表）、`Coverage.def`（读点覆盖）、`MGPipeTypes.h`（payload POD）、`MGPipeHandles.h`、`MGPipeHostSpan.h`、`MGPipeCallbacks.h`、`MGPipe.h`、`generated/*.inc`（G1–G7 产物，提交进树） |
| `scripts/gen_pipe.py` | 七个生成器 G1–G7；`gen_pipe_dirty_surface.py` 前端 mutator 面扫描；`gen_protocol.py` FlatBuffers 头再生成；`check_doc_citations.py` 本目录 `file:line` lint |
| `MobileGL/MG_Remote/` | `Protocol/protocol.fbs`（控制面 schema）、`Transport/`（`Ring`、`Doorbell`、`ShmSegment`、`FdPassing`、`Framing`、`InProcessTransport`、`ITransport`）；仅 `MOBILEGL_BUILD_DISAGGREGATED=ON` 编译 |
| `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}` | 边界计数器（字节 / 动态 accessor 调用 / 六个 memo 门 / 上传形状），`MOBILEGL_PIPE_STATS=1` 开启 |
| `MobileGL/Config.h`、`MobileGL/ConfigLoader.cpp` | `MOBILEGL_PIPE_*` 八个开关 |
| `tools/spikes/server_stub`、`android-plugin/app/src/trace/cpp/spawn_spike.cpp` | spike A：Android 上以 `lib*.so` 打包并从应用进程 exec 第二个原生可执行文件 |
| `tools/spikes/extmem_probe/` | spike B：跨进程外部内存分档探针 |
| `MobileGL/MG_Test/Pipe/`、`MG_Test/Wire/`、`MG_Test/Util/PipeStatsTest.cpp` | 目录算术、wire 层五个套件、计数器测试 |

## 术语

- **client / server**：前端进程 / 后端进程；monolith 下是同一进程的两个角色。
- **verb**：会让 server 做事的命令（draw、dispatch、clear、blit、readback、XFB 跨度、query、纹理操作）。推送只发生在 verb 之前的 validate 时刻。
- **CSO**：常量状态对象（render state、vertex elements、sampler、sampler view、shader），client 侧内容寻址，server 侧按句柄缓存。
- **Track V / Track H**：值类读点的迁移（整块 POD 过线）/ 对象类读点的迁移（`SharedPtr<前端对象>` → 句柄）。
- **`MGGen`**：server 私有的"我重铸了驱动对象"纪元，永不过线；与句柄里的 client 世代严格分开。

## 历史

本目录此前是一份 328 KB 的实施计划（`PLAN.md`）加 135 KB 的设计竞赛与三视角对抗性评审记录（`REVIEW.md`）。设计已定稿，本次改写只保留设计与架构本身；评审记录、早期草案与修订史留在 git 历史里（`8b31de2f`、`1794ac94`、`8349babe`、`87ee17c6`；`git show 87ee17c6:docs/Disaggregated/REVIEW.md` 可取回评审记录全文）。更早的一条已放弃分支 `Feat/CS-Delta-IPC` 的逐文件可复用判定见 `8349babe` 版 `PLAN.md` §17。
