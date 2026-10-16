# fix-aba v1 — the handle-ABA negative control, made to control something again

Tree `~/w7/p2-fix-aba`, branch `p2/fix-aba`, from `feat/disaggregated @ 7a2e2561` (all six P2
packages merged). **One commit, not pushed: `c52364cc`**
`[Fix, Test] (Magma, MG_IntegrationTest): make the handle-ABA negative control construct its own
collision and defeat the {slot, gen} generation`
(`MobileGL/Config.h`, `MG_Backend/DirectVulkan/Renderer/{MagmaPipeArms.h, VertexInputStateFactory.cpp,
VulkanRenderer.cpp}`, `MG_IntegrationTest/{CMakeLists.txt, Scenarios/HandleRecycleScenario.cpp}` —
6 files, +231/−69).

---

## 1. The defect, and the diagnosis that turned out to be wrong

The brief for this round said the arm had gone vacuous because package D re-keyed
`VertexInputStateFactory` / `VaoDrawMemo` on `{slot, gen}`, so the generation made the VAO memo
ABA-safe by construction and defeating the two old guards no longer produced the corruption.

**That is not what was happening, and it is worth saying plainly because the real cause is worse.**
The `AbaControl` lane runs with `MOBILEGL_PIPE_PUSH=0` (`MG_IntegrationTest/CMakeLists.txt`,
`MGL_ITEST_VULKAN_HANDLE_ABA_ENVIRONMENT`), so `MagmaPipeTrackHArmIsHandles` answers false and the
`{slot, gen}` arm is **not executed in that lane at all**. Both of the knob's consumers were on the
pre-handle arm and both were being reached. The corruption failed to appear for two reasons that
have nothing to do with the re-key, and both of them predate package D — the arm only started
*failing* (rather than silently skipping) when package D landed the knob's consumer and the CMake
capability probe armed the lane.

### 1.1 The heap block is never handed back, so "hash the raw address" collides with nothing

D18 wrote the control as *"`VertexInputStateFactory::ComputeHash` hashes `attr.Buffer.get()`
instead of `GetLifetimeId()`; `LookupVaoDrawMemo` skips the `vaoLifetimeId` compare"*, on the
theory that a replacement object lands on the freed heap block and so reproduces the key.

Measured with a temporary `MGLOG_I` probe in `ComputeHash`, `LookupVaoDrawMemo`,
`TryBindResolvedVertexBindings` and `UploadAndBindVertexBuffers`, on the failing lane:

```
ABAPROBE ComputeHash: vao=0x55cf93cb6c90 loc=0 buf=0x55cf93c83a90 key=55cf93c83a90
ABAPROBE Upload:      vao=0x55cf93cb6c90 lid=2 hashKnown=1 hash=56aa84be1c84c8d0 frameSerial=3
ABAPROBE Lookup:      vao=0x55cf93cb6c90 lid=2 idx=981  first.key=(nil) ...
...
ABAPROBE ComputeHash: vao=0x55cf94072540 loc=0 buf=0x55cf93c8d9b0 key=55cf93c8d9b0
ABAPROBE Upload:      vao=0x55cf94072540 lid=3 hashKnown=1 hash=234db5c88488c14b frameSerial=6
ABAPROBE Lookup:      vao=0x55cf94072540 lid=3 idx=360  first.key=(nil) ...
```

The GL **names** are recycled — `TheReproducerRecyclesEveryName` passes, and the case's own
`vao 2 -> 2, buffer 2 -> 2` check passes — but the **addresses are not**: the replacement VAO is at
`0x55cf94072540`, ~3.8 MiB from the dead one, and the replacement buffer is at a different address
too. So the "defeated" key hashes differently *and* indexes a different memo slot (981 vs 360): the
replacement inherits nothing, and the arm asserted stale pixels while looking at correct ones.

A second probe printed `sizeof(VertexArrayObject) == 3920` and showed the object really is destroyed
at the right moment (the `lid=2` destructor fires before `lid=3` is created), so this is not a
leak — it is glibc's binning. 3920 bytes is past the tcache, so the chunk goes to the unsorted bin
and is split by the very next allocation the replacement path makes (`MakeQuadBuffer`'s
`BufferObject` and its 120-byte shadow). Four create/delete cycles in one run of the file produced
four distinct addresses about a mebibyte apart.

The file's own header had written this risk down — *"on a run where the allocator returns the name
but not the block … AbaControl expects the corruption and FAILS"* — and prescribed the fix:
*"a stronger address-reuse proxy … and not a looser assertion."* That is what this round does.

### 1.2 …and even with the addresses recycled, the reproducer could not have shown it

The only backend structure that carries a **GPU slice** rather than a layout is
`VulkanRenderer::ResolvedVertexBindings` (`VaoDrawMemo::bindings`), and
`TryBindResolvedVertexBindings` declines it across frames by design:

> `// NO cross-frame trust: a memo recorded in an earlier frame declines here and the draw
> // re-resolves through the full acquire path.`

Everything else a recycled identity can poison is layout — `BackendVertexInputState`
(`bindingBufferKeys` are never dereferenced; the resolve loop reads `vao.GetAttribute(location)`
live), `VaoBackendMemos`, `VaoDrawMemo::{layoutHash, layoutAuxMasks}` — and both objects are
byte-identically configured by construction, so a layout inherited from the wrong one is invisible
in pixels. This is the same negative result magma-v3 §2.5 reported for its probe B.

The scenario's `DrawQuadAndRead` reads back and calls `EndFrame()` after **every** draw, so the
arming draw and the recycled draw were always in different frames. A second experiment
(temporarily rewriting the case to recycle inside one frame, keeping the address-based defeats)
confirmed both halves at once: the frame serial does stay put across two draws in one frame
(`frameSerial=10` for both), and the addresses still did not repeat
(`vao=0x…333640` → `0x…42ff50`), so it was still `notred`.

**Net:** the control had two independent holes, and closing either one alone leaves it broken.

---

## 2. What the knob now defeats, and why the control is still a control

`Features.PipeHandleAbaControl` (`MOBILEGL_PIPE_HANDLE_ABA_CONTROL`, `#if MOBILEGL_PIPE_PUSH`, off
by default) now means: **replace the object identity in DirectVulkan's vertex-input memo keys with
a constant, on whichever arm the run is on.** One question, asked by every site, so they cannot
disagree — the same discipline `MagmaPipeTrackHArmIsHandles` already has:

`MagmaPipeArms.h` — `MagmaPipeAbaControlDefeatsIdentity()` and `kMagmaPipeAbaControlSlotIndex`.

| site | what the control does | which guard that is |
|---|---|---|
| `VertexInputStateFactory::ComputeHash` | `bufferKey = 0` for every bound buffer, **after** the arm has chosen its key, so it defeats the pre-handle lifetime id *and* the handle arm's `{slot, gen}` | the content hash's proof "this memoised binding still reads the buffer it was resolved from" |
| `VertexInputStateFactory::MemosFor` (handle arm) | one entry for every VAO, claimed without the `Owner == handle` compare | precisely "the slot was recycled and `Gen` did not move" |
| `VulkanRenderer::LookupVaoDrawMemo` | one entry, handed back **uncleared**, ahead of both arms | the legacy `(address, lifetimeId)` pair and the handle arm's `vaoHandle == handle` |

**Why a constant instead of D18's raw address.** The address is not recycled (§1.1), so that
spelling collides with nothing; a constant is the *strongest* form of "the allocator handed the
block back" and it is deterministic — the control no longer depends on an allocator nobody
controls. It also covers strictly more: on the handle arm the constant defeats the **generation**,
which is the whole of what makes the re-keyed memos ABA-safe. Under `MOBILEGL_PIPE_PUSH=0` the
handle arm is never executed, so D18's lane alone said nothing at all about the key P2 ships.

**What the control deliberately does NOT defeat, and this is what keeps it meaningful.** It touches
only *identity* guards. `TryBindResolvedVertexBindings` keeps its frame-serial gate, its
manager-wide slice-epoch compare, its per-binding live-buffer and per-resource slice-epoch compares
and its host-map check; `SetupDrawSnapshot`'s identity compare is untouched; nothing about the
tables' shape or capacity changes. So a green `AbaControl` arm still means *"a replacement object
was handed its dead predecessor's resolved vertex bindings **because** the identity halves of the
keys were defeated"* — not *"every safety net was switched off until something broke"*.

**The reproducer.** `AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput` now:

1. creates **both** buffers up front and draws with each, so the ABA window contains no buffer
   traffic — creating or first-touching a buffer moves `VkBufferManager`'s slice-epoch counter, and
   a moved counter sends the memo into a revalidation that is not an identity gate and would mask
   the ABA behind an unrelated guard;
2. warms up on the red VAO for `kWarmupFrames`, unchanged;
3. does the arming draw, the unbind/delete, the replacement's creation and the replacement's draw
   **inside one frame**, and reads back once — because `ResolvedVertexBindings` is same-frame by
   design (§1.2);
4. keeps the GL-name recycle check as a skip (it no longer constructs the AbaControl collision, but
   it is still what makes this a *recycle* rather than two unrelated objects, and it is what the
   `Handles` / `Legacy` arms assert is not enough to inherit anything);
5. `ExpectPixelsFor` now **prints what it observed** (`STALE` / `FRESH` / `NEITHER`) next to what
   the arm expected, on every arm, pass or fail.

**Lanes.** D18's lane is kept verbatim (`AbaControl`, `MOBILEGL_PIPE_PUSH=0`, pre-handle arm) and a
second one is added: `DirectVulkan.HandleRecycle.AbaControlHandles.`
(`MOBILEGL_PIPE_LEGACY_MEMOS=0`, default push mask, handle arm). Both set
`MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1`; both are registered in the pull build too (where they skip on
`MGITEST_PIPE_PUSH_BUILD`) so G2's name parity is unaffected. **Declared deviation from D18**, which
names one lane: with one lane the control cannot reach the arm P2 ships.

---

## 3. Verification — every command and its actual output

All from `~/w7/p2-fix-aba` at `c52364cc` unless stated. Driver scripts under the session scratchpad
`.../scratchpad/p2aba/`.

### V0 — the defect, before the change (`~/w7/logs/aba-baseline.log`)

```
$ ctest --test-dir build-push -R 'HandleRecycle' --no-tests=error -j 4 --output-on-failure
96% tests passed, 1 tests failed out of 28
  2468 - DirectVulkan.HandleRecycle.AbaControl.HandleRecycleScenario.AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput (Failed)

../MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:234: Failure
  Actual: false (… [AbaControl expects the STALE object's pixels: the two guards are deliberately
  defeated]: region x[2,126] y[2,94] should be all red, but 11625 of 11625 pixels (100%) are not;
  first offender at (2,2) is green rgba(0,255,0,255))
```

### V1 — requirement (1): all HandleRecycle entries pass (`~/w7/logs/aba-after.log`)

```
$ ctest --test-dir build-push -R 'HandleRecycle' --no-tests=error -j 4
rc=0
100% tests passed, 0 tests failed out of 32
```

28 → 32 entries: the four of the new `AbaControlHandles` lane. The three ambient `DirectGLES.*`
vertex-array/texture/framebuffer entries still skip ("runs only in its own lane"), as before.

### V2 — requirement (3): the stale/fresh decision, printed, under every arm (`~/w7/logs/aba-pixels.log`)

```
$ ctest --test-dir build-push -R 'HandleRecycle.*AVertexArrayAtARecycled' --no-tests=error -V
2452: [ HandleRecycle ] arm=Handles    expected=FRESH observed=FRESH (stale=red, fresh=green) - …   # DirectGLES  Handles          Passed
2456: [ HandleRecycle ] arm=Handles    expected=FRESH observed=FRESH (stale=red, fresh=green) - …   # DirectVulkan Handles          Passed
2460: [ HandleRecycle ] arm=Legacy     expected=FRESH observed=FRESH (stale=red, fresh=green) - …   # DirectGLES  Legacy           Passed
2464: [ HandleRecycle ] arm=Legacy     expected=FRESH observed=FRESH (stale=red, fresh=green) - …   # DirectVulkan Legacy          Passed
2468: [ HandleRecycle ] arm=AbaControl expected=STALE observed=STALE (stale=red, fresh=green) - …   # AbaControl (pre-handle) Passed
2472: [ HandleRecycle ] arm=AbaControl expected=STALE observed=STALE (stale=red, fresh=green) - …   # AbaControlHandles       Passed
```

`Handles` sees the replacement's green; `AbaControl` sees the dead VAO's red — **on both arms**.

### V3 — the control is load-bearing: the same lanes with the knob OFF must go red

```
control ON,  pre-handle arm  → observed=STALE   [  PASSED  ] 1 test.
control ON,  handle arm      → observed=STALE   [  PASSED  ] 1 test.
control OFF, pre-handle arm  → observed=FRESH   [  FAILED  ] 1 test.
control OFF, handle arm      → observed=FRESH   [  FAILED  ] 1 test.
```

So the corruption is produced by the defeated identity and by nothing else, and the guards — both
the retired ones and the shipped `{slot, gen}` — are what stop it.

### V4 — requirement (2a): the pull build is untouched (G1)

```
$ python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0
symbol-report: .text 10792579 -> 10792739 (+160, +0.001%)
symbol-report: 27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed
```

| symbol | before | after | delta |
|---|---|---|---|
| `MG_State::GLState::RenderState::RenderState()` | 1700 | 1848 | +148 |
| `MG_State::GLState::RenderState::SetCapability(CapabilityInput, bool)` | 850 | 927 | +77 |
| `MG_State::GLState::RenderState::IsCapabilityEnabled(CapabilityInput) const` | 239 | 268 | +29 |
| `_GLOBAL__sub_I_DirectGLES.cpp` | 1340 | 1331 | −9 |

**Exactly the four of INTEGRATOR-DECISIONS ID-3, unchanged in size, and nothing else — no delta
to explain.** Expected, and by construction: every library branch this round adds is inside
`#if MOBILEGL_PIPE_PUSH`, and the only pull-build TU touched at all is `Config.h`, where the change
is a comment.

### V5 — requirement (2b): names, and the rest of the suites

```
$ for d in build-linux build-push; do ctest --test-dir $d -N | grep -E "^[[:space:]]*Test[[:space:]]+#[0-9]+:" \
      | sed -E "s/^ *Test +#[0-9]+: //" | LC_ALL=C sort > names-$d.txt; done
$ diff names-build-linux.txt names-build-push.txt        # empty
pull/push names identical (2478 entries)                  # G2 name half

$ LC_ALL=C comm -23 names-before.txt names-build-linux.txt        # 0 removed vs the 2363 baseline (G14)
$ LC_ALL=C comm -13 names-before.txt names-build-linux.txt | wc -l
115                                                       # 111 from the P2 packages + 4 from this round
DirectVulkan.HandleRecycle.AbaControlHandles.HandleRecycleScenario.{TheReproducerRecyclesEveryName,
  AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput,
  ATextureAtARecycledAddressDoesNotInheritItsPredecessorsTwin,
  AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin}
```

| # | command | result |
|---|---|---|
| V5.1 | `cmake --build build-linux -j 12` / `build-push` | rc 0 both |
| V5.2 | `ctest --test-dir build-linux -L unit --no-tests=error -j 8` | **1562/1562** |
| V5.3 | `ctest --test-dir build-push -L unit --no-tests=error -j 8` | **1562/1562** |
| V5.4 | `ctest --test-dir build-push -L integration-gpu -R 'DirectVulkan' -j 1` | **455/455** |
| V5.5 | `ctest --test-dir build-linux -R 'HandleRecycle' --no-tests=error -j 4` (the **pull** build) | **32/32**, both push arms skipping on `MGITEST_PIPE_PUSH_BUILD` as designed |

### V5.6 — the `-j 8` caveat, and why it is not this change

`ctest --test-dir build-push -L integration-gpu -R 'DirectVulkan' -j 8` came back
`99% tests passed, 2 tests failed out of 455`:
`DirectVulkan.PrimGenReroute.PrimitivesGeneratedNoXfbScenario.TheRerouteIsActuallyArmedWhenTheEnvironmentPinsItOn`
and `DirectVulkan.PointSizeDemotion.PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn`.

Both pass alone, and the lane is 455/455 at `-j 1`. Isolated — **with this round's new lane not in
the selection at all**:

```
$ for i in 1..5; do ctest --test-dir build-push -R 'PrimGenReroute\.|PointSizeDemotion\.' -j 8; done
run 1: 94% tests passed, 1 tests failed out of 18
run 2: 89% tests passed, 2 tests failed out of 18
run 3: 100% tests passed, 0 tests failed out of 18
run 4: 94% tests passed, 1 tests failed out of 18
run 5: 100% tests passed, 0 tests failed out of 18
```

This is the hazard `MG_IntegrationTest/CMakeLists.txt` documents at its verify block: the library
opens its log `fopen(path, "w")`, so **every process in a lane truncates it**, and these two cases
*read* that log to prove their pin was honoured. Their lane-mates race them. Pre-existing, in the
integration-gpu lane P2 requires green, and worth a follow-up (give the arming case a lane and a log
of its own, the way the verify-arming case already has); not caused by, and not fixable from, this
change.

---

## 4. Open items for the integrator

1. **Declared deviation from D18**: two `AbaControl` lanes, not one, and the knob's meaning is
   "replace the identity with a constant" rather than "hash the raw `BufferObject*` / skip the
   lifetime-id compare". §1.1 is the measurement that forces it. `Config.h`, `MagmaPipeArms.h` and
   the scenario header all carry the reason at the site; **`BRIEF-P2.md` D18 and the G8 row still
   say the old thing** and want amending.
2. **G8 is stated against `build-verify`** (`ctest --test-dir build-verify -R 'HandleRecycle'`).
   This round used `build-push`, which is where the defect was reported. `build-verify` was not
   built or run here; it should be, before the gate is signed off.
3. **The `-j 8` arming-lane race (V5.6) is a live CI hazard** for `ctest -L integration-gpu`,
   independent of this change.
4. **`ResolvedVertexBindings` is the only memo that can express an object-identity ABA in pixels**,
   and it is same-frame. That is worth recording in `MEASUREMENTS.md`: it bounds what *any* future
   handle-ABA reproducer can assert, and it is why magma-v3's probe B (drop the `++Gen`) stayed
   green over 432 integration cases.
5. `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` still steers nothing on DirectGLES; Espryt's Track-H slice
   (subsystem 5) has **no** negative control of this kind. The `Handles` arm asserts its correctness
   positively, which is the same standing the Magma arm had before this round.
