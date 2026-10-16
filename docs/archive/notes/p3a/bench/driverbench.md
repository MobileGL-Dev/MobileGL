# P3a DriverBench (desktop, lavapipe/llvmpipe) - median ns_per_op over repeats

| arm | mc_vanilla_draw | mc_state_toggle | mc_pass_switch |
|---|---|---|---|
| native | 4758.9 (n=5, min 4741.8) | 22714.8 (n=5, min 22390.5) | 438175.0 (n=5, min 433303.1) |
| espryt-pull | 5114.6 (n=5, min 5089.5) | 23287.0 (n=5, min 23116.8) | 435814.7 (n=5, min 433424.2) |
| espryt-push | 6162.3 (n=5, min 6114.3) | 24496.4 (n=5, min 24006.7) | 435974.8 (n=5, min 433336.2) |
| espryt-push7f | 5463.1 (n=5, min 5393.5) | 24575.9 (n=5, min 24272.3) | 436031.2 (n=5, min 435819.3) |
| espryt-push0 | 5692.8 (n=5, min 5648.5) | 24464.6 (n=5, min 24302.3) | 436598.1 (n=5, min 436019.1) |
| espryt-nocso | 6156.1 (n=5, min 6090.0) | 24855.5 (n=5, min 24418.5) | 435275.8 (n=5, min 434381.4) |
| magma-pull | 17098.6 (n=5, min 17004.4) | 32611.7 (n=5, min 32283.6) | 436748.0 (n=5, min 436116.7) |
| magma-push | 17709.4 (n=5, min 17299.1) | 33536.9 (n=5, min 33295.5) | 437314.0 (n=5, min 436211.2) |
| magma-push7f | 17287.4 (n=5, min 17207.1) | 33549.7 (n=5, min 33178.9) | 436457.5 (n=5, min 436283.2) |
| magma-push0 | 17738.7 (n=5, min 17497.4) | 33799.6 (n=5, min 33588.7) | 440445.7 (n=5, min 439371.9) |
| magma-nocso | 17728.9 (n=5, min 17359.6) | 34221.5 (n=5, min 34057.0) | 436465.2 (n=5, min 435002.8) |

## Deltas on mc_vanilla_draw (ns/draw)

- espryt: T1 = push(0x1ff) - pull = **+1047.7**; T2 = push(0x7f, P2 boundary) - pull = +348.5; T1 - T2 (what P3a added) = **+699.2**; push(PIPE_PUSH=0) - pull = +578.2; no-CSO-content-addressing - push = -6.2
- magma: T1 = push(0x1ff) - pull = **+610.8**; T2 = push(0x7f, P2 boundary) - pull = +188.8; T1 - T2 (what P3a added) = **+422.0**; push(PIPE_PUSH=0) - pull = +640.1; no-CSO-content-addressing - push = +19.5

## Blaze3D blend toggle (mc_state_toggle, ns per toggle pair) and pass switch

- espryt mc_state_toggle: pull 23287.0, push 24496.4, delta +1209.4, native 22714.8
- espryt mc_pass_switch: pull 435814.7, push 435974.8, delta +160.1, native 438175.0
- magma mc_state_toggle: pull 32611.7, push 33536.9, delta +925.2, native 22714.8
- magma mc_pass_switch: pull 436748.0, push 437314.0, delta +566.0, native 438175.0
