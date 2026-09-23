# P6.5 第一波证据索引

本次实现与验收的总入口。完整 trace 和性能表仍在采集，不能将本索引理解为阶段收官。

> **2026-09-22 收尾追加**：四切片代码审查完成（数据面/wire/握手无硬缺陷），两项修复已落地并再验收——见
> [code-review-findings.md](code-review-findings.md)。修复：`SocketTransport::AcceptPair` 重试 `EINTR`/`ECONNABORTED`
> （Medium，防常驻 server 因 accept 瞬态整体退出）、supervisor 自设 `MOBILEGL_IPC_ROLE=server`（Low）；均在
> `MG_Remote/` 门内，G1 不受影响。固定制品（client `e17f82f1` + APK `780bb00e`，同 stamp `p65-acceptfix-20260922`）
> 再验收：主机 unit 2399/2399、integration-tcp loopback 102/102、**device-102 102/102 全 ARMED**、代表性 device
> retrace 3/3 ARMED golden（OpenRA 1.0）。**残余**：完整 39 例 device golden 矩阵与全套必测数按用户「本地代表性、
> CI 兜底」定为 CI/后续；本收尾未宣布 P6.5 阶段收官。临时环境（host route / idle 白名单 / server）已恢复。

## 源码与制品身份

- 基线：远端 `feat/disaggregated` 的 `9043442454ae38568315ddf2b1c1814b183aa235`。
  最后一次 fetch 已确认没有更新；该提交是当前分支的祖先。
- 开发工作树：`codex/p65-all-tcp`，HEAD `018fab0f` 加未提交修改。
  用户纠正提交偏好之前已有的两个本地提交随后只做了 rebase；之后未创建开发提交、未 push。
- 两端显式 build stamp：`410dfa94c3d92241e9ec01ddbba1bb911eb60d4b`。
  它是本轮共享的构建标识，不代表当前 HEAD 或一份已提交的最终源码。
- Android：NDK 27.3、arm64-v8a、debuggable traceRelease，实际 native `-O2 -DNDEBUG`。

| 制品 | SHA-256 | 使用它的证据 |
|---|---|---|
| READY7 host libMobileGL.so | `29fcff6b23557be6b1cc3765db382b3422ed0780643c719937616259298d3116` | host/device 102 与修后 TCP CPU |
| 优化 APK | `f2a957c3c6ce1c3c77dd0b99f233f40eed9e47896830ee7767af6b7fbb738f50` | READY7 真机与正式 trace |
| 最终 ALL 后 host libMobileGL.so | `25ca45d756bc5a4e5bd56ad4ce231e4ef89d45ba41131f389575cda0842cd6be` | 全 unit 与正式 trace |

最后一次 ALL 增量只重编测试和 SPIRV-Cross 的 `spirv_cross_c.cpp`；未重编 MobileGL 生产 object。
其 CMake 每次 configure 都生成 gitversion 时间戳，这次值为 `2026-09-22T08:02:51`，
导致重新链接后 SHA 改变。保留两份身份，不能把较早样本改标为新制品。

## 已完成的验证

所有 `/home/swung/` 路径位于 WSL Arch，日志/XML/原始样本保留在对应位置。

| 检查 | 实际结果 | 证据 |
|---|---|---|
| 最终 ALL | exit 0 | `/home/swung/p65-final-tests-build.log` |
| 全 unit | 2389 PASS、0 FAIL、10 个原有条件性 skip | `/home/swung/p65-ready7-unit.{log,xml}` |
| TCP loopback | 102 PASS、0 FAIL、0 SKIP；102 条实际 run-ahead ARMED | `/home/swung/p65-ready7-tcp.{log,xml}`、`p65-ready7-tcp-lane.json` |
| Redmi TCP | 102 PASS、0 FAIL、0 SKIP；102 条实际 run-ahead ARMED，72.23 s | `/home/swung/p65-armed-device.{log,xml}`、`p65-armed-device-lane.json`、204 份 `p65-armed-device-rolelogs/` |
| 名集合 | split 105 中可比 102 = spawn 102 = TCP 102 = device 102 | `/home/swung/p65-ready7-lane-parity.log` |
| G14 | 4016 → 4257，0 删名、241 新增（含可选 device 102） | `/home/swung/p65-ready7-test-name-parity.json` |
| 最终 G1 | 符号 0 增/删/resize/rename；`.text` 10,822,147 B 逐字节相同 | `/home/swung/p65-g1-ready7-{report.md,symbols.json,verification.json}` |
| 真机 SIGKILL | 133 ms，生产挂断路径闩 device loss | `/home/swung/p65-device-peerloss/kill.log` |
| 真机 Wi-Fi 半开 | 5053 ms，低于 10 s；Wi-Fi 已恢复 | `/home/swung/p65-device-wifi-loss/{probe.log,timing.json}` |
| 真实生产 idle flush 负控 | 原库绿 → 仅删除 idle publication 后 8 s 抓到停滞 → 恢复绿 | `/tmp/p65-idle-redonce-c_n59aqs/results.json` |
| 日志前送负控 | 三个 server marker 各 ON 绿 / OFF 缺指定 marker 红 | `/home/swung/p65-lf-controls2/summary.json` |
| CI 修复 | role-log 正反控通过；真实两后端 RSP 2 PASS；设备离线分类 5/5 | `/home/swung/p65-ci-runtime-proof.{log,xml,json}`、`scripts/ci/test_trace_infrastructure.py` |

初始 caps 的三个新回归包含在全 unit 中：真实 TCP/ControlInbox 延迟快照正确武装，
健康但不发快照的 peer 在 5 s 内具名启动失败且不伪造 device loss，坏快照拒绝。
之后的能力更新仍只能 demote，不能重新 promote。

此前的保留车道是 spawn 102/102、inproc 186/186、零 skip，见 `p65-final-{spawn,inproc}.{log,xml}`。
它们与 G1 细节见 [wire-and-validation.md](wire-and-validation.md)。早期 TCP/device 的 lockstep 样本
仍保留作历史证据，不能替代上表的实际 run-ahead 结果。

最终 G1 更新独立 source snapshot 后只增量构建 after；before 制品与固定的 hash 头未变。
双方 `.text` SHA-256 为 `35115edd2c265162c7f845a87d1a2cb9849609a40ba0febac317b91370b9163c`。

## trace、测量与审查

- 正式 GLES 39 项：`/home/swung/p65-final-matrix-gles/`。使用原有 golden 与逐例实际 ARMED 门；进行中。
- 已完成条目中 `minecraft-1.21.11-main-menu` 为 FAIL（SSIM 0.890161）。原样主机 TCP 对照通过；
  只从临时 trace 删除 3,878 个不满足 Redmi 32 字节 UBO 对齐的 bind，便能在主机复现缺失，
  与设备实际图 SSIM 0.998729。该诊断不计正式通过，设备本地对照尚待；
  详见 [mainmenu-trace-difference.md](mainmenu-trace-difference.md)。
- CPU 前后与正式 TCP/spawn 对比：[performance.md](performance.md)。保留非零差值、线程测量范围、
  样本散布和 before 制品身份限制；没有把性能“只记录”解释成可以不测。
- 阶段审查发现 Present/glFlush 尾批可能留在本地；已补真实传输边界并验证前三个 present credit
  各自到达 idle peer，client waits=0、applied=0。后续窄查未发现正常关闭路径死锁/UAF；
  `WaitClosed=true` 也可能表示 terminal protocol error，不能独自作为正常 EOF 证明。
- link seam 明确保留 cached endpoint、旧白盒 raw-ring constructor 和共享段 bootstrap accessor。
  ownership/dispatch gate 与七个负控通过；没有声称所有历史 accessor 或字面 grep 命中归零。

唯一已知的 verify+split 前置债仍为 `PipeRespecifyScope`，inproc/spawn/TCP 同因复现。
LF 第四个 PipeVerify marker 属于 client，本来就不能成为 server forwarding 的负控；
未执行到的断言不计通过。说明见 [validation-status.md](validation-status.md)。

临时设备 idle 豁免与 WSL 手机 host route 要在全部真机工作结束后恢复；当前仍在使用。
闲置的本机 40614 supervisor 与 adb forward 40615 已清理。
