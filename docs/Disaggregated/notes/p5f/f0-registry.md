# f0 普查子项：§2.5 registry 重键普查（`MGPipeFrontendKeyedRegistryScope`）

> 基线 `feat/disaggregated @ 8b68b92c`（代码头 `25fba0d5`，其后仅文档提交）。只读普查，未改动任何代码。
> 路径省略前缀 `MobileGL/`。行号均在当前 HEAD 实测。

## 0. 范围与实际 grep 结果

计划 §2.5 写"约 12 个活跃站点（:2542、:4505、:5540、:5659、:5940、:6490、:6576、:7092、:7682、:8213、:11231 …）"。
当前 HEAD 实测（`rg MGPipeFrontendKeyedRegistryScope`，全树）：

- **`MG_Backend/DirectGLES/DirectGLES.cpp`：13 处构造点**（比计划列举多 `:12606`、`:14461`），另有 `:2090` 一处注释记录 vi 家族已删除的 VAO 站点（非构造点）。
- `MG_Backend/DirectGLES/Managers.cpp`：**11 处**（`:3063`、`:4689`、`:6101`、`:8934`、`:9762`、`:9816`、`:10953`、`:10974`、`:14102`、`:14463`、`:14760`）。
- `MG_Test/Wire/RemoteClientTest.cpp:1930`、`:1948`：测试夹具，非生产路径。

本报告主体是 DirectGLES.cpp 的 13 处；Managers.cpp 的 11 处在 §4 作附注（同一豁免家族，超出本子项口径但处置同类）。

## 1. 机制回顾：scope 保护的是什么，P5e id 已经重键了什么

**scope 的语义**（`MG_Impl/Pipe/SlotAllocator.h:250-282`、`:272-282`；实现 `SlotAllocator.cpp:146-157`）：
P5c G6 的具名豁免——豁免的探测是"server 私有、但以**前端身份**键控的 twin registry 查询"。
P5e（id，CONTRACT-P5E §4.4）把豁免条件收窄为**仅当前记录设障**（`MGPipeApplierCurrentRecordIsBarriered()`）：
设障时 client 停在 `WaitForApplied` 里，其分配器与 lifetimeId→slot 映射静止，只读探测无撕裂；
未设障记录下 scope 不豁免任何东西，探测即 `Fatal{RoleViolation, "MGPipeSlots"}`（`SlotAllocator.cpp:32-70`）。
monolith 下守卫首行即返回（`SlotAllocator.cpp:33`），scope 只是标记。

**被保护的"键"是什么**。七张 twin 表全部是 `BackendSlotTable`/`TwinRegistry`（`Managers.h:1579` VAO、`:2118` 纹理、`:2297` FBO、`:3110` 程序、`:3298` 采样器、`:3383` sampler-view、`:3449` renderbuffer）。
P5e（id）之后，**每张表都已按 {slot,gen} 键控**：`FindByHandle` / `GetOrCreate(MGPipeHandle)` / `ReleaseByHandle` 全部存在
（`SlotTables.h:351`、`:461`、`:486`），Gen 检查、composite band、ABA 语义齐备（CONTRACT-P5E §4.2/§4.3）。
scope 站点走的是**前端身份半边**：

- `Find(StateObject*)` ≡ `FindByHandle(HandleOf(stateObj))`（`SlotTables.h:477-480`），
  `HandleOf` = `stateObj->GetLifetimeId()` → `MGPipeSlots().FindByLifetimeId(kind, lifetimeId)`（`SlotTables.h:501-517`）——
  即"前端对象指针 → lifetime-id → client 分配器探测"。这是 **lifetime-id 键**，不是裸地址键；
- `GetOrCreate(const StatePtr&)` 在 miss 时经 `Acquire` **铸造**句柄（`SlotTables.h:263-298` 一带，守卫在 `:298`）；
- legacy-memos 臂（`MOBILEGL_PIPE_LEGACY_MEMOS`）才是裸前端地址键 + weak_ptr owner-equality（`DirectGLES.cpp:75-153`）。

所以每张表"按句柄重键缺什么"的答案**不是缺表的句柄能力**——表早就有了——缺的是**句柄到达该站点**：
要么记录已带句柄但 sink/后端没用它（:2542、:12606），要么该站点整个是 monolith glue，transport 下根本不该到达（其余 11 处）。

**P5e id 已删的**：draw path 上约 20 处 scope 构造点连同其包裹的前端探测（CONTRACT-P5E §4.4 列旧行号；
当前 HEAD 的 `DirectGLES.cpp:2086-2093` 注释记录了 VAO 站点是"删除而非收窄"，删除使回退变响）。
**存活的口径**（CONTRACT-P5E §4.4 末段）：设障行站点（CopyTex / GetTexImage / mipmap-shape /
set_storage_block_binding / detach-walk）、两个 verify 臂、`BufferImpl::HandleOfBuffer`。
注意该列举写于 P5e 基线，当前 HEAD 已有漂移：CopyTex 与 mipmap 已改走句柄（见下），GetTexImage 在 wire 上已是 class-C 具名拒绝（见 :14461 条）。

## 2. 总结论

13 个站点分两类：

- **A 类（P5f 跨进程必需，2 处）**：transport 下真实活着、被设障行保护的前端键探测——
  **`:2542`（resource_copy_region 的 CopyImage 端点同步）** 与 **`:12606`（set_storage_block_binding）**。
  这两处的记录**早已携带句柄**（`MGPCopyRegion::Src/Dst`，`MGPipeTypes.h:1469`；`MGPStorageBlockBinding::ShaderCso`，`:1792`），
  sink/后端没用而已。
- **B 类（P3b/P4b 债 / monolith glue，11 处）**：在受支持的 transport 配置下不可达——或前面已有 transport 具名拒绝/abort
  （:6576、:7682），或臂选择器先行分流到 by-handle 臂（:4505、:5540、:5659、:5940、:6490、:7092），
 或 wire 槽位本身是 class-C 拒绝（:14461），或仅 monolith 路径调用（:11231），或**全树无调用者**（:8213）。
  这些在"拆进程后还成立吗"的判据下**不成立也无妨**，因为它们到达不了 server 进程；
  它们退役的正确归属是 P3b/P4b 的 monolith-glue 清理，而不是 P5f 的 wire 完备性。

## 3. DirectGLES.cpp 逐站点

### A 类：transport 下活着的设障行站点

#### :2542 — `TextureImpl::SyncTextureObjectToBackend(textureObject, imageBindable)`

- **保护的 registry**：`TextureImpl::g_backendTextureObjects`（纹理 twin 表）。
- **今天的键**：`Find(textureObject.get())` / `GetOrCreate(textureObject)`（`:2544-2546`），slot 臂上经 lifetime-id 探测；
  注释自己写明（`:2538-2541`）。
- **对象是否已有 {slot,gen}**：有。纹理的句柄由 `resource_create/respecify` 先行（CONTRACT-P5E §4.2 纹理行）。
- **transport 下谁到达这里**：`DirectGLES::CopyImageSubData` → `MakeGLESCopyImageEndpoint`（`:12301`），
  其端点由 sink `ServerVerbSink::OnResourceCopyRegion`（`MG_Remote/Server/PipeApplier.cpp:856-859`）
  用 **BARRIER_PULLED 的 `gPipeInputs.GetTextureObject(GlName)`** 重建出前端对象再传入。
  `resource_copy_region` 是 `kWaitApplied`（`PipeCalls.def:254`），client 停车，故探测合法。
  （视图嵌套同步 `SyncTextureViewToBackend` 也调它，但 transport 臂已改为 `SyncTextureViewToBackendByRecord` 走 `Desc.ViewOf` 句柄，`Managers.cpp:7178-7195`。）
- **按句柄重键缺什么**：不缺载体——`MGPCopyRegion` 已同时携带 `Src/Dst` 句柄与 `SrcGlName/DstGlName`（`MGPipeTypes.h:1468-1476`）。
  缺：(1) sink 用 `Src/Dst` 句柄而非 `GetTextureObject` 重建端点（顺带退役 FieldOwnership 的 `GetTextureObject` P7 行）；
  (2) `CopyImageEndpoint` 增加句柄形态或后端函数表加 by-handle 入口；(3) `MakeGLESCopyImageEndpoint` 改 `SyncTextureToBackendByHandle`。
- **今天为何能工作**：设障（kWaitApplied）+ scope 豁免，client 停着，lifetime-id 探测读的是静止的 client 分配器。
- **P5f 处置建议**：归 fr 包。这是 §2.1 `GetTextureObject`（P7 行）在 Espryt 侧的**实际消费点**，句柄已在记录里，属纯接线；
  做完后 `:2542` 的 scope 在 transport 下失去最后一个到达者，可随 P3b/P4b 的 monolith-glue 清理一起删。

#### :12606 — `ShaderStorageBlockBinding(program, storageBlockName, binding)`

- **保护的 registry**：`PrgramImpl::g_backendProgramObjects`（程序 twin 表，kind ShaderCso）。
- **今天的键**：sink `OnSetStorageBlockBinding`（`PipeApplier.cpp:910-928`）按 GL 名调后端表项，
  后端先 `MGB_CTX->ValidateProgramName` / `GetProgramObject(program)`（两个 BARRIER_PULLED 行，P9），
  再 `Find(programObject.get())`（`:12608`）→ lifetime-id 探测。
- **对象是否已有 {slot,gen}**：有；且**记录已带**——`MGPStorageBlockBinding::ShaderCso`（`MGPipeTypes.h:1791-1796`）。
  CONTRACT-P5E §2.2 本就写着"trailing: pg resolves through `MGPStorageBlockBinding::ShaderCso` and flips it"，未落。
- **按句柄重键缺什么**：sink/后端按 `ShaderCso` 解析 twin（`ResolveProgramTwin`/`FindShaderCsoRecord` 已在），
  跳过 `GetProgramObject` 全程；该行 `kWaitApplied`（`PipeCalls.def:297`）即可翻 `kWaitNone`。
  注意当前实现的"不同步即返回"语义（`:12614-12617`，link version 落后则留给 reseed）在 by-handle 臂需用
  `m_syncedShaderCsoSerial` vs `record->Serial` 重述。
- **今天为何能工作**：同上——设障 + scope。
- **P5f 处置建议**：归 fr 包（或与 §2.1 的 `GetProgramObject`/`ValidateProgramName` P9 行一起：这两行的 Espryt 消费点主要就是这里）。
  契约已预定、记录已带句柄，是 13 处里**收益最直接**的一处。

### B 类：monolith glue / transport 下不可达（P3b/P4b 债）

#### :4505 — `SyncCurrentFBO()` 的 legacy 循环

- registry：`FramebufferImpl::g_backendFramebufferObjects`；键：前端 FBO 指针（经 `HandleOf`）。
- transport 下：`SyncCurrentFBOByRecord()` 先行（`:4479`），decline 且 fb 位开启时直接
  `RefuseFramebufferBindingSlotRead()` abort（`:4490-4492`）。fb 位关 + transport 的组合里，
  环绕它的 `GetFramebufferBindingSlot` 读本身已是 BARRIER_PULLED 具名红——**非受支持配置**。
- 处置：P3b/P4b 删 monolith glue 时连同 `GetOrCreate(currentFBO)` 臂一起删；P5f 不需要动它。

#### :5540 — `ResolveGlobalConstantsRecord(program)`（monolith-glue 半边）

- registry：`g_backendProgramObjects`；键：`HandleOf(program)`（`:5546`）。
- 注释自带结论（`:5530-5533`）："Reached only when `Transport == Monolith`"；transport 的唯一调用点
  走 `ResolveGlobalConstantsRecordForHandle`（`:7139`）。
- 处置：P3b/P4b。P5f 无需动。

#### :5659 — `SyncCurrentProgram(SharedPtr<ProgramObject>)`

- registry：`g_backendProgramObjects`（另含两张表的 `CollectGarbageIfNeeded`，`:5661-5662`）；键：前端程序指针（`:5679-5680`）。
- transport 下：调用点全部按 `ProgramHandleArm()`（`Managers.h:714-716`）分流到
  `SyncCurrentProgramByHandle`（draw `:6260-6265`，dispatch `:8156-8161`）。
- 处置：P3b/P4b。

#### :5940 — `BindCurrentFBO(target)` 的 legacy 臂

- registry：`g_backendFramebufferObjects`；键：前端 FBO 指针 `Find(currentFBO.get())`（`:5947`）。
- transport + fb 位：记录臂 `FindByHandle(record.Fbo)`（`:5889`）先行返回；decline 则 abort（`:5917-5919`）。
- 处置：P3b/P4b。

#### :6490 — `ResolveAndBindUnitTextures()` 的 legacy 单元绑定 pass

- registry：`g_backendTextureObjects`；键：前端纹理指针 `Find(textureObject.get())`（`:6492`）。
- transport + 纹理位：`UnitTexturesByHandle()` 的记录臂在 `:6397` 返回，走不到这里（`:6341-6398`）。
- 处置：P3b/P4b。该 legacy pass 的外层读 `MGB_CTX->GetTextureUnitObject(unit)`（`:6425`）本身是 §2.1 的
  BARRIER_PULLED 行（Magma 的那半归 fm），scope 只是其中 registry 探测的标记。

#### :6576 — `ResolveUnitSamplerBackend(unit, samplerObject)`

- registry：`SamplerImpl::g_backendSamplerObjects`；键：`HandleOf(samplerObject)`（`:6582`）。
- transport 下：**scope 之前就已 abort**——`Fatal{ProtocolCorruption, "BindSamplerStates.Count"}`（`:6565-6573`），
  P5e（tx2）按 CONTRACT-P5E §4.2 把 identity sampler 家族在 transport 下整个删除。
- 处置：scope 已形同虚设（只为 monolith 编译期保留），随 P3b/P4b 删。

#### :7092 — `BindCurrentProgramWithResources` 的 else 臂

- registry：`g_backendProgramObjects`；键：前端程序指针 + stash 比较（`:7096-7104`）。
- transport 下：`handleArm` 走句柄 stash / `ResolveProgramTwin`（`:7080-7083`）。注释自标"monolith glue only"（`:7088-7091`）。
- 处置：P3b/P4b。

#### :7682 — `GetCurrentBackendProgram()`

- registry：`g_backendProgramObjects`；键：前端程序指针 `Find(currentProgram.get())`（`:7684`）。
- transport 下：`ProgramHandleArm()` 臂（`:7659-7670`）在 scope 之前返回，scope 不可达（注释 `:7680-7681` 自明）。
- 处置：P3b/P4b。

#### :8213 — `GetBackendProgramId(GLuint program)`

- registry：`g_backendProgramObjects`；键：前端程序指针（`:8215-8217`，Find-or-create + 按需 `SyncToBackend`）。
- **特殊发现：当前 HEAD 全树无任何调用者。** 该函数无头文件声明（`DirectGLES.h`、`Managers.h` 均无），
  不在后端函数表（`BackendObject.h` 无此槽），MG_Impl/MG_Remote/MG_Pipe 无引用；唯一同名物是
  `BackendProgramObjectImpl::GetBackendProgramId()` 成员（`Managers.h:2894`）。CONTRACT-P5E §5.5 称它
  "is a monolith entry"，但至少在当前 HEAD 它是**死代码**（唯一旁证：`MG_Test/Program/ProgramInterfaceTest.cpp:1411`
  的注释提到这个名字描述一种既有危害）。
- 处置：先确认删除（或恢复声明与调用者），与 P5f 无关；若保留则归 P3b/P4b。**建议 fr 包顺手删掉它连带这处 scope**——
  这是普查发现的唯一"scope 保护着没人调的函数"。

#### :11231 — `ScopedDetachedTextureFramebufferAttachments(SharedPtr<ITextureObject>)`（detach-walk 前端构造）

- registry：`g_backendTextureObjects`（`Find(texture.get())`，`:11233`）+ `g_backendFramebufferObjects.ForEachLive`
  内经 `StateForHandle` 取回前端 FBO 对象（注释 `:11243-11255`）。
- transport 下：唯一 transport 调用者是 `GenerateMipmapByRecord` 的**句柄构造**（`:12076`，走 `:11162` 的
  `MGPipeHandle` 重载，纯记录走查）；前端构造只被 monolith 的 `GenerateMipmap` 调用（`:12151`），
  而 transport 的 `GenerateMipmap` 在 `:12105-12117` 已整臂分流到 `VerbMipRes`。
- 处置：P3b/P4b（fb 包的 reverse index 已是句柄臂的替代）。注意 `StateForHandle` 这半边由
  `MGPipeRefuseFrontendKeyedRegistryFromUnbarrieredApply` 单独把守（`SlotAllocator.cpp:76-86`），
  scope 删后该守卫仍在。

#### :14461 — `GetTexImage(target, level, format, type, pixels)`

- registry：`g_backendTextureObjects`；键：前端纹理指针（`:14463`），其前端对象来自
  `GetActiveTextureUnit`/`GetTextureUnitObject`/`GetBindingSlot` 三连 BARRIER_PULLED 读（`:14448-14453`）。
- transport 下：**不可达**——wire 槽 `GetTexImage` 在 `MGR_UNMIGRATED_TAIL_SLOTS`（`EmitTables.cpp:1673`），
  即 class-C 具名拒绝 `Fatal{UnmigratedVerb, "GetTexImage"}`；DSA 的 `GetTextureImage` 由前端自己的 CPU 读回
  应答（`SlotCaps.h:57`），其后端槽同样在拒绝名单（`EmitTables.cpp:1674-1676`）。
  即 CONTRACT-P5E §4.4 当年列举的"GetTexImage 设障行站点"在当前 HEAD 已漂移成 class-C。
- 处置：本分两个阶段看——P9 若把 GetTexImage 接上 wire，`MGPReadbackInfo::Res`（`MGPipeTypes.h:1555`）已带句柄，
  届时按 :2542 同款模式重键；在那之前此 scope 仅 monolith 可达，归 P3b/P4b。**当前不属于 P5f 跨进程必需。**

## 4. 附注：Managers.cpp 的 11 处（同一豁免家族，本子项口径外）

均为 G6 注释标记的同类探测，除注明外均为 monolith-glue 性质：

| 行 | 函数 | registry / 键 |
|---|---|---|
| `:3063` | `BufferImpl::HandleOfBuffer` | buffer twin 表；lifetime-id。CONTRACT-P5E §4.4 点名的残留（"until vi/sb carry the handle"）；当前调用者在 monolith 臂（`DirectGLES.cpp:1076`、`:1366`） |
| `:4689` | `SamplerViewImpl::HandleOfSamplerViewForTexture` | sampler-view 表；lifetime-id |
| `:6101` | 纹理 remint-pull 的 rearm（`RequireImageBindableStorage` 族） | 纹理表 `HandleOf` |
| `:8934` | `BackendTextureObject::ResolveOwnRecord` | 纹理表 `HandleOf`；by-handle 臂靠 `m_pushedSyncHandle` 不探测 |
| `:9762` / `:9816` | `SyncAttachmentObject` | 纹理 / renderbuffer 表 Find-or-Create；legacy FBO 附件走查 |
| `:10953` / `:10974` | FBO verify 臂（`glGetFramebufferAttachmentParameteriv` 断言走查） | 纹理 / renderbuffer 表 Find；debug 断言代码 |
| `:14102` | 程序 `SyncToBackend` 的 serial 盖章 | 程序表 `HandleOf`；注释自标 "MONOLITH GLUE ONLY" |
| `:14463` | 采样器参数同步的"未注册"判别 | 采样器表 `HandleOf`；后端自产采样器（raw-depth-fetch）靠它区分 |
| `:14760` | renderbuffer `SyncToBackend` 的记录解析 | renderbuffer 表 `HandleOf`；注释自标 monolith glue |

建议 fr 包把 Managers.cpp 这 11 处与 DirectGLES.cpp 的 B 类一起列入"随 P3b/P4b 删除"清单，避免 P5f 范围漂移。

## 5. 对 P5f（fr 包）的处置清单（汇总）

1. **fr 的真工作只有两处接线**：`:2542`（resource_copy_region 改吃 `MGPCopyRegion::Src/Dst` 句柄，
   顺带退役 `GetTextureObject` 拉取）与 `:12606`（set_storage_block_binding 改吃 `MGPStorageBlockBinding::ShaderCso`，
   顺带退役 `GetProgramObject`/`ValidateProgramName` 拉取，WaitClass 可翻 `kWaitNone`）。
   两者记录都已带句柄，registry 都已是句柄键——**是接线，不是重键**。
2. `:8213`（`GetBackendProgramId`）疑似死代码，建议确认后随 fr 删除。
3. 其余 11 处（DirectGLES.cpp）+ 11 处（Managers.cpp）是 monolith glue / P3b/P4b 债，
   **不应计入 P5f 的跨进程判据**；双块演练（§4）不会让它们变红，因为它们根本不被 transport 到达。
   计划的 fr 包若按"删全部 scope"来写会虚胖；按本普查，fr 的 red-once 判据应落在 A 类两处。
4. 口径修正建议：P5F-WIRE-COMPLETENESS.md §2.5 写"registry 仍按前端地址 / lifetime-id 键控"——
   实测 **registry 本体已全部按 {slot,gen} 键控**（P5e id 完成），残存的是"调用点仍用前端身份解析"，
   且 13 处中 11 处在 transport 下不可达。建议把 §2.5 措辞改为"两个设障行调用点仍经前端身份解析"。

## 6. 未知项（如实记录）

- `:2542` 的 `SyncTextureObjectToBackend` 在 transport 下是否还有 CopyImageSubData 之外的到达者，
  本普查核对了其全部 8 个调用点（`DirectGLES.cpp:3323`、`:3458`、`:3543`、`:3852`、`:10363-10364`、
  `:12131`、`:12134`、`:12301`，加 `Managers.cpp:7104`）：其余均在 monolith/legacy 臂或被
  `FramebufferRecordArmIsMandatory` 之类的分流挡住；若臂选择器被操作员改出非默认组合，可达性会变化——未逐组合穷举。
- `:8213` "无调用者"的结论基于全树文本检索；若存在经函数指针表以外的动态到达（如 dlopen 符号），未能覆盖——未发现任何迹象。
