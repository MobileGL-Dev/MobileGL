# P5f 收口报告

**状态：DONE，2026-09-20。** f0 / f1 / fc / fe / fm / fs / fr / fv 已全部集成，
字段归属、主机出口门、Redmi 设备门与跨模型族阶段审查均完成。P6 前置条件解除，
其 a6 / c6 / spawn 仍未实施。本报告取代各分包报告中的阶段中间状态。

最终行为验收头 **`cfca93c7885fd8db1e881f91189ba59090ae9c52`**（WSL `codex/p5f-final`），
包含 Windows 收口头 `c42577a4` 的全部修复；之后仅补文档与设备复现工具。
交付分支为 `feat/disaggregated`。包提交与环境入口见 [HANDOFF-P5F.md](HANDOFF-P5F.md)。

## 1. 完成的行为

波次 2 的 fc / fe / fm 已把 EGL 控制面、Espryt 对象读点、XFB Begin 快照与 Magma 可达
资源/渲染路径改成值帧、server archive / handles / staged bytes。波次 3 完成剩余边界：

- **fs**：unpack/render shadow 按 native generation、served context serial 和角色核键；
  XFB server namespace 以 lifetime id 索引，包括 name 0 的 native 对象隔离。served context
  切换保留仍活着的 paused span，native context 销毁清旧世代。raw-depth sampler 走纯 native
  对象；server liveness 只由控制生命周期提供，verb stamp 不会复活它；双块诊断位按角色持有。
- **fr**：apply-side frontend registry / allocator 不再允许任何旧 scope 豁免；wrapper 在
  选择 legacy 臂前拒绝。无调用的前端 program-name helper 从 D-P 编译中移除，pull 不变。
- **fv**：错误经保留 message 的 OnGlError 发到事件环；callback 安装/关闭/重接受和单 owner
  CAS 有明确生命周期。旧 GPU-written object fallback 在解引用前拒绝，Magma transport
  初始化不再构造两组隐藏 frontend 资源。四个 callback 统一投递实际 active session。

字段机器表最终为 **41 RECORD_SUPPLIED / 6 APPLIER_DERIVED / 0 BARRIER_PULLED / 16 FATAL**。
带 client 指针的旧 getter 为 FATAL；消费者直接读取 server records，没有把句柄记录谎称为
“供给整个旧指针字段”。`EmittedCallSuppliesTheWholeField` 的 8 个拒绝保留。
生成器拒绝任何 production BARRIER_PULLED 行，历史 debt 仅在 synthetic admission fixture
中测试。即使伪造 fresh stamp，FATAL getter 也必须拒绝；sticky forwards 同样执行归属。

新 `EachWireFrameHasZeroResidualPulls` 在两后端各注册独立 RSP 车道：连续三个改变 uniform
的帧逐个断言像素、真实 applied verbs 和 `rsp=0`。Magma 的 draws stat 尚未实现，绘制证据
取实际变化的像素，未以缺少 draws 计数假称执行过 draw。

## 2. 七项出口门

| 出口 | 最终结果 |
|---|---|
| 1 双块车道 | **212：206 PASS / 6 既有 skip / 0 failed**；两后端，同一归属与拒绝机制 |
| 2 strict / 棘轮 | ordinary / strict split **180：176 PASS / 4 skip / 0 failed**；Fatal、普通 Admitted、ESCALATED 均零；strict 与 dualblock 两份 expected 文件为空 |
| 3 字段 / 逐帧 RSP | production **0 BARRIER_PULLED**；`integration-p5f-rsp` **2 PASS / 0 skip**；设备 36 windows 全零 |
| 4 两后端能力声明 | GLES run-ahead ARMED；Magma 明确不发布 capability、运行 lockstep；理由如下，无 frontend 豁免 |
| 5 G1 / G2 / G14 | pull `.text` **10806051 → 10806051**，defined symbols **27815 → 27815**，added/removed/resized/renamed **0/0/0/0**，data/bss/rodata 不变；pull/push CTest **3016 == 3016**；f1 名集合 **3646 → 3744，+98 / -0**，无重复名 |
| 6 red-once | 逐包控制均真红后还原绿；f1 双块 knob=1 绿 / knob=0 真断言红；fresh FATAL、结果集完整性、非 singleton callback 修复均各有真实红转绿 |
| 7 Redmi | 指定 `2f7cbe2e`，六个不同 boot-id，同一最终库，两后端 role0/role1 clean-boot 配对及 monolith sanity；**60 PASS / 6 skip / 0 failed** |

Magma 不发布 run-ahead 的原因仍是 P7 buffer/native-format 功能边界，以及 wire render-pass
提交后等待 GPU 再退休对象的现有生命周期/性能契约。role-split 下 server 块没有 client
指针，lockstep 不能掩盖这种读；registry/allocator guard 对 barriered 记录同样拒绝。
fs/fv/字段收口已经完成，不再用这些已解决事项作为不发布 capability 的理由。

## 3. 主机完整验证与门的可信性

| 检查 | 结果 |
|---|---|
| split / pull / push 构建 | 全部通过 |
| unit | **2310：2300 PASS / 10 既有 skip / 0 failed** |
| GPU（审查修复后的最终代码） | **1377：1118 PASS / 259 既有功能、驱动、专属车道 skip / 0 failed** |
| field generator | check 通过；**19 个 negative controls + 13 个 synthetic admission controls** |
| pipe / dirty-surface / include closure | check 与现有 self-test 通过；include closure 4 probes、0 skipped / 0 problems |
| 文档引用 | 两处旧 basename 歧义改为完整路径，strict 检查通过 |

双块 gate 不只检查“空观测集合等于空预期集合”：必须有非空 discovery，JUnit 与发现集合
完整且一一对应，missing / extra / duplicate / notrun / disabled 均失败；绿和 skip 条目也
扫描 marker，GLES/Magma 私有日志均核所有权。CI 空 expected 表要求 ctest rc=0。
六个允许 skip 按**确切名称和原因**锁定：四个专属计数车道的 sibling，两个 Magma clip
功能限制。任何额外 skip 都失败；RSP 另有不可 skip 的双后端独立门。

P7 已知红 `integration-magma-p7-known-red` 真实执行唯一用例，**1 failed / 0 skip**，
退出码 8，具名 `Fatal{UnmigratedVerb, "Magma:mipmap-native-format@P7"}`。
这是保留的功能边界，不计入 PASS。旧 readback fixture 也已补完整 FBO/pack/context/sampler
records，在无 residual fill 时通过，`DstSize=80` / tight reply `48` 断言未放松。

## 4. 阶段审查与设备

跨模型族审查通过只读 Claude Code 完成，结果记录 `claude-opus-5[1m]` /
`claude-haiku-4-5`，实现方为 GPT 系列。发现 P1 结果集可不完整、P2 三个 callback 投错
singleton，均已修复并真跑红转绿；没有遗留本次审查 P0/P1/P2。详情、原文及局限见
[close-review.md](close-review.md)。

最终库 SHA256 `764a790d024fbe1b83ebdec5d896cc77ecda4f4a2698d63d2b2d16239f40e74c`，
六臂都核主机/设备 hash 一致。GLES 每臂 11 PASS；Magma 每臂 9 PASS + 两个既有
per-distance clip-enable skip。全部 36 个 stats windows `rsp=0`；两 inproc GLES 臂各
`vbs=72`，Magma 各 `vbs=68`，monolith 为 0。实际 backend、strict/role 和 capability
日志均核对，不以环境回显代替运行证据。

设备 runner 使用公开 GL/EGL、AImageReader native window：clip distance 3 项、layered copy
6 项、mipmap 2 项。没有把 host 私有 F1/Ct/DualBlock 测试冒称 Android 覆盖。
suite 时间只记录一次 clean-boot 样本，不作为 MC/FPS 性能结论；数值与全部 boot-id 见
[device-report.md](device-report.md)。复现工具位于 `tools/device_bench/p5f/`。

## 5. 证据索引与边界

主机证据根 `/home/swung/w7/p5f-logs/`：

- `exit-reviewed-{unit,gpu}.log/xml`：审查修复后的完整主机结果。
- `exit-reviewed-{strict,dualblock,negctl}-gate.log`：最终 marker、双块与机关控制。
- `exit-reviewed-g1.log`、`exit-reviewed-name-gates.log`：二进制与测试名门。
- `exit-rsp.log/xml`、`exit-rsp-lane.json`：双后端逐帧 RSP；`exit-known-red.log/xml`：P7 拒绝。
- `close-fresh-{red,green}.log`、`exit-review-gate-{red-once,green}.log`、`fv-review-*.log`：收口修复控制。
- 各分包报告列出该包独立门与 red-once；中间计数不替代本文最终计数。

设备证据 `C:/Users/geekerwan/.codex/tmp/p5f-redmi-exit-cfca93c7/`；审查原始结果
`C:/Users/geekerwan/.codex/tmp/p5f-final-review-de78f47d/result.json`。

P7 的 Magma buffer/native-format 等功能、P8 client arrays、P12 window/present 到达，
以及 P5e 历史 E1 对照设计条目继续按其原阶段跟踪。它们保留具名边界，不是未完成的 P5f。
本次不启动 P6，也不宣称两进程 spawn 已实现。
