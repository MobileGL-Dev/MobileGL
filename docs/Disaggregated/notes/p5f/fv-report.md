# P5f fv — 反向通道归属与遗留对象边界

> 包树 `MobileGL-p5f-fv`，分支 `codex/p5f-fv`，基线 `4667e13b`。
> 已验证代码头 `1153c561`。WSL `~/w7/p5f-fv`，日志 `~/w7/p5f-logs/fv-*`。最终门结果见 §4。

## 1. 改动

`PipeInputs::RecordError` 的 transport 臂现在调用 `gMGPipeCallbacks.OnGlError(code, message)`。
回调补上借用文本参数，且仅在同步调用期间借用：server producer 在返回前调用既有
`PostGlError`，复制 code、截断后的 message 和 NUL 进同一个 SEG_EVENT。
`kEventGlError` 的 wire 形状、1024 字节上限、FIFO 发布与 client drain 时机均未改变。
没有建设 P9 的异步槽池、跨 verb 排序或 texel 拉取。

transport 错误发布不再先执行 `MGP_STICKY_FORWARD_PULL(RecordError)`，因此它不会被误计为
残余 frontend pull，也不因双块 poison 阻止合法的反向错误事件。该计数宏只留在原 monolith 臂。
本包不修改 `FieldOwnership.def` / generated 表；阶段集成者负责把 legacy accessor 分类收口。
缺少回调是 `Fatal{RoleViolation, "OnGlError.callback-missing"}`，不静默丢错。

`ServerSession::Accept` 安装第四个 producer，沿用不同函数重复安装的
`callback-double-install` 检查。`Close` 在 apply join 之后只卸载仍属于自己的函数，不覆盖
其他 owner。新 `ServerOnGlError` 使用当前 active session；active 指针在 callbacks 安装完成后
release 发布。Accept 的 owner claim 用 CAS，连握手进行中的 session 也占有唯一的进程 callback /
segment-resolver 名额；同一对象重复 Accept 仍返回 invalid-argument，另一个对象则具名拒绝。
失败握手与 Close 对称释放 claim，没有加入多 session 支持。

`MGPipeAnnounceBufferGpuWritten(SharedPtr<BufferObject>)` 明确成为 monolith-only 对象入口。
transport 在第一次对象解引用之前 `Fatal{RoleViolation, "gpu-written-legacy-object"}`；
删除原 Magma allocator scope 豁免，无法再通过 lifetime-id 探 client allocator 或落到
`MarkGpuWritten` 直写。纯 monolith 的原 lookup / callback / fallback 语义保留。
已带 record handle 的 `OnGpuWritten` 事件路径保持不变。

## 2. 复核与附带补漏

fe 已迁走 XFB 身份：`XfbProgramSource` 的 transport 侧持 server `ProgramArchive`，targets 持
`handle/start/end`，scatter 读 server staged shadow，结果走 `OnBufferWriteback`。
源码中的 `target.buffer` / `scatterProgram` 留作 monolith 成员；transport 的赋值、循环与写回
均走 handle/archive 臂。本包没有重做 fe 的快照载体，也没有改变 scatter 字节格式。

fr 复核还发现 Magma 的 `VulkanRenderer::Initialize` 会调用两个 hidden-resource 初始化方法，
构造前端 ShaderObject/ProgramObject。先前“headless 另有跳过初始化分支”的快速推断不成立：
仓内没有该分支，约 682 行实际是 line-width caps 读取。fv 在
`InitializeBlitResources` / `InitializeDepthMipmapResources` 自身入口统一排除 transport，
不论调用者使用什么 surface。wire blit/mipmap/draw 已有 native server 路径，无需这两个前端
program；monolith 方法体保持原样。无设备测试直接调用这两个生产方法，要求 transport 不触碰
前端编译器、program factory 或 Vulkan device。真实窗口 / P12 设备行为没有被此测试冒充。

## 3. 测试与 red-once

新增 `P5fReverseChannel` 共 9 个测试（Linux）：

- RecordError 必须经过已安装 callback，保留 error code / prefix / message，残余计数为 0。
- Accept / 重复 Accept / Close / 再 Accept 的 callback 所有权；Close 不擦掉外部替换的函数。
- 不同 callback 的重复安装、第二个 session 的 ownership 冲突、缺 callback 的 RecordError 均具名 abort。
- 不可读地址 0x1 作为伪造 BufferObject，分别覆盖 callback 缺失和存在两臂；必须 SIGABRT，
  不可因先解引用而 SIGSEGV，不可进入直写 fallback。
- 真实 apply thread 产生 error → gpu-written → buffer-writeback → 长 error → 空 error，
  client-side ring consumer 验证 FIFO、code、完整短文本、1024 字节截断含 NUL、空字符串及
  producer 临时字节在被覆盖后仍已复制；没有手造这些事件头。
- 两个 hidden frontend program 的生产初始化方法，在无 device / factory / client context 时
  都直接跳过 transport 构造。

R-16 实际执行：WSL 临时删除 RecordError 的 callback 投递，
`GlErrorsUseTheOwnedCallbackWithoutAResidualPull` 因 callback 次数 0 而非 1 及消息缺失真红；
恢复原源码后当时全部 8 项通过，工作区 diff 为空。
日志：`fv-red-once.log` / `fv-green-after-red.log`。第 9 项随后补用于双 session owner 拒绝。

## 4. 门

| 门 | 结果 |
|---|---|
| split build | `1153c561`，rc=0（`fv-build-final.log`） |
| unit | **2295 条，2285 PASS、10 基线 skip、0 failed**；包含新增 9 项（`fv-unit-final.log`） |
| 普通 integration-split | **179 条，175 PASS、4 基线专属车道 skip、0 failed**（`fv-isplit.log`） |
| strict integration-split | **179 条零失败**；空表棘轮、零 Fatal / ordinary Admitted / ESCALATED（`fv-strict-gate.log`） |
| 双块 split + magma | **210 条，204 PASS、6 基线 skip、0 failed**；空表棘轮通过、零字段 Fatal / Admitted（`fv-dualblock-final.log`） |
| pull / push | 构建 rc=0，生效宏分别 absent / `MOBILEGL_PIPE_PUSH=1` |
| G1 | 两项 fail-on 硬门 rc=0：`.text` 10806051 → 10806051，defined symbols 27815 → 27815；0 added / removed / resized / renamed；data/bss/rodata section size 也零差（`fv-g1.log`） |
| generators / closure | 全 check/self-test 通过（12/13/27 个阴性控制），include closure 4 probes、0 skipped、0 problems（`fv-gens.log`） |
| doc citations | 与 f1 / wave2 相同的两处 baseline basename 歧义；不报成 strict 全绿 |
| integration-gpu / G2 / G14 | 整阶段集成者在 fs + fr + fv + 字段分类树执行；本包未删既有测试、未改公共入口 catalogue |

R16 扰动只在独立 WSL 树临时执行并恢复，未提交失败版本。
同一目录的 strict / 双块 / GPU 日志不并行覆盖；各车道结论在其运行后立即保存。
G1 的最后一个源码差异只位于 split-only 的测试宏声明，不影响已构建的 pull / push 库。

设备门没有可用的指定 Redmi `2f7cbe2e`；不申请或操作其他 unauthorized 设备。
