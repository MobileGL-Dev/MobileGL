# W6 真机验收进度 · p7w6（F 指纹 + M2 r2）

设备：Redmi `2f7cbe2e`，APK stamp `p7w6-0e16ca27`，签名 APK SHA-256 `45759c3038854c5cff558a352b27f86553ebcddad2ee218fe796d74208406cf8`（[`apk-p7w6.sha256`](apk-p7w6.sha256)）；安装的 `versionName=26.09.0e16ca2-trace`、`lastUpdateTime=2026-09-23 12:03:48`（[`apk-install.txt`](apk-install.txt)）。源码 `0e16ca27` 相比交接的 `3374becd` 仅多一笔文档提交。`svc power stayon true` 后 `stay_on_while_plugged_in=15`，手机 `mWakefulness=Awake`。

## M2 设备项：bsl-esc-menu-854

DirectVulkan × pbuffer，spawn 与 inproc 各三遍：6/6 replay passed、零 Fatal，三遍内逐位相同，两臂之间逐位相同；每遍对 golden 的 SSIM `0.999791667`，实际图像 SHA-256 全部 `155dcdd13a24e0ee…`，与窗口 E0a 的 monolith 图像 SHA 完全相同（[`bsl-actual.sha256`](bsl-actual.sha256)，[`spawn 摘要`](bsl-spawn-summary.txt)，[`inproc 摘要`](bsl-inproc-summary.txt)）。旧 p7w4 的 spawn server 曾在 scudo `internal map failure` 中死亡；本轮三遍均完整结束。这关闭了 bsl 的设备正确性项。原始产物留在 Windows worktree `.trace-work/p7w6/bsl-{spawn,inproc}/`。

本轮前六次回放没有打开 `MOBILEGL_PIPE_STATS`，因此没有 `wbuf[]` 峰值；一次回放后的 server pid 15900 `/proc/<pid>/maps` 为 3595 行、`VmRSS=353484 kB`，**这是结束后的样本，不是峰值**。峰值与 `wbuf[]` 仍待开 stats 的专门一遍采集，不能把这条 3595 当作 M2 的峰值证明。

**归档清单的 APK SHA 有误**：这六遍使用 `MOBILEGL_TRACE_SKIP_INSTALL=1`，由上述已安装 p7w6 包执行，但 `run_android_retrace_local.py` 的 `run.json` 仍对工作树中最新的 `MobileGL-plugin-trace-release-nogit.apk` 求 SHA，记成 `18701603…`；它没有读取设备已安装 APK。原始 `run.json` 保留，不能用其 `apk_sha256` 证明本批身份。之后已把已签名 p7w6 APK复制到 Windows trace/release 输出目录，供后续门 3 回放的归档清单选中；本批身份按安装记录与 APK SHA 交叉核对。

## TCP 对照与终局

手机 supervisor 已用新 APK在 `0.0.0.0:40613` 监听。相同 stamp 的 WSL 主机制品在 `~/w7/logs/p7w6host/host/`，OpenRA × DirectGLES × TCP 冒烟回放通过（14.941 s，run-ahead ARMED）；先前一次因本机 bsl 回放强制停掉该包的 Service 而连接失败，重启 supervisor 后通过，属于采集顺序，不是指纹拒绝。p7w6 的 38 例 DirectGLES TCP 对照正在运行，逐例保留 `results.json`、client 日志、完整手机 logcat 与 child reap；门 3 的 36 例 × inproc/spawn × 三遍尚未开始。
