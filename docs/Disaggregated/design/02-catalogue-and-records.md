# 调用目录与记录格式（原 ARCHITECTURE §3–§4）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

## 3. 调用目录

### 3.1 单一真相源

`MobileGL/MG_Pipe/PipeCalls.def`：一行一个 `X(Name, Payload, Class, Flags, WaitClass)`。**线上 opcode 就是行序**，只能追加、退役保留槽位；行数由 `MGP_CALL_LIST_DOCUMENTED_COUNT`（今天 81）与 `PipeCatalogueTest` 钉住。生成器产物提交进树，CI `pipe-gates` 重生成并 `git diff --exit-code`：

| | 产物 | 内容 |
|---|---|---|
| G1 | `PipeTables.inc` | 两张函数指针表 |
| G2 | `PipeThunks.inc` | monolith 直调 thunk |
| G3 | `PipeWire.inc` | wire 记录、尺寸 `static_assert`、applier 边界检查 |
| G4 | `PipeVerify.inc` | `MOBILEGL_PIPE_VERIFY` 逐字段比对器（`PipeFields.def`） |
| G5 | `PipeFilled.inc` | `PipeInputs` 字段 id 与逐 verb 世代 poison |
| G6 | `PipeCoverage.inc` | 后端读点 → 调用映射（`Coverage.def`），0 UNMAPPED 是门 |
| G7 | `PipeSpanTable.inc` | render-state pipeline 子集成员表 |
| G8 | `PipeFieldOwnership.inc` | 每个 `PipeInputs` 字段恰属 `RECORD-SUPPLIED / APPLIER-DERIVED / BARRIER-PULLED / FATAL` 之一（P5f 起 0 BARRIER-PULLED） |

另有 `FillPoints.def`（填充点）与 `DirtySurface.def`（§5.2）。G1 codegen 规则：capability gate 替换指针表达式时保留**表达式形状**，否则 pull `.text` 漂移。

### 3.2 分组与 flag

- Class：`kScreen`（caps、resource、persistent map、fence、`ApplierReset`）、`kCtxQuery`、`kCtxCso`（create/bind/delete × 五种 CSO）、`kCtxState`（`Set*` 状态、`SetContextValues`、`SetProgramBindings`、迁移期的 `SetResidualValueState`）、`kCtxObject`（subdata、readback、mip、`ObjectDeath` …）、`kCtxVerb`（blit、clear、`DrawVbo`、dispatch、XFB、`Present` …）。
- Flags：`kNeedsAck`、`kHasBlob`、`kVarTail`、`kHostSpan`、`kReplySlot`、`kOptional`。第五列 `WaitClass`（P5e）决定 run-ahead 下 client 等不等（§11.6）。
- 20 个 draw 入口塌成 `DrawVbo` 一条；`Clear` 一条判别式；sampler 集合**没有 stage 维度**（192 单元合并空间）；`SetTextureParams` 按资源寻址、与 sampler view 分开（D10）；`SetIndexBuffer` 独立于 VAO 配置（D5）。
- 显式不移植：`GetIntegeri_v` / `GetInteger64i_v` / `GetProgramiv`、`set_pixel_unpack_state`（前端已解析）、压缩格式概念、`pipe_transfer`。

### 3.3 能力位（`MGPCapBit`）

`CallMask` 取代"槽位是否为 null"这一隐式能力探测；`MGPCaps` = `DynamicBackendParameters` + `CallMask` + 格式能力表与 renderer 字符串两个 blob，握手后快照、`MakeCurrent` / `InitCapabilities` 后按 generation 重发布。位包括 viewport array、fp64 顶点、`kCapResidentSubData`、XFB / query 家族、`kCapNeedsHostIndexBytes`、`kCapNeedsHostUboBytes`、`kCapBackendOwnsXfbCapture`、`kCapRunAheadApply`（`MGPipeRunAheadCapBitsFor`：Espryt 自 P5e、Magma 自 P5f 之后的 Magma run-ahead 起发布，后者契约 `MG_Remote/CONTRACT-MAGMA-RUNAHEAD.md`）。**不存在"归属开关"（D-B7）**：multi-draw 分档与 restart 重写永远由 server 拥有，client 只在 caps 要求时提供索引字节。

## 4. 记录与 payload 约定（`MG_Pipe/MGPipeTypes.h`）

- 每个 payload 是平坦 POD、显式 padding、`static_assert` 精确尺寸；**永不含指针**。
- `MGPBlobRef{Offset, Size, Seg}` 指向 blob 区；`MGHostSpan` 是唯一随传输而变的形状（monolith 下是指针，split 下字节在 `SEG_STAGE`），只进变长尾。
- wire 记录头 `MGPWireRecHeader{Op, Flags, Size}`（8 B）；**没有逐记录序号**，seq 就是记录序数。
- 单条记录上界 = ring 一半，**记录本身永不分块**（R-10）；**内容侧分块**：会超出 `SEG_STAGE` 的 blob 按 `MGPipeStageChunkBytes()`（默认 segment/4 = 8 MiB）切成多条记录（buffer 范围走查、纹理整宽 slab，server 拼回整级）。

### 4.1 关键 payload

| payload | 要点 |
|---|---|
| `MGPResourceDesc` | buffer / 纹理 / renderbuffer 一个判别式 create/respecify 形状；`ImageBindableHint`、`ViewOf`（视图的存储属主）、`HasDefinedContent` |
| `MGPRenderStateDesc` / `MGPBindRenderState` / `MGPDynamicState` | §5.3 |
| `MGPVertexElements` | blob 同时带解析后的属性与绑定点视图（记录自洽） |
| `MGPSamplerView` / `MGPTextureParams` | view 只带视图限制；纹理参数挂纹理对象，`BuiltinSampler` 句柄不许为空（D-E1） |
| `MGPProgramDesc` | 逐 stage SPIR-V + 反射归档（§7） |
| `MGPFramebufferState` | 8 color + depth + stencil + client 解析后的 read surface；`ContentHash` 同时是 render-pass memo 键与发射抑制器；`Target` 含 `Named = 3`，applier 按 framebuffer 句柄存 |
| `MGPSubData` / `MGPSubRegion` | §6 |
| `MGPDrawInfo` / `MGPDrawRange` | `DrawVbo`；multi-draw 范围与用户索引在变长尾，indirect 的 `DrawCount` 由 client 解析 |

每条 `kVarTail` 的 `set_*` 带 `ContentHash`，hash 未变就不发（§5.4）。
