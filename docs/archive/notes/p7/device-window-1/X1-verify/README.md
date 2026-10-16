# X1 复验 · persistent-map tracker 在 arm64 上认领自己的 fault

APK `p7w2-ec3ba00a`（`../00-session/apk-p7w2.sha256` = `4b11f647…`；`apk-p7w2-install.txt`），= wave 0 + X1（`4cbf78c2`，`notes/p7/x1-persistent-map-segv.md`）+ CTS skip list。同一 reboot-clean 会话（boot id `18d8d589…`），并排包 `top.mobilegl.plugin.p7w1.trace` 原地升级。DirectVulkan × inproc × pbuffer × 1 遍，外加 26.3 的 DirectGLES 臂：

| case | 后端 | E0a / E4a（修前） | X1 后 | tracker DECLINED |
|---|---|---|---|---|
| `improved-transparency-minecraft-26.3` | DirectVulkan | SIGSEGV | **pass 0.999600026**，逐位确定 | 0 |
| `improved-transparency-minecraft-26.3` | DirectGLES | SIGSEGV（E4a） | **pass 0.999683991** | 0 |
| `minecraft-1.21.1-neoforge-create-instancing-in-world` | DirectVulkan | SIGSEGV | **pass 0.999983621** | 0 |
| `minecraft-1.21.1-neoforge-create-indirect-in-world` | DirectVulkan | SIGSEGV | 0.832130088（不再崩；monolith 0.000005——OQ-17 的 dev 侧问题，仍在分母外） | 0 |

机制（X1 报告）：bionic 的堆指针带 top-byte 标记 `0xb4`，内核交付的 `si_addr` 不带；`PersistentMapTracker.cpp` 的归属判断拿两者比较，在 arm64 上永远不认领 → 每个被它自己 `mprotect` 成只读的页上的写都被交给 debuggerd。修法：两端 `UntagAddress()`、`SlotOwnsFault()` 共用的归属行、注册时不再加保护（map 返回的范围可写，页在首次 push 时才武装）。主机 red-once 用设备的两个地址跑真实谓词。日志：`X1-dv-inproc.log`、`X1-gles-inproc.log`；原始归档 `.trace-work/p7w2/X1-*`（不入库）。

含义：门 3 分母里的 26.3 与 create-instancing 从「死」变「同 monolith」；分母 36 里现在 inproc 对 monolith = **31 同、1 像素分歧（OpenRA → F1）、2 具名拒绝（iterationt ×2 → B）、1 内存上限（bsl-esc-menu，待 spawn 臂复测）**。
