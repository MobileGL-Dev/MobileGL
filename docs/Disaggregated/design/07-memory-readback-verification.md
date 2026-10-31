# persistent map、回读与验证（原 ARCHITECTURE §12–§13）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

## 12. persistent map 与 ≥16 MiB 采纳

`AcquirePersistentMap` 是永久的地址空间捐赠（≥16 MiB 可变 store 自动采纳，MC 26.3 p99 163 → 21 ms），整个 monolith 改造期不动（D-B4）。拆分下三档由 POST 探针选择（spike B 实测）：

| 档 | 形态 | 结论 |
|---|---|---|
| T0 | client 分配 `AHardwareBuffer` BLOB，server 导入（Vulkan / GLES） | 唯一在两台设备、两个后端上都完整读写的档（P11 主攻） |
| T1 | server 导出 opaque fd | 仅 Adreno 的 Vulkan 路径 |
| T2 | 拒绝，client 侧推送 | 永久正确回退；**今天 split 用的档**（`MOBILEGL_IPC_ADOPT_TIER=2`），stream 数据面上强制（P11 A1：两端握手时 `Transport::SettleAdoptTierAtHandshake` 对 0/1 打一行 `Refuse{AdoptTierOnStream}` 并走 T2；共享段上 client 握手期 `Fatal{UnimplementedAdoptTier}`，`CONTRACT-P11.md` §1） |

T2 下 client 侧推送三件套：不做 map/unmap 命令对（payload 带 `hasLiveHostWrites`）；在每个 validate 点按 64 KiB 块推送可达的已映射 buffer，脏页追踪 + 哈希抑制只发变化的块（P5d）；门 `PersistentCoherentMapScenario`。`mpr` 数的是每一次 `MapPersistent` 发射（铸成或拒绝都算），所以在 monolith 下也可断言。

### 12.1 采纳档的握手期结算

档在握手期、第一条 `map_persistent` 之前，由知道数据面的一侧结算（`MG_Remote/Transport/AdoptTier.h`）。T0 用不了从不是 `Fatal`，而是 T2 加一行：`MOBILEGL_IPC_ADOPT_TIER=0` 时是具名行，未设时只打 MGLOG_D，照样计数。

| 数据面 | 一侧 | knob 0 / 未设 | knob 1 |
|---|---|---|---|
| stream | 两端 | 只有 T2（不共享内存）；knob 0 打 `Refuse{AdoptTierOnStream, "T0"}` | `Refuse{AdoptTierOnStream, "T1"}`，T2 |
| 共享段 | client | 在 `Hello` 的 `LinkTerms.adoptTier` 请求 T0（0；未设为 `kAdoptAskT0Default`）；是否真跑 T0 在第一条 `map_persistent` 时读 `kCapAdoptT0`（`ClientSession::AdoptT0`） | `Refuse{AdoptTierClosed, "T1"}`，请求 T2 |
| 共享段 | server | 不看自己的 knob：按 client 的请求，在第一次原生 bind 时批准（`ServerSession::SettleAdoptT0AtBind`，之后的 `CapsSnapshot` 带 `kCapAdoptT0`）或具名说明原因 | 同左 |

T2 路径上的使用时检查 `Client::AdoptTierIsEmulate` 读回结算结果：已结算的会话照常走 T2；knob 0 / 1 下没有任何握手结算过的 `map_persistent`（无会话的调用方）具名中止为 `Fatal{UnimplementedAdoptTier}`，不静默当成 T2。

## 13. 回读、roundtrip 清单与验证

### 13.1 稳态零 roundtrip 与不可避免的阻塞点

- 零 roundtrip：全部 draw / clear / blit / copy / dispatch / XFB / bind / CSO / `set_*` / 上传 / `present`；全部 caps 站点；`glGetError` / `glFinish` / `glFlush`；fence 与 query 的创建和非阻塞轮询；`eglSwapBuffers`；restart / multi-draw。
- 不可避免（罕见）：握手；surface 生命周期；`glReadPixels` 到客户内存；GPU-write pending 的 buffer 首次 CPU 读；带超时的 sync / query 结果；`glBufferStorage` 的 ack；纹理拉取；ring / stage 耗尽与 present credit。
- 跨机上每条 `kWaitReply`（`ResourceCreate`、`SetTextureParams`、纹理 `ResourceSubData`）都是一个 RTT：逐帧次数是 P6.5 必测数，减少它是 P9 / P10 的活。

#### 13.1.1 kWaitReply 行的逐记录豁免

`kWaitReply` 是行级属性；一条记录是否真的阻塞由逐记录谓词决定，route 与 wire emit 表读同一个函数，避免一边等另一边没要的回包。因此上面第三条所列的纹理 `ResourceSubData` 在 wire 臂上已不逐条付 RTT。
- `resource_subdata`（`MGPipeSubDataWantsItsReply`）：buffer 那半的 Bool 无人读，不等。wire 臂上纹理那半也不等：纹理记录只在 server 消费纹理族时才发（caps 镜像里的 consumer 位，与 applier 的接受检查读同一事实），所以 applier 唯一的合法拒绝"无 consumer"落不到这条记录上；其余拒绝（如目标不是纹理、无纹素或无字节、未知句柄）都是 bug，server 侧会 trip wire 报告或计数具名。client 自己取消的记录（device lost、stage 耗尽）仍答"未接受"，level 留脏重试。等待由此从每条记录移到 `SEG_STAGE` 窗口回收，与窗口成正比而不是与调用数成正比（实测见 `MEASUREMENTS.md`「纹理上传的等待粒度」一条）。
- `resource_respecify`（`MGPipeResourceRespecifyWantsItsReply`）：wire 臂上 buffer 那半不等；纹理那半等到对象的 create 被接受（`MGPipeHandleIsPublished`），之后 acceptance 已是 client 知道的事实，首次使用时付一次阻塞的 create；create 被拒则闩不置位，后续照常等真答案，代价是吞吐而不是静默。
- monolith 下答案在调用线程上直接返回、没有代价，谓词保持原答案（纹理那半照收）。

#### 13.1.2 client 侧保守 GPU 写集合

`BufferObject::SyncGpuWrites` 在应用线程上、于 `glMapBuffer` / `glGetBufferSubData` 时回答"GPU 是否可能写过这块 buffer"。后端的标记点在 server 侧，逐次去问是稳态禁止的 roundtrip，所以 client 在每条 draw / dispatch 记录发出之前，从自己持有的绑定构建这个集合（`MG_Impl/Pipe/Verb/GpuWriteSet.h`）。

- 宁多勿少：多标一次的代价是一次回读，漏标则静默读到陈旧 shadow。每个生产者有计数器（`ProducerMarkCount`），单元用例断言它确实触发。
- 前四个生产者对应后端标记点：SSBO 绑定点、atomic counter 绑定点（标全部 touched 点，比后端宽）、`Access != GL_READ_ONLY` 的 image buffer texture、XFB 捕获目标。后端故意窄的两处照搬：只读 image 绑定不标；image 遍历在 draw 准备时做，不在 `glBindImageTexture` 时做。
- 后端没有的两个生产者：读进 pack PBO 的 `glReadPixels` 发出即返回、只做标记，拷贝推迟到第一次真要读时；传输下 `glEndTransformFeedback` 不再无界等 fence，只标记捕获目标（monolith 仍等）。
- 收窄尚未实现：`MGPipeClientOnGpuWritten` 只接受一个整 buffer 范围；零个范围留给将来的"已完全收窄"，不能当作整块已写。
- 不覆盖后端自己的 scratch buffer（Espryt 的转换顶点流与 primitive-restart 替换 buffer、Magma 的 UBO ring）：后端那边同样没有 `MarkGpuWritten`，是两边共有的洞。

#### 13.1.3 glReadPixels：线上只有矩形，pack 状态留在 client

`read_pixels` 记录带的是矩形而不是布局：`MGPReadbackInfo::DstSize` 是紧凑的 `w*h*bytesPerPixel`，server 用中性 pack 状态读进这段紧凑行（就是 reply payload），client——唯一持有应用 pack 状态的一侧——再按应用的 `GL_PACK_*` 把各行散射到应用指针，行间空隙不写。若 `DstSize` 取打包后的尺寸，server 按它分配、后端又按实时 pack 状态写，就会越界（4x3 RGBA8、`PACK_ROW_LENGTH=8`、`SKIP_ROWS=1`、`SKIP_PIXELS=2`：分配 80 B，驱动写到第 120 B）。规则只关乎布局，深度与深度模板读同样适用。
- 紧凑答案大于一个 reply slot（`LinkTerms.maxReplyBytes`）时切的是读，不是 reply：一行放得下就切成 `floor(maxReplyBytes / rowBytes)` 行的整宽行带，否则把每行切成 `floor(maxReplyBytes / bpp)` 像素的列段。每段是一条普通 `read_pixels` 记录，带自己的框与紧凑 `DstSize`，各自按 pack 状态散射；段按行主序恰好覆盖矩形一次，且在紧凑布局里连续，中性 pack 的快路径因此仍可直接读进应用指针（`PlanReadbackBands` / `ForEachReadbackBand`）。
- 只有连一个像素都放不进 reply（含没有 reply 池的链路）时，client 才在发射前具名拒绝为 `Fatal{ReplyTooLarge}`：像素不能拆得比自身更小。

### 13.2 五部分验证门（取代 monolith 的字节一致门）

1. **接口纯度**：A 门 include 图（`scripts/check_include_closure.py`）、B 门 server 镜像不引用 `MG_State::GLState` 与 glslang（P6 的 a6 实测 server 仍链 `MG_Impl`，由 `scripts/link_ratchet.py` 的下行棘轮追踪；P13 把 172 条降到 27 条具名债务、`# P13` 注解清零并由脚本拒绝回潮，W7 的 `MobileGL_server_linkcheck` 把它变成真链接）、C 门 `MG_Backend` 里零 `pGLContext`；外加"每个后端 memo 键都是 `{slot, gen}`"（`HandleRecycleScenario`）。
2. **语义影子比对** `MOBILEGL_PIPE_VERIFY=1`：tracker 再用 `SnapshotFromGLContext()` 填一份 `PipeInputs`，G4 逐字段每 draw 比对，打印第一个分歧；verify 构建永不出货，活过 P13。
3. **行为 A/B**：trace 语料在 monolith / split 下 SSIM ≥ 0.99（P13 前还有 monolith-pull / monolith-push 两臂）；测试名只增不删（G14），skip 普查守住「跳过」与名集合；CTS 逐后端 0.5 pp 内；上传形状金标与 recorder 金标（§13.3）。
4. **性能**：Redmi `2f7cbe2e` reboot-clean、同热窗口配对 A/B，指标是**逐线程 CPU 时间**；**只记录、不设门**（用户 2026-09-08）。P13 之后的性能轮以 `dev` 构建为基线（用户 2026-10-07）。
5. **覆盖 + poison + 句柄纪律**：G6 0 UNMAPPED、dirty-surface 0 未映射、G8 归属完备、逐 verb poison、G7、残余块棘轮。

幸存的字节级等式：`MOBILEGL_BUILD_DISAGGREGATED=OFF` 时库里零 `MG_Remote` 符号（P13 起它就是出货库）。**G1**（pull 构建的符号集与 `.text` 对基线恒等）随 pull 构建在 P13 退役，由零 `MG_Remote` 符号、skip 普查与 recorder 金标接班。**每个门必须能因它存在的理由变红**（R-16）：带阴性对照并真跑过一次红；公共 GL 看不见的改白盒断言；拒绝普查逐用例读私有日志（`ctest -V` 是假零）。

### 13.3 长期语义门：MGPipe recorder（P13）

`MG_Test/Pipe/RecorderGoldenTest.cpp`（P13 W8）：记录器装在记录离开 client 的每一扇门上——两张 MGPipe 接口表与路由 escape（全部状态 / 对象记录）、verb 会话（全部 verb 记录）。固定的 GL 调用序列（缓冲 + VAO + 采样纹理 + program + 四种 draw；两张纹理的 framebuffer + 两种 clear + blit + mipmap）逐条录下，规范化只做一件事：指向调用方内存的 `MGPBlobRef` / `MGHostSpan` 地址清零、长度保留、所指字节入哈希。金标是一行一条记录（序号、调用名、字节数、哈希），在 `MG_Test/Pipe/goldens/recorder/`；`MOBILEGL_RECORDER_GOLDEN_WRITE=1` 才重写，且重写的那次必红。阴性对照：改一条 verb 记录的一个字段必须在那条红；改一个 GL 输入必须在资源记录处红。两种库形态（有无 `MG_Remote`）对同一金标——它第一次跑就发现无传输库的单进程不发 `set_context_values`（已修）以及 `RenderStateParameters` 的填充字节未清零（会让内容寻址的 CSO 在两个状态相同的 context 间不复用，已修）。它只覆盖推送内容，不覆盖后端对它的解释（开放问题 12）。
