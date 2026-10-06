# P13 — 退役 pull 路径（进行中）

> 计划 [`PLAN-P13.md`](PLAN-P13.md)，裁定 [`INTEGRATOR-DECISIONS-P13.md`](INTEGRATOR-DECISIONS-P13.md)，CI 清理记录 [`docs/ci/ci-cleanup-log.md`](../../../ci/ci-cleanup-log.md)。本页保存该阶段在路线图上的完整范围与出口门（2026-09-24 从 [`ROADMAP.md`](../../ROADMAP.md) 阶段表移入，原文照录）；阶段开工后，计划、裁定与报告都放在本目录。

## 进度

| 波 | 状态 | 提交 | 备注 |
|---|---|---|---|
| CI 基线 | 完成 | `cde6512b` `0cbb1a84` `c9085533` | 托管 Test `37414803850`、APK `37405703442` 的红全部归类修复：1.17 名字表（apitrace 分支 `45843686`）、CTest include 的 `IN_LIST`、环回 TCP 端口、spawn Welcome 冷启动预算 |
| W1 | 完成 | `5ff33cbb` | `runtime_mode_proof` 的 monolith = 无 MG_Remote；push 无 MG_Remote 控制库 + 零 MG_Remote 符号检查；retrace-split 不再因控制库失败整体跳过；skip 普查 |
| W2 | 完成 | `c6cb5dea` `68b56165` `9f05e665` `3e11c848` | CMake 默认 push；Gradle 删 `pipePush`（FCL 内嵌 push monolith、无 MG_Remote）；撤 pull 控制库。Test `37433779649` 三红均为非产品问题，已修（ci-cleanup-log 的基础设施表） |
| W3a | 完成 | `55de98a0` | `MOBILEGL_PIPE_PUSH` 不再是选项（旧缓存 OFF 警告并忽略） |
| W3b | 完成 | `9e14c16d`…`a4d10dd9`、`daf2b44c`…`883b8614`、`c0404018`、`a32ca001` | 删 `MOBILEGL_PIPE_LEGACY_MEMOS`、`MOBILEGL_PIPE_PUSH` 宏与 `MGB_CTX`；位图 bits 0-13 固定开，清位具名拒绝（`SubsystemMaskRefusal.*` 对照）；删 HandleRecycle Legacy / AbaControl 车道；每条移除记在 ci-cleanup-log |
| W3c | 完成 | `e2c90986` `be86ca17` | 删 `ResidualValueBlock`；op 46 退役为拒绝行；verify 的 A2 负对照接管能力比对 |
| W4a–c（Espryt） | 本地完成 | 14a `3e2aff1d` `cef29781`；谓词、W4a、W4b/W4c（本地） | Espryt monolith 全部族走记录臂：缓冲、indirect、client 数组、绑定点、GPU 写标记、fp64、重启索引、multi-draw、纹理（别名式 staged 纹理 store）、读回（含 pack buffer）、帧缓冲、单元 / sampler / image、program（monolith CSO 记录带档案副本）、XFB；进程内 verb 端口；§1.4 别名刷新。本地 unit 2905/2905、integration-gpu 3650/3650、DirectGLES retrace 29/29（另 11 条缺 LFS fixture 或主机 llvmpipe 的 iterationrp） |
| W4（Magma） | 本地完成 | 一次性换臂（本地） | monolith 注册 wire 资源表；进程内持久映射捐赠（`DonateWireBuffer`：wire store 本就 host-visible / coherent / 常驻映射，交给前端，取 T0 的「回读无字节」语义）；`GetSyncStatus` 在记录臂上对同一未提交 fence 连续轮询 64 次后提交一次（split 由 apply 线程空闲时提交，monolith 无空闲时刻）；ABA 对照车道钉前端臂（其对象是前端臂的身份表，W6 随之删）；`DescriptorPoolGrowth` 的 wire 预算按记录臂而非传输判断。本地 integration-gpu 3650/3650、DirectVulkan retrace 29/29（本机须 `MOBILEGL_GPU_HANG_BUDGET_MS=0`，与 CI 相同，否则 llvmpipe 上 >2 s 的提交触发挂死看门狗） |
| W4b 补 monolith 登记 | 本地完成 | | `DirectGLES.Monolith.CpuMip.*`（`MGITEST_ESPRYT_FORCE_CPU_MIPMAP`）与 `DirectGLES.Monolith.StoreRead.*`（`MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT`），各自私有日志断言本臂的日志行。StoreRead 首跑 3/4 红：别名式纹理 store 不跟随 `glCopyImageSubData`，目的 level 被标为驱动写过，驱动又拒读 → `Fatal{ReplyError, "ReadPixels"}`（W4 引入的 monolith 回归，只在驱动拒读该格式时可达）。修复：`FollowCopy` 在别名模式下先把目的 level 取成服务端自有副本再搬窗口（从不写前端内存），`AdoptClientBoxes` 随后在该副本上落客户端的框；单元 `AnAliasingStoreFollowsACopyIntoAServerOwnedDestination`。**真机（Adreno 750，Lenovo Y700，2026-10-06）**：不加旋钮时修复前后都全绿，日志里没有 store 路由——该驱动有 `GL_EXT_texture_norm16` 与 `GL_EXT_render_snorm`，四种格式的读回都走 GPU，P8-E 记的「Adreno 拒读 RGB16 / 16 位 SNORM」是读代码推断、在 750 上不成立（更老的 Adreno 或缺这两个扩展的驱动才可达）。加 `MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT=12x6` 强制拒读：修复前 monolith 的四个 copy 用例 `Fatal{ReplyError, "ReadPixels"}`（inproc 全绿），修复后 monolith 与 inproc 全绿（探针 `storeprobe`，裸 adb-shell ELF + pbuffer） |
| W4d | 部分（本地） | | monolith 记录臂的 8 个值类字段改由 `set_context_values` 记录供给（不再 residual 填充）。转发访问器替身、`kFatal` 前端指针字段删除、`PipeInputs.h` 去 `MG_State` 头并入 W6/W7（它们是删前端臂与模块边界的一部分） |
| GL_ARB_shader_objects（用户插单） | 本地完成 | | 原先 11 个入口是桩（`glCreate*ObjectARB` 恒返回 1）、其余 28 个在 `eglGetProcAddress` 里根本查不到，扩展却对外宣告（KWin 要这串）。现在：句柄就是核心共享的 shader/program 名字空间里的名字（`glCreateShaderObjectARB` = `glCreateShader`），核心与 ARB 调用互用；`DeleteObjectARB` / `GetHandleARB` / `GetObjectParameter{i,f}vARB`（`OBJECT_TYPE` / `OBJECT_SUBTYPE` 与按类型的核心 pname，错误码按扩展规范）/ `GetInfoLogARB` 在 `GL_ShaderObjectsARB.cpp`，其余转发核心。同样宣告的 GL_ARB_vertex_shader 的 46 个入口（29 个桩、17 个缺失）一并转发核心。顺带修了核心缺陷：`glDetachShader` 后 `GL_ATTACHED_SHADERS` / `glGetAttachedShaders` 仍算该 shader、二次 detach 不报错、下次 link 前重新 attach 被拒（GL 4.6 §7.3）。`glGen*` 各名字空间互相独立、保留名在首次绑定前不是对象（sampler 例外）、删除后不再是对象——新场景逐类断言，全部本来就合规。wire 侧 ARB 入口全走核心入口，记录臂按 `{slot, gen}` 一视同仁。场景 `ArbShaderObjectsScenario`（两后端 × monolith / split / spawn / tcp） |
| W5 S0 | 本地完成 | | 设计见 `notes/p13/` 下的 `W5-DESIGN.md`（临时 CMake 变量 `MOBILEGL_BUILD_RECORD_ARM`，S7 一次翻转，S8 unifdef 掉）。开放三点已定：记录失败钩子在 `FatalFunnel.cpp` 静态注册；`RecordArmTable` 承接三个持久映射 / wire 延迟旋钮（`IpcTable` 仍只在 DISAGG）；S7 加 mprotect 臂开关（FCL 是 JVM 进程，SIGSEGV 链接是真风险）。清单 `W5-INVENTORY.csv`（`tools/w5_classify.py`）：A 385 块 / 10586 行、B 29/434、C 92/10878、D 89/4683、E 77/933、F 292/3184、N（取反 / 复合）30/1545。主机 x86_64 Release 基线：push 库 `.text` 11540563、0 个 `MG_Remote::` 符号；split 库 `.text` 12766899、1651 个 |

### G1 退役读数（ID-P13-4，2026-10-06）

pull 构建（`DISAGGREGATED=OFF`、`PIPE_PUSH=OFF`，只编 `MobileGL` target），同机同编译器（WSL archlinux，clang 22.1.6）：

| | P12 收官 `743f4e8e` | W1 `5ff33cbb` |
|---|---|---|
| `.text` | `0xa52203` | `0xaa6ba3` |
| 定义符号（`nm --defined-only` 名字） | 30570 | 31578（增 1300、减 292） |

基准一侧的 `.text` 与 P12 门记录的 `0xa52203` 相同。变化来自 P12 收官之后的 346 个提交（GL 层修复、KWin 用的 ARB 字符串等，两臂共用的代码；增量按命名空间：`MG_Backend` 290、`MG_Impl` 140、`MG_State` 92），G1 在 P12 之后就不再是任何提交的门。本读数之后 G1 退役，接班：零 MG_Remote 符号检查（W1）、skip 普查（W1）、recorder 金标（W8）。

## 摘要

- 删 `SnapshotFromGLContext()` 非 verify 分支、`MGB_CTX`、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`、`set_residual_value_state`；保留 `MOBILEGL_PIPE_VERIFY`；MGPipe recorder；重调幸存缓存容量；建立模块 target 与边界（D12）。P7 留下的 86 个链接棘轮符号标 `# P13`。
- 门：`static_assert(sizeof(ResidualValueBlock) == 0)`；三道纯度门在非 verify 构建上转绿；recorder 金标建立；monolith 逐线程 CPU 不差于 P0 基线。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P13** 退役 pull 路径
- **状态**：待排
- **落地什么 / 范围**：删 `SnapshotFromGLContext()` 非 verify 分支、`MGB_CTX`、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；保留 `MOBILEGL_PIPE_VERIFY`；MGPipe recorder；删 `set_residual_value_state`；在计数器活着的情况下重调幸存缓存容量；建立模块 target 与边界（D12：`SOURCE_FILES` 是一张平表喂两个库 target，无模块可链）
- **验收门 / 证据**：`static_assert(sizeof(ResidualValueBlock) == 0)`；三道纯度门在非 verify 构建上转绿；recorder 金标建立；monolith 逐线程 CPU 不差于 P0 基线
