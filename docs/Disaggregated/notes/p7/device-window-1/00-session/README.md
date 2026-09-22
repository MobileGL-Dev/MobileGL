# 设备窗口 #1 · 会话记录（Redmi `2f7cbe2e`，2026-09-22）

按 [`../../device-window-1-runbook.md`](../../device-window-1-runbook.md) 执行。本窗口分两段：**1a**（E0–E3，只需 APK，不需要主机制品）在 wave 0 合并中途跑；**1b**（P6.5 残余 39 例 TCP 矩阵 + CTS `$BASE`）等 wave 0 全部合入、两端同 stamp 后跑。

## reboot-clean

- `boot-id-before.txt` = `7e6c6e9f-1894-4916-bf39-cce48c9abd37`（这是 P6 门 7 第二会话结束时的 boot id）
- `boot-id-after.txt` = `18d8d589-1b96-4cf1-b655-16d5b8d198ba` → **确实重启了**
- 唤醒 + `svc power stayon usb`；`deviceidle get deep` = `ACTIVE`；本包不在 Doze 白名单（`deviceidle-whitelist-before.txt` 为空）

## 钉频：VERDICT = DRIFT，如实记录，原因已定位

`pin_device.sh 2f7cbe2e pin` / `check` 都报 **DRIFT**：`gpu pinned to pwrlevel 0 but gpuclk=1100000000 (expected 1050000000)`。原始 kgsl 读数（root）：

```
gpuclk=1100000000  max_gpuclk=1100000000  min_pwrlevel=0  max_pwrlevel=0  thermal_pwrlevel=0  default_pwrlevel=12
devfreq/max_freq=1050000000
available: 1100000000 1050000000 967000000 ... 160000000
```

也就是说：**这次重启后板子不再被 `thermal_pwrlevel=1` 闩住**（P6 记为「永久闩死、写 0 读回 1」，`pin_device.sh:85-97`），GPU 真的钉在 pwrlevel 0 = **1100 MHz**，与 `35d0befa` 那块板一样；DRIFT 是脚本对本板的期望值（1050 MHz）过期，不是钉频失败。判读：

- 对 **1a（E0–E3）**：实验量的是像素与重复确定性，频率不是变量，两臂同会话同频；本段照常进行，臂前后的 `check` 仍逐次记录（都会是同一条 DRIFT 行）。
- 对任何**逐线程 CPU 时间**的读数（本窗口没有）：1100 MHz 会话与 P6 的 1050 MHz 会话绝对值不可比——这与 P6 原本的口径一致（`pin_device.sh:92-93`）。
- 后续小件（wave 2-E）：`pin_device.sh` 的 `2f7cbe2e` 条目改成「pwrlevel 0 且 gpuclk ∈ {1100, 1050} MHz 都算 PINNED，并把实际值写进 check 输出」，理由在此。

## APK 身份

见 `apk.sha256` / `apk-install.txt`（安装后写入）。构建：`~/w7/pipe@d260f110`，`assembleTraceRelease -Pmobilegl.buildDisaggregated=ON -Pmobilegl.buildDisaggregatedInproc=ON -Pmobilegl.pipePush=ON -Pmobilegl.debuggableRelease=true -Pmobilegl.applicationIdSuffix=.p7w1 -Pmobilegl.apkSuffix=p7w1`，显式 `MOBILEGL_BUILD_STAMP=p7w1-d260f110`，debug keystore 签名，并排安装为 `top.mobilegl.plugin.p7w1.trace`。
