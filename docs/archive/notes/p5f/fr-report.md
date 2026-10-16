# P5f / fr — registry 身份边收口

> 分支 `codex/p5f-fr`，基线 `4667e13b`。工作树 `MobileGL-p5f-fr`；
> 独立 Linux 验证树 `/home/swung/w7/p5f-fr`。这是波次 3 的 fr 包，
> fe/fm 已迁移的 CopyImage 和 storage-block 接线不重复计数。

## 1. 结果与边界

所有 transport apply 对 client allocator 的访问都具名拒绝，设障、后端种类和历史
scope 均不再豁免。`MGPipeSlots()` 本身新增守卫，覆盖旧三方法守卫之外的
`Allocate`、`IsLive`、`HighWater` 等入口；backend 私有 allocator 不经这个 singleton。

`BackendSlotTable` 的 `GetOrCreate(StatePtr)`、`HandleOf`、`NoteStateForHandle`、
`StateForHandle` 在所有 apply 上拒绝 frontend 身份；`Find(StateObject*)` 经 `HandleOf`。
上层 `StateBackendObjectRegistry` 的 frontend lookup/mint/iterator 在臂选择之前检查，
不能靠关闭 slot-table 位退回裸地址 map。按 handle 的查找、创建、遍历、释放仍正常工作。
拒绝为 `Fatal{RoleViolation, "MGPipeSlots"}`，日志还包含具体成员名。

两种历史 scope 现在是无状态标记，不持有深度、不产生豁免。P3b/P4b 可随 monolith
glue 一起删除这些标记；P5f 不删掉对应的 monolith 功能来消除 grep 结果。

自由函数 `DirectGLES::GetBackendProgramId(GLuint)` 全树无声明、调用者或 dispatch 槽。
本包以 `!MOBILEGL_BUILD_DISAGGREGATED` 隔离，D-P 中不再存在；保留非 D-P 函数体，
避免仅为删除死代码改变 pull 的 `.text` 与符号。它不等于仍被大量使用的
`BackendProgramObjectImpl::GetBackendProgramId() const` 成员。

## 2. 原 13 个 DirectGLES scope 的重新核对

这里按函数名定位，不沿用 f0 的旧行号。家族位关闭导致的 legacy 回退不视为安全的 wire
功能；新的无条件身份守卫和字段收口会拒绝它，不能用“client 已停在 barrier”解释放行。

| 原 scope 所在函数 | 当前 transport 路线 / 阻断点 |
|---|---|
| `SyncTextureObjectToBackend` | `OnResourceCopyRegion` 已由 fe/fm 将 `Src/Dst` 放入 `CopyImageEndpoint.TextureHandle`，GLES endpoint 走 `SyncTextureToBackendByHandle`；旧对象 overload 留 monolith。 |
| `SyncCurrentFBO` | 先走 `SyncCurrentFBOByRecord`；开启 family 后的 decline 经 `RefuseFramebufferBindingSlotRead`，不会落入对象 lookup。 |
| `ResolveGlobalConstantsRecord(program)` | transport 的调用点用 `ResolveGlobalConstantsRecordForHandle`；旧对象 overload 留 monolith。 |
| `SyncCurrentProgram(program)` | draw/dispatch 经 `ProgramHandleArm()` 选 `SyncCurrentProgramByHandle`。 |
| `BindCurrentFBO` | record 的 `Fbo` 经 `FindByHandle`；mandatory record 臂 decline 即拒绝。 |
| `ResolveAndBindUnitTextures` | `UnitTexturesByHandle()` 的 record 分支先行返回；legacy unit pass 留 monolith。 |
| `ResolveUnitSamplerBackend` | transport 在 scope 前以 `BindSamplerStates.Count` 拒绝 identity sampler 家族。 |
| `BindCurrentProgramWithResources` | `handleArm` 经句柄 stash / `ResolveProgramTwin`，对象 registry 在另一臂。 |
| `GetCurrentBackendProgram` | `ProgramHandleArm` 在旧 scope 前返回。 |
| 自由函数 `GetBackendProgramId(GLuint)` | 无调用者；本包从 D-P 编译中排除。 |
| `ScopedDetachedTextureFramebufferAttachments(texture)` | transport mipmap 用 handle 构造与 record reverse-index；对象构造及 `StateForHandle` 留 monolith。 |
| `ShaderStorageBlockBinding` | fe 已用 `VerbStorageBlockProgram` → `FindByHandle` → native program id，并先行返回；旧 GL-name 查对象路径留 monolith。 |
| `GetTexImage` | wire 槽仍在 `MGR_UNMIGRATED_TAIL_SLOTS`，`Fatal{UnmigratedVerb, "GetTexImage"}`；不能因为本包关闭 scope 就宣称 P9 功能已实现。 |

## 3. Managers 的 11 个 scope 与普查遗漏

`HandleOfBuffer` 的 frontend overload 在 legacy buffer path；transport buffer consumers
用 handle 入口。`HandleOfSamplerViewForTexture` 全树没有调用者，仍是 monolith glue。
`RequireImageBindableStorage` 的 object overload 在 legacy texture path；transport 使用
`RequireImageBindableStorageByHandle`。`ResolveOwnRecord` 在 transport twin 已带
`m_pushedSyncHandle`，不进入对象 lookup。

`SyncAttachmentObject` 的 texture/renderbuffer 两站和 FBO verify 的两站位于 object
`BackendFramebufferObject::SyncToBackend`；transport 使用 `SyncToBackendByHandle`。
program serial 盖章和 renderbuffer record 解析位于各自 object `SyncToBackend`；
transport 的 by-handle overload 不问 frontend identity。

**第 11 项 sampler registration probe 不能照 f0 写成不可达。** transport 的
`BindCurrentProgramWithResources` 在 depth-texture、普通 `sampler2D`、需要强制 nearest
的组合下实际调用 `GetRawDepthFetchSampler()`；该 helper 原来创建 `SamplerObject`，
setter 还会读 client context，随后 `BackendSamplerObject::SyncToBackend` 的无 CSO 分支
进入这个 registration probe。此遗漏已交 fs 包改为 server native sampler，并补世代测试。
fr 关闭所有 registry 豁免后，这个遗漏不会再静默放行。

同时发现 `VulkanRenderer::Initialize()` 仍无条件创建 blit/depth-mipmap 隐藏
frontend program；相应两个 shutdown scope 不能当作不可达依据。已交 fv 包将 transport
初始化改道。原有 host Magma 用例通过不能代替完整 surface 初始化证据；本报告不声称
另有已经核实的 headless 初始化豁免。fr 仍无条件关闭 Magma allocator 豁免。

## 4. 验证

- 行为代码 `8377e6cf` 的独立 split `RemoteClientTest` 构建成功，新增四个测试；
  `RemoteGuards` 44/44 通过，完整 executable 147/147 通过（`fr-remoteclient-full.log`）。
- `BarrieredLegacyScopesCannotExemptAllocator` 在 GLES / Vulkan 两 backend 都打开两种旧
  scope，然后实际调用 client singleton 的 `HighWater`，必须在访问前具名 abort。
- `BarrieredFrontendRegistryMembersRefuseBothLegacyScopes` 对四个身份入口逐项检查；对象
  和表在 client 创建，只有被测操作运行在 apply，日志核对具体成员名，避免在无关构造处死掉。
- wrapper 测试覆盖 frontend `Find`、mint 和 iterator，核对 wrapper 自己的 marker。
- handle 正对照在 barriered / unbarriered 两种记录下验证创建、查找、遍历、释放。
- **真实 red-once**：临时在两个 production guard 恢复“当前记录 barriered 就放行”的旧
  条件，构建并执行两条 barriered 测试，**2/2 真红**，失败为预期 abort 没有发生；恢复健康
  源码并重建后守卫测试全绿。日志为 `fr-red-once.log`、`fr-restored-green.log`，临时修改未提交。
- 非 D-P 行为改动全部由预处理隔离。pull / push、G1/G2/G14、完整 unit/GPU、strict 与
  dualblock 在 fs/fr/fv 合并树由集成者执行；本包不拿单测结果冒充整阶段门。

Linux 新树的子模块递归初始化遇到本地缺少 apitrace nested commit，随后复用了现有
`p5f-int` 的依赖与 fixture 文件；未运行 `git lfs pull/checkout`，未修改参考树。
