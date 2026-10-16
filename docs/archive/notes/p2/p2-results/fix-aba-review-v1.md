# fix-aba review v1 — adversarial review of `c52364cc`

Reviewer ran everything below itself, in `~/w7/p2-fix-aba` (the tree has since been rebased onto
`2d690754`; HEAD is `55d2af9b` and `git show 55d2af9b --format="" == git show c52364cc --format=""`,
byte-identical, so every command below is equally a command against `c52364cc`). Builds used:
`build-linux` (pull), `build-push` (push), and `build-verify`, which this reviewer configured and
built (`-DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON`, rc 0) because the implementer
had not. Tree left unmodified (`git status --porcelain` empty; only `build-verify/` was added).

**Verdict: 1 major (a coverage claim that the lane does not deliver), 6 minors.** No functional
defect was found: the control works, it is load-bearing, it is deterministic, it touches only
identity guards, the pull build is untouched, and every gate command is green.

---

## 1. Verdict summary

| # | claim under review | outcome |
|---|---|---|
| 1 | the control is load-bearing on BOTH arms | **holds** for the vertex-array case; the review brief's own wording about the Texture/Framebuffer cases is **refuted** (§4.1) |
| 2 | the knob touches nothing but identity guards; pull build untouched; push build unchanged knob-off | **holds** (one vestigial 4th consumer, minor 1) |
| 3 | the reproducer is deterministic; the old one depended on an allocator that never obliged | **holds** (10/10, zero flips); "depends on no allocator behaviour" needs one qualification (minor 3) |
| 4 | the second lane is registered in every build shape the others are; G8 satisfied or restated | **holds**; G8's command is green in `build-verify`, and the restatement is declared — but what it restates to is **overstated** (MAJOR 1) |
| 5 | `build-verify`: `ctest -R HandleRecycle`, `ctest -L unit` | **holds**: 40/40 and 1562/1562 |
| — | P1's ScopedPipeVerb/no-verb concern: public GL only | **holds** |

---

## 2. MAJOR 1 — the `AbaControlHandles` lane does not defeat the generation, because the scenario never constructs a slot reuse

**Where the claim is made** (five places, all as fact):

- commit message: *"That is what makes the control cover the key P2 SHIPS … the old control said nothing
  about the generation in {slot, gen} - the whole of what makes the re-keyed memos ABA-safe"*
- `MobileGL/MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h:170-173` — *"on the handle arm the constant
  defeats the GENERATION in {slot, gen}, which is the whole of what makes the re-keyed memos ABA-safe"*
- `MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.cpp:102-105` — *"claimed without the
  Owner compare, which is precisely 'the slot was recycled and Gen did not move'"*
- `MobileGL/MG_IntegrationTest/CMakeLists.txt:834-836` — *"`AbaControlHandles` … defeats the {slot, gen}
  GENERATION, which is what makes the re-keyed memos ABA-safe"*
- `~/w7/notes/p2/p2-results/fix-aba-v1.md` §2, and §4.1's declared deviation, which is what the
  integrator will amend `BRIEF-P2.md` D18/G8 to say.

**Why it is not what the lane demonstrates.** Magma's mint has **no death notification**; slots are
returned to the free list only by an age sweep:

- `MagmaPipeArms.h:316-317` — `kSweepInterval = 256`, `kRetireAgeBoundaries = 1024`;
- `MagmaPipeArms.h:285-309` — `OnFrameBoundary()` is the **only** producer of `m_freeSlots`, and it
  sweeps only when `(m_boundary % 256) == 0`, retiring entries idle for more than 1024 boundaries;
- `MagmaPipeArms.h:327-367` — `ClaimSlot()` reuses a slot **only** from `m_freeSlots`; otherwise it
  appends a fresh entry with `Gen = 1`;
- nothing in `MobileGL/MG_Backend/DirectVulkan` consumes `NotifyStateObjectDestroyed`
  (`grep -rn NotifyStateObjectDestroyed MobileGL/MG_Backend/DirectVulkan` → no hits; the only site is
  `MG_State/GLState/VertexArrayState/VertexArrayObject.cpp:53`, which feeds the client-side tracker).

`AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput` issues **5** `EndFrame()`s
(1 primer + `kWarmupFrames`=3 + 1 ABA frame). So when the replacement VAO calls `Acquire`, the free
list is empty and it gets a **brand-new slot with `Gen = 1`**; the dead VAO's slot is still occupied.
The handle compare that the knob-off run passes is decided by the **slot**, and the generation never
participates.

Reproduce (standalone, outside the tree — the file is `slotprobe.cpp`, reproduced in §6):

```
$ clang++ -std=c++23 $(<flags from build-push/compile_commands.json>) \
      -I MobileGL/MG_Backend/DirectVulkan/Renderer -o /tmp/slotprobe slotprobe.cpp && /tmp/slotprobe
A) HandleRecycleScenario's shape (5 frame boundaries, then the replacement):
  primerVao (lifetimeId 1)     slot=1 gen=1
  redVao    (lifetimeId 2)     slot=2 gen=1
  greenVao  (lifetimeId 3)     slot=3 gen=1          <-- a NEW slot, not the dead one's
B) what a slot REUSE looks like (the ABA the generation exists to stop):
  first     (lifetimeId 1)     slot=1 gen=1
  second    (lifetimeId 2)     slot=1 gen=2          <-- needs >1024 idle boundaries
```

**Consequences.**

1. A regression that removes `++m_entries[index].Gen` (`MagmaPipeArms.h:343`) leaves **every** arm of
   this scenario green — `Handles`, `Legacy`, and both `AbaControl` lanes. That is the exact result
   magma-v3's probe B already measured (green over 432 integration cases) and which the result file
   §4.4 attributes to the cross-frame gate alone; slot non-reuse is the second, independent reason,
   and it survives the same-frame rewrite this round performed.
2. What the knob really defeats on the handle arm is the **whole handle compare** (`Owner == handle` /
   `vaoHandle == handle`), of which the generation is one field that this scenario can never make
   load-bearing. "Defeats the handle compare" is true and worth having; "defeats the generation, which
   is the whole of what makes the re-keyed memos ABA-safe" is not what the lane shows.
3. The failure shape is the same one this round was convened to fix, one level up: §1.1 of the result
   file correctly refuses to trust an unverified assumption about the **heap** allocator, and then
   makes an unverified assumption about the **slot** allocator in its replacement claim.

**And the two requirements are mutually exclusive as the code stands**, which the fix must face:
the pixel-visible memo is `ResolvedVertexBindings`, which declines across frames by design
(`VulkanRenderer.cpp:3635-3644`), so the ABA window must be one frame; a genuine slot reuse needs
≥1024 frame boundaries of idleness after the dead VAO's last draw, which necessarily puts the two
draws in different frames. So **no same-frame pixel reproducer can ever cover the generation** for
this memo — the claim is not merely unproven here, it is unreachable by this shape.

**Remedies (either is acceptable; the first is a paragraph).**

- (a) Restate at all five sites: the control defeats *the identity compare* (slot **and** generation
  together) on whichever arm is running, and record — beside result-file §4.4, in `MEASUREMENTS.md` —
  that a generation-only regression is out of this scenario's reach because Magma's mint retires
  slots by age (256/1024) and the pixel-visible memo is same-frame. Amend `BRIEF-P2.md` D18/G8 to
  the same words, not to the stronger ones.
- (b) Cover the generation where it is expressible: a unit test over `MagmaPipeIdentityTable` +
  `MemosFor`/`LookupVaoDrawMemo` that drives a real retire→reuse and asserts the memo is not
  inherited; that costs one `MG_Test` file and it is the only place a `++Gen` deletion can be
  caught today.

---

## 3. Minors

**Minor 1 — a fourth consumer of the knob, now dead, that bypasses the "one question" accessor.**
`VulkanRenderer.cpp:3737`:

```cpp
const Bool compareLifetimeId = !MG_Config::Features.PipeHandleAbaControl;
```

The control's early return at `VulkanRenderer.cpp:3653-3668` fires first, so this line can never be
`false`. It contradicts the commit message ("Three sites, all behind one question
(`MagmaPipeAbaControlDefeatsIdentity`)"), the result file's three-row table (§2), and the discipline
`MagmaPipeArms.h:151-154` sells ("three sites deciding separately could disagree"). It is also a trap:
narrow the early return later and this site silently returns to D18's retired semantics. Delete it
(restoring `constexpr Bool compareLifetimeId = true;` unconditionally), or route it through the
accessor and say why it is unreachable.

**Minor 2 — a stale comment the file's own doctrine forbids.**
`HandleRecycleScenario.cpp:478-482` still says *"BOTH the VAO and the buffer are recycled here so that
a key built out of raw addresses matches while the bytes behind it do not."* After this change the
buffer is **not** recycled: both buffers are created up front (`:494-495`) and neither is deleted
inside the ABA window (`:524-536`); only the VAO is. The header 30 lines above now says the opposite.

**Minor 3 — "the reproducer no longer depends on the allocator" needs one qualification.**
It no longer depends on the C++ heap allocator (correct, and well evidenced). It still depends on the
GL **name** allocator: `:547-550` skips when `glGenVertexArrays` does not hand the name back, and a
skip in an `AbaControl` lane is a control that stopped controlling while `ctest` stays rc 0. That is
survivable only because `TheReproducerRecyclesEveryName` (`:440-475`) reds the same lane in that case
— worth one sentence at `:543-546`, since the name check there now advertises itself as vestigial.

**Minor 4 — the knob costs the shipped push arm a per-draw branch it did not cost before.**
D18's consumer sat on the legacy path (`:3737`), after the handle arm had returned, so the arm P2
measures carried no control branch. Now `MagmaPipeAbaControlDefeatsIdentity()` is a global load +
branch at the top of `LookupVaoDrawMemo` (once per draw) and at the top of `MemosFor` (up to 3× per
draw: `VertexInputStateFactory.cpp:124,143,171,182`), plus one per enabled attribute in `ComputeHash`
(`:67`, which the old `else if` skipped entirely on the handle arm). Tiny, but P2's whole subject is
per-draw overhead and D.4.2 will measure it. `[[unlikely]]`, or hoisting the question once per draw,
would remove the question.

**Minor 5 — the aliased entry hands out `vkBuffers` belonging to whatever VAO last used slot 0.**
With the knob on, `LookupVaoDrawMemo` returns entry 0 for *every* VAO uncleared, so
`TryBindResolvedVertexBindings`' fast path (`VulkanRenderer.cpp:3602-3605`) can bind a stale
`entry.vkBuffers[...]` — including one whose `BufferObject` has since been deleted, if a lane's other
cases ever delete a buffer between two draws of one frame. It cannot happen in the three cases as
written (buffers outlive the frame), the frame-serial gate bounds it to one frame, and lavapipe is
green 10/10 — but it is a live-VkBuffer hazard confined to the two control lanes, and worth one line
next to `:3661-3664`'s "nothing else about the entry is relaxed".

**Minor 6 — G8's `build-verify` command does not arm the comparator over these lanes.**
The `AbaControl*` lanes do not set `MOBILEGL_PIPE_VERIFY=1` (only the `*.Verify.*` lanes do,
`MG_IntegrationTest/CMakeLists.txt:1012-1062`), so running G8 in `build-verify` proves the *build*,
not that the control survives an armed comparator. Pre-existing lane design, not this change; the
result file's item 2 ("`build-verify` should be run before the gate is signed off") is satisfied by §5
below, but the gate's prose implies more than the lane does.

---

## 4. Refutations of the review brief's own premises

**4.1 "the VertexArray/Texture/Framebuffer AbaControl cases see STALE pixels on both arms" — false,
and correctly so.** The knob steers DirectVulkan's *vertex-input* keys only. Only the vertex-array
case asserts the corruption (`:553`, `armExpectsCorruption=true`); the texture case passes
`armExpectsCorruption=false` (`:604`) and the framebuffer case never calls `ExpectPixelsFor` at all
(`:684-692`), so both expect FRESH in every arm — stated at `:570-571`. Measured in `build-verify`:

```
2468: [ HandleRecycle ] arm=AbaControl expected=STALE observed=STALE  - the draw after the VAO was recycled inside one frame
2469: [ HandleRecycle ] arm=AbaControl expected=FRESH observed=FRESH  - the draw after the texture was recycled
2472: [ HandleRecycle ] arm=AbaControl expected=STALE observed=STALE  - (AbaControlHandles) the VAO case
2473: [ HandleRecycle ] arm=AbaControl expected=FRESH observed=FRESH  - (AbaControlHandles) the texture case
```

So "with the knob off the AbaControl lanes FAIL" means **one entry of four per lane** fails; the other
three pass unchanged. G8's "ABA-control arm red" is carried by a single case.

**4.2 "the old version depended on the allocator handing back a 3920-byte block" — confirmed from the
diff.** Old `ComputeHash` set `bufferKey = (Uint64)(SizeT)attr.Buffer.get()`, and old
`LookupVaoDrawMemo` only dropped the lifetime-id half of `first.vaoKey == vao && first.vaoLifetimeId ==
lifetimeId` — so the *address* was still required to match on both the buffer and the VAO. The
recorded red is `~/w7/logs/aba-baseline.log` (`96% tests passed, 1 tests failed out of 28`, the
vertex-array entry, "should be all red, but 11625 of 11625 pixels (100%) are not").

---

## 5. Everything this reviewer ran

All from `/home/swung/w7/p2-fix-aba`. Logs under `~/w7/logs/rv/` (deleted at the end of this round
except where cited).

### 5.1 The four states (claim 1), reproduced without ctest so the knob can be removed

```
BIN=build-push/MobileGL/MG_IntegrationTest/MobileGLIntegrationTest
F=--gtest_filter=HandleRecycleScenario.AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput
COMMON="MOBILEGL_BACKEND_TYPE=DirectVulkan MGITEST_HANDLE_ARM=aba MGITEST_PIPE_PUSH_BUILD=1 \
        MGITEST_HANDLE_ABA_IMPLEMENTED=1 \
        __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
        VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
        MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH=1 MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS=1 \
        MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER=1"

env $COMMON MOBILEGL_PIPE_PUSH=0        MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1 $BIN $F   # A
env $COMMON MOBILEGL_PIPE_PUSH=0                                           $BIN $F   # B
env $COMMON MOBILEGL_PIPE_LEGACY_MEMOS=0 MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1 $BIN $F  # C
env $COMMON MOBILEGL_PIPE_LEGACY_MEMOS=0                                    $BIN $F  # D
```

| | arm | knob | printed line | result |
|---|---|---|---|---|
| A | pre-handle | ON | `expected=STALE observed=STALE` | `[  PASSED  ] 1 test.` |
| B | pre-handle | OFF | `expected=STALE observed=FRESH` | `[  FAILED  ]`, rc 1 |
| C | handle | ON | `expected=STALE observed=STALE` | `[  PASSED  ] 1 test.` |
| D | handle | OFF | `expected=STALE observed=FRESH` | `[  FAILED  ]`, rc 1 |

(D also proves the handle arm is genuinely entered: with `MOBILEGL_PIPE_LEGACY_MEMOS=0` a clear
Track-H bit 6 would have been a startup `Fatal{PipeLegacyMemosDisabled}` (`MagmaPipeArms.h:108-116`),
so the process running at all means bit 6 is set and `MagmaPipeTrackHArmIsHandles` is true.)

### 5.2 The knob's blast radius (claim 2)

`grep -rn "PipeHandleAbaControl\|MagmaPipeAbaControlDefeatsIdentity\|kMagmaPipeAbaControlSlotIndex"`
over the tree gives exactly four executable consumers, all `#if MOBILEGL_PIPE_PUSH`:
`VertexInputStateFactory.cpp:67` (`bufferKey = 0`), `:101` (`MemosFor` returns entry 0),
`VulkanRenderer.cpp:3653` (aliased `VaoDrawMemo`), and the vestigial `:3737` (minor 1). Plus
`Config.h:371` (the field), `ConfigLoader.cpp:267` (the parse), and comments.

Untouched by the diff, and verified by reading: `TryBindResolvedVertexBindings`' frame-serial gate
(`VulkanRenderer.cpp:3635-3644`), its manager-wide slice-epoch compare (`:3602`), its per-binding
live-buffer compare (`:3611-3613`), its per-resource slice-epoch compare (`:3624-3628`), its host-map
check (`entry.anyBufferMapped`, `:3602`) and `SyncPersistentMappedRange` (`:3621`); `SetupDrawSnapshot`'s
own identity compare (`:6360`) is also untouched. `m_vaoMemos[0]` is safe on an empty table:
`MagmaPipeSlotTable::operator[]` grows on demand (`MagmaPipeArms.h:430-436`).

Pull build (G1), `--threshold 0`:

```
$ python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so \
      --after build-linux/libMobileGL.so --threshold 0
.text 10792579 -> 10792739 (+160, +0.001%)
27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed
  RenderState::RenderState()                1700 -> 1848   (+148)
  RenderState::SetCapability(...)            850 ->  927   (+77)
  RenderState::IsCapabilityEnabled(...)      239 ->  268   (+29)
  _GLOBAL__sub_I_DirectGLES.cpp             1340 -> 1331   (-9)
```

Exactly ID-3's four, nothing else — reproduced independently of the implementer's run.

Push build, knob off:

```
$ ctest --test-dir build-push -L integration-gpu -R DirectVulkan --no-tests=error -j 1
rc=0    100% tests passed, 0 tests failed out of 455
```

### 5.3 Determinism (claim 3)

```
$ for i in $(seq 1 10); do ctest --test-dir build-push -R 'HandleRecycle\.AbaControl' \
      --no-tests=error -j 1; done
run 1..10: rc=0 | 100% tests passed, 0 tests failed out of 8      # 2 lanes x 4 entries, zero flips
```

Sampled with `-V`: `expected=STALE observed=STALE` on both `2468` (pre-handle) and `2472`
(`AbaControlHandles`) every time.

### 5.4 Lane registration and G2 (claim 4)

```
$ for d in build-linux build-push; do ctest --test-dir $d -N | grep -E '^[[:space:]]*Test[[:space:]]+#[0-9]+:' \
      | sed -E 's/^ *Test +#[0-9]+: //' | LC_ALL=C sort > names-$d.txt; done
$ diff names-build-linux.txt names-build-push.txt    # empty; 2478 entries each
$ grep -c HandleRecycle names-build-linux.txt        # 32, including all four AbaControlHandles entries
$ ctest --test-dir build-linux -R HandleRecycle --no-tests=error -j 4
rc=0    100% tests passed, 0 tests failed out of 32   (36 skip lines; the push arms skip)
```

The pull build's skip reason is the designed one (`HandleRecycleScenario.cpp:329`): *"the AbaControl
arm needs a library built with MOBILEGL_PIPE_PUSH, and this one was not …"*. CI needs no edit: the
G8 step selects by regex (`.github/workflows/test.yml:600`, `-R 'HandleRecycle|CsoContentAddressing'`),
so the new lane is picked up automatically.

### 5.5 `build-verify` (claim 5) — configured and built by this reviewer

```
$ cmake -S . -B build-verify -G Ninja ... -DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON
$ cmake --build build-verify -j 12                                   # rc 0
$ ctest --test-dir build-verify -R HandleRecycle --no-tests=error -j 4 --output-on-failure
rc=0    100% tests passed, 0 tests failed out of 40                   # G8
$ ctest --test-dir build-verify -L unit --no-tests=error -j 8
rc=0    100% tests passed, 0 tests failed out of 1562
$ for i in 1 2 3; do ctest --test-dir build-verify -L integration-gpu \
      -R 'HandleRecycle|CsoContentAddressing' --no-tests=error -j 4; done   # the CI step's shape
run 1..3: 100% tests passed, 0 tests failed out of 48
```

The configure log confirms both probes arm: *"DirectVulkan's vertex input is keyed on {slot, gen}"*
and *"MOBILEGL_PIPE_HANDLE_ABA_CONTROL has a consumer"* (both now resolve to `MagmaPipeArms.h`).

### 5.6 P1's ScopedPipeVerb / no-verb concern

`HandleRecycleScenario.cpp` includes only `<Harness/HeadlessGL.h>`, `<Harness/ScenarioFixture.h>`,
`<GL/gl.h>`, `<GL/glcorearb.h>` and five standard headers (`:106-123`); every operation in the three
cases is a `gl*` entry point or a harness helper that is itself public GL. No backend type, no
`MG_Pipe` symbol, no verb is named. Nothing regressed here, and the `build-verify` build compiles the
comparator alongside without a divergence (§5.5) — see minor 6 for what that does and does not prove.

### 5.7 The `-j 8` arming-lane race (result file V5.6) — independently corroborated as pre-existing

```
$ for i in 1 2 3; do ctest --test-dir build-push -R 'PrimGenReroute\.|PointSizeDemotion\.' \
      --no-tests=error -j 8; done
run 1: 89% tests passed, 2 tests failed out of 18
run 2: 89% tests passed, 2 tests failed out of 18
run 3: 94% tests passed, 1 tests failed out of 18
```

No `HandleRecycle` entry is in that selection, and the same lane is 455/455 at `-j 1`. The diagnosis
in V5.6 (a shared `fopen(path,"w")` log truncated by lane-mates) stands, and the hazard is not this
change's.

---

## 6. `slotprobe.cpp` (the MAJOR 1 probe, standalone, outside the tree)

```cpp
#include <cstdio>
#include "MagmaPipeArms.h"
namespace MobileGL::MG_Util::Debug { void Log(const char*, int, const char*, ...) {} }  // link stub
using namespace MobileGL::MG_Backend::DirectVulkan;
using MobileGL::MG_Pipe::MGPipeHandle;
static void show(const char* what, MGPipeHandle h) {
    std::printf("  %-28s slot=%u gen=%u\n", what, h.Slot, h.Gen);
}
int main() {
    std::printf("A) HandleRecycleScenario's shape (5 frame boundaries, then the replacement):\n");
    MagmaPipeIdentityTable t("VertexElementsCso");
    show("primerVao (lifetimeId 1)", t.Acquire(1));
    show("redVao    (lifetimeId 2)", t.Acquire(2));
    for (int i = 0; i < 5; ++i) t.OnFrameBoundary();      // primer + 3 warm-ups + the ABA frame
    show("greenVao  (lifetimeId 3)", t.Acquire(3));       // redVao died here; the table is not told
    std::printf("B) what a slot REUSE looks like (the ABA the generation exists to stop):\n");
    MagmaPipeIdentityTable t2("VertexElementsCso");
    show("first     (lifetimeId 1)", t2.Acquire(1));
    for (int i = 0; i < 1281; ++i) t2.OnFrameBoundary();  // > kRetireAgeBoundaries, landing on a sweep
    show("second    (lifetimeId 2)", t2.Acquire(2));
    return 0;
}
```

Build flags are the `-D`/`-I` set of `VertexInputStateFactory.cpp` from
`build-push/compile_commands.json`, plus `-std=c++23` and `-I MobileGL/MG_Backend/DirectVulkan/Renderer`.
