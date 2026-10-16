# 设备窗口 #1 运行手册（wave 1，Redmi `2f7cbe2e`）

本文是 [`PLAN-PH-P34B-P7.md`](PLAN-PH-P34B-P7.md) §3 wave 1 的**可逐条粘贴**执行稿：一次
reboot-clean 会话里打三笔债——P7-7 实验 E0–E3、P6.5 残余的 39 例 DirectGLES TCP golden
矩阵、P3b/P4b + P7 出口门 5 的 CTS `$BASE`。

> **它不做什么。** 不改任何源码，不调阈值，不删用例。每一步都先说明**什么结果支持哪个假设**，
> 再说明证据落在哪里。没有跑到的步骤写进 `unrun.md`，不按「大概没问题」记过。

设备由集成者独占；本手册假定执行者就是持机的人。所有命令块都标了它跑在哪个 shell 里：
**[Win]** = Windows（PowerShell 或 Git Bash，`adb` 在这一侧），**[WSL]** = WSL Arch。

---

## 0. 设备与环境事实（不要重新发现）

来自 [`../p65/validation-status.md`](../p65/validation-status.md) 与
[`../../../../tools/device_bench/pin_device.sh`](../../../../tools/device_bench/pin_device.sh) 的 `2f7cbe2e` 行：

| 事实 | 值 | 出处 |
|---|---|---|
| 设备 | Redmi M332BF / SM8750 / **Adreno 830v2**，aarch64 | `pin_device.sh:73-74` |
| LAN IP | `192.168.21.181` | `validation-status.md:130` |
| GPU 钉频上限 | **1050 MHz**（本板 `thermal_pwrlevel=1` 闩死，`max_gpuclk` 只读 1050000000） | `pin_device.sh:85-97` |
| 大核 / 小核钉频 | policy6 `1958400`，policy0 `1555200` | `pin_device.sh:82` |
| 绝对时间可比性 | **不可**与 `35d0befa` 的行比较（那台还能到 1100） | `pin_device.sh:92-93` |
| 测试包 | `top.mobilegl.plugin.trace`（可用 `MOBILEGL_TRACE_PACKAGE` 改） | `run_android_retrace_local.py:34-64` |
| Doze 状态文件 | `/home/swung/.cache/mobilegl/tcp-server/2f7cbe2e-top.mobilegl.plugin.trace.json`，内容 `originally_whitelisted=false` | `validation-status.md:211-213` |
| 已知真机 DirectVulkan 分歧 | 两次观测 **0.988998 / 0.976494**，inproc 与 spawn 一致，**同 pbuffer 的 monolith = 1.0** | `CURRENT_STAGE_PROGRESS.md:158-160` |
| 已知 GLES 失败 | `minecraft-1.21.11-main-menu` SSIM 0.890161（trace 的 UBO offset 不满足本机 32 字节对齐，非分离缺陷） | `../p65/mainmenu-trace-difference.md` |
| 出口门 3 分母排除 | `minecraft-1.21.4-rd12-odinlite-in-world`、`create-indirect`（OQ-17 dev 侧崩溃） | ID-P7-4 |

**`--use-pbuffer` 在本机的 DirectVulkan 上是可用的**：`CURRENT_STAGE_PROGRESS.md:158` 记的
monolith 1.0 就是同一 pbuffer 口径取的。（`tools/cts/README.md:120-124` 说 DirectVulkan 的
pbuffer 在 Adreno 上不可用——那条**只针对 CTS 的 dEQP platform 端口**，retrace 走 MobileGL 自
己的 EGL，两者不是同一条路径。若当天 E0 的 DirectVulkan pbuffer 臂在 `eglMakeCurrent` 上红，
改用窗口表面重跑并在证据里写明换了口径，不要把它当成分歧。）

证据根目录（**每一步都写进去**）：

```
docs/Disaggregated/notes/p7/device-window-1/
  00-session/            boot id、pin 前后、APK 身份、设备信息
  E0-attribution/        归属与重复确定性
  E1-lockstep/           RUN_AHEAD=0 对照
  E2-credit/             PRESENT_CREDIT 1 vs 3
  E3-caps/               两臂 caps 日志比对
  P65-tcp-matrix/        39 例 DirectGLES TCP golden 矩阵
  CTS-base/              monolith × DirectVulkan 的五个块
  unrun.md               窗口内没跑到的步骤，逐条写明原因
```

`.trace-work/` 与 `/home/swung/` 下的原始产物**不入库**；入库的是 `result.json`、
`*-actual.png`（只在分歧 case 上）、`summary.json`、`transport-proof.json`、`pin-*.txt`、
`run.json` 和各步的一段结论。大于 ~2 MiB 的原图留在 WSL 并在文里记绝对路径与 SHA-256。

---

## 1. 窗口前置（**不占设备时间**，前一天做完）

1. **主机工具自测**（全部不需要设备、不需要 GPU）：

   ```bash
   # [WSL] 在本窗口要用的那棵树里
   cd /home/swung/w7/p7-runner
   python3 tools/trace_replay/test_run_tcp_matrix.py      # 18 tests, OK
   python3 tools/trace_replay/test_compare_actuals.py     # 10 tests, OK
   python3 tools/trace_replay/compare_actuals.py --self-test
   python3 scripts/ci/tcp_lane_tools_test.py              # 6 tests, OK
   python3 tools/trace_replay/test_android_lifecycle.py   # 含 SpawnServerPath 的解析门
   ```

2. **构建并签名 APK**（`-Pmobilegl.debuggableRelease=true` **不是可选项**：没有它
   release APK 不可 `run-as`，`trace-replay-ci.sh` 取不到私有角色日志，而「没有库日志」与
   「跑了但没产生日志」在结果里长得一模一样，见 `tools/device_bench/p6/README.md`）：

   ```bash
   # [WSL 或 Win] 见 tools/device_bench/p6/README.md「Building the APK」
   gradle --no-daemon -p <tree>/android-plugin :app:assembleTraceRelease \
     -Pmobilegl.buildDisaggregated=ON -Pmobilegl.buildDisaggregatedInproc=ON \
     -Pmobilegl.pipePush=ON -Pmobilegl.debuggableRelease=true \
     -Pmobilegl.applicationIdSuffix=.p7w1 -Pmobilegl.apkSuffix=p7w1
   apksigner sign --ks ~/.android/debug.keystore --ks-pass pass:android \
     --ks-key-alias androiddebugkey --key-pass pass:android \
     --out MobileGL-plugin-trace-release-p7w1.apk <unsigned>.apk
   ```

   `applicationIdSuffix` 让它**并排**装在既有 trace 安装旁边（两个不同签名的 APK 不能共用
   application id，`adb install -r` 会 `INSTALL_FAILED_UPDATE_INCOMPATIBLE`）。用它就必须同时
   设 `MOBILEGL_TRACE_PACKAGE`，见 §3。

3. **主机侧 TCP 矩阵的制品先备好**（第 7 步用）：一份 WSL 的 `libMobileGL.so` +
   `mobilegl_trace_replay`，以及 `ctest --show-only=json-v1` 的 catalog。两端必须共享同一个
   **显式** `MOBILEGL_BUILD_STAMP`，否则 `MOBILEGL_IPC_REQUIRE_SAME_BUILD=1` 会在握手上拒绝。
   **设备上的 server 也必须是本次构建**（wave 2-F 起 TCP 握手换成了「一条连接 + `Welcome.dataNonce`」）：
   新 APK 的 server 对旧 client 会回 `Refuse{WireFingerprint}`，但**旧 APK 的 server 对新 client 没有任何
   Refuse 帧**——旧 supervisor 等第二条连接 2000 ms 后把两条都关掉（设备日志 `only 1 of 2 connections
   arrived`），主机侧只看到 `MG_Remote client: no Welcome ... (rc=6)`；看到这条先回 §3 核对
   `apk-install.txt` 的 `lastUpdateTime`，再怀疑别的：

   ```bash
   # [WSL]
   cd /home/swung/w7/p7-runner
   ctest --test-dir build-split --show-only=json-v1 > /home/swung/p7w1-catalog.json
   sha256sum build-split/libMobileGL.so build-split/<...>/mobilegl_trace_replay \
     > docs/Disaggregated/notes/p7/device-window-1/00-session/host-artifacts.sha256
   ```

4. **CTS 的 caselist**：见 §8 —— **树内生成不了**，必须在窗口前解决，否则第 8 步只能记 unrun。

---

## 2. 会话开始：reboot-clean、钉频、唤醒（~10 min）

```bash
# [Win] 0) 记下重启前的 boot id，重启，再记一次。两者相同 = 没有真的重启，按失败处理。
adb -s 2f7cbe2e shell cat /proc/sys/kernel/random/boot_id | tee 00-session/boot-id-before.txt
adb -s 2f7cbe2e reboot
adb -s 2f7cbe2e wait-for-device
adb -s 2f7cbe2e shell 'while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 2; done'
adb -s 2f7cbe2e shell cat /proc/sys/kernel/random/boot_id | tee 00-session/boot-id-after.txt

# 1) 唤醒并让屏幕别灭。Dozing 时 native supervisor 会停在 do_freezer_trap，
#    前台 Service 的 partial wake lock 显示 DISABLED（validation-status.md:141-143）。
adb -s 2f7cbe2e shell input keyevent KEYCODE_WAKEUP
adb -s 2f7cbe2e shell svc power stayon usb
adb -s 2f7cbe2e shell dumpsys deviceidle get deep | tee 00-session/deviceidle-before.txt

# 2) 钉频。需要 root（脚本自己会先验 su）。0=PINNED / 1=DRIFT / 2=UNPINNED。
bash tools/device_bench/pin_device.sh 2f7cbe2e pin   | tee 00-session/pin-session-start.txt
bash tools/device_bench/pin_device.sh 2f7cbe2e check | tee 00-session/pin-check-00.txt
```

**纪律：每一个臂开始前 `check`、结束后再 `check`。** 任一次不是 `PINNED`，那个臂的数据作废
（`status: UNPROVEN`），不降级使用。`check` 的三值语义见 `pin_device.sh:19-31`。

> 判别：`boot-id-before` ≠ `boot-id-after` ⇒ 窗口是 reboot-clean 的；相同 ⇒ 重启没发生，
> 停下重来，不要把它记成干净窗口。

---

## 3. APK 安装身份（~5 min）

```bash
# [Win] 注意：Git Bash 里若设了 MSYS_NO_PATHCONV=1，adb.exe 收到 /c/... 会 "failed to stat"——
#       APK 参数用 Windows 路径（$(cygpath -w ...)）；设备侧 /data/... 路径才需要 MSYS_NO_PATHCONV。
#       flavour 后缀在 applicationIdSuffix 之后：包名是 top.mobilegl.plugin.p7w1.trace。
adb -s 2f7cbe2e install -r "$(cygpath -w MobileGL-plugin-trace-release-p7w1.apk)"
adb -s 2f7cbe2e shell pm list packages | grep mobilegl | tee 00-session/packages.txt
adb -s 2f7cbe2e shell dumpsys package top.mobilegl.plugin.p7w1.trace \
  | grep -E 'versionName|lastUpdateTime|codePath|nativeLibraryPath' | tee 00-session/apk-install.txt
sha256sum MobileGL-plugin-trace-release-p7w1.apk | tee 00-session/apk.sha256
# 等 dex2oat 退出再开始任何臂：一条 am_kill ... due to installPackageLI 落在臂里量的是安装器。
adb -s 2f7cbe2e shell 'n=0; while pgrep dex2oat >/dev/null && [ $n -lt 150 ]; do sleep 2; n=$((n+1)); done; echo "dex2oat idle after $((n*2))s"'
# （`pgrep -f dex2oat` 会匹配到承载这条循环的 sh 自己，永远不退出；按进程名匹配。）

# 本窗口的其余步骤都要看得见这个 id：
export MOBILEGL_TRACE_PACKAGE=top.mobilegl.plugin.p7w1.trace   # [Win] 每个 shell 都要设
export MOBILEGL_TRACE_SKIP_INSTALL=1                            # 已装好，别让每个 case 再装一次
```

> 判别：`00-session/apk.sha256` + `apk-install.txt` 的 `lastUpdateTime` 是**这组图像由哪个二进
> 制产生**的唯一答案。`run_android_retrace_local.py --archive-dir` 写的 `run.json` 里也会带
> 同一个 `apk_sha256`（它自己重新算，所以两者不一致本身就是发现）。

---

## 4. E0：分歧归属与重复确定性（~1.5 h）

**问题**：DirectVulkan 的 0.988998 / 0.976494 是**哪些 case**贡献的？同一个 case 跑三遍是不是
逐位相同？

> **窗口预算说明（必须读）。** 39 例 × 2 传输 × 3 遍 = 234 次回放，按每例 45–90 s 计约
> **3–4 h**，一个窗口里放不下它加第 7、8 步。所以 E0 拆成 **E0a 普查（1 遍）→ E0b 确定性
> （只对分歧集 + 两个对照 case 跑 3 遍）**。判别力不受损：确定性问题只对已经分歧的 case 有
> 意义。若当天时间充裕，E0b 直接把 `--case` 换成 `--matrix` 就是完整版。

### E0a：一遍全矩阵，得到分歧 case 列表（~50 min）

```bash
# [Win] 对照臂：monolith。这是「同 pbuffer 下 DirectVulkan 本来是 1.0」的复验。
bash tools/device_bench/pin_device.sh 2f7cbe2e check | tee E0-attribution/pin-before-monolith.txt
python tools/trace_replay/run_android_retrace_local.py \
  --matrix --backend DirectVulkan --use-pbuffer --transport monolith --repeat 1 \
  --archive-dir .trace-work/p7w1/E0a-monolith \
  2>&1 | tee E0-attribution/E0a-monolith.log
bash tools/device_bench/pin_device.sh 2f7cbe2e check | tee E0-attribution/pin-after-monolith.txt

# [Win] 验收臂：inproc。同一进程内跨真实 ring 到 apply 线程；--require-inproc 由脚本自动带上。
bash tools/device_bench/pin_device.sh 2f7cbe2e check | tee E0-attribution/pin-before-inproc.txt
python tools/trace_replay/run_android_retrace_local.py \
  --matrix --backend DirectVulkan --use-pbuffer --transport inproc --repeat 1 \
  --archive-dir .trace-work/p7w1/E0a-inproc \
  2>&1 | tee E0-attribution/E0a-inproc.log
bash tools/device_bench/pin_device.sh 2f7cbe2e check | tee E0-attribution/pin-after-inproc.txt
```

`--matrix` 取的是 **manifest 的 CI split 子集（39 例）**，按各 case 自己的 `ci_backends` 过滤
（DirectVulkan 下是 39 例，DirectGLES 下是 38 例，差的一例是 DirectVulkan-only 的
`iterationrp`）。`--all` 会多带上 `ci:false` 的 rd12——**ID-P7-4 把它排除在出口门 3 的分母外**，
所以这里用 `--matrix`，不用 `--all`。

`iterationrp` 需要 apk.yml 的三个 Magma 旋钮（`.github/workflows/apk.yml:460-462`，原文逐字）：

```bash
# [Win] 只在跑 iterationrp 时导出；它们由 trace-replay-ci.sh 自己读环境，不走 --env
export MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH=1
export MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS=1
export MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER=1
```

`create-indirect` 与 rd12 按 ID-P7-4 排除：它们在 `dev@81b17c0b` 就在 Adreno 830 上坏，不是本
分支造成的。rd12 不在 `--matrix` 里；`create-indirect` **在**，所以它的红**单列**，不计入出口
门 3 的分母（在 `E0-attribution/exclusions.md` 里写清楚，别在汇总表里悄悄删行）。

汇总：

```bash
# [WSL 或 Win]
python3 tools/trace_replay/compare_actuals.py summary .trace-work/p7w1/E0a-inproc \
  --json E0-attribution/E0a-inproc-summary.json | tee E0-attribution/E0a-inproc-summary.txt
python3 tools/trace_replay/compare_actuals.py summary .trace-work/p7w1/E0a-monolith \
  --json E0-attribution/E0a-monolith-summary.json | tee E0-attribution/E0a-monolith-summary.txt
```

> **判别**：monolith 全 1.0 而 inproc 有 N 个 case < 阈值 ⇒ 分歧属于**分离路径**，且那 N 个
> case 的名字就是 wave 2 的工作清单；monolith 也分歧 ⇒ 问题不在分离，在 Magma 本身（整条
> run-ahead / credit 线索出局，E1/E2 不必跑，直接进 E3 与 wave 2-B）。

证据：`E0-attribution/{E0a-*-summary.json,E0a-*.log}`，以及**只对分歧 case** 归档的
`*-actual.png` + `result.json`（从 `.trace-work/p7w1/E0a-inproc/<case>-DirectVulkan/repeat-01/`
拷进 `E0-attribution/divergent/`）。分歧 case 名单写成 `E0-attribution/divergent-cases.md`。

### E0b：分歧集 × 3 遍 × 两传输（~40 min）

```bash
# [Win] $D = E0a 得到的分歧 case，逐个 --case；外加两个 E0a 通过的 case 作对照
python tools/trace_replay/run_android_retrace_local.py \
  --case <divergent-1> --case <divergent-2> ... --case <control-1> --case <control-2> \
  --backend DirectVulkan --use-pbuffer --transport inproc --repeat 3 \
  --archive-dir .trace-work/p7w1/E0b-inproc 2>&1 | tee E0-attribution/E0b-inproc.log
python tools/trace_replay/run_android_retrace_local.py \
  --case <同一组> --backend DirectVulkan --use-pbuffer --transport monolith --repeat 3 \
  --archive-dir .trace-work/p7w1/E0b-monolith 2>&1 | tee E0-attribution/E0b-monolith.log

python3 tools/trace_replay/compare_actuals.py summary .trace-work/p7w1/E0b-inproc \
  --json E0-attribution/E0b-inproc-summary.json | tee E0-attribution/E0b-inproc-summary.txt
```

汇总表的列就是要回答的问题：`case × repeat × ssim_vs_golden × ssim_vs_first × bit-identical`。
`ssim_vs_first` 用的是 `result.json` 自己记的 crop，和 `ssim_vs_golden` 描述同一个矩形；
SSIM 的实现是 retrace 门自己的那一个（`trace_replay_core.cpp:630/:671` 的逐字转写）。

> **判别**：三遍 `bit-identical=yes` ⇒ 分歧是**确定性**的，可以在桌面上用固定输入追（E4/E5 有
> 意义，RenderDoc 抓一次就够）；三遍不逐位相同、`ssim_vs_first < 1` ⇒ 分歧带**时序/竞态**成分，
> 先看 E1（run-ahead 排队）再谈内容路径，且任何单次截图的对比都不可作结论。

---

## 5. E1：`MOBILEGL_IPC_RUN_AHEAD=0` 的 lockstep 对照（~20 min，判别力最高）

这是 §0 结论 3 说的「从未在 golden 口径下跑过、且不需要改任何代码」的那个实验。

```bash
# [Win] 与 E0b 完全相同的 case 集、相同 pbuffer、相同 3 遍，只多一个 --env
bash tools/device_bench/pin_device.sh 2f7cbe2e check | tee E1-lockstep/pin-before.txt
python tools/trace_replay/run_android_retrace_local.py \
  --case <与 E0b 同一组> --backend DirectVulkan --use-pbuffer --transport inproc --repeat 3 \
  --env MOBILEGL_IPC_RUN_AHEAD=0 \
  --archive-dir .trace-work/p7w1/E1-inproc-ra0 2>&1 | tee E1-lockstep/E1.log
bash tools/device_bench/pin_device.sh 2f7cbe2e check | tee E1-lockstep/pin-after.txt
python3 tools/trace_replay/compare_actuals.py summary .trace-work/p7w1/E1-inproc-ra0 \
  --json E1-lockstep/E1-summary.json | tee E1-lockstep/E1-summary.txt
```

### 证明 lockstep 真的生效的那一行日志

**不能指望 `running lockstep`。** 那句话来自 `MG_Remote/Client/ClientSession.cpp:1877-1879`：

```
MG_Remote client: run-ahead requested, server does not publish kCapRunAheadApply -
running lockstep. The selected backend has not enabled its implementation-readiness gate
```

它的守卫是 `else if (MG_Config::Ipc.RunAhead != 0 && m_barrierArmed)`（`:1873`）——即
「**旋钮开着**但 server 没有那个 cap」。`RUN_AHEAD=0` 时这一句**根本不会打印**，
`LatchRunAheadFromCaps` 一行也不打。所以证据是**一条在 + 一条不在**：

| 必须出现 | 出处 |
|---|---|
| `Config: IPC ring=... verb-barrier=1 run-ahead=0 present-credit=... strict=1 ... role-split-state=1` | `ConfigLoader.cpp:459-462`，只在非 monolith 打 |
| `Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream` | ConfigLoader 的 inproc 臂，pull 库写不出来 |

| 必须**不**出现 | 出处 |
|---|---|
| `MG_Remote client: run-ahead ARMED - the server publishes kCapRunAheadApply` | `ClientSession.cpp:1869-1872` |

```bash
# [Win/WSL] 逐遍核对，三遍都要
for r in .trace-work/p7w1/E1-inproc-ra0/*/repeat-*; do
  echo "== $r"
  grep -c 'run-ahead ARMED'        "$r/mobilegl.log" || true      # 必须是 0
  grep -o 'run-ahead=[0-9]*'       "$r/mobilegl.log" | sort -u    # 必须只有 run-ahead=0
  grep -c 'MOBILEGL_TRANSPORT=inproc - the MGPipe record stream' "$r/mobilegl.log"
done | tee E1-lockstep/arm-proof.txt
```

这个形状与 `../p5f/magma-runahead.md:120` 记的一致：「Magma inproc RA=0 | 实际 Config
run-ahead=0，无 ARMED」。

> **已知副作用，先说在前面**：`run_android_retrace_local.py --transport inproc` 会自动带上
> `--require-inproc`，而那个门要求 `Config: IPC` 行里 `run-ahead=1`
> （`android-plugin/trace-replay-ci.sh:601`）。所以 **E1 的每个 case 都会以非零退出码结束，
> 且退出发生在 `result.json` 已经拉回来之后**（`trace-replay-ci.sh:548` 拉结果、`:586` 判
> `passed`、`:587` 才跑 arm proof）。这不是失败：按上表自己核对 `Config: IPC` 行即可，SSIM 与
> 归档照常可用。把 `E1.log` 里的非零退出连同这句说明一起入库，**不要**为了让它绿而去掉
> `--env MOBILEGL_IPC_RUN_AHEAD=0`。

> **判别（本窗口判别力最高的一行）**：分歧 case 在 RA=0 下回到 **1.0** ⇒ 原因在 run-ahead 这条
> 线（未等待的发布排队 / 延迟退休 / present credit），wave 2 从 `ClientSession` 的发布路径查；
> **不变** ⇒ run-ahead 整条线出局，原因在 **server 的内容路径**（Magma 的 wire 应用），E2 的
> credit 扫描多半也会不变，直接转 E3 + wave 2-B/C。

---

## 6. E2：`MOBILEGL_IPC_PRESENT_CREDIT` 1 vs 3（~25 min）

只在 E1 「不变」**或**「部分回到 1.0」时才有意义；E1 全数回 1.0 时这一步降级为确认性抽查。

```bash
# [Win] 两个臂，其余参数与 E0b 逐字相同
for c in 1 3; do
  bash tools/device_bench/pin_device.sh 2f7cbe2e check | tee E2-credit/pin-before-credit$c.txt
  python tools/trace_replay/run_android_retrace_local.py \
    --case <与 E0b 同一组> --backend DirectVulkan --use-pbuffer --transport inproc --repeat 3 \
    --env MOBILEGL_IPC_PRESENT_CREDIT=$c \
    --archive-dir .trace-work/p7w1/E2-credit$c 2>&1 | tee E2-credit/E2-credit$c.log
  python3 tools/trace_replay/compare_actuals.py summary .trace-work/p7w1/E2-credit$c \
    --json E2-credit/E2-credit$c-summary.json | tee E2-credit/E2-credit$c-summary.txt
  bash tools/device_bench/pin_device.sh 2f7cbe2e check | tee E2-credit/pin-after-credit$c.txt
done
grep -ho 'present-credit=[0-9]*' .trace-work/p7w1/E2-credit*/*/repeat-*/mobilegl.log \
  | sort | uniq -c | tee E2-credit/credit-actually-applied.txt
```

最后那一行不是装饰：`present-credit=` 来自 `ConfigLoader.cpp:459-462` 的同一条 `Config: IPC`
行，它是「旋钮真的到达了库」的唯一证据。两个臂各自只应出现自己的值。

> **判别**：credit=1（最严格的节流）下分歧消失、credit=3 下仍在 ⇒ 是**帧间排队深度**问题，与
> E1 的结论应当一致，wave 2 查 present credit 的释放点；两个 credit 表现相同 ⇒ 排队深度不是
> 变量，credit 线索与 run-ahead 线索一起出局。

---

## 7. E3：两臂 caps 日志比对（~10 min，不占额外回放）

E3 不需要新回放，读 E0a/E0b 已经产生的日志即可。

```bash
# [Win/WSL] 两臂、两个角色的日志一起扫
for d in .trace-work/p7w1/E0a-monolith .trace-work/p7w1/E0a-inproc; do
  echo "=== $d"
  grep -rh 'CapsMirror::.* read before the first CapsSnapshot - answering a PLACEHOLDER' "$d" | sort | uniq -c
  grep -rh 'the server does not consume MGPipe subsystem' "$d" | sort | uniq -c
  grep -rh 'CapsMirror generation .* adopted' "$d" | sort | uniq -c
  grep -rhE 'GL_(UNIFORM_BUFFER|SHADER_STORAGE_BUFFER)_OFFSET_ALIGNMENT: [0-9]+' "$d" | sort | uniq -c
done | tee E3-caps/caps-compare.txt
```

精确出处，免得当天去找：

| 行 | 出处 | 期望 |
|---|---|---|
| `MG_Remote client: CapsMirror::<accessor> read before the first CapsSnapshot - answering a PLACEHOLDER. Expected exactly once at startup, from LogBackendInfo (MG_Backend/Init.cpp:21); anything later means a caps read beat the handshake` | `MG_Remote/Client/CapsMirror.cpp:47-51` | 启动期**恰好一条**（来自 `LogBackendInfo`）。多于一条 ⇒ 有 caps 读跑在握手前面，客户端在用占位能力决策 |
| `MG_Remote client: the server does not consume MGPipe subsystem 0x%llx - this family emits NOTHING and the legacy pull path runs for it (R-8). callMask=...` | `CapsMirror.cpp:143-149` | **零条**。出现即该 family 整族不过线、走 legacy pull，是分歧的一等嫌疑 |
| `MG_Remote client: CapsMirror generation N adopted (backend=..., callMask=0x...)` | `CapsMirror.cpp:81-83` | 至少一条，且 `backend` 是 DirectVulkan 对应值；`callMask` 两臂应相同 |
| `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: 32`（SSBO 64） | `MG_Util/BackendLoaders/OpenGL/Loader.cpp:1094`；本机实测值见 `../p65/mainmenu-trace-difference.md` | Adreno 830 的真值是 **32 / 64**（`Loader.h:1263-1265`）。inproc 臂的 client 若打出 256（`Loader.h:1261/:1266` 的默认值）⇒ client 用了默认而不是远端上报的对齐 |

> **判别**：inproc 臂出现 `does not consume MGPipe subsystem` 或 placeholder 多于一条 ⇒ 分歧
> 是**能力协商**问题，与 run-ahead 无关，归 wave 2-C（OQ-8/OQ-10 同族）；两臂 caps 逐字相同 ⇒
> 能力线索出局，留下 Magma wire 应用（wave 2-B）。

---

## 8. P6.5 残余：39 例 DirectGLES TCP golden 矩阵（~2.5–3 h）

口径就是 `validation-status.md:231-233` 欠的那一条：**WSL 作 client，Redmi 作 server**，
逐例要求 `run-ahead ARMED`。

```bash
# [Win] 8.1 起设备端 supervisor（前台 Service），并临时豁免 Doze
python3 tools/trace_replay/tcp_device_server.py start \
  --serial 2f7cbe2e --package top.mobilegl.plugin.p7w1.trace \
  --listen tcp://0.0.0.0:40613 --token <TOKEN> --allow-idle \
  --state-file /home/swung/.cache/mobilegl/tcp-server/2f7cbe2e-top.mobilegl.plugin.p7w1.trace.json \
  | tee P65-tcp-matrix/server-start.txt
adb -s 2f7cbe2e logcat -s MobileGLServer -d | tee P65-tcp-matrix/server-ready.txt
```

`--allow-idle` 是**显式**选项，它把改动前的原状态存进 `--state-file`（重复 start 不覆盖原
值），`stop` 再按原值恢复——不要手工 `dumpsys deviceidle whitelist -`。

```bash
# [WSL] 8.2 LAN 直连。WSL 默认经 eth2/xray_tun 到手机时只回第一个 MSS
#      （validation-status.md:144-146），所以加一条只针对手机的临时 host route。
sudo ip route add 192.168.21.181/32 via 192.168.31.1 dev eth0 metric 10
ping -c 3 192.168.21.181 | tee P65-tcp-matrix/route.txt

# [WSL] 8.3 正式矩阵。--require-run-ahead 强制 RUN_AHEAD=1 + VERB_BARRIER=1，
#      并逐例要求私有 client 日志有 ARMED、没有 lockstep / DISARMED。
cd /home/swung/w7/p7-runner
export MOBILEGL_BUILD_STAMP=<两端共享的显式 stamp>
python3 tools/trace_replay/run_tcp_matrix.py \
  --catalog /home/swung/p7w1-catalog.json \
  --runner /home/swung/w7/p7-runner/build-split/<...>/mobilegl_trace_replay \
  --library /home/swung/w7/p7-runner/build-split/libMobileGL.so \
  --endpoint tcp://192.168.21.181:40613 --token <TOKEN> \
  --backend DirectGLES --credit 2 --require-run-ahead \
  --wake-adb-serial 2f7cbe2e \
  --out /home/swung/p7w1-tcp-matrix 2>&1 | tee P65-tcp-matrix/matrix.log
# 中断 / 超时后原样续跑（同一 library、runner、peer）：
#   ... --out /home/swung/p7w1-tcp-matrix --resume
```

`--wake-adb-serial 2f7cbe2e` 让 driver 每 15 秒打一次 `KEYCODE_WAKEUP`
（`run_tcp_matrix.py:363-371`），这是长 case 跨越息屏的唯一保险。看门狗是
**300 秒无日志进展** + **7200 秒绝对上限**两条独立线，和 manifest 的 `timeout_seconds` 无关。

一条 case 超时或留下残留进程时 driver 会**停下整个矩阵**（避免把对端不健康级联成一串 Busy），
checkpoint 保留；先查对端再 `--resume`。

**预期里已经有一条红**：`minecraft-1.21.11-main-menu`，SSIM ≈ 0.890161。它的成因已定位为
trace 的 UBO offset 不满足本机 32 字节对齐（`../p65/mainmenu-trace-difference.md`），**单列，
不当成新发现，也不改阈值**。

```bash
# [WSL] 8.4 收口
python3 - <<'PY' | tee P65-tcp-matrix/summary.txt
import json, collections
rows = json.load(open('/home/swung/p7w1-tcp-matrix/results.json'))
print(collections.Counter(r['status'] for r in rows))
for r in rows:
    if r['status'] != 'passed':
        print(r['case'], r['backend'], r['status'], r.get('error'))
PY
cp /home/swung/p7w1-tcp-matrix/results.json P65-tcp-matrix/results.json
```

每行都带 `backend`、`golden`、`alternate_golden`、`ssim_threshold`、`requested_arm`、
`actual_arm`，所以这份 `results.json` 自己就能回答「用哪张图、哪个阈值、实际有没有 ARMED」。

> **判别**：38 例（DirectGLES 下 39 例里 `iterationrp` 是 DirectVulkan-only）逐例 ARMED 且除
> main-menu 外全绿 ⇒ P6.5 的这笔残余结清；出现新的红 ⇒ 按 case 名单单列，不与 DirectVulkan
> 的分歧混记。

---

## 9. CTS `$BASE`：monolith × DirectVulkan 的五个块（~1.5–2 h，可无人值守）

出口门 5 要的是「**迁移落地之前**的基线读数」。五个块（`PLAN-PH-P34B-P7.md` §1.3 出口门 5）：
`texture_*`、`shader_image_*`、`packed_pixels`、`direct_state_access.*`、`uniform_buffer_object*`。

### 9.1 两个必须先解决的前置（**当天临时抱佛脚会浪费整个窗口**）

**(a) caselist 树内生成不了。** `tools/cts/` 有完整 harness，但**没有任何 caselist 入库**，
mustpass 清单也不在本仓：

```
$ find tools/cts -iname '*caselist*' -o -iname '*mustpass*' -o -name '*.txt'
（空）
```

本机 `/home/swung/cts-src` 的 checkout 里 `external/openglcts/data/` 是**空的**，也没有任何
mustpass 文件，所以连它也生成不出来。正确做法，窗口**之前**做完：

```bash
# [WSL] 取一个 release tag 的完整 checkout（用 tag，不用 main，读数才可引用）
git -C "$CTS" checkout opengl-cts-4.6.8.1
python "$CTS"/external/fetch_sources.py
ls "$CTS"/external/openglcts/data/mustpass/gl/khronos_mustpass/4.6.*/    # gl46-main.txt 在这里
# 从 mustpass 切出五个块，入库到 tools/cts/caselists/p7-*.txt
cd /home/swung/w7/p7-runner
mkdir -p tools/cts/caselists
MP="$CTS/external/openglcts/data/mustpass/gl/khronos_mustpass/4.6.8/gl46-main.txt"   # 以实际路径为准
grep -E '^KHR-GL46\.texture_'            "$MP" > tools/cts/caselists/p7-texture.txt
grep -E '^KHR-GL46\.shader_image_'       "$MP" > tools/cts/caselists/p7-shader-image.txt
grep -E '^KHR-GL46\.packed_pixels'       "$MP" > tools/cts/caselists/p7-packed-pixels.txt
grep -E '^KHR-GL46\.direct_state_access\.' "$MP" > tools/cts/caselists/p7-dsa.txt
grep -E '^KHR-GL46\.uniform_buffer_object' "$MP" > tools/cts/caselists/p7-ubo.txt
wc -l tools/cts/caselists/p7-*.txt        # 入库时把这几个数写进提交信息
```

入库这五个文件的提交要单独说明它取自哪个 tag、哪个 mustpass 文件、各多少条——
**读数只有连着 caselist 的出处才可引用**。

**(b) `KHR-GL46` 在 Android 跑器上多半够不着。** `tools/cts/skills/gl-cts-on-mobilegl/SKILL.md`
的全部参考读数都是 **KHR-GL33**，并明说「`KHR-GL33` 不需要解门——包注册表无条件注册它」，
暗示别的包**可能**被门住；README 的 4.5 读数取自**桌面 lavapipe**，不是设备。平台端口本身
会把包要求的版本原样传给 `eglCreateContext`（`tcuMobileGLPlatform.cpp:401-405`），所以这是
「MobileGL 的 Android EGL 能否给出 4.6 core context」＋「这个 arm64 glcts 构建里有没有
KHR-GL46 包」两个问题，**都没有树内证据**。窗口前必须先 5 分钟验掉：

```bash
# [Win] 设备上已部署的 glcts，列一下包与其中一个块的条数（只读，不跑用例）
adb -s 2f7cbe2e shell 'cd /data/local/tmp/mgcts && LD_LIBRARY_PATH=. \
  MOBILEGL_BACKEND_TYPE=DirectVulkan ./glcts --deqp-runmode=stdout-caselist \
  --deqp-case=KHR-GL46.texture_\* 2>&1 | head -20'
```

有输出 ⇒ 按 9.2 跑 `KHR-GL46.*`。无输出或 context 创建失败 ⇒ **改用 `KHR-GL33` 的同名块**，
并在 `CTS-base/README.md` 里写明基线的 API 版本是 33 而不是 46，**AFTER 读数必须用同一个版本**
（0.5 pp 是两个同口径读数的差，跨版本比较没有意义）。这是给集成者的裁定项，不是执行者当场
能定的事。

### 9.2 preflight 与跑

```bash
# [Win] 先 preflight，绝不跳过：它一秒内证明这台设备 + 这个库能给出 core context 且 FBO 回读正确
adb -s 2f7cbe2e shell 'cd /data/local/tmp/mgcts && LD_LIBRARY_PATH=. ./mgprobe \
  --backend DirectVulkan --surface imagereader --lib ./libMobileGL.so' | tee CTS-base/preflight.txt
# 期望 PASS ... user_fbo=ok；DirectVulkan 的 default_fb=broken 是已知且不设门的

# [Win] 五个块，逐块一个 outdir。monolith 是 $BASE 的定义，所以不传任何 MOBILEGL_TRANSPORT。
for b in texture shader-image packed-pixels dsa ubo; do
  bash tools/device_bench/pin_device.sh 2f7cbe2e check | tee CTS-base/pin-before-$b.txt
  python tools/cts/scripts/run_cts.py \
    --serial 2f7cbe2e --backend DirectVulkan \
    --caselist tools/cts/caselists/p7-$b.txt \
    --outdir /c/.../CTS-base/runs/$b \
    --env MOBILEGL_CTS_FBO_COLOR_TEXTURE=1 \
    --surface fbo --gl-config-name rgba8888d24s8 --cpu-mask fast \
    --chunk-timeout 900 \
    --skip-file tools/cts/caselists/p7-skip.txt 2>&1 | tee CTS-base/run-$b.log
  python tools/cts/scripts/qpa_report.py /c/.../CTS-base/runs/$b --label "DirectVulkan-$b" \
    | tee CTS-base/report-$b.txt
  bash tools/device_bench/pin_device.sh 2f7cbe2e check | tee CTS-base/pin-after-$b.txt
done
```

不可省的三个参数，理由见 `tools/cts/skills/gl-cts-on-mobilegl/SKILL.md`「Required flags」：

| 参数 | 没有它会怎样 |
|---|---|
| `--surface fbo` | DirectVulkan 从**默认 framebuffer** 回读全零且不报 GL 错，整套读数接近 0 而与一致性无关 |
| `--env MOBILEGL_CTS_FBO_COLOR_TEXTURE=1` | `--surface fbo` 单独不够：dEQP 的 FboRenderContext 挂的是 **renderbuffer**，DirectVulkan 从 renderbuffer-attached FBO 一样回读零。实测影响 46.15% → 72.74%；DirectGLES 两种方式逐位相同，是证明这个开关中性的对照 |
| `--deqp-terminate-on-device-lost=disable`（`run_cts.py` 自己加，`:281`） | 默认 enable 会在每个 case 后调 GL 4.5 的 `glGetGraphicsResetStatus()`，3.3 core 里没有，第一个 case 就段错误 |

`run_cts.py` 的超时是**「device 侧 .qpa 900 秒不增长」**而不是墙钟上限（`:66-76`）——一个 chunk
合法地跑一小时，而 GPU hang 会让日志停止增长。到期它按 caselist 路径 `pkill` 设备侧 glcts 并报
124。它还区分「case 崩了」与「设备没了」（`device_alive`，`:36-44`），会等设备回来、重新拉那份
在 `/data/local/tmp` 上活下来的 `.qpa`，把当时打开的那个 case 记成 DeviceHang 并隔离。

结果落点：每块 `outdir` 下 `chunk*.qpa`、`crashed.txt`、`hung.txt`、`unrun.txt`、`skipped.txt`。
**`unrun.txt` 非空就不能把这块当完整读数**——`qpa_report.py` 会把隔离与未跑的单独列出并排除在
比率之外，照抄它的口径。

> **判别**：五块都有 `unrun.txt` 为空的完整读数 ⇒ `$BASE` 成立，wave 4 的 AFTER 读数按同一
> caselist、同一 flag、同一 API 版本比 0.5 pp；任一块 `unrun.txt` 非空或 `hung.txt` 有新条目
> ⇒ 该块只记为**部分读数**并把 hang 的 case 加进 `p7-skip.txt`，下一窗口补。

---

## 10. 临时环境恢复（**窗口结束必做**）

顺序不能反：先停 server（它会按状态文件恢复 Doze 豁免），再撤路由，最后解钉频。

```bash
# [WSL] 1) 停测试包并恢复本次添加的包级 idle 豁免（validation-status.md:215-218）
python3 <tree>/tools/trace_replay/tcp_device_server.py stop --serial 2f7cbe2e \
  --package top.mobilegl.plugin.p7w1.trace \
  --state-file /home/swung/.cache/mobilegl/tcp-server/2f7cbe2e-top.mobilegl.plugin.p7w1.trace.json

# [WSL root] 2) 撤掉本次新增的临时网络辅助配置
sudo ip route del 192.168.21.181/32 via 192.168.31.1 dev eth0 metric 10
adb -s 2f7cbe2e forward --remove tcp:40615    # 若本窗口用过；40613 的 --forward 同理

# [Win] 3) 解钉频并复核。unpin 之后 check 应报 UNPINNED（退出码 2，对这个动作而言即成功）
bash tools/device_bench/pin_device.sh 2f7cbe2e unpin | tee 00-session/pin-session-end.txt
bash tools/device_bench/pin_device.sh 2f7cbe2e check | tee 00-session/pin-check-final.txt

# [Win] 4) 还原屏幕常亮，并核对 Doze 白名单确实回到会话前的样子
adb -s 2f7cbe2e shell svc power stayon false
adb -s 2f7cbe2e shell dumpsys deviceidle whitelist | grep -i mobilegl | tee 00-session/deviceidle-after.txt
```

`00-session/deviceidle-after.txt` 必须与 `deviceidle-before.txt` 对本包一致。不一致就地修，
不要留给下一个窗口。若本窗口用了并排安装的 `.p7w1` 包，明确决定是卸载还是保留，并写进
`00-session/README.md`——一台共享设备上留一个来历不明的安装，是下一次 A/B 的噪声源。

---

## 11. 收口清单

窗口结束时 `docs/Disaggregated/notes/p7/device-window-1/` 里应当有：

- [ ] `00-session/`：两个 boot id、pin 起止与每臂前后的 `check`、APK SHA-256 与安装时间、Doze 前后
- [ ] `E0-attribution/divergent-cases.md`：**分歧 case 名单**（本窗口最重要的一件产出）+ 两臂 summary
- [ ] `E0-attribution/exclusions.md`：按 ID-P7-4 单列的 `create-indirect`（与未纳入 `--matrix` 的 rd12）
- [ ] `E1-lockstep/arm-proof.txt`：三遍都 `run-ahead=0` 且 `run-ahead ARMED` 计数为 0
- [ ] `E1-lockstep/E1-summary.json` 与一句结论：RA=0 下**回到 1.0 / 不变**
- [ ] `E2-credit/credit-actually-applied.txt`：两个臂各自只出现自己的 `present-credit=`
- [ ] `E3-caps/caps-compare.txt`：placeholder 条数、subsystem 拒绝条数、两臂 `callMask`、UBO/SSBO 对齐实测值
- [ ] `P65-tcp-matrix/results.json` + `summary.txt`：逐例 `actual_arm`，红的单列
- [ ] `CTS-base/report-*.txt` ×5 + 各自的 `unrun.txt`/`hung.txt` 状态，以及 §9.1(b) 的 API 版本裁定
- [ ] `unrun.md`：没跑到的步骤逐条写原因

写完之后，`PLAN-PH-P34B-P7.md` §1.3「出口门 3」一行应当从「不可测」变成「已定位到 N 个
case，原因指向 X」——那句话就是 wave 2 派活的依据。
