# persistent map、回读与验证（原 ARCHITECTURE §12–§13）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

## 12. persistent map 与 ≥16 MiB 采纳

`AcquirePersistentMap` 是永久的地址空间捐赠（≥16 MiB 可变 store 自动采纳，MC 26.3 p99 163 → 21 ms），整个 monolith 改造期不动（D-B4）。拆分下三档由 POST 探针选择（spike B 实测）：

| 档 | 形态 | 结论 |
|---|---|---|
| T0 | client 分配 `AHardwareBuffer` BLOB，server 导入（Vulkan / GLES） | 唯一在两台设备、两个后端上都完整读写的档（P11 主攻） |
| T1 | server 导出 opaque fd | 仅 Adreno 的 Vulkan 路径 |
| T2 | 拒绝，client 侧推送 | 永久正确回退；**今天 split 用的档**（`MOBILEGL_IPC_ADOPT_TIER=2`），stream 数据面上强制 |

T2 下 client 侧推送三件套：不做 map/unmap 命令对（payload 带 `hasLiveHostWrites`）；在每个 validate 点按块（`MOBILEGL_IPC_PERSISTENT_BLOCK_KB`，默认 64）推送可达的已映射 buffer，脏页追踪 + 哈希抑制只发变化的块（P5d）；门 `PersistentCoherentMapScenario`。`mpr` 数的是每一次 `MapPersistent` 发射（铸成或拒绝都算），所以在 monolith 下也可断言。

## 13. 回读、roundtrip 清单与验证

### 13.1 稳态零 roundtrip 与不可避免的阻塞点

- 零 roundtrip：全部 draw / clear / blit / copy / dispatch / XFB / bind / CSO / `set_*` / 上传 / `present`；全部 caps 站点；`glGetError` / `glFinish` / `glFlush`；fence 与 query 的创建和非阻塞轮询；`eglSwapBuffers`；restart / multi-draw。
- 不可避免（罕见）：握手；surface 生命周期；`glReadPixels` 到客户内存；GPU-write pending 的 buffer 首次 CPU 读；带超时的 sync / query 结果；`glBufferStorage` 的 ack；纹理拉取；ring / stage 耗尽与 present credit。
- 跨机上每条 `kWaitReply`（`ResourceCreate`、`SetTextureParams`、纹理 `ResourceSubData`）都是一个 RTT：逐帧次数是 P6.5 必测数，减少它是 P9 / P10 的活。

### 13.2 五部分验证门（取代 monolith 的字节一致门）

1. **接口纯度**：A 门 include 图（`scripts/check_include_closure.py`）、B 门 server 镜像不引用 `MG_State::GLState` 与 glslang（P6 的 a6 实测 server 仍链 `MG_Impl`，由 `scripts/link_ratchet.py` 的下行棘轮追踪，P13 转绿）、C 门 `MG_Backend` 里零 `pGLContext`；外加"每个后端 memo 键都是 `{slot, gen}`"（`HandleRecycleScenario`）。
2. **语义影子比对** `MOBILEGL_PIPE_VERIFY=1`：tracker 再用 `SnapshotFromGLContext()` 填一份 `PipeInputs`，G4 逐字段每 draw 比对，打印第一个分歧；verify 构建永不出货，活过 P13。
3. **行为 A/B**：trace 语料在 monolith-pull / monolith-push / split 下 SSIM ≥ 0.99；`DirectGLES.` 与 `DirectGLES.Split.` 等名集合逐名相同（G2）、测试名只增不删（G14）；CTS 逐后端 0.5 pp 内；上传形状金标。
4. **性能**：Redmi `2f7cbe2e` reboot-clean、同热窗口配对 A/B，指标是**逐线程 CPU 时间**；对着 pull 臂**只记录、不设门**（用户 2026-09-08）。
5. **覆盖 + poison + 句柄纪律**：G6 0 UNMAPPED、dirty-surface 0 未映射、G8 归属完备、逐 verb poison、G7、残余块棘轮。

两条幸存的字节级等式：`MOBILEGL_BUILD_DISAGGREGATED=OFF` 时库里零 `MG_Remote` 符号；**G1** pull 构建的符号集与 `.text` 对基线恒等（每阶段 0 增 / 0 删 / 0 resize / 0 重命名）。**每个门必须能因它存在的理由变红**（R-16）：带阴性对照并真跑过一次红；公共 GL 看不见的改白盒断言；拒绝普查逐用例读私有日志（`ctest -V` 是假零）。

### 13.3 长期语义门：MGPipe recorder（P13）

`MG_Test` 的 mock 后端变成 MGPipe recorder，在一组 fixture 上录每 draw 的已推送状态做金标；它只覆盖推送内容，不覆盖后端对它的解释（开放问题 12）。
