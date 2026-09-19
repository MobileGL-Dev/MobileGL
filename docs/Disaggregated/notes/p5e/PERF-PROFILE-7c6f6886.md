# The symbolized profile, and what run-ahead actually removes

Companion to `PERF-BASELINE-7c6f6886.md`. This is the number that phase's perf scout said had to be
measured before any optimisation was written, and it is now measured.

Capture: `p5d_profile_r3.sh`, Redmi `2f7cbe2e`, inproc, Minecraft 26.3-rc-3 in world, GPU pinned,
`simpleperf record -e cpu-cycles -p <pid> -g --duration 15`. 136,856 samples, 73.4 G cycles.
Symbolized against the **unstripped** `libMobileGL.so` from
`FCL/build/intermediates/merged_native_libs/fordebug/...`, not the APK copy.

## 1 Where the cycles are, by thread

| thread | share of process cycles |
|---|---|
| `Thread-17` (the client GL thread) | 47.3% |
| `mgl-srv-apply` (the apply thread) | 30.1% |
| `Server thread` (the JVM's) | 9.4% |
| `C2 CompilerThre` | 6.0% |

## 2 The client thread, inclusive

| inclusive | self | symbol |
|---|---|---|
| 35.56% | 0.41% | `ClientSession::EmitAndWaitTails` |
| **31.77%** | **31.58%** | **`SessionProducer::WaitForAppliedOrEventBacklog`** |
| 34.42% | 0.15% | `EmitIndexedDraw` |
| 31.19% | 0.09% | `EmitDrawRecord` |
| **18.03%** | 3.72% | **`MGPipeValidateForVerb`** |
| 16.88% | 0.06% | `DrawElementsInstancedBaseVertex_Backend` |

**31.6% of the client GL thread is spent spinning inside the lockstep wait**, almost all of it self
time — it is the spin, not work reached through the wait.

## 3 The apply thread, inclusive

| inclusive | self | symbol |
|---|---|---|
| 97.74% | **60.64%** | `ServerLoop::ApplyThreadMain` |
| 36.60% | 0.26% | `ServerLoop::DrainRing` |
| 34.73% | 0.17% | `PipeApplier::ApplyOne` |
| 22.13% | 0.48% | `ServerVerbSink::OnDrawVbo` |
| 14.10% | 0.17% | `DirectGLES::PrepareForDraw` |

**60.6% of the apply thread is self time in `ApplyThreadMain`** — the idle poll waiting for the next
record. Only ~37% is real applying.

## 4 Run-ahead removes TWO costs on the client, not one

The wait is the obvious one. The second is `MGPipeValidateForVerb`, and it is worth reading
`MG_Impl/Pipe/PipeFill.cpp:3219-3280` to see why:

```cpp
const Bool runAhead = ClientRunsAhead();
...
const Bool barriered = !runAhead || ClientVerbIsBarriered(verb, ctx);
const Bool fillOwed = barriered;
...
if (fillOwed) { /* the residual fill */ }
```

Under run-ahead, an **unbarriered** verb skips the residual fill entirely — `gPipeInputs` becomes
server-role memory for that record and the client neither fills it, stamps it, nor withdraws the
server's stamp. So the 18.03% is not merely reduced, it is **not executed** for unbarriered draws.

Today `ClientRunsAhead()` is false by construction: `RunAheadArmed()` requires
`Caps().HasCap(kCapRunAheadApply)`, which requires `kMGPipeP5eRunAheadReady`, which is `false` until
the integration commit.

## 5 The arithmetic, and the prediction

Measured client thread cost: 6.1 ms/frame. Of it, `EmitAndWaitTails` 35.6% and
`MGPipeValidateForVerb` 18.0% — about **53% of the thread** — are what run-ahead removes for
unbarriered records. Barriered records (readback, XFB, CopyTex, `set_storage_block_binding` per
ruling ID-84) keep both, but in this workload those are a handful per frame against ~849 draws.

| | now | predicted after run-ahead |
|---|---|---|
| client GL thread | 6.1 ms/frame | **~3.0 ms/frame** |
| apply thread | 5.3 ms/frame | **~2.0 ms/frame** |
| bound by | client | client |
| implied fps | ~164 | **~330** |
| monolith, same session | — | 3.9 – 4.8 ms/frame, 208 – 256 fps |

**So the honest prediction is not merely parity but past it.** The split arm's remaining client work
(frontend tracking plus record encode) is smaller than monolith's whole frame, because the backend GL
work has genuinely moved to the other core and the apply thread has ~2 ms of real work against a
~3 ms client.

This is a prediction from a profile, not a measurement of the end state, and it should be treated as
such until the flip lands and the four-arm matrix is re-run. The two things that could spoil it:

1. **The apply thread's 60.6% idle poll** is a spin too. If the client stops feeding it in lockstep
   and starts feeding it in bursts, that poll's behaviour changes and its cost must be re-measured,
   not assumed to fall proportionally.
2. **Core placement.** In this capture the split client sat on a mid core at 1555 MHz while the
   monolith render thread gets cpu7 at 1958 MHz, and the kernel ignores the library's affinity
   requests. That is a ~26% clock handicap the split arm pays for reasons outside this phase, and it
   is explicitly out of scope by instruction. Compare per-thread CPU ms/frame, which is clock-biased
   the same way in both arms only if both land on the same core class — state the core in every
   result (`p5d_bench_fcl4.sh` already records the `processor` field per thread).

## 6 What this means for the order of work

The two asks — "make the strict lane hard green" and "make inproc as fast as monolith" — are **one
task**. `GetProgramForDraw@DrawArrays` is read once per draw; a record that reads client memory
cannot be unbarriered; run-ahead cannot be armed until that read is gone. So the strict lane is not a
precondition imposed by process, it is the same work seen from the correctness side.

After the flip, the next target is visible in §2 and is NOT the wait: `MGPipeValidateForVerb`'s
residual 3.72% self plus whatever fill survives for barriered verbs, and the encode path
(`EmitIndexedDraw` / `EmitDrawRecord`) which is where the client's remaining ~3 ms lives.

## 7 The monolith arm, profiled the same way

Same capture recipe, `MOBILEGL_TRANSPORT=monolith`, 15 s. Its `Thread-17` is 62.0% of process cycles
(there is no apply thread to share with).

| inclusive | self | symbol |
|---|---|---|
| 46.35% | 0.24% | `DirectGLES::DrawElementsInstancedBaseVertex` |
| 30.28% | 0.27% | `DirectGLES::PrepareForDraw` |
| 15.64% | 1.37% | `TextureImpl::SyncNeccessaryTextures` |
| **14.45%** | 3.44% | **`MGPipeValidateForVerb`** |
| 7.02% | 0.88% | `BindCurrentProgramWithResources` |
| 4.92% | 2.64% | `MGPipeFillAccess::CopyField` |

**The push-monolith arm pays the residual fill too**, and that is not a surprise once stated: the
fill is gated on `ClientRunsAhead()`, which is false on monolith for the same reason it is false on
inproc today, so `fillOwed` is true and `MGPipeValidateForVerb` does the full copy. 14.45% of
monolith's thread, ~0.62 ms/frame at its measured 4.3 ms.

This sharpens the comparison rather than muddying it. After the flip, inproc's unbarriered draws stop
paying that cost while the monolith arm keeps paying it, because monolith never arms run-ahead
(ID-81 keeps its frontend arms and `kCapRunAheadApply` is a transport-side bit). So the projected
end state is:

| | client thread ms/frame |
|---|---|
| monolith (unchanged) | 3.9 – 4.8, including ~0.62 of residual fill |
| inproc after run-ahead (projected) | ~3.0 |

The split arm should end up ahead. Note also what this says about the pull build, which has no fill
at all: part of what the push build costs BOTH arms today is machinery that only the split arm will
eventually stop paying.

