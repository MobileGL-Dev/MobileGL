# P3a device A/B - per-thread CPU per frame (ms), best-of-3 repeat, trailing window

| device | case | backend | finish | pull p50 | P2-only (0x7f) p50 | P3a (0x1ff) p50 | d P2 | d P3a total | pull p99 | P3a p99 | pins pull/7f/P3a | mpr 7f/P3a |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 35d0befa | improved-transparency-minecraft-26.3 | DirectGLES | on | 10.789 | - | 11.705 | - | +8.5% | 25.481 | 26.286 | P/P / - / P/P | - / 8 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectGLES | off | 10.810 | 11.485 | 11.697 | +6.2% | +8.2% | 25.457 | 26.322 | P/P / P/P / P/P | 0 / 8 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectVulkan | on | 10.710 | - | 11.486 | - | +7.2% | 25.046 | 26.020 | P/P / - / P/P | - / 0 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectVulkan | off | 10.675 | 11.473 | 11.459 | +7.5% | +7.3% | 25.014 | 26.035 | P/P / P/P / P/P | 0 / 0 |
| 35d0befa | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectGLES | on | 942.838 | - | 951.887 | - | +1.0% | 1881.772 | 1899.830 | P/P / - / P/P | - / 1 |
| 35d0befa | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectGLES | off | 944.367 | 945.239 | 949.916 | +0.1% | +0.6% | 1884.954 | 1896.098 | P/P / P/P / P/P | 0 / 1 |
| 35d0befa | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectVulkan | on | 1002.282 | - | 1009.560 | - | +0.7% | 1881.965 | 1896.724 | P/P / - / P/P | - / 0 |
| 35d0befa | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectVulkan | off | 1003.851 | 1019.461 | 1015.625 | +1.6% | +1.2% | 1884.710 | 1908.215 | P/P / P/P / P/P | 0 / 0 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | on | 1.286 | - | 1.389 | - | +8.0% | 2.382 | 2.461 | P/P / - / P/P | - / 1 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | off | 1.292 | 1.349 | 1.382 | +4.4% | +7.0% | 2.418 | 2.443 | P/P / P/P / P/P | 0 / 1 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | on | 0.984 | - | 0.500 | - | -49.2% | 2.106 | 1.165 | P/P / - / P/P | - / 0 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | off | 0.485 | 0.504 | 1.010 | +3.9% | +108.2% | 1.469 | 2.187 | P/P / P/P / P/P | 0 / 0 |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | on | 8.241 | - | 10.656 | - | +29.3% | 21.905 | 24.415 | P/P / - / P/P | - / 0 |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | off | 8.220 | 9.117 | 10.650 | +10.9% | +29.6% | 21.895 | 24.487 | P/P / P/P / P/P | 0 / 0 |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | on | - | - | - | - | - | - | - | P/D / - / P/P | - / None |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | off | - | - | - | - | - | - | - | P/D / P/P / P/P | None / None |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectGLES | on | 9.625 | - | 10.698 | - | +11.1% | 25.806 | 26.407 | P/P / - / P/P | - / 8 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectGLES | off | 9.741 | 10.570 | 10.542 | +8.5% | +8.2% | 26.312 | 27.022 | P/P / P/P / P/P | 0 / 8 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectVulkan | on | 8.120 | - | 9.064 | - | +11.6% | 23.126 | 23.899 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectVulkan | off | 8.162 | 8.920 | 8.973 | +9.3% | +9.9% | 22.851 | 23.638 | P/P / ?/P / P/P | 0 / 0 |
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
