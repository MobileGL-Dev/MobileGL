# wave 2 包 A — Magma 的 UBO / SSBO / texel 对齐与 range 簇

基线 `feat/disaggregated @ 5c4235d7`（pipe HEAD，wave 0 与 X1 已落地）。分支 `p7/magma-a`，
worktree `~/w7/p7-magma-a`。规范：`CONTRACT-P7.md` §0 规则 I / J、§3.2 退役表、§9 的文件分区。

本包只改 `Renderer/UniformManager.{cpp,h}` 与 `Renderer/VkBufferManager.{cpp,h}` 里一个**新增**
函数（`CopyWireBufferSubWordRangeToSlice`）；`VkBufferManager.cpp:~123/~148` 的 resident
sub-data 属包 C，未触及；`WireFramebuffer.inc` / `VulkanRenderer.cpp` 属包 B，未触及。

---

## 1. 逐站点结果

`grep -rn "@P7" Renderer/UniformManager.cpp` 落地前 **8 行 / 8 个站点**（其中 `:1555/:1556`
是一个三元里的两个串），落地后 **1 行 0 个站点**——剩下那行是记录 A.1 退役的注释，不是拒绝。

| 名 | 原站点 | 判定 | 形状 | 可达性 | red-once |
|---|---|---|---|---|---|
| `uniform-buffer-byte-tail` | `:2391` | **退役** | 拷贝窗口按 4 字节外扩进 transient staging，再一次子字 `vkCmdCopyBuffer` 进 padded block；block 仍落在 slice 自身偏移，所以 dynamic offset 保持 `minUniformBufferOffsetAlignment` 对齐 | **可达**（`size` 不受 GL 对齐约束） | 真用例，三臂先红后绿 |
| `uniform-buffer-dynamic-offset` | `:2400` | decline | `MGLOG_E_ONCE` + 跳过该 binding（丢这次 draw，不丢 session） | 不可达（需单帧 4 GiB transient ring） | 强制条件，双臂对照 |
| `uniform-block-native-range` | `:2367` | decline（**钳**） | 钳到 `maxUniformBufferRange`，超出部分在描述符外、robustBufferAccess 读零 | 不可达（`GL_MAX_UNIFORM_BLOCK_SIZE` 即由该 limit 发布） | 强制 limit=16，双臂对照 |
| `storage-buffer-native-range` | `:1557` | decline（**钳**） | 钳到 `maxStorageBufferRange`；monolith 臂根本不查这个 limit | 不可达（同上） | 强制 limit=16，双臂对照 |
| `unaligned-atomic-buffer-range` | `:1552` | decline（具名） | `MGLOG_E_ONCE` + 返回失败，丢这次 dispatch | **可达，且是本簇唯一真缺口** | 真用例，三臂先红后绿 |
| `unaligned-storage-buffer-range` | `:1551` | decline（具名） | 同上（同一分支） | 不可达（前端按同一 limit 校验） | 同上用例覆盖分支 |
| `unaligned-texel-buffer-range` | `:1161` | decline（具名） | 只读 → 占位视图（读零，保住 draw）；可写 imageBuffer → 返回失败 | 不可达（`glTexBufferRange` 按同一 limit 校验） | — |
| `texel-buffer-native-format` | `:1153` | decline | 占位视图，texel fetch 读零，与 monolith 缺视图同观感 | 主机不可达（lavapipe 全支持）；真机可达 | 强制支持检查失败，双臂对照 |

两处**保留具名 Fatal**，都不是应用能绑出来的形状，是算术不变量，各自换了自己的词
（`storage-buffer-offset-overflow` / `texel-buffer-offset-overflow`，不再带 `@P7`）：
`start > SIZE_MAX - slice.offset`。它们经 `WireDescriptorFatal` → `MGPipeSessionFailHook` →
`Session::Fail`，符合规则 I (b)。

## 2. 可达性：为什么「不可达」不是推脱

前端 `MG_Impl/GLImpl` 在 client 侧，wire 臂上照样跑。`ValidateBufferRangeOffsetAndSize`
（`Buffer/GL_Buffer.cpp:1553`）按 `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT` /
`GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT` 校验 offset，`GL_Texture.cpp:2939` 按
`GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT` 校验 `glTexBufferRange`——而这三个 cap 都由
`BackendLoaders/Vulkan/Loader.cpp` 从**同一批** `minUniformBufferOffsetAlignment` /
`minStorageBufferOffsetAlignment` / `minTexelBufferOffsetAlignment` 发布。
`GL_MAX_UNIFORM_BLOCK_SIZE` 同理来自 `maxUniformBufferRange`。所以「不对齐的 offset」和
「超 native range 的 block」在一条健康会话里根本到不了后端。

**唯独原子计数器是个洞。** GL 4.6 core 6.1.1 没给 `GL_ATOMIC_COUNTER_BUFFER` 任何可查询的
offset 对齐 pname，前端因此只查 `offset % 4`（`GL_Buffer.cpp:1597` 的注释自己写了这点）；而
glslang 把每个 `atomic_uint` 降到一个合成 storage block，描述符偏移必须是
`minStorageBufferOffsetAlignment` 的倍数（lavapipe 16，Adreno 64）。
`glBindBufferRange(GL_ATOMIC_COUNTER_BUFFER, 0, buf, 4, 4)` 就是一次合法 GL 绑定、后端表达不出来，
落地前它会带走整个 session。

同理，`uniform-buffer-byte-tail` 的活边界是 **size 不是 offset**：GL 不约束
`glBindBufferRange` 的 size，19 字节是合法绑定而 `19 % 4 != 0`。任务书给的
`offset = alignment + 1` 在公开 API 上打不到（前端记 `GL_INVALID_VALUE`），用例因此改成
`offset = alignment, size = 19`，并把范围外第一个字节下毒，专抓外扩多拷一个字节的错。

## 3. 那个设计决定：可写范围为什么 decline 而不 copyback

`unaligned-{storage,atomic}-buffer-range` 选了 **decline**，理由三条，按分量排序：

1. **这个站点分不出只读和可写。** storage block 的反射里没有 readonly 位（`ProgramFactory.h`
   只有 name 和 index），原子计数器按定义就是写的，而紧接着的
   `MarkWireBufferGpuWritten(range.Res, start, size)` 本来就假设这里任何绑定都可能被写。
   把窗口拷进对齐 transient slice 会**读对、然后悄悄丢掉所有 shader 写**——返回陈旧计数器且
   一声不吭，比丢掉 dispatch 更坏。
2. **copyback 的记录点不在本包。** 正确做法是 draw / dispatch 尾部补一条从 transient slice
   回写到不对齐源的 `vkCmdCopyBuffer`，带 barrier 定序。那个尾部在
   `VulkanRenderer.cpp` 与 `WireDraw.inc` 里，分别属于 wave 2 的包 B 和包 C（契约 §9）。
   `notes/p5f/magma-inproc-fix.md` #5 明确写了 copyback 未实现，本包不越界去实现它。
3. 可达的那一个（原子计数器）**必然可写**，所以「只读走拷贝」的半边对它没有价值。

`unaligned-texel-buffer-range` 的只读半边确实有 `kMGPipeImageAccessReadOnly` 可判，但**也没拷**：
transient arena 的 usage 里没有 `{UNIFORM,STORAGE}_TEXEL_BUFFER`
（`VkBufferManager::InitializeTransientArenas`），在它上面建不出 `VkBufferView`；为一条不可达
路径去加宽一个每帧共享分配的 usage（可能改变内存类型选择、影响所有臂），不划算。

### 3.1 交给包 B / 包 C 的 copyback 配方

要退役可写范围，需要三件东西，都不在本文件里：

- `MG_State` 侧的 storage-block 反射加 `readonly` 位（OQ-8 的 `LinkArtifacts.storageBlocks`
  正好要动同一处，顺带加一个字段即可）——有了它，只读 SSBO 可以直接走「拷进对齐 slice」退役。
- `VkBufferManager` 加 `CopySliceToWireBufferRange(src, srcSkip, res, offset, size)`，形状与本包
  新增的 `CopyWireBufferSubWordRangeToSlice` 镜像（子字方向反过来，同样只在自家 arena 里做
  不对齐那一步）。
- draw / dispatch 尾部（`VulkanRenderer.cpp` / `WireDraw.inc`）在提交前记录这条回写，
  barrier `SHADER_WRITE → TRANSFER_READ`，并把 transient slice 挂到该帧不被回收。

## 4. 新增的那个 helper

`VkBufferManager::CopyWireBufferSubWordRangeToSlice(res, offset, size, frameIndex, dst, dstSkip)`。

对**应用 buffer 的读**按 4 字节外扩，并 clamp 到 store 末尾——所以加宽后的窗口永远碰不到应用
不拥有的字节，这是「外扩」而不是「直接不对齐拷贝」的全部理由。外扩后的那段落进一个 transient
staging slice，只有 staging → dst 那一步是子字的，而它整段在我们自己的 arena 里。
`vkCmdCopyBuffer` 对 region 的 offset / size 没有对齐要求（有要求的是 `vkCmdUpdateBuffer` /
`vkCmdFillBuffer`），所以这一步是一次普通合法拷贝。

为什么**不能**让描述符直接指进外扩窗口内部：`out.dynamicOffset` 进的是
`vkCmdBindDescriptorSets` 的 dynamic offset，必须是 `minUniformBufferOffsetAlignment` 的倍数；
而 head pad 是 0..3。所以 block 的第 0 字节必须**恰好**落在 padded slice 自己的对齐偏移上，
子字位移只能发生在拷贝里。

`BufferArena::Allocate` 可能为了满足 staging 而增长 arena 并换新 `VkBufferObject`；旧的被 park
而不是释放（`EnsureCapacity` 的注释），`dst` 先分配所以仍然有效——两个 slice 允许在不同
`VkBuffer` 里，拷贝各自取自己的 handle。

## 5. red-once（R-16 / 规则 J）

真实用例两条，都在 `integration-magma-buffers` 的三臂 foreach 里注册，spawn / tcp 都跑到：

**A.1 `F1WireScenario.SubWordUniformBufferRangePadsMissingBytes`** — `ShortUniformBufferRange...`
的同胞，range 19 字节。范围外第一个字节置 `0x3F`：若外扩多拷进 block，`tail.x` 读成 0.5f，
红通道回 127 而不是 0。

```
DirectVulkan.{Split,Spawn,Tcp}.Buffers.F1WireScenario.SubWordUniformBufferRangePadsMissingBytes
  before  Subprocess aborted - MGPipe: Fatal{UnmigratedVerb, "Magma:uniform-buffer-byte-tail@P7"}
  after   3/3 Passed
```

**A.4 `F1WireScenario.UnalignedAtomicCounterRangeDeclinesAndTheSessionLives`** — 本包唯一可达缺口。

```
DirectVulkan.{Split,Spawn,Tcp}.Buffers.F1WireScenario.UnalignedAtomicCounterRangeDeclinesAndTheSessionLives
  before  Subprocess aborted - MGPipe: Fatal{UnmigratedVerb, "Magma:unaligned-atomic-buffer-range@P7"}
  after   3/3 Passed；角色日志：
          block 'gl_AtomicCounterBlock_0' is bound at offset 4, which is not a multiple of this
          device's minStorageBufferOffsetAlignment (16); atomic-counter ranges cannot be expressed
          as a descriptor ... so the binding is DECLINED (unaligned-atomic-buffer-range)
```

用例钉的是 decline 的完整可观测：绑定不报 GL 错、dispatch 没跑、计数器四个字节一个没动、
之后 context 还能正常干活。将来真做了 copyback，只需把其中一条 `EXPECT` 改成 `seed[1] + 1`。

不可达的四个站点用**强制条件**对照，两种形状各自编译、背靠背跑，证据在
`~/w7/logs/p7-magma-a/red{2,3,5}.log`：

| 站点 | 强制方式 | before | after |
|---|---|---|---|
| A.2 dynamic-offset | 条件改 `true` | `Fatal{...,"Magma:uniform-buffer-dynamic-offset@P7"}` + abort | `***Failed` 于像素，**无 Fatal 无 abort**，日志有具名 decline |
| A.3 uniform-block-native-range | limit 强制 16（block 32） | `Fatal{...,"Magma:uniform-block-native-range@P7"}` + abort | **双臂 Passed**——钳住后可见字节不变 |
| A.3 storage-buffer-native-range | limit 强制 16（block 24） | `Fatal{...,"Magma:storage-buffer-native-range@P7"}` + abort | `***Failed` 于数据（8 字节确实超出强制 limit），无 Fatal，session 活 |
| A.5 texel-buffer-native-format | 支持检查强制失败 | `Fatal{...,"Magma:texel-buffer-native-format@P7"}` + abort | `***Failed` 于 texel（读到占位零），无 Fatal，session 活 |

每轮末尾都还原并重跑，`grep -c RED-FORCE` 归 0，用例回绿。

## 6. 门

| 门 | 基线 | 落地后 |
|---|---|---|
| `integration-magma-split` | 73（71 PASS + 2 SKIP） | **75/75**（73 PASS + 2 SKIP），新增 2 条用例 |
| `integration-magma-spawn` | 52 | **54/54**（52 + 2 SKIP） |
| `integration-magma-tcp` | 54 | **56/56**（54 + 2 SKIP；parity 归一化后 54） |
| `integration-magma-buffers` | 19 | **21/21** |
| `integration-magma-full-split` | 510 条 / 453 PASS / 57 SKIP | **512 条 / 454 PASS / 57 SKIP / 1 红**（见 §6.2，与本包无关） |
| `ctest -L unit` | 全绿 | **2408/2408** |
| `fatal_census.py` | 79 abort / 20 文件 / 43 家族词 / 3 拒绝词 / 0 无标记 | **一个数都没动，无需重基线**（见 §6.1） |
| `link_ratchet.py --assert-monotone` | 186 | **186，unchanged**，RC=0 |
| `spawn_lane_parity.py` | 通过 | **通过**，RC=0（gated 75/54/54 名集合相等） |
| G1 pull `.text` | `0xa52203` | **`a52203`**，`nm --defined-only` 与 `~/w7/p7-before/pull-syms.txt` 逐符号相同（added=0 removed=0，`SYMBOLS IDENTICAL`） |

G1 不动是结构性的，不是运气：本包所有改动都在 `#if MOBILEGL_BUILD_DISAGGREGATED` 内
（`VkBufferManager.cpp` 的新函数落在 `:167-453` 那个块里），pull 构建看不见它们。

`grep -rn "@P7" MobileGL/ --include=*.{cpp,inc,h}` 从 **15** 降到 **7**——包 A 的 8 个站点全清，
剩下 7 个全在包 B（`WireFramebuffer.inc` ×4、`VulkanRenderer.cpp` ×1）与包 C
（`WireDraw.inc` ×2）的文件里。本包散文里提到旧名时**不带** `@P7` 后缀，免得把注释算成站点。

### 6.1 census 为什么一个数都没动

`abort_sites` 数的是 `std::abort()`。wave 0（`8bb6309a`）已经把三个 Magma wire fatal 漏斗改走
`MGPipeSessionFailHook` → `SessionFail`，`abort` 本身住在 `FatalFunnel.cpp` 这个被 sanction 的
漏斗里。所以**退役一个 `WireDescriptorFatal` 调用点不改变任何 census 数字**，
`--write-baseline` 不需要跑。任务书里「retirements should LOWER abort_sites」的预期来自
wave 0 之前（那时这些站点确实是裸 `abort`）。`FatalFamilies.def` 也不用加行：本包没有引入新的
`Fatal{Word`，两个保留 Fatal 仍走 `UnmigratedVerb`。

### 6.2 `integration-magma-full-split` 的既有红

`DirectVulkan.Split.Full.IterationRPProgram203Scenario.FixedCompleteInputProducesFixedCompleteGoldenOutput`
在本 worktree 的 `build-split` 上**稳定红（3/3）**，且**把本包的后端改动全部还原后照样红**
（`~/w7/logs/p7-magma-a/probe3.log`）——所以不是本包造成的。失败位与
`notes/p7/magma-two-process-first-run.md:155` 记录的 S1 曝光 texel 逐位相同：

```
complete Program 203 output differs at 0,0: actual half bits=(0x357a, 0x4b65)
golden half bits=(0x3a74, 0x4821); mismatched 1 of 262656 texels
```

差别在于契约 §2.5 把这条记在 **tcp** 臂，这里它出现在 **split** 臂。`~/w7/pipe` 自己的
`build-split` 上同一条用例是绿的，而 `~/w7/pipe` 已经走到 `206abdfa`（本 worktree 从
`5c4235d7` 分出），两边 `build-split` 的 configure 也不是同一次生成的。**需要集成者裁定**：
是 5c4235d7→206abdfa 之间被改掉了，还是 S1 本来就与 configure 有关而不只与 tcp 有关。
本包只能确认「与包 A 无关」。

## 7. 还剩什么 / 需要真机

- **真机（我跑不了，用户跑手机）**：
  - `texel-buffer-native-format` 的 decline 在 Adreno 上才有真观感——`GL_RGB32F` 缓冲纹理
    是最可能命中的形状（lavapipe 全支持，主机打不到）。
  - `unaligned-atomic-buffer-range`：Adreno 的 `minStorageBufferOffsetAlignment` 是 64，
    比 lavapipe 的 16 更容易被真实应用打到；decline 之后要确认丢的是 dispatch 而不是整帧。
  - `uniform-buffer-byte-tail` 退役后，Redmi Minecraft 那条活边界应当消失——这是本包最值得
    真机复验的一条（`notes/p5f/magma-inproc-fix.md` #4 的三组对照）。
- **交给包 B / 包 C**：§3.1 的 copyback 配方（可写 SSBO / 原子计数器范围的真正退役）。
- **交给集成者**：§6.2 的 split 臂 IterationRP203 归属。
- 本包**没有**碰 `VkBufferManager.cpp` 的 resident sub-data（包 C）、`WireFramebuffer.inc` 与
  `VulkanRenderer.cpp`（包 B）。
