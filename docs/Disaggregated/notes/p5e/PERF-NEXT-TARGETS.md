# After run-ahead: there is no second lever

Client GL thread, inproc, ranked by SELF time (the same capture as `PERF-PROFILE-7c6f6886.md`):

| self | symbol |
|---|---|
| **31.58%** | `SessionProducer::WaitForAppliedOrEventBacklog` |
| 3.72% | `MGPipeValidateForVerb` |
| 3.43% | `MGPipeTracker::Update` |
| 2.77% | `Client::ReadDrawBindings` |
| 1.78% | `MGPipeSlotAllocator::FindByLifetimeId` |
| 1.55% | `BufferState::GetBindingSlot` |
| 1.36% | `MGPipeShaderBufferEmitter::EmitClass` |
| 1.30% | `change_protection` (kernel: the persistent-map dirty-page tracking) |
| 1.26% | `MGPipeFillAccess::CopyField` |
| 1.26% | `__memcpy_aarch64_nt` |
| 1.13% | `PipeWireEncoder::EncodeRecord` |

**One symbol is 31.6%. The next is 3.7%.** Everything after the wait is a long flat tail of
one-to-three-percent functions.

That has two consequences, and they are the honest answer to "optimise inproc until it matches
monolith":

1. **The projection in `PERF-CORRECTION.md` is sound.** There is no hidden concentration for the
   estimate to have missed — removing the wait really does leave a thread whose cost is the sum of
   many small things, which is what a ~3.9 ms/frame client looks like.
2. **Going PAST parity has no single lever.** After P5e there is no 10% function to delete. Reaching,
   say, 1.5x monolith would mean a long grind across the tail: the tracker walk, the draw-binding
   read, the slot allocator's lifetime lookup, the shader-buffer emitter, the encoder, and the
   persistent-map page tracking that shows up here as kernel `change_protection`. Each is worth one
   to three percent of one thread.

So the correct thing to promise is parity, and the correct thing to do after landing it is to
re-measure rather than to plan the grind in advance: with the wait gone, the tail's shares all change
denominator and the ranking above is not the ranking that will matter.

One more caution for whoever does re-measure. `change_protection` at 1.30% is kernel time inside the
mprotect-based dirty-page tracking P5d introduced for persistent maps, and `MOBILEGL_IPC_PERSISTENT_HASH_SUPPRESS=0`
plus `MOBILEGL_IPC_PERSISTENT_BLOCK_KB` are its A/B knobs. It is the one item in the tail with a
ready-made control arm, so it is the cheapest thing to attribute properly and the easiest to
mis-attribute by staring at self-percent.
