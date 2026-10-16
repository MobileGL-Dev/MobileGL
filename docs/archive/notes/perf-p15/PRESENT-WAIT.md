# P15 present wait on FCL: per-frame timeline

Offline analysis of the existing P15 Perfetto traces. Scene: Minecraft 1.21.5 vanilla, frozen world,
cpuhunt clocks (CPU 1497/1248 MHz, GPU 903 MHz, FCL pinned to cpu2-6), display at 165 Hz (6.06 ms
vsync). All 35 runs used are VALID. Frames counted: Espryt monolith 11049 (fbm/y, c4/v, c4/w), Magma
monolith 10938 (c1c2/t, c1c2/x, fbm/y), Magma inproc 10079 (c1c2/t, c1c2/x), Espryt inproc 4639
(c1/u), MobileGlues 14398 (c1c2, fbm, c4).

## Answer

The producer throttle in `queueBuffer` waits for the previous frame's GPU-completion fence. That fence
is late because of the display, not because the GPU is busy:

1. About every other frame (45-56 %) renders into the window buffer that the display has just handed
   back. Its release fence is still pending, and it signals only at the display's next commit-done
   (0.24 ms after the `crtc_event` kernel thread runs).
2. The whole frame's GPU work waits behind that fence. MobileGL submits the frame as one batch at the
   swap (Espryt: one driver flush at `eglSwapBuffers`; Magma: the frame's first `vkQueueSubmit`
   carries the acquire-semaphore wait with `ALL_COMMANDS`).
3. Once the buffer is released, the batch still needs about 1.3 ms before its fence signals: about
   0.8 ms of GPU start latency plus 0.5 ms of execution.
4. Meanwhile the CPU has already rendered the next frame. Its `queueBuffer` throttles on that late
   fence.

How the candidates come out:

- **(b) The wait for the compositor's buffer release is the trigger.** 99.4-99.6 % of all throttle
  time follows such a gated frame. Frames whose buffer was already released cause no wait (0.00
  ms/frame on every arm).
- **(a) Late, single-batch submission is an amplifier.** It puts the whole frame, not just the final
  window pass, behind the gate.
- **(d) Something else: a fixed GPU start latency of about 0.8 ms** whenever a batch becomes runnable
  on an idle GPU. It does not depend on how long the GPU was idle (0.3 ms or 4 ms), and it does not
  count as GPU busy. It is the larger part of the time after the release.
- **(c) SurfaceFlinger GPU composition is ruled out.** `hasClientComposition` is 0 on every
  SurfaceFlinger frame in every run, so composition is device-only and SurfaceFlinger does no GPU
  work.

The GPU is idle about 4 ms per burst and runs about 0.5 ms per frame. That is the 15 %
`gpu_busy_percentage` the frequency sampler records.

## What the throttle waits on (mechanism)

- FCL's SurfaceView is a BLAST surface: the BufferQueue and its consumer (BLASTBufferQueue) live in
  the FCL process. Both EGL and the Android Vulkan swapchain connect as `NATIVE_WINDOW_API_EGL`, so
  `BufferQueueProducer::queueBuffer` ends with `lastQueuedFence->waitForever` (the `waitForever`
  slice) on the previous queued buffer's acquire fence, which is its GPU-completion fence.
- The display runs at 165 Hz. SurfaceFlinger commits about 10.1-10.3 ms ahead of the target vsync
  (`beginFrame ... vsyncIn 10.1ms`), and the HWC commit takes 3.2-4.1 ms. One FCL buffer per vsync is
  latched (about 813 per 5 s). Every other queued frame is dropped by replacement in the BufferQueue,
  which runs in async (MAILBOX / swap-interval-0) mode.
- BLAST keeps 2-3 buffers in flight to SurfaceFlinger (the `BufferTX` counter). Magma's swapchain has
  5 images: MAILBOX, `max(FIF hint 3, surface minImageCount 5)`. That leaves the app 2 buffers: one
  being rendered and one queued, which may be dropped. The Espryt and MobileGlues EGL surfaces show
  the same A/B alternation, so they also leave the app 2 buffers (inferred, not read from the
  driver).
- Each vsync, SurfaceFlinger hands back the buffer leaving the screen, with a release fence that
  signals when the display flips. The free list is FIFO, so the app's next dequeue takes that buffer.
  This is a gated frame (A). The frame after it (B) gets a dropped buffer, which is already free.
- GPU timing comes from libgui's `FenceMonitor` threads in the FCL process. They exist because atrace
  `gfx` is on:
  - `GPU completion` gives one `waiting for GPU completion N` slice per queued fence; its end is the
    fence signal time E;
  - `HWC release` does the same per dequeued buffer; its end is the release time R, and a
    `... has signaled` instant marks a buffer that was already free.
- They are cross-checked against kernel-thread scheduling:
  - `kgsl_hwsched` runs at R (median offset -0.00 ms): the kgsl sync object is satisfied and the
    batch is dispatched;
  - `kgsl-events` runs 0.12-0.16 ms before E (the fence signal);
  - `kgsl-events` is also what wakes the presenting thread out of `waitForever` in 100 % of long
    waits, with a 60-70 us runnable latency (clocks pinned, no frequency changes in the window).

## Espryt monolith: the typical cycle

Anchor: swap entry of a gated frame A = 0. Values are medians, with p10..p90 in brackets. Frame A is
followed by frame B; about 1.5 frames per vsync. Medians of different events do not compose exactly.
Per frame pair, B's throttle ends when A's fence signals. `kgsl-events` wakes the presenting
thread directly (+0.06 ms), so W1_B lands at or slightly before the FenceMonitor's E_A, which is
about 0.1 ms late.

```
Espryt monolith
  t(ms)      -3        -2        -1        0         1         2         3         4         5
             |---------|---------|---------|---------|---------|---------|---------|---------|--
  render thr  ============A cpu========d===S ============B cpu========d===S~~~~~
  display                                                      v  R
  GPU (FCL)                                .........A held........+++st+++##A####B###
  fence A                                                                      E
  (d = dequeue, S = swap entry, ~ = queueBuffer throttle, v = crtc_event, R = buffer release,
   . = A's batch submitted but held by the release fence, st = GPU start latency, # = GPU execution)
```

1. **-2.90** [-3.44..-2.47]: the previous swap returns and A's CPU frame starts on the render thread.
2. **-0.43** [-0.58..-0.38]: the first default-framebuffer use makes the Adreno EGL dequeue lazily.
   A gets buffer X, whose release is pending.
3. **0.00**: `eglSwapBuffersWithDamageKHR(A)`. The driver flushes A's whole frame as one batch, and
   kgsl holds it behind X's release fence. The GPU stays idle.
4. **0.09 to 0.10**: `queueBuffer(A)` throttles on the previous frame's fence. That fence has already
   signaled, so there is no wait.
5. **0.19**: the swap returns and B's CPU frame starts.
6. **1.97** [0.85..4.66]: `crtc_event` runs (display commit-done).
7. **2.27** [1.11..4.98]: R_A, X is released. `kgsl_hwsched` dispatches A's batch at the same
   instant.
8. **2.27 to about 3.1**: GPU start latency of about 0.8 ms, for dispatch and power-up (not GPU busy).
9. **2.71**: B dequeues a buffer that is already free.
10. **3.09**: `eglSwapBuffers(B)`. B's batch queues behind A's in the in-order context.
11. **About 3.1 to 3.58**: A executes on the GPU (0.5 ms).
12. **3.20** [2.69..4.11]: `queueBuffer(B)` starts throttling on E_A.
13. **3.58** [2.42..6.31]: E_A. `kgsl-events` signals, and the render thread is runnable after 60
    us.
14. **About 3.6**: `queueBuffer(B)` returns. B's batch then runs back to back with A (E_B is about
    E_A + 0.5), and the GPU idles about 4 ms until the next release.

The throttle is 0.61 ms/frame on average. 58 % of the frames that follow a gated frame wait, with a
median of 2.0 ms [0.35..3.35]. The two halves of the wait:

- **0.27: display before release.** B is already throttling before the display has released A's
  buffer.
- **0.33: GPU after release.** About 0.20 of this is start latency and about 0.13 is A's execution.

Critical path for B's wait: display commit-done (`crtc_event`), then +0.24 ms to the release fence of
A's buffer, then the kgsl sync object, then `kgsl_hwsched` dispatch, then about 0.8 ms of GPU start,
then about 0.5 ms of A's execution, then `kgsl-events` signals E_A, then the render thread wakes
(+60 us) and `queueBuffer(B)` returns. MobileGL CPU work is not on this path.

Worked example: `fbm/y-DirectGLES-r1`, frames 648-649, from `win.py`, offsets from swap 648:

- crtc_event 1.874;
- HWC release thread wakes 2.054, R 2.09;
- `kgsl_hwsched` 2.082;
- swap 649 at 2.471, throttle from 2.57;
- `kgsl_hwsched` (retire) 3.247, `kgsl-events` 3.317;
- the render thread resumes at 3.38 (a 0.82 ms wait);
- 649 completes at 4.056, 0.48 ms after 648, back to back.

## Magma monolith

```
Magma monolith
  t(ms)      -3        -2        -1        0         1         2         3         4         5
             |---------|---------|---------|---------|---------|---------|---------|---------|--
  render thr       d=========A cpu=========S d=========B cpu==========S~~~~wait~~~~
  display                                                       v  R
  GPU (FCL)                                .........A held.........++++st+++##A####B###
  fence A                                                                        E
```

1. **-2.36**: `AcquireNextImageKHR` runs right after the previous present. A gets X, whose release is
   pending; `HWC release` waits on it.
2. **-2.36 to 0**: A's CPU frame. All GPU work is recorded and nothing is submitted. There are 1.33
   `QueueSubmit` per frame: the first comes 2.34 ms after the previous present, and the last 0.07 ms
   before `QueuePresentKHR`.
3. **0**: `QueueSubmit`, then `QueuePresentKHR(A)`. The first submit of the frame carries the
   image-available semaphore wait with `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT`
   (`SubmitPendingCommandBuffer` / `FrameContext::GetSubmitInfo`, `imageAvailableSemaphoreConsumed`),
   so the whole frame is gated. `queueBuffer(A)` has no wait.
4. **0.20**: B acquires a buffer that is already free.
5. **2.11**: `crtc_event` runs.
6. **2.41** [1.64..5.46]: R_A. `kgsl_hwsched` dispatches A's batch.
7. **2.66 to 2.75**: B submits and presents, then throttles on E_A.
8. **3.79** [3.06..6.88]: E_A, which is R + 1.39 (about 0.87 start latency + 0.52 execution). B's
   throttle ends about here.

The throttle is 0.71 ms/frame: display before release 0.26, GPU after release 0.45. 85 % of the
frames after a gated frame wait, with a median of 1.21 ms. Magma waits more per frame than Espryt
because it is faster on CPU: B reaches its throttle about 0.45 ms earlier relative to the release.

## Inproc arms (the apply thread presents)

```
Magma inproc (apply thread)
  t(ms)      -3        -2        -1        0         1         2         3         4         5
             |---------|---------|---------|---------|---------|---------|---------|---------|--
  apply thr                   =d==A cpu====S d===B cpu===S~~~~~~~~~~~~~~wait~~~~~~~~~~~~~~~
  display                                                              v  R
  GPU (FCL)                                .............A held............++++st+++###A#####B###
  fence A                                                                                E

Espryt inproc (apply thread)
  apply thr               ======A cpu=d====S ======B cpu==d==S~~~~~~~~~~wait~~~~~~~~~~~
  display                                                           v  R
  GPU (FCL)                                ...........A held...........++++st+++##A####B###
  fence A                                                                            E
```

Magma inproc:

1. **-1.21**: acquire, release pending.
2. **0**: A's submit and present on `mgl-srv-apply`.
3. **0.20**: B acquires a free buffer.
4. **1.41 to 1.52**: B presents, then throttles on E_A.
5. **2.82**: `crtc_event` runs.
6. **3.12**: R_A.
7. **4.59**: E_A, which is R + 1.46 (0.90 start latency + 0.56 execution). B's throttle ends here, a
   3.0 ms wait in the median waiting pair.

The throttle is 1.28 ms/frame: display before release 0.69, GPU after release 0.58. 96 % of the
frames after a gated frame wait. Espryt inproc has the same shape: throttle 1.08 ms/frame
(0.54 + 0.53), with R at 2.79 and E_A at 4.20.

The chain reaches the GL thread (Thread-14) too. It sleeps 1.3-1.6 ms/frame, and 0.7-1.1 ms/frame of
those sleeps end within 0.3 ms of the apply thread's throttle ending. So the chain is: display, then
GPU, then the apply thread's `queueBuffer`, then the present credit, then the GL thread.

This explains the HANDOFF's "about 0.3 ms per frame beyond the apply CPU is unexplained". It is not
CPU. The apply thread's frame is its CPU plus this throttle, and the throttle grows as the presenting
thread gets faster, because the release is locked to the display and 1.3 ms of GPU path is chained
after it. Present credit 2 helps for the same reason (+3-7 % measured in HANDOFF): it lets the GL
thread run on while the apply thread sits in the throttle.

## MobileGlues control

```
MobileGlues
  render thr                 =====A cpu==d=S   =====B cpu==d=S  ~~~~~~~~~~~wait~~~~~~~~~~~
  display                                                              v  R
  GPU (FCL)                                .............A held............++++st+++##A####B###
  fence A                                                                               E
```

MobileGlues has the identical mechanism: the same lazy dequeue just before the swap, the same
whole-frame batch at `eglSwapBuffers`, and the same 47 % gated frames. Its offsets:

1. **-0.24**: A dequeues.
2. **0**: A swaps. `queueBuffer` comes 0.29 ms into `eglSwapBuffers`, which spends about 0.3 ms
   before queueing.
3. **1.60**: B dequeues.
4. **2.08**: B throttles.
5. **3.13**: R_A.
6. **4.48**: E_A, which is R + 1.35 (0.87 start latency + 0.48 execution).

The throttle is 1.12 ms/frame: display before release 0.59, GPU after release 0.52. That matches the
1.4 ms/frame off-CPU seen in simpleperf. MobileGlues waits more than MobileGL monolith only because
its CPU frame is shorter (1.95 ms against 2.9-3.4 ms), so B reaches the gate earlier. Its GPU per
frame is the same: 0.48 ms back to back, against 0.50-0.56 for MobileGL.

## Why CPU cuts are partly absorbed

Across the arms, frame time is the presenting thread's CPU plus the throttle, and the throttle grows
as the CPU shrinks:

- Espryt monolith: CPU 3.35 + wait 0.61 = 4.00;
- Magma monolith: CPU 2.86 + wait 0.71 = 3.60;
- Espryt inproc: CPU 2.04 + wait 1.08 = 3.18;
- MobileGlues: CPU 1.95 + wait 1.12 = 3.07;
- Magma inproc: CPU 1.53 + wait 1.28 = 2.93.

CPU comes from `sched` over the trace window. At the monolith operating point about 80 % of a CPU
saving reaches frame time (Espryt to Magma: 0.49 less CPU gives 0.40 less frame). At the
inproc/MobileGlues point only about a third does (MobileGlues to Magma inproc: 0.42 less CPU gives
0.14 less frame).

The e70925d3 simpleperf numbers fit the same trend: Espryt has 3.08 ms on CPU and 0.82 off CPU in
Present; Magma has 2.63 and 1.03. The faster CPU waits more.

## Options (portable), with the effect read off the timeline

The effects come from `sim.py`. It is an in-order GPU queue model replayed on the measured CPU
schedule: start latency + execution from `gexec.py`, and each frame's measured release time. Its base
scenario reproduces the measured throttle within 0.01-0.03 ms/frame on all five arms. The results are
first-order: the CPU schedule is held fixed, so phase feedback is not modelled.

1. **One or two more swapchain images (Magma).** Request `surface minImageCount + 1` (or + 2) instead
   of `max(FIF, minImageCount)` = 5. Then the buffer the display has just released goes to the back
   of the FIFO free list and is reused one (or two) frames later, by which time its release fence has
   usually signaled.
   - In the current traces, a buffer reused k frames later at the CPU-bound cadence would still be
     gated in:
     - Magma monolith: 16 % of frames at k=1, 3 % at k=2 (50 % today);
     - Espryt monolith: 21 % at k=1, 0 % at k=2;
     - Magma inproc: 40 % at k=1, 23 % at k=2, 8 % at k=3.
   - First-order effect: Magma monolith's throttle goes from 0.71 to about 0.0 ms/frame, so the frame
     drops from 3.60 ms toward the CPU's 2.9 ms. Treat this as an upper bound; the faster schedule
     reuses buffers sooner.
   - Inproc needs +2 or +3.
   - Cost: one 2560x1600 RGBA buffer (about 16 MB) per image. MAILBOX adds no latency.
   - This is a real trade-off (memory), so A/B the count.
   - Espryt has no NDK-public way to set an EGL window's buffer count. The private `ANativeWindow`
     perform call or ASurfaceControl would need the user's greenlight under the platform-agnostic
     rule.
2. **Gate only the window pass (split the frame's submission).**
   - Magma:
     - submit the offscreen work recorded so far without the image-available semaphore, at least once
       before the swapchain image is first touched;
     - attach the acquire wait only to the submit that contains the first swapchain-image access (the
       final blit and present transition), not to the frame's first submit.
   - Espryt: `glFlush` at the first default-framebuffer bind of a frame, just before the driver's lazy
     dequeue. The offscreen work is then submitted with no release sync object in front of it.
   - Effect: throttle 0.62 to 0.49 (Espryt monolith), 0.72 to 0.54 (Magma monolith), 1.31 to 1.09
     (Magma inproc), 1.09 to 0.91 (Espryt inproc).
   - The gain is small because the 0.8 ms GPU start latency is paid again for the final pass after
     the release.
   - It is the only portable lever for Espryt. With option 1 on top, the model gives about 0 on all
     arms.
3. **Take the throttle off the GL thread.**
   - Inproc: present credit 2 (already measured at +3-7 %; it costs a frame of latency, so it is the
     user's decision).
   - Magma monolith: the equivalent is a present thread that issues `vkQueuePresentKHR` off the GL
     thread, with the same latency trade.
   - Espryt monolith cannot do this: `eglSwapBuffers` stays on the context thread.
4. Not recommended:
   - keeping the GPU awake to hide the 0.8 ms start latency (a power trade, not portable);
   - anything in SurfaceFlinger or vsync.

## What a fresh e70925d3 trace should confirm

Run the scripts in `p15/pwait/` on the new run directories:

- `cycle.py <label> <runs...>`
- `pairs.py`
- `gexec.py`
- `sim.py <label> <start> <exec> <runs...>`

Expected on base e70925d3:

- gated frames about 50 %;
- throttle about 0.8 ms/frame (Espryt) and about 1.0 (Magma), matching simpleperf's Present off-CPU;
- 99+ % of the throttle after gated frames;
- R 0.24 ms after `crtc_event`, with `kgsl_hwsched` at R;
- E - R about 1.3-1.4 ms for gated frames, with back-to-back execution about 0.5 ms.

For a CPU-only change (the TLS agent's): frame time should drop by about 0.8 x the CPU saving at the
monolith point, with the throttle rising by the rest. If it does not rise, the model is wrong.

For the change in option 1:

- `HWC release fence N has signaled` should appear at 85 % or more of `AcquireNextImageKHR`;
- gated frames below 20 %;
- throttle below 0.2 ms/frame (monolith).

For the change in option 2: E - R for gated frames should fall from about 1.35 to about 0.9 ms (start
latency + final pass), and back-to-back execution should not change.

Config notes:

- The `kgsl/adreno_cmdbatch_*`, `adreno_syncobj_*` and `kgsl_waittimestamp_*` ftrace events in
  `perfetto.cfg` are not recorded on this kernel (`ftrace_setup_errors` = 43 in every trace). Drop
  them, or list `/sys/kernel/tracing/events/kgsl/` for the kgsl hardware-scheduler event names.
- `gpu.renderstages` does work on this device: `runs/gpu-bsl/*/gpu.pftrace` has Render/Surface/Blit
  stages. One extra capture with it would split the 0.8 ms start latency from execution directly. Its
  overhead should be A/B'd before using it in a perf capture.
- Keep atrace `gfx`: the `GPU completion` / `HWC release` FenceMonitor threads only exist while it is
  on.
- MobileGL emits no atrace markers. A marker at the frame's flush/submit points and at the first
  default-framebuffer bind would make option 2 directly visible.

## Files

All under scratchpad `p15/pwait/`:

- `tp.py`: trace_processor wrapper, cached;
- `pw.py`: per-frame alignment of S, D, R, W0/W1 and E;
- `cycle.py`: attribution and the cycle medians;
- `pairs.py`, `gexec.py`, `gapcost.py`, `reuse.py`, `wakelat.py`, `glchain.py`;
- `sim.py`: the counterfactuals;
- `win.py`, `seq.py`: raw windows;
- `diag.py`, `mkdiag.py`: the diagrams;
- outputs in `out/`.
