# P7 收官异模型审查（codex，2026-09-23，`6afa077f`）· 集成者分诊

codex 结论：yes-with-fixes。原文附后（`codex-review.md`）。

| # | 发现 | 分诊 | 去向 |
|---|---|---|---|
| 1 | 暂存纹理：对端在已声明的大层末尾写 4 字节，server 按终点 `resize` 零填到整层（16384² RGBA8 ≈ 1 GiB） | **记债，不修**：上限是设备限制下的整层，已认证对端用合法 `glTexStorage` 也能要到同量内存（ID-P7-3 威胁模型 = 未认证远端，已由令牌 + fork 前认证关闭）；稀疏存储是重设计 | CONTRACT-P7 §12 |
| 2 | 越界图像单元共用可写占位：单元 A 的写经 barrier 后可被单元 B 读到（GL 要求写丢、读 0） | **修**：有 `VK_EXT_robustness2` nullDescriptor 时绑空描述符（语义精确），否则每 (binding, unit) 私有占位 | cf-magma |
| 3 | PH-5 按 `sizeof(Element)` 限 count，`Vector<String>` 紧凑编码的合法归档（多个短名 + 空尾向量）被拒 → split 下丢合法程序产物 | **修**（真回归）：按该类型最小编码宽度限 count | cf-core |
| 4 | 门 3 判读信 runner 自报的遍数 / 臂 / 分母，`--repeat 1` 也能 PASS | **修**：判读器硬编码 §7.2 常量（≥3 遍、四臂齐、规范 36 例集合） | cf-reducer |
| 5 | CTS `Pass/(Pass+Fail)` 看不见 Pass→NotSupported 的流失 | **修**：BASE Pass → AFTER 非 Pass/Fail 计入非通过并列出，未裁定即 FAIL；字面公式仍打印 | cf-reducer |
| 6 | 闩：控制线程在 apply 线程每记录检查之后闩住，apply 还会再弹一条；`appliedSeq` 对闩住的记录也前进 | **修前半**（每次 pop 前再查一次）；后半记债 | cf-core / §12 |
| 7 | 深度 resolve 探针判定函数级 static，同进程换物理设备复用旧判定 | **修**：按物理设备身份缓存 | cf-magma |
| — | 跨臂 PNG 不同只报告：需收官时逐例裁定 | 判读器改为「未裁定即 PASS-NEEDS-ADJUDICATION」 | cf-reducer |
| — | 门 1 数字取自 M3、最终树未见完整门 | 最终树 `431bda67`（+ 仅文档到 `6afa077f`）已跑 v2 全门（`~/w7/logs/int2b/gate.log`）；修复轮合入后再跑一次 | 记录 |

---

## codex 原文（`6afa077f`）

## P7 closeout review

**Verdict: yes-with-fixes.** Gates 3 and 5 turning green would not, by themselves, justify closeout at `6afa077f`. The findings below include two peer-facing correctness or resource failures and two ways the window reducer can report green without proving the promised result.

I reviewed the `14e1c8b9..6afa077f` comparison and files fetched at `6afa077f` read-only. I did not build or run tests. The local shell did not return commands, so the source review used the repository connector.

### Findings

1. **Major — a tiny texture upload can allocate a gigabyte in the session child.** [StagedTextureStore.h:673](../../../../MobileGL/MG_Remote/Server/StagedTextureStore.h:673) grows `Bytes` through the *end offset*, while [the new bound](../../../../MobileGL/MG_Remote/Server/StagedTextureStore.h:196) limits dimensions and arithmetic, not staging memory. On a server allowing a 16384×16384 RGBA8 level, a peer can declare that level and send a four-byte slab at its last pixel; the computed end makes `resize` zero-fill about 1 GiB. Repeating this can exhaust memory or kill the child and put the supervisor under system memory pressure. PH-4 raw-peer tests exercise out-of-bound records; this slab is **within** the declared bound.

2. **Major — invalid image stores can become observable through a shared placeholder.** [WirePlaceholderImages.inc:77](../../../../MobileGL/MG_Backend/DirectVulkan/Renderer/WirePlaceholderImages.inc:77) keys writable storage placeholders by format and target, and [line 173](../../../../MobileGL/MG_Backend/DirectVulkan/Renderer/WirePlaceholderImages.inc:173) clears them before descriptor use. Two invalid image units of the same shape therefore bind the same Vulkan image. A shader that stores through invalid unit A and, after an image memory barrier, loads invalid unit B can see A’s value; GL requires the invalid store to be discarded and the load to return zero. The new scenario loads its invalid source **before** storing to its invalid destination, so it cannot catch this alias. CONTRACT-P7 §12 records sharing as a debt; its classification is too mild for this within-dispatch correctness failure.

3. **Major — PH-5 can reject an archive produced by its own encoder.** [ProgramArtifactsCodec.cpp:309](../../../../MobileGL/MG_State/GLState/ProgramState/ProgramArtifactsCodec.cpp:309) requires remaining serialized bytes to cover `count * sizeof(Element)` before decoding every vector. A `Vector<String>` element can serialize as an eight-byte length plus a short name while occupying roughly 32 bytes in the vector. A valid `LinkArtifacts` archive with, for example, ten short [XFB interface names](../../../../MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h:593) and empty later vectors can fail decode solely because its compact wire form is smaller than its in-memory form. The round-trip test uses two longer names; the PH-5 negative controls test malicious counts, not this valid compact case. A split client then loses a valid program artifact that monolith can use.

4. **Major — gate 3 can report PASS without the specified repetitions or control arm.** [60-reduce.py:182](../../../../docs/Disaggregated/notes/p7/device-window-2/tools/60-reduce.py:182) accepts the repeat count written by the runner; [line 251](../../../../docs/Disaggregated/notes/p7/device-window-2/tools/60-reduce.py:251) checks against that count, not three. `30-gate3.sh --repeat 1` can thus yield PASS for 36 cases after one inproc and one spawn image each. [The `RUN_AHEAD=0` arm is optional to the reducer](../../../../docs/Disaggregated/notes/p7/device-window-2/tools/60-reduce.py:279), and [the denominator check](../../../../docs/Disaggregated/notes/p7/device-window-2/tools/60-reduce.py:298) tests only the number 36, not the canonical case set. The default runbook uses the intended inputs, but its green verdict does not prove they were used; reducer self-tests do not enforce these contract constants.

5. **Major — ID-P7-59’s CTS rate can hide loss of supported cases.** [60-reduce.py:410](../../../../docs/Disaggregated/notes/p7/device-window-2/tools/60-reduce.py:410) implements the agreed `Pass/(Pass+Fail)` formula, but [line 525](../../../../docs/Disaggregated/notes/p7/device-window-2/tools/60-reduce.py:525) only warns when `NotSupported` changes. If 123 of the 124 baseline-passing SSBO cases become `NotSupported` and one still passes, the block remains 100% by that formula, has no new crash, and can be marked PASS. This is a flaw in the **recorded gate criterion**, rather than a mismatch between code and contract. The reducer even lists Pass-to-other-status regressions but does not gate on them. The CTS tests cannot catch a loss that their verdict rule explicitly accepts.

6. **Minor — the latch’s “no further record” guarantee has a control/apply race.** [ServerLoop.cpp:673](../../../../MobileGL/MG_Remote/Server/ServerLoop.cpp:673) checks the latch before the batch and [line 711](../../../../MobileGL/MG_Remote/Server/ServerLoop.cpp:711) after applying each record. If the control thread latches a malformed SurfaceOp just after that latter check, the apply thread can pop and execute the next queued command before checking again. Separately, [SessionRings.h:465](../../../../MobileGL/MG_Remote/Transport/SessionRings.h:465) advances `appliedSeq` even when the record’s callback latched a refusal, so a waiter can briefly see that record as applied before the fault reaches it. The batch negative controls latch on the apply path; they do not arrange this cross-thread ordering. The child still exits, limiting the impact to that session.

7. **Minor — the depth-resolve verdict belongs to the first device in a process.** [VulkanRenderer.cpp:15840](../../../../MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:15840) stores a choice derived from `this` device in a function-static value. If inproc tears down a renderer and initializes another physical device or changed driver environment in the same process, it reuses the first verdict and skips the new device’s probe. The current device gate and host controls start with one device, so they do not exercise that lifetime.

### Gate evidence and decisions

The notes substantiate gates 2–7 on their stated host scopes, with **gate 4 achieved by reclassifying 86 still-linked symbols as `# P13`**, not by removing all 101 original Magma references. That deferral is explicit and defensible while the disaggregated monolith reference arm remains required. The closeout should preserve that wording.

Gate 1’s displayed Magma counts in [CURRENT_STAGE_STATUS.md:13](../../../../docs/Disaggregated/CURRENT_STAGE_STATUS.md:13) are from M3. F2 has a later full local gate, while the gate-5 review records targeted tests for subsequent changes. The notes do not establish a full gate run on the final `6afa077f` tree. Gates 3 and 5 are correctly shown as pending.

ID-P7-59’s **report-only cross-arm SHA** rule follows CONTRACT-P7 §7.2, but [the reducer can PASS a stable split-only pixel difference](../../../../docs/Disaggregated/notes/p7/device-window-2/tools/60-reduce.py:284) when its SSIM change stays within 0.0005. Any such mismatch needs explicit image-diff adjudication at closeout; a green summary alone is insufficient.

**Must fix before closing:** bound total staged texture allocation; make invalid image writes truly discard without aliasing loads; preserve valid compact artifact archives; make gate 3 enforce three repeats, the required arms, and the named 36 cases; gate or adjudicate Pass-to-NotSupported CTS regressions; and run the full host gate on the final code after those fixes. Then assess the device-window gates 3 and 5 on that same code.
