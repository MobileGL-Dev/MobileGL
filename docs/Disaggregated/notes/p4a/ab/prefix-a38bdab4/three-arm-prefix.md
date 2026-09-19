# P4a device A/B - per-thread CPU per frame (ms), best-of-3 repeat, trailing window

| device | case | backend | finish | pull p50 | P3a boundary (0x1ff) p50 | P4a (0x1fff) p50 | d P3a-boundary | d P4a total | pull p99 | P4a p99 | pins pull/1ff/P4a | mpr 1ff/P4a |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 35d0befa | improved-transparency-minecraft-26.3 | DirectGLES | on | 10.797 | - | 12.363 | - | +14.5% | 25.557 | 27.004 | P/P / - / P/P | - / 8 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectGLES | off | 10.759 | 11.857 | 12.431 | +10.2% | +15.5% | 25.406 | 27.013 | P/P / P/P / P/P | 8 / 8 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectVulkan | on | 10.729 | - | 11.591 | - | +8.0% | 24.924 | 25.976 | P/P / - / P/P | - / 0 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectVulkan | off | 10.671 | 11.526 | 11.583 | +8.0% | +8.5% | 25.034 | 26.117 | P/P / P/P / P/P | 0 / 0 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | on | 1.702 | - | 1.346 | - | -20.9% | 315.989 | 316.342 | P/P / - / P/P | - / 2 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | off | 1.304 | 1.861 | 1.946 | +42.7% | +49.2% | 317.280 | 317.310 | P/P / P/P / P/P | 2 / 2 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | on | 0.722 | - | 0.817 | - | +13.2% | 459.039 | 447.908 | P/P / - / P/P | - / 0 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | off | 0.763 | 0.784 | 0.794 | +2.8% | +4.1% | 453.873 | 451.470 | P/P / P/P / P/P | 0 / 0 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | on | 1.285 | - | 1.446 | - | +12.5% | 2.436 | 2.715 | P/P / - / P/P | - / 1 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | off | 1.380 | 1.398 | 1.440 | +1.3% | +4.3% | 2.455 | 2.493 | P/P / P/P / P/P | 1 / 1 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | on | 0.961 | - | 1.037 | - | +7.9% | 2.086 | 2.097 | P/P / - / P/P | - / 0 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | off | 0.944 | 1.010 | 0.522 | +7.0% | -44.7% | 1.992 | 1.337 | P/P / P/P / P/P | 0 / 0 |
| 35d0befa | minecraft-1.21.4-in-world | DirectGLES | on | 2.362 | - | 2.853 | - | +20.8% | 5.184 | 5.683 | P/P / - / P/P | - / 0 |
| 35d0befa | minecraft-1.21.4-in-world | DirectGLES | off | 2.341 | 2.724 | 2.841 | +16.4% | +21.4% | 5.183 | 5.633 | P/P / P/P / P/P | 0 / 0 |
| 35d0befa | minecraft-1.21.4-in-world | DirectVulkan | on | 1.966 | - | 1.802 | - | -8.3% | 4.256 | 4.369 | P/P / - / P/P | - / 0 |
| 35d0befa | minecraft-1.21.4-in-world | DirectVulkan | off | 1.050 | 1.154 | 1.735 | +9.9% | +65.2% | 2.363 | 5.902 | P/P / P/P / P/P | 0 / 0 |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | on | 8.221 | - | 11.253 | - | +36.9% | 23.081 | 24.987 | P/P / - / P/P | - / 0 |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | off | 8.236 | 10.840 | 11.204 | +31.6% | +36.0% | 21.918 | 24.882 | P/P / P/P / P/P | 0 / 0 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectGLES | on | 9.733 | - | 11.230 | - | +15.4% | 26.133 | 28.727 | P/P / - / P/P | - / 8 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectGLES | off | 9.819 | 10.660 | 11.224 | +8.6% | +14.3% | 26.154 | 26.980 | P/P / P/P / P/P | 8 / 8 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectVulkan | on | 8.165 | - | 9.267 | - | +13.5% | 22.985 | 23.979 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectVulkan | off | 8.267 | 9.236 | 9.189 | +11.7% | +11.2% | 22.852 | 23.921 | P/P / P/P / P/P | 0 / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | on | 2.205 | - | 2.550 | - | +15.6% | 233.721 | 230.771 | P/P / - / P/P | - / 2 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | off | 2.215 | 2.407 | 2.502 | +8.7% | +13.0% | 230.013 | 230.830 | P/P / P/P / P/P | 2 / 2 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | on | 0.641 | - | 0.649 | - | +1.2% | 262.991 | 266.935 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | off | 0.644 | 0.697 | 0.707 | +8.2% | +9.8% | 263.017 | 263.919 | P/P / P/P / P/P | 0 / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | on | 1.724 | - | 1.951 | - | +13.2% | 3.170 | 3.177 | P/P / - / P/P | - / 1 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | off | 1.697 | 1.835 | 1.917 | +8.1% | +13.0% | 2.927 | 3.116 | P/P / P/P / P/P | 1 / 1 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | on | 0.417 | - | 0.446 | - | +7.0% | 1.126 | 1.152 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | off | 0.430 | 0.474 | 0.447 | +10.2% | +4.0% | 1.221 | 1.135 | P/P / P/P / P/P | 0 / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-in-world | DirectGLES | on | 2.957 | - | 3.579 | - | +21.0% | 4.878 | 5.638 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-in-world | DirectGLES | off | 2.892 | 3.365 | 3.521 | +16.4% | +21.7% | 4.925 | 5.772 | P/P / P/P / P/P | 0 / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-in-world | DirectVulkan | on | 0.898 | - | 1.035 | - | +15.3% | 2.065 | 2.234 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-in-world | DirectVulkan | off | 0.893 | 1.029 | 1.040 | +15.2% | +16.5% | 2.050 | 2.215 | P/P / P/P / P/P | 0 / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | on | 9.863 | - | 15.390 | - | +56.0% | 27.202 | 29.596 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | off | 10.027 | 13.003 | 12.967 | +29.7% | +29.3% | 27.715 | 28.130 | P/P / P/P / P/P | 0 / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | on | 7.681 | - | 10.662 | - | +38.8% | 23.912 | 28.533 | P/P / - / P/P | - / 0 |
| 3B159D009VZ00000 | minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | off | 7.891 | 9.959 | 10.415 | +26.2% | +32.0% | 25.083 | 28.348 | P/P / P/P / P/P | 0 / 0 |
| 3B159D009VZ00000-old | improved-transparency-minecraft-26.3 | DirectGLES | on | 9.857 | - | 11.404 | - | +15.7% | 26.125 | 27.013 | P/P / - / P/P | - / 8 |
| 3B159D009VZ00000-old | improved-transparency-minecraft-26.3 | DirectGLES | off | 9.601 | - | 11.487 | - | +19.6% | 26.230 | 27.477 | P/P / - / P/P | - / None |
| 3B159D009VZ00000-old | improved-transparency-minecraft-26.3 | DirectVulkan | on | - | - | - | - | - | - | - | P/? / - / - | - / - |
| 3B159D009VZ00000-old | improved-transparency-minecraft-26.3 | DirectVulkan | off | 8.197 | - | - | - | - | 23.349 | - | P/P / - / - | - / - |

d P3a-boundary = push(0x1ff) vs pull; d P4a total = push(0x1fff) vs pull; the P4a-only share is the difference of the two.
A '-' p50 means the run produced no result (rc != 0); see the runner.log in that directory.
