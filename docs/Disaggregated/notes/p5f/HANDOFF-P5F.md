# P5f 交接：已全部收官

> 2026-09-20。交付 worktree：
> `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`，分支 `feat/disaggregated`。
> 完整结果见 [close-report.md](close-report.md)，设备见 [device-report.md](device-report.md)，
> 阶段终审见 [close-review.md](close-review.md)。最终行为验收头 `cfca93c7`，之后仅文档/复现工具。
>
> **FCL 游戏补测（2026-09-20）**：源码 `aa78f102` 已编入并安装 `.mgdebug.debug` FCL，
> MC `26.3-rc-3` 的 GLES inproc / monolith 与 Magma monolith 各完成至少60秒世界运行；
> **Magma inproc 真失败于 P7 `buffer-legacy-arm`，尚不能跑该游戏**。
> 见 [fcl-e2e-report.md](fcl-e2e-report.md)，不要将前面的公开 GL runner 当成 FCL E2E。

## 0. 当前结论

**P5f 已全部完成。不要重开 fe / fm 或 fs / fr / fv，也不要重复设备六臂。**
P6 的 P5f 前提解除；P6 的 a6 审计、c6 契约冻结和 spawn 实现均未开工。

| 包 | 完成证据 |
|---|---|
| f0 | `e75e00cb`，八份历史普查 |
| f1 | `cfcc12f7` + `418c765c`；[f1-report.md](f1-report.md) |
| fc | `b5c60d5a`；[fc-report.md](fc-report.md) |
| fe | `203f59cc`；[fe-report.md](fe-report.md) |
| fm | `3cc397d3`，行为代码 `552ade5b`；[fm-report.md](fm-report.md) |
| fs | `863fd1f5`；[fs-report.md](fs-report.md) |
| fr | `bf5ad470`；[fr-report.md](fr-report.md) |
| fv | `4cc4504b`，行为代码 `1153c561`；[fv-report.md](fv-report.md) |
| 字段 / 逐帧 RSP / 结果集门 / 回调终审修复 | 已集成到 `cfca93c7`；[close-report.md](close-report.md) |
| Redmi / 跨模型族终审 | 六个 clean-boot 臂完成；审查两项 P1/P2 均真实红转绿，无未决项 |

- 字段归属 **41 record / 6 derived / 0 barrier / 16 fatal**。遗留指针 getter 被禁止，
  消费者读 server records；8 个 whole-field refusal 保留，未用重标分类掩盖指针。
- unit **2310：2300 PASS + 10 skip**；GPU **1377：1118 PASS + 259 skip**；均零失败。
- 普通 / strict split **180：176 PASS + 4 skip**；双块 **212：206 PASS + 6 skip**。
  两份 marker 棘轮为空；strict 的 Fatal / Admitted / ESCALATED 均零。
- 双后端独立逐帧 RSP 门 **2 PASS / 0 skip**；Redmi 六臂 **60 PASS / 6 既有 skip / 0 failed**，
  36 个 stats windows 全部 `rsp=0`。设备 skip 仅 Magma 两个既有 clip-distance 限制各跑三臂。
- G1 `.text +0`、symbols 0/0/0/0；G2 **3016 == 3016**；G14 **3646 → 3744，+98 / -0**。

## 1. 下一阶段需要知道的边界

- Magma 仍不发布 `kCapRunAheadApply`；可达路径已经在 role-split + strict 下验证，
  lockstep 没有 frontend registry/allocator 豁免。P7 buffer/native-format 等功能仍具名拒绝。
- P8 client arrays、P12 window/present 到达语义等路线图债保留；不是未完成的 P5f 包。
- server context 的 unpack/render shadow 按 native generation / served serial / role 核键。
  XFB 以 lifetime id 分区；served context 切换保留暂停 span，native context 销毁才废弃旧
  driver objects。不要把 P6 的 reset 设计写成“所有 reset 都清所有活对象”。
- P6 a6 要核查进程启动和配置边界。P5f 不声称已经运行两个 OS 进程。
  控制帧、静态量迁移、反向回调归属已经落地，不应再列为 P6 待实现项。

## 2. 环境与复现

- Windows 收口树 `../MobileGL-p5f-close`，分支 `codex/p5f-close`；WSL 集成树
  `/home/swung/w7/p5f-int`，分支 `codex/p5f-final`。各包参考树保留，不 reset。
- 原始主机日志 `/home/swung/w7/p5f-logs/`；最终前缀 `exit-reviewed-*`。
  门脚本 `/home/swung/w7/notes/tools/p5f_gate.sh int <step>`。脚本末尾 grep 不能替代
  实际构建退出码或 JUnit；strict / dualblock / GPU 共用私有日志，必须串行。
- 设备最终证据 `C:/Users/geekerwan/.codex/tmp/p5f-redmi-exit-cfca93c7/`；库 hash 与
  六个 boot-id 已钉在设备报告。工具已入仓 `tools/device_bench/p5f/`。
- Windows → WSL 从 Windows gitdir fetch 完整 ref；WSL → Windows 用 bundle。
  不用 Linux git 直接操作含 Windows 绝对 `.git` 路径的 worktree。
- 主 Windows 树原有 `build.gradle`、`android-plugin/app/build.gradle.kts` 未提交配置，
  以及 `.trace-work/`、`Testing/`，均保留，未混入 P5f。WSL `build-pull/` 为构建产物。
  既有 DiligentCore 子模块 gitdir 噪音不在本次范围。
