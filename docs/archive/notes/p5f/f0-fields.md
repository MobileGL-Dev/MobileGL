# f0 普查 · §2.1 BARRIER_PULLED 15 字段逐字段载体判定

> 基线：`feat/disaggregated @ 8b68b92c`（代码头 25fba0d5 之后的提交只动文档）。行号均以该 HEAD 为准。
> 范围：`MobileGL/MG_Pipe/FieldOwnership.def` 的 21 行 / 15 个 BARRIER_PULLED 字段，逐字段回答
> (a) 存储形态 (b) 后端读者与 verb (c) 现有记录载体 (d) P5f 处置 (e) 依赖 / 工作量 / 包归属。
> 只读普查：本文件是唯一产物，未改任何代码、未构建、未跑测试。
> 姊妹篇：`f0-magma.md`（Magma 侧逐站点清单，本报告与之交叉引用而不重复）、`f0-reverse-channel.md`。

## 0. 总判断

1. **15 个字段全部涉及 client 地址空间，无一例外**。9 个非 sticky 行的存储全部是前端堆引用——
   4 个 `SharedPtr`（`m_boundVertexArray` / `m_programForDraw` / `m_programForDispatch` /
   `m_transformFeedbackProgram`，PipeInputs.h:807、:812-814）+ 5 个裸指针基址/槽
   （`m_bufferBindingSlot[15]` :808、`m_bufferBindingPointBase[15]` :809、`m_framebufferBindingSlot[2]`
   :810、`m_imageTextureBindingBase` :811、`m_textureUnitBase` :815）；6 个 sticky forward 里
   3 个直接交出前端对象或写前端（GetProgramObject / GetTextureObject / RecordError），
   3 个是值类查找（GetBufferBindingPointCount / HasOpenTransformFeedbackSpan / ValidateProgramName）。
   21 行 = 15 字段行 + 6 行 forward 行（FieldOwnership.def:53-165 与 :178-192）。
2. **P5e 的教训（"数据已在线上，读者没跟上"）在 15 个里命中 11 个**。已有记录供给或线上已带等价
   句柄的：GetBoundVertexArray（`bind_vertex_elements`→`BoundVertexElements`）、GetBufferBindingPoint
   （`set_shader_buffers`→`BoundShaderBuffers`，**XFB 目标除外**）、GetFramebufferBindingSlot
   （`set_framebuffer_state`→`BoundFramebuffer`）、GetImageTextureBinding（`set_shader_images`）、
   GetProgramForDraw / GetProgramForDispatch（`set_draw_program` / `set_dispatch_program`→
   `DrawProgram` / `DispatchProgram`）、GetTextureUnitObject（`set_sampler_views`→`BoundSamplerViews`）、
   GetTextureObject（`MGPCopyRegion.Src/Dst` 句柄就在 GL 名旁边，MGPipeTypes.h:1468-1476）、
   GetProgramObject + ValidateProgramName（`MGPStorageBlockBinding.ShaderCso` 就在 GlName 旁边，
   MGPipeTypes.h:1791-1792）、RecordError（`kEventGlError` 已接线，PipeFill.cpp:2166-2178）。
   **需要扩记录或新载体的只有 4 个**：GetTransformFeedbackProgram（`begin_stream_output` 只带
   PrimitiveMode，CONTRACT-P5B.md:215）、GetBufferBindingSlot 的 7 个无载体 target
   （Coverage.def:47-57）、HasOpenTransformFeedbackSpan（需 applier 自推）、GetBufferBindingPointCount
   （需 applier 自推或 constexpr 表）。
3. **包归属有一个真实的缺口**：计划 §5 的 fm 字面只覆盖"Magma 的 9 个字段"，而 15 个里
   GetBufferBindingPointCount / HasOpenTransformFeedbackSpan / GetProgramObject / ValidateProgramName /
   RecordError / GetBufferBindingSlot 六个不在其字面范围；fm 9 字段的 **Espryt 残余**（strict 车道
   活着的 8 对非 escalation 标记里的 6 对，见 §2 各字段）也无家。建议 fm 改写为"对象类字段
   （双后端）"，或新增一个字段包；RecordError 归 fv 是计划 §2.6 文本直接支持的唯一年配。
4. **顺序约束（ID-112 的绊线仍然有效）**：`PipeFill.cpp:2789-2809` 的两个 `static_assert`
   （`NoPointerBackedRowIsWhollySupplied` 等）把 7 个指针残余行钉在"仍被拉"状态；退役任一行的
   `EmittedCallSuppliesTheWholeField` FALSE 臂（PipeFill.cpp:2582-2602）必须与**该字段最后一批
   后端读者的迁移同提交落地**，否则 Magma 第一帧 draw 就解引用空镜像——注释 :2773-2781 明说
   "This residual fill is MAGMA'S ONLY SOURCE"。这不是建议，是构建期强制。
5. **strict 车道活标记 10 对与字段的映射**（strict-expected-markers.txt:34-52）：8 对表驱动
   （disjunct 1/2）+ 2 对仅 escalation（disjunct 3）。出口门要求该文件清空，而
   `GetBoundVertexArray@DrawArrays` / `GetBufferBindingPoint@DrawArrays` 两对的退役题材是 P8 的
   client vertex arrays（P5F 计划 §2.1 自注），**P8 不在 P5f 的六个包里**——要么 P5f 把这两对的
   载体（client VA staging）拉进来，要么出口门 2 需要为 escalation 对留一个具名豁免。这是
   计划文本内部的张力，f0 只记账不裁决。

## 1. 方法与口径

- **三重证据**：(i) `MG_IntegrationTest/Harness/strict-expected-markers.txt` 的 10 对活标记
  （车道实测，含 admitted 判据）；(ii) `rsp` 计数路径——非 sticky 读经 `MGP_INPUT_CHECK` →
  `MGPipeInputUnfreshRead` → `CountBarrierPull`（PipeInputs.cpp:344），sticky forward 经
  `MGPipeStickyForwardPull`（PipeInputs.cpp:372-378，七个本体在 PipeFill.cpp:2119-2186 各自
  第一行调 `MGP_STICKY_FORWARD_PULL`）；(iii) 全树 grep `MGB_CTX-><Field>` / sink 侧
  `gPipeInputs.<Field>`。
- **推导规则**（scripts/gen_pipe_field_ownership.py）：RECORD_SUPPLIED 是推导值 =
  Coverage.def 的 `MGP_COVERAGE_EMITTED_LIST`（48 行，Coverage.def:256-304）减去
  `EmittedCallSuppliesTheWholeField` 的 8 个拒绝（PipeFill.cpp:2582-2602，解析见
  gen 脚本 `parse_supplies_whole_field` :157-175；self-test 钉住"恰为 8 个"，:972-974）。
  BARRIER_PULLED 手维护且每行必须有退役相（gen 脚本 :460-462 无相即构建失败）。
  admitted 表是 ID-116 推导：BARRIER_PULLED ∧ 在 verb 类 may-read 罩内 ∧（verb 的 op 静态设障
  ∨ 退役相不含本阶段）（gen 脚本 `build_admitted` :312-375），再加运行时的 disjunct 3
  （escalation，PipeInputs.cpp:170-174 起）。
- **"今天为何能工作"的公共答案**（下文各字段只写增量）：inproc 下两角色共享一份
  `gPipeInputs`（PipeInputs.h:835），client 的第 4 步残余填充（PipeFill.cpp:3712-3762，
  :3759 的 `CopyField`）写入、server 在同一块上读，verb barrier 保证同一时刻只有一个写者
  （PipeInputs.h:902-908 的裁定）。strict 下这类读要么具名 admitted 要么 Fatal。
- **行号漂移记录**（内容判断不受影响，但引用者须知）：FieldOwnership.def:68-71 引的
  `PipeFill.cpp:1902-1905` 对应现为 `CopyField` 的 GetBoundVertexArray 臂 :128-130；
  FieldOwnership.def:82-84 引的 `SyncCurrentFBO (:2995)` 现为 DirectGLES.cpp:4465，
  `BindCurrentFBO (:4303-4353)` 现为 :5860 起；计划 §1.1 引的 PipeFill.cpp:245/:274 现为
  :244-245/:272-274，:2591-2592 现为 :2591-2597（均只差一两行）。P5F 计划 §2.2 引的
  T5 站点 `VulkanRenderer.cpp:1562-1590` 据 f0-magma.md §2 已漂移到 :1590-1637。

## 2. 逐字段判定

### 2.1 GetBoundVertexArray

- **(a) 形态**：`SharedPtr<VertexArrayObject> m_boundVertexArray`（PipeInputs.h:807；
  accessor :597-601）。O 类，`ReleaseObjectPins` 四行之一（PipeFill.cpp:95-100）。
- **(b) 读者**：Espryt 普通 draw 已走记录——`fromRecords` 三元 DirectGLES.cpp:2275、:2442、
  :6185，`VertexInputReadsRecords()` 门 :2195；**残余读点**全在 escalation 路径：
  `SyncClientSideVertexArraysForDrawArrays` :2207/:2213（client VA，P8）、:8539、
  MultiDraw.cpp:223。Magma 6 站：DirectVulkan.cpp:805、:929，VulkanRenderer.cpp:6446、:6852、
  :12550、:12651（逐站分析见 f0-magma.md §4.1；:805 的 LINE_LOOP 臂还叠加
  `buffer-legacy-arm` Fatal）。**strict 标记**：`GetBoundVertexArray@DrawArrays`
  （markers:51，仅 disjunct 3 escalation；普通 draw 对已在 P5e mv 退役，markers 头注 :29-30
  与 ROADMAP P5e 行 item 5 的 red-once 证明："倒掉 mv 的臂得 GetBoundVertexArray@DrawArrays"）。
- **(c) 载体**：`bind_vertex_elements` → applier `BoundVertexElements`（PipeApply.h:662；
  写入 PipeApply.cpp:2348）+ `VertexElementsCsos` 记录表。Espryt 读者已跟上；Magma 的
  fast-path 句柄臂存在但句柄仍由前端对象推出（f0-magma.md §4.1，VulkanRenderer.cpp:6452-6458）。
- **(d) 处置**：Magma 六站改读 `BoundVertexElements` → 升 RECORD_SUPPLIED（Coverage.def:36、
  :262 的 emitted 行早已在；删 PipeFill.cpp:2585 的 FALSE 臂，与 ID-112 绊线同提交）。
  Espryt 残余的 client-VA 对是 P8 题材——见 §0.5 的门张力。
- **(e)** 依赖：Magma vertex-input 句柄化（与 fm 的 GetBufferBindingPoint 同源）。
  工作量：中。包：fm；escalation 对无包（缺口）。

### 2.2 GetBufferBindingPoint

- **(a) 形态**：裸指针基址 `BindingSlotRange1D<BufferObject>* m_bufferBindingPointBase[15]`
  （PipeInputs.h:809；accessor :615-627，`base[index]` 即前端活槽 :624-626）。填充
  PipeFill.cpp:141-147（只填 `BufferBindPointTargets`）。
- **(b) 读者**：Espryt 的记录臂已落——`SyncBufferBindingPointsByRecord` 读
  `st.BoundShaderBuffers`（DirectGLES.cpp:670-713，:684）、`SyncAtomicCounterBuffers` 记录臂
  :873-926、UBO 循环读 `BoundShaderBuffers[Uniform]`（:7295；CONTRACT-P5E.md:476）。
  legacy 臂仍在：SyncBufferBindingPoints :724-761（:737）、:928-936（:935）。
  **XFB 目标无载体**：SyncTransformFeedbackBindingPoints :796、StartPendingTransformFeedback
  :1891（kXfbSpan 罩住这两个字段，FillPoints.def:315-316）。Magma 3 站：
  UniformManager.cpp:1205、:2035，VulkanRenderer.cpp:11764（XFB）。**strict 标记**：
  `GetBufferBindingPoint@DrawArrays`（markers:52，仅 disjunct 3）。该对的具体读点本普查
  不能从静态证据唯一确定——候选是 XFB-span draw 的 :1891（escalation (i)，open span）或
  client-VA draw；计划 §2.1 把两对都记为 client-VA（P8）。双块演习会直接报名字，f0 不裁决。
- **(c) 载体**：`set_shader_buffers` → `BoundShaderBuffers[3][84]`（PipeApply.h:776-779；
  写入 PipeApply.cpp:2918）。**XFB 目标明确不搭这条记录**（CONTRACT-P5E.md:770：
  "XFB does NOT ride this state"）；`set_stream_output_targets` 今天"has no applier and no
  producer"（CONTRACT-P5B.md:215）。
- **(d) 处置**：Magma 三站改读 `BoundShaderBuffers`；XFB 目标的载体是 `set_stream_output_targets`
  的实体化（P9 题材，落入 P5f 范围）。之后删 PipeFill.cpp:2597 的 FALSE 臂、字段翻
  RECORD_SUPPLIED（Coverage.def:71、:263 已在）。
- **(e)** 依赖：同 2.1 + XFB 记录实体化。工作量：中。包：fm + XFB 半无包（缺口）。

### 2.3 GetFramebufferBindingSlot

- **(a) 形态**：裸指针 `BindingSlot<FramebufferObject>* m_framebufferBindingSlot[2]`
  （PipeInputs.h:810；accessor :628-637，空槽即 poison Fatal）。填充 PipeFill.cpp:190-195。
- **(b) 读者**：Espryt——记录臂 `SyncCurrentFBOByRecord`（DirectGLES.cpp:4479）+ 强制臂具名
  abort `RefuseFramebufferBindingSlotRead`（:4490-4492，P5e fb）；pre-handle 回落臂仍在
  SyncCurrentFBO（:4465 起，:4507 的 `GetFramebufferBindingSlotChecked`，helper 在 :184-202），
  直接站点 :3261、:3491、:4212、:4426、:4507、:5625、:5923、:6096、:8349、:10696-10697，
  Managers.cpp:9903、:10299。FieldOwnership.def:82-84 的"8 Espryt + 13 Magma"与本次 grep
  量级一致。**strict 标记**：`GetFramebufferBindingSlot@ReadPixels`（markers:40，disjunct 1——
  `read_pixels` 是 kWaitReply）；读点是 ReadPixels 内的 `FramebufferImpl::SyncCurrentFBO()`
  （DirectGLES.cpp:14275）回落臂。Magma 13 站：UniformManager.cpp:560；VulkanRenderer.cpp:3158、
  :5510、:5730、:6360、:6465、:6845、:7648、:8016、:9024-9025、:9660、:10371。
- **(c) 载体**：`set_framebuffer_state` → `BoundFramebuffer[2]` + `FramebufferRecords`
  （PipeApply.h:714；写入 PipeApply.cpp:2558-2562），P5e fb 已给 Espryt 记录臂。
- **(d) 处置**：Espryt 的 ReadPixels 回落为何仍命中（记录臂 decline 的路径）需 f1 双块演习
  定位——静态看 :4479 应先试记录臂，标记存活说明 ReadPixels 场景走了回落；Magma 13 站改读
  `BoundFramebuffer`/`FramebufferRecordFor`（PipeApply.cpp:2609-2613）。之后删
  PipeFill.cpp:2586 FALSE 臂、翻 RECORD_SUPPLIED（Coverage.def:84、:274 已在）。
- **(e)** 依赖：fm 的 FBO 记录消费是 Espryt 已铺好的路。工作量：中（Magma 站点多但形状单一）。
  包：fm。

### 2.4 GetImageTextureBinding

- **(a) 形态**：裸指针 `ImageTextureBinding* m_imageTextureBindingBase`（PipeInputs.h:811；
  两个 const 重载 :638-653，null 基址即 poison Fatal）。填充 PipeFill.cpp:196-199。
- **(b) 读者**：Espryt——pre-handle 形式 SyncImageTextureBinding :3842（注释 :3831-3837 自明
  "what the pull build compiles and what a push-monolith build runs"）；transport 下已删的
  MarkWritableImageBufferTexturesGpuWritten :3891（门 :3885-3887）；Managers.cpp:11963（.Format）。
  Magma 4 站：UniformManager.cpp:1021、:1312、:1867、:1955。**strict 标记：无**。
- **(c) 载体**：`set_shader_images` → `BoundShaderImages`（PipeApply.h:751；写入
  PipeApply.cpp:2857）；Espryt 的记录重载已在（DirectGLES.cpp:3810-3829，`ResolveShaderImageRecord`
  + `SyncTextureToBackendByHandle`）。`bind_shader_image` verb 记录的参数全景也在线上
  （PipeApplier.cpp:889-900 的注释）。
- **(d) 处置**：Magma 四站改读 `BoundShaderImages`；Espryt 残余是 monolith 臂，不动。
  删 PipeFill.cpp:2587 FALSE 臂、翻 RECORD_SUPPLIED（Coverage.def:85、:275 已在）。
- **(e)** 依赖：无超出 fm 的。工作量：小-中。包：fm。

### 2.5 GetProgramForDraw

- **(a) 形态**：`SharedPtr<ProgramObject> m_programForDraw`（PipeInputs.h:813；accessor
  :659-663）。填充 PipeFill.cpp:244-245（计划 §1.1 的 :245 引文）。O 类 pin 四行之一。
- **(b) 读者**：Espryt——PrepareForDraw 的提升读 :6215（`programFromRecords` 三元；
  :6132-6134 的注释记了这条提升本身的历史）；记录臂：`GetCurrentBackendProgram` 读
  `MGPipeApplier().DrawProgram`（:7659-7670），legacy 臂 :7672/:7744 只在
  `ProgramHandleArm()==false` 时跑；:7029 同样带三元。Magma 3 站：VulkanRenderer.cpp:6384、
  :6815、:6853。**strict 标记：无**——P5e pa 退役，markers 头注 :25-27 记了棘轮曾指名它消失，
  ROADMAP P5e 行 item 5 的 red-once 即此对。
- **(c) 载体**：`set_draw_program` → `DrawProgram`（PipeApply.h:759）+ `bind_shader_state` →
  `BoundShaderCso`（:761；写入 PipeApply.cpp:3208-3214）；ShaderCso 记录带逐 link 的
  ProgramArchive（ROADMAP P5e 行："最大的那次迁移没有改动任何线上结构：记录早就带着逐 link
  的 ProgramArchive，缺的是仍去问前端对象的读者"——本字段就是那句话的原主）。
- **(d) 处置**：Magma 三站改读记录即毕。删 PipeFill.cpp:2589 FALSE 臂、翻 RECORD_SUPPLIED
  （Coverage.def:100、:289 已在）。
- **(e)** 依赖：无。工作量：小-中。包：fm。

### 2.6 GetProgramForDispatch

- **(a) 形态**：`SharedPtr<ProgramObject> m_programForDispatch`（PipeInputs.h:812；accessor
  :654-658）。填充 PipeFill.cpp:241-243。O 类 pin 四行之一。
- **(b) 读者**：Espryt——PrepareForCompute :8142（记录三元）+ :8154-8157 的
  `DispatchProgram` 句柄臂。Magma 2 站：VulkanRenderer.cpp:7424、:7476。**strict 标记：无**
  （markers 头注 :28：pa 在它被写进表之前就退役了它）。
- **(c) 载体**：`set_dispatch_program` → `DispatchProgram`（PipeApply.h:760）。
- **(d) 处置**：与 2.5 同形。删 PipeFill.cpp:2590 FALSE 臂、翻 RECORD_SUPPLIED
  （Coverage.def:99、:288 已在）。
- **(e)** 依赖：无。工作量：小。包：fm。

### 2.7 GetTextureUnitObject

- **(a) 形态**：裸指针 `TextureUnit* m_textureUnitBase`（PipeInputs.h:815；accessor :669-676，
  null 基址即 poison Fatal——计划 §1.1 的 :669 引文）。填充 PipeFill.cpp:272-274（计划的 :274）。
- **(b) 读者**：FieldOwnership.def:89-90 记"13 Espryt + 8 Magma"，与 grep 一致。Espryt 13 站：
  DirectGLES.cpp:2679、:2707、:2742、:2755、:3451、:6425、:6754、:7535、:10760、:11771、
  :11887、:12120、:14451。Magma 8 站：UniformManager.cpp:511、:787、:826、:852、:1679；
  VulkanRenderer.cpp:9652、:11157、:11401。**strict 标记**：`GetTextureUnitObject@CopyTexImage2D`
  （markers:43，disjunct 1——`copy_framebuffer_to_texture` 记为 verb CopyTexImage2D，
  FieldOwnership.def:285）。读点值得点名：`UpdateTextureBindingAtTarget` 的
  `MGB_CTX->GetTextureUnitObject(unit)`（DirectGLES.cpp:10760）**在 transport 分流检查
  （:10769-10774 的 `VerbCopyTexDst` 句柄臂）之前无条件执行**——句柄臂已经在了，读却先发生；
  这正是"数据已在线上，读者顺序没跟上"的最小样本，也是双块演习下第一批会红的站点之一。
- **(c) 载体**：`set_sampler_views` → `BoundSamplerViews`（PipeApply.h:741；写入
  PipeApply.cpp:2828）+ `BoundSamplerStates`（:746）。
- **(d) 处置**：Espryt 13 站 + Magma 8 站逐个改读记录（:10760 这类"读在分流前"的站点要
  把读取移进各自臂内）。删 PipeFill.cpp:2588 FALSE 臂、翻 RECORD_SUPPLIED（Coverage.def:115、
  :295 已在）。
- **(e)** 依赖：无超出 fm 的；站点数量是 fm 里最大的一批。工作量：中-大。包：fm。

### 2.8 GetTextureObject

- **(a) 形态**：sticky forward，无存储；按 GL 名返回 `const SharedPtr<ITextureObject>&`
  （PipeInputs.h:691；本体 PipeFill.cpp:2131-2135，null context 时返回 `NullShared`）。
- **(b) 读者**：Espryt 后端本体**无站点**（DirectGLES 全树 grep 无 `MGB_CTX->GetTextureObject`）；
  读者在 **server sink**：`ServerVerbSink::OnResourceCopyRegion` 用 GL 名重建两端点
  （PipeApplier.cpp:853-859，注释明说"`rsp` counts every one of these and P7 is what retires
  them"）。Magma：VkTextureManager.cpp:810。**strict 标记**：`GetTextureObject@CopyImageSubData`
  （markers:42，disjunct 2 退役相 P7；gen 脚本 self-test :998-999 与 :1039-1041 钉住它同时被
  disjunct 1/2 覆盖）。
- **(c) 载体**：`MGPCopyRegion.Src/Dst` 句柄与 GL 名**并排在线**（MGPipeTypes.h:1468-1476；
  注释 :1455-1464 自明"A GL name is NEVER an identity; it is the lookup key of a pull the
  handle beside it will replace"）。
- **(d) 处置**：sink 改按 `Src/Dst` 句柄经 server 纹理 twin 表解析（FieldOwnership.def:183-184
  的 forward 机制"a server-side texture handle table"），forward 删除、字段行出表。
  Magma 的 :810 同批。无法升 RECORD_SUPPLIED——sticky forward 没有存储可供"供给"，退役形态
  是 accessor 死亡（仿 InvalidateCompileEnv 的 P5c ev 处置，FieldOwnership.def:157-163：
  字段行改 FATAL + forward 删除）。
- **(e)** 依赖：server 端纹理句柄表（fm 的公共底座，与 2.7 共用）。工作量：小-中。
  包：fm 边缘——读者在"后端无关的 sink"而非 Magma 本体，fm 的字面范围需要为此扩写（§0.3）。

### 2.9 GetBufferBindingPointCount

- **(a) 形态**：sticky forward，值类 `SizeT`，按 target 查前端绑定点的**容量**（不是绑定状态）。
  Coverage.def:145 的 sticky 理由原文："keyed by target: a constexpr capacity table, not verb
  state"。本体 PipeFill.cpp:2119-2123。
- **(b) 读者**：Espryt 1 站：SyncAtomicCounterBuffers 的 legacy 臂 :928——它的记录臂
  （:873-877）已改读 applier 窗口，注释 :871-872 自明"the bound is the applier's window
  instead of GetBufferBindingPointCount's sticky forward, which is the row this site was the
  Espryt reader of"（即 Espryt 侧读者已退役，legacy 臂只在非记录配置跑）。Magma 2 站：
  UniformManager.cpp:1200、:2030。**strict 标记：无**。
- **(c) 载体**：`set_shader_buffers` 的窗口（`ShaderBufferStart/Count`，PipeApply.h:778-779）
  回答"用到哪"；容量本身是常量（线上 84，`kMGPipeMaxBufferBindingPoints`；CONTRACT-P5E.md:650）。
- **(d) 处置**：升 **APPLIER_DERIVED**——applier 从 `BoundShaderBuffers` 窗口/容量表回答，
  读者改读之（FieldOwnership.def:179-180 的 forward 机制就是"a server-side binding-point
  table"）。注意它与 2.2 同源：Magma 的 UniformManager 两站把 count 与 point 成对读
  （:1200/:1205、:2030/:2035），应同批迁移。
- **(e)** 依赖：同 2.2。工作量：小。包：fm（字面外，缺口）。退役相标 P7/P13——P13 的半边
  （transfer target 的容量）本普查未见读者，未见不等于没有，标**未知**。

### 2.10 HasOpenTransformFeedbackSpan

- **(a) 形态**：sticky forward，值类 `Bool`，按 lifetime id 查前端 XFB span 开闭
  （PipeInputs.h:692；本体 PipeFill.cpp:2137-2141；前端实现 Core.cpp:1299）。
- **(b) 读者**：仅 Magma 1 站：VulkanRenderer.cpp:11684（`m_xfbCounterSlotOwner` 的归并检查）。
  Espryt 无后端读者。**strict 标记：无**。
- **(c) 载体**：无现成字段级载体，但 applier 已具备自推的全部输入：`begin/end/pause/resume
  _stream_output` 四个 verb 记录（FieldOwnership.def:267-270 的 stamp 行）+ `bind_stream_output`
  （带 `LifetimeId`，MGPipeTypes.h:1773-1778）+ 已有的 `MGPipeApplierState::IsTransformFeedbackActive`
  镜像（PipeApply.h:805，P5e §2.1 escalation (i) 的两侧同值论证是先例）。注意
  CONTRACT-P5B.md:215 记 `begin_stream_output` 的 sink 今天是 stub——自推依赖 t2 sink 实体化。
- **(d) 处置**：升 **APPLIER_DERIVED**（FieldOwnership.def:185-186 的机制"server-side XFB span
  state"）：applier 从 stream-output 记录序列维护 per-lifetime-id 的 span 开闭集。
- **(e)** 依赖：t2 sink 实体化（P9 题材的边缘）。工作量：小-中。包：无明确归属（缺口；
  读者在 Magma，可挂 fm）。

### 2.11 GetTransformFeedbackProgram

- **(a) 形态**：`SharedPtr<ProgramObject> m_transformFeedbackProgram`（PipeInputs.h:814；
  accessor :664-668）。填充 PipeFill.cpp:285-287。O 类 pin 四行之一。
- **(b) 读者**：Espryt 1 站：StartPendingTransformFeedback（DirectGLES.cpp:1871，PrepareForDraw
  尾部，kDraw 罩住——FieldOwnership.def:102-105 的"XFB itself is off the reduced path, but
  kDraw's may-read mask carries it"即此）；同一函数 :1891 还读 GetBufferBindingPoint（2.2 的
  XFB 半）。Magma 2 站：VulkanRenderer.cpp:11725、:11824。**strict 标记**：
  `GetTransformFeedbackProgram@DrawArrays`（markers:44；gen 脚本 self-test :996-997 钉住它由
  disjunct 2 承载，CONTRACT-P5E §5.7 把 XFB 整体划出 P5e）。
- **(c) 载体**：**今天没有**。`begin_stream_output` 只带 `PrimitiveMode`（CONTRACT-P5B.md:215
  原文，且同文明说 capture program 靠的就是 kXfbSpan/kDraw 的 BARRIER-PULLED 读）；
  `set_stream_output_targets` 无 producer 无 applier。这是 15 个里"线上没数据"的代表。
- **(d) 处置**：升 RECORD_SUPPLIED 需要先造数据：给 `begin_stream_output` 增加 capture
  program 的 ShaderCso handle（或实体化 `set_stream_output_targets` 并让它带上），两端读者
  （Espryt :1871、Magma 两站）改读 applier 状态。**不能降 FATAL**：kDraw 罩内有活读者，
  缩减路径（OpenRA 的 XFB 用例）会读。
- **(e)** 依赖：记录 schema 扩展（线上行，须与 gen 管线同步）+ 双后端读者。工作量：中。
  包：fm（Magma 半）；Espryt 半的退役相标的是 P3b/P4b（与 fr 的 registry 重键同源不同事，
  归属需在 §5 定包时裁决）。

### 2.12 GetProgramObject

- **(a) 形态**：sticky forward，按 GL 名返回 `const SharedPtr<ProgramObject>&`
  （PipeInputs.h:690；本体 PipeFill.cpp:2125-2129）。
- **(b) 读者**：Espryt 2 站，成对出现在 ValidateProgramName 之后：GetBackendProgramId :8204
  （+:8199）、ShaderStorageBlockBinding :12600（+:12599；:12603-12606 还背着 G6 的
  `MGPipeFrontendKeyedRegistryScope`）。Magma 1 站：DirectVulkan.cpp:278（+:275）。
  sink 侧注释 PipeApplier.cpp:921-923 自明"Both backends resolve the PROGRAM through the
  barrier-pulled GetProgramObject(GlName) … retired by P9"。**strict 标记**：
  `GetProgramObject@ShaderStorageBlockBinding`（markers:41；`SetStorageBlockBinding` 是
  kWaitApplied，PipeCalls.def:297，disjunct 1/2 皆覆盖）。
- **(c) 载体**：`MGPStorageBlockBinding.ShaderCso` 已在记录里（MGPipeTypes.h:1791-1792：
  "the identity, and what P7 dispatches on"），名字 blob 走 SEG_STAGE。数据已在线上。
- **(d) 处置**：两端 backend 的 `ShaderStorageBlockBinding` slot 改按 ShaderCso 句柄解析
  （esp. Espryt :12597-12620 的"只推给已建成 program"优化路径，其 frontend-keyed `Find` 一并
  随 fr 的 registry 重键消失）；forward 删除，字段行参照 2.8 的 ev 形态出表。无法升
  RECORD_SUPPLIED（无存储），处置形态是 accessor 死亡。
- **(e)** 依赖：fr 的 program registry 句柄化（:12603 的 scope 是 P5c G6 债，两包有交集）。
  工作量：小-中。包：无明确归属（缺口；可挂 fm 或并入 fr 的 program 半边）。

### 2.13 ValidateProgramName

- **(a) 形态**：sticky forward，值类 `Bool`，前端名表探测（PipeInputs.h:694；本体
  PipeFill.cpp:2158-2162；Coverage.def:127 记 kClientResolved——"the server is never asked"
  是设计答案，今天的实现恰恰相反）。
- **(b) 读者**：与 GetProgramObject 完全同点成对（2.12 的四处；另 DirectGLES.cpp:8199）。
  **strict 标记**：`ValidateProgramName@ShaderStorageBlockBinding`（markers:45）。
- **(c) 载体**：不需要载体——发射端 client 在发 `set_storage_block_binding` 时名字必然已过
  前端 validator；server 按 ShaderCso 解析到记录即隐含合法（与 2.12 是同一笔迁移）。
- **(d) 处置**：随 2.12 一起退役；字段行出表（机制"a client-resolved program name probe"，
  FieldOwnership.def:187-188）。
- **(e)** 依赖：同 2.12。工作量：小（与 2.12 同提交）。包：同 2.12。

### 2.14 RecordError

- **(a) 形态**：sticky forward，**写前端**（错误队列）（PipeInputs.h:695-697；本体
  PipeFill.cpp:2164-2186）。15 个里唯一的写方向。
- **(b) 读者（写者）**：Espryt：Managers.cpp:14858、DirectGLES.cpp:11124（另有
  Managers.cpp:14949-14951 自明"NO RecordError HERE"的反向先例）。Magma：DirectVulkan.cpp:735、
  VulkanRenderer.cpp:1367、:1371、:1427。**strict 标记：无**（车道不制造 GL 错误）。
  **rsp 口径注意**：`MGPipeStickyForwardPull(RecordError)` 在分流**之前**（PipeFill.cpp:2165），
  所以即使 transport 臂根本不碰前端（:2166-2178 直接 `PostGlError` 后 return），server-stamped
  verb 内的每次调用仍被记一次 pull——这是机制的保守，不是残余的债。
- **(c) 载体**：**已存在且已接线**：P5c ev 把 transport 臂改成了 `kEventGlError` 事件
  （CONTRACT-P5C §4.2；`ServerSessionInstance().PostGlError`，PipeFill.cpp:2174-2177），
  错误消息经 `info->toString()` 过线。剩在 P9 的只是**有序化**（:2170-2173 的注释：
  "preserves per-thread program order of error-then-read but not cross-verb interleaving"）。
- **(d) 处置**：这是 15 个里唯一"载体已通、剩记账口径"的字段。建议仿 InvalidateCompileEnv 的
  P5c ev 处置（FieldOwnership.def:157-163 的先例与 :160-161 的"FieldOwnership 只描述 split
  server 的读"论证）：把 `MGP_STICKY_FORWARD_PULL` 移到分流之后（或 transport 臂豁免计数），
  字段行降 FATAL——论据是"transport 下分流臂已是完整答案，server 侧再落到 monolith 臂即真
  缺陷"。需先逐写者核 `OnGlError` 覆盖（消息保真已由 :2174 保证，有序化仍是 P9 的）。
  若 f1 双块演习显示monolith 臂在 split 下仍被合法触达，则退而维持 BARRIER_PULLED 到 P9——
  双块的红就是判据。
- **(e)** 依赖：fv 的反向通道有序化口径（P9 的边界划分）。工作量：小。包：**fv**——计划
  §2.6 明列"OnGlError 有序化"，其中触碰 client 内存的部分归 P5f，而本字段 transport 下已不
  触碰，剩余正好是有序化与计数口径。

### 2.15 GetBufferBindingSlot

- **(a) 形态**：裸指针 `BindingSlot<BufferObject>* m_bufferBindingSlot[15]`（PipeInputs.h:808；
  accessor :605-614，空槽/越界即 poison Fatal）。填充 PipeFill.cpp:131-140——只填
  `GlobalBufferTargets`，**Index 恒 null**（:132-136 的注释：GLContext 经绑定 VAO 的
  element-buffer 槽推导，"a derivation no FillPoints.def row can copy"）。
- **(b) 读者**：按 target 分片（Coverage.def:37-70 的权威拆分：8 个 target 有记录载体，7 个
  没有——CopyRead/CopyWrite/PixelPack/PixelUnpack/Texture/DispatchIndirect/Query）。
  Espryt：PixelPack 读回 6 站（DirectGLES.cpp:12809、:13791、:14028、:14338、:14769，
  Utils.cpp:2353）；DrawIndirect/Parameter 族 :440、:976、:1422、:8921、:8999-9000、:9078、
  :9151-9152、:9332、:9381，MultiDraw.cpp:308。Magma：DirectVulkan.cpp:283、:407、:470、:538、
  :591；VulkanRenderer.cpp:7520（DispatchIndirect）、:11126、:12560-12561、:12661-12662、:12731。
  **strict 标记两对**：`GetBufferBindingSlot@ReadPixels`（markers:34，disjunct 1——读点是
  ReadPixels 的 PBO 槽 :14337-14338 一带）与 `GetBufferBindingSlot@DrawArrays`（markers:36-39，
  wave-3 新：multi-draw 五档被强制后 indirect 档浮出，注释 :35-38 记了它"was named before it
  could fire"）。
- **(c) 载体**：`set_indirect_buffers` 只带 DrawIndirect+Parameter 对（Coverage.def:44-45，
  MGPIndirectBuffers 的自明范围）；`resource_readback`（MGPReadback）带目标 buffer 句柄
  （MGPipeTypes.h:1439-1443）但 PixelPack 槽的读在 verb 内、不在记录解析点。PixelPack 等
  7 target **无任何 call**（Coverage.def:47-57 逐 target 点名）。
- **(d) 处置**：本字段不能一次翻类，只能按 target 渐进——而字段按 Coverage.def:61-69 的裁定
  必须保持一行，所以 P5f 的工具是 **`MGP_FIELD_OWNERSHIP_ARG_LIST` 的按参数例外行**
  （GetPixelStoreParameters,1 已是先例，FieldOwnership.def:214-216）：有载体的 target 逐批窄出
  BARRIER_PULLED，Query target 两端都无读者（Coverage.def:56-57）可率先窄为 FATAL，
  DispatchIndirect（Coverage.def:50-55 点名"两端都读且无 call"）需要扩 `set_indirect_buffers`
  或新记录（P13 题材），PixelPack 随 readback 记录改造（P9 题材）。全部窄完后字段行出表。
- **(e)** 依赖：readback 记录化（P9）+ indirect 扩 target（P8/P13）——**三个退役相分属三个
  后续阶段，是 15 个里唯一需要 P5f 主动把外阶段题材拉进来的字段**。工作量：大
  （15 target × 双后端 × 三类载体）。包：无归属（缺口最大的一行）。

## 3. 包归属汇总与缺口

| 字段 | 处置方向 | 载体状态 | 包（计划 §5 字面） | 建议包 |
|---|---|---|---|---|
| GetBoundVertexArray | 升 RECORD_SUPPLIED | bind_vertex_elements 已在 | fm（Magma 半）；escalation 对无包 | fm + P8 议题 |
| GetBufferBindingPoint | 升 RECORD_SUPPLIED | set_shader_buffers 已在（XFB 除外） | fm；XFB 半无包 | fm |
| GetFramebufferBindingSlot | 升 RECORD_SUPPLIED | set_framebuffer_state 已在 | fm | fm |
| GetImageTextureBinding | 升 RECORD_SUPPLIED | set_shader_images 已在 | fm | fm |
| GetProgramForDraw | 升 RECORD_SUPPLIED | set_draw_program 已在 | fm | fm |
| GetProgramForDispatch | 升 RECORD_SUPPLIED | set_dispatch_program 已在 | fm | fm |
| GetTextureUnitObject | 升 RECORD_SUPPLIED | set_sampler_views 已在 | fm | fm |
| GetTextureObject | accessor 死亡（FATAL 形态） | MGPCopyRegion 句柄已在 | fm 边缘（读者在 sink） | fm |
| GetBufferBindingPointCount | 升 APPLIER_DERIVED | applier 窗口可推 | 无 | fm |
| HasOpenTransformFeedbackSpan | 升 APPLIER_DERIVED | 需 applier 自推（依赖 t2 sink） | 无 | fm 或 P9 边界 |
| GetTransformFeedbackProgram | 升 RECORD_SUPPLIED（**须先造数据**） | 无——begin_stream_output 需扩 | fm（Magma 半）；Espryt 半标 P3b/P4b | fm + fr 裁决 |
| GetProgramObject | accessor 死亡 | MGPStorageBlockBinding.ShaderCso 已在 | 无 | fm 或 fr |
| ValidateProgramName | accessor 死亡 | 随 2.12，无需载体 | 无 | 同 2.12 |
| RecordError | 降 FATAL（计数口径修正后） | kEventGlError 已接线 | fv（唯一字面匹配的） | fv |
| GetBufferBindingSlot | 按 target 渐进（ARG 例外行） | 8/15 有载体，7/15 无 | 无（P8/P9/P13 混合） | 缺口最大 |

**三个给 §5 定包的输入**：
1. fm 的字面"Magma 的 9 个字段"应扩写为"对象类字段的双后端退役"——否则 6 个字段无家可归，
   且 fm 内部还混着 sink 侧读者（GetTextureObject）与 Espryt 残余读者（GetTextureUnitObject@
   CopyTexImage2D、GetFramebufferBindingSlot@ReadPixels、GetTransformFeedbackProgram@DrawArrays）。
2. 两个 escalation 对（client VA，P8 题材）与 GetBufferBindingSlot 的 P8/P9/P13 混合退役，
   是出口门 2（markers 清空）与范围表 §3（不取 P8 staging）之间唯一的正面冲突，需要计划
   层面裁决：拉进来，或为 escalation 对留具名豁免。
3. 顺序约束：7 个指针行的 `EmittedCallSuppliesTheWholeField` FALSE 臂删除（PipeFill.cpp
   :2582-2602）与对应最后一批改者必须同提交，ID-112 的两个 `static_assert`
   （PipeFill.cpp:2789-2809）是构建期强制执行者；`FieldOwnershipTest` 钉住非 sticky
   BARRIER_PULLED 清单（FieldOwnership.def:66-67），gen 脚本 self-test 钉住拒绝集恰为 8
   （scripts/gen_pipe_field_ownership.py:972-974）——每退役一行，这三处与
   strict-expected-markers.txt 的同提交更新是机械义务。
