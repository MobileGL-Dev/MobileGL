# P8 — emulation 下放 + 索引宿主镜像 + 协议广度（待排）

> 尚未开工。本页保存该阶段在路线图上的完整范围与出口门（2026-09-24 从 [`ROADMAP.md`](../../ROADMAP.md) 阶段表移入，原文照录）；阶段开工后，计划、裁定与报告都放在本目录。

## 摘要

- monolith 跑道，P7 之后。把读前端字节的纯 CPU 变换下放到 client（`MG_Impl/Pipe/HostResolve.cpp`：最大索引扫描、`*IndirectCount` 解析）；`Server/IndexHostMirror` **待重裁**（a6 实测宿主索引 span 无 producer）；multi-draw client indices、CopyImage 镜像、`generate_mipmap` CPU 回退、`texture-remint-pull` 仿真下放；大 blob carrier 只剩 program archive 与 `draw_vbo` range 尾（与 P6.5 sl 共用分片）。
- 门：`DirectGLES.Split.*` 与 `DirectGLES.*` 逐名相同；trace split 双后端 SSIM ≥ 0.99；`create-indirect` 上 roundtrips-per-frame 读零。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P8** emulation 下放 + 索引宿主镜像 + 协议广度
- **状态**：待排
- **落地什么 / 范围**：`MG_Impl/Pipe/HostResolve.cpp`（~~client 数组范围~~ 已由 `beba0256` 落地为 owned buffer；最大索引扫描、`*IndirectCount` 解析，逐站点 reconcile）；`Server/IndexHostMirror` **待重裁**（树里 0 处；a6 §6 实测宿主索引 span 无 producer、client 索引已是 owned buffer——按 `kCapNeedsHostIndexBytes` 的真实消费者重新裁定，不照抄 `ARCHITECTURE.md` §10.3）；multi-draw client indices（`MultiDrawElements+CLIENT_INDICES` 仍具名拒绝）；CopyImage 镜像搬到 client；viewport-array 回放验证；`generate_mipmap` 计划 + CPU 回退纹素；`texture-remint-pull` 仿真下放（15 处具名拒绝）；大 blob carrier 只剩 **program archive 与 `draw_vbo` range 尾**（a6 §6；与 P6.5 sl 共用同一分片）；`kCapDriverOrderedXfbCapture`（0 处）；~~无 present fence tick~~ 只记在 P10
- **验收门 / 证据**：`'^DirectGLES\.Split\.'` 与 `'^DirectGLES\.'` 逐名相同；trace split 双后端 SSIM ≥ 0.99 含两个 `coherent_as_flush` fixture；`ClientArrayAfterComputeWriteScenario`；`create-indirect` 上 `roundtrips-per-frame` 读零；`index-mirror-bytes` 逐用例发布
