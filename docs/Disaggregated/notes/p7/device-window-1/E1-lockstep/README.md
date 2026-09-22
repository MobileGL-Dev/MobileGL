# E1 · `MOBILEGL_IPC_RUN_AHEAD=0` 的 lockstep 对照

集合 = E0a 的分歧 / 死亡 case（OpenRA、26.3、create-instancing、bsl-esc-menu-854、iterationt）+ 两个对照（`minecraft-1.21.4-in-world`、`sodium-in-world`），DirectVulkan × inproc × pbuffer × 1 遍。

**臂证明**（`arm-proof.txt`，7/7）：每例 `run-ahead ARMED` 计数 **0**，`Config: IPC` 行只有 `run-ahead=0`。这就是 runbook §5 说的「一条在 + 一条不在」；`running lockstep` 这句在 RUN_AHEAD=0 下本来就不打。每例退出码非零是 `--require-inproc` 门要求 `run-ahead=1` 的已知副作用，结果与归档照常可用（`E1.log`）。

| case | ARMED（E0a） | lockstep（E1） | 读法 |
|---|---|---|---|
| OpenRA | 0.976494 | **0.988998** | 仍分歧；幅度变小。= 历史第二个观测值。run-ahead **不是**原因，只影响首个 CapsSnapshot 前丢掉多少发射 |
| 26.3 | SIGSEGV | SIGSEGV | 与 run-ahead 无关 |
| create-instancing | SIGSEGV | SIGSEGV | 同上 |
| bsl-esc-menu-854 | SIGABRT（scudo OOM） | SIGSEGV | 内存上限附近的不同死法 |
| iterationt | `Fatal{UnmigratedVerb,"Magma:mipmap-shader-format-or-shape"}` | 同 | 确定性具名拒绝 |
| 1.21.4-in-world（对照） | 0.9999956 | 0.9999956 | 同 |
| sodium-in-world（对照） | 0.9999938 | 0.9999938 | 同 |

**判别（runbook §5 的那一行）：不变 ⇒ run-ahead 整条线出局，原因在 server 的内容路径 / 能力协商。** E2（credit 扫描）据此不跑。
