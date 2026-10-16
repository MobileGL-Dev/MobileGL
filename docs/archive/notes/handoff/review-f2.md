# review-f2（集成者审查，2026-09-23）

**Verdict: land.** `p7/f2-all@ec845daa`（29 提交 = 基底 4 + f2-event 7 + f2-latch 8 + f2-auth 8 + 合并收尾 2），`~/w7/p7-f2-driver`。

| 片 | 核对 | 结论 |
|---|---|---|
| 基底 PH-2/3/4/5（Luna 审查 land） | 只跑了 49 条定向测试；全车道下 PH-4 两个真缺陷（零尺寸 `glTexImage*` 被判 `ExtentCarrier` 腐坏、`TextureInternalFormat::R8 == 0` 被当「未声明」）+ PH-3 把 ID-49「DstSize≠tight 读 tight」改成拒绝 + PH-2 测试期望 | 由 f2-latch 片 A（`20b827c0`）与空层修复（`58835fa4`）一次修掉；**教训：F2 落地必须全车道 + 主机 CTS 子集** |
| PH-1 (3)(4) 闩 | `FatalFunnel.cpp` 首错胜出（mutex + release store）、未武装＝`SessionFail` 原样；只有 spawn/TCP 会话子进程武装（`ServerMain` RunSession），inproc 保持 Fatal（同进程无 supervisor，理由在 `FatalFunnel.h`）；DrainRing 入口 + 每记录后查闩、apply 线程离环、闩后控制帧不执行、`_exit(75)` | 正确；64 处闩调用 53 行有原始对端负控、8 处论证不可达、`ph_latch_sites.py --self-test` 丢行即红 |
| fuzz 臂 2 | 64 行表驱动 `PeerLatchTest`，每行独立 `--serve` supervisor、会话 A 伪造、会话 B 必须渲染；D11 五界全有行，PH-5 行已改为旧字节界放行、新界拒绝（RLIMIT_AS 红一次） | 正确 |
| PH-6 + fuzz 臂 3 | drop-with-latch、`MOBILEGL_IPC_EVENT_WAIT_MS`（默认 2000，测试用非默认值并回读）、Stop 结束等待、shm 服务端铃有死亡见证、闩后队列不再 drain | 正确 |
| PH-7 (5) + fuzz 臂 1 | supervisor 在 fork **之前**读 Hello 并先验令牌（未认证只回 `Refuse{Authentication}`，不泄漏指纹/stamp）；预认证队列按地址共享 + 置换 + 退避；五种畸形控制帧 + `CtrlEnvelopeBufferHasIdentifier` | 正确；`MOBILEGL_IPC_PREAUTH_{MS,MAX}` / `AUTH_BACKOFF_{AFTER,MS}` 四旋钮已入文档 |
| 门 | G1 `.text` 0xa52203 / 符号 0/0；census 79；棘轮 173；parity 0；unit 2546、split 306、spawn 223、tcp 227、magma 104/83/73、full 542；verify 构建 1156/1090；OpenRA 两臂 1.0 | 全绿 |

**记 §12 的债**：PipeApplier / QueryServer 另有 29 处读对端字节的 `WireProtocolFatal` 仍是 Fatal（会话进程死、supervisor 照常服务下一个）；生成门 `MGPipeWireProtocolFatal` 与 MG_Pipe D11 钩子按设计 Fatal（改闩会碰 pull 构建）；被丢弃事件的记录仍回 `kStatusOk`，client 无 `DroppedEvents()` 消费者。
**协议变化**：`ControlSchemaRevision` 并入 respecify 尺寸载体修订 → wireFingerprint 变，设备 server 随下一个 APK 重部署。
