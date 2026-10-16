# E4 · SIGSEGV 判别（runbook 之外临时加的两臂）

E0a 发现 26.3 / create-instancing / create-indirect 在 DirectVulkan inproc 下 SIGSEGV（`SEGV_ACCERR`，GL 线程 `__memcpy_aarch64_nt` ← `libtrace_replay_runner.so`，页对齐 fault addr），monolith 通过。两臂判别：

| 臂 | 环境 | 26.3 | create-instancing | 判别 |
|---|---|---|---|---|
| E4a | DirectGLES × inproc × pbuffer | **SIGSEGV** | **SIGSEGV** | 与后端无关 → 不是 Magma 的 |
| E4b | DirectGLES × inproc × `MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0`（Config 行印 `persistent-block=0KiB`，库自己警告这是 E3(a) 负控、persistent-map push 已关） | **SIGSEGV** | — | 与 persistent-map push 无关 → 不是（至少不只是）P5d 的 mprotect 脏页追踪 |

日志：`E4a-gles-inproc.log`、`E4b-gles-inproc-noblock.log`；原始 tombstone / maps 在 `.trace-work/p7w1/{E4a-gles-inproc,E4b-gles-inproc-noblock}/<case>-DirectGLES/repeat-01/logcat.txt`（不入库）。26.3 的 client 日志在崩溃前只有 caps 采纳行，无 `Fatal{`、无 tracker 警告。

判读：client 把一个**不可写**的页交给了应用（替换 `glMapBufferRange` 等返回的指针指向的映射在 client 视角是只读 / PROT_NONE），且这只在 Android 上发生（WSL lavapipe 的 inproc retrace 这几条都绿）——ART 的 sigchain、bionic 的 memcpy 非临时存储路径、零页注册 / shadow 页对齐分配（P5d）都在嫌疑名单上。→ wave 2 **X1**（`notes/p7/PLAN-PH-P34B-P7.md` 之外新增；集成者裁定它进 P7 出口门 3 的关键路径，因为 26.3 在分母里）。
