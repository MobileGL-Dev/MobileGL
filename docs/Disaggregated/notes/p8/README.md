# P8 — ~~emulation 下放 + 索引宿主镜像 + 协议广度~~ → server 侧仿真缺口与 split 覆盖（进行中，2026-09-29 起）

> **2026-09-29 重定界**（用户批准）：只读核查发现原范围 10 项里 6 项已由前序阶段完成，剩下的缺口都在 server 侧，不是"搬到 client"。计划 [`PLAN-P8.md`](PLAN-P8.md)；裁定 [`INTEGRATOR-DECISIONS-P8.md`](INTEGRATOR-DECISIONS-P8.md)。下面原范围照录并划掉，不删。

## 重定界后的包

| 包 | 内容 | 结果 |
|---|---|---|
| P8-0 | 普查：设备与主机的 trace 语料上各拒绝点 / 仿真谓词的命中数 | ✅ 真实内容只 D 命中；G 不做；create-indirect 门在多帧 trace 上成立（[`S.md`](S.md)，ID-P8-5） |
| A | 覆盖对齐 | ✅ 246 例登上三臂，`split_coverage.py` 进门（[`A.md`](A.md)，ID-P8-7） |
| B | Espryt 生成 mip | ✅ 生成窗口 + RGB16F / RGB32F 在 server 完成，两个 split 名退役（[`B.md`](B.md)，ID-P8-8） |
| C | Espryt server 暂存影子与 GPU 写 | ✅ 基线 21/21 红 → 绿（[`C.md`](C.md)，ID-P8-9） |
| D | Magma wire 臂原生 indirect | ✅ create-indirect 每次整 GPU 等待 321 → 0（[`D.md`](D.md)，ID-P8-10） |
| E | CopyImage / 驱动拒读 | ✅ 前提改正：真实缺口是驱动拒读时会话 Fatal，改由 server store 作答（[`E.md`](E.md)，ID-P8-11） |
| F | 死闩清理与重分类 | ✅（[`F.md`](F.md)，ID-P8-6） |
| G | 大 blob 分片 | 不做（ID-P8-5） |

第一波集成头 `39cd8fb7` 整套门全绿（ID-P8-12）。第二波见 [`PLAN-P8.md`](PLAN-P8.md)。

## 摘要（原文，已划掉）

- ~~monolith 跑道，P7 之后。把读前端字节的纯 CPU 变换下放到 client（`MG_Impl/Pipe/HostResolve.cpp`：最大索引扫描、`*IndirectCount` 解析）；`Server/IndexHostMirror` **待重裁**（a6 实测宿主索引 span 无 producer）；multi-draw client indices、CopyImage 镜像、`generate_mipmap` CPU 回退、`texture-remint-pull` 仿真下放；大 blob carrier 只剩 program archive 与 `draw_vbo` range 尾（与 P6.5 sl 共用分片）。~~
- ~~门：`DirectGLES.Split.*` 与 `DirectGLES.*` 逐名相同；trace split 双后端 SSIM ≥ 0.99；`create-indirect` 上 roundtrips-per-frame 读零。~~

## 阶段表行（原 `ROADMAP.md`，已划掉）

- ~~**阶段**：**P8** emulation 下放 + 索引宿主镜像 + 协议广度~~
- ~~**状态**：待排~~
- ~~**落地什么 / 范围**：`MG_Impl/Pipe/HostResolve.cpp`（client 数组范围已由 `beba0256` 落地为 owned buffer；最大索引扫描、`*IndirectCount` 解析，逐站点 reconcile）；`Server/IndexHostMirror` **待重裁**（树里 0 处；a6 §6 实测宿主索引 span 无 producer、client 索引已是 owned buffer——按 `kCapNeedsHostIndexBytes` 的真实消费者重新裁定，不照抄 `ARCHITECTURE.md` §10.3）；multi-draw client indices（`MultiDrawElements+CLIENT_INDICES` 仍具名拒绝）；CopyImage 镜像搬到 client；viewport-array 回放验证；`generate_mipmap` 计划 + CPU 回退纹素；`texture-remint-pull` 仿真下放（15 处具名拒绝）；大 blob carrier 只剩 **program archive 与 `draw_vbo` range 尾**（a6 §6；与 P6.5 sl 共用同一分片）；`kCapDriverOrderedXfbCapture`（0 处）；无 present fence tick 只记在 P10~~
- ~~**验收门 / 证据**：`'^DirectGLES\.Split\.'` 与 `'^DirectGLES\.'` 逐名相同；trace split 双后端 SSIM ≥ 0.99 含两个 `coherent_as_flush` fixture；`ClientArrayAfterComputeWriteScenario`；`create-indirect` 上 `roundtrips-per-frame` 读零；`index-mirror-bytes` 逐用例发布~~
