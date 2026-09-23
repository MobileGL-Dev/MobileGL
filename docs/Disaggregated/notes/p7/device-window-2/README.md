# 设备窗口 #2（P7 收官）· 工具与 dry run

- 运行手册：[`tools/RUNBOOK.md`](tools/RUNBOOK.md)（一条命令 `tools/window.sh <pipe-sha>`，detached、可续跑；工作副本在 `~/w7/notes/p7/window2/`）。
- 脚本：`tools/{lib.sh,detach.sh,window.sh,10-build.sh,21-preflight.sh,20-install.sh,30-gate3.sh,40-bsl-stats.sh,50-cts-after.sh,60-reduce.py,90-restore.sh}`；
  `tools/dry-run-p7w6.sh` 是下面 dry run 的原样命令。reducer 自检：`python3 tools/60-reduce-selftest.py`（主机，秒级；
  夹具 `tools/60-reduce-selftest-pre14e1c8b9.json` = pre-14e1c8b9 CTS 五块相对 `$BASE` 的逐例差异）。
- 集成者裁定（2026-09-23）已落进脚本与 RUNBOOK §1/§3：CTS rate = Pass/(Pass+Fail)、五块缺一即 INCOMPLETE、UBO 记 unrun；
  门 3 单一 boot 会话（否则 INVALID-SESSION）、臂内逐位相同是门而跨臂同图只 WARN、monolith 臂身份核验；`.done` 只在产物齐全时写；
  restore 把 `$BASE` 库推回 `mgcts`。
- 评审后的加固（同日）：reducer 不再从「无」得出 PASS——caselist 读不到 / 为空的块记 MISSING（输出目录另存 caselist 副本，
  构建树删了也能判），AFTER 无一个 Pass/Fail 的块 FAIL，算不出 delta 的块不会 PASS；两边 rate 同口径（只算两边都有结果的用例，
  `$BASE` 缺的与 NS 变化打 WARN），旧口径的两个 rate 逐块列出；只有 36/36、reboot-clean 的门 3 与五块完整的 CTS 才退出 0
  （否则 `PASS-SUBSET` / `PASS-NOT-REBOOT-CLEAN`）；`50-cts-after.sh` 用与判读同一段代码（`60-reduce.py --check-block`）判块完整后才写 `.done`；
  窗口从 preflight 到 restore 持有本波次共享设备锁 `/home/swung/w7/locks/2f7cbe2e.lock`（`flock -o`，不把锁带给残留子进程）。
  自检 81/81（含只由 rate 定的块：dsa −0.541 pp FAIL、−1.081 pp FAIL）；cfd3dde9 的 reducer 在同一套检查上 62/81。
- `$BASE` 的机器可读 JSON 此前缺失，已由 `cts_multi_report.py --adopt-legacy` 从 `~/w7/logs/cts-a64/base/runs` 重生成：
  `tools/cts/baselines/base-DirectVulkan-monolith-p7w1-9be62cbc.{json,md}`（逐块数字与 `../device-window-1/CTS-base/README.md` 一致）。

## dry run（2026-09-23，Redmi `2f7cbe2e`，已装 p7w6 APK，只证管线）

| 目录 | 内容 |
|---|---|
| `dry-run/dry-p7w6/` | preflight（找到：supervisor 在跑 / stay-on 15 / UNPINNED）→ 身份核对（设备 `base.apk` = `45759c30…`）→ 门 3 两例（OpenRA、iris-bsl-in-world）× 四臂 × 1 → bsl inproc → CTS dsa 前 20 例 → reduce → restore；`verdict.txt` 与 `timing.tsv` |
| `dry-run/dry-p7w6-b-bsl/` | 修 supervisor 探测与采样器预停后重跑：bsl inproc + spawn 的 maps / RSS 峰值与 `wbuf[]` |
| `dry-run/dry-p7w6-c-cts-ssbo-dsa/` | CTS 计时样本：p7w6 inproc 全 ssbo + dsa |
| `dry-run/build-p7w7dry-14e1c8b9/` | `10-build.sh 14e1c8b9 p7w7dry-14e1c8b9`：APK 131 s、主机制品 223 s；**未安装** |
| `dry-run/pre-14e1c8b9-cts/` | pipe 头 lib 的 CTS 五块预读（inproc）与红例的 monolith / inproc 逐例归属（`attribution.txt`）——门 5 在该头上会红，见 RUNBOOK §6；`verdict.txt` 是旧口径读数，`verdict-contract.txt` 是现行 reducer 的合同口径（dsa −0.272 pp，红在新 crash；每块 `info` 行列旧口径的两个 rate） |

原始归档（actual PNG、角色日志、qpa）在 `~/w7/logs/devprep/w2/<stamp>/`。
