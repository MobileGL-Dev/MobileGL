# 设备窗口 #2 运行手册（P7 收官：门 3 终局 + bsl stats + CTS AFTER）

一条命令产出三样东西：CONTRACT-P7 §7.2 的门 3 读数（36 例 × DirectVulkan × `--use-pbuffer`，
monolith ×3（ID-P7-62）+ inproc ×3 + spawn ×3 + inproc `RUN_AHEAD=0` ×1，同一 reboot-clean 会话）、
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
4. 窗口期间没有别的 agent 用手机、主机不并行跑 gate（超时是墙钟）。「没有别的 agent」由锁保证：本波次所有 agent 都以
   `flock -w 3600 /home/swung/w7/locks/2f7cbe2e.lock <cmd>` 用手机；`window.sh` 在 build 之后、preflight 之前拿**同一把锁**
   （`lib.sh` 的 `W2_SHARED_LOCK`），一直持有到 restore 结束——两步之间别的 agent 插不进来；单独起的某一步自己拿、自己放。
   拿锁是 `flock -o`：脚本以同样的参数在 flock 进程下重跑，锁 fd 不传给子进程（步骤留下的 adb server / supervisor 启动器不会把锁
   带出窗口）。等锁最多 `W2_SHARED_LOCK_WAIT`（默认 3600 s），等不到退出 75、不碰手机。窗口自己的步骤锁 `W2_LOCK` 照旧。
   手机若有锁屏密码，reboot 后要手动解锁一次（preflight 等 `sys.user.0.ce_available=true` 最多 10 min）。

## 1. 一条命令

```bash
bash ~/w7/notes/p7/window2/detach.sh ~/w7/logs/p7w7/window-<sha8>.log ~/w7/notes/p7/window2/window.sh <pipe-sha>
tail -f ~/w7/logs/p7w7/window-<sha8>.log        # 结束行 "=== WINDOW 2 DONE <stamp>"，其后打印 verdict
```

`detach.sh` = `setsid nohup … < /dev/null &`，与终端 / 工具 10 min 上限脱钩；它打印的 pid 是新会话的进程组长，
要停整个窗口用 `kill -- -<pid>`（只杀 pid 本身会留下正在跑的那一步，且放掉共享锁）。**中断后原命令重跑即续**：
每步完成写 `<out>/steps/<step>.done`；门 3 逐 (arm, case) 写 `gate3/state/<arm>/<case>.done`——**只在该对要求的每一遍都留下
`result.json` 与 actual PNG 时才写**，否则日志记 `INCOMPLETE`、`gate3/progress.tsv` 第 6 列 `INCOMPLETE`、不写 `.done`，下次整对重跑
（残档先删；遍数按首跑冻结在 `gate3/arms.txt` 的 `repeat=` 与 `monolith_repeat=`，后者缺失 = ID-P7-62 之前的脚本开的跑、续跑仍 monolith ×1）；CTS 逐块写 `cts/runs/<block>/.done`——**只在该块是完整读数时才写**：
`run_cts.py` 退出 0，且 `60-reduce.py --check-block`（与判读**同一段代码**）确认 `cts/report-<block>.json` 对 caselist 里每个未 skip 的用例
都有结果、`unrun.txt` 为空；否则不写，原因进 `cts/runs/<block>/complete.txt`。每块另存所跑 caselist 的副本 `cts/runs/<block>/caselist.txt`
（`caselist.path` 指向的构建树删掉之后，输出目录仍能单独判读）。循环跑完仍有 INCOMPLETE 时 `30-gate3.sh` / `50-cts-after.sh` 退出 3：`window.sh` 不给该步写 `.done`、
照走后面各步（reduce 与 restore 每次都跑），结尾打 `WINDOW 2 INCOMPLETE … left open`；原命令再跑一次只补缺的——若上一遍已 restore，
它先以 `21-preflight.sh <stamp>`（不重启）回到**同一** boot 会话（重新钉频、stay-on；boot_id 变了就停；`reboot-clean.txt` 不改写，
`session/resumed.txt` 记一笔）。

**会话**（CONTRACT-P7 §7.2「同一 reboot-clean 会话」）：`session/boot-id.txt` 是本窗口唯一的会话。`30-gate3.sh` 每对前后读手机
boot_id，与它不同立即停；每对的 `.done` 与每遍归档的 `repeat-NN/boot_id.txt` 都记 `boot_id=… boot_id_before=…`；`60-reduce.py`
要求四臂所有记录是同一个 boot_id 且等于 `session/boot-id.txt`，否则 `GATE3 INVALID-SESSION`。

**中断后要删什么**（先比 `adb -s 2f7cbe2e shell cat /proc/sys/kernel/random/boot_id` 与 `<out>/session/boot-id.txt`）：
- **boot_id 没变**（主机 / WSL / adb 断、脚本被杀、某步 INCOMPLETE）：**什么都不删**，原命令重跑。没 `.done` 的对 / 块整对 / 整块重跑，
  残档由脚本先删。只想重跑某个已完成的对：删 `gate3/state/<arm>/<case>.done` 与 `steps/gate3.done`（CTS 块同理：
  `cts/runs/<block>/.done` 与 `steps/cts.done`），再原命令重跑。
- **boot_id 变了（手机重启过）**：门 3 已完成的对全部作废（`30-gate3.sh` 在下一对之前就停；`21-preflight.sh` 不带 `--reboot`
  也停；带 `--reboot` 时只要 `gate3/state` 里还有 `.done` 就拒绝，防漏删）。删 `gate3/state/`、`gate3/archive/`、`gate3/logs/`、
  `gate3/progress.tsv`、`gate3/pin-*.txt` 与 `steps/preflight.done`、`steps/gate3.done`，然后原命令重跑（preflight 带 `--reboot`
  起新会话；`session/found.env`、`gate3/cases.txt`、`gate3/arms.txt` 保留——「找到时」状态、分母与遍数不变）。bsl 与 CTS 不受会话约束，保留。
- 或者换 stamp 从头开（`window.sh <pipe-sha> <new-stamp>`），旧目录留作记录。

不重启：`window.sh <pipe-sha> --no-reboot`（门 3 要 reboot-clean，只在复跑时用）。

## 2. 步骤（等价的逐条命令）与耗时

| # | 命令 | 做什么 | 估时 |
|---|---|---|---|
| 1 | `10-build.sh <sha> [<stamp>]` | `~/w7/p7w7-tree-<sha8>`（pipe 的 detached worktree，子模块从 pipe 拷，LFS fixture 随 checkout）里构建 APK（`assembleTraceRelease`，`.p7w1` 并排 id，debuggable，显式 stamp，debug keystore 签名）与主机制品（`build-host`：`libMobileGL.so`/`libMobileGLServer.so`/`mobilegl_trace_replay` + catalog），两端 stamp 字符串各 ≥1 | 6 min（dry：APK 131 s、host 223 s） |
| 2 | `21-preflight.sh <stamp> --reboot` | 记录「找到时」的状态（supervisor、stay-on、钉频、Doze 白名单）→ reboot，前后 boot_id 必须不同 → 唤醒、`svc power stayon true` → `pin_device.sh pin`，`check` 为 `PINNED` 或 `PINNED-1100`（本板 pwrlevel 0 实跑 1100 MHz，窗口 1 已记，脚本期望值过期） | 2–3 min |
| 3 | `20-install.sh <stamp>` | `adb install -r`；**设备上 `base.apk` 的 sha256 必须等于签名 APK**，versionName 带 `<sha7>`；等 dex2oat 空闲 | ~1 min |
| 4 | `30-gate3.sh <stamp>` | 36 例（`--matrix` 的 DirectVulkan 集减 `create-indirect` / `1.21.11-main-menu` / `photon-v1.3b`，冻结在 `gate3/cases.txt`）× 四臂（monolith 与 inproc / spawn 同为 ×`--repeat`，ID-P7-62）；iterationrp 带 `apk.yml:460-462` 三个 Magma 旋钮；每遍 result.json / actual PNG / 双角色日志 / transport-proof / logcat 归档；每臂前后 pin check；每对前后核 boot_id（§1「会话」），`.done` 只在每遍都有 result.json + actual PNG 时写，否则退出 3 留待续跑。主机自检（stub adb + stub 跑器，不占设备）：`bash 30-gate3-selftest.sh` | 65–80 min |
| 5 | `40-bsl-stats.sh <stamp>` | bsl-esc-menu × {inproc, spawn} 各 1 遍，`MOBILEGL_PIPE_STATS=1 PERIOD=1`；先 force-stop 再起设备端 root 采样器（0.25 s：maps 行数、VmRSS、VmHWM；排除 argv 带 `tcp://` 的 supervisor server）；`wbuf[]` 从归档日志取 | ~1 min |
| 6 | `50-cts-after.sh <stamp>` | APK 的 arm64 `libMobileGL.so` 推到 `mgcts` 并在设备上核 sha256；AFTER 环境 = `$BASE` 的 flag + `MOBILEGL_TRANSPORT=inproc` + `ROLE_SPLIT_STATE=1 STRICT_ERRORS=1 RUN_AHEAD=1`；臂证明（mgprobe PASS 且 logcat 有 inproc 解析句与 `Config: IPC … strict=1 role-split-state=1 run-ahead=1`、0 Fatal；再用一个 glcts 用例证一次，其 StatusCode 记进 `arm-proof.txt`，不是 Pass / Fail 时打 WARN）；五块依次跑（顺序同 `$BASE`），每块 `qpa_report --json`（块的 `.done` 只在 `run_cts` 退出 0 且 `60-reduce.py --check-block` 判完整时写，否则退出 3 留待续跑），最后 `cts_multi_report` 出与 `$BASE` 同形的 JSON；UBO（GTF）块不跑：本 glcts 无 GTF 模块（ID-P7-16） | 6–7 min（pipe 头 lib inproc 实测 358 s；`$BASE` monolith 372 s） |
| 7 | `60-reduce.py <stamp>` | 写 `verdict.txt` / `verdict.json`（见 §3）；每遍都跑。自检：`python3 60-reduce-selftest.py`（主机，不占设备，~7 s） | ~2–3 min（门 3 归档每遍一次纯 Python 的 SSIM-vs-first：`p7w7-6afa077f` 的 monolith+inproc+spawn 归档只读实测 108 s，本包的 reducer 在主机有负载时 130 s；monolith ×3 多 72 遍） |
| 8 | `90-restore.sh <stamp>` | Home；`am force-stop` 包（回放后缓存的 app 进程与 spawn 回放留下的 `libMobileGLServer.so @mgl-…` 子进程会一直占着几百 MB）；**把 `$BASE` 库推回 `/data/local/tmp/mgcts`**（设备 sha256 ≠ `$BASE` = `IDENTITY.txt` 的 `7f58aa0b…` 时才推，推后在设备上核 sha，结果行进 `restored.txt`）；supervisor 若开窗时在跑则以 `tcp_device_server.py start --allow-idle`（同 listen / token / Doze 状态文件）在**新 APK**上重启并核实在听；解钉频（MIUI 启动 boost 会让 `check` 短暂读成 DRIFT，重读至多 20 s）；stay-on 复原；每遍都跑（幂等） | ~15 s |

合计：构建 ~6 min + 设备 ~77–95 min ≈ **1.5–1.7 h**。外推依据见 §5。

## 3. 判读 `verdict.txt`

- **门 3**，先看 `-- session` 行：`session/boot-id.txt` 与四臂的每条记录（`gate3/state/<arm>/<case>.done` 的 `boot_id` / `boot_id_before`、
  每遍 `repeat-NN/boot_id.txt`）必须是**同一个** boot_id；有第二个值、有记录不带 boot_id（含本规则之前的旧 `.done`）、或缺 `session/boot-id.txt`，
  整门判 **`GATE3 INVALID-SESSION`**，与图无关。
- **门 3 按合同常量判，不按跑器自己的记录判（ruling: integrator 2026-09-23 after codex closeout review）**：`gate3/arms.txt` 只说明
  `30-gate3.sh` 被要求跑什么，不说明门要什么；reducer 自己按 CONTRACT-P7 §7.1 / §7.2 核三件事（`-- contract` 行，违反的逐条 `! FAIL-…`）：
  ① **遍数**：inproc 与 spawn 每例归档 ≥ 3 遍。`arms.txt` 的 `repeat=` < 3，或已完成（有 `.done`）的对归档不足 3 遍 → **`GATE3 FAIL-REPEATS`**，
  不管 `arms.txt` 写的是几（`--repeat 1` 跑出来的永远不是门）；没有 `.done` 的对不足 3 遍仍只是该例 INCOMPLETE（续跑整对重来）。
  ② **四臂**：monolith / inproc / spawn / inproc-ra0 每例都要有。某臂没有归档、且 `arms.txt` 的 `arms=` 也没列它（开跑时就没带）→ **`GATE3 FAIL-ARMS`**；
  列了但还没跑到、或某例缺该臂 → 该例 INCOMPLETE。ra0 那一遍的 `mobilegl.log` 必须证明 `run-ahead=0`，否则该例 FAIL（那不是对照臂）；ra0 的图与 ssim 仍只记录、不判。
  ③ **用例集**：`gate3/cases.txt` 必须**等于**规范分母，不是「数到 36」——按 `30-gate3.sh` 的算法从工具树 `trace_cases.json` 取 CI split 且
  `ci_backends` 含 DirectVulkan 的 39 例，减去 §7.1 的三个排除（reducer 的常量 `CONTRACT_EXCLUDED`，不读本次的 `gate3/excluded.txt`，两者不同只打 WARN）。
  有集外用例、有重复行、或清单已给不出 39 / 36 → **`GATE3 FAIL-CASESET`**；是规范集的真子集（`--cases` 冒烟）→ 至多 `PASS-SUBSET`。
  优先级：`INVALID-SESSION` > `FAIL-REPEATS` > `FAIL-ARMS` > `FAIL-CASESET` > 逐例 `FAIL` > `INCOMPLETE`。
- 全部通过但不是门本身的读数时不记 `PASS`：用例是规范 36 例的真子集记 **`PASS-SUBSET`**，
  会话不是 reboot-clean（`session/reboot-clean.txt`）记 **`PASS-NOT-REBOOT-CLEAN`**，跨臂不同而没有裁定记 **`PASS-NEEDS-ADJUDICATION`**（见下），
  用了 `--extra-monolith` 的重判记 **`PASS-WITH-EXTRA-READINGS`**（见下），末行注明「不是判据」，退出码非 0。
  逐例 `PASS / FAIL / INCOMPLETE` + 原因：monolith 本身通过且**确是 monolith**（该遍 `mobilegl.log`（及 client / server 日志）没有
  `Config: IPC` 也没有 `MOBILEGL_TRANSPORT=` 行；没有日志即「未证」）；inproc 与 spawn 各 3 遍齐全、该对有 `.done`、每遍臂证明
  （`transport-proof.json` passed）通过、每遍 ssim ≥ 阈值、**同一臂内三遍 actual PNG SHA-256 相同**、每遍 `|ssim − monolith| ≤ 0.0005`；
  OpenRA 在 monolith / inproc / spawn 每遍都 `1.000000` 且 mismatch 0；一遍的 logcat 与上一遍逐字节相同记 `STALE`。`-- arm identity`
  行汇总 monolith 身份与 split 臂 transport-proof 的通过数。`RUN_AHEAD=0` 臂每例必须有（见上 ②），且 `Config: IPC` 全是 `run-ahead=0`；
  它的 ssim、SHA 只记录、不判；它的 runner rc=1 是 `--require-inproc` 要求 `run-ahead=1` 的已知副作用（E1）。
  末行 `GATE3 PASS: 36/36` 才是门 3。
- **monolith 自身不可逐位复现的例（ruling: integrator 2026-09-23，ID-P7-62，`~/w7/logs/g3det/VERDICT.md` §2）**：「三遍逐位相同」预设参照可逐位复现。
  同一会话 monolith ≥ 3 遍而图不全相同的例，split 臂的逐位相同条款换成**分布判据**：(1) split 每遍对本会话**每个** monolith 读数 `|ssim − m| ≤ 0.0005`；
  (2) 全部 split（inproc + spawn 各遍）× monolith 图对的最大差异像素数（RGBA 任一通道不同）与最大逐通道差，各 ≤ monolith 两两之间最大值的 **1.25 倍**
  （从归档 actual PNG 算，numpy + PIL；没有则用 compare_actuals 的标准库解码，同数，`W2_REDUCE_PNG_DECODER=pure` 可强制）。
  该例打 `nondeterministic monolith (N distinct / M passes) -> distributional check` 与两组最大值、界、`max |ssim - monolith|`，跨臂不同由 (2) 判、
  不需 `gate3/adjudication.tsv` 行；`-- monolith determinism` 行汇总三类例数。monolith 逐位相同、或同会话不足 3 遍（ID-P7-62 之前 ×1 的归档）的例仍按逐位相同判，
  后者 split 不逐位相同时原因注明「需 ≥ 3 遍」。monolith 每遍都要本身通过且确是 monolith；已完成的 monolith 对少于 `arms.txt` 的 `monolith_repeat=` → 该例 FAIL。
  **`--extra-monolith DIR[@BOOT_ID]`**（可重复）把别处归档的同会话 monolith 遍（`run_android_retrace_local.py --archive-dir` 形状，如 g3det 的 `E1-mono`）加进读数：
  每遍要有本会话 boot_id（该遍的 `boot_id.txt`，否则 `@BOOT_ID` 由操作者按那次跑自己的 boot_id 记录作证），没有或不同 → `INVALID-SESSION`；
  用了它的读数至多 **`PASS-WITH-EXTRA-READINGS`**——裁定用的重判，不是判据。
- **跨臂同图不是门（ruling: integrator 2026-09-23）**：§7.2 的「三遍逐位相同」是**臂内**判据——inproc 自己三遍、spawn 自己三遍各自逐位相同。
  inproc / spawn / monolith 三臂之间 actual PNG 是否相同只**报告**：逐例列 `i==s==m`（`yes` / `i==s` / `no`），`-- cross-arm` 行给同图例数，
  不同的例各打一行 `WARN cross-arm <case>: monolith <sha> | inproc <sha> | spawn <sha>`；WARN 不改变该例的判定
  （跨臂的逐例门是每遍 `|ssim − monolith| ≤ 0.0005`）。
  **但不静默放行（ruling: integrator 2026-09-23 after codex closeout review）**：每个跨臂不同的例都要在 `gate3/adjudication.tsv`
  （每行 `<case><TAB><理由>`，`#` 为注释；没有理由的行不算，打 WARN）里有一行，否则本来 PASS 的门记 **`GATE3 PASS-NEEDS-ADJUDICATION`**（退出 1）。
  WARN 行下给出该例各臂归档的 `mismatchPixels`（对 golden），以及三臂首遍之间直接数出的像素差（`inproc-monolith` / `spawn-monolith` / `inproc-spawn`，
  compare_actuals 的 `mismatch_pixels`），再注 `adjudicated: <理由>` 或 `UNADJUDICATED`；文件里列了但各臂同图的例记 note（陈旧行）。
  裁定只针对跨臂同图，不豁免任何逐例判据；裁定文件属于该 stamp 的那批图，重跑某对后要重新看图再写。
- **BSL**：每臂每进程（app / server）的 maps 行数峰值、VmRSS / VmHWM 峰值、`wbuf[wbufs wlivepk wdefpk wdefsync]`。
- **CTS**：五块（shader-image / ssbo / dsa / texture / packed-pixels）**缺一不可**：每块要有 `cts/runs/<block>/.done`、非空且有结果的
  `cts/report-<block>.json`，以及读得到、非空的 caselist（先用输出目录里的副本 `runs/<block>/caselist.txt`，没有才用 `caselist.path`
  所指的文件；指向的文件没了、是空的、或两者都没有，**不**拿报告自己的用例顶替），否则该块记 `MISSING`、整体 `CTS INCOMPLETE`、退出码 ≠ 0——
  0 例的块永远不是读数。UBO 块（`GTF-GL46.gtf31.GL3Tests.uniform_buffer_object*`，
  `p7-ubo-gl46.txt` 90 例）显式记 `unrun (not in this glcts)`（设备 glcts 无 GTF 模块，ID-P7-16），不入判据。
  每块 BASE 与 AFTER 的 `P/F/NS/W/X`（W = CompatibilityWarning / QualityWarning 等，X = Crash / Timeout / InternalError / ResourceError / DeviceHang / Incomplete），
  **rate = Pass / (Pass + Fail + L)**，L = `$BASE` 为 Pass、AFTER 变成 NotSupported 或其他任何非 Pass 非 Fail 状态（warning、crash）的用例：
  丢掉的受支持用例按**非 Pass** 计，不从分母里消失（**ruling: integrator 2026-09-23 after codex closeout review**——按 §7.3 字面的
  Pass / (Pass + Fail)，ssbo 124 个 `$BASE` Pass 里 123 个变 NS、1 个仍 Pass，读数仍是 100%）；其余 NS、warning、crash 不在分母里。
  表里 `L` 列是其个数；§7.3 字面的 Pass / (Pass + Fail) 照印在 `(literal)` 列与每块的 `info  literal` 行，只供对照、**不判**。
  L 例逐条列 `LOST <case>  (Pass -> <AFTER 状态>)`，块里有任何一条没在 `cts/adjudication.tsv`（每行 `<case><TAB><理由>`；没有理由的行不算，打 WARN）
  裁定 → 该块 **FAIL**；**已裁定的 L 例印作 `ADJUDICATED <case>  (Pass -> <状态>)  out of the rate; adjudicated: <理由>`，并从该块两边的 rate 里拿掉**
  （不再算 AFTER 分母里的 L，`$BASE` 少一个 Pass；表里 `A` 列是其个数；`(literal)` 与旧口径两行 info 仍把它算在内。**ruling: integrator 2026-09-23 after the int3 critic**——
  否则 ssbo 1 例合理的 Pass→NS 裁定后仍是 −0.806 pp，永远红）。块判据：delta ≥ −0.5 pp、
  未裁定的 L 例 = 0、**新增 crash = 0 单独硬红且不可裁定**（AFTER 为 X 而 BASE 不是——Fail→Crash 也算——另加 hung.txt；Pass→Crash 既是新 crash 也是 L 例）、`unrun.txt` 为空。
  两边的 rate 只在**两边都有结果的用例**上算（同口径）；`$BASE` 没有的用例打 `WARN <block>: N case(s) have no $BASE result`，不进两边的 rate，
  其中的 crash 仍算新增。AFTER 一个 Pass / Fail 都没有而 `$BASE` 有（例如全变 NotSupported：`$BASE` 的 Pass 全成 L，rate 为 0）→ 该块 **FAIL**；
  `$BASE` 自己没有 Pass / Fail → 该块 INCOMPLETE——算不出 delta 的块永远不是 PASS。NotSupported 数与 `$BASE` 不同时打
  `WARN <block>: NotSupported …`（信息；其中原为 Pass 的是 L 例，见上）。
  每块第二个 `info` 行是旧口径 Pass / (结果数 − NS)：BASE、AFTER 两个 rate 与 delta，只供对照，**不判**。另列 Pass→Fail（`Pass->not`）与非 Pass→Pass 的逐例名单；
  AFTER 高出 0.5 pp 以上只标注不判红。某块用的不是工具树里完整的 `p7-<block>-gl46.txt`（`--limit` 子集）时，全部通过也只记 **`CTS PASS-SUBSET`**。
- 末行 `WINDOW2 GATE3 <v> | CTS <v> -> exit <n>`：门 3 为 `PASS`（规范 36 例、inproc / spawn 各 ≥ 3 遍、四臂齐、reboot-clean、同一会话、
  跨臂不同全部裁定）且 CTS 为 `PASS`（五块完整、无未裁定的 L 例）才是 0；`PASS-SUBSET` / `PASS-NOT-REBOOT-CLEAN` / `PASS-NEEDS-ADJUDICATION` /
  `PASS-WITH-EXTRA-READINGS` 不是判据，`FAIL-REPEATS` / `FAIL-ARMS` / `FAIL-CASESET` 是合同常量不符，均退出 1；门 3 没跑（无 `gate3/cases.txt`）记 `NOT-RUN`，同样非 0。
  `reduce` 的退出码只作记录：红的门也要走完 restore。
- 自检（主机，~7 s）：`python3 60-reduce-selftest.py`（门 3 按规范 36 例造：用例名由自检自己按 `30-gate3.sh` 的算法从 `trace_cases.json` 算）——造目录验：
  缺块 → `CTS INCOMPLETE`；boot_id 变 / 旧 `.done` / 缺 `boot-id.txt` → `GATE3 INVALID-SESSION`；monolith 日志带 `Config: IPC` → FAIL；
  **合同常量**：`--repeat 1`、同一归档配上谎报 `repeat=3` 的 `arms.txt`、已完成的对只有 2 遍 → `FAIL-REPEATS`（同一对没有 `.done` → INCOMPLETE）；
  开跑不带 ra0 / 不带 spawn → `FAIL-ARMS`，ra0 已列未跑 / 缺一例 → INCOMPLETE，ra0 日志 `run-ahead=1` → 该例 FAIL；
  35 例 + create-indirect / 35 例 + 集外名 / 36 例 + 排除例 / 重复行 → `FAIL-CASESET`；
  **跨臂**：spawn 每例不同 → WARN + 像素差、`PASS-NEEDS-ADJUDICATION`，全部写进 `gate3/adjudication.tsv` → PASS，缺一例（无理由行）→ 仍 NEEDS-ADJUDICATION，
  只一例不同且已裁定（另有陈旧行）→ PASS；臂内不同 → FAIL；**ID-P7-62**（8×8 合成图：灰 128、前 n 个像素换值）：monolith ×3 逐位相同 + inproc 不同 → FAIL；
  monolith 3 遍 3 张（两两 10 px / 差 4）+ split 在 1.25 倍内（10 px / 差 5）→ PASS、退出 0、不需裁定行、印出数字，标准库解码同数；
  split 超界（差 6 > 5；13 px > 12.5）→ FAIL；某 split 遍对**一个** monolith 读数差 0.00051 → FAIL；monolith ×1（旧归档）+ split 不同 → FAIL 且注明需 ≥ 3 遍；
  已完成的 monolith 对只有 2 遍 → FAIL（无 `.done` → INCOMPLETE）；×1 归档 + `--extra-monolith DIR@BOOT` 两遍 → 规则生效、`PASS-WITH-EXTRA-READINGS`（退出 1），
  不带 `@BOOT` / 另一 boot → `INVALID-SESSION`；**只由 rate 定**的块（dsa 1 / 2 / 4 例 Pass→Fail：−0.270 pp PASS、−0.541 pp FAIL、
  364/6/0/1/0 −1.081 pp FAIL，无 crash）；**L 例**：dsa Pass→NS 未裁定 → 367/370、−0.270 pp（字面 −0.001 pp 只作信息）、列出、dsa FAIL，裁定后 PASS
  且出 rate（367/369 对 367/369、+0.000 pp、印 `ADJUDICATED`）；一例裁定一例未裁定 → L 1、−0.271 pp、因未裁定例 FAIL；2 例裁定 NS + 2 例 Pass→Fail →
  −0.543 pp 因 rate FAIL；无理由的裁定行不算；ssbo Pass→NS 裁定后 PASS（+0.000 pp，不再 −0.806）；codex 的例子 ssbo 123/124 变 NS：字面 100% 而门 rate 0.806% → FAIL；
  Pass→CompatibilityWarning 也是 L；Pass→Crash 裁定后仍因新 crash FAIL；caselist 没了 / 为空 / 全 NotSupported → MISSING / FAIL 而非 PASS；
  2 例门 3、非 reboot-clean、`--limit` CTS → `PASS-SUBSET` / `PASS-NOT-REBOOT-CLEAN` 且退出非 0；`$BASE` 缺的用例 → WARN；`--check-block` 的 0 / 3；
  dry run 数据（`60-reduce-selftest-pre14e1c8b9.json` = pre-14e1c8b9 五块相对 `$BASE` 的差异）上 dsa 门 rate −0.541 pp（字面 −0.272 pp、
  旧口径 −0.539 pp 只作信息）；`~/w7/logs/devprep/w2/pre-14e1c8b9` 在时再对原始输出验一遍。
  `--reducer <py>` 可对别的 reducer 跑同一套检查（red-once：`6afa077f` 的 reducer 113 项里红 54 项；`8f3d179c` 的 reducer 116 项里红 5 项——裁定例仍进 rate；
  `dd415408` 的 reducer 131 项里红 43 项（monolith ×3 与 ID-P7-62 的 15 项全红）；只关掉本 reducer 的 ID-P7-62 分支 → 131 项里红 9 项）。
  `bash 30-gate3-selftest.sh [--script <sh>]`：stub adb + stub 跑器驱动 `30-gate3.sh`——新跑 `arms.txt` 记 `monolith_repeat=3`、monolith 按 ×3 调用并归档 3 遍、
  删一个 monolith `.done` 只重跑那一对、monolith 缺 PNG → 无 `.done` 退出 3 再跑补齐、`monolith_repeat=3` 的跑以 `--repeat 1` 续跑仍 ×3、
  无 `monolith_repeat=` 的旧 `arms.txt` 续跑仍 monolith ×1（red-once：`dd415408` 的 `30-gate3.sh` 11 项里红 8 项）。

## 4. 入库清单 → `docs/Disaggregated/notes/p7/device-window-2/`

`verdict.txt`、`verdict.json`、`timing.tsv`；`session/`（`found.env`、`reboot-clean.txt`、`boot-id*.txt`、`apk.sha256`、`apk-on-device.sha256`、
`apk-install.txt`、`pin-*.txt`、`device.txt`、`restored.txt`）；`gate3/{cases.txt,excluded.txt,arms.txt,progress.tsv,pin-*.txt}`；
`bsl/<arm>/{peaks.txt,wbuf.txt,wbuf-gauges.txt}`；`cts/{IDENTITY 即 deploy/IDENTITY.txt,arm-proof.txt,preflight.txt,report-*.txt,report-*.json,after-*.json,after-*.md,pin-*.txt}`、
`cts/runs/*/{crashed,hung,unrun,skipped,complete,caselist}.txt` 与 `caselist.path`。归档图像与日志（`gate3/archive/`、`bsl/*/archive/`、qpa）留在 `~/w7/logs/p7w7/<stamp>/`，
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
| CTS 计时样本：p7w6 inproc 全 ssbo + dsa | ssbo 124/124；dsa P/F/NS/W/X 366/3/0/1/1，合同口径 −0.272 pp（旧口径 −0.539），1 新 crash（见 §6） | 3 s + 9 s |
| CTS 预读：pipe 头 `14e1c8b9` 的 arm64 lib（取自上行未安装的 APK，只推到 `mgcts`）五块全跑 | 见 §6：dsa 与 shader-image 各 1 个新 crash；之后 `$BASE` lib 推回并核 sha | 358 s |

**外推**：窗口 1 同机同口径的逐例间隔（E0a monolith 39 例 360 s、E0a inproc 342 s、W4 inproc 412 s；最慢 derivative 30 s、
bsl-esc-menu 15–30 s）与本次 dry run 的 7–8 s/例一致，36 例一遍 ≈ 5.5–7 min；10 遍（monolith ×3 起，ID-P7-62；此前 8 遍）≈ 55–70 min，另加 144 次 runner 调用的
启动 / 汇总页与每臂 pin check ≈ 5 min → 门 3 65–80 min。CTS 五块 inproc 实测 358 s（shader-image 9 / ssbo 3 / dsa 9 / texture 108 / packed-pixels 220 s）。

## 6. 已知事实与限制

- **门 5 在 pipe 头 `14e1c8b9` 上会红（预读，非判据，逐例归属在 `docs/Disaggregated/notes/p7/device-window-2/dry-run/pre-14e1c8b9-cts/attribution.txt`）**：同一 lib 的 monolith 与
  inproc 逐例对照（inproc 带或不带三个 split 旋钮结果相同）：
  - `KHR-GL46.direct_state_access.renderbuffers_storage`：monolith Pass，inproc **SIGABRT** `Fatal{ReplyTooLarge, "ReadPixels 256x512 0x1908/0x1406 2097152 > 2097136"}`
    ——RGBA/FLOAT 回读恰 2 MiB，比一个 reply 槽的载荷上限（槽 − 16 B 头，`MG_Remote/Transport/ReplySlot.h:166-169`）多 16 B；客户端在发射前具名拒绝
    （`MG_Remote/Client/ClientSession.cpp:2319`，ID-47：「chunked readback 是 P6 债」）。
  - `…renderbuffers_storage_multisample`：monolith Pass，inproc Fail。两例合计 dsa 按合同口径 Pass/(Pass+Fail) 为 **−0.272 pp**
    （368/370 → 366/369，在 0.5 pp 内；旧口径 Pass/(结果数 − NS) 为 −0.539 pp）——dsa 红在 1 个新 crash，不在 rate。
    新 reducer 在该数据上的读数：`docs/Disaggregated/notes/p7/device-window-2/dry-run/pre-14e1c8b9-cts/verdict-contract.txt`。
    **按 codex 收官复核后的裁定（ruling: integrator 2026-09-23 after codex closeout review）**，`renderbuffers_storage` 的 Pass→Crash 是 L 例：
    门 rate 366/(366+3+1) = 98.919%，**−0.541 pp**，dsa 同时红在新 crash、rate 与未裁定的 L 例（字面 −0.272 pp 只作信息）；
    读数 `…/dry-run/pre-14e1c8b9-cts/verdict-codex-ruling.txt`。
  - `KHR-GL46.shader_image_load_store.incomplete_textures`：monolith Fail（`$BASE` 亦非 Pass），inproc **SIGABRT** `Fatal{UnmigratedVerb, "Magma:image-view-window"}`
    （`MG_Backend/DirectVulkan/Renderer/UniformManager.cpp:151`）——Fail 变 Crash，按门 5 是新 crash。
  - `…shader_image_load_store.advanced-sync-vertexArray`：`$BASE` Pass，pipe 头 monolith 与 inproc 都 Fail——不是分离缺陷，是 p7w1 之后两臂共有的漂移。
  - `KHR-GL46.texture_border_clamp.Texture2DArrayCompressed`：整块里 Fail 一次，单跑三臂都 Pass——抖动。
  - 其余：ssbo、packed-pixels 与 `$BASE` 相同；texture +0.960 pp；shader-image 整体 +4.284 pp（合同口径；旧口径 +2.9，3 例修好）。
  终局窗口之前要么修（ReadPixels 分块或放大 reply 槽；`image-view-window` 的 incomplete-texture 形状 decline 成 GL 语义而不是 Fatal），要么由集成者裁定。
- dry run 暴露并已修的两处：`run_android_retrace_local.py` 只会用 Git Bash 起跑（WSL 下每例 FileNotFoundError），且一遍没产出时会归档上一遍的图（假「逐位相同」）——`fe4c8745`，带 red-once 单测；
  脚本自身的 `pipefail` + `grep -q` 把在跑的 supervisor 读成「不在」——已改为先取输出再匹配。
- `$BASE` 的机器可读 JSON（CTS-base/README 所称 `base-DirectVulkan-monolith-p7w1-9be62cbc.json`）此前不在任何树里；已按 `cts_multi_report.py --adopt-legacy` 从
  `~/w7/logs/cts-a64/base/runs` 重生成入 `tools/cts/baselines/`（数字与 README 表逐块一致；texture 因 skip 表 1 例记 INCOMPLETE 是该工具的口径）。
  判据本身用的是入库的 `CTS-base/report-*.json` 逐例结果。
- `$BASE` 库的 IDENTITY 写 stamp `p7w1-d260f110`，README / JSON 名写 `9be62cbc`：两者是同一补丁（`d260f110` 与 `9be62cbc` 同题提交），只是命名差。
- 每次回放的 `am force-stop` 会杀 TCP supervisor；restore 负责按开窗状态复原。
- **窗口 2（`p7w7-6afa077f`）按 ID-P7-62 只读重判**（输出 `~/w7/logs/g3-tools/`，归档未改）：原档 monolith ×1 → 三个 Iris 例仍 FAIL（规则需 ≥ 3 遍），
  `GATE3 FAIL 33/36`（`verdict-rereduce.txt`）；加 g3det `E1-mono`（同 boot，`spec1*.log` 前后核）→ sundial、derivative 过，**bliss 超界**
  （monolith 6 遍 4 张、两两最大 17 px / 差 2；split 最大 23 px / 差 3），`GATE3 FAIL 35/36`（`verdict-rereduce-e1mono.txt`）；再加 `E5-mono-{a,b,c}`
  （同 boot，`spec2.log`）→ bliss monolith 18 遍 11 张、22 px / 差 3，split 23 px / 差 3，36/36，只余 create-instancing 的跨臂裁定：
  `PASS-NEEDS-ADJUDICATION`（`verdict-rereduce-e1e5mono.txt`）。
- **bliss 在 monolith ×3 下多半过不了 (2)**：它的不确定性是稀疏事件。同会话 18 遍 monolith 里任取 3 遍作参照、配门 3 自己的 split 6 遍，(2) 只在 16.2% 的组合下成立
  （×6 33%，×9 50%）；sundial 85%、derivative 75%（`~/w7/logs/g3-tools/{x3sim.txt,xksim-bliss.txt}`）。按现行判据下一窗口 bliss 大概率仍红——这是判据的统计性质，
  不是新缺陷；要不要给列名的例加 monolith 遍数或改 (2) 的比较口径，由集成者定。
