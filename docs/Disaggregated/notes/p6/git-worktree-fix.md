# MobileGL-disagg worktree：git 修好报告

> 症状：`git -C MobileGL-disagg status` 直接 `fatal: not a git repository: 3rdparty/DiligentCore/../../../MobileGL/.git/worktrees/MobileGL-p5c/modules/3rdparty/DiligentCore`。
> 修复只动 git 元数据：**没有删库重克隆、没有切分支、没有提交、没有改源码**。
> 日期 2026-09-22，`git version 2.45.2.windows.1`。

## 0 一句话结论

`MobileGL-disagg` 里 **33 个**子模块 checkout 的 `.git` 指针文件全部是**过期指针**（指向已删除的兄弟工作树 `MobileGL-p5c`），
其中 **32 个**直接重定向到磁盘上真实存在的共享 admin 目录即可修复，**1 个（flatbuffers）在任何地方都没有 admin 目录**需要现场补建，
另外**共享 admin 目录里的 `core.worktree` 会盖掉指针文件**，必须一并清掉，否则 `git status` 会把这些子模块全判成"内容已修改"。

现在 `git status`、`git submodule status`、`git add`、`git commit` 在该 worktree 里都能正常工作（见 §5 原始输出）。

## 1 环境事实

| 项 | 值 |
|---|---|
| 本 worktree | `C:\Users\geekerwan\AndroidStudioProjects\FoldCraftLauncher\MobileGL-disagg`，分支 `feat/disaggregated` |
| 本 worktree 的 git dir | `FoldCraftLauncher/.git/modules/MobileGL/worktrees/MobileGL-disagg` |
| MobileGL 项目 git dir | `FoldCraftLauncher/.git/modules/MobileGL`（MobileGL 是 FoldCraftLauncher 的子模块） |
| 子模块共享 admin 根 | `FoldCraftLauncher/.git/modules/MobileGL/modules/` |
| 对照工作树 | `FoldCraftLauncher/MobileGL`（分支 `dev`），git 是好的 |
| 过期指针对应的目标 | `MobileGL/.git/worktrees/MobileGL-p5c/modules/...` —— `MobileGL-p5c` 这个 worktree 目录**已不存在** |

本 worktree 下 `.git` 条目共 **35 个**：**33 个指针文件**（`.git` 是文件）+ **2 个真实 git 目录**
（`3rdparty/glslang/External/spirv-tools/.git` 及其 `external/spirv-headers/.git`，glslang 自带 checkout 的独立克隆）。
**33 个指针文件在修复前全部含 `MobileGL-p5c`**（无一例外，见 `backup.txt`）；
修复后 **33 个全部指向共享 admin 根，含 `MobileGL-p5c` 的为 0 个**。

对照 `MobileGL`（dev）工作树记法是关键依据：它自己的子模块 checkout 是空的（未初始化），
而唯一真实初始化的子模块 `3rdparty/libfork/.git` 写的是

```
gitdir: ../../../.git/modules/MobileGL/modules/3rdparty/libfork
```

即**指向共享 admin 目录**。本次修复采用同一形态。

## 2 过期指针清单与改法

改法：把 `MobileGL/.git/worktrees/MobileGL-p5c/modules/` 整体替换为 `.git/modules/MobileGL/modules/`，
**跳数（`../` 的个数）原样保留**，所以任意嵌套深度都无需逐个特判。

### 2.1 顶层子模块（13 个，改 12 个）

| 路径 | 改后 `gitdir:` |
|---|---|
| `3rdparty/DiligentCore/.git` | `../../../.git/modules/MobileGL/modules/3rdparty/DiligentCore` |
| `3rdparty/SPIRV-Cross/.git` | `../../../.git/modules/MobileGL/modules/3rdparty/SPIRV-Cross` |
| `3rdparty/SPIRV-Reflect/.git` | `../../../.git/modules/MobileGL/modules/3rdparty/SPIRV-Reflect` |
| `3rdparty/Vulkan-Headers/.git` | `../../../.git/modules/MobileGL/modules/3rdparty/Vulkan-Headers` |
| `3rdparty/Vulkan-Utility-Libraries/.git` | `../../../.git/modules/MobileGL/modules/3rdparty/Vulkan-Utility-Libraries` |
| `3rdparty/VulkanMemoryAllocator/.git` | `../../../.git/modules/MobileGL/modules/3rdparty/VulkanMemoryAllocator` |
| `3rdparty/apitrace/.git` | `../../../.git/modules/MobileGL/modules/3rdparty/apitrace` |
| `3rdparty/asio/.git` | `../../../.git/modules/MobileGL/modules/3rdparty/asio` |
| `3rdparty/glslang/.git` | `../../../.git/modules/MobileGL/modules/3rdparty/glslang` |
| `3rdparty/tracy/.git` | `../../../.git/modules/MobileGL/modules/3rdparty/tracy` |
| `include/ska/.git` | `../../../.git/modules/MobileGL/modules/include/ska` |
| `3rdparty/xxHash/.git` | `../../../.git/modules/MobileGL/modules/3rdparty/xxHash`（本会话早期作为单点试验先改好，脚本复查时已正确） |

### 2.2 嵌套子模块（改 20 个）

第三方库自带的子模块，同样是过期指针：

| 组 | 路径 |
|---|---|
| DiligentCore 的 8 个 | `ThirdParty/{SPIRV-Cross,SPIRV-Headers,SPIRV-Tools,Vulkan-Headers,glslang,googletest,volk,xxHash}` |
| SPIRV-Reflect 的 1 个 | `third_party/googletest` |
| apitrace 的 11 个 | `frametrim/tests`、`thirdparty/{brotli,directxmath,gtest,libbacktrace,libpng,snappy,zlib,zstd}`（8 个）、`thirdparty/snappy/third_party/{benchmark,googletest}`（2 个） |

合计 8 + 1 + 11 = **20**。

顶层 12（含 `3rdparty/xxHash`）+ 嵌套 20 = **32 个指针**由本次修复改写（其中脚本一次改 31 个，`xxHash` 是本会话早期手工单点试验改的）；
第 33 个是 `3rdparty/flatbuffers`——它的 admin 目录不存在，需先补建再改写指针，见 §3。

改后形如（以 DiligentCore 的 SPIRV-Cross 为例，跳数 `../../../../../` 原样保留）：

```
gitdir: ../../../../../.git/modules/MobileGL/modules/3rdparty/DiligentCore/modules/ThirdParty/SPIRV-Cross
```

### 2.3 不改的两类

- **`3rdparty/glslang/External/spirv-tools/.git` 与 `.../external/spirv-headers/.git` 是真正的 `.git` 目录**（不是指针文件），
  是 glslang 自带 checkout 的独立克隆，路径相对自身成立，**原样保留**。
- 本 worktree 自己的 `.git` 文件（`worktrees/MobileGL-disagg`）本来就是对的。

## 3 flatbuffers：唯一没有 admin 目录的一个

`3rdparty/flatbuffers` 比较特殊：

- 它是 `feat/disaggregated` 分支**新加**的子模块（`dev` 的 `.gitmodules` 里没有它），index 里有 gitlink `7e163021e59cca4f8e1e35a7c828b5c6b7915953`；
- 磁盘上的内容是在的（`include/flatbuffers/flatbuffers.h` 等都在，版本头 25.12.19 与 pin 一致）；
- 但 `.git` 指针同样指向 `MobileGL-p5c`，而 **`.git/modules/MobileGL/modules/3rdparty/flatbuffers` 不存在**——
  整个 `.git` 树下搜不到任何 flatbuffers 的 admin 目录，也没有对应的逐个 worktree admin。
  也就是说这个子模块从来没在本机被真正 init 过，内容是从别处拷来的。

因为 `git status` 会因为一个死指针整条命令失败，必须把它修到"能解析"的状态。做法：

```
# 用标准 git 机制补建该子模块的 admin 目录（git clone --separate-git-dir，不是手写 .git）
git clone --no-checkout --depth 1 --separate-git-dir=<ADMIN> \
    https://github.com/google/flatbuffers.git <tmp>
git --git-dir=<ADMIN> fetch --unshallow --tags origin     # git submodule update --init 默认非浅克隆，补齐
git --git-dir=<ADMIN> update-ref --no-deref HEAD 7e163021e59cca4f8e1e35a7c828b5c6b7915953
git --git-dir=<ADMIN> read-tree HEAD                      # --no-checkout 不会写 index，这里补上
# 位置选在共享 admin 根，与另外 12 个指针形态一致：
#   FoldCraftLauncher/.git/modules/MobileGL/modules/3rdparty/flatbuffers
printf 'gitdir: ../../../.git/modules/MobileGL/modules/3rdparty/flatbuffers\n' \
    > MobileGL-disagg/3rdparty/flatbuffers/.git
```

补建后：`HEAD` = pin（`git describe` = `v25.12.19`），`index` = 1912 条（与 HEAD tree 一致），`core.worktree` 不设（让指针文件决定工作树路径），
`git rev-parse --show-toplevel` 正确解析到 `MobileGL-disagg/3rdparty/flatbuffers`。
磁盘上的内容文件**一个字节都没动**。

> 注：内容与 pin 的差异只有 6 个 `java/src/test/java/*` 符号链接项——拷来的 checkout 里它们是真实符号链接，
> 而 admin 的 `core.symlinks=false` 使其显示为 ` M`。这与本次指针修复无关，属内容拷贝的既有差异，未处理。

## 4 第二个坑：共享 admin 的 `core.worktree` 会盖掉指针文件

只修指针不够。修完之后 `git status` 能跑了，但 12 个子模块全被标成 ` m`（modified content；当时 12 个是因为 flatbuffers 的 admin 还没补建）。排查结果：

- 共享 admin 目录里带着从主工作树继承来的 `core.worktree`，指向**主 MobileGL 工作树的那个子模块目录**：

  ```
  core.worktree = ../../../../../../MobileGL/3rdparty/DiligentCore
  ```

- 而主工作树的 `MobileGL/3rdparty/*` 是**空壳**（`git -C MobileGL submodule status` 全是 `-<sha>` 未初始化）；
- **`core.worktree` 的优先级高于指针文件**，所以 git 把子模块的工作树解析到那个空目录，于是每个被跟踪文件都"被删除"，
  超项目就把子模块判成 modified content。

处置：把 `core.worktree` 从这些 admin 里删掉，让指针文件决定工作树路径——这正是链接工作树需要的语义
（每个工作树各自解析到自己的目录）。范围严格限定："目标目录存在但为空"或"目标目录不存在"的才删。

- 删除：**32 个 admin 条目**。
  可达 admin 共 34 个（`find` 实测），现在 33 个没有 `core.worktree`、1 个（`3rdparty/libfork`）保留；
  那 33 个里 `3rdparty/flatbuffers` 是本次新补建的、本来就没有这一项，所以**被我删掉的正好 32 个**。
  分解：`3rdparty` 顶层 11 + `include/ska` 1 + DiligentCore 嵌套 8 + SPIRV-Reflect 嵌套 1 + apitrace 嵌套 11 = 32。
- 保留：`3rdparty/libfork`（主工作树里是活 checkout，32 个文件）——它的 `core.worktree` 原样保留
- 回退：`android-plugin/third_party/apitrace` 那棵**孤儿子树**（共 9 个 admin；两个工作树里都没有 `android-plugin/third_party/` 目录，不可达）
  在验证阶段被我的脚本改过，**已按原值逐个还原**，最终与修复前一致。

验证方式（实验先于落地）：在 `/tmp` 里用 `git init` 搭出同构拓扑（top ⊃ MG ⊃ 3rdparty/thing，
再给 MG 开链接工作树、把过期指针和空壳目录都复现），对比"留着 core.worktree"与"删掉 core.worktree"两种处置，
确认后者才能让 `git status` 干净、且 **`git rev-parse --show-toplevel` 落在自己的工作树**里。

## 5 最终输出

### 5.1 逐条目核对：13 个 `.gitmodules` 子模块 + 全部嵌套指针

口径：对 `.gitmodules` 里每一条 `[submodule "..."]`，把该 checkout 的 `.git` 指针按 git 的规则解析
（`gitdir:` 相对的是**装着该 `.git` 文件的目录**，不是 worktree 根——这点是最初误判的根源之一），
再检查解析结果是否真的是一个 git admin 目录（有 `HEAD`），并读出它的 HEAD。

| `.gitmodules` 路径 | 解析结果 | 指向的 admin 目录（相对工作树上级） | admin HEAD |
|---|---|---|---|
| `3rdparty/DiligentCore` | OK | `.git/modules/MobileGL/modules/3rdparty/DiligentCore` | `f36e638805` |
| `3rdparty/SPIRV-Cross` | OK | `…/3rdparty/SPIRV-Cross` | `072444287f` |
| `3rdparty/SPIRV-Reflect` | OK | `…/3rdparty/SPIRV-Reflect` | `10b4f09a24` |
| `3rdparty/Vulkan-Headers` | OK | `…/3rdparty/Vulkan-Headers` | `ad9ce1235e` |
| `3rdparty/Vulkan-Utility-Libraries` | OK | `…/3rdparty/Vulkan-Utility-Libraries` | `738ec97a3f` |
| `3rdparty/VulkanMemoryAllocator` | OK | `…/3rdparty/VulkanMemoryAllocator` | `e722e57c89` |
| `3rdparty/apitrace` | OK | `…/3rdparty/apitrace` | `c8036190fc` |
| `3rdparty/asio` | OK | `…/3rdparty/asio` | `8806a6803c` |
| `3rdparty/flatbuffers` | OK | `…/3rdparty/flatbuffers` | `7e163021e5` |
| `3rdparty/glslang` | OK | `…/3rdparty/glslang` | `d89cf443bc` |
| `3rdparty/tracy` | OK | `…/3rdparty/tracy` | `e6b9ea4609` |
| `3rdparty/xxHash` | OK | `…/3rdparty/xxHash` | `1d7b2a9d21` |
| `include/ska` | OK | `…/include/ska` | `21c1cec95a` |

**13/13 全部 OK；全树 33 个指针文件无一 BROKEN**（另 2 个真实 `.git` 目录照旧，不在指针之列）。

三项交叉核对，全部干净：

1. **pin = admin HEAD = checkout HEAD**：13 条里每一条的 gitlink（index 里记的 sha）、
   admin 目录的 `HEAD`、以及 checkout 内的 `git rev-parse HEAD` **三者完全相同**——
   说明指针既指对了地方，也没把某个子模块的版本弄丢。
2. **index gitlink 集合 == `.gitmodules` path 集合**：两边都是 13 项，差集双向为空
   （没有"是 gitlink 却不在 `.gitmodules`"的，也没有"在 `.gitmodules` 却不是 gitlink"的）。
3. **工作树里的 `.gitmodules` 与分支提交的 `.gitmodules` 逐字节相同**
   （`git show feat/disaggregated:.gitmodules` vs 工作树文件，`diff` 无输出）。

### `git -C MobileGL-disagg status -sb`

```
## feat/disaggregated...origin/feat/disaggregated
 m 3rdparty/DiligentCore
 m 3rdparty/SPIRV-Cross
 m 3rdparty/SPIRV-Reflect
 m 3rdparty/Vulkan-Headers
 m 3rdparty/Vulkan-Utility-Libraries
 m 3rdparty/VulkanMemoryAllocator
 m 3rdparty/apitrace
 m 3rdparty/asio
 m 3rdparty/flatbuffers
 m 3rdparty/glslang
 m 3rdparty/tracy
 M MobileGL/MG_Remote/Wire/PipeWireCodec.cpp
 M MobileGL/MG_Test/Util/PipeStatsTest.cpp
 M MobileGL/MG_Util/Metrics/PipeStats.cpp
 M MobileGL/MG_Util/Metrics/PipeStats.h
 m include/ska
 M tools/device_bench/pin_device.sh
?? .trace-work/
?? Testing/
?? docs/Disaggregated/inproc-dataflow.html
?? docs/Disaggregated/notes/p6/gate7-device-ab.md
?? docs/Disaggregated/notes/p6/gate8-wire-counters.md
?? docs/Disaggregated/notes/p6/git-worktree-fix.md
```

`git status` 退出码 0。工作区里那 4 个 `MobileGL/MG_Util|MG_Test|MG_Remote` 的 `.cpp/.h` 与
`tools/device_bench/pin_device.sh` 是并行代理的 P6 工作产物（见 §7 末尾，**未回滚**）。

那 12 条 ` m` 是**既有 stat 噪声**，不是本修复引入，也不表示内容真被改过——见 §7 第 1 条的完整查证。
最后 3 个 `??` 是并行代理的 `gate7-device-ab.md`、`gate8-wire-counters.md` 与本报告。

### `git -C MobileGL-disagg submodule status`

```
 f36e63880566649d9d8b2336a7c5aa9c8ddb174b 3rdparty/DiligentCore (API256008-78-gf36e63880)
 072444287f4e139c178d6d8fe32e04a0d2c34e8b 3rdparty/SPIRV-Cross (vulkan-sdk-1.4.321.0-49-g07244428)
 10b4f09a24d7ac1603071e767c089551dc6a3949 3rdparty/SPIRV-Reflect (vulkan-sdk-1.4.341.0-2-g10b4f09)
 ad9ce1235e88dc09287e19171dfac384db8ec32c 3rdparty/Vulkan-Headers (v1.4.344)
 738ec97a3f659dd6469bff3c4078ef981b0a343f 3rdparty/Vulkan-Utility-Libraries (v1.4.344)
 e722e57c891a8fbe3cc73ca56c19dd76be242759 3rdparty/VulkanMemoryAllocator (v2.1.0-1021-ge722e57)
 c8036190fcb79ce0d64cf44ab419c54c3307cd71 3rdparty/apitrace (heads/mobilegl/android-trace-replay)
 8806a6803cde7054c3049d3666d3ec36786568c5 3rdparty/asio (asio-1-38-2)
 7e163021e59cca4f8e1e35a7c828b5c6b7915953 3rdparty/flatbuffers (v25.12.19)
 d89cf443bcd220e9205defa0cc4c4b8d8f75dfa2 3rdparty/glslang (11.1.0-1271-gd89cf443)
 e6b9ea460993a6f093283055ef3c98025b19f909 3rdparty/tracy (v0.13.0)
 1d7b2a9d21bf9a740ead540aa93f4bc4caf11ae0 3rdparty/xxHash (v0.7.4-1000-g1d7b2a9)
 21c1cec95abee1beef827e4a7c95f692875d9594 include/ska (heads/master)
```

`git submodule status` 退出码 0，13 条全部有 sha、**没有 `-` 前缀**（未初始化）也没有 `+` 前缀（pin 不匹配）。
修复前这条命令在 `3rdparty/asio` 那一行就因为 flatbuffers 死指针中断了。

### `git add` / `git commit`

```
$ git add MobileGL/MG_Util/Metrics/PipeStats.h      # exit 0
$ git commit --dry-run                             # exit 0
On branch feat/disaggregated
Your branch is up to date with 'origin/feat/disaggregated'.
Changes to be committed:
	modified:   MobileGL/MG_Util/Metrics/PipeStats.h
```

为验证安全性额外查了三件事：

- **暂存不会误 bump gitlink**：在临时 index 上跑 `git add -A`，13 个 gitlink 的 sha 与操作前完全一致（无伪更新）。
- **索引内容零改动**：`git add` + `git reset` 往返后，`git ls-files -s` 与操作前逐行相同（0 行差异，`md5` 也一致）。
- **`git commit --dry-run` 在"没有暂存内容"时退出码本来就是 1**——这是 git 的正常行为（在全新空仓库上复现同样结果），
  不是本 worktree 的问题；一旦有暂存内容即退出码 0（上面那段输出就是证据）。

验证完已 `git reset` 撤销暂存，**没有产生任何提交**。

## 6 回归检查：兄弟工作树与超项目未受影响

| 对象 | 结果 |
|---|---|
| `git -C MobileGL status -sb` | 与修复前**逐字节相同**（12 个未初始化 `-<sha>` 不变） |
| `git -C MobileGL submodule status` | 与修复前**逐字节相同** |
| `git -C FoldCraftLauncher status -sb` | 与修复前**逐字节相同**（含原有的 `M .gitmodules`、`UU FCLauncher.java` 等，均非本次产生） |
| 各仓库 HEAD | `MobileGL` = `fff9d639`、`MobileGL-disagg` = `71aa9951`、超项目 = `6d733f6f`，**均未移动** |

被改到的共享 admin 目录的 **index、HEAD、objects 全部未动**（只删了 `core.worktree` 配置项，以及给 flatbuffers 补建新 admin）。

## 7 遗留 / 已知问题（不粉饰）

1. **` m`（modified content）与 flatbuffers 的 6 个 ` M` 是既有噪声，非本次修复引入。**
   逐项查证：`git status` 报 3109 个文件"已修改"，但（a）`git --no-pager diff --numstat` 的非零内容差异为 **0**，
   （b）用 `GIT_INDEX_FILE=<临时> git read-tree HEAD` 重建全新 index 后降到 **7 个**（那 7 个是 DiligentCore 自己带的嵌套子模块 gitlink，
   它们 pin 与实际 HEAD 相同，只是同样带着这层 stat 噪声），
   （c）拿**修复前的 index 备份**（`/tmp/p6gitfix/idx/…`）跑同一命令，同样报 3109。
   即这些标记来自共享 admin index 里**过期的 stat 缓存**（如 DiligentCore 的 index 记 size 10636、磁盘 10147，mtime 陈旧），
   是这些 checkout 被拷贝/迁移时留下的，`git update-index --refresh` 在这台机器上清不掉。
   它不影响 `status`/`submodule status`/`add`/`commit` 的正确性（见 §5 的 gitlink 与索引零改动验证），
   但会让 `git status` 输出偏噪。**本次未处理**（改 index 超出"修指针"的范围）。

   > **已验证的安全清理办法**（留给后续执行者）：对每个子模块用**临时 index** 试跑 `git add -u`，
   > 实测是**纯 stat 缓存刷新、被跟踪 blob 零改动**。逐个子模块在临时 index 上的效果：
   >
   > | 子模块 | 清理前 | 清理后 | blob 改动 |
   > |---|---:|---:|---:|
   > | `3rdparty/DiligentCore` | 3109 | 7 | 0 |
   > | `3rdparty/SPIRV-Cross` | 4603 | 0 | 0 |
   > | `3rdparty/SPIRV-Reflect` | 276 | 1 | 0 |
   > | `3rdparty/Vulkan-Headers` | 88 | 0 | 0 |
   > | `3rdparty/Vulkan-Utility-Libraries` | 71 | 0 | 0 |
   > | `3rdparty/VulkanMemoryAllocator` | 218 | 0 | 0 |
   > | `3rdparty/apitrace` | 627 | 9 | 0 |
   > | `3rdparty/asio` | 1535 | 1535 | 0 |
   > | `3rdparty/flatbuffers` | 6 | 6 | 0 |
   > | `3rdparty/glslang` | 23 | 0 | 0 |
   > | `3rdparty/tracy` | 382 | 0 | 0 |
   > | `include/ska` | 4 | 0 | 0 |
   >
   > 两个例外清不掉，且都是**既有内容差异**而非缓存问题：`asio` 的 2 个 ` T`（`asio/include`、`asio/src`
   > 是真实符号链接，而上游 pin 是普通文件）和 flatbuffers 的 6 个符号链接项。
   > 真要落地请逐个复查并在报告里说明这些例外。
2. **flatbuffers 的 admin 是我补建的**（原本不存在）。若日后跑标准的
   `git submodule update --init`，git 会另建一份**逐工作树**的 admin
   （`worktrees/MobileGL-disagg/modules/3rdparty/flatbuffers`，共享 objects）并改写指针文件——这是 git 的正常行为，
   两条路径不会冲突；我也把补建的 admin 放在**共享**位置以与另外 12 个指针保持一致。
3. **`3rdparty/glslang/External/spirv-tools` 及其 `spirv-headers` 是独立 `.git` 目录**，
   不受指针机制影响，但它们在 disagg 里也显示 ` M`（43 项，内容差异为 0，同类 stat 噪声），本次未动。
4. 备份留在 `/tmp/p6gitfix/`（`backup.txt` 记全部 35 个 `.git` 条目的原始内容、32 个 admin config、34 个 index、以及 dev/超项目的状态快照），
   本机重启会清掉；需要留档请自行复制。

## 8 边界：本修复没有碰的东西

为了不误伤并行工作，明确列出本修复**未触碰**的部分：

- **没有回滚或修改任何源码**。`MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}`、`MobileGL/MG_Util/Metrics/PipeStatsTest.cpp`、
  `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp`、`tools/device_bench/pin_device.sh` 的工作区改动是并行代理的合法产物，
  本修复只读不写（只在 `git add`/`reset` 往返里短暂进过索引，已验证索引被跟踪内容 md5 与修复前一致）。
- **没有新增/移动任何提交**：`MobileGL-disagg` 仍停在 `71aa9951`，与 `origin/feat/disaggregated` 同步；
  `dev` 工作树仍在 `fff9d639`；超项目仍在 `6d733f6f`。
- **没有改分支**：`feat/disaggregated` 原样。
- **没有动 `3rdparty/*` 里的文件内容**（flatbuffers 的 admin 是建在 `.git/modules/...` 下的新 git 目录，
  不在工作树里；工作树中只改写了 `.git` 指针文件这一行）。
- **没有用 `git submodule update --init`**（那会重新克隆并可能重写指针），全部是元数据级定点修复。

### 回归核对（与修复前快照逐字节比对）

| 对象 | 结果 |
|---|---|
| `git -C MobileGL status -sb`（dev 工作树） | **逐字节相同** |
| `git -C MobileGL submodule status`（dev 工作树） | **逐字节相同**（12 条 `-<sha>` 未变） |
| `git -C FoldCraftLauncher status -sb`（超项目） | **逐字节相同** |
| 共享 admin 的 `index` / `HEAD` / `objects` | 未动（只改 `core.worktree` 配置项 + 新建 flatbuffers admin） |
| `.gitmodules`（工作树 & 分支） | 未动，且二者一致 |
