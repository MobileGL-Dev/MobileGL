# P2 package C — `p2/espryt` (Espryt 0b, the first Track H slice) — result v1

Tree: `~/w7/p2-espryt`, branch `p2/espryt`, branched from the tag `p2/contract` (`9c6a8a25`).
Build dirs: `build-linux` (pull, the G1 build) and `build-push` (`-DMOBILEGL_PIPE_PUSH=ON`).
No `build-verify` (the tree script was invoked without the third argument, per the brief's
`bash ~/w7/wsl_p2_tree.sh espryt p2/contract`).

## 1. Commits

| sha | subject |
|---|---|
| `dc9b7237` | `[Feat] (Espryt): give the backend a dense {slot, gen} twin table beside the address-keyed registry` |
| `fbdaeef3` | `[Refactor] (Espryt): key every backend twin on {slot, gen} instead of the frontend object heap address` |
| `1e0f4d6d` | `[Test] (Espryt): pin the twin table identity contract - a reclaimed slot is a new handle and the stale one resolves to nothing` |
| `d714600a` | `[Fix] (Espryt): do not return a reference through a null slot pointer, and stop a comment claiming a memo that is no longer there` |

Files touched — all inside package C's ownership rows of the C.5 table:

- `MobileGL/MG_Backend/DirectGLES/SlotTables.h` (new)
- `MobileGL/MG_Backend/DirectGLES/Managers.h`
- `MobileGL/MG_Backend/DirectGLES/Managers.cpp`
- `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp`
- `MobileGL/MG_Test/SanityTest.cpp`

No file owned by another package was edited.

## 2. What landed

### `dc9b7237` — step e1

`SlotTables.h` carries `BackendSlotTable<StateObject, BackendObject, kKind>`: a `Vector<Entry>`
indexed by `MGPipeHandle::Slot`, `Gen`-validated, with `GetOrCreate` / `Find` / `FindByHandle` /
`HandleOf` / `ReclaimDeadSlots` / `CollectGarbage{IfNeeded,Now}` / `ForEachLive` / `LiveCount`.
The handle is minted by the contract's `MG_Pipe::MGPipeSlots().Acquire(kKind, GetLifetimeId())`,
so a GL name never enters a key and a recycled heap address cannot reproduce a handle.

`StateBackendObjectRegistry` keeps its name, its member signatures and all ~40 call sites and
becomes the two-arm façade `ARCHITECTURE.md:367` asks for. The arm is latched once per process by
`EsprytSlotTablesEnabled()` (`Managers.cpp`), reading `kMGPipeSubsystemEsprytSlots` out of
`MG_Config::Features.PipePush`; the bit clear together with `Features.PipeLegacyMemos == false`
leaves no arm at all and is `Fatal{PipeLegacyMemosDisabled}`.

Kind mapping (the six registries onto `MGPipeKind`, which has no `Program`/`VertexArray`/`Sampler`
members — `ARCHITECTURE.md` §2.3 is the source of the CSO-kind spellings):

| registry | kind |
|---|---|
| `g_backendVertexArrayObjects` | `VertexElementsCso` |
| `g_backendTextureObjects` | `Texture` |
| `g_backendFramebufferObjects` | `Framebuffer` |
| `g_backendProgramObjects` | `ShaderCso` |
| `g_backendSamplerObjects` | `SamplerCso` |
| `g_backendRenderbufferObjects` | `Renderbuffer` |

Because the handle comes from the shared client allocator keyed on the lifetime id, Magma's
subsystem 4 will resolve the *same* handle for the same VAO — the two Track H slices agree by
construction, and `DirectGLESSlotTable.TwoTablesOfTheSameKindAgreeOnOneObjectsHandle` pins it.

The kind is a template parameter **only in the push build** (`MGB_TWIN_KIND_PARAM` /
`MGB_TWIN_KIND_ARG`). A third template argument renames every instantiation's mangled symbol,
which G1 forbids; in the pull build the parameter, like the arm it selects, does not exist.

### `fbdaeef3` — steps e3 and e4

- `ResolveVaoTwin`, `SyncCurrentProgram`, `BindCurrentFBO`: the three `TwinLookupMemo`s are gone
  on the handle arm and are compiled only under `MOBILEGL_PIPE_LEGACY_MEMOS`. So are
  `OwnerEquals` and the memo template itself.
- `ResolveUnitSamplerBackend` compares `{slot, gen}` instead of owner-equality, keeping the
  "a miss is never cached" contract verbatim.
- `UnitBindingsSnapshot` / `CaptureUnitBindings` / `UnitBindingsUnchanged` stay on the backend
  (as D13 requires) and, under push, hold **lifetime ids** rather than `WeakPtr`s.
- `SyncTextureObjectToBackend` keeps the by-value twin copy (it is what holds the twin alive
  across the nested `SyncTextureViewToBackend`) but drops the second Find-or-create and the
  put-the-twin-back repair on the handle arm.
- The one direct-iteration site (`ScopedDetachedTextureFramebufferAttachments`) walks
  `ForEachLive`, which hands over a strong reference instead of the map key.
- `GetFramebufferBindingSlotFast` → `GetFramebufferBindingSlotChecked`; under push it is a plain
  `MGB_CTX->GetFramebufferBindingSlot(target)` and the cache does not exist. **This is the P1
  poison bypass closed** at all five call sites.

### `1e0f4d6d` — step e5

Five `DirectGLESSlotTable.*` cases in `SanityTest.cpp` against `BackendSlotTable` through a
stand-in state object, so none of them needs a `GLContext`, a driver or ES entry points. The
load-bearing one is the ABA: object dies → sweep frees the slot → the next object takes the same
slot with a moved `Gen` → the predecessor's handle resolves to **null**, not to the successor's
twin. The others pin "Gen moves only on reuse", "Find never mutates the table", the fixture's
copy/reset/restore shape, and the one-handle-per-object property above.

In the pull build the same five names are registered as visible `GTEST_SKIP`s, which is what
keeps G2 (pull and push list the same ctest entries) green — the same "a visible skip, never a
vanishing test" shape section C.4 specifies for `HandleRecycleScenario.Handles`.

## 3. Verification — commands run and what they printed

All from `~/w7/p2-espryt` with `CCACHE_BASEDIR=/home/swung/w7`.

| gate | command | result |
|---|---|---|
| build | `cmake --build build-linux -j 12` / `build-push` | both `rc=0` |
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | **Added 0, Removed 0, Renamed 0.** Resized 4: `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9. **All four are the contract commit's** — see §5.1; this package adds no resize. |
| **G2** | `ctest -N` on both build dirs, robustly extracted (see §5.2), `diff` | **empty** — 2372 names each. |
| **G5** | `awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' … \| sha256sum` for `p2/contract`, `HEAD` and `~/w7/p2-before-syncrenderstate.sha` | all three `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27` — `SyncRenderState` is not one line changed. |
| **G13** | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty |
| **G13** | `python3 scripts/check_include_closure.py` | `rc=0`, `4 probes, 0 skipped, 0 problem(s)` |
| **G13** | `python3 scripts/gen_pipe.py --check` / `--self-test` | `rc=0` / `rc=0` |
| **G14** | `comm -23 ~/w7/p2-before-ctest-names.txt <pull names>` | **empty** — nothing removed. Added: the contract's 4 `*.PlaceholderUntilTheOwningPackageFillsThisIn` stubs and this package's 5 `DirectGLESSlotTable.*`. |
| unit (push) | `ctest --test-dir build-push -L unit --no-tests=error -j 8` | `100% tests passed, 0 failed out of 1494` |
| unit (pull) | `ctest --test-dir build-linux -L unit --no-tests=error -j 8` | `100% tests passed, 0 failed out of 1494` |
| slot-table cases | `ctest --test-dir build-push -R DirectGLESSlotTable --output-on-failure` | `5/5 Passed` |
| integration, handle arm | `ctest --test-dir build-push -L integration-gpu -j 4 -R DirectGLES` | `446/446` (see §5.3 on a pre-existing flake) |
| integration, legacy arm | `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu -j 4 -R DirectGLES` | `446/446` |
| legacy residue | `grep -n 'OwnerEquals\|TwinLookupMemo\|g_fbSlotCache\|CollectGarbageIfNeeded' DirectGLES.cpp` + a guard-stack audit | every `OwnerEquals` / `TwinLookupMemo` / `g_fbSlotCache` hit is inside `#if MOBILEGL_PIPE_LEGACY_MEMOS` or the `#else` of `#if MOBILEGL_PIPE_PUSH`. The seven `CollectGarbageIfNeeded` calls are deliberately still unguarded — deviation §4.1. |
| retrace | `python3 ~/w7/retrace_gate.py --tree ~/w7/p2-espryt --lib build-push/libMobileGL.so --out ~/w7/retrace-out/p2-espryt2 -j 4 --only 'DirectGLES$'` | RETRACE_RESULT_PLACEHOLDER |

## 4. Deviations from the brief

### 4.1 BLOCKING — step **e2** (the explicit-destroy hook) was not implemented: it needs files another package owns

C.2 step e2 says to emit `delete_*` "when a frontend object's last `SharedPtr` drops, following
`BufferBackendOps`' shape". `BufferBackendOps` lives in
`MG_State/GLState/BufferState/BufferObject.h:76-103` and is fired from `BufferObject.cpp:39-41`;
the equivalent for the other six kinds has to be installed in
`MG_State/GLState/{TextureState,FramebufferState,RenderbufferState,SamplerState,ProgramState,VertexArrayState}/*`.
Every one of those files is given to **package B (tracker)** by the C.5 ownership table
(`MG_State/GLState/{VertexArrayState/*, FramebufferState/*, TextureState/*, BufferState/*,
SamplerState/*}` → B; `VertexArrayObject.h` → D). Confirmed by grep: only `BufferObject.h` has an
`OnDestroy` anywhere under `MG_State`.

**The brief contradicts itself here**: C.2's own "**Files**" line for this package lists only
`MG_Backend/DirectGLES/{DirectGLES.cpp, Managers.h, Managers.cpp}`, the new `SlotTables.h` and
`MG_Test/SanityTest.cpp` — none of which can carry e2.

Per the instruction "if you believe you must [edit another package's file], stop and record it as
a blocking deviation instead", e2 is **not implemented**. What was done instead, entirely inside
package C:

- the slot table keeps **one `weak_ptr` per entry**, used for exactly one thing —
  `ReclaimDeadSlots()` frees the slot (and the twin, and the driver storage it owns) once the
  frontend object is gone. It is a *liveness* signal, never an identity test; the key is
  `{slot, gen}` throughout.
- the seven `CollectGarbageIfNeeded` call sites (`DirectGLES.cpp:1223, 1533, 1898, 1899, 1900,
  2728, 2729` at the base ref) therefore **stay**, now driving `ReclaimDeadSlots()` on the handle
  arm at the same cadence (every 1024 ticks) and over a dense array rather than a hash map.

Consequences the integrator must know:

1. The ABA defence is complete and gated (`DirectGLESSlotTable.ARecycledSlot…`): a slot is only
   ever handed out again after it was freed, and freeing is what bumps `Gen`.
2. The **memory** half of the deliverable is not: `Managers.h`'s "~100 CTS cases' worth of dead
   gigabyte-sized objects stay allocated at once" is unchanged, because the death is still
   discovered by a sweep rather than announced.
3. `ARCHITECTURE.md:363`'s memo census cannot count the six registries' GC as deleted yet.
4. When e2 lands (as a follow-up owned by whoever owns `MG_State`), `ReclaimDeadSlots()` becomes
   the fallback path of an explicit `Destroy(handle)` and the seven sweep sites go away. The
   table needs no further change.

### 4.2 The three `TwinLookupMemo`s are replaced by the table's own lookup, which is a hash on the lifetime id, not an array index into the caller

D13 says the memos "existed only to avoid the hash probe". In the monolith that is only half
true: the *table* lookup is an array index, but resolving the frontend object to its handle still
goes through the allocator's `lifetimeId → slot` map, i.e. one hash. So the steady-state cost of
`ResolveVaoTwin` / `SyncCurrentProgram` / `BindCurrentFBO` is one hash on a monotone integer key
instead of one array probe plus an owner compare (memo hit) or one hash on an address plus a
weak-ptr compare (memo miss). It is **correct by construction** rather than defended, which is the
point of the slice, but the package makes **no CPU-cost claim** and none should be read into it.
Under split the handle is what the client already holds and the hash disappears; that is P3+.

### 4.3 `UnitBindingsSnapshot` holds lifetime ids, not `{slot, gen}`

D13 says its two `OwnerEquals` calls "become `{slot, gen}` compares". They cannot: a texture that
is *bound* but has never been *synced* has no twin and therefore no handle, so a handle-keyed
snapshot would read two never-synced textures as equal — a real staleness bug. The identity has to
exist before the twin does. A lifetime id is the client-side identity that does exist then, is
never handed out twice, and is an integer compare. Recorded as a deliberate departure.

### 4.4 The four `pDefaultFramebufferInfo->defaultFBO` identity compares are **not** retired

D13's last table row asks for `handle == kMGPipeDefaultFramebuffer` at `DirectGLES.cpp:1939, 2874,
2898, 6420`. Not done. Reason: the default framebuffer's frontend object is never registered in
the twin table (it has no backend twin), so `HandleOf` answers the null handle for it and a
`kMGPipeDefaultFramebuffer` compare would need the reserved handle to be minted and installed
somewhere — which is a decision about who owns the reserved slot, not a mechanical substitution.
The existing compares are correct today; leaving them costs nothing and touches nothing. Listed in
§6 as unfinished.

### 4.5 Some deletions are arm-split rather than outright, because G1 is stricter than D13 reads

D15 point 2 says the pull build must compile "textually today's code". Two sites needed the pull
text preserved verbatim under `#else` rather than the shared refactor D13 sketches, because the
shared form changed pull-build codegen and G1 admits no resize beyond the three `RenderState`
symbols:

- `ScopedDetachedTextureFramebufferAttachments`' walk. Hoisting the body into a lambda both arms
  call cost the constructor 691 bytes and added an out-of-line lambda `operator()` symbol; the
  push arm now carries that lambda and the pull arm carries the original inline loop.
- `UnitBindingsSnapshot` and its two helpers, duplicated under `#if MOBILEGL_PIPE_PUSH` / `#else`.

### 4.6 `retrace_gate.py` has no `--backend` or `--ssim` flag

C.2's verification block spells `python3 ~/w7/retrace_gate.py … --backend DirectGLES --ssim 0.99`.
The landed script (`~/w7/retrace_gate.py`) takes `--tree --lib --out -j --only`; the SSIM
threshold comes from the reference `build-retrace` `CTestTestfile.cmake` it parses. Run as
`--only 'DirectGLES$'`. Tree wins on the fact.

## 5. Where the tree contradicted the brief

### 5.1 The contract commit resizes a **fourth** pull-build symbol

`_GLOBAL__sub_I_DirectGLES.cpp` is 1340 → 1331 bytes (−9) at `p2/contract` with **no P2 package
changes at all** — verified by stashing this package's edits and rebuilding `build-linux`. G1 and
D15 admit exactly three resized symbols
(`RenderState::{RenderState, SetCapability, IsCapabilityEnabled}`), and C.0's own verification
block says "resized == exactly" those three. The cause is almost certainly D3.1: `DirectGLES.cpp`
has file-static `RenderStateParameters` objects (`RenderStateImpl::g_syncedRenderStateParameters`),
whose static-init code moves when the struct gains three default-initialised `Bool`s. **The
integrator must either widen G1's admitted set by this symbol or get package A to explain it** —
it is not attributable to package C.

### 5.2 The G2 / G14 `ctest -N` extraction in section A silently drops most tests

`grep -E '^\s+Test #'` does not match `ctest -N`'s padded output, which is
`  Test   #83: <name>` (three spaces) once the total test count is four digits. On this tree the
brief's command yields 1368 of 2372 names — it drops every test whose number is padded. That also
explains the mismatch with `~/w7/p2-before-ctest-names.txt` (2363 names): the baseline was
captured with a working extraction, so running the brief's command against it reports ~1000
spurious "removed" tests. Working form used here:

```sh
ctest --test-dir <dir> -N 2>/dev/null | sed -n 's/^ *Test  *#[0-9]*: //p' | sort
```

The integrator should fix the command in section A (G2 and G14) and in `D.3`.

### 5.3 A pre-existing integration flake, on **both** arms

`DirectGLES.PointSizeDemotion.PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn`
and `DirectGLES.UnlocatedIoBlocks.UnlocatedIoBlockScenario.TheEmulationIsActuallyArmedWhenTheEnvironmentPinsItOn`
failed intermittently under `-j 4` — twice in the first eight full-suite runs, then not in the
next six. It reproduced with `MOBILEGL_PIPE_PUSH=0` (the legacy arm, i.e. pre-P2 behaviour) as
well as with the handle arm, and each case passes 4/4 when run alone, so it is **load-dependent
harness flakiness, not this package**. Worth a separate look before D.3 treats a single red run as
a gate failure.

### 5.4 `git stash -u` in a P2 worktree destroys the copied trace fixtures

`wsl_p2_tree.sh` copies the 803 MB fixture set in and marks it `--assume-unchanged`. `git stash`
resets through that bit, so a stash/pop cycle leaves the LFS **pointer** files (131 B each) in
place and every retrace case then fails in 0 s with `Unrecognized archive format`. Recovery is to
re-run the copy and the `update-index --assume-unchanged`. Worth a line in the tree script or the
brief; it cost one full retrace run here.

## 6. Unfinished

1. **Step e2**, the explicit-destroy hook — blocked on file ownership (§4.1). This is the one
   scoped deliverable of C.2 that is not present.
2. The four `pDefaultFramebufferInfo->defaultFBO` compares are not replaced by
   `kMGPipeDefaultFramebuffer` (§4.4).
3. The seven `CollectGarbageIfNeeded` call sites are not deleted; they now drive
   `ReclaimDeadSlots()` (§4.1). D13 lists their deletion as part of e3.
4. No `build-verify` on this tree, so nothing here exercises `MOBILEGL_PIPE_VERIFY`'s
   compare-at-read against the handle arm. The `GetFramebufferBindingSlotChecked` change is
   precisely the one that should now make the verify read-hook fire at five previously bypassed
   sites, and that is worth an explicit check at D.3.
5. No device work, no `HandleRecycleScenario` (package E owns it), no measurement.
6. `MOBILEGL_PIPE_PUSH=ON` with `MOBILEGL_PIPE_LEGACY_MEMOS=OFF` is written for but never built
   here (every configured build has the option ON, and CMake forces it ON when push is off). The
   arms are `#if`-separated so it should compile; nobody has proved it.
