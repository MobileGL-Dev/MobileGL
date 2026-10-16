# B4 集成源码审查（2026-09-23）

## Verdict

**land**。审查针对 pipe 在 a4c1bc8b / b19297d5 后的源码；没有遗留正确性问题。

## 源码核对

- MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:13524-13593：WaitForFrameSerial 不再把首个匹配 fence 当成整个 frame serial 已完成。它复制 submit index、等待完整 fence 前缀、在记录被退休后从头重扫，并且只在 IsFrameSerialComplete 为真时返回成功；异常路径 drain graphics queue 后也重新检查完成地板。
- VulkanRenderer.cpp:13678-13694 与 Renderer/SubmitFencePrefix.h:16-38：聚合等待包含目标 submit 及其之前的所有在飞 fence，使用 waitAll=VK_TRUE；缺失目标记录或无效 fence 会拒绝等待，不会用更早的前缀冒充目标完成。只在整段 wait 成功后推进 submit 完成记录。
- VulkanRenderer.cpp:13602-13612：pending-work 判断同时包含主 command buffer 和 pre-pass command buffer 状态，避免把 pre-pass 录制漏出 pending 账本。
- VulkanRenderer.cpp:14272-14282、:14408-14422：在 FrameContext 等待并重置槽 fence、释放退休 command buffer 之前，先等待该槽覆盖的完整 submit 前缀；初次获取没有历史 submit，仍走原路径。
- VulkanRenderer.cpp:7064-7071、:7689-7695：wire draw 与 dispatch 路径都调用纹理垃圾回收；其内部 256 次调用节流保留，避免只在 frame boundary 清理导致长帧记录滞留。
- MobileGL/MG_Remote/Server/StagedTextureStore.h:234-252：extent 改变时清掉旧 bytes 与 coverage，再接收新 run；StagedTextureStoreTest.AdoptRunExtentMoveDoesNotInheritOldCoverage 覆盖旧坐标系内容不可沿用。
- MagmaAggregateWaitTest 通过生产 fence-prefix selector 对照两笔同 serial submit 与单 fence 反事实；单 fence 保留目标帧在飞，生产选择覆盖目标前缀并排除下一帧；另有缺失目标索引拒绝用例。
- MobileGL/MG_Impl/GLXImpl/GLXImpl.cpp:177 的 ::Display* 是仅为 GCC -Wchanges-meaning 编译错误加的类型限定，未改变对象布局或执行逻辑。

## Luna 验收证据

B4 包树的 split 八车道均为 0 失败：unit 2439（2428 pass / 11 skip）、integration split 304（295 / 9）、spawn 220（211 / 9）、TCP 223（214 / 9）、Magma split 102（100 / 2）、spawn 81（79 / 2）、TCP 71（69 / 2）、full split 540（478 / 62）。Verify lanes：1152（865 / 287）与 1086（958 / 128），失败均为 0。G1 .text=0xa52203、符号增删 0/0；fatal census 79/0 未标记；ratchet 173；parity 通过；wire decline audit 53/53、0 未记录或未知；OpenRA inproc/spawn 均 SSIM 1.000000、0 mismatch、0 Fatal、0 unsound serial。

pipe 集成树合并后的完整门另由 Luna 续跑；这些包树结果不替代该集成门。
