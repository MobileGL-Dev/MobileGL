# P4a device A/B - per-thread CPU per frame (ms), best-of-3 repeat, trailing window

| device | case | backend | finish | pull p50 | P3a boundary (0x1ff) p50 | P4a (0x1fff) p50 | d P3a-boundary | d P4a total | pull p99 | P4a p99 | pins pull/1ff/P4a | mpr 1ff/P4a |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2f7cbe2e | improved-transparency-minecraft-26.3 | DirectGLES | on | 10.727 | - | 12.320 | - | +14.9% | 26.805 | 26.972 | P/P / - / P/P | - / 8 |
| 2f7cbe2e | improved-transparency-minecraft-26.3 | DirectGLES | off | 10.716 | 11.754 | 12.124 | +9.7% | +13.1% | 25.297 | 26.841 | P/P / P/P / P/P | 8 / 8 |
| 2f7cbe2e | improved-transparency-minecraft-26.3 | DirectVulkan | on | 10.634 | - | 11.570 | - | +8.8% | 24.975 | 25.958 | P/P / - / P/P | - / 0 |
| 2f7cbe2e | improved-transparency-minecraft-26.3 | DirectVulkan | off | 10.603 | 11.543 | 11.585 | +8.9% | +9.3% | 24.979 | 26.021 | P/P / P/P / P/P | 0 / 0 |
| 2f7cbe2e | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | on | 1.759 | - | 1.839 | - | +4.5% | 322.670 | 320.204 | P/P / - / P/P | - / 2 |
| 2f7cbe2e | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | off | 1.727 | 1.740 | 1.811 | +0.8% | +4.9% | 319.025 | 319.226 | P/P / P/P / P/P | 2 / 2 |
| 2f7cbe2e | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | on | 0.736 | - | 0.788 | - | +7.1% | 455.587 | 462.612 | P/P / - / P/P | - / 0 |
| 2f7cbe2e | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | off | 0.742 | 0.788 | 0.786 | +6.2% | +5.9% | 462.639 | 454.128 | P/P / P/P / P/P | 0 / 0 |
| 2f7cbe2e | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | on | 1.312 | - | 1.458 | - | +11.1% | 2.370 | 2.554 | P/P / - / P/P | - / 1 |
| 2f7cbe2e | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | off | 1.312 | 1.406 | 1.454 | +7.2% | +10.8% | 2.423 | 2.524 | P/P / P/P / P/P | 1 / 1 |
| 2f7cbe2e | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | on | 0.477 | - | 0.504 | - | +5.7% | 1.174 | 1.179 | P/P / - / P/P | - / 0 |
| 2f7cbe2e | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | off | 0.474 | 0.502 | 0.507 | +5.9% | +7.0% | 1.158 | 1.150 | P/P / P/P / P/P | 0 / 0 |
| 2f7cbe2e | minecraft-1.21.4-in-world | DirectGLES | on | 2.343 | - | 2.833 | - | +20.9% | 5.272 | 5.674 | P/P / - / P/P | - / 0 |
| 2f7cbe2e | minecraft-1.21.4-in-world | DirectGLES | off | 2.369 | 2.732 | 2.867 | +15.3% | +21.0% | 5.281 | 5.752 | P/P / P/P / P/P | 0 / 0 |
| 2f7cbe2e | minecraft-1.21.4-in-world | DirectVulkan | on | 1.034 | - | 1.148 | - | +11.0% | 2.304 | 2.405 | P/P / - / P/P | - / 0 |
| 2f7cbe2e | minecraft-1.21.4-in-world | DirectVulkan | off | 1.028 | 1.147 | 1.145 | +11.6% | +11.4% | 2.270 | 2.419 | P/P / P/P / P/P | 0 / 0 |
| 2f7cbe2e | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | on | 8.315 | - | 11.223 | - | +35.0% | 21.901 | 24.904 | P/P / - / P/P | - / 0 |
| 2f7cbe2e | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | off | 8.210 | 10.798 | 11.182 | +31.5% | +36.2% | 21.778 | 24.804 | P/P / P/P / P/P | 0 / 0 |
| 2f7cbe2e | minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | on | - | - | - | - | - | - | - | P/P / - / P/P | - / None |
| 2f7cbe2e | minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | off | - | - | - | - | - | - | - | P/P / P/P / P/P | None / None |

d P3a-boundary = push(0x1ff) vs pull; d P4a total = push(0x1fff) vs pull; the P4a-only share is the difference of the two.
A '-' p50 means the run produced no result (rc != 0); see the runner.log in that directory.
