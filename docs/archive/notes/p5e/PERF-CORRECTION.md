# CORRECTION to `PERF-PROFILE-7c6f6886.md` §5 — run-ahead removes much less of the fill than §4 implied

§4 is right that an unbarriered verb skips the residual fill. It is wrong to treat
`MGPipeValidateForVerb`'s whole 18.03% as fill. The call tree under it (inproc, client thread,
percentages relative to that subtree) says otherwise:

| share of the subtree | child | skipped under run-ahead? |
|---|---|---|
| 22.05% | `MGPipeTracker::Update` | **no** — the tracker walk runs regardless |
| 20.48% | `MGPipeShaderBufferEmitter::EmitClass` | **no** — emitters PRODUCE records, which is the whole of what an unbarriered verb hands the server |
| 19.76% | `MGPipeFillAccess::CopyField` | **yes** |
| ~6.7% | the sampler / vertex-input / program / framebuffer emitters | **no** |
| remainder | self, including the unconditional prologue | partly |

`PipeFill.cpp:3226-3231` says this in words: "What still runs, unchanged, is the tracker walk and the
emitters (steps 2 and 3): those PRODUCE RECORDS, which is the whole of what an unbarriered verb is
allowed to hand the server." So only about a fifth of that 18.03% goes away, i.e. **3.6 – 4.5% of the
client thread**, not 18%.

## The corrected prediction

| | now | predicted after run-ahead |
|---|---|---|
| client GL thread | 6.1 ms/frame | **~3.9 ms/frame** (the 31.8% wait, plus ~4% of fill) |
| apply thread | 5.3 ms/frame | ~2.0 ms/frame |
| bound by | client | client |
| implied fps | ~164 | **~250** |
| monolith, same session | 3.9 – 4.8 ms/frame | 208 – 256 fps |

**The honest claim is PARITY, not past it.** The split arm should land level with monolith, with the
apply thread holding roughly 2 ms of headroom.

Going beyond parity is separate work, and §2 of the profile already names its targets: the emitters
and the encode path, which is where the client's remaining ~3.9 ms lives. The wait is not one of them
any more once run-ahead lands.

§5 of the profile note is left in place rather than edited, so the error is visible: it
double-counted the tracker walk and the emitters as removable and produced ~3.0 ms / ~330 fps. This
file supersedes it.

## Why this matters beyond the number

The first estimate was built by subtracting whole inclusive percentages of a symbol whose name
suggested it was all fill. That is the same class of error the previous phase recorded after
attributing an idle poll's cost to backend lock contention: **a percentage next to a plausible name
is not an attribution.** The call tree was three commands away and it changed the conclusion from
"comfortably ahead" to "level". Take the tree.
