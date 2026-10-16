# P5f 交接：已全部收官

> **后续 CI / 全量 inproc 工作尚未完成，已按用户要求停止（2026-09-20）。**
> 最新接续点见 [CI / inproc 交接](HANDOFF-CI-INPROC-20260920.md)：实现树停在 `70c8230b`，尚未合并或 push；完整 inproc retrace 为 67/77，另有一个新增 GLES view mip 回归未解决。下文“已收官”仅指此前 P5f 范围，不能作为本轮 CI 全绿证明。

> 2026-09-20。交付 worktree：
> `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`，分支 `feat/disaggregated`。
> 完整结果见 [close-report.md](close-report.md)，设备见 [device-report.md](device-report.md)，
> 阶段终审见 [close-review.md](close-review.md)。原 P5f 行为验收头 `cfca93c7`；
> 后续 Magma run-ahead 的最终行为头为 `194382c9`，见下。
>
> **Magma RD32 性能补修（2026-09-20，`70fb6689`）**：修复重复view/framebuffer创建销毁和
> 临时render-pass句柄造成的pipeline缓存退化；inproc **116.73 FPS**，同包monolith **115.01 FPS**。
> 缓存回归、Vulkan同步验证及主机全门通过。见 [性能报告](magma-rd32-performance-fix.md)。
>
> **Magma run-ahead 已完成（2026-09-20）**：独立 readiness 已开启，GPU 提交/资源
> 退休时序、真实排队/credit 1/3、负控、Vulkan 同步验证均通过。Redmi 同一
> `.mgdebug.debug` APK 的 Magma RA=1 / RA=0 与 GLES RA=1 三臂均完成至少60秒
> 世界运行与验图；修复了实机发现的 VAO 身份哈希碰撞。见 [最终报告](magma-runahead.md)。
>
> **Magma inproc 游戏修复（2026-09-20）**：已补 server buffer consumers 与 Android quarter-turn
> color blit。`38919d45` 的 `.mgdebug.debug` FCL 在 MC `26.3-rc-3` 世界 `test` 上，
> Magma inproc、GLES inproc 和 Magma monolith 均完成至少60秒运行与人工验图。
> 该历史包仍 lockstep、约20 FPS；当前版本以上面的 run-ahead 报告为准。
> 不是 P7 全量完成。见 [修复报告](magma-inproc-fix.md)。
> 旧 [fcl-e2e-report.md](fcl-e2e-report.md) 保留 `aa78f102` 的真实失败，不代表当前结果。

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

- Magma 已发布 `kCapRunAheadApply`；`RUN_AHEAD=0` 保留配对对照。
  可达路径已经在 role-split + strict 下验证，没有 frontend registry/allocator 豁免。
  其余 P7/P8 shape 限制仍具名拒绝；run-ahead 不代表 P7 全量完成。
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
