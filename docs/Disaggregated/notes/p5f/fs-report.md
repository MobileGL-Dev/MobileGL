# P5f fs — 静态状态世代与 server liveness

> 基线 `4667e13b`，分支 `codex/p5f-fs`；行为代码 `35ad51d8`；默认 XFB 身份测试补强 `1b701020`。
> Windows 树 `MobileGL-p5f-fs`；独立 WSL 树 `/home/swung/w7/p5f-fs`。
> 不改 FieldOwnership 分类，不重复 fe 的 XFB archive/handle 迁移，RecordError 由 fv 完成。

## 1. 实现

**unpack 与 render-state shadow 的有效期成为显式键。** `ContextEpoch` 由 native ES
context generation、served-context serial 与执行臂（monolith / transport server）组成。
`ScopedDefaultUnpackState` 每次进入先核键；新世代或另一臂必须重发六个默认 pixel-store 值，
随后再设 tight alignment 并恢复。render-state 在版本号快速命中之前核键；键变动先调用
`InvalidateSyncedRenderState()`，相同参数字节和相同 version 也不能跳过重推。
这里的角色是 backend 执行臂：transport 下后端只由 server 调用，client 使用 remote table；
它不依赖读取 client 的 GLContext 地址。pull 编译不包含新键和分支。

**XFB 的 server 命名空间与 monolith 分开。** transport map 用既有记录携带的、不会复用的
`BoundStreamOutputLifetimeId` 作 key，GL name 不再是跨 context 身份；monolith 保留原 GL-name
语义。每个执行臂持有自己的 map、current/cached entry 与 scratch 状态。native generation
变化即丢弃旧 driver ids / spans；角色切换撤销 driver-binding 快速命中。

served-context serial 变化不清空仍活着的 XFB map：fe 已证明返回 context 不会重发 Begin。
`set_context_values` 恢复的 lifetime id 选择原来的 paused/pending span，并重新绑定其 native XFB
object。包括 virtual name 0 在内，不同 server lifetime 都有自己的 native object。server
scattered capture 的 scratch buffer 也按 capture object 分开，不能让另一暂停 span 的后续捕获
覆盖先前数据。native context 销毁统一清除；未实现的 DeleteTransformFeedback 仍由既有 class-C
边界拒绝，不把 GL name 偷换成 lifetime id。

本包验证的是同名不同身份和世代的隔离，不将它扩大为 P6 的多 client/session 功能承诺。
fe 已把 server map 的 program/target 值改成 Archive / buffer handles，fs 保持该方向。

**raw-depth sampler 的 transport 路线完全原生化。** 重核发现原来的 static frontend
SamplerObject 不只是缓存问题：setter 可读 client context，`SyncToBackend` 的 identity lookup
还会进入 client registry。transport 现在只构造 BackendSamplerObject，直接设置 native sampler
的 NEAREST min/mag、NONE compare mode、ALWAYS compare func；不构造、查询或同步前端
SamplerObject。native generation 变化重建 driver id；旧世代析构不会删除新世代复用的名字。
monolith 原路线不改，pull G1 保持恒等。此接缝与 fr 的 registry guard 收紧配套。

**IsLive 的答案由 server control 生命周期持有。** transport 的 `PipeInputs::IsLive()` 读取
server-owned liveness，完全不看 client `LiveContext()`：成功 make-current（包括 held tuple 的
重复绑定）设 live，client release-current 清 live，release-resources、backend/context 销毁与
loop stop 清 live。native context 可以按 ID-54 继续绑定在 apply thread 上；逻辑上的 client
release 仍必须答 false。surface 创建本身不构成 served current context。verb stamp 只刷新身份
token，不能把 teardown 后的 stale command 当作重建而复活 liveness。

**S13 的诊断位也不跨角色读。** 双块开启时，applier 入栈诊断使用 thread-local observer：
server 看到自己是否正在 apply，client 只看到自己的 false。共享块控制臂继续使用原 shared
atomic，原 R-1 诊断与 negative-control 不变。真正的跨角色等待从来都是 wire 的 `appliedSeq` /
`retiredSeq`，本包没有用 TLS 替换这些同步，也不从“client 观察不到 server”推导队列已排空。

## 2. f0-statics 全清单复核

| f0 项 | fs 结论 |
|---|---|
| S1 render-state shadow | 已补 native/served/role epoch。原 MakeCurrent invalidation 保留；新门覆盖相同 version / bytes 时的世代更换。 |
| S2 unpack shadow | 已补同一 epoch。每世代重新发布已知默认状态，不再依赖新 context 的 GL 默认值碰巧匹配旧 shadow。 |
| S3 Espryt per-draw memo 家族 | 保留现有 context identity/serial + native generation 或 record-content key。server 路线的身份源已是 applier；没有另造一套 memo。 |
| S4 native bindings / scratch / pools / EGL handles | 仍为 backend/server 私有；既有 MakeCurrent/DestroyEGLContext reset 链保留。native tuple 的 ForgetCurrentTuple / NoteNativeContextGone 已在波次 2 接线并有实际 destroy/recreate 测试。 |
| S5 legacy TwinLookupMemo / fb-slot memo | transport 可达读者由 fe/fm/fr 的 record arm 处理；monolith legacy memo 不被当作 server 值来源。地址键的 legacy 存在不等于 server 还可调用。 |
| S6 XFB | 已分执行臂，server 以 lifetime id 重键，native generation 清理，返回 served context 保留其 span；server capture scratch 按对象隔离。 |
| S7 raw-depth sampler | D 项已查明：BackendSamplerObject 析构有 generation gate，但旧 SyncToBackend 不重建死 driver id。transport 已改纯 native、按世代重建。 |
| S8 viewport-index 单向 latch | 仍是保守的 server process 属性；历史 true 只增加检查，不会略过必须的工作，无需按 context 清零。 |
| S9 buffer mutation epoch | 既有 server role guard 与单调 clock 保留；不是 client 共享状态内容。 |
| S10 Magma | renderer generation / manager 成员仍按 renderer 生命周期；动态影子在每次 BeginCommandRecording 的 observer 中 reset，包含中途 Flush 后的新 command buffer。fm 新增 wire resources 由 renderer/manager 持有。 |
| S11 MG_Impl/Pipe 单例 | client-only emit/tracker/publication 状态；原 FreshlyPrimed reset 保留。它们不提供 server fallback。 |
| S12 client query/sync registry | client-only；多 client/context 扩展仍属原范围，不为 P5f 在 server 镜像前端表。 |
| S13 Remote 单例及诊断 | session/scratch/hook 各有既有角色所有权；`g_applyThreadInsideApplier` 的双块跨角色观察已取消，共享控制臂保持。watermark 同步不变。 |
| S14 Pipe 核心 | gPipeInputs 双块由 f1 持有；g_applier 仍经控制记录 reset。fs 增加 server-owned liveness，route/diagnostic 计数不变。 |
| S15 MG_State lifetime allocators / scopes | client allocator 与诊断 scope 保持各自角色；transport raw-depth 不再构造会触发这些 client 语义的 SamplerObject。 |

f0 的“相同 tuple 且 native context 换代”不是另造一次 native bind 的理由：已有控制层在资源
销毁时忘记 tuple，新 surface 创建实际绑定并执行 invalidation。fs 另给消费端 shadow 独立
epoch 防线，避免 correctness 仅依赖这条调用链。原真实 EGL destroy/recreate 测试仍通过。

## 3. 有意义的 red-once

新增五个 `ContextEpochTest` 和两个 `ServerLoopEglTest`。前者调用生产 unpack/sampler/XFB/
liveness/诊断函数，用可观测的 driver-call / native-id / span 状态断言；后者在真实 headless EGL
context 的 apply thread 上验证 driver blend 状态与控制帧生命周期。

健康代码先 **7/7 PASS**。随后只在本包 WSL 树临时回退对应生产逻辑，构建成功后运行同一组
七条测试，**7/7 FAILED，rc=8**，每一条都红于自身断言；恢复原文件、重新完整构建后同组
**7/7 PASS**。没有提交测试旋钮，也没有把编译失败当 red-once。

| 测试 | 临时回退与确实失败的意义 |
|---|---|
| UnpackDefaultIsRepublishedForANewNativeContextOrRole | 关闭 unpack epoch 比较，新 context / 另一臂只产生 2 次 alignment 写而不是六默认值加 save/restore 的 8 次。 |
| RawDepthSamplerOwnsANativeIdForEachContextGeneration | 关闭 native generation 比较，第二世代沿用同一 sampler id、参数只发布一次。 |
| SameXfbNameInTwoServedContextsDoesNotAliasThePausedSpan | server key 改回 GL name，同名不同 lifetime 共享 native id / paused span。 |
| ServerLivenessDoesNotFollowTheClientContextOrAVerbStamp | 改回 client LiveContext：client 存在却 server dead 时误报 true，client 不存在却 server live 时误报 false。 |
| DualBlockApplierDiagnosticIsRoleLocalWhileSharedControlRemainsArmed | 改回 shared atomic，双块时 client 又能看到另一线程的 server 入栈；健康版同时验证 knob=0 的共享控制仍可见。 |
| RenderShadowCannotSkipAnUnchangedVersionAfterContextEpochChanges | 关闭 render epoch 比较；先建立 shadow，再改变真实 driver blend 状态并移动 native/served epoch，相同 version 导致错误跳过重推。 |
| ServerLivenessFollowsControlFramesAcrossReleaseAndRecreation | client LiveContext 不能回答 surface-only、release-current、重复绑定、release-resources、同 handle 重建、stop 的 server 状态。 |

日志：`fs-red-once-build.log`、`fs-red-once.log/xml`、`fs-restored-build.log`、
`fs-restored-green.log/xml`。回退完成后的 WSL `git diff` 为空。另重跑 f1 机关阴性对照：
旋钮开真绿、关真红，失败是 distinct-block 用例自身断言。

默认对象另做了更窄的对照：上述 XFB case 使用两个真实 GLContext 构造得到各自 name-0
lifetime id，断言非零且不同，再在 server 侧都调用 `BindTransformFeedback(0)`，要求 native
ids 非零且不同。临时把 native 创建条件从 `(server || name != 0)` 回退成 `name != 0`，
该 case 真红 rc=8，明确报出 0 对 0 的错误别名；恢复后 1/1 PASS。
证据 `fs-default-xfb-{green,red-once,restored-green}.log`。因此分开的不只是 CPU map key，
还有 driver 对象。该补强不新增测试名，也不改变前述生产代码或门禁数量。

## 4. 门禁

| 门 | 结果 |
|---|---|
| split build | rc=0 |
| unit | **2293：2283 PASS / 10 既有 skip / 0 failed** |
| integration-split | **179：175 PASS / 4 既有 skip / 0 failed** |
| strict | 同一 179 条零失败；Fatal / Admitted / ESCALATED 均为 0，空 marker 棘轮通过 |
| 双块 split + magma | **210：204 PASS / 6 既有 skip / 0 failed**；零字段 Fatal、零 Admitted，空棘轮通过 |
| integration-gpu | **1375：1118 PASS / 257 既有功能/驱动/专属车道 skip / 0 failed** |
| pull / push | 实际构建 rc=0；生效宏分别 absent / PUSH=1 |
| G1 硬门 | 对 e75e00cb：`.text` **10806051 → 10806051**；defined symbols **27815 → 27815**；**0 added / removed / resized / renamed**；data/bss/rodata 也零差 |
| G2 | pull / push 各 **3016** 个 CTest 名，集合差 0 |
| G14 | f1 **3646 → fs 3723**，累计新增77、删除0；其中波次2新增70，本包新增7 |
| generators / include closure | check 全绿，self-test 12 / 13 / 27 控制通过；4 probes，0 skipped / 0 problems |
| doc citations --strict | 两处 f1 已确认的 basename 歧义未变，未报告成绿门 |

全部日志位于 `/home/swung/w7/p5f-logs/`：`fs-{unit,isplit,gpu}.log`、
`fs-strict-gate.log`、`fs-dualblock-gate.log`、`fs-negctl-gate.log`、`fs-flavours-gate.log`、
`fs-g1.log`、`fs-name-gates.log`、`fs-gens.log`。strict / dualblock / GPU 串行运行，
各自 marker 摘要在被后续车道覆盖前已保存。flavour 构建和生成器不占用这些运行日志。

## 5. 集成边界

FieldOwnership 的13字段/17行分类由 root 收口，本包未修改；RecordError / GPU-written fallback
由 fv；registry guard 由 fr。fs 与 fr 的 raw-depth 接缝是纯 native helper，集成时应保留其
generation 重建，不恢复 frontend sampler identity lookup。

本包新键和 server 分支均被 `MOBILEGL_BUILD_DISAGGREGATED` 隔离；传统 pull 编译的行为与
符号门保持原样。设备门、阶段异模型族终审和 P5f 总出口由集成者执行，本包不代为宣告完成。
