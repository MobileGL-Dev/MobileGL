# P5f 会话交接（波次 2 已集成）

> 2026-09-19 续跑更新。主 worktree：
> `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`，分支 `feat/disaggregated`。
> 计划：`docs/Disaggregated/P5F-WIRE-COMPLETENESS.md`。

## 0. 三十秒版

**fc / fe / fm 已完成并按序集成。不要再按旧交接的“fe/fm 从未开工”重开。**

| 包 | 状态 / 分支头 |
|---|---|
| f0 | 已合入 `e75e00cb`，八份普查保留 |
| f1 | 已合入 `cfcc12f7` + `418c765c` |
| fc | 已集成 `p5f/fc @ b5c60d5a`，含控制帧复审修复 |
| fe | 已集成 `p5f/fe @ 203f59cc` |
| fm | 已集成 `p5f/fm @ 3cc397d3`；行为代码 `552ade5b` |
| fs / fr / fv | 尚未开工，见 §3 |
| P5f 收口 / 设备 / 异模型族终审 | 尚未完成，P6 继续阻塞 |

验收代码头 `78ec3b67`，其后只补报告/注释。完整证据见
[波次 2 集成报告](wave2-report.md)、[fe 报告](fe-report.md)、[fm 报告](fm-report.md)。

- unit **2286 条零失败**（2276 PASS、10 skip）。
- GPU **1375 条零失败**（1118 PASS、257 既有功能/驱动/专属车道 skip）。
- 普通 / strict split **179 条零失败**（175 PASS、4 skip）。strict 三类 marker 为零。
- 双块 split + magma **210 条：204 PASS、6 skip、0 failed**；Magma 部分 31 条、29 PASS。
- strict 与 dualblock 两份预期集合均已清空，双向棘轮通过；f1 机关开绿/关红实测通过。
- pull/push 构建通过；G1 `.text +0`、symbols 0/0/0/0；G2 名集合相同；G14 新增70、删除0。

这些结论不等于 P5f 收官：字段表仍有13个 BARRIER_PULLED 字段，静态量和反向通道尚有任务。

## 1. 本轮关键变化

fe：Espryt readback、copy、storage-block binding 改读服务端记录；Begin XFB 扩为携带
program handle / lifetime id / 四组 buffer range 的快照。make-current 只清当前绑定，保留
未结束 span，已有 context-values 记录恢复绑定身份。XFB scatter 的身份也随之转为 archive/handle。

fm：原交接“draw/dispatch 全部不可达”的结论不成立。基线 gl_VertexID 无 buffer draw 真正
通过了像素断言，因此本轮补上 server ProgramArchive / uniform / sampler / image 消费和
无应用缓冲 draw/compute。FBO、copy、mipmap、纹理 view 与 staged bytes 全走服务端记录；
独立 family caps 不再借 buffer consumer 位。带应用 buffer 的既有 P7 拒绝保留，不能假宣称已迁移。

fc：除原值帧化外，集成了 wire 序号、void 成功回复、MetalLayer 拒绝、无指针成员机械检查、
并发/重入 probe metadata 的修复。分包报告均有真实 red-once。

## 2. 已解决的集成接缝

- `PipeApply.h` 同时保留 XFB span 状态与 Magma mip 工作字段。
- CopyImageEndpoint / storage-block program 使用双方相同的 handle 定义与 sink 接线。
- `PipeApply.cpp` 同时保留 XFB reset 生存期与无 buffer ops 的纹理 staged 接收/重定义/销毁。
- 棘轮为双方退役结果的交集：fc 后6对155红 → fe 后1对2红 → fm 后0对0红。
- P8 client-array escalation 仍在自己的具名车道。strict 主车道的空表是实测结果，
  不代表 P8 已实现；两条 baseline-stale 行已有 f1 独立证据并已删除。

## 3. 下一步：波次 3

按 ID-137 包边界继续；先重核本轮已改变的站点，不照 f0 旧行号重复施工。

- **fs，静态量/世代**：输入 `f0-statics.md`。`ScopedDefaultUnpackState::s_synced`、
  `g_syncedRenderStateParameters` 同 tuple 而 context 换代、`XfbImpl::g_xfbObjects` 的世代。
  XFB 值内的跨角色身份已随 fe 改道，仍需核静态容器。补核 f1 §8 的 `PipeInputs::IsLive`
  仍读 client LiveContext，决定 server liveness 的清除点。Espryt per-draw memo 已有双键自失效。
- **fr，registry**：输入 `f0-registry.md`。原 resource_copy_region / storage-block 两项入口
  已被 fe/fm 的句柄接线改动，先核剩余 scope；13站中原本11站不可达的 P3b/P4b 债不扩范围。
  继续核 `GetBackendProgramId` 疑似死代码。
- **fv，反向通道**：输入 `f0-reverse-channel.md`。XFB scatter 的 program/target 身份已有
  fe snapshot/archive/handle 路线，核余下边；主要仍是 RecordError 绕 callback table、
  `ResourceTracker.h` 找不到 GPU-written handle 时的 client fallback。字节面不重做。

## 4. 收口门与仍保留的债

1. 双块主车道已硬绿，继续保持两份空棘轮；新增字段 marker 必须失败。
2. `FieldOwnership.def` 的13字段/17行 BARRIER_PULLED 分类与逐帧 rsp 零，仍需逐项收口。
   不可把 legacy 指针 getter 改标 record-supplied 来消表。
3. Magma 不发布 `kCapRunAheadApply`，理由及不由 lockstep 掩盖缺陷的证据在 fm 报告 §4。
4. PackedFloat mipmap 的原基线功能失败保留为 `integration-magma-p7-known-red`；CI 必须
   真正运行唯一用例并核对 `Magma:mipmap-native-format@P7`，不能跳过该检查。
5. G1/G2/G14、逐包 red-once 照旧。文档 strict 引用仍有两处基线 basename 歧义。
6. Redmi `2f7cbe2e` 设备门未执行。本次 adb 只见 `10AG930MXC002B3 unauthorized`。
7. ID-66 的阶段收官异模型族审查尚未做。本轮分包复审不代替它；P6 不开工。

## 5. 环境与复现

- 现有 Windows 包树 `../MobileGL-p5f-{fc,fe,fm}` 均保留；WSL 为 `~/w7/p5f-{fc,fe,fm}`。
  集成验证树 `~/w7/p5f-int`，分支 `codex/p5f-wave2`。不要重建或 reset 参考树 `~/w7/pipe` / `~/w7/base`。
- 门脚本 `~/w7/notes/tools/p5f_gate.sh int <step>`；最终日志 `~/w7/p5f-logs/int-final-*`。
  脚本某些步骤末尾 grep 的退出码不代表 ctest/build 状态，要核真实日志或 JUnit。
- strict / dualblock / GPU 共用私有日志，必须串行。双块旋钮为
  `MOBILEGL_IPC_ROLE_SPLIT_STATE=1 MOBILEGL_IPC_STRICT_ERRORS=1`。
- Windows → WSL 使用 gitdir 的完整 `refs/heads/p5f/...` fetch，并核完整 SHA；先提交再同步。
  build-pull 未跟踪目录是构建产物，勿混入提交。既有 DiligentCore 子模块 gitdir 噪音不修。
- 主 Windows 树原有 `build.gradle`、`android-plugin/app/build.gradle.kts` 两项未提交设备构建
  配置与 `.trace-work/`、`Testing/` 保留；不是本轮产物，不要清理或打包提交。
