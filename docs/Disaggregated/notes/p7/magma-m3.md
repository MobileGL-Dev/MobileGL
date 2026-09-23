# P7 M3：长帧内 descriptor set 游标回卷（草稿，待门）

基线：集成树 `0e16ca27`。本包只在 disaggregated 构建的 Magma wire draw / compute 入口触发；pull 的语句与对象代码不变。设备 p7w6 在 M2 后已让 bsl-esc-menu 的 inproc / spawn 各三遍通过，但 M2 的 1,024 store 同步只是偶然给 descriptor 游标制造 drain，不能作为 descriptor set 自己的界。

## 机制

`UniformManager::AcquireDescriptorSet` 每次实际占用一个 layout 游标（从缓存取旧 set 或从 pool 分配新 set）都会增加当前 frame slot 的 `allocatedSetsThisFrame`。达到 2048 次时，下一笔 wire draw / dispatch 在任何新 descriptor 解析前调用 `RewindWireDescriptorSetsIfDue`：

1. 若当前 graphics command buffer 已录制，`FlushPendingCommands` 把它作为新的 `submitIndex` 提交。
2. 取此时的 `m_submitCounter` 为 `through`，等待 graphics queue idle 后才 `OnSubmitsCompletedUpTo(through)`；当前 B4 未落地，已有单 fence 退休站点可能从 tracker 中提前移除一条提交，单靠遍历 tracker 的 fence 不足以证明安全。队列 idle 覆盖这些已移除记录，并显式核对 `m_completedSubmitCounter >= through` 与没有尚未提交的当前录制。B4 全面修正聚合等待后可以再评估这笔保守等待的成本。
3. `UniformManager::RewindWireDescriptorSets` 将每个仍存活 layout 的缓存 cursor 置零，保留 pool 和 set 供改写，清空 descriptor-reuse / fast-rebind / bind-dedup / sampler-resolve memo，并重置本 epoch 的计数。image/buffer view 不在此销毁，仍由原 frame 生命周期管理。

因此一个固定活 layout 的长帧不会因 draw 次数持续分配新 descriptor set。多个 layout 的缓存仍保存各自历史高水位；layout 销毁时沿用 `OnDescriptorSetLayoutDestroyed` 的逐 pool 释放。该界不替代 program/layout 数量本身的生命周期管理。

## 负控与门

`MagmaWireReclaimScenario.DescriptorSetsRewindInsideOneLongFrameAfterTheirSubmitRetires` 用一个 SSBO store、一个 program/layout，在一帧内把 2049 个对齐窗口逐个绑定后 draw，M2 的 orphan-store 上限不会触发。最后一个窗口颜色必须正确；server 日志须给出 submit-index 退休后的回卷，回卷时缓存数不超过 2080。禁用入口的回卷调用应红（无回卷行），恢复后 split / spawn 两臂绿。之后跑 G1 `.text` / 符号恒等、普查、棘轮、parity、八车道、OpenRA 回放与 verify 构建。性能只记录。

当前状态：`~/w7/p7-m3` 的完整 `MobileGL` / `MobileGLServer` / `MobileGLIntegrationTest` 构建通过；新用例 split 与 spawn 2/2 绿（各约 1.2 s，server 行均为 `retiredSubmit=2 budget=2048 cached=2048`）。红一次：仅删 draw 入口回卷调用，三目标重建后两臂都在「2049 distinct descriptor writes in one frame did not rewind」处红，像素断言没有先抢红；恢复源码与三目标重建后两臂再绿。原始日志 `~/w7/logs/p7m3/{green-test,red-test,reclaim-test}.log`。最终源码改用 graphics queue idle 作完整提交证明后，完整目标重建并再次全绿：pull G1 `.text=0xa52203`、符号 +0/−0；fatal census 79、棘轮 173、parity 0、wire-declines 审计 53/53/0 与自测 16/16；split 构建 unit 2436、integration-split 304、spawn 220、tcp 223、magma 102/81/71、full-split 540；verify 构建 unit 2436、integration-verify 1152、integration-verify-split 1086；OpenRA DirectVulkan inproc 与 spawn 两臂均 ssim 1.000000、0 `unsound-serial-complete`。门日志在 `~/w7/logs/p7m3/` 与 `~/w7/logs/p7-gate-m3-final-retrace/`。集成者在 pipe 源码上的审查、落地及设备回归仍待做。
