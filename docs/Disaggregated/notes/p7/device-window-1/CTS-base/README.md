# CTS `$BASE`（出口门 5）· monolith × DirectVulkan × KHR-GL46，Redmi `2f7cbe2e`

- 库：`/data/local/tmp/mgcts/libMobileGL.so` = p7w1 APK 的 arm64 库，stamp **`p7w1-d260f110`**（wave 0 之前的 monolith Magma；`../00-session/apk.sha256`；`deploy/IDENTITY.txt`）。
- glcts：VK-GL-CTS `0b04c470e`（main），arm64，`DEQP_TARGET=mobilegl` + `tools/cts/patches/0001-fbo-color-texture-attachment.patch`；caselists `tools/cts/caselists/p7-*-gl46.txt`（同一提交切出）。
- 跑法：`run_cts.py --backend DirectVulkan --surface fbo --gl-config-name rgba8888d24s8 --cpu-mask fast --env MOBILEGL_CTS_FBO_COLOR_TEXTURE=1 --chunk-timeout 900 --skip-file tools/cts/caselists/p7-skip.txt`，无 `MOBILEGL_TRANSPORT`（monolith 是 `$BASE` 的定义）。preflight `mgprobe`：PASS，`default_fb=ok user_fbo=ok rbo_fbo=ok`（`preflight.txt`）。API 探针：`KHR-GL46.texture_swizzle.*` 列出 697 例（`probe-gl46.txt`）→ 读数在 **GL 4.6**，AFTER 必须同版本。
- 五块全部 `unrun.txt` / `hung.txt` / `crashed.txt` 为空 → **完整读数**；总耗时 6 分钟（`run-base.log`）。UBO 块（GTF）本 CTS 提交无 kc-cts，不可运行（ID-P7-16）。

| 块 | 例数 | Pass | NotSupported | Fail | conformance（P+NS）| strict（P） |
|---|---:|---:|---:|---:|---:|---:|
| shader-image (`shader_image_load_store` + `shader_image_size`) | 125 | 63 | 56 | 6 | 95.20% | 50.40% |
| ssbo (`shader_storage_buffer_object`) | 124 | 124 | 0 | 0 | 100% | 100% |
| dsa (`direct_state_access`) | 371 | 368 | 0 | 2 | 99.46% | 99.19% |
| texture (`texture_*`) | 1055（1 例在 skip 表） | 815 | 221 | 18 | 98.29% | 77.32% |
| packed-pixels | 4732 | 4732 | 0 | 0 | 100% | 100% |

失败按组：texture_barrier 4/4、texture_cube_map_array 5/27、texture_swizzle 4/697、texture_border_clamp 2/32、texture_buffer 1/16、texture_size_promotion 1/1、texture_view 1/7、shader_image_load_store 6/51、direct_state_access 2/371——逐例名在 `report-*.json`。它们是 **monolith Magma 的既有一致性状态**，不是 P7 的题；门 5 比的是 inproc × DirectVulkan 对这份读数每块 ≤ 0.5 pp 且新 crash = 0。机器可读：`base-DirectVulkan-monolith-p7w1-d260f110.json`（`cts_multi_report.py --json`）。
