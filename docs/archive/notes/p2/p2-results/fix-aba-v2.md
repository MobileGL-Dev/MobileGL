# fix-aba v2 — closing review v1's MAJOR: the lanes cover the slot, a unit test covers the generation

Tree `~/w7/p2-fix-aba`, branch `p2/fix-aba`. Two new commits on top of `55d2af9b`
(`== c52364cc` byte for byte), **not pushed**:

| commit | subject |
|---|---|
| `9bd05ae4` | `[Fix, Test] (Magma, MG_Test): cover the {slot, gen} generation in a unit test that forces a real slot reuse, and stop claiming the ABA lanes do` |
| `c290693d` | `[Fix] (Magma, MG_IntegrationTest): drop the fourth ABA-control consumer, which was unreachable, and the scenario comment its own body contradicts` |

Files: `MG_Backend/DirectVulkan/Renderer/{MagmaPipeArms.h, VertexInputStateFactory.cpp,
VulkanRenderer.cpp}`, `MG_IntegrationTest/{CMakeLists.txt, Scenarios/HandleRecycleScenario.cpp}`,
`MG_Test/Pipe/{CMakeLists.txt, MagmaPipeIdentityTest.cpp}` (new).

---

## 1. MAJOR 1 is accepted in full, and independently re-measured

The reviewer is right and the claim was mine to make correctly. `AbaControlHandles` does **not**
defeat the generation in `{slot, gen}`, and it cannot:

- Magma's mint has no death notification — `grep -rn NotifyStateObjectDestroyed
  MobileGL/MG_Backend/DirectVulkan` is empty — so a slot returns to the free list only through
  `MagmaPipeIdentityTable::OnFrameBoundary`'s age sweep (`kSweepInterval = 256`,
  `kRetireAgeBoundaries = 1024`);
- `AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput` issues five
  `EndFrame()`s, so the replacement VAO acquires against an **empty free list** and gets a
  brand-new slot at `Gen = 1`. The knob-off FRESH verdict is decided by the **slot**;
- a real reuse needs ≥ 1024 idle boundaries after the dead object's last draw, which puts the two
  draws in different frames — and `ResolvedVertexBindings`, the only memo carrying a GPU slice
  rather than a layout, declines across frames by design. **The two requirements are mutually
  exclusive**, so no same-frame pixel reproducer can ever reach the generation for this memo.

**Re-measured here, not taken on trust.** With `++m_entries[index].Gen` (`MagmaPipeArms.h:365`)
commented out and `build-push` rebuilt:

```
ctest --test-dir build-push -R HandleRecycle  -j 4   ->  100% tests passed, 0 failed out of 32
ctest --test-dir build-push -L unit           -j 8   ->   99% tests passed, 3 failed out of 1566
```

All 32 integration entries — `Handles`, `Legacy`, `AbaControl`, `AbaControlHandles`, both
backends — stay green with the generation deleted. The three reds are three of the four cases
this round adds. That is the MAJOR, reproduced from the other side.

### 1.1 What the five sites say now

| site | now says |
|---|---|
| commit message (`9bd05ae4`) | states the correction, the mint's 256/1024 mechanics, the measured `slot=2 gen=1 / slot=3 gen=1`, and the 32/32-vs-1563/1566 re-measurement |
| `MagmaPipeArms.h` (`MagmaPipeAbaControlDefeatsIdentity`) | the constant defeats **the object identity that SELECTS the slot** — the key the handle arm ships — followed by a "WHAT IT DOES NOT COVER, AND WHY NO REPRODUCER OF THIS SHAPE CAN" block with the three bullets above and a pointer to the unit suite |
| `VertexInputStateFactory.cpp` (`MemosFor`) | "what the control defeats HERE is the identity that SELECTS the entry"; the generation half is named as the unit suite's business |
| `MG_IntegrationTest/CMakeLists.txt` | `AbaControlHandles` "defeats the object identity that SELECTS THE SLOT"; a new paragraph states that **neither** lane exercises the generation and no lane of this shape can |
| this file, §2 below | replaces v1 §2's "on the handle arm the constant defeats the GENERATION … which is the whole of what makes the re-keyed memos ABA-safe" |

Nothing about the lanes' behaviour changed — they were, and remain, a real control over the key
the handle arm ships. Only the claim changed.

---

## 2. Where the generation IS expressible: `MG_Test/Pipe/MagmaPipeIdentityTest.cpp`

Remedy (b) of the review, not (a): the paragraph alone would have left the shipped `++Gen` with
no test anywhere in the tree.

**No test-only hook was needed.** Driving the real sweep costs a 1280-iteration loop over
`OnFrameBoundary()` and runs in under a millisecond, so the suite exercises the production retire
path rather than a back door — no `MOBILEGL_BUILD_TEST` escape hatch, and nothing new in a
shipping build.

**Where it lives, and why.** Beside `SlotAllocatorTest.cpp`, which asserts the same identity
contract ("`Gen` moves on REUSE and never on respecify") for the *client* allocator. There is no
existing directory covering `VertexInputStateFactory` / `VaoDrawMemo` / Magma's mint;
`MG_Test/Backend/DirectVulkan` is registered only under `ENABLE_INTEGRATION_TESTS` and its two
targets need GLFW, a Vulkan loader and a device. This suite needs none of that —
`MagmaPipeArms.h` is header-only — and `MG_Test/Pipe` is registered unconditionally, so the four
cases are in `ctest -L unit` in all three build directories.

**The shape.** A keep-alive object holds the first allocatable slot and is touched on every
boundary, so (i) it is never retired and (ii) the slot under test is **not** slot index 0, the
one negative control C aliases everything onto — the reuse is therefore non-degenerate.

| case | asserts |
|---|---|
| `AnIdleSlotIsRetiredAndReusedWithANewGeneration` | the precondition, on its own so a failure reads as "the mint stopped reusing slots": after 1280 boundaries `LiveCount()` drops to 1, and the next `Acquire` returns the **same slot** with `Gen + 1` and mints no third slot |
| `AReusedSlotDoesNotServeItsPredecessorsMemo` | **knob OFF**: a memo stamped at `{slot, gen=N}` is not served at `{slot, gen=N+1}` — the entry comes back claimed for its new owner with a zero payload. The slot is asserted equal first, so only the generation can be doing the work |
| `TheAbaControlKnobServesTheStaleMemoAcrossAReusedSlot` | **knob ON**: the same entry comes back with the predecessor's payload, uncleared and unclaimed (`Owner` still null) — which is what makes the case above a control rather than a tautology |
| `ALiveObjectKeepsItsSlotItsGenerationAndItsMemo` | the respecify half: across two sweeps a live object keeps its handle and its memo, so a deleted `++Gen` cannot be "fixed" by bumping `Gen` on every acquisition |

**The claim rule is production code, not a copy of it.** `VertexInputStateFactory::MemosFor`'s
body moved into `MagmaPipeArms.h` as `MagmaPipeClaimSlotMemos(table, handle)` — the same three
branches (the control's alias, the `Owner == handle` compare, the clear-and-claim) — and
`MemosFor` is now one call to it. The suite calls the same function. Semantics are unchanged on
both paths; see §4 for the codegen check that says so.

### 2.1 The test is load-bearing (the `++Gen` deletion, with the committed assertions)

`MagmaPipeArms.h:365`, `++m_entries[index].Gen;` commented out, `build-push` rebuilt:

```
$ ctest --test-dir build-push -R MagmaPipeIdentity --output-on-failure
MagmaPipeIdentityTest.cpp:131: Failure   Expected equality of these values:  Which is: 1 / Which is: 2
MagmaPipeIdentityTest.cpp:132: Failure   Actual: true  Expected: false
[  FAILED  ] MagmaPipeIdentityTest.AnIdleSlotIsRetiredAndReusedWithANewGeneration

MagmaPipeIdentityTest.cpp:153: Failure   Expected: (second.Gen) != (first.Gen), actual: 1 vs 1
MagmaPipeIdentityTest.cpp:157: Failure   Expected equality of these values:  Which is: 57005 / Which is: 0
  the replacement inherited the dead object's memo out of the SAME slot: the generation is the
  only thing that separates {slot, gen=N} from {slot, gen=N+1}, and it did not
[  FAILED  ] MagmaPipeIdentityTest.AReusedSlotDoesNotServeItsPredecessorsMemo

MagmaPipeIdentityTest.cpp:183: Failure   Expected: (second.Gen) != (first.Gen), actual: 1 vs 1
[  FAILED  ] MagmaPipeIdentityTest.TheAbaControlKnobServesTheStaleMemoAcrossAReusedSlot

25% tests passed, 3 tests failed out of 4
```

`57005` is `0xDEAD`: the dead object's payload, served to its successor out of the same slot. The
fourth case stays green, correctly — it is about respecify, which the deletion does not touch.
The generation-check in the two knob cases is `EXPECT_NE` rather than `ASSERT_NE` on purpose, so
the memo assertion still runs and the failure names the inheritance rather than only the
precondition.

`++Gen` restored, `build-push` rebuilt: 4/4 pass, and `symbol_report.py --threshold 0` between
the pre-experiment library and the restored one reports `0 added, 0 removed, 0 resized, 0
renamed` — no residue (the files differ only in the linker's build id).

---

## 3. The two minors, closed (`c290693d`)

**Minor 1 — the fourth, unreachable knob consumer.** `VulkanRenderer.cpp`'s legacy `VaoDrawMemo`
compare read `MG_Config::Features.PipeHandleAbaControl` directly, bypassing the one-question
accessor. The control's early return fires ahead of both arms, so it could never be `false`
there. **Deleted** rather than routed: the `#if MOBILEGL_PIPE_PUSH` / `#else` pair is gone and
the lifetime-id compare is unconditional again, with a comment saying where the question *is*
answered and to use `MagmaPipeAbaControlDefeatsIdentity()` if that early return is ever narrowed
— which is the trap the dead line was.

**Minor 2 — the stale comment.** `HandleRecycleScenario.cpp`'s vertex-array header said "BOTH the
VAO and the buffer are recycled here so that a key built out of raw addresses matches"; the body
has contradicted that since `55d2af9b`. It now says only the VAO is recycled, that both buffers
are realised before the window on purpose (buffer traffic inside it moves `VkBufferManager`'s
slice-epoch counter, which is not an identity gate), and that what recycles is the GL **name**,
not the heap block.

Minors 3, 5 and 6 are **not** addressed this round — see §6.

---

## 4. What the knob costs the shipped push arm (review minor 4)

**Stated honestly first.** D18's consumer sat on the legacy path, after the handle arm had
returned, so the arm P2 measures carried no control branch at all. It now carries, per draw:

| site | asks per draw | on the hit path? |
|---|---|---|
| `VulkanRenderer::LookupVaoDrawMemo` | 1 | yes, every draw |
| `MagmaPipeClaimSlotMemos` via `MemosFor` | up to 3 (`TryGetMemoizedHash`, `GetOrComputeHash`, `GetOrCreateVertexInputState` ×2) | yes, every draw |
| `VertexInputStateFactory::ComputeHash` | 1 per **enabled attribute** | no — only on a VAO reconfiguration, which the memo above suppresses |

Each is a load of one `Bool` out of `MG_Config::Features` plus a never-taken, perfectly
predictable branch. Nothing was hoisted and no `[[unlikely]]` was added this round; the honest
description is "about four predictable branches per draw on the shipped arm".

### 4.1 Codegen: measured, exactly

`build-push` library with the knob present, versus the same tree with
`MagmaPipeAbaControlDefeatsIdentity()` compiled out (`return false;`). `symbol_report.py
--threshold 0`, so nothing is below the reporting floor:

```
.text 10872867 -> 10872611 (-256, -0.002%)
28031 -> 28031 defined symbols: 0 added, 0 removed, 6 resized, 0 renamed
```

| symbol | knob present | knob compiled out | delta |
|---|---|---|---|
| `VertexInputStateFactory::GetOrCreateVertexInputState(const VertexArrayObject&)` | 562 | 494 | −68 |
| `VertexInputStateFactory::GetOrComputeHash(...) const` | 303 | 255 | −48 |
| `VertexInputStateFactory::MemosFor(...) const` | 158 | 111 | −47 |
| `VertexInputStateFactory::TryGetMemoizedHash(...) const` | 249 | 210 | −39 |
| `VertexInputStateFactory::ComputeHash(...) const` | 480 | 444 | −36 |
| `VulkanRenderer::LookupVaoDrawMemo(const VertexArrayObject*)` | 459 | 434 | −25 |

**263 bytes across exactly six functions, all of them the vertex-input path, and no others** —
which is also the tightest available check that `MagmaPipeClaimSlotMemos` changed no semantics:
the extraction moved the rule into a template and the only symbols that moved at all are the ones
the knob is in.

### 4.2 Wall clock: attempted, and it cannot resolve the effect on this host

`DriverBench` `mc_vanilla_draw` (5495 draws/frame, 120 frames), magma backend, the two libraries
above, interleaved, `run_driver_bench.sh` with `MGL_EGL_VENDOR=50_mesa.json`,
`MGL_VK_ICD=lvp_icd.json`:

| rep | knob present `ns_per_op` | knob compiled out `ns_per_op` |
|---|---|---|
| 1 | 17141.7 | 17532.5 |
| 2 | 17268.1 | 17686.3 |
| 3 | 17392.8 | 17819.5 |
| 4 | 17533.4 | 17292.8 |
| 5 | 17253.4 | 17202.6 |
| **median** | **17268.1** | **17532.5** |

The arm that should be *faster* is slower at the median, which is the correct reading of "no
resolvable difference". The only GPU stack in this WSL tree is llvmpipe/lavapipe, so `ns_per_op`
is ~17.3 µs of software rasterisation per draw with a run-to-run spread of ±400 ns (±2.3 %),
against an expected effect on the order of 1 ns (0.006 %) — a noise floor roughly 400× the
effect. **Treat the wall-clock number as unmeasured.** The measurement that can see it is D.4.2's
device run on real hardware, or a `perf stat` instruction count; the codegen table in §4.1 is the
evidence this round actually has.

---

## 5. Gate re-runs, at `c290693d`

All three build directories rebuilt first (`cmake --build … -j 12`, rc 0 each).

| gate | command | result |
|---|---|---|
| G8 | `ctest --test-dir build-push -R HandleRecycle --no-tests=error -j 4` | **32/32** |
| G8 | `ctest --test-dir build-verify -R HandleRecycle --no-tests=error -j 4` | **40/40** |
| unit | `ctest --test-dir build-linux -L unit --no-tests=error -j 8` | **1566/1566** (the 4 new cases SKIP visibly in the pull build) |
| unit | `ctest --test-dir build-push -L unit --no-tests=error -j 8` | **1566/1566** |
| unit | `ctest --test-dir build-verify -L unit --no-tests=error -j 8` | **1566/1566** |

1562 → 1566 is exactly the four new `MagmaPipeIdentityTest` entries, and they are named in all
three builds (G2's name parity: the pull build declares them through the same
`MGL_..._TEST_LIST(X)` skip-list shape `CsoCacheTest.cpp` uses).

**G1, the pull build, `--threshold 0`:**

```
$ python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so \
      --after build-linux/libMobileGL.so --threshold 0
symbol-report: .text 10792579 -> 10792739 (+160, +0.001%)
symbol-report: 27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed

  MobileGL::MG_State::GLState::RenderState::RenderState()                       1700 -> 1848  (+148)
  MobileGL::MG_State::GLState::RenderState::SetCapability(CapabilityInput, bool) 850 ->  927  (+77)
  MobileGL::MG_State::GLState::RenderState::IsCapabilityEnabled(...) const       239 ->  268  (+29)
  _GLOBAL__sub_I_DirectGLES.cpp                                                 1340 -> 1331   (-9)
```

**Exactly INTEGRATOR-DECISIONS ID-3's four, at the same sizes as v1, and nothing else — no symbol
to attribute.** Expected: the new unit suite is a test target, `MagmaPipeClaimSlotMemos` is
`#if MOBILEGL_PIPE_PUSH`, the `VulkanRenderer.cpp` deletion removes a push-only branch, and the
rest of the round is comments.

---

## 6. Open items for the integrator

1. **`BRIEF-P2.md` D18 and the G8 row still want amending, and now to *these* words**, not v1's:
   the `AbaControl*` lanes are a control over the object identity that selects the memo slot; the
   generation is covered by `MG_Test/Pipe/MagmaPipeIdentityTest.cpp`. Two lanes, not one, remains
   a declared deviation from D18. v1 §4.1 (the "STALE on both arms" premise) is refuted and
   stands refuted.
2. **`MEASUREMENTS.md` should record why**: Magma's mint retires by age (256/1024) and the only
   pixel-visible vertex-input memo is same-frame, so a generation-only regression is out of reach
   of any `HandleRecycleScenario`-shaped reproducer. This is also the second, independent reason
   magma-v3's probe B stayed green over 432 integration cases; the result file's §4.4 attributed
   it to the cross-frame gate alone.
3. **Not addressed this round** (all from review v1, all pre-existing or cosmetic):
   - minor 3 — the GL-**name** allocator is still a dependency: `HandleRecycleScenario.cpp:547`
     skips if `glGenVertexArrays` does not hand the name back, and a skip in a control lane is a
     control that stopped controlling while `ctest` stays rc 0. `TheReproducerRecyclesEveryName`
     reds the same lane in that case, which is why it is survivable;
   - minor 5 — with the knob on, the aliased `VaoDrawMemo` entry can hand out `vkBuffers`
     belonging to whichever VAO last used slot 0. Confined to the two control lanes and bounded
     by the frame-serial gate;
   - minor 6 — the `AbaControl*` lanes do not set `MOBILEGL_PIPE_VERIFY=1`, so running G8 in
     `build-verify` proves the build, not the armed comparator. Pre-existing lane design.
4. **The `-j 8` arming-lane race** (v1 §V5.6, corroborated by review §5.7) is unchanged and still
   a live `integration-gpu` hazard independent of this work.
5. `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` still steers nothing on DirectGLES; Espryt's Track-H slice
   has no negative control of this kind, and now no unit-level generation test either. Espryt's
   mint is `MG_Impl/Pipe/SlotAllocator`, which `SlotAllocatorTest.cpp` does cover for `Gen` — but
   nothing joins that to a consumer memo the way §2 does for Magma.
