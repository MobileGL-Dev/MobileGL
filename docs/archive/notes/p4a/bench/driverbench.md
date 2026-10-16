# P4a DriverBench (desktop, lavapipe/llvmpipe) - median ns_per_op over repeats

| arm | mc_vanilla_draw | mc_state_toggle | mc_pass_switch |
|---|---|---|---|
| native | 4398.9 (n=5, min 4349.1) | 21387.3 (n=5, min 21208.2) | 412657.8 (n=5, min 407294.9) |
| espryt-pull | 4684.4 (n=5, min 4669.6) | 22010.4 (n=5, min 21461.7) | 410936.2 (n=5, min 407814.3) |
| espryt-push | 5760.5 (n=5, min 5728.0) | 22974.9 (n=5, min 22642.5) | 419703.1 (n=5, min 407773.8) |
| espryt-push7f | 5749.1 (n=5, min 5672.9) | 23768.2 (n=5, min 23188.9) | 419288.0 (n=5, min 407072.0) |
| espryt-push0 | 5355.2 (n=5, min 5299.2) | 23618.8 (n=5, min 23046.1) | 420362.2 (n=5, min 418725.9) |
| espryt-nocso | 5929.0 (n=5, min 5791.3) | 23685.0 (n=5, min 22996.2) | 418677.0 (n=5, min 416263.8) |
| magma-pull | 15394.9 (n=5, min 15206.4) | 31678.5 (n=5, min 30485.3) | 420197.6 (n=5, min 411290.5) |
| magma-push | 16023.9 (n=5, min 15876.0) | 32417.4 (n=5, min 32084.8) | 415641.9 (n=5, min 414170.3) |
| magma-push7f | 15847.5 (n=5, min 15733.7) | 32100.3 (n=5, min 30750.9) | 416670.4 (n=5, min 413160.7) |
| magma-push0 | 15946.6 (n=5, min 15808.2) | 31610.2 (n=5, min 30971.6) | 417410.5 (n=5, min 411148.7) |
| magma-nocso | 15846.2 (n=5, min 15748.2) | 32495.9 (n=5, min 31919.9) | 411306.4 (n=5, min 410235.6) |

## Deltas on mc_vanilla_draw (ns/draw)

- espryt: T1 = push(0x1fff) - pull = **+1076.1**; T2 = push(0x1ff, P2+P3a boundary) - pull = +1064.7; T1 - T2 (what P4a added) = **+11.4**; push(PIPE_PUSH=0) - pull = +670.8; no-CSO-content-addressing - push = +168.5
- magma: T1 = push(0x1fff) - pull = **+629.0**; T2 = push(0x1ff, P2+P3a boundary) - pull = +452.6; T1 - T2 (what P4a added) = **+176.4**; push(PIPE_PUSH=0) - pull = +551.7; no-CSO-content-addressing - push = -177.7

## Blaze3D blend toggle (mc_state_toggle, ns per toggle pair) and pass switch

- espryt mc_state_toggle: pull 22010.4, push 22974.9, delta +964.5, native 21387.3
- espryt mc_pass_switch: pull 410936.2, push 419703.1, delta +8766.9, native 412657.8
- magma mc_state_toggle: pull 31678.5, push 32417.4, delta +738.9, native 21387.3
- magma mc_pass_switch: pull 420197.6, push 415641.9, delta -4555.7, native 412657.8
