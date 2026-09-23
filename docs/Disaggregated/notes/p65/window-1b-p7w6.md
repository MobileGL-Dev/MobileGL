# 窗口 1b：p7w6 DirectGLES × TCP（2026-09-23）

## 分母与结论

清单共 38 例。续跑时 9 例已有终态并跳过（8 PASS、1 idle timeout），本轮新尝试 29 例：16 PASS、8 case-level idle timeout、2 视觉失败、3 missing-result timeout。B4 主机全门与本矩阵有一段时间重叠，可能影响超时耗时；所有超时均保留为未完成/timeout，不能按视觉错图计数。缓存跳过的结果仍计入 38 例分母。

两项真实视觉失败：Iris Photon v1.3b（SSIM 0.008877075，403,024 mismatch）与 Iris Derivative Main D24.4.14（SSIM 0.850784981，402,598 mismatch），阈值 0.99。Photon 输出几乎全黑，仅见准星和快捷栏；phone logcat 记载 clouds/fog shader varying 类型不匹配，server program unusable 后绑定 program 0。D24 输出保留森林岸边，但前景块发黑、地形/水色偏且玩家手缺失；没有显式应用 shader link failure 或 fatal。两例 server child 均记录 exit=0 / sessionsFaulted=1，client frame reply metrics 仍在推进。

同一设备、同一 trace、同一已安装 APK（测前后 SHA-256 均为 `780bb00e81b9cf2e84298543692c1b005c281dfc2a0f499bfa1af86772625590`）随后跑 DirectGLES monolith：Photon 得到完全相同的 SSIM / mismatch（0.008877075 / 403,024），并复现相同 varying link errors；D24 得到 0.850671535 / 402,596（TCP 为 0.850784981 / 402,598），两臂均见 attribute location 超设备限制。两项视觉失败都能在 monolith 复现，不是 TCP 分离路径特有回归。对照输入 trace SHA 分别为 Photon `217d4939…a69b3`、D24 `fe89d01b…9228b0`。

八个本轮 idle timeout：REI normal-world 302.162s、Xaero minimap normal-world 302.244s、Xaero world-map normal-world 328.432s、JourneyMap normal-world 308.783s、ModernUI inventory normal-world 376.767s、Create indirect 380.309s、Improved Transparency 26.3 313.152s、Iris BSL ESC menu 854 302.749s。若无 phone reap 证据，表中标“未知”，不推断服务仍活或已退出。

三个 missing-result timeout：REI inventory、Xaero minimap in-world 由外层 runner 返回 rc=124 / adb_rc=0，但没有 product result JSON；ModernUI inventory 被 1900s 外层上限取消，checkpoint 为 cancelled、child returncode=130，也无产品结果。三例都不计产品视觉失败。

## 逐例结果

“缓存跳过”表示该终态来自续跑前已保存结果；“未知”表示保留的 phone-logcat 中没有 child reap 行。

| # | case id | 结果 | SSIM / mismatch 或耗时 | armed | phone reap |
|---:|---|---|---|---|---|
| 1 | OpenRA | 缓存跳过 PASS | 1.000000 / 0 | 是 | exit=0 |
| 2 | minecraft-1.21.4-startup | 缓存跳过 PASS | 0.999999511 / 520 | 是 | exit=0 |
| 3 | minecraft-1.21.4-main-menu | 缓存跳过 PASS | 0.999695947 / 1,198 | 是 | exit=0 |
| 4 | minecraft-1.21.11-main-menu | 缓存跳过 idle timeout | 302.505s | 是 | 未知 |
| 5 | minecraft-1.17-main-menu-854 | 缓存跳过 PASS | 0.999990861 / 327 | 是 | exit=0 |
| 6 | minecraft-1.21.4-in-world | 缓存跳过 PASS | 0.999995438 / 27,575 | 是 | exit=0 |
| 7 | minecraft-1.21.4-fabric-sodium-in-world | 缓存跳过 PASS | 0.999993736 / 29,859 | 是 | exit=0 |
| 8 | minecraft-1.21.4-fabric-common-mods-in-world | 缓存跳过 PASS | 0.999869638 / 79,685 | 是 | exit=0 |
| 9 | minecraft-1.21.4-fabric-common-mods-inventory | 缓存跳过 PASS | 0.999994284 / 24,845 | 是 | exit=0 |
| 10 | minecraft-1.21.4-fabric-rei-inventory | missing-result outer timeout | rc=124, adb_rc=0 | 未知 | exit=0 |
| 11 | minecraft-1.21.4-fabric-xaero-minimap-in-world | missing-result outer timeout | rc=124, adb_rc=0 | 未知 | exit=0 |
| 12 | minecraft-1.21.4-fabric-xaero-world-map-in-world | PASS | 0.999999866 / 181 | 是 | exit=0 |
| 13 | minecraft-1.21.4-fabric-journeymap-in-world | PASS | 0.999991302 / 51,263 | 是 | exit=0 |
| 14 | minecraft-1.21.4-fabric-modernui-inventory | missing-result outer timeout | 1900s cap; checkpoint cancelled, child rc=130 | 未知 | 未知 |
| 15 | minecraft-1.21.4-fabric-rei-inventory-normal-world | idle timeout | 302.162s | 是 | 未知 |
| 16 | minecraft-1.21.4-fabric-xaero-minimap-in-world-normal-world | idle timeout | 302.244s | 是 | exit=0, sessionsFaulted=1 |
| 17 | minecraft-1.21.4-fabric-xaero-world-map-in-world-normal-world | idle timeout | 328.432s | 是 | 未知 |
| 18 | minecraft-1.21.4-fabric-journeymap-in-world-normal-world | idle timeout | 308.783s | 是 | 未知 |
| 19 | minecraft-1.21.4-fabric-modernui-inventory-normal-world | idle timeout | 376.767s | 是 | 未知 |
| 20 | minecraft-1.21.4-fabric-iris-bsl-in-world | PASS | 0.997109055 / 295,279 | 是 | exit=0 |
| 21 | minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world | PASS | 0.996350744 / 373,304 | 是 | exit=0 |
| 22 | minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world | PASS | 0.990697718 / 204,839 | 是 | exit=0 |
| 23 | minecraft-1.21.4-fabric-iris-sundial-lite-in-world | PASS | 0.996295147 / 402,629 | 是 | exit=0 |
| 24 | minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world | PASS | 0.999697373 / 292,835 | 是 | exit=0 |
| 25 | minecraft-1.21.4-fabric-iris-complementary-unbound-in-world | PASS | 0.999685179 / 299,427 | 是 | exit=0 |
| 26 | minecraft-1.21.4-fabric-iris-mellow-in-world | PASS | 0.999893594 / 142,337 | 是 | exit=0 |
| 27 | minecraft-1.21.4-fabric-iris-nostalgia-in-world | PASS | 0.999396249 / 293,673 | 是 | exit=0 |
| 28 | minecraft-1.21.4-fabric-iris-bliss-in-world | PASS | 0.999113289 / 380,710 | 是 | exit=0 |
| 29 | minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world | PASS | 0.997410148 / 28,716 | 是 | exit=0 |
| 30 | minecraft-1.21.4-fabric-iris-iterationt-in-world | PASS | 0.997988682 / 361,373 | 是 | exit=0 |
| 31 | minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world | PASS | 0.996216918 / 401,106 | 是 | exit=0 |
| 32 | minecraft-1.21.4-fabric-iris-photon-v1.1-in-world | PASS | 0.999391584 / 238,436 | 是 | exit=0 |
| 33 | minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world | VISUAL FAIL | 0.008877075 / 403,024; threshold 0.99 | 是 | exit=0, sessionsFaulted=1 |
| 34 | minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world | VISUAL FAIL | 0.850784981 / 402,598; threshold 0.99 | 是 | exit=0, sessionsFaulted=1 |
| 35 | minecraft-1.21.1-neoforge-create-indirect-in-world | idle timeout | 380.309s | 是 | 未知 |
| 36 | minecraft-1.21.1-neoforge-create-instancing-in-world | PASS | 0.999984504 / 48,423 | 是 | exit=0 |
| 37 | improved-transparency-minecraft-26.3 | idle timeout | 313.152s | 是 | 未知 |
| 38 | minecraft-1.21.4-fabric-iris-bsl-esc-menu-854 | idle timeout | 302.749s | 是 | 未知 |

## 证据位置与注意事项

TCP 原始 runner log、results/checkpoints、diff PNG、client/server logs 和 phone logcat 位于 /home/swung/w7/logs/p7w6-matrix-resume/pass1/；清单在 /home/swung/w7/logs/p7w6-matrix-resume/cases.txt。DirectGLES monolith 对照的 actual/diff PNG、result.json、logcat 和应用日志位于 /home/swung/w7/logs/p7w6-monolith-compare/（`archive/.../repeat-01` 与 `archive-d24/.../repeat-01`）。B4 host gate 与未知子集的 matrix attempts 重叠，timeout 墙钟可能受主机竞争影响；这些超时仍只按各自状态记录。p7w6 APK 早于 M3，后续门 3/CTS 仍须用最终 APK。
