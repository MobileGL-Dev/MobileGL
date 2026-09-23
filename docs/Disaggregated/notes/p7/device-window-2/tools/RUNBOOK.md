# 设备窗口 #2 运行手册（P7 收官：门 3 终局 + bsl stats + CTS AFTER）

一条命令产出三样东西：CONTRACT-P7 §7.2 的门 3 读数（36 例 × DirectVulkan × `--use-pbuffer`，
monolith ×1 + inproc ×3 + spawn ×3 + inproc `RUN_AHEAD=0` ×1，同一 reboot-clean 会话）、
M2/M3 的 bsl-esc-menu 设备读数（`MOBILEGL_PIPE_STATS` 下的 `/proc/<pid>/maps` 行数峰值与 `wbuf[]`），
以及 §7.3 门 5 的 CTS AFTER（五块 KHR-GL46，inproc × DirectVulkan，对 `device-window-1/CTS-base`）。
脚本在 `~/w7/notes/p7/window2/`（仓库副本 `docs/Disaggregated/notes/p7/device-window-2/tools/`）。

**参数**：`<pipe-sha>` = 最终功能包全部落地后的集成树提交；`<stamp>` 缺省 `p7w7-<sha8>`。
输出根 `~/w7/logs/p7w7/<stamp>/`。设备 Redmi `2f7cbe2e`，包 `top.mobilegl.plugin.p7w1.trace`（原地 `install -r`，不卸载任何东西）。

## 0. 前置（不占设备，开窗前核对）

1. `<pipe-sha>` 必须含 `p7/devprep` 的跑器修复（`fe4c8745`：`run_android_retrace_local.py` 能从 POSIX 主机起跑、
   每遍之前清掉上一遍的产物）。没拣就会在第一步被 `lib.sh` 以名字拒绝；应急可 `export W2_TOOLS_OVERRIDE=~/w7/p7-devprep`
   （只借工具，APK 仍按 `<pipe-sha>` 构建）。
2. Windows 侧 adb server 在跑并看得见 `2f7cbe2e`。WSL 的 `/usr/sbin/adb` 经 mirrored networking 连的就是它；
   server 没了在 **Windows** 上 `adb start-server`，不要在 WSL 里起（会抢 5037 而看不见 USB）。
3. 手机 `/data/local/tmp/mgcts` 有 `glcts`、`gl_cts/`、`mgprobe`（窗口 1 部署，现仍在；缺则 `sh ~/w7/logs/cts-a64/deploy/deploy.sh 2f7cbe2e`）。
4. 窗口期间没有别的 agent 用手机、主机不并行跑 gate（超时是墙钟）。手机若有锁屏密码，reboot 后要手动解锁一次
   （preflight 等 `sys.user.0.ce_available=true` 最多 10 min）。

## 1. 一条命令

```bash
bash ~/w7/notes/p7/window2/detach.sh ~/w7/logs/p7w7/window-<sha8>.log ~/w7/notes/p7/window2/window.sh <pipe-sha>
tail -f ~/w7/logs/p7w7/window-<sha8>.log        # 结束行 "=== WINDOW 2 DONE <stamp>"，其后打印 verdict
```

`detach.sh` = `setsid nohup … < /dev/null &`，与终端 / 工具 10 min 上限脱钩。**中断后原命令重跑即续**：
每步完成写 `<out>/steps/<step>.done`；门 3 逐 (arm, case) 写 `gate3/state/<arm>/<case>.done`（未完成的那一对整对重跑）；
CTS 逐块写 `cts/runs/<block>/.done`。只重跑一例：删它的 `.done` 再跑 `30-gate3.sh <stamp>`。
不重启：`window.sh <pipe-sha> --no-reboot`（门 3 要 reboot-clean，只在复跑时用）。

## 2. 步骤（等价的逐条命令）与耗时

| # | 命令 | 做什么 | 估时 |
|---|---|---|---|
| 1 | `10-build.sh <sha> [<stamp>]` | `~/w7/p7w7-tree-<sha8>`（pipe 的 detached worktree，子模块从 pipe 拷，LFS fixture 随 checkout）里构建 APK（`assembleTraceRelease`，`.p7w1` 并排 id，debuggable，显式 stamp，debug keystore 签名）与主机制品（`build-host`：`libMobileGL.so`/`libMobileGLServer.so`/`mobilegl_trace_replay` + catalog），两端 stamp 字符串各 ≥1 | 6 min（dry：APK 131 s、host 223 s） |
| 2 | `21-preflight.sh <stamp> --reboot` | 记录「找到时」的状态（supervisor、stay-on、钉频、Doze 白名单）→ reboot，前后 boot_id 必须不同 → 唤醒、`svc power stayon true` → `pin_device.sh pin`，`check` 为 `PINNED` 或 `PINNED-1100`（本板 pwrlevel 0 实跑 1100 MHz，窗口 1 已记，脚本期望值过期） | 2–3 min |
| 3 | `20-install.sh <stamp>` | `adb install -r`；**设备上 `base.apk` 的 sha256 必须等于签名 APK**，versionName 带 `<sha7>`；等 dex2oat 空闲 | ~1 min |
| 4 | `30-gate3.sh <stamp>` | 36 例（`--matrix` 的 DirectVulkan 集减 `create-indirect` / `1.21.11-main-menu` / `photon-v1.3b`，冻结在 `gate3/cases.txt`）× 四臂；iterationrp 带 `apk.yml:460-462` 三个 Magma 旋钮；每遍 result.json / actual PNG / 双角色日志 / transport-proof / logcat 归档；每臂前后 pin check | 55–65 min |
| 5 | `40-bsl-stats.sh <stamp>` | bsl-esc-menu × {inproc, spawn} 各 1 遍，`MOBILEGL_PIPE_STATS=1 PERIOD=1`；先 force-stop 再起设备端 root 采样器（0.25 s：maps 行数、VmRSS、VmHWM；排除 argv 带 `tcp://` 的 supervisor server）；`wbuf[]` 从归档日志取 | ~1 min |
| 6 | `50-cts-after.sh <stamp>` | APK 的 arm64 `libMobileGL.so` 推到 `mgcts` 并在设备上核 sha256；AFTER 环境 = `$BASE` 的 flag + `MOBILEGL_TRANSPORT=inproc` + `ROLE_SPLIT_STATE=1 STRICT_ERRORS=1 RUN_AHEAD=1`；臂证明（mgprobe PASS 且 logcat 有 inproc 解析句与 `Config: IPC … strict=1 role-split-state=1 run-ahead=1`、0 Fatal；再用一个 glcts 用例证一次）；五块依次跑（顺序同 `$BASE`），每块 `qpa_report --json`，最后 `cts_multi_report` 出与 `$BASE` 同形的 JSON | 6–7 min（pipe 头 lib inproc 实测 358 s；`$BASE` monolith 372 s） |
| 7 | `60-reduce.py <stamp>` | 写 `verdict.txt` / `verdict.json`（见 §3） | <10 s |
| 8 | `90-restore.sh <stamp>` | Home；`am force-stop` 包（回放后缓存的 app 进程与 spawn 回放留下的 `libMobileGLServer.so @mgl-…` 子进程会一直占着几百 MB）；supervisor 若开窗时在跑则以 `tcp_device_server.py start --allow-idle`（同 listen / token / Doze 状态文件）在**新 APK**上重启并核实在听；解钉频（MIUI 启动 boost 会让 `check` 短暂读成 DRIFT，重读至多 20 s）；stay-on 复原 | ~15 s |

合计：构建 ~6 min + 设备 ~65–80 min ≈ **1.3–1.5 h**。外推依据见 §5。

## 3. 判读 `verdict.txt`

- **门 3**，逐例 `PASS / FAIL / INCOMPLETE` + 原因：monolith 本身通过；inproc 与 spawn 各 3 遍齐全、每遍臂证明通过、
  每遍 ssim ≥ 阈值、**三遍 actual PNG SHA-256 相同**、每遍 `|ssim − monolith| ≤ 0.0005`；OpenRA 在 monolith / inproc / spawn
  每遍都 `1.000000` 且 mismatch 0；一遍的 logcat 与上一遍逐字节相同记 `STALE`。`RUN_AHEAD=0` 臂只记录
  （ssim、SHA、`Config: IPC` 全是 `run-ahead=0` 的证明）；它的 runner rc=1 是 `--require-inproc` 要求 `run-ahead=1` 的已知副作用（E1）。
  末行 `GATE3 PASS: 36/36` 才是门 3；列 `i==s==m` 表示三臂同图（信息，不设门）。
- **BSL**：每臂每进程（app / server）的 maps 行数峰值、VmRSS / VmHWM 峰值、`wbuf[wbufs wlivepk wdefpk wdefsync]`。
- **CTS**：每块 BASE 与 AFTER 的 `P/NS/F/X` 与 rate = Pass / (结果数 − NotSupported)（Fail、Crash、warning 全在分母里），
  delta pp；块判据 delta ≥ −0.5 pp、**新增 crash = 0**（AFTER 为 Crash/Timeout/InternalError/ResourceError/DeviceHang/Incomplete 而 BASE 不是，另加 hung.txt）、
  `unrun.txt` 为空。另列 Pass→非 Pass 与非 Pass→Pass 的逐例名单；AFTER 高出 0.5 pp 以上只标注不判红。
- `reduce` 的退出码只作记录：红的门也要走完 restore。

## 4. 入库清单 → `docs/Disaggregated/notes/p7/device-window-2/`

`verdict.txt`、`verdict.json`、`timing.tsv`；`session/`（`found.env`、`reboot-clean.txt`、`boot-id*.txt`、`apk.sha256`、`apk-on-device.sha256`、
`apk-install.txt`、`pin-*.txt`、`device.txt`、`restored.txt`）；`gate3/{cases.txt,excluded.txt,arms.txt,progress.tsv,pin-*.txt}`；
`bsl/<arm>/{peaks.txt,wbuf.txt,wbuf-gauges.txt}`；`cts/{IDENTITY 即 deploy/IDENTITY.txt,arm-proof.txt,preflight.txt,report-*.txt,report-*.json,after-*.json,after-*.md,pin-*.txt}`、
`cts/runs/*/{crashed,hung,unrun,skipped}.txt`。归档图像与日志（`gate3/archive/`、`bsl/*/archive/`、qpa）留在 `~/w7/logs/p7w7/<stamp>/`，
在 README 里记绝对路径（>2 MiB 不入库）。主机制品 `<out>/host/` 与 `host.sha256` 供之后的 TCP 对照（`run_tcp_matrix.py --library/--runner`，同 stamp）。

## 5. Dry run（2026-09-23，已装 p7w6 APK `45759c30…`，只证管线，不是判据）

命令：`dry-run-p7w6.sh`（`W2_APK_OVERRIDE=~/w7/logs/p7w6/trace-p7w6.apk`，`W2_TOOLS_OVERRIDE=~/w7/p7-devprep`，不重启，钉频）。

| 步 | 结果 | 墙钟 |
|---|---|---|
| preflight（无 reboot） | found：supervisor 在跑、stay-on 15、UNPINNED；pin → PINNED | 5 s |
| install `--verify-only` | 设备 `base.apk` sha256 = `45759c30…`，versionName `26.09.0e16ca2-trace` | <1 s |
| 门 3：OpenRA + iris-bsl-in-world × 四臂 × 1 | 8 次回放全部归档；OpenRA 四臂 1.000000 / 0；bsl-in-world 四臂 0.997010 同 SHA；spawn 证明 `server role in pid …`；ra0 两例 `run-ahead=0`，rc=1（预期） | 63 s（7–8 s/例/臂） |
| bsl stats inproc（+ 第二轮 spawn） | 两臂 0.999791667、SHA `155dcdd1…`（= W6/E0a）；p7w6 读数：inproc app maps 峰 8536 行 / VmHWM 823 MB；spawn server 7401 行 / 343 MB，client 4595 行 / 555 MB；`wbuf[wbufs=28 wlivepk=1052 wdefpk=3312832 wdefsync=24]` | 14 s/臂 |
| CTS AFTER dsa 前 20 例 | 部署 p7w6 lib `12861aa1…` 并核 sha；mgprobe PASS；mgprobe 与 glcts 的臂证明 PROVEN；BASE 19/0/0/1 vs AFTER 19/0/0/1，+0.000 pp，0 新 crash；之后 `$BASE` lib `7f58aa0b…` 推回 | 1–2 s/块 |
| restore | Home、supervisor 在新起的 `tcp://0.0.0.0:40613` 在听、UNPINNED、stay-on 15、Doze 白名单同开窗 | 5 s |
| 构建脚本 @ `14e1c8b9`，stamp `p7w7dry-14e1c8b9`（**未安装**） | APK `f2ca6b02…`（id `.p7w1`、versionName `26.09.14e1c8b-trace`、同 debug 证书），lib stamp 串 1；主机制品 stamp 串 1 | 358 s |
| CTS 计时样本：p7w6 inproc 全 ssbo + dsa | ssbo 124/124；dsa 366/0/3/2，−0.539 pp，1 新 crash（见 §6） | 3 s + 9 s |
| CTS 预读：pipe 头 `14e1c8b9` 的 arm64 lib（取自上行未安装的 APK，只推到 `mgcts`）五块全跑 | 见 §6：dsa 与 shader-image 各 1 个新 crash；之后 `$BASE` lib 推回并核 sha | 358 s |

**外推**：窗口 1 同机同口径的逐例间隔（E0a monolith 39 例 360 s、E0a inproc 342 s、W4 inproc 412 s；最慢 derivative 30 s、
bsl-esc-menu 15–30 s）与本次 dry run 的 7–8 s/例一致，36 例一遍 ≈ 5.5–7 min；8 遍 ≈ 45–55 min，另加 144 次 runner 调用的
启动 / 汇总页与每臂 pin check ≈ 5 min → 门 3 55–65 min。CTS 五块 inproc 实测 358 s（shader-image 9 / ssbo 3 / dsa 9 / texture 108 / packed-pixels 220 s）。

## 6. 已知事实与限制

- **门 5 在 pipe 头 `14e1c8b9` 上会红（预读，非判据，逐例归属在 `docs/Disaggregated/notes/p7/device-window-2/dry-run/pre-14e1c8b9-cts/attribution.txt`）**：同一 lib 的 monolith 与
  inproc 逐例对照（inproc 带或不带三个 split 旋钮结果相同）：
  - `KHR-GL46.direct_state_access.renderbuffers_storage`：monolith Pass，inproc **SIGABRT** `Fatal{ReplyTooLarge, "ReadPixels 256x512 0x1908/0x1406 2097152 > 2097136"}`
    ——RGBA/FLOAT 回读恰 2 MiB，比一个 reply 槽的载荷上限（槽 − 16 B 头，`MG_Remote/Transport/ReplySlot.h:166-169`）多 16 B；客户端在发射前具名拒绝
    （`MG_Remote/Client/ClientSession.cpp:2319`，ID-47：「chunked readback 是 P6 债」）。
  - `…renderbuffers_storage_multisample`：monolith Pass，inproc Fail。两例合计 dsa −0.539 pp（门限 0.5）+ 1 新 crash。
  - `KHR-GL46.shader_image_load_store.incomplete_textures`：monolith Fail（`$BASE` 亦非 Pass），inproc **SIGABRT** `Fatal{UnmigratedVerb, "Magma:image-view-window"}`
    （`MG_Backend/DirectVulkan/Renderer/UniformManager.cpp:151`）——Fail 变 Crash，按门 5 是新 crash。
  - `…shader_image_load_store.advanced-sync-vertexArray`：`$BASE` Pass，pipe 头 monolith 与 inproc 都 Fail——不是分离缺陷，是 p7w1 之后两臂共有的漂移。
  - `KHR-GL46.texture_border_clamp.Texture2DArrayCompressed`：整块里 Fail 一次，单跑三臂都 Pass——抖动。
  - 其余：ssbo、packed-pixels 与 `$BASE` 相同；texture +0.96 pp；shader-image 整体 +2.9 pp（3 例修好）。
  终局窗口之前要么修（ReadPixels 分块或放大 reply 槽；`image-view-window` 的 incomplete-texture 形状 decline 成 GL 语义而不是 Fatal），要么由集成者裁定。
- dry run 暴露并已修的两处：`run_android_retrace_local.py` 只会用 Git Bash 起跑（WSL 下每例 FileNotFoundError），且一遍没产出时会归档上一遍的图（假「逐位相同」）——`fe4c8745`，带 red-once 单测；
  脚本自身的 `pipefail` + `grep -q` 把在跑的 supervisor 读成「不在」——已改为先取输出再匹配。
- `$BASE` 的机器可读 JSON（CTS-base/README 所称 `base-DirectVulkan-monolith-p7w1-9be62cbc.json`）此前不在任何树里；已按 `cts_multi_report.py --adopt-legacy` 从
  `~/w7/logs/cts-a64/base/runs` 重生成入 `tools/cts/baselines/`（数字与 README 表逐块一致；texture 因 skip 表 1 例记 INCOMPLETE 是该工具的口径）。
  判据本身用的是入库的 `CTS-base/report-*.json` 逐例结果。
- `$BASE` 库的 IDENTITY 写 stamp `p7w1-d260f110`，README / JSON 名写 `9be62cbc`：两者是同一补丁（`d260f110` 与 `9be62cbc` 同题提交），只是命名差。
- 每次回放的 `am force-stop` 会杀 TCP supervisor；restore 负责按开窗状态复原。
