# M3 集成者源码审查（pipe `9fd768d4`，2026-09-23）

**Verdict: land.** 审查对象是集成树源码，不以包 note 代替。`p7/m3` 包提交 `7acad6b8` 已拣为 pipe `9fd768d4`；下述注释措辞的一处小修在本轮文档提交中落地。

1. `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:7065` 与 `:7687` 只在 wire draw / compute 的外层入口触发，发生于本次 draw 的资源准备、descriptor 写入之前。触发条件来自 `MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp:678` 的当前帧槽实际 Acquire 次数；memo 命中不增加此数，因而不会因重复 draw 无谓地强制等待。
2. `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:13580`：尚有当前录制就先 flush；之后 `vkQueueWaitIdle` 覆盖 graphics queue 上所有已提交工作，包括 B4 尚未修复的单 fence 站点可能提前从 tracker 移除的记录；再推进 `through` 并复核无待提交录制。此处在 `m_wirePreparationDepth` 进入前执行，不会回卷本次 draw 尚未录制的 slice。只看 tracker 的聚合 fence 仍有被旧退休站点漏掉记录的风险，因此保守队列等待是本轮必要的完成证明。原注释的“exact submit index”易误读为单 fence 等待，已改为说明 queue idle。
3. `MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp:683` 保留每个 layout 的 pool/set，只回卷 cursor；清 descriptor-reuse、fast-rebind、bind-dedup 与 sampler-resolve memo，旧 set 不会被新 draw 的 memo 当作未改写继续命中。image/buffer view 保留到原帧生命周期，避免提前销毁。`OnDescriptorSetLayoutDestroyed` 的逐 pool 释放规则仍在；活 layout 数量自身的增长不属于此游标界。
4. `MobileGL/MG_IntegrationTest/Scenarios/MagmaWireReclaimScenario.cpp:525` 固定一个 SSBO store/layout，在同一帧切 2049 个对齐窗口，不触发 M2 的 orphan-store 上限；两臂最后像素正确，server `retiredSubmit=2 budget=2048 cached=2048`。仅删 draw 入口回卷调用并重建三目标，两臂都在“2049 distinct descriptor writes ... did not rewind”处红；恢复后绿。`MobileGL/MG_IntegrationTest/CMakeLists.txt:3479` 只把它注册到 split/spawn 私有 server 日志车道，`scripts/ci/spawn_lane_parity.py:91` 对 TCP 的例外是精确 tail。
5. 集成树门：pull G1 `.text=0xa52203`、符号 +0/−0；census 79、棘轮 173、parity 0、审计 53/53/0；unit 2436、split 304、spawn 220、tcp 223、magma 102/81/71、full-split 540、verify 1152/1086，全部 0 红。OpenRA inproc / spawn 1.000000、0 Fatal / 0 unsound。主机 bsl-esc-menu inproc / spawn 都是既有 `ssim=0.998402`、306892 mismatch、0 Fatal / 0 unsound；server 实际发出一次 M3 回卷（frame 0、submit 32、budget 2048、缓存 3555），spawn `wbuf[wlivepk=1052 wdefsync=22]`，与 M2 后的数量级一致。

**记录项：** queue idle 在极长帧每 2048 次 descriptor Acquire 后可能加一次 stall，性能按 CONTRACT-P7 的非门规则记录；固定活 layout 的游标增长已界定，多 layout 的缓存总量可高于 2048（bsl 的 3555 是各 layout 历史缓存），不把 2048 误写成进程全局上限。设备 p7w6 的 bsl 验收针对 M2，M3 的下一版 APK 仍要做设备回归。
