# Present wait (pwait): levers, timeline and latency

Scene: FCL, Minecraft 1.21.5, frozen world, pinned clocks (CPU 1497/1248 MHz, GPU 903 MHz), Adreno phone
with a 165 Hz display (6.06 ms per vsync). Espryt = DirectGLES, Magma = DirectVulkan. Monolith: one
thread records and applies. Inproc: the GL thread records and `mgl-srv-apply` applies and presents.

The timelines and latency below come from one 4.9 s Perfetto run per arm (`pwlat`, with frametimeline).
The fps numbers come from the 3-rep A/B in [SCOREBOARD.md](../notes/perf-p15/SCOREBOARD.md) (section pwab).
The mechanism is analysed in depth in [PRESENT-WAIT.md](../notes/perf-p15/PRESENT-WAIT.md).

## Summary

- Present wait is a producer throttle in `queueBuffer`. It waits for the previous frame's GPU fence, and that
  fence is late because about every other frame renders into the buffer the display has just released.
- Lever A (gate only the window pass) halves the GPU time after the release (E-R 1.4 to 0.7 ms) on both
  backends. Lever B (`MOBILEGL_MAGMA_EXTRA_SWAPCHAIN_IMAGES=2`) cuts the gated frames on Magma (48 to 37 %
  monolith, 42 to 33 % inproc).
- fps: Magma +32-34 %, Espryt +9-12 %. Magma inproc reaches 1.50x MobileGlues. Presenting-thread CPU barely
  moves; the gain is wait time.
- Input-to-present latency is about 28-32 ms median on every arm, before and after. It is set by the
  compositor's queue (about 16 ms) and its fixed pipeline (10 ms), not by the throttle. The levers neither
  help nor hurt it, and lever B adds no queued frames.
- On this device `extra=2` gives 7 images where the default gives 6, not 5. So lever B added one image,
  not two. That is why about a third of Magma frames are still gated.

## What present wait is

Both the EGL window surface and the Vulkan swapchain sit on an Android BufferQueue. `queueBuffer` for frame
N+1 blocks until frame N's GPU-completion fence signals (`waitForever`). That fence is late for a display
reason:

1. The display hands back the buffer it stops scanning out once per vsync. Its release fence signals only
   at the next display commit.
2. The next dequeue usually gets exactly that buffer. Before the levers, MobileGL submitted the whole frame
   as one batch at the swap, behind that release fence.
3. After the release the GPU needs about 0.8 ms to start and about 0.5 ms to run the frame.
4. By then the CPU has already rendered the next frame, and its `queueBuffer` waits.

So frame time = presenting-thread CPU + throttle. The throttle is 99 % of its time behind a gated frame on
every arm (pairs.py), before and after.

## The levers

**A, gate only the window pass.** Off-screen work goes to the GPU before the window buffer is touched, so
only the final window pass waits for the release.

- Magma, 168451bc. `VulkanRenderer::SubmitAheadOfAcquiredImage` submits the recording without the acquire
  wait when the frame first reaches the acquired image (through `WireFramebuffer.inc`).
  `SubmitPendingCommandBuffer` attaches the image-available semaphore only to a submission that references
  the image (`FrameContext::FrameData::acquiredImageReferenced`). Always on; it logs "a frame's off-screen
  work goes to the queue ahead of its window pass" once.
- Espryt, 66adc7aa. `FlushAheadOfWindowBufferUse` in `DirectGLES.cpp` issues one `glFlush` per frame when
  the default framebuffer is first bound, just before the driver's lazy dequeue. Window surfaces only.
  Always on. It has no log line, but the traces show it working: E-R falls from 1.34 to 0.71 ms (monolith)
  and from 1.38 to 0.78 ms (inproc).

**B, extra swapchain images (Magma only), cdd2160f.** `MOBILEGL_MAGMA_EXTRA_SWAPCHAIN_IMAGES` (Config.h
`MagmaExtraSwapchainImages`, read in ConfigLoader.cpp, used in `SwapchainObject::Create`) requests
`surface minImageCount + extra` images, capped at the surface maximum. Default 0. See
[the knob](#the-knob-mobilegl_magma_extra_swapchain_images) below.

**F, pmflush, eec5e6e8 (a note only).** Espryt drains a partial write to a small store as a whole-store
orphan map instead of an upload-ring copy job. It is a CPU cut in the Espryt backend, not a present lever.
It is not on this branch. It was cherry-picked into the measured "after" libraries, and it does nothing on
Magma.

The measured "after" arms are: Espryt = A + F, Magma = A + F + B(2).

## Effect on fps

3 interleaved reps. MobileGlues control in the same sessions: 315.7 fps.

| arm | before -> after | change | per rep | ratio to MobileGlues |
|---|---|---|---|---|
| Magma inproc | 357.5 -> 471.9 | +32 % | 1.32 / 1.33 / 1.31 | 1.13 -> 1.50 |
| Espryt inproc | 320.3 -> 347.9 | +9 % | 1.07 / 1.10 / 1.09 | 1.01 -> 1.10 |
| Magma monolith | 277.8 -> 371.2 | +34 % | 1.41 / 1.30 / 1.31 | 0.88 -> 1.18 |
| Espryt monolith | 245.6 -> 283.0 | +12 % | 1.09 / - / 1.14 | 0.78 -> 0.90 |

Absolute fps drifts upward over a session (MobileGlues 305.6 -> 320.9), but the after/before ratio holds
per rep. Espryt monolith "before" has no rep 2 (the run never reached the world).

## Effect on the frame timeline

A typical cycle per arm. The anchor is the swap of a gated frame A (t = 0). B is the next frame. Values
are medians from the pwlat runs (cycle.py), 1 character = 0.1 ms. Medians of different events do not add
up exactly.

```
legend   = CPU on the thread        d dequeue / acquire       S swap / present entry
         ~ queueBuffer throttle     f off-screen flush/submit (after only)
         z GL thread blocked on the present credit (inproc)
         v display commit-done      R release of A's buffer   E A's GPU fence (ends B's throttle)
         . A's batch held behind R  + GPU start latency       # GPU execution
         o A's off-screen work (placement inferred: submit + ~0.8 start + ~0.45 exec; no GPU stages traced)
         w A's window pass after R, start latency included
```

### Magma monolith

```
Magma monolith, before
  t(ms)      -3        -2        -1        0         1         2         3         4         5         6
             |---------|---------|---------|---------|---------|---------|---------|---------|---------|
  render thr          =d======A cpu========S d=======B cpu=======S~~~~~~wait~~~~~~~
  display                                                        v   R
  GPU                                      ..........A held..........+++st+++##A#####B##
  fence A                                                                         E
  frame 3.29 ms = CPU 2.49 + throttle 0.78 (display 0.28, GPU after release 0.50); gated 48%; E-R 1.37

Magma monolith, after (A + B2)
  render thr          =d======A cpu======f=S d=======B cpu=======S~wait~~
  display                                                   v  R
  GPU                                    +++st+++oooo          +++w+++###B###
  fence A                                                             E
  frame 2.67 ms = CPU 2.43 + throttle 0.23 (display 0.11, GPU after release 0.11); gated 37%; E-R 0.69
```

1. A acquires its image right after the previous present (-2.1). With B2 it is gated less often.
2. Before: A's whole frame goes out at S behind the release. After: the off-screen part goes out at -0.2,
   with 2.25 instead of 1.30 `QueueSubmit` per frame, and runs while the display still holds the image.
3. R comes at +2.0 to +2.6. Only A's window pass runs after it: E-R is 0.69 instead of 1.37.
4. B's throttle in this cycle shrinks from 1.6 to 0.6 ms. Of the frames that follow a gated frame, 50 %
   still wait, down from 85 %.

### Magma inproc

```
Magma inproc, before
  t(ms)      -3        -2        -1        0         1         2         3         4         5         6
             |---------|---------|---------|---------|---------|---------|---------|---------|---------|
  GL thr                         ===B rec===z====C rec===zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz=====D======
  apply thr                     =d=A cpu===S d==B cpu==S~~~~~~~~~~~~~~wait~~~~~~~~~~~~~~~
  display                                                              v  R
  GPU                                      .............A held............+++st+++###A#####B##
  fence A                                                                               E
  frame 2.76 ms = CPU 1.35 + throttle 1.29 (display 0.73, GPU after release 0.55); gated 42%; E-R 1.44

Magma inproc, after (A + B2)
  GL thr                        ====B rec===z====C rec===zzzzzzzzzzzzzzzzzzzzzzzz=====D======
  apply thr                     d==A cpuf==S d==B cpu==S~~~~~~~~~~wait~~~~~~~~~~~
  display                                                            v  R
  GPU                                   +++st+++ooooo                   ++++w+++###B###
  fence A                                                                       E
  frame 2.24 ms = CPU 1.36 + throttle 0.72 (display 0.51, GPU after release 0.21); gated 33%; E-R 0.74
```

- `mgl-srv-apply` dequeues, swaps and throttles (`eglSwapBuffers`/`QueuePresentKHR` and `queueBuffer`
  are on it, not on Thread-14).
- With present credit 1, the GL thread's swap N+1 waits for the apply thread's swap N to return. So the
  GL thread records one frame ahead and then sleeps through the apply thread's throttle. Its lane is drawn
  from that rule: every GL-thread wake is by `mgl-srv-apply`, within 0.5 ms of an apply swap returning.
- The apply thread reaches B's throttle 1.3 ms after A's swap, long before A's buffer is released. So most
  of the remaining wait is display time (0.51 of 0.72 ms). Only a less-gated buffer can cut that.

### Espryt monolith

```
Espryt monolith, before
  t(ms)      -3        -2        -1        0         1         2         3         4         5         6
             |---------|---------|---------|---------|---------|---------|---------|---------|---------|
  render thr     ===========A cpu======d===S ===========B cpu======d===S~~~wait~~~
  display                                                       v  R
  GPU                                      .........A held.........+++st+++##A#####B##
  fence A                                                                       E
  frame 3.77 ms = CPU 3.07 + throttle 0.69 (display 0.32, GPU after release 0.37); gated 51%; E-R 1.34

Espryt monolith, after (A + F)
  render thr       ==========A cpu=====fd==S =========B cpu=======d=S~~~wait~~~~
  display                                                            v  R
  GPU                                  +++st+++ooooo                    +++w+++###B###
  fence A                                                                      E
  frame 3.39 ms = CPU 2.85 + throttle 0.51 (display 0.27, GPU after release 0.24); gated 50%; E-R 0.71
```

- The flush (f) lands right before the lazy dequeue (d), 0.3 ms before the swap.
- The gated share does not change (51 to 50 %), because Espryt has no lever B.
- The frame gain (-0.38 ms) is about half throttle (-0.18 ms) and half CPU (-0.22 ms, from F).

### Espryt inproc

```
Espryt inproc, before
  t(ms)      -3        -2        -1        0         1         2         3         4         5         6
             |---------|---------|---------|---------|---------|---------|---------|---------|---------|
  GL thr                     ====B rec====zzzz====C rec===zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz======D======
  apply thr                  =====A cpud===S =====B cpud===S~~~~~~~~~~~~wait~~~~~~~~~~~~
  display                                                             v  R
  GPU                                      ............A held............+++st+++###A#####B##
  fence A                                                                              E
  frame 2.98 ms = CPU 1.74 + throttle 1.18 (display 0.64, GPU after release 0.53); gated 45%; E-R 1.38

Espryt inproc, after (A + F)
  GL thr                     ====B rec===zzzzz====C rec===zzzzzzzzzzzzzzzzzzzzzzzzzzzz======D=======
  apply thr                 =====A cpu=fd==S =====B cpu==d=S~~~~~~~~~~wait~~~~~~~~~~~
  display                                                                v  R
  GPU                                  +++st+++oooo                         ++++w+++###B###
  fence A                                                                           E
  frame 2.84 ms = CPU 1.80 + throttle 1.01 (display 0.72, GPU after release 0.29); gated 44%; E-R 0.78
```

The GPU part of the wait halves (0.53 to 0.29 ms). The display part stays (0.64 to 0.72 ms) and is now
most of the throttle. Espryt cannot cut it the way Magma does; see [Espryt](#what-espryt-cannot-do-the-same-way).

## Effect on latency

Definition: the frame's CPU start (Minecraft polls input there) to the moment the frame is on screen.

- Monolith start: the previous swap returns on the render thread.
- Inproc start: the GL-thread wake that starts the frame (after apply swap N-2 returns the credit). 77-88 %
  of presented inproc frames have such a wake. The rest, where the GL thread was not blocked, are left out.
- On screen: the end of SurfaceFlinger's actual display frame, which is the present fence. It matches the
  expected present within microseconds.
- A frame is matched to its display frame by BufferQueue frame number: the BLAST acquire, then
  `latchBuffer ... - N` inside SF `commit V`, then display frame V.

Median / p90, ms:

- Magma monolith: 28.5 / 32.0 -> 29.2 / 32.1
- Magma inproc: 28.4 / 33.9 -> 29.0 / 32.6
- Espryt monolith: 30.6 / 34.4 -> 29.2 / 32.7
- Espryt inproc: 31.8 / 34.5 -> 31.3 / 34.9
- For reference, an older MobileGlues trace (P15 c1c2, another session): 28.4 / 28.7.

The change is within about 1.5 ms either way, a quarter of a vsync. It is not the modest cut that was
expected. The throttle sits in the app part, which is only 2-3 ms (monolith) or 3-6 ms (inproc). The
rest is set by the compositor:

```
presented frame, Magma monolith after (medians; 1 char = 0.5 ms)
 t(ms)   0         5         10        15        20        25        30
         |---------|---------|---------|---------|---------|---------|
 app     ====S                                                          record, submit, queueBuffer (2.2)
 BQ          ..                                                         waits for SF to hand a buffer back (0.85)
 SF queue      a################################                        BLAST acquired; SF takes one buffer per commit (16.1)
 SF/HWC                                         L###################P   latch, compose, HWC; on screen at P (10.0)
```

1. **The compositor queue, about 16 ms (2.6 vsyncs).** BLAST keeps 4 buffers acquired
   (`acquireNextBufferLocked (a:3)` before every acquire). It refills on every release callback.
   SurfaceFlinger applies at most one buffer per layer per commit, and it logs `hasPendingBuffer` for
   this layer about once per commit (815 instants over 816 latches). So 2-3 buffers always wait in SF's transaction queue. This is the same on
   every arm and on MobileGlues.
2. **SF's own pipeline, 10 ms.** SF commits about 10 ms ahead of the vsync it presents on.
3. **Frame drops** happen only in the app's BufferQueue (MAILBOX replaces the queued buffer). No acquired
   buffer is dropped in SF. The presented share falls as fps rises: Magma monolith 54 to 44 %, inproc 45
   to 36 %.

**Lever B and latency.** Extra images under MAILBOX add no queued frames:

- BLAST still holds 4 buffers;
- BLAST acquire to SF latch is 15.7 -> 16.1 ms (Magma monolith) and 15.8 -> 15.9 ms (Magma inproc);
  Espryt, without B, moves by the same amount (15.8 -> 15.9 ms);
- the extra image only lengthens the app's free list.

Shorter latency would need a shallower compositor queue: fewer frames acquired ahead, or pacing the app to
the display. Both are separate decisions and are not measured here.

## The knob MOBILEGL_MAGMA_EXTRA_SWAPCHAIN_IMAGES

- Range 0-16, default 0. It requests `surface minImageCount + extra` images, capped at the surface maximum
  and never below the frames-in-flight hint. It is read once at swapchain creation.
- **Measured image counts on this device:** the surface minimum is 5. A request of 5 (extra 0) gives 6
  images, and a request of 7 (extra 2) gives 7 (logcat "Swapchain created ... imageCount", all runs). The
  platform rounds the MAILBOX request up to at least 6, so extra=2 is only one image more than the
  default. extra=1 would probably give no change; that was not run.
- **Memory:** each image costs a full-size RGBA8 colour buffer plus a D24S8 depth/stencil buffer of the
  same size (Magma's default framebuffer has one per image). At FCL's 1280x800 surface in these runs that
  is about 4.1 + 4.1 = 8.2 MB per image. At a native 2560x1600 surface it would be about 33 MB. Driver
  compression metadata adds a little.
- **Present mode caveat.** Under MAILBOX (what FCL gets at swap interval 0), extra images only lengthen the
  free list, as measured above. Under FIFO every image can hold a queued frame, so extra images add queued
  frames and latency, up to one vsync per image. It should not be raised under FIFO.
- **Trade-off, not decided.** At extra 2 it costs about 16 MB here and buys a third fewer gated frames, no
  latency, and most of Magma's gain over lever A alone. A larger value would gate less (see Q1) and costs
  more memory. The default stays 0 until the user decides.

## What Espryt cannot do the same way

EGL has no portable way to set a window's buffer count. The NDK `ANativeWindow` perform call is private,
and surface-control APIs fall under the platform-agnostic rule. So Espryt keeps lever A only, and its
remaining throttle is mostly display time.

The platform-agnostic option is to present from a separate thread that owns the window surface (EGL
context and surface current there, frames handed over as finished images). The throttle then blocks that
thread, not the GL or apply thread. It is the Espryt counterpart of moving `vkQueuePresentKHR` off the GL
thread on Magma. It is a possible next step, not decided and not measured. Like present credit 2, it lets
one more frame run ahead, so it needs the same latency check.

## Open questions, answered from the traces

**Q1. Why is the release still pending on about 35 % of Magma acquires with "7 images"?**

There are two reasons.

- Lever B added one image, not two. The default swapchain already has 6 images, not 5 (see above).
- The pending window is long. SF hands a buffer back about 8.0-8.4 ms before its release fence signals
  (`releaseBuffer` callback to R, gated acquires, relwin.py). The callback comes when SF composes the next
  frame, but the display keeps scanning out the old buffer until the next flip. At 2.2-2.7 ms per frame,
  3-4 dequeues fall inside that window.

Measured dequeue position after the release callback, for gated acquires:

- before, mostly the 1st (672 of 799 in monolith);
- after, mostly the 2nd (666 of 778), 4.0 ms after the callback, still inside the 8.4 ms window.

Clearing it needs the buffer to come back at about the 4th dequeue (monolith) or the 5th (inproc). That
is about 2-3 more images than extra 2 gives, with the memory cost above. This is inferred from the
positions and was not run. The old model assumed each extra image delays reuse by a full frame, starting
from 5 images, so it predicted below 20 % gated.

**Q2. Why does Magma monolith gain more (+33 %) than the model's +24 % bound for B alone?**

Nothing outside the throttle moved: frame = CPU + throttle still holds (2.43 + 0.23 = 2.66 ms, measured
2.67). There are two reasons.

- The +24 % bound came from the older traces' CPU of 2.86-2.9 ms (3.60 -> 2.9). At this baseline the
  presenting-thread CPU is 2.39 ms (reps) or 2.49 ms (trace). So removing the throttle alone allows up to
  +32 % (trace: 3.29 -> 2.49).
- The after arm also has lever A, and A did more than modelled. The model charged the window pass a full
  0.8 ms start latency plus the pass, about 0.95 ms after the release. Measured E-R is 0.69 ms, so the
  start for the small final batch is shorter than for a whole-frame batch. The GPU-after-release wait fell
  from 0.50 to 0.11 ms per frame. The display-before-release wait fell from 0.28 to 0.11, from fewer gated
  frames (B).

Ruled out: a changed CPU wait in acquire (`AcquireNextImageKHR` median 0.059 -> 0.052 ms) and a CPU cut (F
is Espryt-only; Magma CPU 2.49 -> 2.43 ms in the trace). The GPU idles less per gap (4.2 -> 2.1 ms) because
off-screen work arrives earlier. The trace shows that as a result of lever A, not as a separate cause.
Single trace per arm; the 3-rep fps ratio (1.30-1.41) and the trace (1.23) agree on direction, not exactly
on size.

## Data and scripts

These are not in the repo; they are in the session scratchpad under `p15/`:

- runs: `runs/pwlat/<arm>-r1/p15.pftrace`
- outputs: `pwlat/` (cycle-*, latency.txt, blast-*, relwin.txt, gexec-*, pairs-*, diagrams.txt)
- scripts: `pwait/` (cycle.py, pairs.py, gexec.py, sim.py, plus lat.py, blast.py, relwin.py, glwake.py and
  mkdiag2.py for this note)
