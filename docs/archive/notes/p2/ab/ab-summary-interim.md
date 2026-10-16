# P2 paired A/B - per-thread CPU per frame (ms), trailing window, push - pull

pin columns: before/after verdict per run (P=PINNED, D=DRIFT = a cluster clamped by thermal management, U=UNPINNED) @ cpuss temperature C

| device | case | backend | mode | pull p50 | push p50 | d p50 | pull p99 | push p99 | d p99 | frames | pull acc/draw | push acc/draw | pin pull | pin push |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 35d0befa | improved-transparency-minecraft-26.3 | DirectGLES | finish | 37.282 | 42.583 | +5.301 | 54.933 | 60.636 | +5.703 | 200/200 | 8.39 | 8.39 | P@52.5/P@53.2 | P@52.1/P@53.2 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectGLES | nofinish | 38.691 | 42.436 | +3.745 | 57.099 | 60.409 | +3.310 | 200/200 | 8.39 | 8.39 | P@50.9/P@53.2 | P@52.1/P@52.5 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectVulkan | nofinish | incomplete: pull |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | finish | 4.946 | 5.370 | +0.424 | 2320.752 | 2322.192 | +1.440 | 123/123 | 40.95 | 40.95 | P@49.8/P@48.6 | P@49.8/P@50.5 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | nofinish | 4.983 | 5.459 | +0.476 | 2320.379 | 2318.561 | -1.818 | 123/123 | 40.95 | 40.95 | P@49.8/P@50.2 | P@49.0/P@50.9 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | finish | 4.769 | 4.611 | -0.158 | 1886.629 | 1657.190 | -229.439 | 123/123 | 14.25 | 14.25 | P@50.5/P@50.9 | P@51.7/P@50.5 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | nofinish | 4.158 | 4.525 | +0.367 | 1604.480 | 1624.190 | +19.710 | 123/123 | 14.25 | 14.25 | P@50.2/P@51.3 | P@50.9/P@52.9 |
| 35d0befa | minecraft-1.21.4-in-world | DirectGLES | finish | 7.463 | 8.363 | +0.899 | 10.661 | 11.609 | +0.948 | 200/200 | 9.4 | 9.4 | P@49.0/P@44.0 | P@46.3/P@48.6 |
| 35d0befa | minecraft-1.21.4-in-world | DirectGLES | nofinish | 7.392 | 8.377 | +0.986 | 11.086 | 11.581 | +0.495 | 200/200 | 9.4 | 9.4 | P@47.1/P@46.7 | P@44.4/P@46.7 |
| 35d0befa | minecraft-1.21.4-in-world | DirectVulkan | finish | 5.237 | 5.842 | +0.605 | 6.767 | 7.380 | +0.613 | 200/200 | 8.76 | 8.76 | P@49.8/P@50.2 | P@49.8/P@51.3 |
| 35d0befa | minecraft-1.21.4-in-world | DirectVulkan | nofinish | 5.424 | 5.851 | +0.427 | 6.979 | 7.405 | +0.426 | 200/200 | 8.76 | 8.76 | P@47.8/P@50.2 | P@49.4/P@50.9 |

## GO/NO-GO reading (items 3-4: p50 and p99 per-thread CPU deltas must not be negative for the push arm, i.e. push must not cost more)

Windows where push costs MORE CPU than pull (d > 0):
- 35d0befa / improved-transparency-minecraft-26.3 / DirectGLES / finish: d p50 +5.301 ms, d p99 +5.703 ms
- 35d0befa / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: d p50 +3.745 ms, d p99 +3.310 ms
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / finish: d p50 +0.424 ms, d p99 +1.440 ms
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / nofinish: d p50 +0.476 ms, d p99 -1.818 ms
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / nofinish: d p50 +0.367 ms, d p99 +19.710 ms
- 35d0befa / minecraft-1.21.4-in-world / DirectGLES / finish: d p50 +0.899 ms, d p99 +0.948 ms
- 35d0befa / minecraft-1.21.4-in-world / DirectGLES / nofinish: d p50 +0.986 ms, d p99 +0.495 ms
- 35d0befa / minecraft-1.21.4-in-world / DirectVulkan / finish: d p50 +0.605 ms, d p99 +0.613 ms
- 35d0befa / minecraft-1.21.4-in-world / DirectVulkan / nofinish: d p50 +0.427 ms, d p99 +0.426 ms

## Memo gates, residual bytes and CSO counters (last complete window, push arm)

- 35d0befa / improved-transparency-minecraft-26.3 / DirectGLES / finish: gates[ers=78960/1367 etl=75378/4949 eub=75199/5128 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157
- 35d0befa / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: gates[ers=78960/1367 etl=75378/4949 eub=75199/5128 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / finish: gates[ers=113/133 etl=85/161 eub=82/164 mfp=0/0 mpm=0/0 mdt=0/0] resid=370.67 csom/csob=csom=1/csob=122
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / nofinish: gates[ers=113/133 etl=85/161 eub=82/164 mfp=0/0 mpm=0/0 mdt=0/0] resid=370.67 csom/csob=csom=1/csob=122
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96] resid=370.67 csom/csob=csom=1/csob=122
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96] resid=370.67 csom/csob=csom=1/csob=122
- 35d0befa / minecraft-1.21.4-in-world / DirectGLES / finish: gates[ers=6020/1850 etl=6884/986 eub=6962/908 mfp=0/0 mpm=0/0 mdt=0/0] resid=178.31 csom/csob=csom=2/csob=1719
- 35d0befa / minecraft-1.21.4-in-world / DirectGLES / nofinish: gates[ers=6020/1850 etl=6884/986 eub=6962/908 mfp=0/0 mpm=0/0 mdt=0/0] resid=178.31 csom/csob=csom=2/csob=1719
- 35d0befa / minecraft-1.21.4-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350] resid=178.31 csom/csob=csom=2/csob=1719
- 35d0befa / minecraft-1.21.4-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350] resid=178.31 csom/csob=csom=2/csob=1719
