# DriverBench (desktop, lavapipe/llvmpipe) - median ns_per_op over repeats

| arm | mc_vanilla_draw | mc_state_toggle | mc_pass_switch |
|---|---|---|---|
| native | 4771.3 (n=5, min 4744.7) | 22968.3 (n=5, min 22532.9) | 437758.9 (n=5, min 436480.1) |
| espryt-pull | 5120.7 (n=5, min 5072.3) | 23752.8 (n=5, min 23216.3) | 443300.2 (n=5, min 440425.2) |
| espryt-push | 5442.9 (n=5, min 5396.3) | 24869.1 (n=5, min 24724.6) | 446016.5 (n=5, min 443312.2) |
| espryt-push0 | 5671.2 (n=5, min 5656.1) | 24583.8 (n=5, min 24078.7) | 443806.9 (n=5, min 438585.2) |
| espryt-nocso | 5374.2 (n=5, min 5345.5) | 24630.0 (n=5, min 24395.1) | 435453.6 (n=5, min 434007.7) |
| magma-pull | 16899.6 (n=5, min 16814.4) | 32704.5 (n=5, min 32330.1) | 438405.7 (n=5, min 435954.5) |
| magma-push | 17244.5 (n=5, min 17051.8) | 33857.4 (n=5, min 33629.6) | 446066.5 (n=5, min 439141.4) |
| magma-push0 | 17598.6 (n=5, min 17427.3) | 34081.9 (n=5, min 33269.3) | 445006.0 (n=5, min 443467.1) |
| magma-nocso | 17444.2 (n=5, min 17353.3) | 34779.6 (n=5, min 34306.0) | 444758.0 (n=5, min 444413.2) |

## Deltas on mc_vanilla_draw (ns/draw)

- espryt: T1 = push - pull = **+322.2**; T2 = push(PIPE_PUSH=0) - pull = +550.5; T1 - T2 (what P2 added) = -228.3; no-CSO-content-addressing - push = -68.7
- magma: T1 = push - pull = **+344.9**; T2 = push(PIPE_PUSH=0) - pull = +699.0; T1 - T2 (what P2 added) = -354.1; no-CSO-content-addressing - push = +199.7

## Blaze3D blend toggle (mc_state_toggle, ns per toggle pair) and pass switch

- espryt mc_state_toggle: pull 23752.8, push 24869.1, delta +1116.3, native 22968.3
- espryt mc_pass_switch: pull 443300.2, push 446016.5, delta +2716.3, native 437758.9
- magma mc_state_toggle: pull 32704.5, push 33857.4, delta +1152.9, native 22968.3
- magma mc_pass_switch: pull 438405.7, push 446066.5, delta +7660.8, native 437758.9
