# P5d round 3 - device results log (Redmi 2f7cbe2e, FCL + MC 26.3-rc-3 world "test", VD12, DirectGLES)

Protocol: CPU pinned big 1958400 / little 1555200 (pin_device.sh), fan level 2 (~14.2k rpm), screen on,
cpuss-0-0 50-54 C during runs. GPU: the vendor holds thermal_pwrlevel=1 and resets min_pwrlevel to 12 at
every app start, so the GPU is GOVERNED, not pinned (gpuclk 342 MHz in every inproc arm = GPU idle,
832 MHz in monolith). All inproc arms are CPU-bound on the client thread; monolith may be slightly GPU-
limited. fps = frames/second from the `MGPipe stats:` lines over a 30 s window after 20 s settle (world
loads in ~15 s on this device); ms/frame = per-thread utime+stime over the window / frames.
Thread affinity: `sched_setaffinity`/`taskset` are accepted and silently ignored by this kernel for
app threads (RESOLVED mask 0xff; taskset "new affinity mask: ff"), so core placement is the scheduler's.

## Session base0 (unpatched head 85362a85 lib, APK with mg_env.txt plumbing) - 2026-09-18 03:18-03:26

| arm | env | fps mean / p50 / min | client ms/frame (core) | apply ms/frame (core) |
|---|---|---|---|---|
| base0-inproc | - | 64.8 / 64.3 / 60.9 | 15.03 (cpu5, mid) | 12.42 (cpu7, big) |
| base0-mono | monolith | 129.2 / 116.5 / 112.0 | 5.85 (cpu7, big) | - |
| spin2000 | MOBILEGL_IPC_SPIN_US=2000 | 72.7 / 72.6 / 68.5 | 14.00 (cpu2) | 13.64 (cpu7) |
| nobarrier | MOBILEGL_IPC_VERB_BARRIER=0 | aborted: Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot@FenceSync"} at the first race - the negative control cannot run a real workload | | |
| affoff | MOBILEGL_IPC_SERVER_AFFINITY=off | 65.9 / 65.9 / 57.7 | 14.20 (cpu5) | 12.21 (cpu7) |

## Session place1 (same lib) - 03:27-03:35

| arm | env | fps mean / p50 / min | client ms/frame (core) | apply ms/frame (core) |
|---|---|---|---|---|
| bigclient | taskset c0 on GL thread (ignored) | 64.9 / 65.0 / 57.4 | 15.05 (cpu4) | 12.75 (cpu7) |
| bigclient-spin2000 | SPIN_US=2000 (+ignored taskset) | 69.7 / 68.9 / 65.0 | 13.94 (cpu4) | n/a |
| bigboth-spin2000 | SPIN_US=2000 (+ignored taskset) | 68.3 / 68.3 / 65.2 | 14.62 (cpu4) | 14.31 (cpu7) |
| spin10000 | SPIN_US=10000 | 62.5 / 63.2 / 59.1 | 15.49 (cpu2) | 15.41 (cpu6) |

Readings:
- Split/monolith p50 ratio at the head: 64.3 / 116.5 = 1:1.81 (report said ~1:1.7).
- The split's client thread is the critical path: ~15 ms CPU per 15.5 ms frame (97% busy, spin-waits
  included), and the scheduler puts it on a MID core (cpu2/4/5 at 1555 MHz) while the always-spinning
  apply thread holds a big core; monolith's render thread sits on cpu7 (big).
- Spin 2000 us: +4..+8 fps (3 arms 68-73 vs 64-66) - park/unpark latency is a real, second-order cost.
  Spin 10000 us: worse (63) - both threads then burn full cores and the client lands on cpu2.
- Server affinity knob: no effect (it never took on this kernel).

## Session r3a (round-3 PRE-review build, patches T/D/B/C as first written) - 03:42-03:49

| arm | fps mean / p50 / min | client ms/frame (core) | apply ms/frame (core) | wait[] (run totals at window end) |
|---|---|---|---|---|
| r3-inproc | 112.5 / 114.0 / 88.8 | 8.68 (cpu4) | 6.93 (cpu7) | srv=7.56M srvpark=76k cli=6.70M clipark=17k |
| r3-spin2000 | 113.0 / 112.3 / 106.1 | 8.88 (cpu5) | 8.88 (cpu7) | srvpark=1.6k clipark=286 |
| r3-mono | 115.4 / 115.7 / 108.4 | 5.70 (cpu5) | - | capped |
| r3-spin1000 | 107.6 / 108.5 / 98.0 | 9.27 (cpu5) | 8.63 (cpu7) | |

## Session r3b/r3c/r3d (round-3 POST-review build = commit 1f8de61b) - 04:04-04:32

| arm | fps mean / p50 / min / max | client ms/frame (core) | apply ms/frame (core) | note |
|---|---|---|---|---|
| r3b-inproc | (false "app died": one pidof hiccup; fixed in bench_fcl3+) | | | 14 stats lines, 855 draws/f, no Fatal |
| r3b-mono | 246.9 / 266.1 / 123 / 298 | 3.9 (cpu7, 97% busy) | - | UNCAPPED run - vsync not enforced this time |
| r3b-inproc2 | 107.4 / 106.7 / 98 / 120 | 9.3 (cpu5) | 7.1 (cpu7) | |
| r3b-spin2000 | 109.1 / 109.6 / 102 / 113 | 9.1 (cpu4) | 8.7 (cpu7) | presented (SF timestats) 113 fps |
| r3c-mono1 | 166.1 / 205.8 / 110 / 280 | 5.0 (cpu7) | - | uncapped, swinging 110-280 inside the window |
| r3c-inproc1 | 103.8 / 106.1 / 91 / 112 | 9.24 (cpu4) | 7.0 (cpu7) | |
| r3c-mono2 | 114.6 / 115.3 / 108 / 117 | 6.2 (cpu7) | - | capped at 120 Hz |
| r3c-inproc2 | 101.6 / 103.3 / 82 / 111 | 9.2 (cpu5) | 7.4 (cpu7) | |
| r3c-inproc3 (csbig spec ignored by v3 driver) | 100.9 / 101.9 / 83 / 109 | 9.1 (cpu5) | 7.3 (cpu7) | |
| r3c-mono3 | 114.9 / 115.1 / 109 / 117 | 6.1 (cpu7) | - | capped |
| r3d-inproc-csbig (cpuset 6-7 written for the GL tid) | 112.5 / 115.3 / 92 / 122 | 8.63 (cpu5) | 6.98 (cpu7) | thread still on cpu5: cpuset ignored too |

pmap wire bytes identical pre/post review (364.7 KB/frame); post-review inproc 103-107 vs pre-review 114 is inside run-to-run drift (body 50.8 vs 53-54 C at window start, mid-core placement).
Reading: monolith fps is bimodal on this device (115 = 120 Hz vsync cap; 206-266 = uncapped runs); per-thread CPU ms/frame is the comparable metric. Client thread 15.0 -> 8.7-9.2 ms/frame (mid core), apply 12.4 -> 7.0-7.4 (mostly spin), monolith render thread 5.0-6.2 (big core).
