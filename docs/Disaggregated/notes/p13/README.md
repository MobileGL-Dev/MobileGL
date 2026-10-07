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
| W5 S1 | 本地完成 | `534f14e3` `3b441531`（已推） | 致命词表（`FatalFamilies.def`、`MGFatalFamily`、名字、计数）移到 `MG_Pipe/PipeFatalFamily.h`，`MG_Remote/FatalFamily.h` 只留线上投影并转出旧名。记录臂的失败 / 闩缝：`MGPipeRecordFail` / `MGPipeRecordLatch` / `MGPipeRecordLatched`（`PipeSessionFail.h`，默认：日志 + stderr + abort），链接了 MG_Remote 时由 `FatalFunnel.cpp` 静态注册 `SessionFail` / `SessionLatch` / `SessionLatched`。fatal census 认新调用（规则 2）并认默认分支的 abort 为漏斗；单元 `RecordFailSeamTest` 两棵树都跑 |
| W5 S2 | 本地完成 | | `MOBILEGL_BUILD_RECORD_ARM`（普通 CMake 变量，恒定义 0/1，S7 前等于 DISAGG）；`tools/w5_rewrite.py` 把 MG_State（缓冲 / 程序 / 帧缓冲状态）、MG_Impl（GLImpl、Pipe）、MG_Pipe、MG_Util/SelfTest、MG_Backend 下 A / F 类与 `!DISAGG`、`VERIFY && !DISAGG` 守卫改名，共 536 组、按目录 7 个提交；EGL 层、GL 上下文的设备丢失状态、日志角色文件、统计 dump 路径留给 S3 手工。不变量已核：split 与 push 两库 `.text` 字节相同（sha 前缀 `a0d08e0e` / `df9d3b7a`，与 S1 一致）。`wire_declines_audit` 把新守卫当作构建选择开关（含两条自测） |
| W5 S3 | 本地完成 | `e544121e`…`7f194a85` | 手工改守卫：记录臂的声明、存根、T0 拆分、Magma wire 纹理 / 顶点输入构建、对象死亡、caps 来源跟 `RECORD_ARM`；共享图像、设备丢失闩、双块诊断留 DISAGG。应用侧后端经 `MG_Backend::ApplyRoleBackend` 钩子（`ServerLoop.cpp` 静态安装，无传输时为空，与 monolith 原答案相同） |
| W5 S4 | 本地完成 | `73a185f6` `0f5658f5` `fe5324c9` `8e76f9dc` `85125167` | 代码迁出 MG_Remote，命名空间 `MG_Record`：暂存库到 `MG_Backend/Record/`；持久映射跟踪器到 `MG_State/.../BufferState/`（领养层判定留 `MG_Remote/Client/AdoptTierChoice`）；`ServerVerbSink` 的记录动词抽成 `RecordVerbSink`（`ServerVerbSink` 继承它，只留会话动词）；动词发射器、monolith 动词端口、客户端 GPU 写集合到 `MG_Impl/Pipe/Verb/`（会话经 `MG_Record::VerbSession`，`EmitTables.cpp` 静态装解析器）。include-closure 新探针 `record-core`（`--expect-probes 5`）。link ratchet 171 不变，fatal census 76 不变 |
| W5 S5 | 本地完成 | `b13af729` `b1978890` | 记录臂三旋钮进 `RecordArmTable`；`DataArmIsRecord` / `RecordArmAliasesFrontend`、`MonolithTakesRecordArm`、`MOBILEGL_PIPE_DATA_ARM` 跟 `RECORD_ARM`（不再跟传输） |
| W5 S6 | 本地完成 | `dd7fbbd7` | 集成 peek 在无传输构建里也答 `dataArmIsRecord`；跟踪器探针跟 `RECORD_ARM` |
| W5 S7 | 本地完成（CI 待跑） | `19610290` `23593c9f`…`e9a353c3` | **翻转**：`MOBILEGL_BUILD_RECORD_ARM` 恒 ON，无 MG_Remote 的库（FCL 形态）也跑记录臂。`MOBILEGL_PIPE_PERSISTENT_MPROTECT=0` 关掉跟踪器的 SIGSEGV 处理器（FCL 是 JVM；启动打日志；单元 `TheMprotectSwitchSendsEveryNewMapToTheHashScan`），mprotect 臂只在 Linux / Android 编。`runtime_mode_proof` 加正半：两形态都要 `MG_Record` 符号（push 294、split 300），自测含两条无 `MG_Record` 的红例。翻转后 push 单元暴露的残留误分类（记录动词的 verb 状态盖章、按记录读的顶点 / IBO / 绑定点 / 采样器路径、XFB 捕获回落、视图窗口 mip、镜像可绑定重铸、暂存库跟随、跟踪器成员资格、测试的前端臂钉）全部改跟 `RECORD_ARM`；push 单元 2028/2028 |
| W5 S8 | 已推（CI 37515697754：382 绿、2 红均已归类并修） | `7fb18840` `c1e985c8` | `MOBILEGL_BUILD_RECORD_ARM` 被 unifdef 掉，`retired_switches` 禁止它回到 `#if`。verify 车道的阴性对照 B 改省略 `ReadPixels:GetPixelStoreParameters`（monolith 记录臂不再读 GenerateMipmap 的活动单元）；monolith 启动时记一行数据臂 |
| W6a | 已推（`887547c7`，CI 37534648608 / 37534648624） | `d61a4240` `740dd301` | 记录臂是唯一数据臂：`DataArmIsRecord()` 恒 true（constexpr），`MOBILEGL_PIPE_DATA_ARM` / `InitDataArm` / `ScopedMonolithFrontendArm` 删；`HandleRecycle.AbaControlHandles` 车道退役；立方面读回两条规则移到 `CubeFaceReadbackScenario` |
| W6b | 已推（同上） | `742d43ae` `e0113503` `3aed7ae0` `e822c933` | 数据臂谓词从两后端所有分支折掉，记录臂 return 之后的前端臂代码删；XFB 角色状态只留一份；10 个前端臂单元用例删（逐条记入 `docs/ci/ci-cleanup-log.md`），两条 GL 采样规则移到 `TextureUnitSamplingScenario` |
| W6c | 本地完成 | `dc1e95b4` | Magma 前端臂：前端 `MagmaProgramSource` 构造与 `IsWire()` 折掉，前端采样集遍历、占位纹理、VAO draw memo、`MagmaPipeIdentityTables` 与 ABA 旋钮、`TrySetupDrawFastPath` 与快照、前端 fallback 纹理删（约 7100 行） |
| W6d | 本地完成 | `8abae647` `8bc39c79` | Espryt 各族臂谓词（framebuffer / texture / sampler / program / buffer / vertex input 及其别名）折为记录臂，`g_fboTextureSyncList` 等前端遍历删；启动时只查色彩附件上限（`ResolveRecordArmFamilies`）；未迁移仿真名单清空（审计保留，守 0） |
| W6e–f | 本地完成 | `dab827d5` `737ef5e9` `922507f8` | 后端表里 verb port 在各模式都覆盖的前端对象入口（`ClearNamedFramebuffer*`、`BlitNamedFramebuffer`、`GetTex(ture)Image`、`MultiDrawElements`）及其实现删；Magma 前端 render pass、pending clear、renderbuffer 半边删。Magma 管线构建只读 draw framebuffer 记录，其后的前端纹理同步删。link ratchet 171 → 49，`# P13` 95 → 9（余下见 HANDOFF §3d，转 W7） |
| W7 | 本地完成 | `9fa61b84` | D12：`SOURCE_FILES` 按 link ratchet 的 PARTITION 拆成 7 个 OBJECT 库（`mg_util` / `mg_pipe` / `mg_backend` / `mg_frontend` / `mg_remote_transport` / `mg_remote_client` / `mg_remote_server`），`MobileGL` 与 `MobileGL_s` 链同一批对象（每个文件只编一次），编译侧使用要求集中在接口目标 `mgl_build_iface`。新目标 `MobileGL_server_linkcheck`（split、Linux）：只链 server 与 shared 模块、`--no-undefined`，基线里每条前端引用经 `scripts/ci/server_link_check.py` 生成的 `--defsym` 具名豁免到一个 abort 桩；基线没列的 server→前端引用在链接前按名拒绝（两个阴性对照已验：少一条基线、少一条豁免都红）。当前 65 条豁免：49 条来自 server 对象（即基线），16 条来自 shared 的入口胶水（`Init` / 全局对象）。`link_ratchet.py` 改为同时扫 `mg_*.dir` |
| `# P13` 出口门 | 本地完成 | `fa77b187` `9987e57f` | 死亡通知带 handle（`StateObjectDeathOps.OnDestroyed(kind, lifetimeId, handle)`），Espryt 按 handle 放孪生（`ReleaseTwinsForWireObjectDeath`，与 server 的 object_death 同一释放），槽由 client 在通知后释放；Espryt 前端键槽表入口（`GetOrCreate(StatePtr)` / `Find(object)` / `HandleOf` 及其记忆 / `Note/StateForHandle` / 寿命 id 死亡遍历）、遗留 map 臂与 GC、注册表拒绝、`MGPipeApplierIsUnbarrieredApply` 与两个空作用域删掉；`InvalidateCompileEnv` 走 `MGPipeNotifyCompileEnvChanged()` 钩子，`HasOpenTransformFeedbackSpan` 改读 applier 的 `StreamOutputSpans`；link ratchet 49 → 36，`--assert-monotone` 拒绝任何 `# P13` 注解；10 个 `DirectGLESSlotTable` 用例改驱 handle 入口，9 个用例退役（记入 ci-cleanup-log） |
| §1.3 残留 | 本地完成 | `de6ab167` | 删遗留 `BufferBackendOps` 表（前端分派、两后端 Ops_*、`BufferObject` Get/SetBackendResource）、Magma 从未填充的缓存（`VkTextureManager` 前端键 map 与记忆、`VkRenderPassManager` 的 render pass / renderbuffer / 待清除 / 追踪附件缓存与驱逐观察者）和 `VkClearManager`；约 30 个 BufferTest mock 用例与 1 个 SplitBufferSet 用例改驱 `MGPipeResourceOps` mock，未退役；ratchet 36 → 27 |
| W9 | 移出 P13 | | 缓存容量重调属性能工作，按用户裁定移到 P13 之后的性能轮（DEBTS.md 对应行） |

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
