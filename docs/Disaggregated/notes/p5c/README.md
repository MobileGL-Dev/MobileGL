# P5c — `inproc` 共享内存读点归零（`11ac3de6..b88e8487`）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 起因：对 `a79a0af6` 的只读审计（[`p5c-audit-v1.md`](p5c-audit-v1.md)，59 行）证明 P5 / P5b 的 `inproc` 仍有一批不经任何载体、只靠 verb barrier 与同一地址空间才正确的直接读写。P5c 让 `inproc` 成为只经 wire 交换的诚实两角色，从而 P6 只是传输替换。
- 落地：server 端纹理 staged shadow（`StagedTextureStore`）；`SEG_EVENT` 三个 producer + `kEventGlError`（反向通道零裸指针）；sink / twin 按记录句柄解析；`applier_reset` / `object_death` 控制记录（opcode 77 / 78）；`set_context_values`（79）退役值类 BARRIER-PULLED 行；双层 `Fatal{RoleViolation}` 守卫；两个具名豁免 scope 承接未到期的债。契约 `MobileGL/MG_Remote/CONTRACT-P5C.md`。
- 门：守卫开启下 unit 2187/2187、`integration-split` 111/111，每层守卫 red-once；纹理 `0xDD` audit 零 Fatal；G1 三个认定 resize + `.text` −16 B。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P5c** `inproc` 共享内存读点归零
- **状态**：✅ `11ac3de6..b88e8487`
- **落地什么 / 范围**：server 端纹理 staged shadow（`StagedTextureStore`，句柄为键、整段覆盖、defined-ness 追踪）；`SEG_EVENT` 三个 producer + `kEventGlError`（反向通道零裸指针）；sink / twin 按记录句柄解析（blit 具名臂经 verb 句柄工作区，Magma 走 G6 消费臂）；`applier_reset` / `object_death` 控制记录（opcode 77/78，framebuffer 首个 wire delete）；`set_context_values`（79）退役全部值类 BARRIER-PULLED 行，三 shutter 自答；双层 `Fatal{RoleViolation}` 守卫 + `InBarrierWait` 接线；两个具名豁免 scope（通告家族 P4b/P7、G6 registry 家族 P3b/P4b）承接未到期的债
- **验收门 / 证据**：角色守卫开启下 unit 2187/2187（strict 下同绿）、`integration-split` 111/111、每层守卫 red-once 按名变红；纹理 `0xDD` audit 在 bsl / complementary 两条 in-world trace 零 Fatal；rsp 按帧实测（bsl 948.5/帧，残留全为钉住的对象类 15 行）；G1 三个认定 resize（SwapchainObject::Create / CopyTexSubImage2D / ScopedRestartIndexSubstitution，均已具名）+ `.text` −16 B、pull 构建零 MG_Remote 符号；G5 保护区字节一致；双生成器 --check/--self-test 绿

## 历史：P5c 计划与审计（`inproc` 共享内存读点；审计头 `a79a0af6`，2026-09-17）

**为什么插在 P6 之前**：P5 / P5b 的 `inproc` 只把 verb 记录、`SEG_STAGE` blob、reply 与 caps 快照做成了 wire。对 `a79a0af6` 的只读静态审计（`~/w7/notes/p5c/p5c-audit-v1.md`，59 行清单，逐条在该头上重新解析）证明两个角色之间仍有一批**不经任何载体的直接内存读写**，靠 verb barrier 与同一地址空间才正确。它们不是传输问题：`spawn` 换掉的只是 ring 的传递方式，而这些读写在另一个进程里根本没有对应内存。P5c 的目标是让 `inproc` 诚实——两线程之间除 `SEG_CMD` / `SEG_STAGE` / `SEG_REPLY` / `SEG_EVENT` / caps 快照 / 控制帧之外零直接访问——从而 P6 只是传输替换。**本节保留 P5c 启动时的计划与审计语境；P5c 已收官，后续对象类、控制帧、静态量与反向通道的收口结果以 P5f 报告为准。**

### 审计清单（按家族；`file:line` 以 `a79a0af6` 为准，完整 59 行见报告）

| # | 方向 | 直接访问的是什么 | 站点 | 今天为何能工作 | 现有守卫 | P5c 载体 |
|---|---|---|---|---|---|---|
| T1 | S→C 读 | **纹理纹素**：`resource_subdata` 已把 level 字节放进 `SEG_STAGE`，但 `ApplyTextureUpload` 只做门与累积、把指针丢掉，Espryt 同步时从 client 的 `MipmapStorage` 重读纹素；纹理对象的形状与脏区也直接读前端对象 | 丢指针 `MobileGL/MG_Pipe/PipeApply.cpp:989-1008`；重读 `DirectGLES/Managers.cpp:7315`（`MapMipmapData`）、`:6940`、`:7129`、`:7198`；形状 `:7314`、`:7332`、`:6878-6880`、`DirectGLES/DirectGLES.cpp:8502-8507`、`:8528-8529`；脏区 `DirectGLES/Managers.cpp:7360`、`:7371` | barrier + 同地址空间 | **无**：整个纹理家族不在 `FieldOwnership.def`，`rsp` / `MOBILEGL_IPC_STRICT_ERRORS` / `MOBILEGL_IPC_AUDIT` 都看不见 | server 端纹理 staged shadow（buffer `StagedShadowStore` 的纹理半边）；`SyncMipmapsToBackend` 改读它，形状与脏区读描述符 |
| T2 | S→C 读写 | client 的 slot 分配器 `MGPipeSlots()`：mip / CopyTex / twin 创建时按前端 `GetLifetimeId()` 在 client 分配器里查找甚至铸造 | `DirectGLES/DirectGLES.cpp:8087` → `DirectGLES/SlotTables.h:392-400`（`HandleOf` → `FindByLifetimeId`）；铸造 `DirectGLES/SlotTables.h:232`（`GetOrCreate`）、`:418`、`:428`；`DirectGLES/Managers.cpp:2708`、`:4293`、`:5627`；`DirectGLES/DirectGLES.cpp:6706`、`:8185`、`:8564`、`:8659` | barrier；mip 只 Find 不 mint | 无（`MOBILEGL_ASSERT` 在 INFO 构建失效） | 记录已带句柄（`MGPMipPlan::Res`、`MGPCopyFromFramebuffer::Dst`、`MGPBlit::ReadFbo/DrawFbo`）→ `GetOrCreate(MGPipeHandle)`（`DirectGLES/SlotTables.h:278`） |
| T3 | S→C 读 | 具名 blit：client 临时改绑自己的 read / draw 绑定槽，server 经 `MGB_CTX` 读绑定槽 | client `MobileGL/MG_Remote/Client/EmitTables.cpp:741-786`；server `DirectGLES/DirectGLES.cpp:7782-7785` | barrier + RAII 恢复 | `rsp`（`GetFramebufferBindingSlot`） | 记录里的 `ReadFbo` / `DrawFbo`，sink 侧按句柄解析（`MobileGL/MG_Remote/Server/PipeApplier.cpp:228-244`） |
| T4 | S→C 读 | `CopyTexImage2D` / `CopyTexSubImage2D` 经纹理单元绑定槽取目的纹理 | `DirectGLES/DirectGLES.cpp:8561-8563`、`:8656-8658` | barrier | `rsp`（`GetTextureUnitObject`） | 记录里的 `Dst` 句柄 |
| T5 | S→C 写 | Magma 生成 mip 直接写 client 的 level 存储（Espryt 的重推导臂没有 Magma 对应） | `DirectVulkan/Renderer/VulkanRenderer.cpp:1562-1590`（调用 `:11317`） | barrier | 无 | 与 GLES 同形：只按描述符验证 |
| B1 | S→C 读 | `BufferObject::HasDefinedContent()`，每个 ensure 的 draw 都读 | `DirectGLES/Managers.cpp:3147-3148`（`EnsureBufferResourceForHandle`；`:3141-3146` 自述为债） | barrier | 无 | 描述符旁的 live-content 位，client 发布 |
| B2 | S→C 读 | `HandleOfBuffer` 每次 ensure 读前端 `GetLifetimeId()` | `DirectGLES/Managers.cpp:2705-2716` | barrier + memo | 无 | 记录里的句柄 |
| B3 | S→C 读 | 旧 buffer 臂（`MappedData()` / `IsMapped()` / `GetChangeSerial()`）在 split 构建里位 7 清零时可达，arm resolver 只打一行 `MGLOG_D` | `DirectGLES/Managers.cpp:2586-2620`；臂体 `:1012-1022`、`:1119-1121`、`:2944-2949`、`:3281-3327` | 同进程 | 无 | 有传输时具名拒绝（同 `Fatal{PipeLegacyMemosDisabled}` 的形状） |
| B4 | S→C 读写 | XFB：`SharedPtr<BufferObject>` 跨整个 span 持有，结束时 `WritebackFromBackend`，scatter 对 client shadow 读改写 | `DirectGLES/DirectGLES.cpp:950`、`:1060`、`:1077`、`:1081`、`:1172`、`:1185` | barrier | 无 | `OnBufferWriteback` / `OnXfbScatterReady` 事件——P9 的题材，P5c 只记账 |
| R1 | S→C 写 | 反向通道是 apply 线程直接调进 client 的 `MG_State`：`OnBufferWriteback` 把**裸指针**塞进 `MGPBlobRef.Offset`，client 侧再转回指针；consumer 会拒绝任何真实段 | 生产 `DirectGLES/Managers.cpp:2339-2343`（`Ops_H_Readback`）；消费 `MobileGL/MG_Impl/Pipe/ResourceTracker.h:553-572`（拒绝真实段 `:560-564`） | 同地址空间 + barrier | 无 | `SEG_EVENT` `kEventBufferWriteback`：consumer 已在（`MobileGL/MG_Remote/Client/ClientSession.cpp:176-267`），**全仓零 producer**（`MobileGL/MG_Remote/Server/ServerSession.cpp:484-492` 无调用者） |
| R2 | S→C 写 | `OnGpuWritten` 直接调进 client tracker；Magma 更是绕过回调直接 `MarkGpuWritten()` | Espryt `DirectGLES/Managers.cpp:2729-2752`，喂入 `DirectGLES/DirectGLES.cpp:568`、`:616`、`:2601`；Magma `DirectVulkan/Renderer/UniformManager.cpp:1075`、`:1231`、`DirectVulkan/Renderer/VulkanRenderer.cpp:11618` | 同地址空间 | `MGLOG_E_ONCE` | `SEG_EVENT` `kEventGpuWritten`（consumer `MobileGL/MG_Remote/Client/ClientSession.cpp:227-236`） |
| R3 | S→C 写 | 默认 framebuffer 附件（`pDefaultFramebufferInfo`）由 server 在 surface 创建时写 | `DirectGLES/DirectGLES.cpp:11519-11620`；Magma `DirectVulkan/Renderer/SwapchainObject.cpp:276-335` | barrier；罕见 | 无 | `SEG_EVENT` `kEventSurfaceChanged`（consumer stub `MobileGL/MG_Remote/Client/ClientSession.cpp:242-253`） |
| R4 | S→C 写 | sticky forward：`RecordError` 写 client 错误队列；`InvalidateCompileEnv` 写 client 编译环境（在 stamped verb 之外静默 no-op） | `MobileGL/MG_Impl/Pipe/PipeFill.cpp:1753-1756`、`:1764-1772` | barrier | `rsp`；strict 下 Fatal | `RecordError` → `OnGlError` 事件（顺序保证是 P9 的）；`InvalidateCompileEnv` 的活已由 R-12 caps 重发布完成（`MobileGL/MG_Remote/Client/CapsMirror.cpp:78-80`），删 forward |
| R5 | — | `MGPipeCallbacks` 十个回调只装了两个，且都是 apply 线程内联调用；`SEG_EVENT` 段已创建、映射、握手公告、每个 reply verb 后排空，但没有任何生产者 | 安装 `MobileGL/MG_Impl/Pipe/ResourceTracker.h:606-613`；段 `MobileGL/MG_Remote/Server/ServerSession.cpp:285-301`；排空 `MobileGL/MG_Remote/Client/ClientSession.cpp:836` | — | 溢出 latch 与 `eventDropped` 已建未用 | 本身就是载体 |
| C1 | S→C 读 | Magma 四处直接读 client 镜像 `pActiveBackendObject->GetDynamicParameters()`（Espryt 已改走 server 自己的 backend）；Espryt 的格式表回落臂在 server backend 为空时读 client 镜像 | `DirectVulkan/DirectVulkan.cpp:713-715`；`DirectVulkan/Renderer/VulkanRenderer.cpp:667-676`；`DirectVulkan/Renderer/VertexInputStateFactory.cpp:249-250`、`:502-503`；`DirectGLES/Utils.cpp:50` | 镜像值等于快照 | 无 | server 自己的 `Backend()->GetDynamicParameters()`；回落臂拒绝 |
| G1 | shared | `gPipeInputs`：client 残余填充写、applier 写、server 的 36 行 BARRIER-PULLED 读 | `MobileGL/MG_Backend/MGPipe/PipeInputs.h:806`；填充 `MobileGL/MG_Impl/Pipe/PipeFill.cpp:2798`；applier `MobileGL/MG_Pipe/PipeApply.cpp:1415-1560`；stamp `MobileGL/MG_Backend/MGPipe/PipeInputs.cpp:103-132`；`rsp` `MobileGL/MG_Backend/MGPipe/PipeInputs.cpp:72` | **只靠 verb barrier** 保证单写者 | `rsp` + strict Fatal + poison | 值类行（`GetActiveTextureUnit`、`GetMaxTouchedTextureUnit`、`GetTouchedBufferBindingPointCount`、`GetCurrentVertexAttribute`、XFB 六项、三个世代）→ 每 verb 一条残余值记录或按 `Coverage.def` 由 server 自答；**对象类行**（`GetBoundVertexArray`、`GetProgramForDraw/Dispatch`、`GetTextureUnitObject`、`GetImageTextureBinding`、`GetFramebufferBindingSlot`、`GetBufferBindingSlot/Point`、sticky `GetTextureObject` / `GetProgramObject`）返回前端对象指针、无法序列化，留 P3b/P4b/P7/P8 的 twin 表 |
| G2 | C→S 写 | GL 线程在 `FreshlyPrimed` 时整体 `MGPipeApplierReset()` server 的 `g_applier` | `MobileGL/MG_Impl/Pipe/PipeFill.cpp:2654-2656` → `MobileGL/MG_Pipe/PipeApply.cpp:1202-1299` | barrier 的后置条件（validate 时无在飞记录） | 无 | make-current 边上的 `applier_reset` 控制记录 |
| G3 | C→S 写 | 六种前端对象死亡：栈上结构体指针经 mailbox 送到 apply 线程（framebuffer 没有 wire delete opcode） | `DirectGLES/Managers.cpp:206-228` | 阻塞 + 同地址空间 | 无 | `object_death` wire 记录 |
| G4 | C→S 控制 | 十二个 `Server*` EGL forwarder 经 one-slot mailbox 传**函数指针 + `void*` 栈局部** | `MobileGL/MG_Remote/Client/BackendObject_Remote.cpp:125-264`；邮箱本体在 fc 前的 HEAD 上是 `ServerLoop.cpp:533-568`（PumpControlRequest）+ `:645-707`（RunOnApplyThread）——本行原引 `:543-598`，f0-egl F9 修正 | 阻塞 | `Fatal{ApplyThreadNotRunning}` | **已由 P5f 包 fc 帧化**（原记 P6）：`SurfaceControlFrame` 纯值帧走同一阻塞通道（`Server/SurfaceControlFrame.h`、分发 `ApplySurfaceControlFrame`），wire 编解码与具名拒绝在 `Protocol/SurfaceOpCodec.cpp`；报告 `notes/p5f/fc-report.md` |
| G5 | C→S 读 | `ServerLoop::OnApplyThread()` 每条 routed 记录读；bring-up 直接读 `loop.Backend()` / `MGPipeGetResourceOps()` | `MobileGL/MG_Remote/Client/WireTables.cpp:71`；`MobileGL/MG_Backend/Init.cpp:100`、`:145`、`:164-170`、`:247` | 同进程 | — | client 本地角色标志；caps 快照的 cap 位（P6 顺手） |
| G6 | shared | 前端地址 / lifetime-id 键控的 Espryt twin registry；持前端裸指针的 memo（`ResolvedDrawBuffers::Entry::frontend`、三张 texture sync list）；server 读 `pDefaultFramebufferInfo`；`g_rawDepthFetchSamplerState`；`ScopedDefaultUnpackState::s_synced` 从不按 context 世代重置 | `DirectGLES/Managers.h:309-377`、`:1185`；`DirectGLES/DirectGLES.cpp:61-62`、`:1958-1987`、`:2846`；`DirectGLES/Managers.cpp:5806-5812` | barrier / server 私有 | — | 历史移交给 P3b/P4b / P6；transport registry 与 `s_synced` 世代问题已由 P5f fr / fs 收口 |
| A1 | — | barrier 的 client 半边断言从未接线：`InBarrierWait()` 零调用点，只有 `ApplyThreadIsInsideApplier` 在 emit 时检查 | `MobileGL/MG_Remote/Client/ClientSession.cpp:880`、`:748-753` | — | — | 接上，或删掉契约里的这句声明 |

已确认 wire-clean、不进 P5c：buffer 全家族（`Ops_H_*` 只拿句柄 + 记录 + `SEG_STAGE` 字节，`StagedShadowStore::Adopt` 复制，`Fatal{StageSnapshotTooNarrow}` 六处）；`resource_respecify` 初始字节双向拒绝；persistent map 仿真档按块过 `SEG_STAGE`，`Fatal{RoleViolation}` 挡住 apply 线程；`CreateShaderState` 归档是一个 blob；caps 的格式表与 renderer 字符串走 `CapsSnapshot` 控制帧（`GetCaps` 记录本身已死，只是卫生项）；tight ReadPixels；`DrawVbo` 用户索引；fence 家族；`gMGPipeSegmentResolver` 与 `gBackendFunctionsTable` 的角色划分。

### 包与顺序

| 包 | 内容 | 关闭的行 |
|---|---|---|
| **c0c** 契约 | `MG_Remote/CONTRACT-P5C.md`：纹理 staged shadow 的所有权与覆盖规则；`SEG_EVENT` 记录与 blobref 约定（`Seg = kSegEvent`）；sink / twin 按句柄解析的规则；`applier_reset` / `object_death` 记录；角色守卫的语义（有传输时 apply 线程触前端对象表面、GL 线程触 server 状态表面都是 `Fatal{RoleViolation}`）；残余值记录的字段表；把纹理家族补进 `FieldOwnership.def` | — |
| **tx** 纹理 | `StagedTextureStore`：`ApplyTextureUpload` 采纳 `SEG_STAGE` 字节到 server 侧 level shadow；`SyncMipmapsToBackend`、形状读、脏区改读描述符与 shadow；Magma mip 改按描述符；`MOBILEGL_IPC_AUDIT` 的 `0xDD` 覆盖纹理 | T1、T5（T2 的 mip 半边随之） |
| **ev** 反向通道 | `ServerSession::PublishEvents` 接三个 producer（writeback / gpu-written / surface-changed），consumer 的段臂改为解析 `kSegEvent`；Magma `MarkGpuWritten` 改走回调；`RecordError` 进事件（顺序仍 P9）；删 `InvalidateCompileEnv` forward；溢出策略沿用 `ARCHITECTURE.md` §11.7 | R1–R5 |
| **hd** 句柄解析 | blit / mip / CopyTex / `HandleOfBuffer` 的 sink 与 twin 按记录里的句柄解析（`GetOrCreate(MGPipeHandle)`），server 不再触 `MGPipeSlots()`；`HasDefinedContent` 改读描述符的 live-content 位；Magma 四处 caps 读改 server backend，`Utils.cpp` 回落臂拒绝 | T2、T3、T4、B1、B2、C1 |
| **ct** 控制记录 | `applier_reset` 与 `object_death` 两条 wire 记录（framebuffer 首次有 delete opcode）；mailbox 只剩 EGL forwarder 给 P6 | G2、G3 |
| **rv** 残余值 | 值类 BARRIER-PULLED 行改为每 verb 的残余值记录（或 server 自答），`gen_pipe_field_ownership.py` 把它们改成 RECORD-SUPPLIED；对象类行保持 BARRIER-PULLED 并逐行标注归属阶段 | G1 的值类半边 |
| **gt** 门 | 角色守卫（两层 `Fatal{RoleViolation}`，仅 split 构建、有传输时生效）；`InBarrierWait` 接线；旧 buffer 臂传输拒绝；`rsp` 按帧进 stats 行并在四条 A/B trace 上测得；CI 加一条 `MOBILEGL_IPC_STRICT_ERRORS=1` + 角色守卫的 `integration-split` 车道 | A1、B3，以及所有行的红一次证据 |

顺序：c0c → tx / ev / hd 并行 → ct / rv → gt 收口。每包自带 red-once（R-16），阶段末一次 Codex / Kimi 审查（ID-66）。

### 出口门（E-P5c）

1. **角色守卫下全绿**：守卫开启时 `integration-split`（107）、broad inproc 车道对 `348d22a4` 普查零回退、79 trace 无新增首阻塞；关掉任一层守卫必须能在一条具名用例上变红。
2. **纹理字节不再回读 client**：`MOBILEGL_IPC_AUDIT=1` 对纹理 staged 字节的 `0xDD` 填充在四条 A/B trace 上无失败；把采纳改回丢指针必须变红。
3. **`SEG_EVENT` 有生产者**：三条事件的往返单元与集成用例；排空点上 `eventDropped == 0`；把 producer 改回裸指针必须变红。
4. **`rsp` 分类完成**：四条 A/B trace 每帧 `rsp` 已测；剩余读全部是对象类行，名单由 `FieldOwnershipTest` 钉住，值类行为 0。
5. **Redmi 四臂复测**（记录，不设门）：writeback / gpu-written 改走事件后 barrier tax 重新记录。
6. G1 0/0/0/0、G2 / G14、G5 照旧。

### 历史移交项（P5c 当时不碰）

P5c 当时移交的对象类 BARRIER-PULLED、transport registry 前端键、EGL 控制面、`s_synced` / `g_syncedRenderStateParameters` 世代与反向通道跨角色访问，已在 P5f 收口。P9 的异步化/排序、P8/P9 的协议广度与仿真功能仍按各阶段推进；不要把这份历史清单重新当成 P6 的帧化或静态量施工单。

## 实测：P5c（`11ac3de6..b88e8487`，同日收官）（原 `MEASUREMENTS.md` §8）

出口门（E-P5c）实测：

| 门 | 结果 |
|---|---|
| 守卫开启下 unit | **2187/2187**（strict `MOBILEGL_IPC_STRICT_ERRORS=1` 下同绿） |
| 守卫开启下 `integration-split` | **111/111**（107 + ct 的 4 条 CtWireScenario，全真跑） |
| 每层守卫 red-once | layer-1（MipmapStorage 守卫短路 → `TextureMapMipmapDataFromTheApplyThreadIsFatalByName` 红）与 layer-2（`RefusePipeInputsTouchWhileApplierOwnsIt` 短路 → `ClientPipeInputsFillWhileTheApplierOwnsItIsFatalByName` 红）各红一次并复原；tx 的采纳改回丢指针 → 恰 `StagedTextureProductionTest` 红；ev 的 writeback 改回裸指针 → `AtomicCounterScenario.SubDataAfterDispatchSurvivesAnImmediateReadback` 按名红；ct 的发射短路 → Ct 4/4 按名红；rv 的发射门短路 → 7 条按名红 |
| 纹理 `0xDD` audit | bsl in-world（100000 调用）与 iris-complementary in-world 全程零 Fatal |
| `SEG_EVENT` 往返 | 三种事件 + `kEventGlError` 各有单元往返；排空点 `eventDropped == 0` |
| `rsp` 分类 | 值类 = 0（rv 的 FieldOwnershipTest 钉住）；按帧实测：bsl 948.5/帧（38.7/draw，123 帧）、complementary 1591.9/帧（151 帧）、iterationrp 286.2/帧（108 帧后止于既有具名 `texture-remint-pull`）；残留全为对象类 15 行 |
| 普查（integration-gpu @ inproc，对同机 11ac3de6 基线逐名比对） | 基线 253 红 / 收官头 423 红；183 条 newly-failing 中 11 条为三个真回归（默认 FBO 格式时序、大 writeback 溢出、XFB scatter 读前端）——已修并回归绿；其余 172 条全部带 Fatal 名证据归类为设计红（传输下旧臂具名拒绝的对照车道 14 条 + Magma P7 未迁移面 156 条）与 2 条 DirectVulkan CtWire（改按名 skip：Magma 无 object_death 生产者，P7） |
| G1（pull 符号恒等，`11ac3de6` ↔ 收官头，CI 同款 sym 构建） | 0 增 / 0 删 / **3 认定 resize** / 0 重命名，`.text` −16 B：`SwapchainObject::Create`（ev 表面事件化）、`CopyTexSubImage2D`（hd 传输臂）、`ScopedRestartIndexSubstitution`（server-shadow 臂）；pull 构建零 `MG_Remote` 符号 |
| G5（p3a / p4a 保护区） | 字节一致 |
| 生成器 / 卫生门 | 双生成器 --check/--self-test 绿；doc 引用、include 闭包、dirty-surface 绿 |
| Redmi 四臂复测 | **未做**（记录项，需设备窗口；barrier tax 重测随之） |
| 79 trace 普查 | **未重跑**（全集语料不在本机；P5b 的 72/6/1 仍以其头为准） |

审计的 59 处直接访问的终态：纹理纹素 / 形状 / 脏区改读 server staged shadow（tx）；反向通道四条事件（ev）；句柄解析全部按记录（hd）；`applier_reset` / `object_death` 上 wire（ct）；值类残余读清零（rv）；双层角色守卫 + `InBarrierWait`（gt）。**未到期的具名债**：`MGPipeReverseAnnouncementScope`（绑定记录 P4b 才发射的 ensure/通告族）与 `MGPipeFrontendKeyedRegistryScope`（G6 前端键 twin registry，P3b/P4b 重键）两个 scope 内的只读探测，以及对象类 15 行 BARRIER-PULLED——全部具名、可 grep、有退役阶段。

## 落地形状（原 `ARCHITECTURE.md` §17.6）

### 17.6 `inproc` 仍经共享地址空间的访问，与 P5c 的形状（已落地，头 `b88e8487`）

P5 / P5b 的 wire 只覆盖 verb 记录、`SEG_STAGE` blob、reply 与 caps 快照。对 `a79a0af6` 的只读静态审计（`ROADMAP.md` "P5c 计划"，报告 `~/w7/notes/p5c/p5c-audit-v1.md`）列出 59 处仍靠 verb barrier 与同一地址空间才正确的直接访问，最重的四类：**纹理纹素**虽已过 `SEG_STAGE` 但 applier 丢掉指针、Espryt 回读 client 的 `MipmapStorage`（整个纹理家族不在 `FieldOwnership.def`，`rsp` / strict / audit 都看不见）；**反向通道**是 apply 线程直接调进 client `MG_State`（`OnBufferWriteback` 传裸指针，`SEG_EVENT` 已铺好但零 producer，十个回调只装了两个）；**server 用前端 `GetLifetimeId()` 去 client 的 slot 分配器查找甚至铸造句柄**，而记录里其实已带句柄；**Magma** 直接读 client 的 caps 镜像、直接 `MarkGpuWritten`、直接写 client 的 mip 存储。

P5c 的设计决定（六条全部落地，落地形状与偏差以 `MobileGL/MG_Remote/CONTRACT-P5C.md` 为最新权威）：(1) **server 端纹理 staged shadow**——`ApplyTextureUpload` 采纳 `SEG_STAGE` 字节，`SyncMipmapsToBackend` 只读它与描述符，`0xDD` audit 因此覆盖纹理；(2) **`SEG_EVENT` 成为唯一反向通道**——`OnBufferWriteback` 的 `MGPBlobRef` 约定 `Seg = kSegEvent`，`OnGpuWritten` / `OnSurfaceChanged` 走同一 ring，Magma 与 Espryt 共用回调，溢出策略按 §11.7；(3) **sink 与 twin 按记录里的句柄解析**（`GetOrCreate(MGPipeHandle)`），server 永不触 `MGPipeSlots()`；(4) **两条控制记录** `applier_reset`（make-current 边）与 `object_death`（framebuffer 首次有 delete opcode），mailbox 只剩 EGL forwarder 给 P6；(5) **值类 BARRIER-PULLED 行改为每 verb 残余值记录或 server 自答**，对象类行保持 barrier 直到 twin 表落地（P3b/P4b、P7、P8）；(6) **角色守卫**——split 构建有传输时，apply 线程触前端对象表面、GL 线程触 server 状态表面都是 `Fatal{RoleViolation}`；这是 P5c 的出口门，也是 P6 只做传输替换的前提。

落地时的三条结构性补充（契约 §3.1 与 §5.4 的具名豁免）：审计漏了三族无句柄站点——绑定记录 P4b 才发射的 ensure/通告族（`MGPipeReverseAnnouncementScope`）、G6 前端键 twin registry（`MGPipeFrontendKeyedRegistryScope`，P3b/P4b 重键）、Magma 拆除期的隐藏资源；两个 scope 内的只读探测是具名、可 grep、带退役阶段的债，不是守卫的洞。

## 本目录

| 文件 | 内容 |
|---|---|
| [`BRIEF-p5c-audit.md`](BRIEF-p5c-audit.md) | Brief: p5c-audit — where the `inproc` split still reads the other role's memory |
| [`p5c-audit-v1.md`](p5c-audit-v1.md) | p5c-audit-v1 — where the `inproc` split still reads the other role's memory |
