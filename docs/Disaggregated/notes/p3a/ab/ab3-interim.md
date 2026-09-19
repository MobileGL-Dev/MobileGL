# P3a device A/B - per-thread CPU per frame (ms), best-of-3 repeat, trailing window

| device | case | backend | finish | pull p50 | P2-only (0x7f) p50 | P3a (0x1ff) p50 | d P2 | d P3a total | pull p99 | P3a p99 | pins pull/7f/P3a | mpr 7f/P3a |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 35d0befa | improved-transparency-minecraft-26.3 | DirectGLES | on | 10.789 | - | 11.705 | - | +8.5% | 25.481 | 26.286 | P/P / - / P/P | - / 8 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectGLES | off | 10.810 | - | 11.697 | - | +8.2% | 25.457 | 26.322 | P/P / - / P/P | - / 8 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectVulkan | on | 10.710 | - | 11.486 | - | +7.2% | 25.046 | 26.020 | P/P / - / P/P | - / 0 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectVulkan | off | 10.675 | - | 11.459 | - | +7.3% | 25.014 | 26.035 | P/P / - / P/P | - / 0 |
| 35d0befa | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectGLES | on | 771.937 | - | 769.946 | - | -0.3% | 1540.659 | 1536.544 | U/U / - / U/U | - / 1 |
| 35d0befa | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectGLES | off | 825.877 | - | 775.026 | - | -6.2% | 1648.361 | 1546.747 | U/U / - / U/U | - / 1 |
| 35d0befa | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectVulkan | on | 838.626 | - | 846.970 | - | +1.0% | 1490.291 | 1507.965 | U/U / - / U/U | - / 0 |
| 35d0befa | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectVulkan | off | 839.236 | - | 849.688 | - | +1.2% | 1492.840 | 1511.462 | U/U / - / U/U | - / 0 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | on | 3.546 | - | 2.859 | - | -19.4% | 6.761 | 6.936 | U/U / - / U/U | - / 1 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | off | - | - | 3.828 | - | - | - | 6.945 | P/U / - / U/U | - / 1 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | on | 1.474 | - | 1.486 | - | +0.8% | 3.213 | 3.353 | U/U / - / U/U | - / 0 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | off | 0.565 | - | 0.647 | - | +14.5% | 1.411 | 2.150 | U/U / - / U/U | - / 0 |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | on | 8.241 | - | 10.656 | - | +29.3% | 21.905 | 24.415 | P/P / - / P/P | - / 0 |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | off | 8.220 | - | 10.650 | - | +29.6% | 21.895 | 24.487 | P/P / - / P/P | - / 0 |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | on | - | - | - | - | - | - | - | P/D / - / P/P | - / None |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | off | - | - | - | - | - | - | - | P/D / - / P/P | - / None |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectGLES | on | 9.625 | - | 10.698 | - | +11.1% | 25.806 | 26.407 | P/P / - / P/P | - / 8 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectGLES | off | 9.741 | - | 10.542 | - | +8.2% | 26.312 | 27.022 | P/P / P/P / P/P | None / 8 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectVulkan | on | 8.120 | - | 9.064 | - | +11.6% | 23.126 | 23.899 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectVulkan | off | 8.162 | 9.092 | 8.973 | +11.4% | +9.9% | 22.851 | 23.638 | P/P / P/P / P/P | 0 / 0 |
| 3B159D009VZ00000 | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectGLES | on | 1046.285 | - | 1048.879 | - | +0.2% | 2084.086 | 2089.778 | P/P / - / P/P | - / 1 |
| 3B159D009VZ00000 | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectGLES | off | 1075.081 | 1045.102 | 1060.064 | -2.8% | -1.4% | 2142.660 | 2110.799 | P/P / P/P / P/P | 0 / 1 |
| 3B159D009VZ00000 | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectVulkan | on | 760.555 | - | 776.948 | - | +2.2% | 1506.263 | 1538.494 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectVulkan | off | 758.298 | 731.023 | 760.035 | -3.6% | +0.2% | 1501.555 | 1504.494 | P/P / P/P / P/P | 0 / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | on | 1.732 | - | 1.839 | - | +6.2% | 3.085 | 2.966 | P/P / - / P/P | - / 1 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | off | 1.896 | 1.770 | 1.841 | -6.6% | -2.9% | 2.993 | 3.079 | P/P / P/P / P/P | 0 / 1 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | on | 0.402 | - | 0.457 | - | +13.7% | 1.186 | 1.265 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | off | 0.399 | 0.431 | 0.437 | +8.0% | +9.5% | 1.206 | 1.409 | P/P / P/P / P/P | 0 / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | on | 9.944 | - | 12.852 | - | +29.2% | 27.530 | 29.540 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | off | 9.938 | 11.009 | 12.963 | +10.8% | +30.4% | 27.137 | 29.765 | P/P / P/P / P/P | 0 / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | on | 7.718 | - | 10.226 | - | +32.5% | 24.451 | 27.359 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | off | 8.001 | 8.933 | 10.161 | +11.6% | +27.0% | 24.723 | 27.355 | P/P / P/P / P/P | 0 / 0 |

d P2 = push(0x7f) vs pull; d P3a total = push(0x1ff) vs pull; the P3a-only share is the difference of the two.
A '-' p50 means the run produced no result (rc != 0); see the runner.log in that directory.
