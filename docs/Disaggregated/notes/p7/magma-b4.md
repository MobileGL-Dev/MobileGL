# P7 B4：Magma 提交等待与长帧清理（草稿，未落地）

基线：`0e16ca27`，独立 worktree `~/w7/p7-b4`。本包仍在实现／审查，以下是当前源码状态，不是集成树结论。

## 提交与 frame serial

`WaitForSubmitsUpTo(index, timeout)` 收集 tracker 中所有 `submitIndex <= index` 的在飞 fence，以 `vkWaitForFences(..., VK_TRUE)` 等全部信号之后才推进完成计数。`WaitForSubmitIndex` 不再拿一条较晚 fence 代替整个前缀。`WaitForFrameSerial` 等待第一条匹配记录后重查地板；若同一 serial 仍被第二条提交持有，就从头重扫（不能在 `OnSubmitsCompletedUpTo` erase 后继续旧迭代器，也不再读 `record` 引用）。没有可用记录时才 queue idle，最终只按 `IsFrameSerialComplete` 答 true。

`Present` 在 `FrameContext::WaitAndAcquireNextImage` 重置槽 fence / 释放退休命令缓冲之前，先等该槽 `lastSubmitIndex` 的完整前缀；suspended 后首次 acquire 同样做。readback 原本已有全 fence 等待，保持；mid-frame flush 的退化重用分支改走聚合等待。`HasPendingRecordedWork` 在 disagg 构建下也计 pre-pass 录制，`FlushPendingCommands` 的空批判断对应扩展。这样 `TryDrainFrameTransients` 的 wire `CollectWireObjects(all=true)` 不会在 pre-pass 尚未提交时把未来标签视作空闲。pull 分支的可执行语句保持原样。

`FlushWirePendingCommandsForTextureUpdate` 在提交当前录制后等所有在飞 graphics 提交，才允许新的独立上传批修改旧 draw 可能采样的图像。这里可能有明显的 stall 代价，按规则只记录性能，不设门；需在长 trace 上实测。该修正还没有针对单 fence 错退的确定性负控，当前包不得以现有绿数直接落地。

## 长帧回收与 staged bytes

wire draw / dispatch 入口现在调用 `VkTextureManager::CollectGarbage`，使每 256 次的 `pruneWire` 能在一帧内执行。`StagedTextureStore::AdoptRun` 遇到 extent 变化时清旧 `Bytes` 与 `Covered`：单测先送旧 [0,16)，再送新 extent 的 [16,32)，不能把两种坐标系拼成已覆盖。禁用清理时该单测在旧范围误判覆盖处红，恢复后绿。

## 已验证／待做

完整 `MobileGL`、`MobileGLServer`、`MobileGLIntegrationTest` 与 `StagedTextureStoreTest` 构建通过；新 extent 单测绿，红一次按上述覆盖断言红；Magma split/spawn/tcp 为 101/80/71、full-split 539，全绿；OpenRA DirectVulkan inproc / spawn 都 ssim 1.000000、0 Fatal、0 `unsound-serial-complete`。原始日志 `~/w7/logs/p7b4/`、`~/w7/logs/p7-gate-b4a-retrace/`。

待做：聚合等待的可证伪负控；G1 / 普查 / 棘轮 / parity / 完整八车道 / verify 门；集成者对 pipe 源码复审；与 M3 的 `VulkanRenderer.cpp` 冲突合并；真机回归。未完成前不推送本包。
