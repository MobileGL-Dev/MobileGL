# review-ratchet88（集成者审查，2026-09-23）

**Verdict: land.** p7/ratchet88 `6a488d90`（产品 + 基线）/ `989c53fb`（CONTRACT-P7 §4.1/§12）。

| 项 | 核对 | 结论 |
|---|---|---|
| 退役 `MGPipeTextureLegacyArmScope` | `VkTextureManager.cpp` SyncTexture 只在 monolith 臂被调用（唯一调用者 `SyncTextureAndGetDescriptor`）；split 臂同步走 `SyncTextureResourceByHandle`；探针 + 检查者 78 行 split 子集 retrace 0 命中 | 正确；回归现由 `Fatal{RoleViolation,"texture-legacy-arm"}` 按访问器名拦截 |
| G1 | 替换注释与原块同行数（`__LINE__` 进 pull `.text`）；`.text` sha256 与基线相同、符号 30570/30570 | 不动 |
| 余 86 标 `# P13` | disaggregated 构建里 monolith 是默认运行时且是门 3 / 门 5 的参照臂（`ConfigLoader.cpp:322-330`、`test.yml:1531`、`CMakeLists.txt:805-811`）；改用 wire placeholder 会改变参照臂可观测行为 | 接受；**新裁定 ID-P7-55 取代 ID-P7-31 的「不标 # P13、归 wave 3」**，§4.2 加注 |
| 措辞 | 4 个 MGB_CTX 符号里 `PipeInputs::HasOpenTransformFeedbackSpan` / `InvalidateCompileEnv` 不在 monolith draw 路径，计入原因是 `PipeFill.cpp` 按路径规则归 FRONTEND（同 5 个 SlotAllocator 符号） | 集成文档更正 |
| 缺口 | 退役后无常驻负控证明 apply 线程 `SyncTexture` 会按名 abort（只有临时探针 + 棘轮 red-once） | 记 §12 债 |
