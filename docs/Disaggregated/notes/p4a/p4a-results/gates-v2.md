# P4a package F — `gates`. Rework result, v2

`refs/heads/p4a/gates` = **`ecf9cb45`**, rebased onto `refs/heads/feat/disaggregated` = `2cb44039`
(c0 + c0b; no c0c had landed there when this round started, so `2cb44039` is the head ID-16 names).
Working tree clean, **nothing pushed**, no other worktree written to.

**`feat/disaggregated` moved WHILE this round ran** and is now `712c9467` — c0c plus the wire
package's rework (`Tracker.h`, `MGPipeTypes.h`, `PipeApply.{h,cpp}`, `PipeFields.def`, and real
cases in seven `MG_Test/Pipe/*EmitTest.cpp` suites; 14 files, +4982/-116). **This package was not
rebased onto it**, and every number below is measured on `2cb44039` + the eight commits. The delta
touches **none** of F's files — `MG_IntegrationTest/**`, `MagmaPipeArms.h`, `scripts/p4a_*.sh`,
`.github/workflows/test.yml` — so the rebase is textually clean; it is the integrator's to take,
along with a re-run of section 6 (the emission suites now carry real cases, which moves this
package's ctest counts even though nothing it owns changed).

Answers `gates-review-v1.md`'s six majors, the cheap minors, and `INTEGRATOR-DECISIONS.md` **ID-16**
(which is this round's specification) plus **ID-15**'s new D-K2 row.

---

## 1. Commits

The four v1 commits are unchanged in content and keep C.5's exact messages; the rebase gave them new
hashes. Four rework commits sit on top.

| # | hash | was | message |
|---|---|---|---|
| 1 | `5cdcce59` | `0bc9aafa` | `[Test] (Pipe): reproduce the texture, framebuffer, renderbuffer, sampler, view and program handle ABA through public GL and prove the pre-rekey guards are what stops it` |
| 2 | `f6ab550f` | `4f006f3c` | `[Test] (Espryt): assert a glTexParameter on a texture that only ever was an attachment, an image binding or a copy endpoint reaches the driver` |
| 3 | `7789584b` | `70868f93` | `[Test] (Pipe): switch P4a's four object subsystems off against the shipping mask and record the texture upload shape the two counters make visible` |
| 4 | `84ed7177` | `303a9dc8` | `[CI] (Pipe): gate that the unpack ring, the attachment permutation, the depth-stencil sampling core and the format caveat did not move` |
| 5 | `e85ba86a` | — | `[Fix] (Test): make G7's descriptor negative control able to report success - a patch that compiles into a scoped enum, a verdict its own patcher cannot pollute, and repair state the signal traps can see` |
| 6 | `77f31f90` | — | `[Fix, Test] (Pipe): read the shader composite band's own counters in the composite leak case, arm the ABA flip from anywhere under a backend, match Espryt's exact refusal line and pin the bit-10-requires-bit-11 arm` |
| 7 | `2e18d35d` | — | `[Fix, CI] (Pipe): order the byte-identity listing, prove every protected region reaches its closing brace, and pin the include-closure probe count` |
| 8 | `ecf9cb45` | — | `[Fix] (Test): stop G7's descriptor control carrying on past a Ctrl-C - latch the interrupt from the trap and from a signalled child, and report it as an interruption rather than as the contract tree's verdict` |

Commit 8 is not on the review's list. It is a defect **the F-M3 proof run found in the fixed
script** — section 3.2 — and it is reported as a finding rather than folded away.

The rebase was clean (c0b touched `PipeMutation.h`, `PipeFill.*`, `SlotAllocator.*`,
`PipeCatalogueTest.cpp` and `check_include_closure.py`; this package touches none of them). Files
this round changed: `MG_IntegrationTest/{CMakeLists.txt, Harness/PipeSlotPeek.{h,cpp},
Scenarios/{HandleRecycle,ObjectSubsystemControl,TextureUploadShape}Scenario.cpp}`,
`MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h` (F's one granted DirectVulkan file),
`scripts/p4a_{untouched_regions,descriptor_negative_control}.sh`, `.github/workflows/test.yml`.

---

## 2. The six majors

### F-M1 — the control patch now compiles. `scripts/p4a_descriptor_negative_control.sh:331`

The replacement value is `{}`, not `0`:

```python
patched, count = pattern.subn(r'\g<1>{} /* G7 NEGATIVE CONTROL: was \g<2> */\g<3>', text)
```

`= {}` is valid for `SamplerParameters::borderColorForm` (the scoped `enum class BorderColorForm :
Uint8`) **and** for `MGPSurface::Layered` (`Uint8`), in an assignment and in a designated
initializer, so one string covers both controls and neither has to know its field's type. The
reasoning is written into the header at `:48-61`, together with the caveat that `{}` is the type's
zero — so a suite every case of which expects the field's zero would legitimately report
`did-not-trip`, and both suites' own headers already require a red **by field name**.

**Proved, not argued** — see 3.1: on a scaffolded tree the `borderColorForm` control compiled, went
red naming the field, and the run exited **0**.

### F-M2 — the verdict cannot be polluted. `:250`, `:339`, `:452-457`

`run_control` no longer echoes its verdict: it sets the global `CONTROL_VERDICT` (`:250`) and the
driver reads it after a **plain call** (`:455`). The patcher's success line goes to `sys.stderr`
(`:339`), which is the literal ask, but the fix that matters is that the verdict no longer travels
through a stream anything else can write to. The rows are also collected into an array first
(`:445-452`) and every child gets `</dev/null`, so a `cmake` or `ctest` that read stdin could not
eat the second control's row out of the loop's heredoc.

### F-M3 — `repair()` really runs from the traps. `:220-232`, `:455`

`PATCHED_HEADER` is set in **this** shell (`run_control` is no longer invoked in `$(...)`), so the
`EXIT`/`INT`/`TERM` traps see it. Measured end to end — 3.2: a SIGINT sent to the process group
mid-rebuild, with the patch demonstrably in the tree, ran the trap's `repair`, restored and rebuilt,
re-raised, and the script died **130** leaving `git status --porcelain` **empty**.

### F-M4 — the composite leak case reads the band. `Harness/PipeSlotPeek.{h,cpp}`, `HandleRecycleScenario.cpp:2200`

`PipeSlotPeek.h:97-99` gains the members `contract-v2.md` 4.3/7.6 asks for, and the now-false
paragraph about a merged high-water mark (old `:47-55`) is replaced by `:47-61`, which states c0b's
split:

```cpp
bool PeekPipeCompositeSlotLiveCount(unsigned* outLive);       // CompositeLiveCount()
bool PeekPipeCompositeSlotHighWater(unsigned* outHighWater);  // CompositeHighWater(), VERBATIM
bool PeekPipeCompositeSlotBandBase(unsigned* outBandBase);    // kMGPipeShaderCsoCompositeSlotBase
```

`PeekPipeCompositeSlotHighWater` returns the allocator's number **unmodified** — an absolute slot
index that starts at the band base, not a base-relative delta. A peek whose name says HighWater and
whose value is a delta is the same class of quietly-redefined counter this member exists to correct,
so the base is a third member instead (`:91-96`, bodies `.cpp:57-73`) and the scenario reads the two
against it.

`AssertChurnReturnsEverySlot` now takes a `SlotSpace` (`HandleRecycleScenario.cpp:217-247`, `:707`),
stated at every one of the seven call sites — six `SlotSpace::Ordinary`, and
`EvictedPipelineCompositesReturnTheirShaderCsoSlots` `SlotSpace::CompositeBand` (`:2200`). Three
consequences:

* the three assertions (`live`, `high water`, `peak in flight`) are about the **band** for that case;
* the "nothing was ever minted" skip compares against the space's **floor** (`:734-741`, `:782`) —
  0 for the ordinary space, the band base for the band — so a band that never moved from 983040 is
  a named skip and not a silent pass;
* `maxInFlight` for the composite case is **1**, not 3: a round creates three `ShaderCso`s but only
  one of them is a band slot (`:2192-2202`). F-m6's stale "TWO in flight" prose is gone with it.

Today, on both lanes, the case prints its band numbers and skips:

```
[ HandleRecycle ] backend=DirectGLES ShaderCso (pipeline composites) [composite band]
    live 0 -> 0 (peak 0), high water 983040 -> 983040 (floor 983040) over 48 rounds
... Skipped: the client minted no ShaderCso (pipeline composites) [composite band] slot at all
```

**It can go red** — 3.3.

### F-M5 — the ABA-flip probe is directory-wide. `MG_IntegrationTest/CMakeLists.txt:411-460`, `:571-595`

`mgl_itest_probe_for_two_symbols` now answers "**both regexes matched somewhere under the
directory**" and reports `<file matching A> + <file matching B>`, instead of requiring one file to
carry both. The layout dependency the review found — package D wiring the knob in `Managers.cpp`
while the P4a subsystem constants live in `SlotTables.h`, the marker never arming, six controls
silently keeping the old expectation — is gone.

It costs nothing today, verified by grep on this tree: `MobileGL/MG_Backend/DirectVulkan` matches
`PipeHandleAbaControl` (only `MagmaPipeArms.h`) and matches
`kMGPipeSubsystem{Framebuffer,TextureResources,Samplers,Programs}` **nowhere**;
`MG_Backend/DirectGLES` matches neither. So `MGITEST_HANDLE_ABA_OBJECTS_*` is still unset for both
backends and the six controls still assert the correct pixels and say so.

### F-M6 — the refusal is matched as a LINE. `ObjectSubsystemControlScenario.cpp:150-215`, `:556-575`

`FindTheRefusalLine(log, bitThatWasSet, bitThatWasNeeded)` (`:190-215`) scans the log line by line
and requires **one line** that is all of:

* at **ERROR** severity — the library writes `[<time>] [<os> <thread>/<TAG>]: <message>`, one record
  per line (`MG_Util/Debug/Log.cpp:89-103`), and D-K2 asks for an `MGLOG_E`;
* carrying `REFUS`/`refus`, so a line that merely mentions the two bits is not the decision;
* naming the bit that was set **and** the bit it needed, on that same line. Three spellings each —
  the constant's name, `(bit 11)` / `(bit 10)`, `0x800` / `0x400` (`:207-215`).

That is Espryt's exact sentence. `p4a-esprytobj`'s `Managers.cpp` emits it from the helper the three
dependent families share (`PipeSubsystemDependencyMissing`, ~`:3523`, used by
`ResolveSamplerSubsystemArm` ~`:3659`):

> `MGPipe: kMGPipeSubsystemSamplers (bit 11) is set but kMGPipeSubsystemTextureResources (bit 10) is
> clear; every MGPBoundView::Texture and MGPImageView::Res names a Texture handle and only bit 10
> populates that slot table - REFUSING the dependent bit and running the legacy arm. Set both bits,
> or clear both`

The matched line is printed and `RecordProperty`'d on the **pass** as well (`:570-575`), so a green
refusal lane shows the sentence it went green on. The old form — a lowercase `"sampler"` anywhere in
the whole file — is gone.

---

## 3. The proofs

Run on a **throwaway commit** `8266916a` on a private branch `tmp/gatesF-g7proof`, which gave the
tree the two things it does not have yet: a `.Layered =` copy in `FramebufferEmit.h` and a
`.borderColorForm =` copy in `SamplerEmit.h` with one comparison case per emission suite, and one
deliberately leaked composite band slot per churn round. **The branch has been deleted and no
scaffold commit is reachable from anything** (`git log --oneline --all | grep -c SCAFFOLD` = 0);
`p4a/gates` never carried it.

### 3.1 G7 reaches "tripped" and exits 0

```
[p4a-g7] [FramebufferEmit-Layered] the patched header still compiles, so the record still has the field and its size
[p4a-g7] [FramebufferEmit-Layered] negative control tripped, naming Layered, and the tree is green again
[p4a-g7] [SamplerEmit-borderColorForm] the patched header still compiles, ...
[p4a-g7] [SamplerEmit-borderColorForm] negative control tripped, naming borderColorForm, and the tree is green again
[p4a-g7] ---- G7 (P4a descriptor emission) ----
  Layered (MobileGL/MG_Impl/Pipe/FramebufferEmit.h): tripped
  borderColorForm (MobileGL/MG_Impl/Pipe/SamplerEmit.h): tripped
[p4a-g7] both controls tripped and named their field; exit 0
G7_RC=0
```

`git status --porcelain` empty afterwards, and both headers still carry their field copies. Re-run
after commit 8's hardening: **still exit 0, both tripped**. This is the first time the script has
ever reached that path — F-M1 and F-M2 together.

### 3.2 A SIGINT mid-rebuild leaves the tree clean

```
===== mid-way: the patch IS in the tree =====
 M MobileGL/MG_Impl/Pipe/FramebufferEmit.h
69:        dst.Layered = {} /* G7 NEGATIVE CONTROL: was src.Layered */;
===== sending SIGINT to the process group =====
SCRIPT_EXIT_RC=130
[p4a-g7] [FramebufferEmit-Layered] rebuilding with the dropped field
[p4a-g7] restored MobileGL/MG_Impl/Pipe/FramebufferEmit.h; rebuilding build-push from it
[p4a-g7] the tree is restored, rebuilt and green again
===== tree AFTER the interrupt =====   (empty)
===== leftover control marker anywhere? =====   (none)
```

**A methodology note the integrator needs.** The first attempt at this proof was invalid and looked
like a pass. A command started with `&` from a **non-interactive** shell has `SIGINT` set to
`SIG_IGN`, and a disposition inherited as ignored **cannot be trapped** — so the script could never
run its `INT` trap however it was written, `ninja` (which installs its own handler) took the signal
instead, and the tree was repaired through the explicit build-failure path. Repaired, but the run
then carried on into the second control and signed off with the summary it prints on an untouched
contract tree. The proof above launches the script through `python3` with the disposition reset to
`SIG_DFL`, which is what a terminal's Ctrl-C gives it. **Anyone re-running this must do the same or
they will measure the launcher, not the script.**

That first attempt is also what found commit 8's defect, now fixed two ways (`:220-232`, `:353-360`,
`:380-389`, `:462-470`, `:476-483`): the traps latch `INTERRUPTED`, every child's exit status is
additionally checked for "killed by a signal", the driver **breaks** rather than running the next
control, and the summary says `(INTERRUPTED - the remaining control(s) did not run)` instead of the
contract-tree wording.

### 3.3 The composite leak case can go red

With one band slot leaked per round, on the scaffold:

```
[ HandleRecycle ] backend=DirectGLES ShaderCso (pipeline composites) [composite band]
    live 2 -> 50 (peak 50), high water 983042 -> 983090 (floor 983040) over 48 rounds
HandleRecycleScenario.cpp:793: Failure  - 48 ... slots never came back
HandleRecycleScenario.cpp:804: Failure  - the ... slot space grew with the churn
HandleRecycleScenario.cpp:809: Failure  - more than 1 churned ... object(s) were live at once
```

**Failed on all three lanes that pin a live allocator** — `DirectGLES.HandleRecycle.Handles.`,
`DirectVulkan.HandleRecycle.Handles.` and `DirectVulkan.HandleRecycle.AbaControlHandles.` — and
correctly **skipped** on the `Legacy` / `AbaControl` lanes (`MOBILEGL_PIPE_PUSH=0`) and on the
ambient entries. 3 failed out of 8. All three assertions fire, which is what says the case is wired
to the band and not to the ordinary space.

That third lane is also **F-m4's proof**: it used to decline on `arm == Handles` while telling the
reader it had no allocator, which was false.

---

## 4. ID-15 — the 0x5ff arm

`ObjectSubsystemControlScenario.ATextureBitWithoutTheSamplerBitIsRefusedAndNamed` (`:589-693`), lane
`DirectGLES.ObjectSubsystemControl.RefusedTexture.` pinning `MOBILEGL_PIPE_PUSH=0x5ff`
(`CMakeLists.txt:1446-1451`, `:1478-1487`). Same two assertions as the 0x9ff arm with the bits'
roles swapped: one ERROR line naming both bits and saying it refused, and the same pixels every
other lane draws.

**On this tree it is a named SKIP, and it is visible as one:**

```
DirectGLES.ObjectSubsystemControl.RefusedTexture....ATextureBitWithoutTheSamplerBitIsRefusedAndNamed
  ***Skipped
  subsystem not implemented on this tree: no source under MobileGL/MG_Backend/DirectGLES names any
  of kMGPipeSubsystem{Framebuffer, TextureResources, Samplers, Programs}, so this backend does not
  honour P4a's mask and cannot refuse a dependency inside it. D-K2's fourth row (bit 10 requires
  bit 11, ID-15) lives in the texture family's Resolve*SubsystemArm ...; this control arms itself
  when that lands. The lane itself is not wasted: the library came up under 0x5ff ...
```

> **[CORRECTED by gates-v3.md sections 2 and 3.1 - review finding F-v2-m2. Do not act on the
> paragraph below.]** The red window this section predicts DOES NOT EXIST. `p4a/esprytobj` v2
> already carries D-K2's fourth row (`Managers.cpp`, `ResolveTextureResourceSubsystemArm`
> refuses bit 10 without bit 11), so at integration the arm ARMS and its refusal assertion
> PASSES - measured on the integrated tree at `29d51ab9`, matching Espryt's exact sentence.
> What IS red there is the lane's OTHER assertion, the pixels, and that is a real finding about
> the CLIENT: at 0x5ff the emitter still emits the texture family (`ctu=5`) while Espryt refuses
> the bit and runs its legacy arm, so the dirty flags are cleared on an acceptance nobody
> consumes and the sampled texture is black. See gates-v3.md section 3.1.

**The integrator must know what that gate implies.** It is the same marker the 0x9ff case uses -
"does any source under this backend name a P4a subsystem constant". `p4a/esprytobj` v1 already sets
it, and its `ResolveTextureResourceSubsystemArm` (`Managers.cpp` ~`:3618`) refuses only bit 10
without bit **7**. So **the moment esprytobj is integrated, this entry stops skipping and goes RED
until package D's rework adds the bit-10-requires-bit-11 row.** That is exactly the pin ID-15 asks F
to provide, and it is deliberate — but it is a red that appears at integration time and not at F's,
and it must not be read as a regression in this package.

The lane marker vocabulary is now `on / off / refused / refused-texture`; an unrecognised value is
still a `FAIL()`, and the on/off case declines **both** refusal lanes.

---

## 5. The minors

| id | what changed |
|---|---|
| **F-m1** | `p4a_untouched_regions.sh:522`, `:527-543` — `reorder_sha_list` puts the baseline listing back into `REGIONS` order after the pinned rows are appended. **Measured: `p4a_untouched_regions.sh 37da3c3a` and `... 37da3c3a HEAD` now produce byte-identical sha lists** (`diff` empty; it was seven spurious differences). |
| **F-m2** | `:366-411` (`perturb(... where)`) and `:646-679` — every self-test region is now perturbed at its **head** and again at its **tail**, immediately before the closing brace. **8 negative controls, all tripped, all named**; the pin is `!= 8`. This is the control that proves an extent reaches its closing brace, which no head-only control can. |
| **F-m3** | `:74-82` — one paragraph stating the hash's exact extent: it starts at the line carrying the name, so a return type, attribute or template header on an earlier line of a multi-line signature is outside it; the end IS covered, and the tail controls are why. |
| **F-m4** | `HandleRecycleScenario.cpp:194-212`, `:707-733` — the leak cases gate on `LanePinnedALiveAllocator()` (the lane's own non-zero `MOBILEGL_PIPE_PUSH`) instead of on `arm == Handles`. The false sentence is gone and `DirectVulkan.HandleRecycle.AbaControlHandles.` is now covered — see 3.3. |
| **F-m5** | `MagmaPipeArms.h:229-290` — the per-kind answer is now `constexpr MagmaPipeAbaControlKindIsRekeyedHere(kind)` (`:245`), **exhaustive with no `default:`** (a new `MGPipeKind` is a `-Wswitch` warning, and there is no arm for it to read `true` from), **pinned by three `static_assert`s** in the same file (`:273-290`) saying the two minted kinds are covered and P4a's six are not. It can no longer rot with no caller: the asserts compile in every Magma build. `MagmaPipeAbaControlCoversKind` is that predicate AND the knob. |
| **F-m6** | folded into F-M4: the composite case's prose and its `maxInFlight` now agree, and both say **1** (the band's answer), not two or three. |
| **F-m7** | `TextureUploadShapeScenario.cpp:326-378` plus a new probe `MGITEST_PIPE_CLIENT_TEXTURE_UPLOAD_EMITTER_PRESENT` (`CMakeLists.txt:614-632`, over `MG_Impl/Pipe` for `ClientTextureUploadEmissions`). `ctu=` is published in **every** push build, so "absent" and "zero" were never distinguishable from the number; the build now answers it. With no emitter the case **asserts `ctu == 0`** (a non-zero one would mean the probe is looking at the wrong symbol); with an emitter it asserts `ctu > 0` **and** `ctu == emit`. |
| **F-m8** | `p4a_descriptor_negative_control.sh:28-33` — the header now says the `Layered` control perturbs two mechanisms on a tree that has the framebuffer emitter (the record and the ContentHash staging copy), rather than claiming a single-field drop. Narrowed in prose, not in the regex: excluding the hash copy means hard-coding another package's helper name. |
| **F-m9** | `.github/workflows/test.yml:793-802` — `--expect-probes 4` added; `clang++-20` kept, with the reason (the step above installs `clang-20`; the bare `clang++` spelling exists only because the WSL box has no `clang++-20`). **Verified both ways locally: `--expect-probes 4` rc 0, `--expect-probes 5` rc 1.** |
| **F-m10** | `p4a_descriptor_negative_control.sh:129-155` — the build directory is checked, reading **both** `MOBILEGL_PIPE_PUSH` and `MOBILEGL_PIPE_VERIFY` from `CMakeCache.txt`, because `MOBILEGL_PIPE_VERIFY=ON` forces push on for the configure without writing it back (root `CMakeLists.txt:470-472`) and refusing a verify directory would be the wrong answer. **Measured: `... build-linux` now exits 2 naming the cache lines, instead of scoring two `did-not-trip`s.** |
| **F-m11** | `scripts/p4a_*.sh` are `100644`, matching `scripts/p3a_untouched_regions.sh` and the two older negative controls. |
| **F-m12** | `HandleRecycleScenario.cpp:1319-1347` — the framebuffer ABA corruption assertion is `EXPECT_TRUE(sawStale)`, the predicate the case already computes and prints, not `EXPECT_NE(deadIsNotRed, 0)` which a garbage attachment satisfies. The message names the third answer explicitly. |
| **F-m13** | declared here: `~/w7/notes/tools/wsl_p4a_gate.sh` and the `p4a_ab.sh` / `p4a_ab_1ff.sh` / `wsl_p4a_bench.sh` / `ab_reduce3.py` family are C.5 rows that **ID-3 reassigns to the integrator**. They exist, timestamped before this package's round began, and this package did not write them. `gates-v1.md` section 8 was silent about them; this row is the correction. |

**Not fixed, and why.** R1's gap (`TextureParamsWithoutASamplerView` catches emitted-but-not-applied,
not deferred-to-first-view) is not this package's — ID-16 assigns the backend-side probe to package
D's verification round, and nothing F can write through public GL closes it (v1's section 3.4 still
holds).

---

## 6. Gate numbers, all at `ecf9cb45`, working tree clean

| gate | result |
|---|---|
| **G1** `symbol_report.py --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | **0 added / 0 removed / 0 resized / 0 renamed**, rc 0. `.text` 10806323 -> 10806323 (+0). File 19114360 -> 19114360. `.data` / `.bss` / `.rodata` all +0. 27811 -> 27811 defined symbols |
| `ctest -L unit` x 3 | `build-linux` **1636**, `build-push` **1636**, `build-verify` **1636** — 100%, 0 failed in each |
| **G2** ctest names pull == push | `diff` empty, **2711 == 2711** |
| **G14** vs `~/w7/p4a-before-ctest-names.txt` (37da3c3a) | **0 removed**, **+123 added** (v1's +119, plus c0b's one unit case, plus this round's three `ATextureBitWithoutTheSamplerBitIsRefusedAndNamed` entries) |
| `p4a_untouched_regions.sh 37da3c3a HEAD` | **rc 0**, 17 rows |
| `p4a_untouched_regions.sh --self-test` | **rc 0**, 3 positive + **8 negative** controls, each tripped and named |
| `p4a_untouched_regions.sh 37da3c3a` vs the two-ref listing | **byte-identical** (F-m1) |
| `p3a_untouched_regions.sh 37da3c3a HEAD` / `--self-test` | **rc 0** / **rc 0** |
| `check_include_closure.py --mode text --self-test --require-all --expect-probes 4` | **rc 0** ("4 probes, 0 skipped, 0 problems"); with `--expect-probes 5`, **rc 1** |
| `scripts/g7_negative_control.sh build-push` (P2) | **rc 0**, tripped naming `SetColorMask`, tree green again |
| `scripts/p3a_vertex_input_negative_control.sh build-push` (P3a) | **rc 0**, tripped naming `IsBgra`, tree green again |
| `scripts/p4a_descriptor_negative_control.sh build-push` (P4a) | **rc 2**, both `could-not-run` — the contract tree's correct answer, both emit headers are the contract's stubs. **rc 0 on a scaffolded B+C-shaped tree** (3.1) |
| `scripts/p4a_descriptor_negative_control.sh build-linux` | **rc 2**, refused as a non-push build directory (F-m10) |
| `ctest --test-dir build-push -R 'HandleRecycle'` | **144/144**, 0 failed |
| `ctest --test-dir build-push -R 'ObjectSubsystemControl\|TextureParamsWithoutASamplerView\|TextureUploadShape'` | **21/21**, 0 failed |
| `ctest --test-dir build-verify -R 'HandleRecycle'` | **180/180**, 0 failed |
| `ctest --test-dir build-verify -R 'ObjectSubsystemControl\|TextureParamsWithoutASamplerView\|TextureUploadShape'` | **37/37**, 0 failed |
| `ctest --test-dir build-linux -R 'HandleRecycle\|ObjectSubsystemControl\|TextureParamsWithoutASamplerView\|TextureUploadShape'` (PULL) | **165/165**, 0 failed |
| `ctest -L integration-gpu` push / pull | **1075/1075** and **1075/1075**, 0 failed — G2's unverified half in the v1 review is now measured on both builds |
| working tree, attribution, message shape | clean; no `Co-Authored-By`; eight single-line `[Type] (Scope): description` messages |

Every one of the new and changed scenarios runs on **both** lanes: the `HandleRecycle`,
`ObjectSubsystemControl`, `TextureParamsWithoutASamplerView` and `TextureUploadShape` families all
appear under `DirectGLES.` and `DirectVulkan.` prefixes in the counts above (the `RefusedTexture`
lane is DirectGLES-only by design, like its 0x9ff sibling; the scenario's ambient entries exist on
both).

`GLIBC_TUNABLES=glibc.malloc.tcache_count=0` was exported for every build and every ctest run above.
`CCACHE_BASEDIR=/home/swung/w7`, `-j 8` for builds. No retrace was run, so no `git lfs checkout` was
needed; D.3/D.4's retrace and device work stay the integrator's (ID-3).

**The recorded `TextureUploadShape` baseline is unchanged by this round:**
`server[emit=6 box=6 rect=0 jobs=6] client[ctu=0]`, 3 frames x (40 scattered 2x2 rects + one 64x8
band), DirectGLES on llvmpipe.

---

## 7. What the integrator must re-run on the integrated tree

`gates-v1.md` section 9's list stands in full. It is amended and extended by:

| what | why |
|---|---|
| `bash scripts/p4a_descriptor_negative_control.sh build-push` on an integrated B+C tree | **Expect exit 0 naming `Layered` and `borderColorForm`.** It has now been shown to reach that path (3.1), so exit 2 with the contract-tree wording is a real finding here — and so is exit 2 with the *interruption* wording, which means the run was killed and reported nothing about the controls |
| `ctest --test-dir build-verify -R 'EvictedPipelineComposites'` after C lands, and **read the printed band line** | The case reads `CompositeHighWater` / `CompositeLiveCount` now; the line is `... [composite band] live A -> B (peak P), high water H1 -> H2 (floor 983040)`. A high water still equal to the floor means C's composite resolver is not minting through `AllocateComposite` and the case is skipping, not passing |
| `ctest -R 'ObjectSubsystemControl.RefusedTexture'` **as soon as esprytobj is integrated** | **[CORRECTED by gates-v3.md sections 2 and 3.1 - review finding F-v2-m2.]** The REFUSAL assertion is expected GREEN: esprytobj v2 carries D-K2's fourth row and the sentence matches (measured on 29d51ab9). The lane is nonetheless RED there on its OTHER assertion, the pixels, because the client still emits the texture family at 0x5ff while Espryt refuses the bit - a real finding about the client gate, not a sequencing artefact |
| `MOBILEGL_PIPE_PUSH=0x9ff ctest -R 'ObjectSubsystemControl'` and the 0x5ff lane, **with the lane log kept** | The refusal assertion is now a single ERROR line naming both bits and saying it refused; the matched line is printed and recorded, so confirm it is Espryt's sentence rather than trusting the exit status |
| grep the **configure output** for `MOBILEGL_PIPE_HANDLE_ABA_CONTROL steers <backend>` after D lands | F-M5: the probe is directory-wide now, so the message names the two files that supplied the two halves. If it is absent while `is keyed on {slot, gen} for P4a's object families` is present, the knob has no consumer and the six ABA controls are still not controls |
| `bash scripts/p4a_untouched_regions.sh 37da3c3a HEAD` **and** `--self-test` after D and E | Unchanged, and the self-test is stronger: **8** negative controls, and a TAIL control that does not trip while its head twin does means the extraction stops before the closing brace |
| the CI `pipe-gates` include-closure step | `--expect-probes 4` is now pinned; it must change in the same commit as any change to `check_include_closure.py`'s `PROBES` list. **Do not** "fix" `clang++-20` to bare `clang++` on the strength of a local failure (ID-9 D16 / ID-11) |
| `ctest -L integration-gpu` on both builds | Measured here at **1075/1075** each; the counts move as B..E land |
| a SIGINT test of the G7 script, if it is ever re-verified | **Launch it with SIGINT deliverable** (3.2). A `&` from a non-interactive shell makes the trap unreachable and the test measures nothing |
| **rebase `p4a/gates` onto whatever `feat/disaggregated` is at integration time, and re-take section 6** | It was `2cb44039` when this round rebased and `712c9467` by the time it finished (c0c + wire's rework). The delta touches none of F's files, so the rebase is textually clean, but the seven `MG_Test/Pipe/*EmitTest.cpp` suites now carry real cases: the unit count, the `-N` name counts and the G7 script's "N matching test(s)" line all move without anything F owns changing. Re-take G1, the three `-L unit` runs, the pull/push name diff and the G14 delta on the rebased tree |

**Carried forward for the esprytobj round (ID-16, R1):** the backend-side probe must take its
reading while the texture is still attachment-only *and* distinguish "not applied yet" from "applied
late". `TextureParamsWithoutASamplerView` catches the first shape and not the second, and no
public-GL case can, because the sample that observes the parameter is also what repairs it.

**Artefacts** stay where v1 left them in `~/w7/notes/p4a/p4a-results/`
(`p4a-handlerecycle-before.log`, `p4a-handlerecycle-before-verbose.log`, `p4a-texparams-before.log`,
`p4a-object-subsystem-before.log`). They are the pre-rework readings; sections 3 and 6 above are the
post-rework ones. Everything else this package wrote under `~/w7` has been deleted.
