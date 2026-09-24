# 后端改造与 server 侧（原 ARCHITECTURE §9–§10）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

## 9. 后端状态机改造

### 9.1 原样不动的东西

Espryt 的 persistent ring、buffer pool、fallback-repack、scratch FBO、驱动绑定影子、Adreno / Mali workaround、SPIRV-Cross 会话、restart 重写与 multi-draw 分档；Magma 的 `VulkanRenderer` memo、`PipelineFactory`、`ProgramFactory`、`UniformManager`、五个 `Vk*Manager`、`SwapchainObject`、D18 的节点式容器纪律。"输入变了、算法不许动"的那一类由 **G5 字节一致门**把关（`scripts/p3a_untouched_regions.sh` 十一函数、`scripts/p4a_untouched_regions.sh` 17 区），re-pin 必须两份同改。

### 9.2 strangler 脚手架：`PipeInputs` + 逐 verb 填充 + poison 世代（P1）

```cpp
// MG_Backend/MGPipe/PipeInputs.h —— 按 memo 键组织，访问器类型与后端原来读到的完全一致
#if MOBILEGL_PIPE_PUSH
#  define MGB_CTX (&::MobileGL::MG_Pipe::gPipeInputs)
#else
#  define MGB_CTX (::MG_State::pGLContext)
#endif
```

三步：A 别名（P1，机械替换 + 逐 verb 类填充点，`nm` 不变）→ B 推送（P2，tracker 填 `gPipeInputs`，verify 构建逐 draw 比对快照版）→ C 句柄化（P3a–P4a、P7，`SharedPtr<前端对象>` → 句柄 + 描述符，写回变回调）。poison 是**逐 verb 世代**：读一个当前 verb 未填的字段是 `Fatal{UnmigratedPipeInput, "<Getter>@<Verb>"}`。

### 9.3 Track V / Track H

Track V（值类型：render state、pixel store、capability 位、标量）是机械迁移；Track H（167 个 `SharedPtr<MG_State…>` 读点）是真活。74% 的读点是翻译输入，所以"bump 一个版本让 server 自己拉"行不通，值本身必须过去。

### 9.4 残余值块

迁移期临时调用 `SetResidualValueState`（`ResidualValueBlock`）：尺寸只降不升（1248 → 8，只剩 `CapabilityBits`），P13 变成 `static_assert(sizeof == 0)`；布局逐成员 `offsetof` 断言；stats 单独计字节。

### 9.5 身份 memo 的重键

`{slot, gen}` + 显式 destroy 让 21 条身份 memo 中 **11 条直接删除**、2 条的去抖搬到 client（§5.4）、7 条重键成更便宜的比较、1 条（D18）不动。统一事实：进 memo 键的版本计数器要么会回绕，要么根本不被它害怕的 mutation bump，身份比较是堵洞的补丁。

### 9.6 A/B 与口径收窄

`MOBILEGL_PIPE_PUSH` 子系统位图在阶段 B 是真 A/B；阶段 C 之后位清零时后端仍跑重键后的代码，所以**编译期** `MOBILEGL_PIPE_LEGACY_MEMOS`（默认 ON）保留 pre-handle 臂，随 pull 路径在 P13 退役。位依赖两侧都拒（客户端族门里**根本不发射**，否则会"客户端已清 dirty、服务端却走旧臂"而丢上传）；没有消费者的后端一条不发。退役的 twin 成员仍在 pull 构建里编译——真删会动 G1。

## 10. server 侧

### 10.1 对象表与 applier

`MG_Remote/Server/PipeApplier`：解码 → 更新对象表与 `PipeInputs` → 调后端。server 不持有 buffer 的完整副本（只有 staged shadow）、不持有前端对象图；任何传输下都不得有 `SharedPtr` 或裸前端指针跨过 applier 边界（P5c 角色守卫、P5e 规则 F）。`InProcessTransport` 与 spawn 走完全相同的编解码路径。

### 10.2 monolith 侧的净收益

即使 IPC 永不上线：复用地址 ABA 一整类不可表达；FBO → program 排序 hazard 消失；`SwapchainObject` 写 `MG_Impl` 的分层倒置消失；`inproc` = 渲染线程。monolith 净代码量是增加的，论据只看逐线程 CPU。

### 10.3 索引宿主镜像（`Server/IndexHostMirror`，P8，待重裁）

设计：`kCapNeedsHostIndexBytes` 下，server 从本来就要收的 `ResourceCreate/Respecify/SubData` 流增量维护 element-array buffer 的宿主镜像，零额外流量；预算 `MOBILEGL_PIPE_INDEX_MIRROR_MB`（默认 64），超预算退化为逐 draw 经 `MGHostSpan` 传送。**P6 的 a6 审计实测宿主索引 span 无 producer、client 索引已是 owned buffer**，所以 P8 要按真实消费者重新裁定，不照抄本节（[`notes/p8/README.md`](../notes/p8/README.md)）。
