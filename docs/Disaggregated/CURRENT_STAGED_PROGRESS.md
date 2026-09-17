# P5 集成者决策日志

基线 `feat/disaggregated @ a29807cc`。每条带证据与"推翻它需要什么"。

---

**ID-1（阶段选择）** P4a 之后取 **P5**（传输 + inproc applier + 发射表），不取 P3b/P4b。
理由：P5 只依赖 P4a，且承载「首个 IPC 帧（缩减路径）」这个里程碑（`ROADMAP.md:21`）；
P3b/P4b 的深化不挡跨进程第一帧，可以之后与 P7 并行。
推翻需要：P5 的某个前置在侦察里被证明属于 P3b/P4b（侦察已跑完，未出现）。

**ID-2（侦察轮）** 八个只读 scout 并行（codec / transport / install+thread / caps+reply+reverse /
unmigrated census / gpu-writes+persistent-map / test-ci / 对抗性事前尸检），报告落
`~/w7/notes/p5/scout-*.md`，共 4494 行，全部 file:line 可复核。事前尸检那一个是新增的角色，
它的产出（九类缝在 P5 表面的定位 + 三张必须冻结的表）直接成了 brief 的 §3。

**ID-3（brief）** `~/w7/notes/p5/BRIEF-P5.md`，797 行（§12 追加后 873 行），裁定 R-1…R-15。

**ID-4（核心裁定：lockstep inproc，R-1）** apply 侧是一条真线程且终身持有原生 context，
但客户端在每个 verb 边界阻塞等 `appliedSeq == emitSeq`。
证据：63 个 `PipeInputs` 字段里 31 个没有推送载体，缩减路径实读其中 21 个
（`scout-unmigrated-census:§3`），`PrepareForDraw` 无条件读两个前端指针
（`DirectGLES.cpp:4486`/`:4497`），退役它们的是 P8。没有栅栏，后端会读到"未来的"字段值。
栅栏是可退役对象（`MOBILEGL_IPC_VERB_BARRIER`，默认 1），随表 2 的 BARRIER-PULLED 那一栏清空而逐族打开。
**代价：P5 不产生 CPU 收益，这是预期不是回归**（性能按 2026-09-08 规则只记录不设门）。
推翻需要：一个能在不阻塞的前提下让服务端正确回答那 21 个字段的方案，且不把 P8 的工作提前进 P5。

**ID-5（核心裁定：诚实 inproc 五条，R-2）** 事前尸检的裁定 A 是「inproc 的六条门可以在
一个字节都没离开地址空间的实现上全绿」——因为 `MGHostSpan::Ptr`、`Blob.Offset`、
`MapPersistent` 的返回指针在同一地址空间里全都能解析。
对策写成五条可检验命题（`Ptr` 恒 null、`Blob.Size` 恒非零、四条 Fatal 臂、T2 恒定 decline、
`MOBILEGL_IPC_AUDIT=1` 下 retire 后把暂存字节填 `0xDD`），每条必须被观察到红过一次并记录（退出门 E5）。

**ID-6（回复槽 id = 记录序号，R-3）** 十条 `kReplySlot` 调用的 payload 一个都不含 `MGPReplySlot`；
与其让第一个需要它的包自己发明铸造规则（S-1 那一类的形状），不如规定 id 就是记录序数
（`ARCHITECTURE.md:124`：wire 上本来就没有逐记录序号字段，seq 就是序数）。
配套：栅栏的那次等待同时就是「等我的回复」，所以阻塞 `read_pixels`、`MapPersistent` 的 decline、
四个 `Bool` acceptance（R-5）零额外往返。P9 把它推广成异步池时，这条升级为「seq 是 id 的初值」。

**ID-7（split bring-up，2026-09-11）** 在任何包分叉之前先跑了本分支**第一次**
`-DMOBILEGL_BUILD_DISAGGREGATED=ON` 的配置与构建（`~/w7/notes/tools/p5_split_bringup.sh`）。
结果：configure rc=0、build rc=0；`nm --defined-only build-split/libMobileGL.so | grep -ci MG_Remote`
= **98**，pull 构建 = **0**；五个 `MG_Test/Wire` 套件注册 **52** 个测试（它们在本分支上第一次被编译）。
`ctest -L unit` = **1819/1837，18 个 abort**，全在 `ResourceEmit.*` / `FramebufferEmit.*` / `TextureEmit.*`。
诊断（当时未确认，**后被 ID-11 推翻**）：以为是毒化。
归属：c0 负责把 build-split 的 unit 弄绿或逐条点名归属给 p1。

**ID-8（c0 的模型）** 契约包本来按「重要部分可用 fable」的规矩派给 fable，**Fable 配额用尽**
（429 rate_limit），改派 opus，prompt 里补上 ID-7 的发现。
推翻需要：配额恢复且契约需要重做时再考虑。

**ID-9（c0 v1 落地，2026-09-11）** 分支 `p5/c0`，七个提交 `41b4f8df..b4bbcc11`，未推送。
验收：四个构建目录全过；`gen_pipe.py --check` 绿、`--self-test` 从 7 个对照涨到 **9**；
`gen_pipe_dirty_surface --check` 绿；`check_include_closure` 4 探针 0 问题；
**G1 一动不动**（pull 侧 `MG_Remote` 符号 0、split 侧 188；`.text` +0、27811→27811、
0 增 / 0 删 / 0 resize / 0 重命名）；`ctest -L unit` pull/push/verify 各 1785/1785、
**split 1842/1842**；`integration-gpu` 在 build-push 与 build-split 各 1117/1117，
与 `a29807cc` 逐名相同（diff 0 行）；G2 0 行、G14 +57/−0（多的 57 正是 Wire 套件）。
G1 之所以没动，是因为**连解析器都在 `#if MOBILEGL_BUILD_DISAGGREGATED` 后面**——
一个 `Features` 成员会 resize `MG_Config::Features`，一句无条件的 warning 会 resize
`MG_ConfigLoader::Init`。

**ID-10（brief 的 14 处更正）** c0 是七个包分叉前的最后一个读者，它交回 14 条更正，
已写进 BRIEF-P5 §12，并确立**冲突时 `MG_Remote/CONTRACT-P5.md` 优先于 brief**。
其中三条会改变某个包做什么：
(a) `ResourceRespecify` 的第二个无载体伴随物 `MGPRespecifiedLevel*`（重定义的作用域）——
没有它，OpenRA 每一次逐 level 的 `glTexImage2D` 都静默走整资源臂并丢掉 pending 上传；
(b) 发射表是 **71** 个指针不是 69（`Present`/`SetSwapInterval` 在 `MG_Impl` 零调用点，走 EGL 路径）；
(c) verb 普查实测 **6 个槽**（`Clear`/`DrawArrays`/`ReadPixels`/`BlitFramebuffer`/`GetIntegeri_v`/`Present`），
`Flush` 根本不是槽且 `glFlush`/`glFinish` 是空函数体，`GetIntegeri_v` 由第一次 `glCompileShader` 到达。
另外两条值得记：`CallMask` 原本没有逐族位所以 R-8 不可实现（c0 裁定 bits 32..47 承载消费者掩码），
表 1 是 23 行不是 19 且 brief 与 premortem 的两个 19 是不同的清单。

**ID-11（我的 ID-7 诊断被推翻）** 18 个 split abort 与毒化无关：它们是测试**故意触发并期待**的
`Fatal{ProtocolCorruption}` 绊线，七个 `MG_Test/Pipe` TU 在 `#if` 里测 `MOBILEGL_PIPE_POISON`
却没 include 唯一定义它的头（`MG_Backend/MGPipe/PipeInputs.h`），于是编了不 abort 的那条臂，
而 `PipeApply.cpp`（include 了）编了 abort 的那条。`MOBILEGL_BUILD_DISAGGREGATED` 是唯一住在
那个头后面的 arming 条件，所以 split 是第一个能暴露它的构建。c0 已修（每文件一个 include
+ 一处潜伏的 guard 更正），**p1 的机制清白**。
**教训：把假设写进 prompt 要写成"待确认的诊断"，c0 是因为去实跑了才没被带偏。**

**ID-12（R-15，新裁定）** getter 形状的 `GLFunctionsTable` 槽由 caps 镜像在客户端就地回答，
不发射记录；`GetIntegeri_v` 属于这一类（它携带的六个 compute 上限本来就在 `MGPCaps::Dynamic` 里），
现成的门是 `AdvertisedLimitsScenario.ComputeWorkGroupLimitsAreTheCapsBlocksAnswer`。
71 个槽的三分类清单（就地回答 / 发射 / `UnmigratedVerbFatal`）进 `CONTRACT-P5.md`，c1 不重推。
同时补文件所有权：`MG_Pipe/MGPipeTypes.h`（payload 结构形状）归 c0。

**ID-13（c0 收尾轮 + 合入推送，2026-09-11）** c0 追加两个提交（区间 `41b4f8df..5175f9cc`，共九个）：
`45b759e7` 落地 respecify 作用域载体（`Pad0` 高字节 `HasRespecifiedLevel`、`Pad1` 两个 `Uint16`，
逐字节等价 `MGPRespecifiedLevel`，五个 helper 免得有人把三个字段开写成一个值，三条 `PipeFields.def` 行，
一条 verify-only 的 `PinWholeResourceRespecifyScope`；`MGP_ASSERT_POD(...,88)` 没动，新增一条 offsetof 钉）；
`5175f9cc` 把 R-15 与 71 个槽的三分类写进 `CONTRACT-P5.md §7`。
G1 复跑仍是 `.text +0`、27811→27811、0/0/0/0；四条 unit 车道与 `integration-gpu` 逐名相同全保持。
已 ff 合入 `feat/disaggregated` 并推送 **GitHub `origin/feat/disaggregated = 5175f9cc`**
（路径：`~/w7/pipe` → `~/mgl/MobileGL` → Windows `MobileGL-disagg` → GitHub；只有 Windows 那个有 GitHub remote）。

**ID-14（R-15 顺带挖出的更大问题）** 69 个槽里 **41 个在调用点有 null 检查，而其中若干条 null 检查其实是
能力探测**，不是安全检查（`GL_Query.cpp:481/:785` 的 occlusion query、`:534` 的 XFB primitives 三元、
`SubDataResident` 的 op 表槽）。R-4 禁止 null 槽 ⇒ 发射表会让这些探测**一律答"支持"**，
它们背后的回退路径静默消失。裁定：**split 下 null 检查一律变成 caps 镜像读**，与槽本身属于哪一类无关。
这正是 `ARCHITECTURE.md:114`「`CallMask` 取代『槽位是否为 null』」那句话的具体清单。归 c1。

**ID-15（两波并行）** 七个包分两波，因为「首个 IPC 帧」本身是一条串行链：
**第一波（已开）w1 编解码器 / s1 会话与传输 / p1 毒化与字段归属 / b1 buffer 侧 / t1 测试与 CI**——
五个的验收都是单元级 + monolith 车道级，互不依赖；
**第二波 v1 服务端与线程 / c1 客户端与发射表**——它们落在 w1+s1 的真实代码上而不是桩上，
并且共同拥有「缩减路径第一帧绿」这个验收。
推翻需要：w1/s1 其中之一落地后证明第二波仍只能对着桩写。

**ID-16（p1 v1 落地，2026-09-11）** `p5/p1`，五个提交 `388364d5..6fc1bcc8`，未推送。
两个生成器 `--check` 绿；新的 `gen_pipe_field_ownership --self-test` 有 **11 个对照全部按名变红**
（第 1 条正是退出门 E4 的「字段不在任何一类里」）。G1 `.text +0`、27811→27811、0/0/0/0。
`ctest -L unit` pull/push/verify 各 1791，split **1862**；`integration-gpu` 在 push 与 split 下
`MOBILEGL_TRANSPORT=monolith` 各 1117/1117 逐名相同；G14 0 删除 / +6。

**给 v1 的 stamp 规则（v1 的 prompt 必须带上）**：
`StampVerbBoundary(op)` → `MGPipeVerbForWireOp(op)`；等于 `kVerbCount` 就什么都不 stamp。
否则每条记录在它的 applier 之前调一次 `MG_Pipe::MGPipeServerStampVerbBoundary(verb)`：
bump serial、stamp 该 verb class mask 里的 RECORD-SUPPLIED + APPLIER-DERIVED 字段，
**并把其余全部清零**——清零是承重的，因为客户端会 stamp 全部 63 个，不清零 `rsp` 恒等于 0。
四个边界 op：`Clear`、`DrawVbo`、`ReadPixels`、`Blit`；**`Present` 不是**（`FillPoints.def:21`）。
计数读取 `MGPipeResidualPullCount()`。

**缩减路径的真实欠债是 27 个 BARRIER-PULLED 字段，不是 brief 说的 21**
（kClear 14 / kDraw 26 / kReadback 18，去重后 27）。brief §3 表 2 的那份 21 字段清单据此更新。

**p1 的两处偏离（我接受）**：(a) pixel-store 按**参数**拆而不是新增第 64 个字段 id
（沿用 `Coverage.def:62-69` 的先例与五个"参数结构上不可能"就毒化的访问器形状），
好处是 `kMGPipeInputFieldCount` 仍是 63、不用动 c0 的文件；unpack 半判 FATAL，
因为六个后端站点全部传 `false`。(b) `MGPipeUnmigratedEmulation` 的 Fatal 臂挂在**传输**上而不是构建上
——一个 split 构建跑 monolith 传输不该 Fatal，这比 `#if MOBILEGL_BUILD_DISAGGREGATED` 正确。

**ID-17（s1 v1 落地，2026-09-11）** `p5/s1`，四个提交 `5175f9cc..4cf59428`，16 文件 +3405/−82，未推送。
G1 `.text +0`、27811→27811、0/0/0/0；四个构建目录绿；`ctest -L unit` pull/push/verify 各 1785、
split **1860**（1842+18）；`integration-gpu` 在 build-push 的 monolith 臂 1117/1117，
`ctest -N` 名单与 2902 行基线逐字节相同；纯度 4 探针 0 问题；`gen_protocol --check` 干净；
`ProtocolSmokeTest.cpp` 一行没动（Wire 75/75）。

三条影响别人的结论：
(a) **单条记录上限是 2 MiB 不是 4 MiB**——契约 §5 与 `Config.h` 都漏了 `SEG_CMD` 头部那 4096 字节控制页，
8 MiB 段里的二次幂 ring 是 4 MiB，于是 `MaxRecordBytes()==Capacity()/2` = 2 MiB。
已转告 w1：R-10 的计数器必须对 `RingProducer::MaxRecordBytes()` 运行期比较，不能自己从
`MOBILEGL_IPC_RING_MB` 推常量；这同时把 `CreateShaderState` 的余量砍半，若实测逼近就要重开 R-10。
公布的四个 `SegmentRef` 尺寸不变（仍是 `ProtocolSmokeTest.cpp:72` 钉住的那四个数）。
(b) **`appliedSeq` 只有一个写者**：`SessionConsumer::ApplyOne`，每记录 +1，pad 永不计数。
v1 的 `ServerLoop` 驱动它但不得直接动 `RingControl`；门铃留在会话上（拿 `SessionProducer&`/`SessionConsumer&`）。
(c) **`CallMask` 在表 0 删字段之后一度连载体都没有了**（c0 删 `tableSlotMask` 是对的，但 R-8 因此第二次
变成不可实现）。s1 给 `CapsSnapshot` 追加 `callMask` + `backendType`、给 `Hello`/`Welcome` 追加
`abiFingerprint`（全是 append，union tag 不动）。`ServerSession::SetConsumedSubsystems`/
`SetCapabilityBits` 的默认值是推导出来的，**标给 v1 确认**。

**ID-18（b1 v1 落地，2026-09-11）** `p5/b1`，八个提交 `5175f9cc..5c45ec2a`，未推送。
四个构建 rc=0；G1 0/0/0/0、`.text +0`；`ctest -L unit` pull/push/verify 各 **1806**（基线 1785）、
split **1863**（基线 1842）；G2 0 行；G14 +57/−0；`integration-gpu` 在 push 与 split 的 monolith 臂
各 1117/1117，每个名字都在基线里；`gen_pipe --check` 干净、纯度 0 问题。

**契约的表 1 有一行是错的，b1 改对了**：`HasLiveHostWrites` 应该骑 **`MGPSubData` 的 pad**，
不是 `MGPResourceDesc` 的。理由：**buffer 的 respecify 永远不可能是 metadata-only**
（`RespecifyRedefinesNoStorage` 先把 buffer 目标拒掉），所以放在描述符上等于每次 map 付一次整份重传。
**由此产生一条会静默毁数据的集成风险，已转告 w1**：编解码器如果把 `MGPSubData::Pad0` 清零／跳过／
断言为零，就静默删掉了这个位，而丢掉 `HasLiveHostWrites` 不是可见故障——它是 `IsBufferDrawClean`
对一个有活宿主写者的 buffer 答"干净"，于是这一帧画的是上一次上传的字节且没有任何诊断
（正是 `Managers.cpp:2650-2655` 与 `SanityTest.cpp:4583` 记住的那次回归）。

另两条：`MarkGpuWritesForDraw/ForDispatch` 与 `PushPersistentMapsBeforeVerb` 写好并测了但**故意没接线**
（客户端调用点是 c1 的，必须在 verb 记录**之前**跑）；`PersistentCoherentMapScenario` 必须断言
自己在哪条臂上（`IsBackendPersistentMapped()` 为 false = 模拟臂），完整断言清单在 b1 报告 §4.1，已转告 t1。

**ID-19（p1 复审，2026-09-11）** 0 BLOCKER / 5 MAJOR / 8 MINOR / 4 QUESTION，有条件放行。
**p1 报的每一个数字都复现了**（G1 0/0/0/0 `.text +0`；unit 1791×3 + 1862；`integration-gpu`
两处 1117/1117 逐名相同；G14 0 删除），三条 E4 对照都实操过并变红。
**最狠的一条 M-1：第 4 号自测对照是空的。** 它的替换串 `r"\1 \"-\","` 是 raw 字符串，
于是往行里写了字面量 `\"-\"`，`ROW_RE` 随即不再匹配、整行消失，生成器退出时打的是**第 1 号对照的**消息。
复审用插桩 `expect_trip` 证明了。所以"每条债务行必须点名退役阶段"这条守卫**根本没有对照**，
"11 个对照全部变红"实际是 10 个。根因是 `expect_trip` 抓任何 `SystemExit`，
而它抄的 `gen_pipe_dirty_surface.py` 是**逐条断言对照自己的问题串**的。
已发返工轮：先修 harness 再重审全部 11 条。
另两条：生成器看不见**被省略**的 stamp 行（三个 op 与某 verb 同名却没有行）；
`gen_pipe_field_ownership.py --check` **没有任何 CI job 在跑**——一张生成并检查的表，CI 不检查就只是惯例不是门。

**ID-20（s1 复审，2026-09-11）** 1 BLOCKER / 8 MAJOR / 7 MINOR / 3 QUESTION。
**九个数字全部复现**，20 次 ctest（`-j 8`）+ 30 次 gtest 重复零 flake，水位线内核判定可靠。
**BLOCKER：`ServerSession::CallMask()` 在没人调 `SetConsumedSubsystems` 时静默代入一个推导出来的消费者掩码，
而没有任何人调它。** 推导（`ServerSession.cpp:120-125`）读的是 `MGPipeGetResourceOps()` 这个**进程级**全局，
于是 inproc 下服务端用**客户端的**注册回答——正是它自己头文件 `:117-118` 警告客户端的那个坑；
`PublishCapsSnapshot` 再零诊断地把它发上线。spawn 下它塌成 `0x7f`，五个 P4a 族停止发射，
客户端照样按 acceptance 清 dirty，车道全绿。**这是 ID-39 的镜像。**
裁定：**不要更好的推导，要没有推导**——掩码未设即具名 Fatal / 拒绝发布。
R-8 的原话是"客户端的门不能由服务端的事实回答"，而"服务端的门由**进程级**事实回答"是同一个缺陷降一层。
**M-1 证实了我在复审 prompt 里点名的那个怀疑：flatbuffers 删表字段不会退役它的 id**——
`CapsSnapshot` 的删除**复用了 vtable 槽 14/16 且类型不同**（复审对着基线生成头证明），
没有 ABI major bump，而 `abiFingerprint` 对此失明。改用 `(deprecated)` 把槽位烧掉。
M-2：`MOBILEGL_IPC_RING_MB`/`_STAGE_MB` 解析了、打日志了、**从不生效**——
一个报告自己没用的值的旋钮，会让每一次用它做的测量都是假的。
M-3/M-4：24 字节的"可验证"帧能让两个握手都空指针解引用（复审有探针）。已发返工轮。

**ID-21（w1 v1 落地，2026-09-11）** `p5/w1`，八个提交 `bc969f9a..52bf9acc`，`PipeApply.{h,cpp}` 一行没动。
G1 `.text +0`、27811→27811、0/0/0/0；pull 侧 `MG_Remote` 符号 0、split 侧 215；
`ctest -L unit` pull/push/verify 各 1785、split **1893**（1842 + 51 条新 `PipeWireCodecTest`，含 12 条 fork 死亡用例）；
`integration-gpu` 在 push 与 split 各 1117/1117、0 个新名字。

**w1 抓到一个会让整条 ring 悄悄吞记录的碰撞**：`MGPWireRecHeader::Flags` **就是**
`RingRecordHeader::flags`，而两个标志空间重叠——`kVarTail == kRecPad`、`kHostSpan == kRecBorrowSlot`。
把 `MGPipeCallFlagsFor(op)` 盖进头里（生成的注释正是这么邀请的）会让 `RingConsumer::Pop`
把九条 `kVarTail` 记录**当成回绕填充静默跳过**。编码器改为翻译，两条 static_assert + 一条测试钉住。
**集成项：c0 要改掉那条注释**（它邀请了这个缺陷）。

**R-10 就此结案，P5 不需要分块**：`CreateShaderState` 的记录恒为 200 字节（blob 全走 `SEG_STAGE`），
有界记录里最大的是 4632 B，对着**真实的 2 MiB**上限有约 400× 余量。
只有 `DrawVbo.NumDraws` 与 `ResourceSubData.RegionCount` 无界，两者都会大声失败。
另两条确认：解码器不写任何 `RingControl` 字段（s1 的要求）、逐字节保留声明的填充字节（b1 的要求，
整结构 memcpy、无 pad 归零、无零断言）。

**ID-22（t1 v1 落地，2026-09-11）** `p5/t1`，四个提交 `5175f9cc..8d0850bd`，13 文件，
外加 `~/w7/notes/tools/wsl_p5_{gate,rss}.sh`。
G1 `.text +0` 0/0/0/0；G2 diff 0；G14 0 删除 / +11（已列名）；
`ctest -L unit` 1785×3 + split 1842；Wire 57/57；`integration-gpu` pull 1128、push 1128、
split 的 monolith 与 inproc 两臂各 1139/1139 且**名字+状态 0 差异**；
`integration-split` 11/11 全部按名 skip，且在没开构建选项的构建里**变红**；
`integration-verify` 930/930 零 `Fatal{`；retrace push 79/79、verify 79/79 全 armed、
OpenRA inproc 2/2 ssim 1.000 普查 0。

**它自己抓到两条"门不能因它存在的理由变红"（premortem class 9 的现场复发）**：
(a) 只看符号的探针会**对着 c0 的桩武装**——实测 11 条 `Split.` 条目全绿而跑的是 monolith；
现在改成与"不再残留 `Fatal{Unimplemented`"的合取，谁落最后一个桩谁武装这 11 条 + 两条负面对照。
(b) **SSIM 根本不能给 split retrace 把门**：一个 pull 库在 `MOBILEGL_TRANSPORT=inproc` 下照样 1.000。
守卫只能是 `nm | grep MG_Remote` 加 ConfigLoader 那行 INFO 日志——**因此每条 split 车道都必须是 INFO 级**。
**裁定：退出门 E2 据此加强**——OpenRA 的 SSIM ≥ 0.99 必须与"库身份 + 传输身份"两个守卫同时成立才算数，
单独的 SSIM 数字不构成证据。
(c) llvmpipe 上 monolith 臂是**采纳**臂（`mpr=1 pmap=0`）而 split 被钉在模拟臂，
所以 E3 的两臂走的是不同的 buffer 路径——A/B 能主张什么要相应收窄。

**ID-23（R-16：门的自证义务，第一波实测出来的本阶段头号缺陷类）**
三个包在同一轮里各犯了一次「测试自己构造了它本该观察的那个状态」：
p1 的自测 harness 抓任何 `SystemExit`，4 号对照因转义写坏而跳去触发 1 号的消息，"11 条全红"实为 10；
b1 的 persistent-map 用例**手写记录字段**，于是把生产者删掉它照样绿；
t1 只看符号的探针**对着契约桩武装**，实测 11 条 `Split.` 条目全绿而跑的是 monolith。
**规则**（已写进 BRIEF §13）：断言不得构造它要观察的状态；负面对照必须断言**自己的**失败串而不是"失败了"；
探针不得对着桩武装（与"不再残留 `Fatal{Unimplemented`"取合取）；
每条门在报告里带一行"我把它弄红过一次，方式是 X"，没有这一行按未验证处理。
这三种长相进 `MEASUREMENTS` 的缝分类学，作为 P4a 九类之外的**第十类：一条不会因自己的理由变红的门**。

**ID-24（裁定 p1 的 M-4：stamp 只按表断言新鲜度，抓不到"发射器被丢掉"）**
p1 落的是设计 A（边界按 class+mask 盖章），它抓不到一个 RECORD-SUPPLIED 字段的记录**从未到达**；
复审建议的"写者盖章、边界只撤回"会在几乎每个 verb 上误 abort，因为 applier 的镜像**跨 verb 持久**
（`bind_render_state`/`set_dynamic_state`/`set_sampler_views` 在没变化时被抑制），
而客户端的填充没有这个问题是因为它每个 verb 都抄满 class mask 里的每一个字段。
**裁定：P5 就用 A，设计 C 排到"给 `MOBILEGL_PIPE_VERIFY` 加 split 臂"的那个阶段（最早 P6）**。
理由：C 同时动 c0 与 w1 的文件，并且会改掉 v1 **此刻正在照着写**的规则；
而它要抓的失败在 P5 有两个替代 oracle——w1 的逐字节编解码往返，以及同一批发射器在 monolith verify 车道上的对照。
p1-v1 §7 的三设计表就是这条裁定的记录。

**ID-25（裁定 p1 的 q-1：`texture-remint-pull` 的 split Fatal 臂保持武装）**
风险是 E2 的 inproc 臂 abort（大声、方向安全），而不是画错一帧。
P4a 实测这条路在 780 个统计窗口里只响过 2 次、且只在两个 Iris in-world fixture 上；OpenRA 不该碰到它。
**保持武装**：如果它在集成时真的响了，那说明缩减路径确实够到了 image-bindable 重铸转换，
这与 P4a 的测量矛盾，属于我们想知道的信息——去查，不要去下掉这条臂。

**ID-26（p1 返工轮 1 落地）** `6fc1bcc8..9d40d3e9`（5 个提交），分支现为 `5175f9cc..9d40d3e9`。
M-1/M-2/M-3 全闭，另闭 M-5 与六条 MINOR。
**自测 harness 重做：`expect_trip` 逐条断言对照自己的消息，并加了一条 harness 对照
（1 号对照的编辑挂在 4 号的理由下必须被拒），现在是 15 条对照 / 15 个互不相同的退出消息 / 15 条各自断言**，
逐条证据在 `~/w7/p5-p1-selftest-evidence.log`。
验收复跑：G1 `.text +0` 0/0/0/0；unit 1791×3、split 1863；`integration-gpu` 两处 1117/1117 逐名相同；G14 0 删除。
**两条要带进 v1 的 prompt**：(a) `MGPipeServerClearVerbBoundary()` 从"可选"升为**applier 必须调用**
（spawn 下 `MG_Impl` 不在进程里、没人调 `MGPipeValidateForVerb`，`m_serverStampedVerb` 会终身 latch 成 TRUE，
于是 `InvalidateCompileEnv` 这种"本该被 sticky 豁免"的读会被计数、strict 下 abort）；
(b) `PipeApplier::m_residualPulls` 是**多余成员**，计数发生在访问器上，v1 删掉它而不是两头接线。

**ID-27（w1 复审）** 0 BLOCKER / 5 MAJOR / 8 MINOR / 3 QUESTION，放行。
所有数字复跑干净（G1 0/0/0/0 `.text +0`；1785×3 与 1893×3 在 `-j 8` 下 12 次车道运行零 flake；
itest 1117/1117 零名字漂移；`PipeApply.{h,cpp}` diff 为空；4632 B 与上限都独立验证过）。
**最狠的 M1：R-2 的第四条臂不覆盖 `MGHostSpan`。** `CheckHostSpanIsHonest` 不接 `SegmentTable`、
从不解析，调用者也不解析；复审伪造了一个 `Offset` 越过 `SEG_STAGE` 末尾、`Size=1024` 的 span，
两条 host-span 行**都没有 Fatal**，而 `DrawVbo` 那条还一路到了 `WireVerbSink::OnDrawVbo`——
那个头文件自己承诺那是一份"已验证的参数表"。P5 里是潜伏的（还没人发 span），**到 P8 就武装**，
而那时没人会再看这段代码。已要求当场关掉。
其余四条 MAJOR：编码器发布的是**消费者的** `SEG_STAGE` 游标（违反 `Ring.h` 写明的归属）；
`m_stageMarks` 在"总有一条记录在飞"时无界增长（SSIM 看不见、两场景车道够不着的稳态泄漏，
正是 RSS **斜率**那条记录项存在的理由）；解码器给四个目录里没有 `kReplySlot` 的 opcode 投了回复槽
（等 s1 按 `MGPipeCallFlagsFor` 定池大小的那天就会吊死一个等待者）；
`BufferSubDataResident` 的布局声称有尾巴而它的目录行否认。
**还有第三处标志碰撞没被钉住：`kReplySlot ≡ kRecVarTail`**——已要求把断言改成对两个枚举穷举。

**ID-28（s1 返工轮 1 落地）** `4cf59428..7c8d2b44`（5 个提交，分支总计 `5175f9cc..7c8d2b44`，9 个）。
B-1 与全部 M、除 m-5 外全部 MINOR 关闭。
**`CallMask()` 在无人设掩码时：两处推导连同那次 `MGPipeGetResourceOps` 读全部删除，
改为具名 `Fatal{UnsetCallMask}` 并点名两个 setter**；`Accept` 警告；`CallMaskIsSet()` 是非致命探针。
**M-7 用"取另一个选项"解决，比改文档好**：旋钮现在命名的是 **ring**，段在其上多加一个控制页，
于是上限**回到 4 MiB**，`CONTRACT-P5 §5` 与 `Config.h` 不用改就是对的。
（已转告 w1：把任何按 2 MiB 推导的测试常量／注释／报告文字改回来；4632 B 不变，余量由 ~400× 变 ~900×。）
复跑：四构建绿、G1 `+0`/0,0,0,0、unit 1785×3 + split **1867**、`integration-gpu` 1117/1117 且 2902 个名字相同、
`gen_protocol --check` 干净、Wire 车道 82/82 × 8 轮 `-j 8` 加 `SessionTest ×30` 零 flake。

**ID-29（t1 复审：R-16 的最强证据）** 0 BLOCKER / 7 MAJOR / 8 MINOR / 3 QUESTION，准入
（M-1/2/3 与 M-6 卡的是"武装车道"那个提交，不是这次合并）。全部数字复现。
**M-1 复审不是论证而是实做**：t1 换上的武装合取是一条 `file(STRINGS … REGEX)`，
grep 的是桩的**消息文本**。复审把 c0 六个桩里的 `Fatal{Unimplemented` 用 `sed` 改名成
`Fatal{NotYetImplemented`——每个入口仍然 `abort()`、什么都没实现——重建之后
**11 条 `Split.` 条目里有 8 条全绿，而且是端到端跑的 monolith**。
这正是 t1 自己 §3(a) 声称已经关掉的那个缺陷。
M-2：一条**注释**里含有这个标记就能让 11 条永远不武装；M-3：探针漏了另外两个桩文件，可以部分武装。
**裁定：武装条件必须是运行期的行为事实，不能是源码文本。**
关于源码文本的断言总能被编辑源码文本推翻；关于"这个进程实际做了什么"的断言不能。
选一个 monolith 下结构上不可能、真 split 下不可避免的量（会话存在且 emit 序号非零、
服务端 applied 序号非零、只有 ring 路径能推动的计数器），照 `BackendCapsPeek`/`P4aFinalFixPeek`
的形状用 harness peek 读。验证方式照复审：**保持所有字符串不变地把实现打断，看车道拒绝武装**。
其余 MAJOR 同一物种（"因为错误的理由通过"）：运行期传输守卫 grep 的裸子串
**一个 DEBUG 级 pull 构建也会打**（复审用构造的日志观察到全绿）；
`retrace-split` 是 `retrace-verify` **减掉它那条常开的负面对照**，而 E2 的宪章对照被悄悄丢了；
退出门 **E3(e)（小 ring 逼出至少一次背压等待）整条缺席且未提及**；
`wsl_p5_gate.sh` 的 E1/E3(a) 对照在**一个匹配到零个测试的过滤器上**打印"RED, as it must be"。
**并且：t1 只观察到 13 条门里的 5 条真的变红过。** R-16 的义务是每条门带一行
"我把它弄红过一次，方式是 X"，其余 8 条要么补上，要么显式标注未验证——不许看起来像已检查。

**ID-30（w1 返工轮 1 落地）** `bc969f9a..4f96e810`（9 个提交）。五条 MAJOR 全闭、8 条 MINOR 闭 7
（m5 自行消失——暂存的字节串不再携带 ring 头）。
M1 的证据：伪造 `{Ptr=nullptr, Seg=kSegStage, Offset=kStageBytes-8, Size=1024}` 打 `SetShaderBuffers`、
`{Offset=0xFFFFFFFF, Size=0xFFFF}` 打 `DrawVbo`，两个子进程现在都以 SIGABRT 死于
`Fatal{ProtocolCorruption, "host-span"} … does not lie inside that segment (R-2.3 arm 4)`。
**`~/w7/p5-w1-redcheck.py` 逐条把修复打断、重建、要求对照变红：8/8，`CONTROLS_THAT_DID_NOT_GO_RED=none`。**
**其中两条是先修测试才红得起来**（M3 的循环每轮都把队列排空了；`StageMarksHeld()` 报的是存活计数，
而那个数在 vector 泄漏时始终是 1）——R-16 在同一个包里第二次现形，这次被红检查抓住了。
M5 是 w1 自己的 bug 不是目录的：`BufferSubDataResident` 是 buffer 半边，非零 `RegionCount` 现在 Fatal。
M2 在根上修掉：`SEG_STAGE` 成为真正的编码器本地线性分配器，`RingControl` 的 stage 三元组**没有人写**，
**因此 s1 不得给 `RingCursorSet::Stage` 挂 `RingConsumer`**（已转告）。
验收：G1 0/0/0/0 `.text +0`；unit 1785×3 + split **1902**（60 条用例）；`integration-gpu` 两处 1117/1117 零漂移。

**ID-31（裁定 w1 的 M4：四条 acceptance 行补 `kReplySlot`）**
`CONTRACT-P5.md:74` 说 `DECLINED` 是"四个 `Bool` acceptance 入口说 false 的方式"，
但目录没给这四行 `kReplySlot`，于是解码器给四个"按标志不该有回复槽"的 opcode 投了槽；
等 s1 按 `MGPipeCallFlagsFor` 定池大小，就会吊死一个等待者。
**裁定：目录是错的。** `ResourceCreate`/`ResourceRespecify`/`ResourceSubData`/`SetTextureParams`
补上 `kReplySlot`，让 `MGPipeCallFlagsFor` 成为唯一真源（s1 据此定池、w1 据此投槽）。
不动任何 opcode；`PipeCatalogueTest` 里 `kReplySlot` 计数 10 → 14 是合法的测试数据修改。
同时要在契约里写明：**栅栏之下一条 `kReplySlot` 行不增加阻塞**，因为回复是在栅栏本来就要做的那次等待里读的，
否则这个标志"从不阻塞"的文档说法看起来自相矛盾。已派给 c0（`PipeCalls.def` 是它的文件），
连同"改掉那条邀请缺陷的生成注释 + 考虑对两个枚举穷举 static_assert"。

**ID-32（t1 返工轮 1 落地）** `8d0850bd..1abe716f`（包内共 6 个提交）。七条 MAJOR 全闭、8 条 MINOR 闭 7。
M-1/2/3 的修法是**把源码探针整个删掉**。
**新的武装条件是行为性的**：一条 split 用例只有在运行中的进程报告
`MG_Config::Transport != Monolith` **且** `ClientSession::Active() != nullptr` **且**
`ImplementedVerbCount() > 0` 时才允许断言，并且只有在**这条用例期间客户端编码器的记录序数真的动过**才允许通过。
**复审当初那个 `sed` 改名的变异现在留下 21/21 全部 skip；一个"存在但什么都不发"的假客户端能让 21 条里的 19 条变红。**
门的红检查从 5/13 变成 **11/15**（13 行变 15 行）。
另外：`retrace-split` 补回了 pull-库对照并把 `build-linux` 加进 `needs`（E2 的发射器丢弃对照记成 c1 的债）；
补了 `DirectGLES.Split.SmallRing.` 车道（E3(e) 的小 ring 背压）。
验收：G1 `.text +0` 0/0/0/0、G2 0、G14 0/11、unit 1785×3 + 1842、
`integration-gpu` pull 与 push 各 1128/1128、split 两臂 1149/1149 diff 0、
`integration-split` 21/21 skip、OpenRA inproc 1/1 ssim 1.000 普查 0。

**ID-33（`CapsCodec.cpp` 的合并解，唯一一处真冲突）**
w1 与 s1 各写了自己那半：s1 保留了 c0 的四个编解码桩、加了消费者位的三条 static_assert 与一个
`CapsAbiFingerprint()`；w1 实现了四个编解码器（493 行），**同样带那三条 static_assert（内容一致）**，
并且自己也写了一个 `CapsAbiFingerprint()`——但它的**严格更强**：除三个 `sizeof` 外还混入
format-cap 表的两个维度、两个编解码版本号、以及 `MGPWireOp::kOpCount`（两个 sizeof 相同却字段顺序不同的构建，
任何 sizeof 都看不见，所以多混几个结构性常量是对的）。
**解：整份取 w1 的，再把 s1 版本里的 `MOBILEGL_ABI_VERSION(...)` 一项并进去**，
并确认 s1 的 `Transport::MixAbiFingerprint` 若因此无人调用，是删掉还是另有用处。

**ID-34（s1 的 stage 游标确认，结果比报告的问题更糟）** `7c8d2b44..69cf5a0c`（分支总计 10 个提交）。
它从来没有给 `RingCursorSet::Stage` 挂过消费者——但它**一直在建一个 stage `RingProducer`**，
那比"死代码"更糟：`stageHead` 被发布而没有任何东西推进两个 tail，于是
**`FreeBytes()` 在第一圈就归零并且永远不恢复**。已删除，连同 `WaitForStageSpace` 与二次幂圆整；
编码器的 stage 参数现在收 `nullptr`。三元组保留在 `RingControl` 里、**显式记为"故意是死的"**，
并由一条驱动 500 条记录、断言三个游标始终为 0 的测试钉住——这正是我要求的"不许留成半接线"的正解。
另外：`Pop` 现在要求 `kind == kRingPadRecordKind` 与 `kRecPad` 同时成立，四条 static_assert 钉住全部三处碰撞，
borrow 的出现点被具名。验收全绿（G1 +0、split unit 1870、1117/1117、Wire 85 × 8 轮干净、RingTest 21/21）。
回复池的定容等 c0 的标志。

**ID-35（c0 第三轮落地）** `b5b6cef2..225819ab`。
四行按**枚举每一个有返回值的 `MGPipeApply*`** 核实后确定（不是照我给的名单照抄）：
`ResourceCreate`/`ResourceRespecify`/`ResourceSubData`/`SetTextureParams`
（`PipeApply.h:820/:868/:897/:1023`）；`kReplySlot` 10 → 14，无 opcode 移动。
G1 `.text +0`、27811→27811、0/0/0/0；unit 1785×3 + split 1843；Wire 58/58；`integration-gpu` 1117/1117 diff 0。

三条要记的：
(a) **`PipeCatalogueTest` 里本来就没有 `kReplySlot` 的计数钉**——任何分支上都没有。c0 补了（计数 + 四行逐名）。
(b) **w1 那条发现比树里实际支持的要强。** `RingProducer::Reserve` 会把 `kRecPad` 掩掉，
所以 `kVarTail` 其实是有防御的——一个位、一条路径。真正没有防御的是
`kHostSpan`→`kRecBorrowSlot`（生命期谎报）与 `kReplySlot`→`kRecVarTail`（幻影尾巴，现在还涨到 14 行）。
c0 第一版测试照着"戏剧性版本"断言，**失败了**；落地的那版把三种形状连同那次掩码一起钉住。
这是本轮第三个"去实跑而不是相信描述"的例子。
(c) **`p5/w1` 与 `p5/b1` 动过 c0 专属文件**（`gen_pipe.py`；`MGPipeTypes.h`/`PipeFields.def`/`CMakeLists.txt`），
区域互不相交、合并干净，**但 `generated/*.inc` 必须重新生成，绝不手工合并**。

---

## 整合计划（等 b1 返工轮）

顺序：`feat/disaggregated`（已含 c0 前九个提交）→ c0 新提交 → w1 → s1 → p1 → b1 → t1。
1. 唯一手工冲突 `CapsCodec.cpp`：整份取 w1，把 s1 的 `MOBILEGL_ABI_VERSION(...)` 一项并进指纹。
2. 全部合完后**重新生成** `MG_Pipe/generated/*.inc` 与 `MG_Remote/Protocol/generated/protocol_generated.h`
   （`gen_pipe.py`、`gen_protocol.py`），不要手工合并任何生成物。
3. 跑 `wsl_p5_gate.sh` 五部分 + G1/G2/G14 + 四条 unit 车道 + `integration-gpu` 三臂逐名 + retrace。
4. 推送后再开第二波 v1/c1。

**ID-36（b1 返工轮 1 落地）** `5c45ec2a..2a539881`（7 个提交，包内共 15 个）。
B-1 两半全闭，另闭 M-2/3/4/5/7/8 与六条 MINOR。
**B-1 的修法**：map 的上升沿发布一条真实的单块记录，于是探针不可能在推送跑起来之前就 latch 成 clean。
**负面对照证据**（`~/w7/p5-b1-negctl.sh`）：对照 2/2 通过 → 删掉 wire 生产者 **1/2 失败** →
删掉上升沿记录 **0/2 失败** → 恢复 2/2 通过。这正是 R-16 要的形状。
验收：G1 0/0/0/0 `.text +0`；unit 1807×3 / 1864；
**新增的 `build_verify_split` 首次运行就是红的，红在 M-8 点名的那条绊线上**（然后修好变绿）；
G2 0、G14 0 删除；`integration-gpu` monolith 两臂 1117/1117 逐名相同。
留一条给 v1：M-6 的快照那一半（见下条裁定）。

**ID-37（裁定 b1 的 M-6 后半：flush 阶梯的快照范围）**
**快照范围恰好是记录自己声明的那个范围，永不加宽。** 服务端的阶梯只在暂存字节上操作；
若它需要任何一个不在已暂存范围里的字节，那是 **Fatal**，不是静默地回头再读一次。
依据：tier 1 的 `INVALIDATE_RANGE` 是"旧字节已死"的断言，把它加宽已经静默打烂过一次 GPU 写的数据
（`Managers.cpp:1126-1129`）。已写进 v1 的 prompt。

**ID-38（第一波整合完成）** 顺序 c0 → w1 → s1 → p1 → b1 → t1 全部合入，
**唯一手工冲突是 `CapsCodec.cpp`**，按 ID-33 解：整份取 w1，并把 s1 版指纹里的
`MOBILEGL_ABI_VERSION(...)` 一项并进 w1 的 FNV 链（附一句为什么：上面那些 sizeof 抓的是"结构变形"，
这一项抓的是"每个结构尺寸都没变而协议变了"，是其余混入项唯一看不见的那种断裂）。
`Transport::MixAbiFingerprint` 保留——它是 s1 会话侧的真 helper，有自己的测试。
合完后重新生成全部生成物：**`gen_pipe --check` 报告"已是最新"**（说明各包自己生成得对、合并也没有破坏它们），
`--self-test` 9 个对照、`gen_pipe_field_ownership --check` 最新、dirty-surface 绿。
整合头 `32563993`。全门已在后台开跑（`~/w7/p5-integrate-gate.log`）。

**ID-39（第二波开工）** 不等全门跑完就开 v1/c1，因为五个包各自都过了自己的四条车道验收，
基座风险低，而这两个包是整个阶段的长杆。它们从 `32563993` 分叉；若全门查出问题我转告。
v1 的 prompt 带了：p1 的 stamp 规则（含"离开 applier 必须 clear"与"删掉 `m_residualPulls`"）、
ID-37 的快照裁定、s1 那条 `Fatal{UnsetCallMask}` 现在需要 v1 去当那个"设掩码的人"。
c1 的 prompt 带了：CONTRACT §7 的 71 槽三分类（不许重推）、
**41 处 null 检查里的能力探测必须改成 caps 镜像读**、b1 那三个零调用者入口要接在 verb 记录**之前**、
以及 t1 记在 c1 账上的两笔债（E2 的发射器丢弃对照；谁落最后一个桩谁武装 21 条车道）。
两个 prompt 都带了 R-16 的红检查义务。

---

## Integration, round 2 (2026-09-16, after the Sep 11 quota outage)

### ID-40 — The wave-1 lineage is rewritten onto the GitHub branch; the pipe repo's stray identity is gone

**What was found.** GitHub `feat/disaggregated` was at `ff2994d9`, which is NOT an ancestor of our
integration head `32563993`, yet `git diff --stat 5175f9cc ff2994d9` is empty and
`git cherry 32563993 ff2994d9` has no `+` line: the two lineages are tree-identical and patch-identical
from `92dffb81` (Sep 8) onward. The Windows reflog says why: `feat/disaggregated@{0}: filter-branch:
rewrite`. Every commit made from `~/w7/pipe` and its worktrees since Sep 8 carried
`committer: rereview <rereview@local>` (author too for 43 of the 81), because `~/w7/pipe/.git/config`
held `user.name=rereview` / `user.email=rereview@local` — a repo-local override that every
`git worktree add` inherits. The user rewrote all 81 to `Swung0x48 <swung0x48@outlook.com>` on the
Windows side and force-pushed; our 58 wave-1 commits (`5175f9cc..32563993` plus `p5/c1`) had the same
defect and were not on GitHub in any form.

**What was done** (`~/w7/notes/tools/p5_lineage_repair.sh`, log `~/w7/p5-lineage-repair.log`):
the local override was unset (the global config is the user's identity); `git filter-branch` over
`^5175f9cc feat/disaggregated p5/{c0,w1,s1,p1,b1,t1,c1,v1}` with an env-filter that maps
`rereview@local` to the user's identity and a parent-filter that maps `5175f9cc` to `ff2994d9`. Merge
commits (including the hand-resolved `CapsCodec.cpp` merge, ID-33) keep their trees. Verified: `ff2994d9`
is now an ancestor of `feat/disaggregated`; the new head's tree equals `32563993`'s; `p5/c1`'s tree equals
`805f3322`'s; 59 commits, all `swung0x48@outlook.com` as author and committer; the GitHub push was a
fast-forward (`ff2994d9..5e5bf7b9`).

**SHA map** (old to new): `feat/disaggregated 32563993 -> bfaf5f9d`; `p5/c1 805f3322 -> d628d906`;
`p5/c0 -> 6e858fa3`, `p5/w1 -> 79b78665`, `p5/s1 -> f33e5d60`, `p5/p1 -> 3aa48bcc`, `p5/b1 -> 505b5084`,
`p5/t1 -> 949ed1f0`, `p5/v1 -> bfaf5f9d`. The P4a landing commit `a29807cc` is `0648b0d9` on the GitHub
lineage; the P5 base `5175f9cc` is `ff2994d9`. Reports under `p5-results/` that cite the old SHAs cite
tree-identical commits. `~/w7/notes/tools/wsl_p5_gate.sh`'s `BASE` is now `ff2994d9`.

**What would overturn it.** A `+` line in `git cherry ff2994d9 32563993` (there is none) or a non-empty
tree diff between any old/new pair (there is none). **Standing rule from here:** before the first commit
of any phase, `git var GIT_COMMITTER_IDENT` in the WSL tree must print the user's identity.

### ID-41 — G5: `FlushPendingRangesFrom`'s P5 parameterisation is admitted, isolated, and re-pinned

`scripts/p3a_untouched_regions.sh ff2994d9 HEAD` and the p4a twin are RED on the integration head:
`FlushPendingRangesFrom` moved (`37fc94ff...` to `172b0222...`). The change is b1's: two defaulted
parameters `hostBaseFrom`/`hostBaseTo` (`kHostBaseCoversWholeStore`), a `MOBILEGL_PIPE_VERIFY`-only
`StageSnapshotTooNarrow` log, and the tier-1 access computation moved into `InvalidateFlushAccessFor`.
G1 reports `.text +0`, 0/0/0/0 on the pull build, so the pull arm (`FlushPendingRangesNow`) is untouched
and the push arm's codegen for every existing caller is byte-identical.

**Ruling.** This is the intended P5 change to the eleventh row, not a drift: R-11 makes the server's
`hostBase` a `SEG_STAGE` snapshot that covers a RANGE, and v1's work-in-progress already passes a real
extent and red-checks the widening refusal ("coverage-widened ... Managers.cpp:1126-1129"). G5's own
header text (`p3a_untouched_regions.sh:40`) says the push arm "is compared against a sha pinned at the
commit where it was reviewed - see PINNED_FUNCTIONS", but the implementation only consults the pin when
the function is missing at `<ref-a>`. So: (a) the eleventh row is compared against `PINNED_SHA_...` ALWAYS,
in both scripts, and the pin becomes the reviewed P5 body's sha with the commit and this ID beside it;
(b) the other ten (p3a) and P4a's regions stay ref-a-vs-ref-b; (c) `--self-test` gains a negative control
that perturbs the ladder and must go red against the pin; (d) both scripts go green on `ff2994d9 HEAD` and
on the P3a/P4a base refs CI passes. Owner: b1, round 2. No edit to `Managers.cpp` is part of this.

**What would overturn it.** A pull-build symbol diff (there is none), or a P5 caller found to depend on
the widened `INVALIDATE_RANGE` behaviour that the refusal now blocks.

### ID-42 — The persistent-map membership assertion never ran on the adopted arm (provisional; b1 confirms)

Seven `PersistentCoherentMapScenario` entries fail in the split build under `MOBILEGL_TRANSPORT=monolith`
(3 DirectGLES, 3 DirectVulkan, 1 `PersistentMapArm`), every one at
`PersistentCoherentMapScenario.cpp:270`: `AssertMembership` expects `live == true` unconditionally. In the
push build the tracker is "unavailable" and the assertion returns before asserting, so it was never
exercised. In the split build, monolith transport leaves the map ADOPTED (tier T1; `IsLivePersistentMap`
answers false on its `IsBackendPersistentMapped()` row, by design - "the predicate must read the chain,
not the tier"). The claim "a PERSISTENT|WRITE|COHERENT non-FLUSH_EXPLICIT map is a member by
construction" is only true on the emulated arm. Ruling: the expectation is `live == (arm == emulated)`,
asserted from the same `PeekBufferIsAdoptedPersistentMap` answer `AssertOrRecordArm` already reads; the
arm-independent statement ("the predicate reads the chain") is pinned by a unit test on
`PersistentMapTracker::IsLivePersistentMap`, not by an integration entry. R-16: b1 must show the case red
once on the emulated arm (inproc, once c1/v1 land - or by forcing tier T2) before it is called fixed.
Overturned if `PeekBufferIsLivePersistentMap` answers false on the EMULATED arm.

### ID-43 — `RingTest.TheTwoFlagSpacesAreDisjointByTranslation:774` states a defence that s1 has since doubled

c0's control (commit `225819ab`, now `6e858fa3`) asserts that a header stamped with the CALL flags outside
`Reserve` "vanishes into Pop's wrap-filler skip". s1's fix (`f33e5d60`: a wrap filler must carry kind
`kRingPadRecordKind` as well as the flag it shares with `kVarTail`) means Pop no longer skips a
`DrawVbo`-kind record on the flag alone, so the stamped record is popped and `EXPECT_FALSE` fails. The
production behaviour is the ruled one; the test states the pre-s1 tree. Ruling: the case asserts the new
behaviour (the stamped record is popped, with the lying `kRecBorrowSlot`/`kRecVarTail` bits visible), and
keeps a control that names the kind check as the defence - with the kind check removed, the record must
vanish again. Owner: s1, round 2. `PipeWireCodecTest.NoReplyIsWrittenForARowTheCatalogueGivesNoReplySlot`
(:1203, ID-31 fallout: `ResourceCreate` now carries `kReplySlot`) stays with c1's round 2 as already
assigned.

### ID-44 — `dev` is merged; every wave-2 branch merges the integration head first; the G1 baseline is re-taken

`feat/disaggregated@5e5bf7b9` = wave 1 + `github/dev fff9d639` (DirectVulkan resident-buffer ordering +
coherent trace copies; 3 files, `merge-tree` clean, all four flavours build). Every wave-2 branch
(`p5/v1`, `p5/c1`, `p5/b1`, `p5/s1`) merges `feat/disaggregated` before doing anything else, so one G1
baseline serves everyone. That baseline is re-taken as the pull build of `0648b0d9` (P4a landed) +
`fff9d639` on the throwaway branch `p5/g1base` (`~/w7/notes/tools/p5_g1base.sh`, log
`~/w7/p5-g1base.log`): it replaces `~/w7/p5-before-libMobileGL.so` (the old one is kept as
`~/w7/p5-before-p4a-libMobileGL.so`) and writes `~/w7/p5-before-libMobileGL.so.READY` holding the
baseline commit. **A G1 run before that marker exists is against the wrong baseline and is not a
verdict.** `~/w7/p5-before-ctest-names.txt` is unchanged: `fff9d639` adds no test.

### ID-45 — Wave 2 is relaunched from where the Sep 11 quota outage stopped it

v1's work-in-progress (9 files, +1602/-45, plus `StagedShadow.h` and `ServerLoopTest.cpp`, uncommitted
in `~/w7/p5-v1`; copy at `~/w7/notes/p5/v1-wip.diff`) is resumed in place; its red-check log
`~/w7/p5-v1-redcheck.log` ends `P5_V1_REDCHECK_ALL_WENT_RED` with the baseline green before and after,
and its last words were "Found a real ordering defect via the red-check baseline. Let me fix it." c1's
round 2 is R-17 as issued. b1's round 2 is ID-41 + ID-42; s1's round 2 is ID-43. The 175 `inproc`
aborts on the wave-1 head (no client installed) are not triaged: they are superseded by c1 + v1 landing,
after which the `inproc` lane arms for real and is judged on its own.

**ID-43, amended after s1's round 2 (`4ba14fa4`):** the third collision (`kReplySlot -> kRecVarTail`) does NOT lie on this record - `draw_vbo` carries no `kReplySlot`, so bit 4 arrives clear and the case pins it clear; only `kRecPad` and `kRecBorrowSlot` arrive lying. The case also gained the opposite control (a header with `kRecPad` AND kind `kRingPadRecordKind` still vanishes), so "the record is popped" cannot be satisfied by removing the skip. Red once by deleting `&& header.kind == kRingPadRecordKind` in `Ring.cpp`'s Pop; red at `RingTest.cpp:784` on its own message. Accepted and merged.

### ID-46 — A cross-family review of wave 1 (codex, GPT) produced ten majors; every one is verified before it is assigned

The user opened the `codex` CLI to the integrator (2026-09-16). Its first use was a READ-ONLY adversarial
review of `ff2994d9..5e5bf7b9` by a different model family, on the R-16 premise that a same-family
review shares the author's blind spots. Report: `p5-results/wave1-codex-review.md` (10 findings, all
"major", each with a concrete unexecuted perturbation): (1) `SEG_STAGE` wrap arithmetic rejects a blob
that fits when the stage is empty; (2) the default 8-slot reply pool holds 1 MiB minus a 16-byte header,
so a 512x512 RGBA8 `ReadPixels` cannot be posted; (3) blob validation accepts any mapped segment, not
only `SEG_STAGE`, and audit poisoning ignores the others; (4) the wire `MapPersistent` decline skips the
adoption-tier refusal; (5) `SessionTest`'s pad-bit regression control never sends the bit (`Reserve`
strips it); (6) the ABI-sensitivity gate tests `MixAbiFingerprint`, which production no longer calls;
(7) the null-union control never reaches either handshake guard; (8) three CI negative controls accept
any non-zero ctest exit; (9) `gen_pipe.py`'s `expect_trip` still accepts any `SystemExit` (the defect p1
fixed in the sibling generator); (10) two `SessionTestDeath` controls use an empty diagnostic regex.
Findings 5-10 are R-16 in their purest form: tests that would stay green with the code they name deleted.

**Ruling.** Nothing is assigned on the review's word. A verification agent executes all ten
perturbations in an isolated worktree (`~/w7/p5-verify`, branch `p5/verify`, off the integration head
`4ba14fa4`) and reports CONFIRMED/REFUTED/PARTIAL with verbatim outcomes
(`p5-results/wave1-codex-verify.md`). Confirmed findings then go to their owners (w1: 1, 3; s1: 2, 5,
6, 7, 10; w1/b1: 4; t1: 8; p1: 9) as a "wave 1.5" fix round, each fix shipping its own red-once
control. A fix that would change a wire struct, a segment size or a catalogue flag (finding 2 is the
candidate) needs its own ID before it lands. What would overturn the ruling: a verifier that
cannot build the tree in isolation, in which case the perturbations run on the integration tree
after wave 2 lands.

**ID-41, amended after b1's round 2 (`f63c9483`):** the eleventh row is now compared against its pin ALWAYS in both scripts; the pin is `172b0222…` at `3dadd4c1` (b1's wave-1 commit that parameterised the ladder), and re-pinning means editing BOTH scripts. Correction to the ruling's justification: v1's work-in-progress never passed `hostBaseFrom/To` — it asserted coverage and, at landing, moved that assertion OUT of the pinned body to `RequireStagedCoverageForPendingRanges` at the two call sites whose base can be a server shadow (v1-v1.md §5.2); the two defaulted parameters currently have no caller. The re-pin stands on b1's change alone. Debt for t1 (wave 1.5): three stale comment lines in `.github/workflows/test.yml` (`:2328` "pinned at 3e298c9a", `:2340` "six canned controls", `:2377` "FOUR negative ones") — b1-v2.md §1.4 has the exact text.

**ID-42, amended after b1's round 2 (`6cabc6b0`):** confirmed, with one correction — "tier T1" was wrong. `MOBILEGL_IPC_ADOPT_TIER` is 2 throughout; the adopted arm is reached under monolith transport because `PipeApply.cpp:2005` ANDs R-6's decline with `Transport != Monolith`, so a split BUILD under monolith TRANSPORT mints like push. b1 measured a six-cell matrix (b1-v2.md §2.4): no knob reaches the emulated arm in a monolith lane; it is reachable only when `Transport != Monolith`, i.e. the `DirectGLES.Split.*` lanes, which skip until the session lands. The seven reds are zero (split monolith 1149/1149); the arm-independent half is pinned by a unit test on `IsLivePersistentMap` (`SplitBufferTest.cpp`). **The emulated-direction red-check is PENDING the joint merge** — the exact three-step command is in b1-v2.md §2.4 (`return false` at the top of `IsLivePersistentMap` must turn `DirectGLES.Split.PersistentCoherentMapScenario` red on its own message) and the integrator runs it on the joint head before calling ID-42 closed. b1 also made the gate print `NO MEASUREMENT` + reason for a split counting lane whose case did no work (E3(c) is not closed; it was never closed).

### ID-47 — `SEG_REPLY` is sized from the largest P5 read: 16 MiB, eight slots of 2 MiB; an oversize read is refused by name at the client

Verification of ID-46's finding 2 (`p5-results/wave1-codex-verify.md` §2) made it worse than the review
said: `MaxReplyBytes()` is 1,048,560 (1 MiB minus the 16-byte header), a 512x512 RGBA8 answer is 16 bytes
too big, and the E2 retrace harness's snapshot forces a full-surface `GL_RGBA/GL_UNSIGNED_BYTE` read of
OpenRA's 640x480 surface = 1,228,800 bytes through the interposer - `Post` would abort the day c1 lands.
CONTRACT §2 row 23 says "size slots from the largest scenario read"; the default did not.

**Ruling.** `SEG_REPLY` becomes 16 MiB with eight slots of 2 MiB (`MaxReplyBytes = 2 MiB - 16`); the
canonical sizes are now 8/32/16 MiB + 256 KiB. `ProtocolSmokeTest.cpp`'s size pin, CONTRACT §2 row 23 and
every canonical-size sentence (CONTRACT §2, BRIEF §2, this file's §2) change in the SAME commit, citing this
ID; if the ABI fingerprint mixes segment sizes it moves, and that is correct (both roles are one build in
P5). A read whose answer would exceed `MaxReplyBytes` is refused at the CLIENT before emission with
`Fatal{ReplyTooLarge, "ReadPixels <w>x<h> <format> <bytes> > <cap>"}` - never truncated, never a server-side
abort the client cannot name - and the control is a boundary pair: exactly `MaxReplyBytes` bytes posts,
one more is refused by that message; plus the verifier's case that posts `640*480*4` into the DEFAULT pool
and must succeed (red once by reverting the geometry). Reads larger than 2 MiB (a 2400x1080 RGBA8 device
surface is ~10.4 MB) are a P6 debt - chunked readback or a dedicated readback carrier - recorded in the
ROADMAP by the integrator, not solved here. Owner: s1 (`Transport/ReplySlot.h`, `SessionRings.h`,
`ProtocolSmokeTest.cpp`, the contract row - c0's file, granted for this row only).
What would overturn it: an exit-gate read larger than 2 MiB in P5 (there is none: E2 is 640x480).

### ID-48 — Wave 1.5: the ten confirmed findings go back to their owners, s1 at the third round goes to the premium tier

All ten of ID-46's findings were CONFIRMED by execution (0 refuted, 0 partial;
`p5-results/wave1-codex-verify.md`, which also holds the fix shape, the red-once control and the owner
per finding). Assignments: **w1** - 1 (stage wrap arithmetic: an empty stage must take a blob that fits),
3 (`RequireDeclaredBlob` requires `Seg == kSegStage`; the audit poison covers what it must), 4 (the codec's
`MapPersistent` arm goes through `AdoptTierIsEmulate()`, so T0/T1 are the named Fatal the contract §5
promises); **s1** - 2 (ID-47), 5 (the pad-bit case is fixed or deleted in favour of ID-43's `RingTest`
control, and s1-v1.md §8.2 re-pointed), 6 (`CapsAbiFingerprint` is what the sensitivity case drives;
`MixAbiFingerprint` either becomes its implementation or is deleted - it has zero production callers),
7 (the null-union frame is driven through `Accept`/`Start` so the two guards are what the case protects),
10 (both death controls name their diagnostic); **t1** - 8 (both CI negative controls assert their own
failure reason; the verifier's stubbed harnesses under §8 are the shape) plus b1-v2.md §1.4's three stale
`test.yml` comment lines; **p1** - 9 (`expect_trip` takes the guard's required message; `sys.exit(0)`
cannot count as a trip). This is s1's THIRD rework round, so by the user's rule it runs on the premium
tier; w1, t1 and p1 are at their second. Every fix ships its red-once line; the package reports the
verifier falsified (`s1-v1.md:134`, `:338`, `§8.2`; `t1-v1.md:419`; `w1-v1.md:341`) are corrected in each
owner's v2 report. `PipeWireCodecTest.NoReplyIsWrittenForARowTheCatalogueGivesNoReplySlot` stays c1's.

### ID-49 — `ReadPixels` crosses TIGHT: the server reads with neutral pack state into the reply, the client scatters per the app's pack state

The cross-family review of v1 (`p5-results/v1-codex-review.md` finding 1, blocker) shows the seam: c1's
`PackedReadbackBytes` computes `DstSize` without the initial skip (`GL_PACK_SKIP_ROWS/PIXELS`), v1
allocates exactly `DstSize` and calls the real backend, which honours the pack state it can see and writes
past the allocation (a 4x3 RGBA8 read with `ROW_LENGTH=8, SKIP_ROWS=1, SKIP_PIXELS=2`: 80 bytes allocated,
last write at 120). The joint inproc lane already shows the shape: the two SEGFAULTs v1's census omitted are
both `DepthReadbackHonoursThePackPixelStoreParameters`. Even without skips, copying the whole scratch back
overwrites the application's row gaps.

**Ruling.** Pack state never crosses for a read. The server performs the read with NEUTRAL pack state
(`ROW_LENGTH 0`, `SKIP_* 0`, `ALIGNMENT 1`, restored afterwards) into a tight `w*h*bytesPerPixel` extent
that IS the reply payload; `DstSize` is that tight size and nothing else, so the ratified "`OnReadPixels`
writes exactly `DstSize` bytes" now means "into the reply", and the CLIENT scatters the tight rows into the
application's pointer according to the application's own pack state (row length, skips, alignment), which
it holds. Owners: v1 - the server-side neutral read (`ServerVerbSink::OnReadPixels` / `PipeApplier.cpp`),
with a control that a non-default server-visible pack state cannot change the reply's size or bytes; c1 -
`PackedReadbackBytes` becomes the tight size and the scatter lands in the emitter, with the review's
exact case as the control (sentinel-filled destination, `ROW_LENGTH=8, SKIP_ROWS=1, SKIP_PIXELS=2`, 4x3
RGBA8: the gaps keep their sentinel, the 12 pixels land where GL says). Under monolith the path must be
byte-identical to today (G2/G14). Overturned if a P5 exit-gate read needs a packed form the reply cannot
carry (none does: E2 reads tight RGBA8).

### ID-50 — v1's cross-family review: two blockers and eight majors, verified before assignment; the census correction stands

`p5-results/v1-codex-review.md` (codex, read-only): blockers 1 (ID-49) and 2 (a control request posted after
the apply thread's final cancellation pass waits forever - `ServerLoop.cpp:310/:322/:428-446`); majors 3
(draw-time ensure prefers the client's `MappedData()` and republishes it into `hostBytes`, so a corrupt staged
upload can render the frontend's correct bytes and the audit poison cannot see it), 4 (whole-store uploads
bypass `RequireCoverage`), 5 (`DropAll` frees shadows while twins keep `hostBytes`), 6 (seven backend-internal
capability reads still go through `pActiveBackendObject`), 7 (make-current/release-current still reach the
native EGL unconditionally - "holds the context for life" is not implemented at that layer), 8 (the R-11
tests test `StagedShadowStore` as a container, not Managers.cpp's wiring), 9 (`p5-v1-redcheck.py` accepts any
non-zero ctest exit), 10 (the park tests arm on `ParkCount`, which increments before the wait); minor 11
(the logged RESOLVED affinity mask is the requested one, not `sched_getaffinity`'s). Report corrections
accepted now: the inproc census is 428/185/506/28 **+2 SEGFAULT** (= 1149); the 506 aborts are attributed
to class-C by one probed example, not per-abort; the joint logs hold no `Fatal{` lines (ctest captures only
gtest output), so the barrier-off/strict failure NAMES are unverified until the MobileGL log files are read.
As with ID-46: nothing is assigned on the review's word - the same-family review (running) is merged with
this list, then one verifier executes every perturbation on `~/w7/p5-v1` / the joint worktree (ASan where a
finding needs it) and v1's round 2 takes what survives. Finding 1's design half is ruled now (ID-49)
because c1's round is in flight and owns the client half.

### ID-51 — The first CI run on the dev-merged head is red for three known reasons and one new one

GitHub `Test` run 35079459114 on `5e5bf7b9` (ubuntu-24.04): job "MGPipe generators and hygiene gates" failed
at G5 (ID-41, fixed at `ce53553c`); job `integration-split`'s "Unit tests on the split runtime" failed 3 of
1974: `RingTest.TheTwoFlagSpacesAreDisjointByTranslation` (ID-43, fixed at `4ba14fa4`),
`PipeWireCodecTest.NoReplyIsWrittenForARowTheCatalogueGivesNoReplySlot` (ID-31 fallout, c1's round 2), and
one never seen locally: `SessionTest.TheMemoryLedgerIsPerRoleAndIsReleasedOnClose` -
`SessionTest.cpp:235: Expected PeakRssBytes >= CurrentRssBytes, actual 4784128 vs 4849664`. Diagnosis to be
confirmed by s1 (added to its round 3 as item 6): the ledger asserts the kernel's `VmHWM` against the
kernel's `VmRSS`, and Linux folds per-thread split-RSS counters into the mm lazily, so a fresh `VmRSS` can
exceed `VmHWM`; the ledger must own its peak (running max of its own samples). The APK lane stays the
chronically red emulator lane and is not authoritative. The next run (`ce53553c`) is the one to read after
c1 and s1 land.

### ID-52 — v1's two reviews merged: the confirmed set goes to v1's round 2 now, five codex items are verified in parallel

The same-family review (`p5-results/v1-review-v1.md`, executed) and the cross-family one
(`v1-codex-review.md`, proposed) overlap on the worst of it: R-11 has no gate that fails when its production
wiring is deleted (M-1 = codex 8 - the reviewer restored `hostBytes = raw - offset` AND neutered
`RequireStagedCoverageForPendingRanges` and all eleven of v1's cases stayed green); the audit row is vacuous
because `liveHostBase()` prefers the client's `MappedData()` whenever a frontend object exists, which under
inproc is always (M-2 = codex 3); two whole-store readers bypass the coverage refusal (M-3 = codex 4);
`DropAll` frees shadows the twins still name (m-5 = codex 5); the affinity gate cannot fail for its reason
(M-4, with codex 11's "resolved mask is the requested one"). The same-family review alone found: M-5 (the 28
wrong-answer joint failures are undiagnosed and §8's "what passes" list is wrong), M-6 (`ShutdownSplitRoles`
skips the apply thread when `ClientSession::Start` failed early), M-7 (`RunOnApplyThread`'s no-thread arm is a
silent EGL-on-the-app-thread fallback) and seven minors. The cross-family review alone proposed: C1 (ID-49,
ruled), C2 (control request posted after the final cancellation pass waits forever), C6 (seven backend
capability reads through `pActiveBackendObject`), C7 (make-current not once-only at the native layer), C9
(the red-check runner accepts any non-zero exit), C10 (park tests arm on a counter incremented before the
wait). It also verified as claimed: G1, the 19/21 lanes, the `VERB_BARRIER=0` / `STRICT_ERRORS=1` failure
strings (from the MobileGL log files) and the 100x sweep.

**Ruling.** Everything executed or source-confirmed by both reviews (C1/ID-49 server half, M-1..M-7,
m-1..m-7, codex 5 and 11, the census correction) is v1's round 2, launched now. C2, C6, C7, C9 and C10 are
executed by a verifier in `~/w7/p5-verify` (detached at `571cbabb`) in parallel, and their verdicts are
messaged to v1 mid-round; a CONFIRMED one is fixed in the same round, a REFUTED one is cited in the report.
Ruling inside item 3: under an active transport the server's staged copy is the ONLY base - `liveHostBase()`
may not fall back to the client object under split (R-2, table 3); under monolith nothing changes. The
`ScopedApplierEntry` bracket and the direct `BackendObject_Remote` construction remain the integrator's
merge-time hunks unless c1's round 2 has landed by the time v1 finishes. Overturned per finding by the
verifier's evidence only.

**ID-48, progress:** p1 (`7110cdb6`), w1 (`ce53553c..bb6ba282`, merged as `cd2079ec`) landed and merged; s1 and t1 in flight. w1's corrected bound: an empty `SEG_STAGE` takes exactly `MOBILEGL_IPC_STAGE_MB` (33,554,432 B); R-10's max-record counter does not count staged bytes (asserted). w1 put the `SEG_STAGE` requirement in `CheckBlobIsHonest` (covers the optional arms and the encoder too) under the existing `ProtocolCorruption` family, and notes it now reaches `GetCaps`'s two server→client blobrefs - inert in P5, and **a ruling is owed the day caps ride a record** (P6 debt: those two blobrefs need their own carrier rule, not `SEG_STAGE`).

### ID-53 — E1's negative control needs a diagnostic it can read: the `Split` lanes get a private log file, and the control asserts the Fatal line from it

t1's round 2 (`ce53553c..0a22f363`, merged as `129969c3`; the three CI negative controls now assert their own
failure reason through one script, `scripts/ci/split_negative_controls.sh`, that CI and the local gate both
call, with a stubbed-ctest smoke test 9/9 and its red-check) found that `Fatal{BarrierViolation, "DrawVbo"}`
appears nowhere in the integration tree and nowhere in v1's barrier-off joint log: the 18 reds were SILENT
aborts. The string does exist - on `p5/c1` (`ClientSession.cpp:635`, `MGLOG_F`) - but `MGLOG_F` reaches only
the log file, the console sink is compiled out, and the three `DirectGLES.Split.*` lanes set no
`MOBILEGL_LOG_FILE_PATH`. So on the joint head E1 would report an unattributed red.

**Ruling.** (1) Every `DirectGLES.Split.*` lane entry gets a private `MOBILEGL_LOG_FILE_PATH` the way
`MG_IntegrationTest/CMakeLists.txt:706-734` already does for the arming lanes (one file per entry, nothing
else writes it), and the E1 control asserts `Fatal{BarrierViolation, "<slot>"}` from that file, not from
ctest's status; E3(a) asserts the persistent-block scenario's own assertion text or b1's named diagnostic,
whichever the source produces - derived from source and then EXECUTED on the joint head before it counts.
(2) t1's widening of both control filters to `(SmallRing\.)?` is signed off: the `:290/:295` assertion red
came from a `SmallRing` entry the old filter excluded. (3) This is t1's third round, so the lane plumbing is
done inside the joint-merge package (below) under the premium tier, with t1's files granted to it; the
joint-merge package also runs ID-42's emulated-arm command and cross-checks v1's M-5 triage. What would
overturn (1): a lane whose entries share one process (they do not - ctest gives each entry a process).

**ID-47 / ID-48, progress:** s1's round 3 landed (`ce53553c..949ba509`, merged as `39292958`; 16 files, +965/−151): `SEG_REPLY` 16 MiB / eight 2 MiB slots, `ReplySlotPool::CanHold` + `ClientSession::RequireReadPixelsReplyFits(w, h, format, type, dstSize)` with `Fatal{ReplyTooLarge, "ReadPixels <w>x<h> 0x<fmt>/0x<type> <bytes> > <cap>"}`, the fingerprint driven from `CapsAbiFingerprint` (it mixes no segment size, so ID-47 did not move it), the null-union case through `Accept`/`Start`, both death regexes named, the pad-bit case re-pointed at ID-43's control, and ID-51's ledger confirmed (two-pass `/proc` read + lazy kernel hiwater) and fixed with its own running peak; 11 red-once perturbations executed. Head after the merge: G1 0/0/0/0 `.text +0`; split unit 1985 with the one known red (c1's); Wire+Session+codec 156 with that same one. **Docs the integrator owes:** `ARCHITECTURE.md:422` and `BRIEF-P5.md:742` still say `SEG_REPLY` is 8 MiB (the docs pass at the end of the phase, citing ID-47). All five wave-1.5 owners have now landed; the head `39292958` = wave 1 + dev + every ID-48 fix.

### ID-54 — "holds the context for life" is implemented at the forwarder: bind once per tuple, never unbind on a client release

The verifier confirmed codex C7 by instrumentation (`p5-results/v1-codex-verify.md`: two identical binds →
`NATIVE_BIND #1,#2` + `OWNER_WRITE #1,#2`; a client release → `NATIVE_RELEASE #1` + `OWNER_CLEAR`), and
asked whether the fix belongs to c1's EGL virtuals. Ruling: it belongs to v1's forwarders, which c1's
virtuals already owe a call to (v1-v1.md §9.2). `ServerMakeEGLCurrent` binds natively ONCE per
(dpy, draw, read, ctx) tuple and treats an identical repeat as a no-op apart from the R-12 republish
decision; a different tuple is a real rebind. `ServerReleaseCurrent` records the client's release and does
NOT unbind the apply thread's native context - the context stays current on that thread until destroy or
context loss. Charter §2's claim stands; the comment at `ServerLoop.cpp:608-611` becomes true rather than
struck. Controls: the verifier's counts (2 identical binds → 1 native bind, 1 owner write; release → 0
native releases; bind after release, same tuple → 0 native binds). C2 (a control request posted after the
final cancellation pass hangs; the verifier's fix returns NOT_INITIALIZED), C6 (v1 converts the seven
capability reads to the server-private backend; the two-object harness is the control), C9 (the red-check
runner asserts each perturbation's own string) and C10 (the park tests must observe a real blocked park -
the wait replaced by `true` leaves all 11 cases green) are v1's round 2 as messaged. Overturned if a client
release must reach the driver for correctness on some platform (none known; EGL's release is per calling
thread and the apply thread is a different thread).

### ID-55 — c1's round 2 lands R-17 at its real size, and the first IPC frame is measured: 14 of 21

`p5/c1` `d628d906..41992649` (c1's own commit `02f15bb6`, 25 files, +2336/−273; it merged `p5/v1@571cbabb` and
`feat/disaggregated@39292958` into itself because its EGL-forwarder edit names symbols only v1 has - accepted;
merging v1 then c1 into the head still works). R-17's estimate was right on call sites (40) and wrong on
routing: the generated row signature is the P2 monolith shape (no return, no companion pointer), so of 37
entry points 24 fitted, 9 needed a five-line symmetric generator change (a `kHasBlob` row gains
`const void* blobBytes, Uint64 blobByteCount`, as `kVarTail` already gains its pair; `--check` byte-identical,
no `.def` touched), 4 return a value (the acceptance rows + `MapPersistent`) and got the reply MAILBOX (a
reader for the slot ID R-3 mints), and 4 cannot ride a generated row for a written reason (`ResourceRespecify`
initialBytes, R-13.3; `ResourceFlushRange`, R-13.2; `MapPersistent`'s size/seed; `CreateShaderState`'s seven
blobrefs) and go through a hand-written `gMGPipeRouteEscapes` installed by the same two functions;
`PipeCatalogueTest` pins 33 + 4 = 37. Three wrappers take a byte count their applier lacks
(`SetGlobalConstants`, texture `ResourceSubData` `levelBytes`, `BufferSubDataResident`) - not defaulted, on
purpose. Both round-1 reds gone; G1 0/0/0/0 `.text +0`; G2/G14 0 removed; red-check 20/20.

**Accepted at the merge, in files c1 does not own:** the four-line `PostReply` in w1's `PipeWireCodec.cpp`
(`noteAcceptance` posts for the four `kReplySlot` acceptance rows; without it every acceptance row waits for
an answer nobody wrote, `Fatal{ReplyMissing}`), and one CMake line each for `MG_Pipe/PipeRoute.cpp` (push
list) and `MG_Remote/Client/WireTables.cpp` (disaggregated list). **What arms `integration-split`:**
`InstallClientWireTables()` at the end of a successful `ClientSession::Start` step 9, on the GL thread, after
`ServerLoop::Start` returned; `Uninstall` at the top of `Stop()`, putting the monolith adapters back.

**The first IPC frame:** with the `MG_Backend/Init.cpp` direct-construction one-liner applied locally,
`integration-split` under inproc is 14/21 - both `TriangleScenario` and all five `ClearThenReadPixels` entries,
on both arms. The seven `PersistentCoherentMapScenario` entries abort on ONE server-side seam:
`MGPipeApplyResourceFlushRange` (`PipeApply.cpp:~1873`) faults when `Size != 0 && bytes == nullptr` and the
decode arm passes `nullptr` by R-13.2; the consumer half must read the bytes from v1's `StagedShadowStore`
(R-11/ID-37) - assigned to v1's round 2 (item 12). c1 also concedes its round-1 §8.4: `TriangleScenario`
renders without the routing "right by accident" (P3a's deferred handle arm), which is R-17's strongest
argument, not evidence against it. Reviews launched: same-family (`c1-review-v2.md`) and cross-family
(`c1-codex-review-v2.md`); verified-before-assigned as before.

### ID-56 — c1's cross-family review: three blockers (two are v1's open items), nine majors, all unexecuted; verified after the same-family review lands

`p5-results/c1-codex-review-v2.md` (codex, read-only). Blockers: (1) the server half of ID-49 is absent on this
head (v1's item 1, in flight - the tight `DstSize` is paired with a backend that still honours pack state);
(2) a bound PACK PBO's offset is treated as a CPU destination (`EmitTables.cpp:~321/:332`; `DstOffset` is always 0;
the tight path would copy the reply to address 16) - see ID-57; (3) the `resource_flush_range` seam (ID-55, v1's
item 12). Majors: (4) `Stop` reinstalls the monolith adapters while the transport is still live, so a routed call
during teardown or after a failed `Start` runs the applier on the caller - the forbidden path, by design;
(5) the two escapes (`ResourceRespecify`, `MapPersistent`) turn reply Status=ERROR into an ordinary decline
instead of `Fatal{ReplyError}`; (6) the scatter applies `PACK_SKIP_IMAGES` to a 2-D read (GL ignores it; the
monolith path uses `honorPackImageParams=false`); (7) R-1's client-side check sits at emission, after the
residual-state fill that races the applier, and is inert without the bracket; (8) the 20/20 red-check runner
accepts any non-zero exit / any failed marker (ID-46 finding 9's shape, again); (9) `PipeCatalogueTest` installs
and counts the MONOLITH table, so deleting one `InstallClientWireTables` assignment leaves it green; (10) the
readback red-checks mutate a helper production does not call (`TightReadbackByteCount`) and the capacity
perturbation cannot detect deletion of the `RequireReadPixelsReplyFits` call; (11) a short OK reply or an
ERROR/DECLINED with zero payload is scattered as pixels - no exact-extent check on the client; (12) a repeated
successful `MakeEGLCurrent` publishes a snapshot the client never pumps (base `MakeEGLCurrent` skips
`InitCapabilities` once initialised). Report corrections: c1's own saved monolith-lane log says 19 failed of
1149, not 7 (the `Split.` entries force inproc inside the monolith invocation; the "7" run has no saved log);
installation is after an ATTEMPTED caps pump, not a successful first adoption; `ClientWireRecordsEmitted` is
read by no test.

**Ruling.** As with ID-46/ID-50: the same-family review (`c1-review-v2.md`, executing) lands first, the lists
are merged, the residue is executed by a verifier, and c1's round 3 - its third, so on the premium tier -
takes what survives. Nothing here is assigned yet except what is already v1's.

### ID-57 — Under split, a `ReadPixels` into a bound PACK PBO is refused by name in P5; no offset is ever dereferenced

Codex blocker 2 stands on source: `EmitReadPixels` has no PBO branch and the frontend permits an aligned PBO
offset as `pixels`. The reduced path's exit gates do not read into a PBO (E2's snapshot reads to client memory).
Ruling for P5: when `GL_PIXEL_PACK_BUFFER` is bound at a `ReadPixels` under an active transport, the client
refuses by name - `Fatal{UnmigratedVerb, "ReadPixels+PACK_BUFFER"}` (R-4's class-C shape) - before any
emission and before any dereference; under monolith nothing changes. The real form (the server writes the
reply into the buffer resource and the client marks it GPU-written, b1's `MarkReadPixelsPackBuffer` becoming
the producer contract §3 names) is a P6 item on the ROADMAP. Control: a bound 64-byte pack PBO and an offset-16
read under inproc dies with that string; the same call under monolith reads back through the buffer as today.
Owner: c1, round 3. Overturned if an exit-gate trace reads into a PBO (E2 does not; OpenRA's readbacks are
harness snapshots to client memory).

### ID-58 — c1's same-family review: three blockers, eight majors, six minors, verdict "more than one round"; round 3 on the premium tier, with a split as the contingency

`p5-results/c1-review-v2.md` (executed). **B1**: R-17's conversion renamed `MGPipeApply<Name>(` CALL expressions,
so the five sites that take the applier BY ADDRESS (`PipeFill.cpp:1513` `EmitDeleteIfPublished`, callers
`:1536/:1557/:1598/:1635/:1656`) survived - four rows (`DeleteSamplerView`, `DeleteShaderState`,
`DeleteSamplerState`, texture/renderbuffer `ResourceDestroy`) still run synchronously on the GL thread under
split; `MGPipeRouteDeleteSamplerView`/`DeleteShaderState` have zero callers; an abort probe there took
21/21 split entries down (baseline 14/21) - it runs on every entry. **B2**: ID-49's server half is absent
(v1's, in flight; the joint readback mis-places pixels and corrupts the heap). **B3** (= codex 9): nothing
observes the CLIENT arm's 37 rows - deleting a row from `InstallClientWireTables` leaves every gate green.
Majors: M1 `Fatal{BarrierViolation}` cannot fire on this head (ID-53 still has nothing to read); M2 (= codex
11) the readback never reads reply status or size; M3 `Fatal{ReplyTooLarge}` in `EmitAndWait` unreachable and
the failure it was written for reports a false cause; M4 (= codex 5) ERROR handled three ways across the five
acceptance rows, `SessionWait::ShutDown` comment vs abort; M5 `Fatal{InitialBytesNotCarried}` not role-aware,
can abort the server's apply thread; M6 (= codex 8) the red-check runner accepts any non-zero exit, scores a
build break as red, never restores the integration controls, exits zero on failures; M7
`ClientWireRecordsEmitted()` is not what arms the lane; M8 (= ID-57) the pack-PBO destination neither wired
nor refused. The inproc `integration-gpu` census on this head: 386 passed / 180 skipped / 556 aborted /
23 failed / 4 segfault (= 1149).

**Ruling.** c1's round 3 (its third → premium tier, user's rule) takes B1, B3, M1-M8, the six minors, the
report corrections, and codex 4/5/6/8/9/10/11/12 conditionally on the verifier's verdicts (messaged
mid-round); B2 and codex 3 stay v1's. **Contingency:** if round 3's reviews still find a blocker, the client
package is SPLIT - the routing + catalogue gates (R-17/R-4) from the readback + reply mailbox (ID-47/49/57)
- and each half gets its own reviewer; the integrator does not run a fourth round of the same shape.
Overturned per finding by the verifier's evidence only.

**ID-51, closed:** GitHub `Test` run 35088928148 on `39292958` fails exactly one test of 1985 on the split unit
lane - `PipeWireCodecTest.NoReplyIsWrittenForARowTheCatalogueGivesNoReplySlot` (c1's, fixed on `p5/c1`, not
yet merged) - and every other job is green: G5 (ID-41), `RingTest:774` (ID-43) and the memory ledger (ID-51,
s1's round 3) are confirmed on the runner, not only locally.

**ID-56, closed by execution (`p5-results/c1-codex-verify-v2.md`):** all ten c1-owned codex findings CONFIRMED
(2 pack-PBO offset dereferenced: SIGSEGV under inproc, `DstOffset` literal 0; 4 `Stop` reinstalls the adapters on
a live session, a routed mutation ran with the ordinal unmoved and no refusal; 5 escape ERROR → `rc=0`, generated
row → `Fatal{ReplyError}`; 6 `PACK_SKIP_IMAGES` lands bytes 48-95; 7 zero production uses of `ScopedApplierEntry`,
flag up during the fill and down at emission; 8 the runner accepts an unrelated failure as RED and exits 0 even
when all 20 are green; 9 a deleted row keeps the catalogue 32/32 while a split destroy leaves the ordinal at 16;
10 `tight+16` and a deleted `RequireReadPixelsReplyFits` both stay green; 11 a one-row-short OK reply leaves
16/48 stale bytes and passes; 12 repeated make-current leaves the mirror at gen 3 while snapshots pile up
unpumped). Discrepancy (i): c1's "7 failed under monolith" is wrong twice - the log's 19 are right, all
`DirectGLES.Split.*`, which force inproc through their ctest ENVIRONMENT property; 7 is the count WITH the init
patch. (ii) the full inproc `integration-gpu` census on `41992649` + init patch: 386 passed / 180 skipped /
558 aborted / 23 failed / 2 segfault; first failures `Fatal{UnmigratedVerb, "DrawElements"}`,
`"BeginTransformFeedback"`, `resource_flush_range` (v1's). New unowned diagnostic on ordinary lanes under inproc:
`Fatal{BlobMissing, "SetDynamicState"}` - under R-4 an unimplemented slot dies as `Fatal{UnmigratedVerb}`, not as
a blob-shape complaint; classification assigned to c1's round 3. ID-53's lane log path
(`MGL_ITEST_GLES_SPLIT_ENVIRONMENT` sets no `MOBILEGL_LOG_FILE_PATH`) is the joint-merge package's. All of it
messaged to c1's round 3, whose conditional items are now unconditional.

### ID-59 — v1's round 2 lands: the reduced-path lane is 21/21 on the joint, and two out-of-package edits are accepted

`p5/v1` `3fb97cc5..c9d84e33` (9 commits; report `p5-results/v1-v2.md`). Claims, pending the round-2 review
(`v1-review-v2.md`, executing): every confirmed item of ID-52 plus C2/C6/C7/C9/C10 and item 12 fixed with a
red-once line each; G1 0/0/0/0 `.text +0`; unit 1814x3, split 1994 with the one known c1 red; push
integration-gpu 1128/1128; split monolith 1149/1149; flake x100 green; the red-check runner now has a C9
meta-control and 14 perturbations each red on its own string. **On the joint** (`c9d84e33` +
`p5/c1@41992649` + the two merge-time hunks, conflict-free): `integration-split` **21/21**, also **21/21 under
`MOBILEGL_IPC_AUDIT=1`**; inproc census 0 SEGFAULT (was 2) / 23 failed (was 28). ID-52's M-2 ruling (no
`MappedData()` fallback under an active transport) exposed two real gaps the fallback had masked - m-5's null
was too broad (`HasShadow`), and the buffer twin is lazy until draw (D-A2) so a subdata must mint it to stage.
Item 12's flush guard (the bytes from `StagedShadowStore`) closed the last seven.

**Accepted:** v1 edited `MG_Pipe/PipeApply.cpp` (item 12) and `MG_Backend/DirectGLES/Utils.cpp` +
`BackendObject_DirectGLES.cpp` (C6) under explicit assignment, all `#if MOBILEGL_BUILD_DISAGGREGATED`-guarded
(G1 `.text +0`, monolith codegen unchanged). **Still the integrator's at the merge:** the direct
`MakeUnique<BackendObject_Remote>()` in `Init.cpp` and the `ScopedApplierEntry` bracket at the top of
`ApplyOne` (they need c1's headers; `p5/v1` keeps the weak placeholder). v1 attributes the 23 inproc failures to
P4b/P7 debts, c1 and shared backend (two flake under `-j`), none to v1 - the round-2 review tests that on two
of them. The joint merge waits for c1's round 3 (B1: four rows still bypass the wire) and both reviews.

### ID-60 — Implementation packages may go to the GPT family; the first is j0, ID-53's lane plumbing

The user extended the codex authorisation (2026-09-16) from reviews to implementation and fix tasks. Standing
practice from here: implementer and reviewer come from different model families whenever a package is
handed out (GPT implements → Claude reviews, or the reverse), for the same reason the cross-family reviews
were worth their cost (ID-46: ten confirmed majors a same-family review had passed). **j0** = ID-53 ruling (1):
every `DirectGLES.Split.*` entry gets a private `MOBILEGL_LOG_FILE_PATH`, a distinctness check, the E1/E3(a)
controls in `scripts/ci/split_negative_controls.sh` read the Fatal line from the entry's file, and t1's stubbed
smoke test gains the file-read cases (red once by falling back to ctest status). Branch `p5/j0` off
`c3e736a3`, worktree `~/w7/p5-j0`, codex `gpt-6-astra`, prompt kept at the scratchpad `codex-j0-lanelogs.md`,
report `p5-results/j0-v1.md`. It is independent of c1's round 3 and v1's review, and the joint-merge package
consumes it. Its review (Claude) follows the same verified-before-merged rule.

**ID-60, progress:** j0 landed from codex `gpt-6-astra` as `p5/j0@b2dcae71` (one single-line commit, correct identity, six files, no C++, no workflow edit): 21 entries with 21 distinct private logs measured from `ctest --show-only=json-v1` (the `PersistentMapArm` counting entry keeps its file and `RESOURCE_LOCK`), `Harness/split_log_paths.py` as the distinctness check, the control script discovering paths from ctest and requiring E1's `Fatal{BarrierViolation, "<slot>"}` in a fresh selected private file, smoke tests 14/14 and red once by removing the file read; split unit 1985 with the one c1 red, push integration-gpu 1128/1128 name-for-name. One premise corrected by j0: the harness's skip reason is not written through MGLOG, so a skipping entry's file holds only the library's own lines. Cross-family review (Claude) launched: `j0-review-v1.md`.

### ID-61 — c1's round 3 lands; j0's review sends it to a second round; the joint merge runs on a scratch branch in parallel with the reviews

**c1 round 3:** `p5/c1` `41992649..5682f429` (work commit `5682f429`, 9 files; report `p5-results/c1-v3.md`): B1
(the five address-of sites → `&MGPipeRoute<Name>` + a grep gate), B3 (a test over the CLIENT tables), M1-M8, codex
4/5/6/8/9/10/11/12 and the `BlobMissing` classification, all with red-once lines; unit split 2030/2030, push
1128/1128, G1 0/0/0/0, red-check 9/9 each on its own string; inproc census on its own head 420/182/517/27/3 with
`BlobMissing` 0; joint against v1's round 1 still 14/21 (the seven are v1's flush seam, fixed on `p5/v1@c9d84e33`).
c1 re-measured that a strong `CreateRemoteBackendObject` does NOT displace v1's weak one under the static
archive's on-demand semantics, so the direct-construction hunk stays the merge-time form. Agreed formula:
`DstSize = width*height*GetInputBytesPerPixel` (tight, neutral pack). Minor m1 deferred by judgment (a marker
would reshape the 33/4/37 catalogue gate for dead code) - the round-3 review (`c1-review-v3.md`, executing)
decides whether that stands and, per ID-58, whether any blocker remains.

**j0:** the cross-family review (`j0-review-v1.md`) found four majors, all in files outside j0's six - the five
new smoke cases run nowhere (CI runs the un-extended `control_smoke_test.sh`), t1's
`redcheck_control_smoke_test.sh` broken by the new branch, the artifact glob misses `split-logs/`,
`SplitLogPaths.PrivateAndDistinct` unlabelled - plus seven minors; the plumbing itself (21 distinct paths, the
file reads, the red-onces) verified. j0's round 2 runs on codex with those files granted (`j0-v2.md`).

**jm, the joint merge (ID-60's second GPT implementation package):** scratch branch `p5/joint` off `c3e736a3`,
merge order j0 → c1 → v1 at the heads current at launch, the two merge-time hunks as one commit, generator
checks, then `wsl_p5_gate_joint.sh` (a copy with its own tree, log prefix and completion marker) and Step 3's
exit-gate controls: `integration-split` under inproc (predicted 21/21), E1/E3(a) through
`scripts/ci/split_negative_controls.sh` with `Fatal{BarrierViolation}` read from the private files, ID-42's
emulated-arm red-check, `MOBILEGL_IPC_AUDIT=1`, `MOBILEGL_IPC_STRICT_ERRORS=1`, the inproc `integration-gpu`
census with every failed/segfaulted entry probed and attributed, and the R-10/ledger numbers for MEASUREMENTS.
It runs in parallel with the c1/v1 reviews on purpose: a blocker they find is re-merged onto the scratch after
its fix; the integrator merges `p5/joint` into `feat/disaggregated` only when both reviews and the joint report
are in. Report: `p5-results/joint-v1.md`.

### ID-62 — v1's round-2 review: 7 closed / 7 partial / 1 not closed, five new majors; round 3 on the premium tier; E1's control must refuse a skipped selection

`p5-results/v1-review-v2.md` (executed, 745 lines). CLOSED: ID-49's server half, M-2, C2, C9, C10, item 12, G1.
NOT CLOSED: M-3 (the two whole-store readers have no gate anywhere). PARTIAL: seven, including C7 - releases
1→0 but native binds still 2→2, so ID-54's bind-once is not implemented. New majors: **N-1** under
`MOBILEGL_IPC_VERB_BARRIER=0` the pre-flight child dies of `Fatal{BarrierViolation}`, the harness SKIPS all 21
entries and ctest exits 0, so E1's gate reads GREEN; **N-2** two M-5 triage rows called deterministic split
defects "-j flakes" on the strength of probes that ran zero tests; **N-3** `m_haveCurrentTuple` is never
invalidated on context/surface destroy (a recycled handle classifies as a no-op and never binds); **N-4** the
report's inproc census (575/183/551/23/0) sums to 1332 - the real one is 392/183/551/23/0, passed went
428→392 between rounds; **N-5** nine deletions of production wiring go red on nothing. Joint with c1's round 3:
`integration-split` 21/21, audit 21/21, `STRICT_ERRORS=1` 19 red, `BARRIER=0` green (N-1); none of the 23 inproc
failures is v1's (two independent reverts).

**Ruling.** v1's round 3 (its third → premium tier) takes M-3, the seven partials (C7's bind-once included),
N-2, N-3, N-4 (with the explanation of the 36-entry drop - c1's B1 routing may have moved entries from
"passed by accident" to class-C aborts, which would be correct), N-5's nine gates, the minors, and item 10's
prose. **N-1 is a control defect, j0's:** `scripts/ci/split_negative_controls.sh` must treat a selection whose
entries SKIPPED as a control FAILURE by name ("the knob made the pre-flight die, not the entry") - a red that
comes from the harness skipping everything is not E1's red; and v1 states in its report whether the barrier
Fatal should be raised inside the entry rather than in pre-flight. Assigned to j0's next round (codex) and to
the joint package's reading of the control. Per ID-58's shape, a blocker after v1's round 3 splits the package.

**ID-61 / ID-62, progress:** j0's round 2 landed (`b2dcae71..68d8ef9f`, 8 files, +47/−24): the five smoke cases now run in the CI step and the local gate (one invocation line changed in `wsl_p5_gate.sh`), t1's `redcheck_control_smoke_test.sh` repaired and shown red once, the artifact glob covers `split-logs/`, `SplitLogPaths.PrivateAndDistinct` labelled so the split lane runs it (22 tests under the label), the minors as written; Python stays `REQUIRED` with the stated policy (a missing interpreter must fail the gate loudly rather than omit it — accepted). j0's round 3 (ID-62's N-1: a skipped selection is an E1 control failure by name) is running on codex. The joint package started from `p5/j0@b2dcae71`; its E1 reading is therefore round-1 plumbing, and the integrator merges `p5/j0`'s final head separately.

**ID-62, progress:** j0's round 3 landed (`68d8ef9f..cc20de37`, 5 files, +110/−13): the E1/E3(a) control detects skipped entries from the JUnit output and fails by name when any selected entry skipped; E1 passes only when every selected entry ran, failed, and its own private file holds the expected Fatal; smoke 19/19, red once by removing the skip check. `p5/j0`'s final head is `cc20de37`; rounds 2-3 changed only CI wiring and control scripts, so their verification is the CI run on the merged head plus the joint package's re-run of the controls, not a fourth review. The joint package (started from j0@`b2dcae71`) has reached Part 3 of the gate: the E2 OpenRA retrace under inproc passed 2/2 on both backends with 0 `Fatal{` lines, and its negative control (a pull library under inproc) is red for the transport's reason - the first time E2 has been observed green on a split build.

### ID-63 — The joint gate on the scratch head `e61d0012` (j0@b2dcae71 + c1@5682f429 + v1@c9d84e33 + the two hunks): the five parts, measured

From `~/w7/p5-joint-gate.log` (the jm package's copy of the gate; its Step-3 report is still being written):
Part 1 - include-closure 4 probes / 0 problems; `G1-split nm MG_Remote pull=0 split=610`; **G1 `.text
10806611 → 10806611 (+0)`, 27814 → 27814 symbols, 0/0/0/0**; **G5 p3a and p4a byte-identical against
`ff2994d9`**. Part 5 - `gen_pipe --check` up to date, self-test 9/9; dirty-surface self-test 27/27;
field-ownership up to date, 15/15. Part 3 - G2 pull-vs-push name diff 0; G14 0 removed / 42 added; unit
**1816/1816 ×3 and split 2038/2038** (every known red gone, c1's `:1203` included); Wire 58/58;
integration-gpu pull 1128/1128, push 1128/1128, **split monolith 1149/1149**; split inproc 53% (538 red of
1149 - the class-C `Fatal{UnmigratedVerb}` aborts by design plus the ~23 wrong-answer failures the census
triages; the three-arm name+status diff of 1080 is that lane, expected); **`integration-split` 21/21 (19 ran,
2 skipping by design)**; the negative control "-L integration-split is red in a build without the option"
holds; **E1 (`MOBILEGL_IPC_VERB_BARRIER=0`) turned 14 selected entries red carrying the scenario's own
diagnostic; E3(a) (`MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0`) turned 6 red the same way**; the persistent-map arm
lanes 2/2 with the split counting lane MEASURING for the first time (`pmap=2160.00 mpr=1` versus the push
lane's `pmap=0.00 mpr=1` - E3(c)'s reading). Part 2 - integration-verify 930/930. Retraces - **E2 OpenRA under
inproc 2/2 on both backends, 0 `Fatal{` lines, negative control red for the transport's reason**; push retrace
79/79; verify retrace 79/79. This is the phase milestone ("first IPC frame, reduced path") observed on a full
gate. What still separates it from the landing: the jm Step-3 numbers (ID-42's emulated-arm red-check, the
audit/strict runs, the attributed inproc census, the R-10/ledger readings), the c1 round-3 review verdict,
v1's round 3 (then a re-merge and a re-run of the gate on the final head), j0's final head `cc20de37`, and the
docs/APK/device A/B that follow.

### ID-64 — c1's round-3 review: MERGEABLE, no blocker, no split; the residue is a cross-family gates package

`p5-results/c1-review-v3.md` (executed, 646 lines): 8 CLOSED (B3, M1, M5's code, M6, M7, codex 4, codex 6,
`BlobMissing`; every self-test number exact), 6 PARTIAL (B1, M2, M3, M4, M8 + one), 1 NOT CLOSED (codex 12).
New: 0 blockers, 5 majors - N-1 B1's grep gate does not exist (claimed in the report and in a shipped comment;
reverting a site leaves 2030/2030 green), N-2 the teardown refusal misses the five class-B verbs, N-3 the
M2/M3 production wiring deletes clean, N-4 the escape ERROR rule has no control, N-5 five items ship no executed
red-once line - and 4 minors. The reviewer itself observed the behaviour c1 did not demonstrate: a real pack
PBO with an offset-16 read dies `Fatal{UnmigratedVerb, "ReadPixels+PACK_BUFFER"}` (monolith reads back
0,255,0,255), and `Fatal{BarrierViolation, "SetTextureParams"}` fires with v1's bracket. Joint (c1 r3 + v1 r2 +
the hunks): `integration-split` 21/21, audit 21/21, barrier-off all-21-skipped false green (ID-62's N-1, j0's
round 3 fixes the control); census 426/185/511/27/0 segfault; none of the tail c1's.

**Ruling.** ID-58's contingency is not triggered (no blocker): the package is not split and c1 gets no fourth
round of the same shape. The residue - N-1…N-5, codex 12, the six partials, the minors, the report
corrections - is a GATES package, **c1f**, implemented by the GPT family (codex `gpt-6-astra`, branch `p5/c1f`
off `p5/c1@5682f429`, worktree `~/w7/p5-c1f`, report `p5-results/c1f-v1.md`) and reviewed by Claude, per ID-60.
c1's head merges into the joint now on the review's verdict; c1f merges on top when its review passes. What
would overturn: c1f finding that a "partial" hides a wrong behaviour rather than a missing gate - then it is a
finding for c1's owner half per ID-58, not a c1f fix.

### ID-65 — The joint report's dispositions: what is proven, what is recorded debt, what still owes a control; and the landing plan

`p5-results/joint-v1.md` (jm, codex; 1495 lines, evidence under `~/w7/p5-joint-evidence/`). **Proven on
`e61d0012`:** the five parts (ID-63); `integration-split` under inproc 19 ran / 2 skipped-by-design / 0 failed,
three times; audit the same; E1 14/14 selected entries red with `Fatal{BarrierViolation, "<slot>"}` in every
private file; E3(a) 4 red / 2 skipped on the scenario's own pixel assertion (no library diagnostic exists for
block size 0 - x2 adds one); **ID-42's emulated direction VERIFIED** (green → `return false` red on b1's
message → restored green; the arm property reads `emulated`); `STRICT_ERRORS=1` 19 abort on
`Fatal{UnmigratedPipeInput, "GetTextureContextId@Clear"}` (R-7 debt, recorded); the census 426 passed / 185
skipped / 511 aborted / 27 failed / 0 segfault, the 27 re-run with private logs (zero Fatal lines, all rc=1)
and 8 of them re-run three more times each with no flake (v1-v2's flake claim is refuted); the ledger pinned
by `ProtocolSmokeTest` at 8/32/16 MiB + 256 KiB; per-frame `pmap`/`mpr`/`rsp` for two scenarios.

**Rulings.** (1) The 27 ordinary inproc failures are P5's recorded debts, not blockers: 14
`LayeredAttachmentShapeScenario` + 3 packed depth-stencil `GetTexImage` + 5 `HandleRecycleScenario` framebuffer
cases are texture READBACK through the client-shadow fallback (`GL_Texture.cpp:~6804`) - P4b/P7, outside BRIEF
§4's reduced path; 3 `PrimitivesGeneratedNoXfb` are P7 query accounting; 1 `TextureParamsWithoutASamplerView`
is an inspection harness without an apply-thread forwarder; 1 `P4aFinalFixScenario` deletion case is a shared
backend FBO/RBO lifetime after transported deletes - the last two go on the ROADMAP as named items for P6,
the rest into the P4b/P7 rows. The 511 aborts are class-C by construction (two sampled: `DrawElements`,
`BeginTransformFeedback`); the claim "all 511" stays unmeasured and is written that way. (2) **E2's negative
control is NOT verified**: with `MOBILEGL_IPC_E2_DROP_CLEAR=1` armed the OpenRA retrace stayed at SSIM 1.0 -
a control that cannot go red is R-16's exact defect; package **x2** finds out why and ships a control that
reddens for its reason (drop draws or the first N records), plus R-10's `maxrec=` in the stats line (the joint
could not measure the maximum record bytes - BRIEF §8 item 3 unmet), plus `SmallRing` wrap/wait counters so
E3's small-ring lane proves back-pressure happened, plus the E3(a) named diagnostic. (3) **E5's R-16 half** -
the corrupt-staged-upload control - is v1's round-3 item 10 (messaged). (4) The APK package (`ap-v1.md`)
found the plumbing: the repo-root Groovy `build.gradle:23-25` reads `mobilegl.pipePush` (the `.kts`-only
search missed it); five uncommitted lines in `android-plugin/build.gradle.kts` (diff at
`~/w7/notes/p5/apk/build-plumbing.diff`) add `mobilegl.buildDisaggregated` / `buildDisaggregatedInproc`; the
integrator commits them as `[Build] (Android): …` on the final head; three APKs from `e61d0012` proven
(`MGPipe` strings 3/116/190; `MG_Remote` names and the `MOBILEGL_TRANSPORT` strings only in split;
`ConfigLoader.cpp:321` reads the env, default monolith), to be rebuilt from the final head with
`~/w7/notes/tools/wsl_build_p5_apks.sh <tree>`.

**Landing plan.** When v1 round 3, c1f and x2 have landed and been reviewed: `feat/disaggregated` ← merge
`p5/joint` (j0 r1 + c1 r3 + v1 r2 + the hunk commit) → `p5/j0@cc20de37` → `p5/v1` (r3) → `p5/c1f` → `p5/x2` →
the gradle mapping commit; regenerate/check; the full gate on `~/w7/pipe` with `wsl_p5_gate.sh` (now
calling j0's controls); push; then the APK rebuild, the Redmi three-arm A/B (pull / push / split-inproc, plus
the split-no-env control arm that must equal push), the docs (README/ROADMAP/ARCHITECTURE/MEASUREMENTS),
push again, and P6.

### ID-66 — Process change (user, 2026-09-16): no over-verification, no reviews between rounds, one codex review per phase; the joint head is landed

The user's two instructions: "尽快推进，不要做过度的验证。目标是尽快达到完全分离式线程的渲染" and
"对抗审查不要在轮次当中跑，可以在整个 P 阶段完成后由 Codex 执行". In force from here: a package lands on its own
gate plus the integrator's quick gate (unit lanes, the inproc `integration-split` lane, push monolith parity,
G1); no verifier packages, no mid-round adversarial reviews; ONE codex adversarial review at the close of a
phase, its findings becoming the next phase's first fix round; the full five-part gate once per phase at
the close; the device A/B recorded in the background, never blocking. Landed under that rule:
`feat/disaggregated@eec0e836` = `c3e736a3` + `p5/joint@e61d0012` (j0 r1 + c1 r3 + v1 r2 + the hunk commit)
+ `p5/j0@cc20de37` + the Android gradle mapping (ap's five lines, committed as `[Build] (Android): …`); quick
gate: G1 0/0/0/0 `.text +0`, split unit 2038/2038, `integration-split` under inproc 22/22 (j0's
`SplitLogPaths` test included), push integration-gpu 1128/1128; pushed to GitHub. Still to land on their own
gates: v1 round 3, c1f, x2. The user stopped the Claude A/B-prep agent and asked for codex `gpt-5.6-sol` to
run the Redmi A/B - launched (`p5-results/ab-v1.md`), four arms (pull / push / split-inproc / split-no-env
control) with the APKs from `e61d0012` (native code identical to the landed head).

**Direction for the next phase, per the user's goal ("fully separate-thread rendering as soon as possible"):**
inproc already renders on a separate thread for the reduced path; what stands between that and real workloads
is the class-C census - 64 `Fatal{UnmigratedVerb}` slots (511 aborts in the inproc lane) plus the P3b/P4b/P7
residual-input debts (`rsp`) - not the spawn transport (P6, a separate PROCESS). So the next phase is the
verb-migration push under inproc, ordered by what the inproc lane and the Minecraft traces actually hit: a
census package tallies the aborts per slot and per family (`~/w7/notes/p6/census-classC.md`), then parallel
packages migrate slots by subsystem with the inproc lane's abort count as the only gate, until the four trace
cases render under inproc on the Redmi. P6's spawn transport follows once rendering is fully on the apply
thread.

### ID-67 — c1f and the docs are landed; an identical repeated make-current republishes nothing, and the control says so

c1f (`5682f429..6d86f9d9`, codex; 9 files, +641/−14: the address-of grep gate, the client-table test, the
teardown refusal shared with the five class-B verbs, the exact-extent reply check's gates, the escape ERROR
death tests, the PACK-PBO control, the M8/M2/M3 gates, a red-check runner asserting each perturbation's own
string 15/15) is merged as `fbeea876`; quick gate: G1 0/0/0/0 `.text +0`, `integration-split` under inproc
22/22, push 1128/1128, split unit 2056 with ONE red - c1f's new control
`RemoteClientControls.RepeatedMakeCurrentAdoptsRepublishedCapsWithoutAPumpOrPresent`, which codex-12 asked for
and which v1's ID-54 bind-once now contradicts: an identical (dpy, draw, read, ctx) repeat is a native no-op, so
the server publishes NO new snapshot on it and the mirror generation correctly does not move. **Ruling:** ID-54
stands; caps cannot change without a real context change, so the control's expectation becomes: identical
repeat → no republish, mirror generation unchanged, no snapshot accumulates; a DIFFERENT tuple → the server
republishes (R-12 arm (a)) and the client adopts it without a pump or a Present. The expectation change is a
test edit in c1's file (non-core → codex sol); v1's round 3 owes the "different tuple republishes" half if its
bind-once dropped it. The docs package (codex sol, `p5/docs@a48bc46e`, 4 files +202/−10; 103 citations
checked, 0 problems; the A/B, max-record and RSS-slope figures marked pending) is merged as the docs head.

### ID-68 — The class-C census (codex): every Minecraft trace first-stops at `DrawElements`; the next phase is P5b, verb migration under inproc, Minecraft-first

`~/w7/notes/p6/census-classC.md` (codex sol; per-entry private logs under `~/w7/p6-census-logs/`, runner
`~/w7/notes/tools/p6_census_lane.{sh,py}` and `p6_census_traces.{sh,py}`). The inproc lane reproduces
426/185/511/27 exactly; all 511 aborts have a FATAL first line - 505 `UnmigratedVerb`, 6
`StageSnapshotTooNarrow "respecify_whole_store"` (P8/P11 staging debt, not class C). First-blocker table by
slot (entries / distinct scenarios): `BindImageTexture` 138/71, `BeginTransformFeedback` 95/44,
`DrawElements` 56/27, `DispatchCompute` 49/25, `PatchParameteri` 43/19, `CopyImageSubData` 30/14,
`MemoryBarrier` 18/9, `MultiDrawElementsBaseVertex` 18/9, `ClearBufferiv` 12/6, `ClearBufferuiv` 8/4, then a
tail of 15 slots; 39 of the 64 static class-C slots are hit by nothing measured (queries, syncs, indirect
variants, named-framebuffer clears, XFB pause/resume, `GetTexImage`, swap interval). **Traces: all 40 fixtures
are hydrated; OpenRA passes on both backends at SSIM 1.0; every Minecraft fixture (38 cases, 75 backend runs)
first-stops at `Fatal{UnmigratedVerb, "DrawElements"}`, and `improved-transparency-minecraft-26.3` at
`DrawElementsInstancedBaseVertex`.** The census also corrects a premise: the five P5 class-B verbs have no
`MGPipeApply*` entry point (they reach `IVerbSink::On*`), so a migration package must add the named seams
rather than claim an existing one.

**Ruling - P5b, verb migration under inproc, Minecraft-first.** Since the user's goal is fully separate-thread
rendering of real workloads, the order is driven by the traces and the lane together: (1) a short contract
package **c0b** (Claude, premium tier) appends the wire records, catalogue rows (opcodes never move; new calls
appended), `IVerbSink` methods and named-Fatal stubs for all 25 MEASURED slots, so four packages compile on
day one - `draw_vbo` grows the index/instancing/base-vertex/multi-draw form (P8's "收编 multi-draw", the index
host mirror), `set_shader_images`/dispatch/barrier, stream-output/patch state, the clear family - plus
`CONTRACT-P5B.md`; (2) four packages in parallel: **d1** the indexed/instanced/multi-draw family (Claude
premium; the Minecraft blocker; its gate is "every Minecraft trace reaches its NEXT first blocker under
inproc, reported"), **i1** image/compute (`BindImageTexture`, `DispatchCompute`, `CopyImageSubData`,
`MemoryBarrier`, `ShaderStorageBlockBinding`; Claude), **t2** XFB/tessellation (`BeginTransformFeedback`,
`PatchParameteri`, `Bind/End/Pause/ResumeTransformFeedback`; Claude), **f1** the clear family
(`ClearBuffer{iv,uiv,fv,fi}`, `ClearNamedFramebuffer*`; codex astra - additive and mechanical); (3) then the
loop: re-run the census, migrate the new first blockers, until the four A/B trace cases render under inproc
on the Redmi - that is P5b's exit; P6's spawn transport follows. Per ID-66 no reviews between rounds; one
codex review at P5b's close; the inproc lane's abort count and the traces' first-blocker list are the only
gates. P5's own close (v1 r3, x2, c1g, the final full gate, the end-of-phase codex review) proceeds in
parallel and c0b starts now.

**ID-66, note on the A/B in flight:** the Redmi four-arm run (codex sol) is executing for real - the `improved-transparency-minecraft-26.3` case is complete on both backends across pull / push / split / splitctl, the sodium case is running. Expected and to be recorded as such, not as a failure: the `split` arm (inproc) of every MINECRAFT case aborts at `Fatal{UnmigratedVerb, "DrawElements"}` (ID-68's census), so its frame/CPU figures are empty; the meaningful P5 device figures are pull vs push vs `splitctl` (the split BUILD under monolith transport, which must equal push - the on-device G2 for the split flavour) and, for OpenRA if it is in the case list, the true split arm. The split arm rendering Minecraft on the device is P5b's exit, by construction.

**ID-67, closed:** c1g (`c9878d69`, codex sol, one test file +33/−4) aligned the make-current caps controls with the ruling: an identical repeat republishes nothing (mirror generation unchanged, no snapshot pending), a different-tuple make-current republishes and the client adopts it without a pump or Present - and case (b) is LIVE on the head, so v1's half of ID-67 already holds. split unit 2057/2057, `RemoteClientControls` 18/18, both red-once mutations red on their own cases. Fast-forwarded into `feat/disaggregated` and pushed (`c9878d69`).

### ID-69 — v1's round 3 lands; the Redmi A/B is measured; x2's merge goes to codex; c0b is nearly in

**v1 round 3** (`c9d84e33..b13f2b02`, six commits + a merge; report `v1-v3.md`): the review residue closed -
M-3's two whole-store refusals now key on the descriptor's `HasDefinedContent` (round 2's refusal had aborted
the ordinary `glBufferData(NULL)` + partial `glBufferSubData` idiom on six joint entries, hidden under c1's
`BlobMissing` - **ratified**: content the app never defined is not a coverage violation), ID-54/ID-67 bind-once
at the native layer (different tuple: 1 native bind + 1 republish; identical: 0 + 0), N-3's tuple invalidation,
N-5's nine gates, the C9 runner (36/36 red on their own strings), N-4's census correction (the drop was c1 r2's
`BlobMissing` 34 + v1 M-3's 6; the B1 hypothesis refuted; N-2 withdrawn - the 27 are P4b/P7/shared and
deterministic), E5's corrupt-draw control shipped. Merged as `f5614995` (one conflict, `MG_Test/Wire/CMakeLists.txt`,
HEAD's `foreach` already registered both suites v1's side added; taken as-is). Quick gate: G1 0/0/0/0 `.text +0`;
split unit 2071/2071 under `MOBILEGL_ITEST_REQUIRE_GPU=1`; `integration-split` inproc 22/22; split monolith
1149/1149; push 1128/1128. Pushed. v1's CI note - the split unit lane should set `MOBILEGL_ITEST_REQUIRE_GPU=1`
so the EGL-backed gates cannot skip - is a debt for the close.

**The Redmi A/B** (`ab-v1.md`, codex sol; 27/40 groups, all 40 pin checks clean, fan-off and unpin verified):
the `split` arm aborted before benchmarking on every case - `UnmigratedVerb "DrawElementsInstancedBaseVertex"`
(improved-transparency) and `"DrawElements"` (the four others) - exactly ID-66's expectation, so the barrier tax
is UNMEASURABLE on device until P5b's d1 lands; **`splitctl` equalled `push` within noise** (frame p50 −0.1% to
+1.6%, p99 −3.0% to +1.4%) - the split BUILD's monolith path is unchanged on device, the on-device G2 for the
new flavour; pull vs push tables per case are in the report; rd12/DirectVulkan reproduced the known
`scudo::reportMapError` crash in pull, push and splitctl alike (a `dev`-side issue, recorded, not P5's).

**x2** (`e61d0012..579118a1`): `maxrec = 784` bytes (row `SetVertexAttribDefaults`) against a 4 MiB cap -
R-10 needs no chunking, for Triangle, PersistentCoherentMap and the OpenRA retrace alike; E2's clear-drop
control dropped all 29 Clear records and still scored SSIM 1.0 (OpenRA overdraws every pixel: 788
`glDrawArrays`, 0 `glDrawElements`), so `MOBILEGL_IPC_E2_DROP_DRAW=1` is the control - 758 records dropped,
SSIM 0.000036; E3's 1 MiB `SmallRing` lane had written ~4 KiB and never wrapped - it now drives 1,314,880
bytes and asserts `ringwraps=1 ringwaits=1`. Its merge conflicts with j0's rounds 2-3 in four CI-plumbing
files; per the user's rule the resolution is codex's (`p5/x2m`, `x2m-v1.md`). **c0b** (P5b contract) is in
its final chain: four builds green, G1 0/0/0/0, unit 1817 ×3, split 2084/2084 (+13 round-trip cases), G2 0,
G14 0 removed / 43 added.

### ID-70 — c0b lands, the four P5b packages start, the APK lane's root cause is the SDK action's removed `tools` package

**c0b** (`p5b/c0b` `4dd4027e..f5676103`, incl. the merge of v1 r3; fast-forwarded into `feat/disaggregated`
and pushed): `CONTRACT-P5B.md`, wire rows + `WireVerbSink` dispatch + `ServerVerbSink` named-Fatal stubs for all
25 measured slots (no P5b slot gains an `MGPipeApply*`; rule D: the record carries the GL call verbatim beside
the handle), the `EmitTables.cpp` partition into `MGR_UNMIGRATED_{D1,I1,T2,F1}_SLOTS` with ownership asserts
19/7/7/11, 13 codec round-trip cases, `~/w7/notes/p5b/{PACKAGE-PREAMBLE,BRIEF-P5B}.md`, the census runner
`~/w7/p5b-c0b-census.sh` with its baseline `~/w7/p5b-c0b-census-logs/results.json` (432 / 185 / 505 / 27; the
six `StageSnapshotTooNarrow` aborts gone with v1 r3). G1 0/0/0/0, split unit 2084/2084, inproc 22/22, push
1128/1128, G5 green. **Launched on `f5676103`:** d1 (Claude premium: the 19 draw rows; gate = every Minecraft
trace reaches its next first blocker under inproc), i1 (Claude: image/compute/barrier/copy-image/storage
block), t2 (Claude: XFB spans, XFB bind, patch parameter), f1 (codex astra: clears, framebuffer copies, mips).
Each lands on its own gate + the quick gate; the integrator re-censuses on the merged head (BRIEF §5).

**The APK lane** (user, 2026-09-16: "a few retrace failures are fine; failing to compile is not"): every run
today dies at `Setup Android SDK` in both the `build` and `android avd image` jobs -
`android-actions/setup-android@v4` now defaults to cmdline-tools 14742923 (20.0), whose repository no longer
carries the legacy `tools` package, and the action's default `packages` is `tools platform-tools`, so
`sdkmanager tools` exits 1 before anything compiles ("Warning: Failed to find package 'tools'"). Fix delegated
to codex sol (`p5/apkci`): `packages: platform-tools` and a pinned `cmdline-tools-version` on all three steps
(port `dev`'s fix if it has one); verified by the next CI run after the merge.

**ID-70, progress:** the APK-lane fix landed (`94ce2ee7`, codex sol; `.github/workflows/apk.yml` +6: `packages: platform-tools` and `cmdline-tools-version: 13114758` on all three `Setup Android SDK` steps; `github/dev` had no fix to port; YAML validated), fast-forwarded and pushed. The next "MobileGL APK" run on `94ce2ee7` is the verdict - watched for its `Setup Android SDK` step; if it reaches compilation the lane is back to "retrace failures only", which the user accepts.

### ID-71 — P5's code is complete on `37fc4fdb`; the one full gate and the one codex review run against it; the APK lane compiles again

x2 landed through x2m (`bd11ab18`, codex sol: the four CI-plumbing conflicts against j0 resolved keeping both
intents - j0's generalised `if`/`elif` repair subsumes x2's matcher fix; unit 2073, `SmallRing` `ringwraps=1`,
OpenRA SSIM 1.0 baseline / 0.000036 under `MOBILEGL_IPC_E2_DROP_DRAW=1` with 758 drops), merged as `37fc4fdb`
with the quick gate green (G1 0/0/0/0 `.text +0`, split unit 2086/2086, inproc 22/22, push 1128/1128) and
pushed. Every P5 package is now on `feat/disaggregated`: c0 · w1 (2 rounds) · s1 (3) · p1 (2) · b1 (2) · t1 (2)
· c1 (3) + c1f + c1g · v1 (3) · j0 (3) · x2 · the docs · ap's gradle mapping · the APK-lane fix. The APK lane's
`Setup Android SDK` step now succeeds on both jobs (run 35115753041), so the lane is back to compiling; its
retrace failures, if any, are the accepted kind. Running now: the full five-part gate (`wsl_p5_gate.sh` on
`~/w7/pipe`, log `~/w7/p5-final-gate.log`) and the single end-of-phase adversarial review (codex astra,
`p5-results/p5-close-codex-review.md`) - its findings go to P5b's first fix round per ID-66, not to another P5
round; the docs close-out (the A/B tables, x2/v1-r3/j0 numbers, the final gate) follows the gate as one codex
sol run. P5b's four packages are running in parallel on `f5676103`+.

### ID-72 — P5b's first landing: f1 (codex astra) removes all 34 clear/copy/mip aborts from the inproc lane

`p5b/f1` `f5676103..fe5ac006` (implementation `7ffa4998`, 28 files +1888/−94, merged with the P5 head
`37fc4fdb`; report `~/w7/notes/p5b/p5b-results/f1-v1.md`): the 11 f1 slots emit and apply under inproc; the
census moves from 432/185/505/27 to **478 passed / 185 skipped / 477 aborted / 31 failed** on f1's head - all
34 f1 first-blocker aborts gone (28 scenarios pass outright, 6 reach their next blocker), 26 negative controls
red-once. Unit 1817 ×3 / split 2101; `integration-split` inproc 33/33 (+11 lane cases); push 1128/1128; G1
0/0/0/0 `.text +0`. Next-round items it names: named-clear `+UNBOUND` refusals firing on DirectVulkan although
the app bound the framebuffer (a bound-named-framebuffer publication gap on the Vulkan side), a zero-valued
depth-copy readback (three `CopyTexImage` depth variants execute and reach the readback, which reads 0 - a
wrong answer, the four new "failed" entries), depth/stencil-mode inspection. **Merge is queued** behind the
P5 final gate that is running in `~/w7/pipe`'s build directories (rebuilding under a running gate would void
its numbers); it lands with the quick gate as soon as `P5_GATE_DONE` fires, followed by d1/i1/t2 as they
finish, then the P5b re-census on the merged head (BRIEF-P5B §5).

### ID-73 — P5's end-of-phase review: one blocker, ten majors, two minors; P5b's first fix round is two packages; the APK lane compiles

`p5-results/p5-close-codex-review.md` (codex astra, on `37fc4fdb`). **Blocker 1:** on the apply thread under
an active transport, `EnsureBufferResourceForHandle` → `SyncPersistentMappedRange` runs a CLIENT producer that
reads `MappedData()` and pushes through the monolith adapter (`PushIsArmed()` checks only the transport), so a
coherent-map draw can replace transported bytes with client memory on the server - R-2/ID-52's rule broken on
exactly the path E5 claims; the corrupt-shadow control used an unmapped buffer and never reached it.
**Majors:** 2 framebuffer death still does driver work on the client thread (no wire delete; the joint's
`P4aFinalFixScenario` 11,589 wrong pixels are its shape); 3 tight ReadPixels drops `PACK_SWAP_BYTES`; 4
`StageAllocate` polls `retiredSeq` once and aborts as `RingOverrun` where R-9's lag should be a wait; 5 CI stops
on the indebted inproc census before the E1/E3 controls; 6 the E2 draw-drop control runs against the pull
library the preceding control left; 7 E3(a)'s selection includes by-design skips, so j0's skip rule breaks it;
8 `ringwaits` counts reclaim attempts and `SmallRing` asserts no wait; 9 losing the split implementation still
yields an all-skip green baseline; 10 the EGL-backed unit gates are optional in CI (no `REQUIRE_GPU`); 11 the
census runner can pass on zero executed tests and read stale Fatal evidence. **Minors:** 12 G5's pin self-test
bypasses production pin selection; 13 c1f's mutation runner names the test c1g renamed. Plus a corrections
list for the reports/docs and unnamed-debt rows.

**Ruling (ID-66's shape):** no further P5 round; the findings are P5b's first fix round in two packages -
**r1** (Claude premium): 1, 2, 3, 4, with the review's confirm steps as the controls; **r2** (codex astra):
5-13, plus the corrections list and the debt rows for the docs close-out. Both off `37fc4fdb`, landing on
their own gates + the quick gate, then the re-census. **The APK lane:** run 35115753041 on `94ce2ee7` -
`build` job SUCCESS (compiles again), `retrace` failures 0 so far; the user's condition is met.

### ID-74 — Codex takes over; the four migration packages and r2 integrate on `9606466a`

User authorization (2026-09-16): Codex replaces Claude and completes the remaining work.
Implementation continues in isolated worktrees, preserving the old Windows r1/d1 trees and
their work. Integration tree `/home/swung/w7/p5b-integrate-codex`, branch
`p5b/integrate-codex`; neither a phase review nor another P5 round is introduced.

Merged f1 `fe5ac006`, r2 `d50183cb`, i1 `e352e9a6`, t2 `fc5b52e4`, and d1
`fbe17d06`, reconciling shared emitter partitions and retaining all package scenarios.
Quick gate at **`9606466a`** (`/home/swung/w7/p5b-integrate-q1/`, `head.txt` pins it):
four builds; unit **1817 / 1817 / 1817 / 2123**, zero failures; inproc reduced split
**99 selected = 97 passed + 2 designed skips**; push GPU **1144 selected, zero failures**;
G1 **0 added / 0 removed / 0 resized / 0 renamed, `.text +0`**; G2 pull/push name
equality and G14 no removals; CI control smoke and the real E1/E3(a) controls pass.
The 16 additional push entries come from d1's ordinary scenario coverage; historical
1128 is not the current suite size. Docs close-out follows as `cc0d56e4` (source `dab23893`).

**Evidence correction:** P5's purported running final gate had already exited at E3(a):
four expected pixel failures plus two designed skips caused the old selection rule to refuse
the control. Part 2/4 did not run in that invocation. Its earlier results remain revision-pinned,
and the subsequent r2 fix does not retroactively turn it into a completed full gate.

**d1 dynamic census**, newly executed at its standalone head, is 77 selected, 28 pass,
49 fail; all old draw first blockers are gone. The Windows trace table was a static dump
cross-reference, not replay evidence. Vanilla and Sodium in-world render on both backends at
SSIM about 0.99998. Iris BSL and improved-transparency both first-stop on
`BlitNamedFramebuffer`; a new `p5b/blit-codex` package owns this common blocker. The static
`SetSwapInterval` prediction is withdrawn: the EGL remote backend already forwards that call
through the server control mailbox. Sync migration continues separately, with no claim that
FenceSync is a measured first blocker of these four traces.

**t2 correction:** 95 Begin + 43 Patch + 2 Bind = **140** original first blockers, not 138.
All reach successors; original 432 passes remain passing, 185 skips remain skips, and 27
original failures remain failures. The 51 newly exposed Vulkan wrong answers are named in
`p5b-results/t2-codex-v1.md`, not hidden by the reduced GLES lane.

Remaining before P5b close: r1 core fixes, named blit and any resulting target-trace blockers,
merged census, three APKs and Redmi inproc correctness/four-arm measurements, then the one
P5b full gate and one phase-end review. Redmi `2f7cbe2e` is connected; barrier tax remains
unmeasured. P6 follows the P5b exit, as ID-68 requires.

## ID-75 — P5b closing candidate, explicit stage profile, and the one closing review (2026-09-16)

The integrated candidate is `348d22a4b9c15cc571af840dedc4b4edc8c53dbb`.
It contains f1/r2/i1/t2/d1, r1, sync, named framebuffer blit, and the GLES generated-mip
storage follow-up. Four host builds completed. The one host closing gate is executing in
`~/w7/p5b-final-host-348d22a4/`; the separately frozen debt census is in
`~/w7/p5b-final-census-348d22a4/`. Do not label either complete before its terminal evidence.

The improved-transparency GLES trace contains one 128 MiB staging blob. Its isolated replay
passes with the existing `MOBILEGL_IPC_STAGE_MB=256` (SSIM 1.0); this does not establish that
the production default 32 MiB can accept the blob. Keep the default unchanged. Use the
explicit 256 MiB profile consistently for the complete trace census and all four device
A/B arms; retain default 32 MiB for the integration lane. Mapped capacity is not an RSS
increase. The Iris BSL GLES generated-mip follow-up passed at default 32 MiB, SSIM 0.997496.

The phase's only closing review is complete: `p5b-results/p5b-close-codex-review.md`,
0 blockers / 2 majors / 1 minor, source-confirmed. Fix the user-index declared-span extent
and long FenceWait watchdog mismatch in bounded follow-up commits; also reject unknown
ReadPixels reply status. These fixes receive targeted checks, not another review/full-gate
cycle. Keep the candidate's full-gate provenance distinct from the resulting fixed head.

The three exact-head APKs and Redmi correctness/A-B campaign wait for these fixes. Windows
`MobileGL-disagg` is fast-forwarded to the candidate for the runner; GitHub remains
`37fc4fdb` until the close is documented and pushed. The dev checkout and historical dirty
worktrees remain intact. P5b is not yet closed, and barrier tax remains unmeasured.

### ID-74 — During the integrator's rate-limit outage the user's Codex root task carried P5b's second wave; reconciliation rule

State found at 14:23 (2026-09-16): my Claude agents d1, t2 and r1 died on the session limit (`d1`'s worktree
gone; `t2` left three commits on `p5b/t2` and a merge; `r1` never started), while my i1 landed
(`p5b/i1@e352e9a6`: all five image/compute slots to zero, census 503/195/335/116, 89 wrong answers of which 82
on Magma) and r2 landed (`p5b/r2@d50183cb`, findings 5-13 with executed red-onces). In parallel, between 12:24
and 14:12, the user's Codex sessions (a "root task" with sub-tasks, four `codex.exe` live at 14:23, ninja
building the APK in `~/w7/p5b-apk-codex`) integrated d1 + f1 + i1 + t2 + r2 (`p5b/integrate-codex`), landed
r1's core fixes "with multi-tail command-ring retry", migrated the sync family (fence lifecycle and wait replies
to the apply thread), named-framebuffer blits, GLES mip storage verification, a user-index span bound and the
fence-wait budget / readback-status fixes, ran a P5b close review (0 blocker / 2 major / 1 minor, both majors
fixed on the tip), drafted the P5b docs (`p5b/docs-final-codex`), and staged an APK/Redmi runner
(`codex-apk-redmi-runner-v1.md`, device commands reserved to the root task). **The tip is
`p5b/apk-codex@82683d4a`** (26 ahead of `feat/disaggregated@37fc4fdb`, contains `indexspan-codex` and
`wait-codex` wholly). d1's own census: Minecraft 28/77 backend cases render under inproc (SSIM >= 0.99995),
49 first blockers led by `BlitNamedFramebuffer` 31 and `GenerateMipmap` 7 - both migrated later on the tip,
final census pending their run; the integration census on the tip: 811 passed / 203 skipped / 62 aborted of
1267. My P5 final gate on `37fc4fdb` stopped at the E3(a) control (close-review finding 7, fixed by r2) and is
not re-run there.

**Ruling.** No duplicate agents: d1/t2/r1 are not relaunched; `p5b/t2`'s three commits are superseded by
the tip's t2 merge. The user's root task owns the P5b integration, the one host gate, the final census, the
APK build and the Redmi run until it says otherwise; I own the master progress doc, the landing of
`p5b/apk-codex` onto `feat/disaggregated` (fast-forward + push) when the root task's step completes or the user
says so, and the docs/ROADMAP reconciliation after their device numbers exist. Nothing in `~/w7/p5b-*-codex`
is touched by me.

### ID-75 — P5b's whole verb-migration wave is landed and pushed: `feat/disaggregated@171f1867`

The user ended the Codex budget and asked for everything compilable to be committed and pushed. Inventory before
landing: `p5b/apk-codex@82683d4a` was the integration tip (26 commits, 57 files, +4993/−364) and CONTAINED every
real branch - all P5 branches, `p5b/{c0b,f1,i1,r1-codex,r2,t2,t2-codex,docs-final-codex,integrate-codex}`.
The apparent exceptions were checked and dismissed: `p5b/d1-codex`'s one "unique" commit is the same sync patch
as the tip's `f018c0ef` (463 vs 464 insertions after a rebase) and its tree is 986 lines BEHIND the tip;
`p5b/{blit,docs,indexspan,wait}-codex` are `+0 unique`; `p5/v1-joint*`, `p5/c1*-joint*` and `p5/g1base` are
branches whose own commits say `[Scratch]`/preview-only. The only uncommitted work anywhere was three P5b doc
files in `~/w7/p5b-docs-final-codex`, committed as `696acce5`.

**Landed:** `37fc4fdb` → fast-forward to the tip `82683d4a` → merge the docs → **`171f1867`**, pushed to GitHub.
Bar met: `gen_pipe --check` and `gen_pipe_field_ownership --check` up to date; **all four flavours build**
(linux/push/verify/split, 15-18 s each on warm ccache); **G1 `.text +0`, 27814 symbols 0/0/0/0** - the pull
build is byte-identical, so none of the migration is reachable from it.

**What the wave contains, by package.** f1 (codex): the 11 clear/copy/mip slots - 34 lane aborts to zero.
i1 (Claude): `BindImageTexture`/`DispatchCompute`/`CopyImageSubData`/`MemoryBarrier`/`ShaderStorageBlockBinding`
+ two companions - 235 lane entries freed; 82 of its 89 wrong answers are Magma's split compute/image path, the
next round's biggest item. t2 (Claude, finished by codex): the four stream-output span rows, the XFB object bind,
the patch parameter, `kCapBackendOwnsXfbCapture`. d1 (codex): the nineteen indexed/instanced/multi-draw/indirect
slots on `draw_vbo`; split unit 2098/2098, `integration-split` 54/54. sync (codex): `FenceSync`/`ClientWaitSync`/
`GetSyncStatus`/`WaitSync`/`DeleteSync` on the apply thread, client-minted Fence handles, `{slot,gen}` only on the
wire - and it found that the frontend's `MGL_BACKEND_SLOT_PTR_LOCAL` guard had been returning nullptr for every
split session, which is why no census ever showed a `FenceSync` blocker. named blit (codex):
`BlitNamedFramebuffer` through scoped client-shadow bindings. mip (codex): generated-mip storage verified from
pushed descriptors. r1/r2 (the P5 close review's 13 findings). Then the P5b close review (0 blocker / 2 major /
1 minor) and its two fixes: the user-index span bound (`Count * IndexSize` must fit the declared run - a
two-byte span could otherwise make the backend read six index bytes) and the fence-wait budget +
`Fatal{ReplyStatusInvalid, "ReadPixels"}`.

**Where the goal stands.** On d1's census 28 of 77 Minecraft backend cases already rendered under inproc; the
named-blit and mip commits then took `improved-transparency-26.3` (DirectVulkan, SSIM 0.999914) and
`minecraft-1.21.4-fabric-iris-bsl-in-world` (DirectVulkan 0.997324, DirectGLES 0.997496) past their blockers.
The integration census on `348d22a4` was 811 passed / 203 skipped / 62 aborted of 1267 (from 432/185/505 at c0b).
Not yet measured on `171f1867`: the full census, the host gate, the APK build and the Redmi run - all four were
in flight when the budget ended.

**Open, in priority order.** (1) `Fatal{RingOverrun, "SEG_STAGE"}` on improved-transparency DirectGLES: a
134,217,728-byte blob against the 33,554,432-byte segment - the default capacity is the real debt; codex ran its
census with `MOBILEGL_IPC_STAGE_MB=256` as a declared profile, which is not a fix. (2) Magma's split
compute/image path (82 wrong answers). (3) rd12 DirectGLES `InitialBytesNotCarried/resource_respecify`; rd12
DirectVulkan `BarrierTimeout/Present`. (4) RGB three-channel CPU mip fallback. (5) The device numbers: the
barrier tax is still unmeasured because every earlier split arm aborted at `DrawElements`, which no longer
blocks.

**ID-75, verified on the pushed head.** Quick gate on `171f1867`: unit pull/push/verify **1817/1817** each;
`integration-split` under inproc **107/107** (22 at P5's close - the migration packages added 85 lane entries and
every one passes); push `integration-gpu` **1148/1148**; split under monolith transport **1267/1267** - the
monolith path is unmoved; both G5 scripts rc=0; G1 `.text +0`, 0/0/0/0. The split unit lane was 2136/2138 with
two reds, both from the last codex commit's own cases (`PipeWireCodecTest.UserIndexSpan*`): side 0 (the encoder)
passed and sides 1-2 (the forged decoder, the sink) aborted as designed but their diagnostic read back empty.
Cause, fixed in `7cb29d46`: **the library truncates its log file the first time a process writes to it**, so
`RunInChild`'s delta read (`ReadLog().substr(before.size())`) - correct for the one-fork cases every earlier
suite used - slices the second and third child's log from an offset past the end of a file that child had just
truncated. The harness now empties the log before each fork and reads it whole; production is untouched.
`PipeWireCodecTest` 88/88, split unit **2138/2138**, push unit 1817/1817. Pushed as `7cb29d46`.
