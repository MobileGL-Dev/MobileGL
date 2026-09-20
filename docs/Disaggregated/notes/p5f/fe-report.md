# P5f / fe — Espryt 字段退役报告

> 分支 `p5f/fe`，基线 `418c765c`。本次恢复时已有四个提交：`85f31047`、
> `07bf8d7c`、`881e050e`、`a0004db9`；并非 handoff 所说的未开工。
> 本次保留这些提交，补单测/strict 棘轮，发现并修复 XFB make-current 生命周期缺陷，
> 完成门禁。最终行为代码 `19a4400a`，注释整理 `63c2a331` / `31e95b07`。
> Windows 树 `../MobileGL-p5f-fe`；WSL 独立树 `/home/swung/w7/p5f-fe`。

## 1. 结果与范围

Espryt `integration-split` 的 179 条目在普通、strict、双块形态均无失败；strict 实测
零 Fatal / 零 Admitted / 零 ESCALATED，允许标记文件清空。双块总车道 181 条目中只余
Magma 两条 NamedBlit 的 `GetFramebufferBindingSlot@Clear`，本包未声称 P5f 已收官。

退役的方法是改读者：readback 读 framebuffer surface 记录、copy 端点读 texture handle、
storage-block binding 读 program handle。XFB 需要新数据，故扩 `MGPStreamOutputBegin`
为 120 字节 POD，携带 capture-program handle、XFB lifetime id、四组 buffer handle/range。
没有增加 call/opcode，没有用指针或整个 PipeInputs 块充当载体。

`GetBufferBindingPointCount`、`HasOpenTransformFeedbackSpan` 的字段与 forward 行翻为
APPLIER_DERIVED；全表仍有 13 个 BARRIER_PULLED 字段，原因是 Magma、P8 client arrays、
RecordError 以及尚未删除的不可达 legacy accessor。fe 只完成 Espryt 本包范围；不能在
Magma 最后一批读者迁移前删除 `EmittedCallSuppliesTheWholeField` 的拒绝臂和 ID-112 绊线。

## 2. 十五字段逐项交账

| 字段 | Espryt 的来源 / 本包改动 | 剩余边界 |
|---|---|---|
| GetBoundVertexArray | 普通 draw 已由 `BoundVertexElements` / vertex records 回答，本包核对原记录臂 | client-VA escalation 属 P8，Magma 属 fm |
| GetBufferBindingPoint | 普通 UBO/SSBO/atomic 读 `BoundShaderBuffers`；XFB capture 目标本包改读 Begin snapshot | Magma 属 fm，legacy monolith 仍读前端 |
| GetFramebufferBindingSlot | `IsAlphaWidenedFallbackReadAttachment` / `IsFixedPointFallbackReadAttachment` 改读 `ReadFramebuffer()->ReadSurface`；其它 transport FBO 路线已有记录臂 | Magma Clear 是本包双块唯一剩余首阻塞 |
| GetImageTextureBinding | 既有 `BoundShaderImages` / record overload；剩余 frontend overload 是 monolith 臂 | Magma 属 fm |
| GetProgramForDraw | 既有 `DrawProgram` / shader-CSO archive / handle twin | Magma 属 fm |
| GetProgramForDispatch | 既有 `DispatchProgram` / shader-CSO archive / handle twin | Magma 属 fm |
| GetTextureUnitObject | `UpdateTextureBindingAtTarget` 把 frontend 读移到 transport 早退之后；copy destination 直接消费 `VerbCopyTexDst` | 其它 transport sampler readers 已有记录臂；Magma 属 fm |
| GetTextureObject | `OnResourceCopyRegion` 用记录已有 `Src/Dst`；`CopyImageEndpoint.TextureHandle` 送入 Espryt server texture twin/format record | Magma 共用 endpoint seam 由 fm 接；renderbuffer copy 维持原 class-C 拒绝 |
| GetBufferBindingPointCount | transport 返回四种 indexed target 的 `kMGPipeMaxBufferBindingPoints`（84）；与 frontend 数组容量相同，无 context 查找 | 这是存储容量，不是设备公布的较小 XFB/SSBO limit；非法 target 返回 0 |
| HasOpenTransformFeedbackSpan | transport 查 server `StreamOutputSpans`，Begin/End 维护 | reset 保留 snapshot，object release 清空；见 §3 |
| GetTransformFeedbackProgram | deferred Begin 读 snapshot 的 `CaptureProgram`，由 server shader-CSO record 取得不可变 ProgramArchive；scatter pin 的也是该 archive | Magma 属 fm；monolith 保留 SharedPtr 路线 |
| GetProgramObject | storage-block verb 改读 `VerbStorageBlockProgram` 对应 server twin | `GetBackendProgramId` 的 legacy helper 属 fr 死代码核查；Magma 属 fm |
| ValidateProgramName | storage-block transport 不再按 GL name 探 frontend 表 | 同上一行；frontend GL 验证仍在 client |
| RecordError | 本包未改 | fv 的 reverse-channel 包明确拥有 |
| GetBufferBindingSlot | 六个 pack-readback 站点在 transport 选 server reply scratch，不读 PixelPack slot；其它 target 见下表 | Magma、P8 与 legacy 读点不能被 Espryt census 代为认证 |

### f0 所称七个“无载体” target 的复核

f0 把 `set_indirect_buffers` 的覆盖面当成整个 verb 的覆盖面，因此“必须扩七种 target”
并非最终结论。按当前 executable 路线逐项核实后，fe 无须为它们新增记录：

| target | 实际路线与证据 |
|---|---|
| CopyRead | Espryt backend 无 `MGB_CTX` 的此 target 槽读取；client buffer 操作解析为 resource 身份 / 数据后发射 |
| CopyWrite | 同 CopyRead；不是 server 必须恢复的 context binding |
| PixelPack | `EmitReadPixels` 在任何 emission 前已具名拒绝 `ReadPixels+PACK_BUFFER`。server 接受的是 reply scratch 地址，因此其 pack buffer 必须为空。本包六个站点都选空 server binding；没有声称实现 PBO readback。GetTex* 仍受原 class-C / shadow-emulation 拒绝 |
| PixelUnpack | client `GL_Texture` 解析 offset 和 unpack 状态为上传数据；texture/resource 记录供 server 消费，Espryt backend 不读 client PixelUnpack 槽 |
| Texture | Espryt 无 context `BufferTarget::Texture` 槽读者；纹理 buffer 的对象内绑定与 server texture/resource records 是另外一层身份，不能混同为 context 槽 |
| DispatchIndirect | 已有 `MGPGridInfo.IndirectBuffer` → sink `VerbDispatchIndirectBuffer` → `SyncComputeBuffers` 的 `EnsureBufferResourceForHandle`，P5c hd 已实现；`SyncBoundBuffer(DispatchIndirect)` 仅 monolith else 臂 |
| Query | 两后端都没有此 context 槽的 backend reader，Coverage.def 也明确点名无读者 |

DrawIndirect / Parameter 则早已有 verb handles（含 count buffer），`MultiDraw` 的
`BoundDrawIndirectBufferId` 在 P5e ra2 已改读记录；这是旧 strict `GetBufferBindingSlot@DrawArrays`
早在 f1 基线就 stale 的原因。P8 client arrays 没有在本包被假称完成。

## 3. XFB snapshot、身份与生命周期

Begin emitter 在 `BeforeReadOnlyVerb` 后取得 capture program 的 CSO handle，并把四组
buffer range 按存储大小裁剪。resource records 与 shader-CSO archive 先到，Begin snapshot
随后到。server sink 以 lifetime id 保存 snapshot；Espryt deferred Begin 用 archive 回答
stride/varying/layout，按 buffer handles 建 capture targets。End readback/scatter 同样按
handle 找 server buffer resource，保留现有事件字节写回路径。

这同时消掉 f0-reverse-channel 中 XFB 的两条身份债：transport 的 `scatterProgram`
不再 pin client ProgramObject，`targets` 不再从 client buffer 裸地址反查 handle。它们
随 capture source 一起迁移，不能让后续 fv 再重复造一套。monolith 的旧字段仍留在结构内。
`g_xfbObjects` 按 GL name 的静态对象命名空间/世代分区仍归 fs；RecordError 与
`ResourceTracker` 的 client 直写仍归 fv；这些没有被本包绿门证明已解决。

恢复后的审查还发现初稿把 `StreamOutputSpans` 放进 `MGPipeApplierReset()` 清空。
reset 会在 make-current 返回活 context 时发生，而 client 不会重发 Begin，所以首次
capture draw 会丢 archive/targets。本包修为：

- reset 只撤当前 bound lifetime id，保留所有活 span snapshot。
- 已存在的 `MGPContextValues.BoundTransformFeedbackLifetimeId` 在 apply 时恢复当前绑定。
- End 删除当前 span；`MGPipeApplierReleaseObjectRecords()` 清空全部 snapshot 和绑定。

此处没有新 context 值记录。单测模拟“Begin 的对象快照已到 → make-current reset →
返回 context 的值记录 → 查 capture → object release”，校验 64 位 identity、program
handle 和 buffer range 跨 reset 保留，并校验 release 后消失。静态 XFB backend 的全套
context 世代问题仍属 fs，这个测试只证明新 snapshot 的生命周期。

## 4. 双向棘轮，逐对解释

双块从 f1 的 **155 红 / 6 对**变成 **2 红 / 1 对**，剩下 fm 的
`GetFramebufferBindingSlot@Clear`（两个 NamedBlit）。没有新增预期对，也没有无名崩溃。

| 被删除的双块首阻塞 | f1 条目数 | 退役改动 |
|---|---:|---|
| GetFramebufferBindingSlot@ReadPixels | 132 | read attachment 分类用 server framebuffer surface；其后 PixelPack 用 reply scratch |
| GetTransformFeedbackProgram@DrawArrays | 11 | Begin snapshot 的 capture-program archive；后续 binding-point pull 也由 range snapshot 替代 |
| GetTextureObject@CopyImageSubData | 6 | copy 两端使用记录已有 texture handles |
| GetTextureUnitObject@CopyTexImage2D | 2 | copy destination handle 分流先于任何 frontend texture-unit 读取 |
| ValidateProgramName@ShaderStorageBlockBinding | 2 | storage-block program handle 分流；其后 GetProgramObject pull 同时消失 |

strict 从文件原有 10 对到空表，每一行的出处如下：

| strict 消失对 | 归因 |
|---|---|
| GetFramebufferBindingSlot@ReadPixels | 上表 read surface 路线 |
| GetBufferBindingSlot@ReadPixels | server reply scratch 路线 |
| GetTextureObject@CopyImageSubData | 上表 copy handles |
| GetTextureUnitObject@CopyTexImage2D | 上表分流顺序 |
| ValidateProgramName@ShaderStorageBlockBinding | 上表 program handle |
| GetProgramObject@ShaderStorageBlockBinding | 同一 handle 臂的下游读取 |
| GetTransformFeedbackProgram@DrawArrays | capture archive |
| GetBufferBindingPoint@DrawArrays | capture target ranges；旧表虽列在 ESCALATED 区，活来源是 XFB，不能仅凭旧注释当作 P8 client arrays |
| GetBoundVertexArray@DrawArrays | f1-report §8.1 已在独立基线树证明 stale（client-array 条目移出 integration-split）；本包不领取这笔实现功劳 |
| GetBufferBindingSlot@DrawArrays | f1-report §8.1 同样证明基线已 stale；multi-draw 记录路线已在 P5e ra2 |

因此这个车道的空表不要求拉入 P8 staging，也不代表所有 backend/所有 lane 的字段表已归零。

## 5. Red-once 与最终门禁

恢复时旧 `fe-red-once.log` 有 22 个 Espryt 具名场景红，但没有足够完整的独立 revert
操作记录，不单凭这份旧日志领取 R-16。本次另做可复现、确实执行的 red-once：

- `81d20554` 加 `FieldOwnershipTest.SplitCaptureSnapshotSurvivesMakeCurrentUntilObjectRelease`，
  实际 rc=8；bound lifetime id 被清成 0、HasOpen=false、snapshot 查找失败三项断言红。
- `19a4400a` 修 reset/restore 后，同条目 rc=0。红/绿日志分别是
  `/home/swung/w7/p5f-logs/fe-xfb-reset-red.log`、`fe-xfb-reset-green.log`。
- f1 机关阴性对照也重跑：旋钮 1 绿，旋钮 0 红且为 distinct-block 用例自己的断言。

| 门 | 本包实测 |
|---|---|
| split build | rc=0 |
| unit | **2265/2265**（f1 2262 + 本包 3 条） |
| integration-split，旋钮关 | **179/179** |
| strict，旋钮关 | **179/179**；零 Fatal / Admitted / ESCALATED；空棘轮核对绿 |
| 双块 split + magma | 181 条：**175 green / 4 skip / 2 red**；一对 fm Fatal、零 Admitted、每条红都具名；棘轮绿 |
| integration-gpu | **1359/1359**，零失败（包含车道设计的 skip） |
| pull / push flavours | 两者实际完成构建 rc=0；compile_commands 验证 pull 无 PUSH、push 有 PUSH=1 |
| G1，对 e75e00cb 基线 | `.text` **10806051 → 10806051**；defined symbols **27815 → 27815**；**0 added / 0 removed / 0 resized / 0 renamed**；data/bss/rodata 也零差 |
| G2 | pull 与 push 都 **3008** 个 ctest 名，集合差为 0 |
| G14 | split 名集合 **3646 → 3649**，删 0、只加本包三条 FieldOwnershipTest |
| 生成器 / include closure | gen_pipe check+12 控制、ownership check+13 控制、dirty check+27 控制全绿；include 4 probes、0 skipped/0 problems |
| doc citations --strict | 与基线相同的 2 处 basename 歧义：CONTRACT-P5 的 Init.cpp / Core.cpp；无新问题 |
| 设备门 | 未跑；未取得 Redmi 设备，本包不声称通过 |

恢复时发现并修好的旧失败是 `TheStickyExemptionIsCancelledByTheServerStamp`：它以前
要求所有 sticky forward 都不 fresh；两个 scalar forward 改为 server-derived 后，测试应
按 ownership 区分，其他 client forward 的撤销断言仍保留。旧 strict gate 也会因为全部
10 对消失而红；按实际 census 删除上述行后，两侧棘轮都绿。

日志在 `/home/swung/w7/p5f-logs/`：`fe-unit.log`、`fe-isplit.log`、`fe-gpu.log`、
`fe-final-strict-gate.log`、`fe-final-dualblock-gate.log`、`fe-final-{pull,push}-build.log`、
`fe-g1.log`、`fe-name-gates.log`、`fe-final-gens.log`、`fe-final-doc-citations.log`。
strict / dualblock / gpu 必须串行，因为它们复用条目私有日志；最后重跑 strict → dualblock
保存最终摘要和 census，避免 GPU 车道覆盖双块 marker 后再拿它核棘轮。

## 6. 集成注意

按 fc → fe → fm 集成。fm 需要共同的 `CopyImageEndpoint.TextureHandle`、
`VerbStorageBlockProgram` 与 sink 赋值；两包已交换相同命名和代码边界。
`FieldOwnership.def` 与生成文件按最后存活读者统一重生成，不能盲目保留两边文本。
全局零 BARRIER_PULLED、fs 静态分区、fr 死代码/registry、fv 反向通道和设备门仍待后续。
