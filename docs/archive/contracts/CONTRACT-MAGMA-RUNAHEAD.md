# CONTRACT — Magma run-ahead

This extends CONTRACT-P5E to DirectVulkan after P5f and the Magma application-buffer
migration. The P5e rule F, wait-class table, reply ownership, event flow control,
present credit and one-way run-ahead latch remain the protocol. This document
supersedes the historical requirement that Magma never publish `kCapRunAheadApply`.
It does not make Magma a multi-process transport or complete every P7/P8 GL shape.

## Readiness and waits

Magma has an independent implementation-readiness constant in `MG_Backend/Init.cpp`.
Only the integration commit may enable it after the queued-state tests pass. A
ready server advertises the existing bit 10; `MOBILEGL_IPC_RUN_AHEAD=0` remains the
paired lockstep control. Unsupported backends/readiness=false advertise no bit.
The knob alone never arms run-ahead. A later caps snapshot may disarm, not arm it.

Unbarriered apply reads only records, server storage and immutable capabilities.
No frontend object/registry or legacy pointer getter is admitted by a barrier,
scope, or backend choice. Unsupported GL shapes keep named refusals. Reply rows,
explicit synchronization, control operations and present-credit exhaustion retain
their existing waits; a test that removes all waits would violate the protocol.

## GPU resource and submission order

1. Wire buffer updates use exact owned bytes. A busy store is updated by ordered
   GPU copy or a justified host-access wait; CPU readback does not revoke a slice
   already reserved for a following draw. Store identity includes generation.
2. Lazy texture upload/preserve submissions are ordered after earlier renderer
   recordings. Descriptor resolution prepares all active texture/image resources
   and framebuffer attachments before capturing the current command buffer; a
   later lazy upload cannot silently move bindings into an already submitted one.
3. CPU draw preparation can own future transient slices even before a draw is
   recorded. Idle-drain/arena rewind must not reclaim them during that interval.
4. Wire draw, clear and color blit retain every native framebuffer, render pass,
   view and descriptor pool until the submission that names it completes. Retirement
   is keyed to submit indices, not an assumed number of frames or client progress.
   Swapchain replacement/shutdown drains these after device idle, including abandoned
   recordings. Independent blits never overwrite a descriptor set still in flight.
   Wire sampled views and draw framebuffer objects may be reused within a frame slot:
   exact keys include native storage identity/generation and every view/window value,
   and the slot is cleared only after its actual fence completes. Overflow still uses
   submit-index retirement. A cache hit never skips the image dependencies or pass boundaries.
   Pipelines use renderer-lifetime, non-recycled identities interned from exact render-pass
   compatibility values. A creator's temporary native render-pass handle is not a wire
   pipeline cache key or an eviction proof for another compatible pass using that pipeline.
5. GPU writes retain their reverse notifications; readback waits for actual GPU
   completion and returns owned bytes. Removing a per-draw queue idle does not
   remove explicit GL synchronization or image/buffer memory dependencies.

## Evidence required before activation

Real Magma tests must hold apply at a known boundary while the client publishes
later commands, observe real sequence/ack counters, then verify pixels/bytes from
each queued state. Include resource rewrite/respecify/death-reuse, program/uniform
rebinding, GPU write/readback, texture ordering/promotion, and present credits 1/3.
The dedicated discovery set must execute completely without skips. A real
run-ahead-off control must fail the queued-progress assertion, then restore green.

Keep strict/dual-block and zero-residual gates; preserve pull G1 and test-name
ratchets. On Redmi, verify the installed library identity, actual `run-ahead ARMED`,
both roles, world rendering and liveness, and record a paired run-ahead-off arm.
Performance numbers are recorded with their workload/clock limits, not substituted
for correctness or inferred from the capability log alone.
