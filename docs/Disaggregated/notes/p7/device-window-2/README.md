# 设备窗口 #2（P7 收官）· 工具与 dry run

- 运行手册：[`tools/RUNBOOK.md`](tools/RUNBOOK.md)（一条命令 `tools/window.sh <pipe-sha>`，detached、可续跑；工作副本在 `~/w7/notes/p7/window2/`）。
- 脚本：`tools/{lib.sh,detach.sh,window.sh,10-build.sh,21-preflight.sh,20-install.sh,30-gate3.sh,40-bsl-stats.sh,50-cts-after.sh,60-reduce.py,90-restore.sh}`；
  `tools/dry-run-p7w6.sh` 是下面 dry run 的原样命令。
- `$BASE` 的机器可读 JSON 此前缺失，已由 `cts_multi_report.py --adopt-legacy` 从 `~/w7/logs/cts-a64/base/runs` 重生成：
  `tools/cts/baselines/base-DirectVulkan-monolith-p7w1-9be62cbc.{json,md}`（逐块数字与 `../device-window-1/CTS-base/README.md` 一致）。

## dry run（2026-09-23，Redmi `2f7cbe2e`，已装 p7w6 APK，只证管线）

| 目录 | 内容 |
|---|---|
| `dry-run/dry-p7w6/` | preflight（找到：supervisor 在跑 / stay-on 15 / UNPINNED）→ 身份核对（设备 `base.apk` = `45759c30…`）→ 门 3 两例（OpenRA、iris-bsl-in-world）× 四臂 × 1 → bsl inproc → CTS dsa 前 20 例 → reduce → restore；`verdict.txt` 与 `timing.tsv` |
| `dry-run/dry-p7w6-b-bsl/` | 修 supervisor 探测与采样器预停后重跑：bsl inproc + spawn 的 maps / RSS 峰值与 `wbuf[]` |
| `dry-run/dry-p7w6-c-cts-ssbo-dsa/` | CTS 计时样本：p7w6 inproc 全 ssbo + dsa |
| `dry-run/build-p7w7dry-14e1c8b9/` | `10-build.sh 14e1c8b9 p7w7dry-14e1c8b9`：APK 131 s、主机制品 223 s；**未安装** |
| `dry-run/pre-14e1c8b9-cts/` | pipe 头 lib 的 CTS 五块预读（inproc）与红例的 monolith / inproc 逐例归属（`attribution.txt`）——门 5 在该头上会红，见 RUNBOOK §6 |

原始归档（actual PNG、角色日志、qpa）在 `~/w7/logs/devprep/w2/<stamp>/`。
