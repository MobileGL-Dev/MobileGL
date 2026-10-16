# P5f 波次 2 集成报告

> 包：fc `b5c60d5a`、fe `203f59cc`、fm `3cc397d3`（fm 行为代码 `552ade5b`）。
> 按 fc → fe → fm 合入，验收代码头 `78ec3b67`；其后只补报告、注释和门名称。
> WSL 验证树 `/home/swung/w7/p5f-int`，日志 `/home/swung/w7/p5f-logs/`。

## 1. 结果

Espryt 与 Magma 的合并双块车道 **210 条：204 PASS、6 既有 skip、0 failed**。
`strict-expected-markers.txt` 与 `dualblock-expected-fatals.txt` 均为空集合，双向棘轮通过；
普通 Admitted、ESCALATED 与字段 Fatal 全为零。P5f 尚未收官：静态量、registry/反向通道复核、
字段表分类及设备门仍在后续范围，不能从本车道全绿推导这些工作已完成。

fe 改读服务端 framebuffer/texture/program 记录，扩 Begin XFB 快照，并修复快照被 make-current
重置丢失。fm 接通服务端纹理/FBO、无应用缓冲的 draw/compute、程序 Archive 和 uniform/image
描述符；不再把“所有 draw 都先到 buffer-legacy-arm”当事实，基线 gl_VertexID 像素用例已经证明
它不成立。支持范围由实际记录消费者决定，已有 P7 buffer 路径仍在触及客户端对象之前具名拒绝。

fc 的控制帧复审修复一同进入集成：wire 回复序号、void 成功回复、MetalLayer 拒绝、无指针
成员检查与并发/重入 probe 配对。分包说明见 [fc-report](fc-report.md)、[fe-report](fe-report.md)、
[fm-report](fm-report.md)。

## 2. 集成接缝与棘轮

- `PipeApply.h` 保留 fe 的 XFB span 状态与 fm 的 mip 工作字段；双方的 copy-image handle、
  storage-block program handle 使用同一份定义和 sink 接线。
- `PipeApply.cpp` 同时保留 XFB make-current 生存期修复与无 buffer backend ops 时的纹理 staged
  接收/重定义/销毁。不会为 Magma 假注册 buffer 支持，也不会与 Espryt hook 重复接收字节。
- 独立 fm 为可独立验证而携带的 Espryt copy-image/readback 接缝，与 fe 合并后仅保留一份。
- 合 fc 后实测仍是原 **6 对、155 条具名红**（`int-after-fc-dualblock-*`）。
  合 fe 后为 **1 对、2 条 Magma Clear 红**（`int-after-fe-dualblock-*`）。
  合 fm 后为 **0 对、0 红**；没有新增预期对。
- fe 负责原五对 Espryt 首阻塞；fm 负责 `GetFramebufferBindingSlot@Clear`。
  `GetTextureObject@CopyImageSubData` 是双方共享端点接缝，集成只记一次删除。
  strict 的两条原有 stale 行已有 f1 基线证据，其余八对随 fe 的读者迁移消失。

复审还修复并验证了：无缓冲 shader 支持、独立 family caps、sampler/image 记录解析、纹理 view
的 native STORAGE 用途升级及旧内容保留、非分层 image 忽略 Layer、sRGB 开关、1D 附件视图、
同 image 传输 layout 与临时 render-pass 资源的 GPU 生存期。

## 3. 最终门

表中 PASS 与 skip 分开计数；skip 不作为像素正确性的证据。

| 门 | 总条目 | PASS | skip | failed / 结论 |
|---|---:|---:|---:|---|
| unit | 2286 | 2276 | 10 | 0 |
| integration-gpu | 1375 | 1118 | 257 | 0；含原有功能/驱动/专属车道跳过 |
| integration-split，旋钮关 | 179 | 175 | 4 | 0 |
| strict，旋钮关 | 179 | 175 | 4 | 0；零三类 marker，空表棘轮通过 |
| 双块 split + magma | 210 | 204 | 6 | 0；空表棘轮通过 |
| Magma 主车道（包含在上一行） | 31 | 29 | 2 | 0；两项为既有 ClipDistance 驱动能力跳过 |
| f1 机制阴性对照 | 1 | — | 0 | knob=1 真绿；knob=0 因 distinct-block 自身断言真红 |
| P7 PackedFloat 格式探针 | 1 | — | 0 | 按预期失败，JUnit 与精确具名 Fatal 核对通过 |

pull / push 最终增量构建都 rc=0，生效宏分别 absent / `MOBILEGL_PIPE_PUSH=1`。
G1 带 `--fail-on-text-delta --fail-on-symbol-set-change` 硬门 rc=0：
`.text` **10806051 → 10806051**，defined symbols **27815 → 27815**，
**0 added / removed / resized / renamed**；data/bss/rodata 也零差。
G2：pull / push 各 **3011** 个 CTest 名，集合差为 0。
G14：f1 **3646 → 3716**，**新增 70、删除 0**，不存在重复注册名。

生成器 check 与 self-test 通过（12 / 13 / 27 个阴性控制），include closure 4 probes、
0 skipped、0 problems。文档引用 strict 检查仍有基线两处 basename 歧义：
`CONTRACT-P5.md` 的 `Init.cpp:44` 与 `Core.cpp:39`；不是本次新增，未把它报成全绿。

核心证据：`int-final-{unit,gpu,isplit}.log/xml`、`int-final-strict-gate.log`、
`int-final-dualblock-gate.log`、`int-final-negctl-gate.log`、`int-final-known-red.log/xml`、
`int-final-{pull,push}-build.log`、`int-final-g1-hard.log`、`int-final-name-gates.log`、
`int-final-gens-gate.log`。strict、双块、GPU 串行执行，marker 判定在各自运行后立即保存，
没有用被后续车道覆盖的私有日志补证。

## 4. Red-once

- fc：控制帧投递回退真红；后续序号、成功回复、MetalLayer、probe 配对与指针成员控制均
  有实际红/绿证据，见 fc 报告。
- fe：`SplitCaptureSnapshotSurvivesMakeCurrentUntilObjectRelease` 先红后绿。
- fm：ProgramSource 退回 archive 旧 binding 值真红，record overlay 版本绿；另在集成树
  `a6144426`（尚缺 named-mip 修复）运行 staging 生产测试，level-1 extent 为 0 而非 2×2×1
  真红；合入修复后同用例 1/1 绿。证据为 `int-fm-staging-{red-once,green}.log`。

## 5. 明确保留的边界

- `FieldOwnership.def` 仍有 **13 个 BARRIER_PULLED 字段（17 行，含 forward 重复行）**。
  可达消费者已改道；legacy accessor 分类、P8 client arrays 和 fv 的 RecordError 要在收口
  逐项裁定，不能为把表清空而伪称旧指针 getter 是 record-supplied。逐帧 rsp 零门尚未单独验收。
- Magma 不发布 `kCapRunAheadApply`。普通无缓冲 draw、采样、compute imageStore 均在严格双块
  下有真实像素证据；应用 buffer 消费、部分 native 格式/窗口能力仍是 P7/P12，具名拒绝。
  lockstep 不作为跨角色读写的豁免，具体出口门 4 的回答见 fm 报告。
- R11F_G11F_B10F mipmap 的真实旧基线 inproc 像素测试也失败；保留单独已执行的
  `integration-magma-p7-known-red`，强制 `Magma:mipmap-native-format@P7`，没有靠移标签藏红。
- fs / fr / fv 尚未开工。XFB snapshot/identity 与部分 registry 入口已被 fe/fm 改动，下一轮
  先重核 f0 清单，不重复处理已退役的站点。`PipeInputs::IsLive` 与 `RecordError` 的边界仍在。
- 设备门未执行：本次 `adb devices -l` 只见 `10AG930MXC002B3 unauthorized`，未见指定的
  Redmi `2f7cbe2e`。host lavapipe 结果不替代设备门。
- 本轮分包并行复审不替代 ID-66 要求的 P5f 阶段收官异模型族审查；P6 仍不开工。
