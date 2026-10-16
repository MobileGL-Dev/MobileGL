# P3a package C — Espryt verification round (`espryt-v3`)

Branch `p3a/espryt`, worktree `/home/swung/w7/p3a-espryt`, **rebased onto `refs/heads/feat/disaggregated`
(`31e370be` = contract + dev merge + wire + client + the two version bumps)** and then **three new commits**.
**Not pushed.** Every file touched is still in C.2's list; `MultiDraw.cpp` and `Utils.cpp` are untouched.

**This is the round in which the handle path executed for the first time against real emitters and real applier
bodies.** It found three defects, all in C.2's files, all fixed and all with a measured before/after. It also
found one red that is **not** this package's — 26 `BufferTest` unit cases — with a proof of ownership and the
exact one-line fix for the owner (§7).

| | |
|---|---|
| rebase | clean, **no conflict**, 16 commits replayed |
| new commits | `13b380fe`, `75ea7ee2`, `3c55e027` |
| HEAD | `3c55e027` |
| default-mask integration lane | **920 / 920 green** (was 204/463 red on the stub base, 13 red on the first run of this tree) |
| retrace | **79/79 push, 79/79 verify-armed, 0 Fatal** |
| still red | 26 `BufferTest` unit cases — package-boundary, §7 |

---

## 1. The rebase

```
git rebase refs/heads/feat/disaggregated          Successfully rebased and updated refs/heads/p3a/espryt
```

**No conflict, and this time no hidden trap either** (the ID-9 three-way-merge trap of the previous round was
about `Managers.cpp` and dev's `d7655247`; `31e370be` adds nothing in `MG_Backend`). All sixteen commits
replayed: `6a1a9bc7 → ed83424c` at the tip, `9ae4ec09/4db6bf1e → 9951961d` at the base. `git diff --stat`
against the new base names exactly `SlotTables.h`, `Managers.h`, `Managers.cpp`, `DirectGLES.cpp`,
`SanityTest.cpp`.

One environmental repair was needed before the retrace could run: **this worktree's
`tools/trace_replay/fixtures` were LFS pointers** (7.7 MB of 131/133-byte stubs against 803 MB in `~/w7/pipe`),
so the first retrace invocation reported `passed 2 / 79` with `rc=1  0s  ssim=None` on every Minecraft case —
a missing-fixture failure that reads exactly like a total regression. They were materialised by copying
`~/w7/pipe/tools/trace_replay/fixtures/.` over them; the 92 `assume-unchanged` entries were already set, so
`git status` stayed clean. **Recorded for the integrator: a package worktree's fixtures are not materialised by
`wsl_tree.sh`, and a retrace run against pointer files is a false red.**

---

## 2. The two cross-package items

### B-M3 — the `hostBytes` convention (client passes shadow base + offset)

**Already conformant; no change was needed, and the report that said otherwise was wrong about the code.**

`client-v2.md` §5 item 1 asks espryt to move its latch because the client sends `base + at` on
`resource_subdata` and `base + offset` on `resource_flush_range`. The handle arm has **always** subtracted:

```cpp
Ops_H_SubData    (Managers.cpp:1921):  if (bytes != nullptr) resource->hostBytes = static_cast<const Uint8*>(bytes) - offset;
Ops_H_FlushRange (Managers.cpp:1969):  if (bytes != nullptr) resource->hostBytes = static_cast<const Uint8*>(bytes) - start;
```

`git log -S` puts both lines in `e2` (`4e446501`, the original switch-over commit), not in any later one. The
subtrahends are the right ones: `offset` is `MG_Pipe::MGPipeSubDataBufferOffset(record)`, which
`MGPipeTypes.h:661` defines as the record's `UnionBox.X`, i.e. exactly the `at` the client adds
(`PipeFill.cpp:650` `MGPipeApplyResourceSubData(record, base + at)` beside
`MGPipeBuildSubDataRecord(handle, at, …)`); `start` is `record.Offset`, which the client adds at
`PipeFill.cpp:701`. `Ops_H_Respecify` takes the pointer verbatim because the client sends the true base there
(`PipeFill.cpp:640`), and `Ops_H_ResidentSubData` records nothing (D10), which matches the client's
`base + (at - offset)` staging pointer.

So `espryt-v1.md` §1's phrase "latches the client's shadow base" described the *value after the subtraction*,
which is what it is. **B does not change and C did not need to.** The behavioural half is now also observed
rather than argued: `CrossFrameBufferScenario`'s non-zero-offset flush and `StreamedArenaScenario`'s two
recycle cases — the shape `client-v2` named as the one that would catch it — are green in the default-mask
lane (§4, L1/L4), and they were green with the same code before and after this round, so nothing here is a
new claim.

### ID-13 — one `FlushPendingRangesNow`, and the handle arm's separate ladder

Confirmed, with the residual difference from ID-13's literal text stated rather than glossed:

* **Defined exactly once.** `grep -n FlushPendingRangesNow Managers.cpp` finds one definition
  (`:1316`) and two call sites (`:1723`, `:2872`), plus two comments. Package D's script exits **2** on a
  second definition, and it exited **0**.
* **Byte-identical to `5cb826b0`.** Its extracted-body sha is
  `c65570022a5856c1afccfd0cb42fefac707689f5bb3154c3a8e17d2495ccdf8e` at both refs.
* **`FlushPendingRangesFrom` is the separate ladder** (`:1051`, under `#if MOBILEGL_PIPE_PUSH`), with four
  call sites (`:1721`, `:2044`, `:2732`, `:2870`).
* **No hunk lands inside any of the ten.** `git diff 5cb826b0 HEAD -- Managers.cpp` is 16 hunks (`-U0`), and
  the gate below proves none of them touches a protected body.

**The one deviation from ID-13's wording, and it is `espryt-v2`'s declared D12, not a regression:** ID-13 says
"defined exactly once, **outside any `#if`**". The definition sits in the `#else` of
`#if MOBILEGL_PIPE_PUSH` (verified by walking the preprocessor nesting: guard stack at `:1316` is
`#else of #if MOBILEGL_PIPE_PUSH`), so a **push build contains no `FlushPendingRangesNow` at all** and its
legacy arm calls `FlushPendingRangesFrom` instead. Consequences, stated plainly so the integrator can rule:

* both gates ID-13 names are satisfied — G1 (the pull build's text and symbol set are untouched) and G5 (the
  extractor finds one definition and it is byte-identical);
* a push build contains **one** tier ladder, which is what ID-11's row exists for;
* the price is that in a push build the `MOBILEGL_PIPE_PUSH=0` / `0x7f` arms run
  `FlushPendingRangesFrom` rather than the frozen body. The only textual difference between the two ladders is
  `espryt-v2` m4's added `if (hostBase == nullptr) return;`, unreachable on the legacy arm. Both legacy arms
  are 920/920 green (§4).

Taking ID-13 literally instead would compile **two** ladders in every push build. This round did not change
the shape; it is recorded here because the task's wording ("defined exactly once, byte-identical … and the
handle arm's `FlushPendingRangesFrom` is the separate ladder") is met exactly, while ID-13's "outside any
`#if`" is not.

### Package D's G5 script, and whether it accepts a foreign cwd

**It does not, and an invocation that looks like it worked is a false pass.**
`scripts/p3a_untouched_regions.sh` self-locates: `REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$REPO_ROOT"`.
Running `bash /home/swung/w7/p3a-gates/scripts/p3a_untouched_regions.sh 5cb826b0 HEAD` from
`/home/swung/w7/p3a-espryt` exits 0 and prints "byte-identical between 5cb826b0 and HEAD" — but it has `cd`-ed
into `p3a-gates` first, so `HEAD` is **gates' HEAD**, not espryt's. The authoritative form, which needs no copy
of D's file in this tree, is to run it from gates' worktree against espryt's ref (the worktrees share
`/home/swung/w7/pipe/.git`, so `git show <ref>:<path>` reaches it):

```
cd /home/swung/w7/p3a-gates && bash scripts/p3a_untouched_regions.sh 5cb826b0 p3a/espryt      rc 0
  [p3a-untouched] the 10 pool / deferred-release / ring / flush-drain functions are
  [p3a-untouched] byte-identical between 5cb826b0 and p3a/espryt
bash scripts/p3a_untouched_regions.sh --self-test                                              rc 0
  positive control: 10 bodies extracted / an untouched copy compares equal / an edit outside the ten is invisible
  negative control: a perturbed ClearBufferPool body is reported, and named
  negative control: a perturbed FlushPendingRangesNow body is reported, and named
```

The ten shas are unchanged from `espryt-v2` §5.3.

---

## 3. The three defects this round found, and the fixes

The first execution of the handle path under real traffic left **13 failures** in the default-mask
integration lane (920 entries). Two of the thirteen (`DirectVulkan.PrimGenReroute.…ActuallyArmed…` and
`DirectVulkan.PointSizeDemotion.…ActuallyArmed…`) are the shared-log-file race `client-v2` §6 item 6 names and
appear only in a `-j 8` run; they are absent from every `-j 4` run and from the serial reruns, and neither
touches a file this package edits. The other **eleven** were real, and a mask bisect split them cleanly:

| mask | failures |
|---|---|
| `0x7f` (both subsystems off) | 0 |
| `0xff` (bit 7 resources only) | 5 — `XfbCaptureBufferReuse` ×4, `LargeArenaAdoption.RespecifiedIndexArenaKeepsVaoBinding` |
| `0x1ff` (both) | 11 — the five above plus `VertexAttribBinding` ×6 |

i.e. one defect in the vertex-input family and one in the resource family, plus a third that only the third
of them exposed.

### F1 — `13b380fe` — the vertex-buffer entry was resolved by the attribute's GL **binding point**, not by its **attribute index**

`Managers.cpp:3647` (the helper, renamed `VertexBufferForBindingIndex` → `VertexBufferForAttributeIndex`),
`:4070` (the call site).

```cpp
-                    VertexBufferForBindingIndex(st, attrib.BindingIndex);
+                    VertexBufferForAttributeIndex(st, attribIndex);
```

`MGPVertexBuffer::BindingIndex` and `MGPVertexAttribWire::BindingIndex` are two different numbers.
Espryt consumes **resolved** attributes, so the client emits `set_vertex_buffers` as one entry per attribute
slot with `MGPVertexBuffer::BindingIndex == the attribute index` (`VertexInputEmit.h:190-193`, `:229` —
"Espryt consumes RESOLVED attributes, so the set is one entry per attribute slot with BindingIndex == the
attribute index"), each carrying that attribute's already-folded buffer, stride and divisor.
`MGPVertexAttribWire::BindingIndex` is the GL **binding point** the attribute was attached to by
`glVertexAttribBinding` (`VertexInputEmit.h:325`: `vao.GetAttributeBindingIndex(i)`) and is the key of the
binding-point view — a view **this arm never reads**, because the resolution already happened on the client.
The helper's own comment already said the entries are keyed by attribute index; only the call site disagreed.

The two numbers coincide for every attribute configured through `glVertexAttribPointer`, which is why every
other scenario stayed green. They differ exactly in `KHR-GL43.vertex_attrib_binding`'s subject:
`glVertexAttribBinding(2, 3)`, `glVertexAttribBinding(0, 5)`, `glVertexAttribBinding(1, 0)` — the six
`VertexAttribBindingScenario` cases that failed, all of which read the disabled/zero default
(`point 0 attribute 2 is (0, 0, 0, 0)`) because the lookup found the wrong entry or none.

**Measured:** the six went to three immediately after this fix alone (the three fp64 ones survived to F3).

### F2 — `75ea7ee2` — the ensure path asked a descriptor no content call refreshes whether the shadow held bytes

`Managers.cpp:2759-2761` (`EnsureBufferResourceForHandle`).

```cpp
-            const void* initialData = record->Desc.HasDefinedContent != 0 ? hostBase : nullptr;
+            const Bool shadowHasContent =
+                bufferObject ? bufferObject->HasDefinedContent() : (record->Desc.HasDefinedContent != 0);
+            const void* initialData = shadowHasContent ? hostBase : nullptr;
```

`MGPResourceDesc::HasDefinedContent` states what was true at the last `resource_respecify` and **nothing
refreshes it afterwards**: `NotifySubData` (`BufferObject.cpp:85`), `NotifyFlushMappedRange` (`:101`),
`NotifyContentWrite` (`:127`), `MarkGpuWritten` (`:365`) and `LandBytesIntoResidentStore` (`:441`, `:459`) all
set the frontend's `m_hasDefinedContent` without re-emitting a descriptor, and re-emitting one per
`glBufferSubData` would be new wire traffic D-J forbids. The legacy arm never had the problem because
`RespecifyStorageNow` (`Managers.cpp:967-968`) reads `bufferObject.HasDefinedContent()` **and**
`bufferObject.MappedData()` — the source of the bytes and the statement about them come from the same object.
The handle arm paired the frontend's pointer with the applier's descriptor, and the two disagreed after the
commonest idiom in the corpus:

```
glBufferData(target, size, NULL, usage)     -> Desc.HasDefinedContent = 0
glBufferSubData(target, 0, size, data)      -> record.Serial moves; the twin queues the range
   (or: the twin does not exist yet, and the call is dropped by design - "lazy: the ensure path full-uploads")
draw -> EnsureBufferResourceForHandle: pendingRespecify -> RespecifyStorageWith(size, usage, NULL, serial)
                                       which CLEARS pendingRanges and STAMPS syncedChangeSerial
```

— an empty store, declared current, with the application's bytes dropped and no diagnostic anywhere. It is a
silent wrong-pixels defect, not a crash.

**Measured, with the instrumented build** (temporary `MGLOG_E` traces, removed before the commits):

```
before  TRACE respecifyWith id=1 size=16 data=(nil)         serial=2   extentChanged=1
after   TRACE ensure {2,0} … pendRes=1 init=0 ranges=0 synced=5 serial=8 content=1 host=0x5b73c1bad3c0
        TRACE respecifyWith id=2 size=4096 data=0x5b73c1bad3c0 serial=8 extentChanged=1
```

That first line is `XfbCaptureBufferReuseScenario`'s **vertex** buffer (`glBufferData(16, nullptr)` in the
fixture, then `glBufferSubData` in `SetVertex`): its store was created empty, so the capture program captured
zeros and every span read back `component 0 is 0, expected 10`. The second is
`LargeArenaAdoptionScenario.RespecifiedIndexArenaKeepsVaoBinding`'s final `glBufferData(4096, nullptr)` +
`glBufferSubData(elements)` after the 24 MiB adopted arena is retired — the element store came up empty and
the draw was black ("VAO did not fetch the replacement index store").

**Closed by this commit alone:** all four `XfbCaptureBufferReuse` cases and the `LargeArenaAdoption` index
case, i.e. every `0xff` failure.

The read is **declared**, exactly like C-1's `IsMapped()`: it is a frontend read this function keeps for P3a
(the signature already carries the object for D-N's `SyncPersistentMappedRange`, and C-2 already re-reads
`MappedData()` from it at every use) and it retires the moment the client publishes a live content flag beside
the descriptor. With no frontend object — the handle-only drains — the descriptor is all there is, and that is
what the `?:` says. **This is not the prohibited "re-read `MGB_CTX` to make a lane pass"**: no GL context is
read, the object is the one this function is already given, and the alternative (a twin-side
`hostContentDefined` mirroring six frontend setters, two of which — `NotifyContentWrite` on a resident store,
and the write-map path at `BufferObject.cpp:209` — emit no wire call at all) could not be proven equivalent
today. That alternative is the P5/P8 shape and is named in the comment.

### F3 — `3c55e027` — a lazily twinned store never published a shadow base, so the fp64 narrowing refused every draw

`Managers.cpp:2741` (`EnsureBufferResourceForHandle`).

```cpp
+            if (hostBase != nullptr) resource->hostBytes = hostBase;
```

`resource->hostBytes` is what the readers that hold **no** frontend object read, and there are three:
`Ops_H_Readback`'s queue flush (`:2048`), `Ops_H_FlushRange`'s kill-switch map arm (`:2005`), and
`SyncFloat64AttributeAsFloat32ByHandle` (`:4479-4482`, whose legacy counterpart reads
`bufferObject->MappedData()`). It is written by the content-carrying ops — but a twin is created **lazily**,
because D-A2's row makes `Ops_H_Create` a no-op, so the very first `resource_respecify` of a buffer finds
`FindBufferResourceForHandle == nullptr` and drops its base on the floor:

```
TRACE respecify {2,0} width=16 defined=1 bytes=0x5fdd854e5440 res=(nil) id=0 storeSize=0
```

For the ordinary static vertex array — `glGenBuffers` + `glBufferData(size, data)` and no further content
call — `hostBytes` then stayed **null for the object's whole life**. The full re-upload never noticed (it uses
`liveHostBase()`), but the fp64 narrowing did: `sourceBase == nullptr` → `return false` → the Adreno
workaround disables the array → the shader reads the disabled default `(0, 0, 0, 1)`. That is exactly what
`DoubleArrayIsFetchedAtFloat32Precision`, `NormalizedIsIgnoredForDoubleArrays` and
`LongDoubleArrayIsFetchedAtFloat32Precision` reported.

The refresh is placed where the base is already computed, which is the one place that both holds the object
and runs before every draw that uses the store. It is reached **only for a non-adopted resource** (the adopted
arm returns at `:2683`), so C-2's rule is intact: an adopted store's `hostBytes` stays null and its bytes are
read through `persistentPtr`.

**Closed by this commit:** the last three `VertexAttribBinding` failures. After it the whole lane is
920 / 920.

### Instrumentation hygiene

Five temporary `MGLOG_E("TRACE …")` lines (in `Ops_H_Respecify`, `Ops_H_Readback` ×2, `RespecifyStorageWith`
and `EnsureBufferResourceForHandle`) were used to obtain the traces above. They were **removed before any
commit**; the reconstruction of the three commits was diffed against the verified working tree and is
code-identical (the only textual difference is one re-wrapped comment line). `grep -n "TRACE " Managers.cpp`
is empty at HEAD.

---

## 4. Verification transcript

All in `/home/swung/w7/p3a-espryt` at `3c55e027`, `CCACHE_BASEDIR=/home/swung/w7`. All three build directories
were rebuilt before anything was judged. `build-verify` was configured with the COMMON flags of
`~/w7/notes/tools/wsl_p3a_gate.sh` plus `-DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON`.

### 4.1 Builds and unit

```
cmake --build build-linux  -j 24    rc 0
cmake --build build-push   -j 24    rc 0
cmake --build build-verify -j 24    rc 0

ctest --test-dir build-linux  -L unit -j 12    100% passed, 0 failed out of 1619
ctest --test-dir build-push   -L unit -j 12     98% passed, 26 failed out of 1619   <-- §7, NOT this package's
ctest --test-dir build-verify -L unit -j 12     98% passed, 26 failed out of 1619   <-- the same 26
```

The 26 are the whole and only red left on this tree. They are `BufferTest.*` and they are a test-fixture
collision at the client×espryt seam; §7 proves the ownership and gives the fix, which was measured green.

### 4.2 G1 — the pull build is still inert

```
python3 scripts/symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so \
        --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0            rc 0
symbol-report: .text 10806323 -> 10806323 (+0, +0.000%)   .data +0  .bss +0  .rodata +0
symbol-report: Total 17226471 -> 17226471 (+0)
symbol-report: 27811 -> 27811 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
```

**0 / 0 / 0 / 0.** `~/w7/p3a-before-head.txt` reads `b9eaa474…`, i.e. ID-8d's re-captured baseline (the pull
build taken after the two version bumps), which is this branch's base.

### 4.3 Names — G2's name half, and G14

```
ctest -N:  build-linux 2539   build-push 2539   build-verify 3371
diff <linux names> <push names>                                     -> EMPTY (0 lines)
comm -23 ~/w7/p3a-before-ctest-names.txt <linux names>              -> 0 removed
comm -13 ...                                                        -> 20 added
```

**0 removed.** The 20 added are this package's **two** (`DirectGLESBufferDrawProbe.ALiveHostMapKeeps…`,
`DirectGLESSlotTable.AGenerationBehindTheLiveTwinIsRefused…`, both running in `build-push` and **skipping
visibly** in `build-linux`) plus **18** from the merged client/wire (`ResourceEmit.*` ×7,
`VertexInputEmit.*` ×7, `TrackerWalk.*` ×3, `TrackerShippedEmitter.*` ×1). This round adds no test name.

### 4.4 G5 — the ten byte-identical functions

`rc 0`, see §2. Self-test `rc 0`, both negative controls tripped and named.

### 4.5 Purity

```
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'        -> empty
grep -c  'MG_State'   MobileGL/MG_Pipe/PipeApply.h               -> 0
git status --porcelain                                            -> clean
```

`g_pendingFetchBaseInstance` / `ScopedFetchBaseInstance` still have hits and every one is inside a
`MOBILEGL_PIPE_LEGACY_MEMOS` arm (`Managers.h:1191-1200`, `Managers.cpp:3523-3530`, `:3745`,
`DirectGLES.cpp:5293`) — D2, unchanged.

### 4.6 The integration lanes — every arm green

Run **serially**, no competing load (the protocol `client-v1` established for the
`…ActuallyArmedWhenTheEnvironmentPinsItOn` family).

| # | lane | result |
|---|---|---|
| L1 | `ctest --test-dir build-push -L integration-gpu -j 8` (**DEFAULT mask 0x1ff, both backends**) | rc 0 — **100%, 0 failed of 920** |
| L2 | the same with `MOBILEGL_PIPE_PUSH=0` | rc 0 — 100%, 0 of 920 |
| L3 | the same with `MOBILEGL_PIPE_PUSH=0x7f` | rc 0 — 100%, 0 of 920 |
| L4 | the buffer/VAO family, `-j 4 -R 'LargeArenaAdoption\|StorageBufferRegrow\|VertexAttribBinding\|MultiDraw\|PrimitiveRestart\|CrossFrameBuffer\|ResidentIndex\|BufferTexture\|AtomicCounter\|XfbCaptureBufferReuse\|PackedWordReadback\|DoublePrecision\|VertexArrayEnableDisable\|DrawParameters'` | rc 0 — **100%, 0 failed of 184** |
| L5 | `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1 … -j 8` (D.3's named kill-switch arm) | rc 0 — 100%, 0 of 920 |
| L6 | `ctest --test-dir build-verify -L integration-verify -j 4` | rc 0 — **100%, 0 failed of 832**, **`Fatal{` lines: 0** |
| L7 | `MOBILEGL_PIPE_PUSH=0xff` (bit 7 alone, the supported one-family A/B) | rc 0 — 100%, 0 of 920 |
| L8 | `MOBILEGL_PIPE_PUSH=0x17f` (bit 8 without bit 7) | rc 0 — 100%, 0 of 920, and the refusal fires (below) |

**`DoublePrecisionScenario` — the declared watch item — is green** in L1 and L4 and needed no fix. D7's
`SyncGpuWrites` gap on `SyncFloat64AttributeAsFloat32ByHandle` is still real and still declared; it is only
observable when the fp64 **source buffer** is shader-written and not yet pulled back, which no scenario in the
corpus does. What `DoublePrecisionScenario` and the three `VertexAttribBinding` fp64 cases *did* expose is F3,
which was a different defect on the same function (a null source base, not a stale one) and is fixed.

**L8 / espryt-v2 §6 item 10** — `0x17f` refuses, by name, and runs the legacy vertex-input arm:

```
[ERROR]: MGPipe: kMGPipeSubsystemVertexInput (bit 8) is set but kMGPipeSubsystemResources (bit 7) is clear;
         the vertex-input handle arm resolves attribute buffer ids through the resource slot table, which only
         bit 7 populates - REFUSING bit 8 and running the legacy vertex-input arm. Set bit 7 as well, or clear both
```

### 4.7 ID-10 — the base-instance control, both directions

The brief asks for the base-instance scenarios green on this tree and red with the three
`MG_Impl/GLImpl/Drawing/GL_Drawing.cpp` call sites stubbed out. **The plain form of that control is not
falsifiable on this host, and the reason is a capability, not a defect:**

```
[INFO]:  base instance (EXT_base_instance; emulated by attribute offsets when absent): yes
```

llvmpipe exposes `GL_EXT_base_instance` and all three entry points, so
`BackendUsesNativeBaseInstance()` (`Managers.cpp:3569` = `g_GLESCapabilities.SupportsBaseInstance`) is **true**
and `Managers.cpp:4044` forces `fetchBaseInstance` to `0` — the attribute-offset emulation, which is the
**only** consumer of `MGPipeApplierState::VertexFetchBaseInstance` anywhere in the backend, never runs. First
experiment, measured: with the three call sites stubbed out, the base-instance scenarios were **10/10 green**
and the whole lane was **920/920 green**.

So the control was run in the configuration where the pushed value is observable — the emulation — by forcing
**both** native-base-instance gates false (`Managers.cpp:3569` and `DirectGLES.cpp:5276`'s
`UseNativeBaseInstance()`; forcing only the first double-applies the shift, which is itself a positive proof
that the pushed value reaches the fetch, and was measured: 2/10 red with the call sites **present**, 10/10
green with them stubbed — the inverse, exactly as a double shift predicts).

| experiment (temporary, in place, never committed) | base-instance scenarios | whole lane |
|---|---|---|
| **F1** both native gates forced false (emulation live), the three call sites **present** | **10 / 10 PASS** | 920 / 920 |
| **F2** both native gates forced false, the three call sites **STUBBED OUT** | **8 / 10 — 2 FAILED** | — |
| restored (`git checkout --` on all three files) | 10 / 10 PASS | 920 / 920 |

The two that go red are `VertexAttribBindingScenario.BaseInstanceMovesTheInstancedArraysStartElement` and
`BaseInstanceLeavesPerVertexArraysWhereTheyWere`. **`DrawParametersScenario`'s eight cases stay green in both
directions**, and that is correct rather than a hole: they observe `gl_BaseInstance` / `gl_DrawID`, which come
from `SetCurrentBaseInstance` — "orthogonal and runs either way — it feeds the shader's `gl_BaseInstance`, not
the fetch" (`DirectGLES.cpp:5272-5274`). **The ID-10 gate is the two `VertexAttribBinding` base-instance
cases, not the `DrawParameters` eight**, and the integrator should record that correction.

`git status` was verified clean and the tree rebuilt after every experiment; `grep -rc 'ID-10 CONTROL,
TEMPORARY'` is 0 in all three files.

### 4.8 The retrace, both sweeps

```
python3 ~/w7/retrace_gate.py --tree ~/w7/p3a-espryt --lib ~/w7/p3a-espryt/build-push/libMobileGL.so \
        --out ~/w7/retrace-out/p3a-espryt -j 4                                     rc 0
    passed 79 / 79; failed: []
    logs 79, with Fatal{: 0

MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/p3a-espryt \
        --lib ~/w7/p3a-espryt/build-verify/libMobileGL.so \
        --out ~/w7/retrace-out/p3a-espryt-verify -j 4                              rc 0
    passed 79 / 79; failed: []
    logs 79;  armed (MGPipe verify: present) 79;  NOT armed 0;  with Fatal{: 0
```

Every case at or above its own SSIM threshold on both backends (`rd12-odinlite` 1.0 / 0.999991,
`1.21.4-in-world` 0.999976, `1.21.4-main-menu` 0.997009, `OpenRA` 1.0/1.0, `startup` 1.0/1.0). **This is the
first verify-armed retrace of the whole corpus with the backend switched over**: the compare-at-read re-reads
every migrated field from the live context at every backend read and found zero divergence and zero
unmigrated reads. No `Fatal{UnmigratedPipeInput}`, so no `FillPoints.def` row is owed. Both output trees were
deleted afterwards (`retrace-out/p3a-espryt*` → 0 left).

### 4.9 D.4.6 — peak RSS

**Not available, so skipped.** `~/w7/retrace_gate.py` has five arguments (`--tree --lib --out -j --only`) and
contains no `rss` / `maxrss` / `getrusage` reference at all (`grep -ci` → 0). A push-vs-pull peak-RSS reading
over the retrace would need the gate to grow the measurement; it is not something this round can report
without changing package D's tool.

### 4.10 Cross-package confirmations owed by `espryt-v2` §6

| item | result |
|---|---|
| 12 — wire's C2: the applier reset must **advance** the two serials | **holds.** `PipeApply.cpp:645-646` (`MGPipeApplierReset`) and `:658-659` (`MGPipeApplierReleaseObjectRecords`) are `++`; `grep` finds no `= 0` on either serial. |
| 17a — `OnGpuWritten` sets **both** `m_hasDefinedContent` and `m_gpuWritePending` (m8) | **holds.** `ResourceTracker.h:614` calls `buffer->MarkGpuWritten()`, which sets both (`BufferObject.cpp:364-367`). |
| 17b — `MGPVertexBuffer::BindingIndex` is the attribute index (D-H3) | **holds** (`VertexInputEmit.h:229`) — and F1 is the fix for the arm that assumed the opposite. |
| B-DEV-7 — `kMGPipeResourceTargetBuffer == 0` | **confirmed** (`ResourceTracker.h:160`). |
| 13 — D.3's `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` arm | green, §4.6 L5. |
| 10 — `0x1ff` / `0x7f` / `0x17f` | all three run; `0x17f` refuses by name, §4.6 L8. |

---

## 5. Track H census inputs

Unchanged from `espryt-v1` §2 as amended by `espryt-v2` §3, plus this round's three:

**Members deleted from the handle arm and kept compiled under `MOBILEGL_PIPE_LEGACY_MEMOS`** (a pull build
forces that ON, so no `sizeof` moves — G1 is 0/0/0/0 at HEAD, which is the measurement):

1. `BackendVertexArrayObject::m_hasSyncedConfigVersion`
2. `BackendVertexArrayObject::m_syncedConfigVersion`
3. `BackendVertexArrayObject::m_syncedAttributeVersions` (`Array<VertexAttributeVersion, 32>`) — the largest single deletion of the phase
4. `BackendVertexArrayObject::m_syncedIndexBufferVersion` (wrapping `Uint16`)
5. `BackendVertexArrayObject::m_syncedIndexBufferObject` (raw frontend pointer — the wrap-hole identity patch)
6. `ConvertedFloat64Stream::sourceLifetimeId` (`ConvertedVertexStreamKey`'s `sourcePin`)
7. `VertexArrayImpl::g_pendingFetchBaseInstance` + `SetPendingFetchBaseInstance` + `GetPendingFetchBaseInstance` + `ScopedFetchBaseInstance` and its three scopes

**Kept, NOT deleted (D-K):** `PipeResource::m_backend`, `SetBackendResource`, `ReleaseBackend`,
`BackendBufferResource` — under push they simply stop being written. Deleting them moves `sizeof(BufferObject)`
in the **pull** build, which is a straight G1 break; they retire with the pull path at P13.

**Re-keyed memos:**

| memo | was | is |
|---|---|---|
| `BackendVertexArrayObject`'s sync gate | config version + 32 per-attribute version triples | `{elementsHandle, elementsSerial}` + `VertexBuffersSerial` |
| the twin's index memo | wrapping `Uint16` + raw `BufferObject*` | `m_syncedIndexSerial` (`Uint64`) |
| `ResolvedDrawBuffers::configVersion` | frontend VAO config version | `{elementsHandle, elementsSerial, buffersSerial}` |
| `ResolvedDrawBuffers::Entry` | raw `BufferObject*` | `+ MGPipeHandle handle` (the probe's identity); `iboFrontend` likewise `+ iboHandle` |
| `ConvertedFloat64Stream` | frontend lifetime id + frontend change serial | buffer `{slot, gen}` + applier `Serial` |
| `GLESBufferResource::syncedChangeSerial` | mirrors `BufferObject::GetChangeSerial()` | mirrors `MGPipeResourceRecord::Serial` |

**Push-only members added** (none of them in a pull build): `GLESBufferResource::hostBytes`,
`BackendVertexArrayObject::m_syncedVertexBuffersSerial` / `m_syncedElementsHandle` / `m_syncedElementsSerial` /
`m_hasSyncedElements`, `ResolvedDrawBuffers::{elementsHandle, elementsSerial, buffersSerial, Entry::handle,
iboHandle}`, `BackendSlotTable::kMaxHandleSlot` + `LiveGenAt` (no member).

**Unchanged on purpose (D-J / D-A4):** `g_bufferBackendIdGeneration`, `m_hasConvertedFloat64Attribute`,
`m_syncedFetchBaseInstance` (as the *emitted* record), `BackendSlotTable`'s single-entry
`m_memoLifetimeId`/`m_memoHandle` front memo, `TempBufferTarget` and its redundant-bind cache.

**This round's additions to the census:**

* `GLESBufferResource::hostBytes` gains a **publisher**: `EnsureBufferResourceForHandle` republishes the live
  shadow base at every ensure (F3). Its lifetime rule is otherwise unchanged (refreshed by a content-carrying
  call, cleared by an orphaning respecify and by a successful persistent-map adoption, never read by a path
  that holds the frontend object).
* `IsBufferDrawCleanByHandle` reads five things (four applier reads + the frontend's `IsMapped()`, C-1) and
  `EnsureBufferResourceForHandle` now reads **two** frontend things beyond `SyncPersistentMappedRange`:
  `MappedData()` (C-2) and `HasDefinedContent()` (F2). All three retire at P5/P8 and are named at their sites.
  D6's "no `MG_State` type in any new handle-arm signature" is unchanged: it holds for the nine op bodies and
  the object-free helpers, and its three named exceptions are `EnsureBufferResourceForHandle`,
  `MarkBufferGpuWritten` and `IsBufferDrawCleanByHandle`.
* No memo was retired or re-keyed by this round; `VertexBufferForBindingIndex` → `VertexBufferForAttributeIndex`
  is a rename of a lookup helper, not of a memo.

---

## 6. Deviations

D1-D14 stand as `espryt-v1` §5 / `espryt-v2` §4 record them, with two notes:

* **D7 narrows again.** The fp64 function had three problems, not one: the declared `SyncGpuWrites` gap
  (still open, still P8's), the freed shadow base (C-2, fixed last round), and the base that was **never
  published at all** (F3, fixed here). Only the declared one is left.
* **D12 is the live divergence from ID-13's letter** — a push build contains no `FlushPendingRangesNow`. §2.
  It wants an explicit integrator ruling; it costs nothing today (both legacy arms are 920/920).

No new deviation is declared by this round.

---

## 7. Still red, and it is NOT this package's: 26 `BufferTest` unit cases

**Symptom.** In `build-push` and `build-verify` under the default mask, 26 of the 86 `BufferTest` cases fail;
they pass in `build-linux`, and under `MOBILEGL_PIPE_PUSH=0` and `=0x7f` in `build-push`. The failures are all
`EnsureGpuResidentStorage()` returning `false` and `mock.gpu.data()` staying `NULL` — the mock backend is
never called.

**Mechanism, with the evidence.** `MG_Test/Buffer/BufferTest.cpp:1650-1655`'s `ScopedBackendOps` installs a
mock through `MG_State::GLState::SetBufferBackendOps(&kZeroCopyMockOps)`. The `BufferTest` binary **brings up
the real DirectGLES backend** (its log: `Initializing MobileGL… Renderer Name: Espryt`), so
`BufferImpl::RegisterBufferBackendOps()` runs and, per this package's D3, also installs the handle-shaped table
(`Managers.cpp:2454` `MG_Pipe::MGPipeSetResourceOps(&g_glesResourceOps)`). `MGPipeResourceSubsystemEnabled()`
(`PipeFill.cpp:593`) is therefore true, and the frontend's push arms
(`BufferObject.cpp:602-608`, `:655-661`, and six more) route the call to `MGPipeEmitMapPersistent` and
**never reach `g_bufferBackendOps`** — the mock the fixture installed. The fixture scopes one table and not
the other.

**Whose it is.** Not espryt's implementation choice: C.2's step `e2` prescribes installing the table "at
bring-up … beside `RegisterBufferBackendOps`", so any conformant espryt produces this. Not the client's
either: the eight `BufferObject.cpp` dispatch branches are exactly what D-K specifies. It is the
**merge-seam** case C.5 warns about in as many words — *"Green on two branches separately is not green on the
merge"*: at `espryt-v2` these 86 were green because nothing emitted through the pipe; at `client-v2` they were
green because nothing registered a table; together they are red. `MG_Test/Buffer/BufferTest.cpp` is
**"nobody"** in C.5's ownership table, i.e. the integrator's (or D's) to land, and it is not in C.2's file
list, so this package may not commit the fix.

**The fix, and it is measured, not proposed.** `ScopedBackendOps` must scope the pipe table too, exactly as
`MG_Test/Pipe/ResourceEmitTest.cpp:139-153`'s `ApplierGuard` already does for the applier:

```cpp
    struct ScopedBackendOps {
        explicit ScopedBackendOps(const MG_State::GLState::BufferBackendOps* ops) {
            MG_State::GLState::SetBufferBackendOps(ops);
#if MOBILEGL_PIPE_PUSH
            m_savedResourceOps = MG_Pipe::MGPipeGetResourceOps();
            MG_Pipe::MGPipeSetResourceOps(nullptr);   // these 86 cases are BufferBackendOps tests;
#endif                                                // the pipe-side dispatch is ResourceEmitTest's
        }
        ~ScopedBackendOps() {
#if MOBILEGL_PIPE_PUSH
            MG_Pipe::MGPipeSetResourceOps(m_savedResourceOps);
#endif
            MG_State::GLState::SetBufferBackendOps(nullptr);
        }
#if MOBILEGL_PIPE_PUSH
        const MG_Pipe::MGPipeResourceOps* m_savedResourceOps = nullptr;
#endif
    };
```

plus `#include <MG_Pipe/PipeApply.h>`. **Applied temporarily in this worktree and measured:**

```
B: BufferTest (default mask) : 100% tests passed, 0 tests failed out of 86
B: whole unit suite          : 100% tests passed, 0 tests failed out of 1619
(reverted; back to the committed tree: 70% passed, 26 failed of 86)
```

It weakens nothing: those 86 cases are `BufferBackendOps` dispatch tests, the pipe-side dispatch is covered by
`ResourceEmitTest`'s 25, and the fix restores exactly the arm they were written against. A larger alternative
— giving the fixture a pipe-shaped mock so the same assertions run over the handle path — is worth doing, but
it is a rewrite of 86 cases and belongs to whoever owns that file.

**Also recorded, and not fixed here:** the two `DirectVulkan.…ActuallyArmedWhenTheEnvironmentPinsItOn` cases
that failed only in the very first `-j 8` run of this tree and in no `-j 4` run, no serial rerun and no later
`-j 8` run. `client-v2` §6 item 6 already names this as the shared-log-file race at
`MG_IntegrationTest/CMakeLists.txt:1040`; this package touches no `DirectVulkan` file and no
`MG_IntegrationTest` file.

---

## 8. Open items carried forward (not this round's to close)

* `wire-review-v1` M-D's applier half is closed (`PipeApply.h:112-113`, bounded); the backend half was bounded
  by M-1 last round. Nothing left here.
* The dangling-`ResolvedDrawBuffers::Entry::resource` invariant after `ReleaseByHandle`: both consumers still
  check identity through `FindByHandle` first, so nothing is dereferenced, and the invariant still deserves a
  sentence at the member in a later pass.
* D7's `SyncGpuWrites` gap on the fp64 narrowing — P8's, unchanged.
* The three `GL_Buffer.cpp` `NoteBoundAs` sites (`client-v2` §6 item 2) and `delete_vertex_elements` / the VAO
  CSO slot free (`client-v2` §6 item 3, which C.5 gives to this package but which C.2's ordered steps do not
  contain) — both still open, both invisible in monolith.
* Package D's itest for C-1 (a persistently mapped, non-adopted buffer written only through its pointer, then
  drawn and read back) is still owed; `SanityTest`'s unit case pins the probe's semantics but issues no GL.

---

*Intermediate logs (`~/w7/espryt3-*.log`, `~/w7/espryt3-*.sh`, `~/w7/espryt3-fixes*.patch`) and the
scratchpad's `espryt3-*` scripts are deleted; `~/w7/retrace-out/p3a-espryt` and `-verify` are deleted;
`git status` in `/home/swung/w7/p3a-espryt` is clean at `3c55e027`. The trace fixtures are left materialised
(803 MB, `assume-unchanged` intact), which is the state the next retrace run needs.*
