# P5f fm — Magma 跨角色读写退役

> 分支 `p5f/fm`；Windows worktree `MobileGL-p5f-fm`；WSL gate tree `~/w7/p5f-fm`。
> 已验证代码头：`552ade5b`；最后一轮运行与棘轮结果见 §5。后续本报告与注释提交不改变执行路径。
> 本报告只裁定 fm；不宣布 P5f 总出口完成。

## 1. 真实可达性修正

f0 的“带 buffer 的 draw 最终到 buffer-legacy-arm”不能推出整个 draw 子树不可达。
`ClipDistanceScenario.AnEnabledClipDistanceRemovesTheNegativeHalf` 使用 `gl_VertexID`，没有 VBO；在 f1 基线
`~/w7/p5f-f1` 的真实 Magma/inproc、role-split=0、strict=0 下像素测试通过（`/tmp/p5f-fm-baseline-clip.log`）。
因此本包保留无 buffer draw/dispatch，新增 server program source、descriptor 和 render-pass 路径；没有把所有 draw 拒绝。
迁移后同一 case 在 role-split=1、strict=1 下通过（`fm-clip.log`；集成者独立复核 `int-wire-clip.log`）。

记录能被消费也必须能被发射。client 的 P4a family gate 和 server 的 `NoP4aConsumer`
原来都绑在 buffer resource-ops 上，而 Magma 没有该 ops。本包改为 client 查询该 family
的已发布 consumer bit、server 接受其 object records；残余 fill memo 加 caps generation。
Magma 仍不发布 buffer resources bit 7；发布 indexed-binding bit 13，用记录辨别未绑定与 P7 buffer 债。
texture staged bytes 原本也只由 GLES 的 resource-ops hook 采纳，本包补无 hook 时的 server-owned
adoption、respecify/defined-ness、destroy/release，保留有 hook 的 Espryt 路径避免双采纳。

## 2. 九字段与邻接字段

| 字段 | Magma transport 的现在来源 / 边界 |
|---|---|
| GetBoundVertexArray | `SetupWireDraw` 读 `BoundVertexElements` / `VertexElementsCsos`；无数组与 current generic attribute 正常画；实际读取 enabled array 时在 client 读前具名 `buffer-legacy-arm`（P7）。|
| GetBufferBindingPoint | UniformManager 读 `BoundShaderBuffers` 判断绑定；已存在的无绑定 native buffer placeholder 保留，有真实 SSBO/UBO buffer 时具名 `buffer-legacy-arm`。XFB capture 同样在读取前端绑定前拒绝。|
| GetFramebufferBindingSlot | `WireFramebuffer.inc` 的 clear/blit/read/copy 与 `WireDraw.inc` / pipeline builder 全读 framebuffer record / `MGPSurface`。|
| GetImageTextureBinding | UniformManager 读 `BoundShaderImages`；native image view 与 sampler 使用 server resource record。|
| GetTextureUnitObject | sampler 读 `BoundSamplerViews` / `BoundSamplerStates`；mipmap 与 framebuffer-copy 读各 verb handle。|
| GetProgramForDraw | `MagmaProgramSource(handle, ShaderCsoRecord)`；SPIR-V / reflection 来自 `Archive`，可变绑定和 global UBO 来自记录尾部。|
| GetProgramForDispatch | 同一 source，以 `DispatchProgram` 句柄选择 compute program。|
| GetTransformFeedbackProgram | 无 XFB draw 不进该读点；active XFB capture 在 wire setup 的 P7 buffer 边界具名拒绝。本包没有宣称 Magma 的 XFB capture 已可用。|
| GetTextureObject | handle-keyed VkTextureManager 完成 sync，无 GL-name / client weak_ptr 查询。|

邻接项：`GetBufferBindingPointCount` 不再被 wire uniform 路径读取；`HasOpenTransformFeedbackSpan`
只留在 class-C query / P7 capture 子树。`GetBufferBindingSlot` 的 indirect wrappers 在第一处前端读取前
保持具名 P7 拒绝；server 生成的 LINE_LOOP indices 用 transient server bytes，普通无 buffer draw 不受影响。
`ShaderStorageBlockBinding` 用 `VerbStorageBlockProgram` 更新 server `StorageOverrides`，不再经
`TryGetDirectVulkanProgram` / `ValidateProgramName` / `GetProgramObject` 探 client 名表。

独立 fm 的全局 FieldOwnership 分类仍保留 15 个 BARRIER_PULLED 字段；fe 集成后两个 scalar sticky
改为 APPLIER_DERIVED，仍余 **13 个字段**。其余对象行需在收口时与 residual-fill /
generator 的静态不变量一起归零，不是本包门绿后自动翻成 RECORD_SUPPLIED。
这里没有把“没有调用旧对象 getter”冒充“旧 SharedPtr 字段已由记录填充”。`FieldOwnership.def` 已加该说明。
`RecordError` 的反向通道归 fv；`PipeInputs::IsLive()` 仍 forward 到 `LiveContext()`，也是
未退役的 client liveness 读取，留给 fv / 阶段收口核对；均未在本包领取完成。

### Program source、descriptor 与发射约束

`MagmaProgramSource` 是仅在 disaggregated 构建存在的只读视图：monolith/internal-program
构造接 `const ProgramObject&`，wire 构造接 handle 和 server `MGPipeShaderCsoRecord`。
非 disaggregated 构建是原 `ProgramObject` 的类型别名，保持 pull 代码形态。
SPIR-V、linked stages、uniform/block/XFB reflection 来自 ProgramArchive；`BlockBindings`、
`SamplerUnits`、`StorageOverrides`、`GlobalConstants` 来自当前记录，后者覆盖 archive 的旧值。
全局 UBO 字节与版本不再问 client，SSBO 索引沿用旧 SPIR-V reflection 的排序规则。

server 视图不使用或写回 `ProgramObject` 的 hash memo：每次按当前 archive/绑定尾部算 hash，
编译产物仍由 ProgramFactory 的 server cache 复用。按内容进一步缓存 hash 属性能工作，
本包没有以共享 client memo 换取“快”。draw 和 dispatch 各自从记录选 program；普通
无 buffer draw 支持 current generic attributes，`gl_InstanceID` 的既有 SPIR-V rebase 保留。

sampler/image descriptor 消费 `BoundSamplerViews` / `BoundSamplerStates` / `BoundShaderImages`，
以 server texture record 创建原生 image view、按 record sampler 参数创建 sampler；view 的
mip/layer window、format alias、swizzle 与隐式 alpha 按记录恢复。真实 buffer/texel-buffer
消费者仍在前端解引用前具名拒绝。无绑定 image 所需的 frontend placeholder 则明确报
`Fatal{UnmigratedVerb, "Magma:unbound-image-placeholder@P7"}`，不是构造 client 对象来兜底。

vertex-input 的发射不受 Magma 缺 bit 7 的牵连：`NewVertexElements` 属 bit 8，P4a 的
consumer/dependency gate 只管该阶段的四个 object families。server 因而能区分 enabled array
与 current attributes；wire 缺少 vertex-elements record 会具名拒绝，不能静默当空 VAO。

`CapsMirrorTest.ObjectFamilyEmissionTracksIndependentConsumerCapsWithoutBufferOps` 在同一
子进程中采用 Texture-only → Framebuffer-only → Texture-only 三个
snapshot；buffer bit 始终为 0，逐次断言真实 `MGPipeP4aFamilyEmits` 的正反变化。
当前对象行仍被 BARRIER_PULLED 分类支配，测试没有假造 `SuppliedFieldMask` 的值变化；
caps generation 是该环境缓存的失效键，也是后续归属表收口必须保留的条件。

## 3. T5 与三类结构耦合

- T5：mipmap 使用 `VerbMipRes/BaseLevel/LevelCount`，只改 server image / staged shadow；upload-target 集合由 resource descriptor 推导，view window 从 `ViewOf` / sampler-view record 解析。无 client texture unit、upload-target vector 或 mip-level storage 写入。
- texture twin / memo：独立 `std::unordered_map` 以 `{slot, gen}` 为键；dead-generation prune、shutdown 清理；shape 重建保留旧 GPU 内容。pending-upload 只上传指定 regions，避免 CPU 旧影子覆盖 GPU clear/blit/mipmap 后未改区域。失败上传不推进 synced serial。
- program twin：`MagmaProgramSource` 在非 disaggregated 编译下是 `ProgramObject` 别名，在 split 下按值引用 server archive/record；`GetBackendHashMemo/SetBackendHashMemo` 不再写 client 对象。
- server 构造 client 类型：wire clear/blit/mipmap/draw 使用 native image/view/framebuffer/render-pass，sampler 从 `SamplerParameters` 构造；不走隐藏 ShaderObject/ProgramObject/SamplerObject 或前端 texture placeholder 构造。原 frontend placeholder 的 allocator 边界显式改写为上节的具名 P7 拒绝，未装成新可用功能。
- buffer twin / 直写：真实前端 BufferObject 的 backing resource、mapped bytes、EnsureGpuResident 等仅留 monolith 或既有 P7 拒绝之后；`ResourceTracker.h` 的 GPU-written fallback 归 fv，没有改。

`CopyImageEndpoint.TextureHandle` / `VerbStorageBlockProgram` 与 fe 采用相同 seam。
为独立 fm 门可运行，同时带入 fe 的 GLES CopyImage endpoint 接线和 shared readback helper 的 server reply-scratch
分支；集成保留同一份，不重复计算为两份迁移。

新 draw render-pass 仍沿用既有正确性规则：sRGB attachment 随 `GL_FRAMEBUFFER_SRGB`
选择 sRGB/UNORM 写 view；1D/1D-array storage 保留相应 view 类型；各 attachment 使用
GENERAL layout 与 descriptor 一致。旧 pass/framebuffer/view 退休前等待已提交 GPU 工作完成。
每次 command buffer 重开均经 FrameContext observer 重置 dynamic-state shadow，viewport、
scissor、blend/stencil 等不会因连续 draw 的相同值而漏发。此处选择正确性优先的提交/等待，
不声称完成 P7 的批处理性能设计。

staged store 的 hookless 路径与 GLES hook 二选一，绝不双 Adopt。named respecify 定义对应
mip（包含 null-data），whole immutable respecify 重定义完整链，destroy 删除 handle key，
object-record release 清空；make-current reset 保留仍活对象的数据。

## 4. 出口门 4 的回答

**Magma 不发布 `kCapRunAheadApply`。** 无 buffer draw / compute、framebuffer、texture 诸实际可达路径
以与 Espryt 相同的 role-split + strict 机制验证；分块时旧 client pointers 根本不在 server 块里，lockstep 不能
掩盖这些读取。真实 buffer consumers、host-texel placeholders 和少量 native format/function shapes 仍是 P7 功能债。
新 wire render-pass 为保证对象退休正确，提交后等待 queue 完成才销毁，尚未建立 run-ahead 的资源版本和性能契约。
此外 `IsLive` forward、fv 剩余反向写入与完整字段归属收口仍未完成，不能把当前已测工作集
扩大成“全 server apply 路径已满足 run-ahead”的能力承诺。保留 lockstep 是能力声明；
不把它当跨角色读写的豁免。`MagmaPipeIdentityTest.AMagmaServerNeverPublishesTheRunAheadCapBit`
分别对 ready=false/true 固定 Magma 的回答为不发布，避免随 Espryt readiness 翻转而误开启。

R11F_G11F_B10F 的 mip blit 是已实测的原有功能债：f1 基线真实 inproc 下
`F1WireScenario.GenerateMipmapPackedFloatPixels` 像素失败，RGB 都为 0（`fm-base-packed.log`）。
本包对应失败为 `Fatal{UnmigratedVerb, "Magma:mipmap-native-format@P7"}`；新增 Magma 同名测试留在
`integration-magma-p7-known-red`，CI 强制恰一条实际执行、failure 非 skip、私有日志精确含该 marker。
它不加入双块 field-fatal 棘轮；原 Espryt case 与所有既有测试名保留。

## 5. 门与 red-once

独立包的验证在 `~/w7/p5f-fm @ 552ade5b` 完成，日志在 `~/w7/p5f-logs/`。两份 red-once 证据均为真实执行：

- 无 buffer ClipDistance：baseline 共享块绿 / fm 双块 strict 绿。
- Program source 单测 3/3；临时退回 archive 旧 binding 值，`MagmaProgramSourceTest.ServerBindingTailsReplaceLinkTimeDefaults` 真红（旧值 9 分别不等于记录值 3/6）；健康源码 3/3 绿（`fm-program-source-tests.log` / `fm-program-source-red-once.log`）。这份证据为独立临时 object/executable 执行；最终集成 unit 门仍单列核对。
- `StagedTextureProductionTest.HooklessTextureConsumerOwnsBytesAndScopedStorageLifetime`：旧 metadata-only 条件吞 named null-data mip，level-1 extent 为 0 导致真红；修复后 1/1 绿（集成者 `int-fm-staging-red-once.log` / `int-fm-staging-green.log`）。

最终门记录：

| 门 | 最终结果 |
|---|---|
| 代码 / build | `552ade5b`，split build rc=0。|
| unit | **2267/2267，0 failed**；10 个基线平台/宏 skip（`fm-unit-final.log`）。|
| integration-split，旋钮关 | **179/179，0 failed**；4 个既有专属车道 skip（`fm-isplit.log`）。|
| strict，旋钮关 | **179/179，Fatal=0**，6 ordinary + 1 escalated = 7 对；双向棘轮通过（`fm-strict-final.log`）。|
| Magma 双块 + strict | **31 条，29 PASS + 2 既有 driver-capability skip，0 failed**。无 buffer sampler/uniform、compute imageStore（含 ignored Layer=7）、view-only STORAGE 升级、sRGB/1D、VBO 具名拒绝都实际执行通过（`fm-magma-final.log`）。|
| 双块 Fatal 棘轮，独立 fm | **210 条：51 PASS + 6 skip + 153 具名红**；四对首阻塞，zero Admitted、零无名崩溃，双向棘轮通过（`fm-dualblock-final.log`）。这是尚未合 fe 的包树，不是集成树全绿结论。|
| packed-format known-red | 实际 1 条 failure、0 skip，私有日志精确 `Magma:mipmap-native-format@P7`；同 CI 的 JUnit + marker 核对通过（`fm-known-red.xml` / `fm-known-red.log`）。|
| pull / push / G1 | 最终头两 flavour 生效宏正确且构建成功；G1 带两项 fail-on 硬门 rc=0。`.text` 10806051 → 10806051，defined symbols 27815 → 27815，0 added / removed / resized / renamed（`fm-g1-final.log`）。|
| generators / include closure | 全部 check/self-test 通过（12/13/27 个生成器阴性控制），include closure 4 probes、0 skipped、0 problems。doc citations 仍只有 f1 已记的两处 basename 歧义（`fm-gens.log`）。|
| G2 / G14 | 公共入口 catalogue 未改；旧注册名保留。新增 Magma 专属主车道、known-red 和 5 个 unit cases；完整集成名集合核对由集成者执行。|
| integration-gpu / f1 negative control | 不在独立 fm 重复跑整阶段门；集成者在 fc + fe + fm 树执行并记录。|

双块棘轮只减两对：`GetFramebufferBindingSlot@Clear` 随 Magma framebuffer 路径退役；
`GetTextureObject@CopyImageSubData` 随 shared fe/fm endpoint-handle seam 退役，那 6 个 Espryt
case 前进到仍有的 `GetFramebufferBindingSlot@ReadPixels`，该对条目数 132 → 138。
其余三对是 `GetTransformFeedbackProgram@DrawArrays`（11）、
`GetTextureUnitObject@CopyTexImage2D`（2）、`ValidateProgramName@ShaderStorageBlockBinding`（2）。
没有增加预期对。strict 删 shared CopyImage 对，并清理 f1 已独立复现的两条 baseline-stale
`GetBoundVertexArray@DrawArrays` / `GetBufferBindingSlot@DrawArrays`；实测只剩上述七对。

sampler-view 请求 STORAGE usage 时会在 native root 上保留用途、升级并复制已有 GPU 内容，
不伪造 client root descriptor 的 BindMask。descriptor 先于 framebuffer view 解析，避免
同一 draw 的两种用途引用升级前后的不同 image。`ComputeImageStoreThroughViewPreservesOtherRootLayer`
先建立无 STORAGE 的 root image，再只绑定 layer-1 view，验证新层绿、原 layer-0 红，真实通过。

strict、双块与 gpu 会覆盖同一测试的私有日志，串行运行并保存各自摘要；后跑车道的日志
不能拿来替前一车道补证。新增 scenario 名集合保留既有测试，known-red 专门车道也必须实际
执行并验证自己的失败原因，避免通过移标签隐藏回归。

设备门未执行：没有已授权的 Redmi `2f7cbe2e`；集成者枚举只见另一台 unauthorized 设备。
该项由阶段收口记录，不能由 host lavapipe 结果代替。
