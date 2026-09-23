# P7 B4：Magma 提交等待与长帧清理（已落地）

包分支基于 M3；集成提交为 a4c1bc8b（B4 实现）和 b19297d5（聚合等待负控与 build compatibility）。集成者源码审查为 land，见 review-b4.md。

## 修复

- DirectVulkan disaggregated 路径新增 WaitForSubmitsUpTo：从在飞提交中收集到目标 submit 为止的完整 fence 前缀，waitAll 等待成功后才推进完成计数。缺失目标索引或空 fence 会失败，不会用更早的前缀冒充完成。
- WaitForFrameSerial 在等待后检查 IsFrameSerialComplete 并从头重扫；不能完整证明时才 queue idle，最后仍重查完成地板。Present 在 FrameContext 释放退休命令缓冲、重置槽 fence 前等待该槽的完整提交前缀。pending-work 判断纳入 pre-pass command buffer。
- FlushWirePendingCommandsForTextureUpdate 先提交当前录制，再等待 graphics submit 前缀，保证旧纹理采样/保存任务先于独立上传。
- Wire draw/dispatch 每 256 次调用触发纹理缓存 prune；StagedTextureStore 在 AdoptRun 遇到 extent 改变时清旧 bytes 和 coverage，不混合不同 level 坐标系。
- GLXImpl.cpp 中 XDisplay 的类型拼写限定为 ::Display，规避 GCC -Wchanges-meaning 编译错误；这是编译兼容改动，不改变对象布局或执行逻辑。

## 负控与审查

MagmaAggregateWaitTest 与生产 fence-prefix selector 共用实现。单 fence 反事实只退休同 serial 的第一笔提交，目标 frame 仍在飞；聚合选择器覆盖目标前缀并排除下一 frame。缺失目标索引控制拒绝早期前缀。两个 focused tests 通过；AdoptRun extent 单测 red-once 记录在包原始日志中。

## 完整 pipe 门（HEAD b19297d5）

G1 pull .text=0xa52203，符号新增/删除 0/0；fatal census 79 个站点、0 未标记；ratchet 173；parity 通过；wire decline audit 53/53、0 unlogged/unknown、自测 16/16。八条 split lanes 均 0 失败：unit 2439（11 skip）、integration-split 304（9 skip）、spawn 220、tcp 223、Magma split/spawn/tcp 102/81/71、full-split 540。Verify：unit 2439、integration-verify 1152、integration-verify-split 1086，0 失败。OpenRA DirectVulkan inproc/spawn 均 SSIM 1.000000、0 mismatch、0 Fatal、0 unsound.

主机完整门日志：~/w7/logs/p7b4-integration-b19297d5-rerun-20260923/。graphics fence 全前缀等待可能增加同步等待时间；性能只记录，不设门。
