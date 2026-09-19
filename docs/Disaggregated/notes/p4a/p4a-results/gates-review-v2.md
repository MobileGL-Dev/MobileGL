# P4a package F — `gates`. Focused re-review of rework v2

Reviewed: `refs/heads/p4a/gates` = **`ecf9cb45`** (`/home/swung/w7/p4a-gates`, read-only for this
round) — the four v1 commits rebased onto `2cb44039` plus the four rework commits `e85ba86a` /
`77f31f90` / `2e18d35d` / `ecf9cb45` — against `gates-review-v1.md` (F-M1..F-M6, R1/R2/R3, twelve
minors), `gates-v2.md`, `INTEGRATOR-DECISIONS.md` ID-15 / ID-16 / ID-19 / ID-27 / ID-28 / ID-29,
`contract-v2.md` §4.3/§7.6, `contract-v3.md`, `contract-v5.md`.

Every experiment ran in two private detached worktrees, `/home/swung/w7/p4a-rereview-gates`
(@ `ecf9cb45`, its own `build-push`) and `/home/swung/w7/p4a-rr-rebase` (the rebase probe). Nothing
in `p4a-gates` was written to; both worktrees and their build dirs are removed. `CCACHE_BASEDIR=
/home/swung/w7`, `-j 8`, `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`. No device work, no retrace,
so no `git lfs checkout` was needed.

---

## Verdict

**ACCEPT WITH MINORS — merge is not blocked.**

All six majors are closed, and I re-proved five of them myself rather than reading the report's
transcript: G7 reaches `tripped` and exits **0** (§2.1); a SIGINT mid-rebuild leaves the tree
**empty under `git status --porcelain`** and the script dies **by SIGINT** (§2.3); commit 8's *other*
detection path — a child that returned ≥128 while bash never reached the trap — stops the run and
prints the interruption wording at exit 2 (§2.3); one leaked composite band slot makes
`EvictedPipelineCompositesReturnTheirShaderCsoSlots` fail **all three** assertions on **all three**
live-allocator lanes while every ordinary-space case stays untouched (§2.4); and the ABA conjunction
is directory-wide with no false positive on any tree in flight (§2.5). G5 is stronger than it was
(17 rows, **8** negative controls, the one-ref listing now byte-identical to the two-ref one) and
still green against the integrated pipe head. The rebase onto `712c9467` is **conflict-free, 8/8
commits, zero file overlap** (§4).

Four minors, none blocking; two of them are corrections to the report rather than to the code, and
one of those matters to the integrator's sequencing because **the §4 red-window warning is now
false** — esprytobj already carries the bit-10-requires-bit-11 row, so the 0x5ff arm goes GREEN at
integration, not red.

---

## 1. Per-item closure table

| item | v1 ask | landed | verified how | verdict |
|---|---|---|---|---|
| **F-M1** | `= {}` not `= 0`, so the scoped-enum control compiles | `p4a_descriptor_negative_control.sh:331` | script half: end-to-end run reached "the patched header still compiles" and `tripped` for **both** fields (§2.1). Compile half: the patcher's output on a real `SamplerParameters::borderColorForm` copy is `canon.borderColorForm = {} /* … */;` — accepted; `= 0` is `cannot convert 'int' to 'BorderColorForm'` | **CLOSED** |
| **F-M2** | the verdict cannot be polluted | global `CONTROL_VERDICT` (`:250`), plain call (`:455`), patcher writes to `sys.stderr` (`:339`), rows collected into an array first (`:445-452`), `</dev/null` on every child | exit **0** reached with a deliberately noisy stub builder writing to stdout during every step. Under v1's code this run scored `did-not-trip` | **CLOSED** |
| **F-M3** | `repair()` must run from the traps | state lives in this shell (`:220-232`, `:455`) | SIGINT to the process group with the patch demonstrably in the tree → trap fired, restore + rebuild + re-run, `git status --porcelain` **empty**, no `G7 NEGATIVE CONTROL` marker anywhere, exit **by SIGINT** (§2.3) | **CLOSED** |
| **commit 8** | *(found by F-M3's proof run)* stop carrying on past an interrupt | `INTERRUPTED` latch, `child_was_signalled`, `break`, separate summary wording | second path exercised on its own: child returns 130, script never got the signal → repair, `break`, `(INTERRUPTED - the remaining control(s) did not run)`, exit **2** (§2.3) | **CLOSED** |
| **F-M4** | read the band's own counters | `PipeSlotPeek.h:97-99` + `.cpp:57-73`; `SlotSpace` at `HandleRecycleScenario.cpp:217-247`, `:707`, `:2200`; `maxInFlight=1` | leak-one-composite reproduced independently (§2.4): 3 failed / 8, `live 2 -> 50 (peak 50), high water 983042 -> 983090 (floor 983040)`, all three assertions, on `DirectGLES.HandleRecycle.Handles.`, `DirectVulkan.HandleRecycle.Handles.` and `DirectVulkan.HandleRecycle.AbaControlHandles.`. Positive control (mint and free) **passes rather than skips**. The three peeks are 1:1 with c0b's API and that API is unchanged on `712c9467` | **CLOSED** |
| **F-M5** | directory-wide conjunction | `mgl_itest_probe_for_two_symbols`, `CMakeLists.txt:411-463`, `:580-595` | read + grepped four trees for both halves (§2.5). No false positive anywhere in flight; residual holes are narrow and named | **CLOSED** |
| **F-M6** | match the refusal as a LINE | `ObjectSubsystemControlScenario.cpp:181-220`, `:561-577` | the `/ERROR]` spelling is exactly what `MG_Util/Debug/Log.cpp:89-103` writes (`"/" + levelTag + "]: "`, `MGLOG_E` ⇒ `"ERROR"`); simulated against `p4a-esprytobj`'s **exact** sentences: matches both arms, rejects the framebuffer refusal, the bit-10-requires-bit-7 refusal, the same sentence demoted to WARN, and a line naming both bits without `REFUS` (§2.6) | **CLOSED** (one direction minor, F-v2-m1) |
| **ID-15 0x5ff arm** | write the arm now, SKIP visibly until D lands | case at `:614-692`, lane at `CMakeLists.txt:1446-1451`, `:1479-1487` | ran it: `***Skipped` with the full named reason quoted in §2.7. Expected refusal text checked against `p4a-esprytobj`'s HEAD — it **matches**, and the bit-7 check ahead of it does not pre-empt under 0x5ff | **CLOSED**, but see **F-v2-m2** |
| **F-m1** | fixed listing order | `:515-522`, `reorder_sha_list` `:529-541` | `p4a_untouched_regions.sh 37da3c3a` vs `… 37da3c3a 37da3c3a` — `diff` **empty** | CLOSED |
| **F-m2** | a control for the closing boundary | `perturb(..., where)` `:374-415`, `:642-680` | `--self-test` rc 0, **8** negative controls, head and tail per region, each named | CLOSED |
| **F-m3** | state the hash's extent | header `:74-82` | read; it also says the tail controls are what make the end covered | CLOSED |
| **F-m4** | the false skip reason / cover `AbaControlHandles` | `LanePinnedALiveAllocator()` `:194-202`, gate `:716-726` | measured: that lane now runs — it is one of the three that went red in §2.4 | CLOSED |
| **F-m5** | call `MagmaPipeAbaControlCoversKind` or delete it | exhaustive `constexpr` per-kind predicate + three `static_assert`s (`MagmaPipeArms.h:245-288`) | grepped: the *inner* predicate is pinned in every Magma build; the *wrapper* is still uncalled | CLOSED in substance — **F-v2-m3** for the wrapper |
| **F-m6** | "TWO in flight" is stale | folded into F-M4; the band's answer is 1 (`:2201-2203`) | read; matches the measured peak of 1 in the no-leak control | CLOSED |
| **F-m7** | separate "absent" from "zero" | `TextureUploadShapeScenario.cpp:326-383` + probe `CMakeLists.txt:622-633` | read: `clientEmitterExists` decides, `ctu == 0` is asserted when it does not, `ctu > 0 && ctu == emit` when it does. Verified the probe finds nothing on `712c9467` either, so the assertion stays on its zero arm through this integration | CLOSED |
| **F-m8** | narrow or document the `Layered` triple patch | header `:28-33` | read; narrowed in prose with the reason regexes may not name another package's helper | CLOSED |
| **F-m9** | `--expect-probes 4` | `test.yml:793-802` | ran it: `--expect-probes 4` **rc 0** ("4 probes, 0 skipped, 0 problems"), `--expect-probes 5` **rc 1** (`expected 5 probe(s), ran 4`). `clang++-20` kept with the reason | CLOSED |
| **F-m10** | verify the build directory | `:129-156`, reads PUSH **and** VERIFY | ran it against a synthetic pull cache (`PUSH:BOOL=OFF`, `VERIFY:BOOL=OFF`) → **rc 2**, naming both cache lines | CLOSED |
| **F-m11** | exec bits | `100644` | `ls -l`: both `p4a_*.sh` are `-rw-r--r--`, matching `p3a_untouched_regions.sh` and `g7_negative_control.sh` | CLOSED |
| **F-m12** | use `sawStale` | `HandleRecycleScenario.cpp:1328-1347` | read: `EXPECT_TRUE(sawStale)`, and the message names the third answer and prints the count | CLOSED |
| **F-m13** | declare the two `~/w7/notes/tools` rows | `gates-v2.md` §5 | read; ID-3 reassigns them and the report now says so. Correct | CLOSED |
| **R1** | not F's to fix | carried to D's round | ID-16 assigns it; nothing F can write through public GL closes it. ID-19 then made G9 white-box — that is **F v3's**, §6 | CARRIED |
| **R2** | the flip must not be forgettable | F-M5's fix | see §2.5 | CLOSED |
| **R3** | four commits fine | eight now, four of them rework | messages are eight single-line `[Type] (Scope): description`, C.5's three exact texts still present in order | CLOSED |

---

## 2. What I re-ran, and what it showed

### 2.1 G7 reaches "tripped" and exits 0 — and does the contract-tree thing on the contract tree

Two runs, one real and one instrumented:

* **On the clean tree with the real `build-push`** (configured Release/Ninja/clang from
  `p4a-gates/build-push`'s own cache, `MOBILEGL_PIPE_PUSH=ON`): **rc 2**, both controls
  `could-not-run`, each naming its header, its field, its owning package and the contract-stub
  reason, and the summary saying exit 2 is the expected answer here. Confirmed independently that
  `MG_Impl/Pipe/{FramebufferEmit,SamplerEmit}.h` assign nothing at `ecf9cb45` **and** at `712c9467`
  (`grep -c '\.Layered[[:space:]]*=' = 0`, `.borderColorForm` likewise), so exit 2 stays the right
  answer across this integration.
* **On a scaffolded tree** — the two headers given the one field copy each that B and C will carry,
  committed so the tree was clean at the start, with `cmake`/`ctest` replaced by stubs on `PATH`
  that make a suite red **iff** its header carries the control marker — the script produced:

  ```
  [p4a-g7] [FramebufferEmit-Layered] the patched header still compiles, …
  [p4a-g7] [FramebufferEmit-Layered] negative control tripped, naming Layered, and the tree is green again
  [p4a-g7] [SamplerEmit-borderColorForm] negative control tripped, naming borderColorForm, …
  [p4a-g7] ---- G7 (P4a descriptor emission) ----
    Layered (MobileGL/MG_Impl/Pipe/FramebufferEmit.h): tripped
    borderColorForm (MobileGL/MG_Impl/Pipe/SamplerEmit.h): tripped
  [p4a-g7] both controls tripped and named their field; exit 0
  G7_RC=0
  ```

  `git status --porcelain` empty afterwards, both field copies restored.

**What the stub does and does not prove, stated plainly.** It replaces the *builder and the runner*,
not the script: every line of `run_control`, the verdict propagation, the repair, the traps and the
summary are the shipped ones. What it cannot prove is that a real `FramebufferEmitTest` goes red
with the field dropped — that is B's and C's contract and is the integrator's row. It **does** prove
what F-M1/F-M2 were about: that exit 0 is now reachable at all, which under v1's code it was not,
for two independent reasons.

The patcher's replacement value is the one thing the stub cannot vouch for, so I checked it
separately: `SamplerParameters::borderColorForm` is `BorderColorForm` at
`MG_Pipe/MGPipeValueTypes.h:485`, `enum class BorderColorForm : Uint8` at `:456`, and `MGPSurface::
Layered` is a `Uint8` at `MG_Pipe/MGPipeTypes.h:545`. `= {}` is valid for both; `= 0` is valid for
neither the scoped enum in an assignment. F-M1's one character is right.

### 2.2 — *(merged into 2.1)*

### 2.3 The interrupt, both ways

The launcher matters, exactly as `gates-v2.md` §3.2 warns. I launched through Python with
`os.setsid()` and `signal.signal(SIGINT, SIG_DFL)` in `preexec_fn`, waited until the patched line was
visible in the working tree, and then `killpg(SIGINT)`:

```
===== mid-way: the patch IS in the tree =====
M MobileGL/MG_Impl/Pipe/FramebufferEmit.h
75:        dst.Layered = {} /* G7 NEGATIVE CONTROL: was src.Layered */;
===== sending SIGINT to the process group =====
[p4a-g7] [FramebufferEmit-Layered] rebuilding with the dropped field
[p4a-g7] restored MobileGL/MG_Impl/Pipe/FramebufferEmit.h; rebuilding from it
[p4a-g7] the tree is restored, rebuilt and green again
SCRIPT_EXIT_RC=-2   (killed by signal 2, i.e. 130 as a shell reports it)
===== tree AFTER the interrupt =====   (nothing tracked)
===== leftover control marker anywhere in MG_Impl/Pipe? =====   (none)
```

**One observation the integrator needs and the report does not say.** On this path the script dies
inside the `INT` trap's `kill -INT $$` (`:225`), so the `(INTERRUPTED - the remaining control(s) did
not run)` summary and the "the run was INTERRUPTED …; exit 2" wording at `:465` / `:477-479` are
**never printed**. That wording belongs to commit 8's *other* detection path, which I exercised on
its own by making the stub builder return 130 without signalling the script:

```
[p4a-g7] [FramebufferEmit-Layered] INTERRUPTED during the rebuild (exit 130). Repairing the tree and stopping:
[p4a-g7] ---- G7 (P4a descriptor emission) ----
  Layered (…/FramebufferEmit.h): could-not-run
  (INTERRUPTED - the remaining control(s) did not run)
[p4a-g7] the run was INTERRUPTED (exit 130); exit 2. …
G7_RC=2
```

Tree clean, second control not run. Both halves of commit 8 work; they simply produce different
exits (130 by signal, or 2 with the wording), and `gates-v2.md` §7 row 1 tells the integrator to
treat "exit 2 with the interruption wording" as a real finding without saying that a terminal Ctrl-C
usually produces the signal exit instead. One sentence.

### 2.4 The composite leak case really reads the band

I did not reuse the package's scaffold. In my own worktree I added two scaffold entry points to
`Harness/PipeSlotPeek.cpp` that mint and free **one band slot per churn round through the
allocator's only door**, `MGPipeSlots().AllocateComposite(lifetimeId)` / `.Free(ShaderCso, handle)`,
and called them from the composite round in `HandleRecycleScenario.cpp` — mint before `observe()`,
free after it unless `MGL_RR_LEAK_COMPOSITE` is set. Three runs of
`ctest -R EvictedPipelineComposites` (8 entries):

| tree | result |
|---|---|
| no scaffold (`ecf9cb45` as it is) | 8/8 **Skipped**, the "the client minted no ShaderCso (pipeline composites) [composite band] slot at all" reason |
| scaffold, **no** leak | the three live-allocator lanes **Passed** (they no longer skip — the band moved and came back); the five others skip | 
| scaffold **+ one leaked band slot per round** | **3 failed of 8**, on `DirectGLES.HandleRecycle.Handles.`, `DirectVulkan.HandleRecycle.Handles.` and `DirectVulkan.HandleRecycle.AbaControlHandles.` |

The red is the one the case claims, and all three assertions fire:

```
[ HandleRecycle ] backend=DirectGLES ShaderCso (pipeline composites) [composite band]
    live 2 -> 50 (peak 50), high water 983042 -> 983090 (floor 983040) over 48 create/draw/destroy rounds
HandleRecycleScenario.cpp:793: Failure   … 48 slots never came back
HandleRecycleScenario.cpp:804: Failure   … the … slot space grew with the churn
HandleRecycleScenario.cpp:809: Failure   more than 1 churned … object(s) were live at once
```

The numbers reproduce `gates-v2.md` §3.3 exactly. The middle row is the one the report does not
have and that I think matters most: with a mint that *does* come back, the case **passes rather
than skips**, which is what says `ReadSpaceHighWaterFloor` (`:240-246`) is not swallowing the
assertions at the band base.

**And the split is real.** Running the whole `HandleRecycle` family with the band leaking, the only
failures are the three composite entries; every ordinary-space leak case — texture, renderbuffer,
framebuffer, sampler, sampler view, ordinary program — is unmoved. That is c0b's `HighWater(kind)`
= ordinary-only, measured rather than argued, and it is exactly the blindness F-M4 was about,
inverted.

**Consistency with c0b on `712c9467`.** `PeekPipeCompositeSlotLiveCount` / `…HighWater` /
`…BandBase` (`PipeSlotPeek.cpp:57-73`) map 1:1 onto `MGPipeSlotAllocator::CompositeLiveCount()`,
`CompositeHighWater()` and `kMGPipeShaderCsoCompositeSlotBase`, and I re-read all three at
`712c9467`: `SlotAllocator.h:108`, `:114`, `MGPipeHandles.h:100`, unchanged by c0c, c0d, c0e or
wire. `PipeSlotPeek.h:52-61` now states c0b's split correctly and `:82-90` explains why HighWater is
returned absolute rather than base-relative — which is the right call and is what makes the third
member necessary.

### 2.5 The ABA-flip probe: can the flip still be forgotten?

`mgl_itest_probe_for_two_symbols` (`CMakeLists.txt:438-463`) now answers "both regexes matched
somewhere under the backend directory" and prints `<file A> + <file B>`. The layout dependency v1
found is gone.

**Can it still be forgotten?** I looked for the three ways:

* **A new file** — no. The glob is `file(GLOB_RECURSE … CONFIGURE_DEPENDS "*.h" "*.hpp" "*.cpp"
  "*.c")` and every file found is also appended to `CMAKE_CONFIGURE_DEPENDS` (`:444`), so both a new
  file and an edit to an existing one re-run the configure.
* **A renamed scenario** — irrelevant: the probe is over the backend directory, not over the test
  sources, and the marker is consumed by name (`MGITEST_HANDLE_ABA_OBJECTS_<backend>`) in
  `HandleRecycleScenario.cpp:329-337`.
* **A file the glob does not name** — the one residual. `.inc` / `.def` / `.hh` / `.ipp` under
  `MG_Backend/**` would be invisible. Measured: there are **no** non-`{h,hpp,cpp,c}` files under
  `MG_Backend` in any of the four trees in flight (`p4a-gates`, `pipe@712c9467`, `p4a-esprytobj`,
  `p4a-esprytdraw`), so it costs nothing today. Not raised as a finding; noted so the next reader
  does not have to re-derive it.

**And the false positive the change trades against is not live.** I grepped all four trees:
`MG_Backend/DirectVulkan` matches `PipeHandleAbaControl` in exactly one file (`MagmaPipeArms.h`) and
matches `kMGPipeSubsystem{Framebuffer,TextureResources,Samplers,Programs}` in **none** — including
after F's own edit to `MagmaPipeArms.h`, which was my first worry, since `file(STRINGS … REGEX)`
matches comments and the new `static_assert` block names all six P4a *kinds*. It names
`MG_Pipe::MGPipeKind::Texture` and friends, never the four subsystem constants, so the conjunction
stays false. `MG_Backend/DirectGLES` matches neither today; it will match the constants once
esprytobj/esprytdraw land (`Managers.cpp`, `DirectGLES.cpp` — measured), and still not the knob. So
the six controls keep asserting the correct pixels through this integration, which is right.

### 2.6 The 0x9ff refusal matches Espryt's line, not a substring

`FindTheRefusalLine` (`ObjectSubsystemControlScenario.cpp:190-209`) requires one line that carries
`/ERROR]`, carries `REFUS`/`refus`, and names both bits. Two checks:

* **The severity token is right.** `MG_Util/Debug/Log.cpp` builds the header as
  `"[" + time + "] [" + os + " " + thread + "/" + levelTag + "]: "` and `Log.h:81` defines
  `MGLOG_E` with `levelTag = "ERROR"`, so `/ERROR]` is the literal spelling and nothing else in the
  record can produce it.
* **The text is Espryt v2's, not v1's.** `p4a/esprytobj` is now at `b6169dba`; the shared helper is
  `PipeSubsystemDependencyMissing` (`Managers.cpp:3523-3529`), `MGLOG_E("MGPipe: %s - REFUSING the
  dependent bit and running the legacy arm. Set both bits, or clear both", what)`. I fed the four
  real `what` strings through the matcher:

  | line | 0x9ff case | 0x5ff case |
  |---|---|---|
  | sampler family: bit 11 set, bit 10 clear (`:3691-3695`) | match | match |
  | texture family: bit 10 set, bit 11 clear (`:3652-3657`) | match | match |
  | framebuffer: bit 9 set, bit 10 clear (`:3582-3586`) | no | no |
  | texture family: bit 10 set, bit 7 clear (`:3627-3631`) | no | no |
  | the same sentence demoted to `MGLOG_W` | no | no |
  | an ERROR line naming both bits but not saying it refused | no | no |

  So the assertion goes green on the real refusal and red on every near-miss I could construct.
  That is a real gate. The one thing it does not check is the **direction** — F-v2-m1.

### 2.7 ID-15's 0x5ff arm

Ran `ctest -R 'ObjectSubsystemControl.RefusedTexture' -V`: `***Skipped`, from
`ObjectSubsystemControlScenario.cpp:634`, with the reason naming the marker, the four constants,
D-K2's fourth row, ID-15, package D's file, and the fact that the library did come up under 0x5ff.
Exactly the "SKIPs by name" ID-16 asked for.

**The expected refusal text is right**, and the arm will go **green** rather than red at
integration — see F-v2-m2.

---

## 3. Findings

Each has a file:line, a failure scenario and the refutation I tried. None is blocking.

### F-v2-m1 (minor, new) — the refusal matcher does not check the DIRECTION of the dependency

`MobileGL/MG_IntegrationTest/Scenarios/ObjectSubsystemControlScenario.cpp:190-209`, used at `:562`
and `:668`.

`FindTheRefusalLine(log, bitThatWasSet, bitThatWasNeeded)` applies `LineNamesTheBit` to each list and
ANDs the two results. That conjunction is **symmetric**: swapping the two arguments — which is
precisely what `:665-668` says makes the 0x5ff case different from the 0x9ff one ("the same line
shape … with the two bits' roles swapped") — cannot change the answer. Measured in §2.6: every row
of that table is identical in both columns.

**Failure scenario.** Package D's texture-family resolver refuses correctly under 0x5ff but prints
the mirror sentence. This is one copy-paste away: `Managers.cpp:3652` and `:3691` are forty lines
apart in the same file, share one helper and differ only in the `what` string, and the file's own
comment at `:3632-3633` calls the new row "the exact mirror" of the old one. The 0x5ff lane then
goes green on a refusal that told the operator the wrong dependency, which is exactly the class of
"the log says something plausible" defect F-M6 closed one level up.

**Refutation I tried, and it half succeeds.** For the wrong sentence to be in the 0x5ff lane's log,
`ResolveSamplerSubsystemArm` would have to run its refusal, and it only does so when bit 11 is set
(`:3687-3696`); 0x5ff leaves bit 11 clear, so on esprytobj's HEAD that line cannot appear. The
finding is therefore **not exploitable against today's Espryt** — it is an assertion that does not
check what its own comment says it checks, and that would stop being safe the moment the two
refusals are ever emitted from one place or one of them is made unconditional.

**Fix (small).** Require the ordering as well as the presence: find each spelling's offset in the
line and require the SET bit's offset to be smaller than the NEEDED bit's — Espryt's sentence is
`"<A> … is set but <B> … is clear"`, so the order is the direction. Two lines in
`LineNamesTheBit`'s caller.

### F-v2-m2 (minor, new, and it changes the integrator's sequencing) — §4's red window no longer exists

`gates-v2.md` §4 (and §7's third row) tell the integrator:

> **the moment esprytobj is integrated, this entry stops skipping and goes RED until package D's
> rework adds the bit-10-requires-bit-11 row** … Either sequence D's rework before F, or expect that
> red

That was true of `p4a/esprytobj` v1. It is not true now. `p4a/esprytobj` is at **`b6169dba`**, and
its commit **`a8c8824e`** — "*…and refuse bit 10 without bit 11*" — carries the row:
`ResolveTextureResourceSubsystemArm`, `Managers.cpp:3652-3657`,
`PipeSubsystemDependencyMissing(mask, kMGPipeSubsystemSamplers, "kMGPipeSubsystemTextureResources
(bit 10) is set but kMGPipeSubsystemSamplers (bit 11) is clear; …")`. So at integration the
`RefusedTexture` lane arms **and passes**.

**Failure scenario if the sentence stands.** The integrator either re-orders the phase to avoid a
red that is not there, or — worse — reads a genuine red on that lane as "expected, see §4" and ships
a resolver that refuses for the wrong reason or not at all.

**Refutation I tried.** (a) Could the bit-10-requires-**bit-7** refusal at `:3627-3631` fire first
and short-circuit the bit-11 one? No: `0x5ff = 0x1ff | 0x400` and `0x1ff` already carries bit 7
(`0x80`), so that check passes and `if (!refused)` at `:3651` lets the bit-11 check run. (b) Could
the arming marker fail to set? No: `MGITEST_HANDLE_REKEY_OBJECTS_DirectGLES` probes
`MG_Backend/DirectGLES` for the four constants and `Managers.cpp` names them (grepped). (c) Could
the sentence not match? No — §2.6's table.

**Fix.** One line in `gates-v2.md` §4 / §7 and in the integrator's re-run list: the 0x5ff arm is
expected **green** once esprytobj v2 is in; a red there is a real finding about D's resolver.

### F-v2-m3 (minor, residual of F-m5) — the wrapper is still uncalled

`MobileGL/MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h:292-294`. `MagmaPipeAbaControlCoversKind`
has no caller anywhere in the tree (grepped `MobileGL/**`); only `MagmaPipeAbaControlKindIsRekeyedHere`
is referenced, by the three `static_assert`s at `:273-288`.

**Failure scenario.** The rot v1 described, one level up: the wrapper's `&&` could be inverted or
one operand dropped and nothing would say so, because it is not `constexpr` (it reads
`MG_Config::Features`) and no assert can pin it.

**Refutation, and it mostly succeeds.** The substance of F-m5 *is* closed: the per-kind table — the
part that could be silently wrong about which kinds are covered — is now exhaustive with no
`default:` and pinned in both directions by asserts that compile in every Magma build, which is a
better answer than the token caller v1 asked for. The wrapper is three lines whose entire body is
the conjunction of two things that are each pinned or trivial. Judged: **carry**, or delete the
wrapper and let the two pinned pieces stand alone. Not worth a rework round.

### F-v2-m4 (trivial, new) — `REPAIR_RC` is write-only

`scripts/p4a_descriptor_negative_control.sh:179`, `:188`, `:193`, `:199`. It is set to 2 on three
failure paths inside `repair()` and never read anywhere. The header's claim at `:90` — "*a repair
that itself fails downgrades the verdict to 2*" — is true, but it is delivered by `run_control`'s
`if ! repair; then … CONTROL_VERDICT=could-not-run` at `:407-413`, not by this variable. A reader
tracing the claim finds a dead one. Delete it, or read it after the loop so a trap-time repair
failure also reaches the exit code.

### Nothing new elsewhere

* **Hygiene: clean.** Eight single-line `[Type] (Scope): description` messages, no bodies, no
  `Co-Authored-By` and no other attribution (grepped for it and for tool names). Only F's nine files
  plus the granted `MagmaPipeArms.h` and the two scripts. No trailing whitespace in either script.
  Both `scripts/p4a_*.sh` are `100644`.
* **No scaffold leftovers.** `tmp/gatesF-g7proof` does not resolve; no commit reachable from any ref
  mentions the scaffold; the only file in the tree containing the word `SCAFFOLD` is the
  pre-existing `MG_Util/SelfTest/DriverBugProbes.cpp`. `p4a/gates` is the only branch containing
  `ecf9cb45`.
* **`test.yml`.** The rework's only change to it is the include-closure step; `git diff HEAD~5
  ecf9cb45 -- .github/workflows/test.yml` is that hunk and nothing else. The **TEMPORARY** trigger
  lines at the top (`:9`) are untouched and **`BASELINE` is still `37da3c3a`** (`:1599`), with both
  P3a and P4a region gates reading it.
* **The itest CMake.** The rework adds exactly one `TEST_PREFIX` and one `TEST_FILTER`
  (`DirectGLES.ObjectSubsystemControl.RefusedTexture.` / the 0x5ff case) and renames nothing —
  `git diff HEAD~5 ecf9cb45` over that file shows two `+` lines and no `-` line among them. Re-derived
  the pin audit on the whole file: every `MOBILEGL_PIPE_PUSH=` env pin is `0x1fff` except
  `ResourceSubsystemControl`'s Off lane and `LargeArenaAdoption`'s Off lane (`0x7f`, `:1304`,
  `:1372`), `ObjectSubsystemControl`'s Off lane (`0x1ff`, `:1438`), the two refusal lanes
  (`0x9ff` `:1443`, `0x5ff` `:1448`), the CSO Off lanes (`0x8000000000001fff`) and the three
  `MOBILEGL_PIPE_PUSH=0` arm lanes. Exactly C.5's shape.
* **G14, re-derived.** `ctest --test-dir build-push -N` on my own build of `ecf9cb45`: **2711**
  names; against `~/w7/p4a-before-ctest-names.txt` (37da3c3a, 2588 names, `LC_ALL=C`) —
  **0 removed, +123 added**, of which 109 carry a `DirectGLES.`/`DirectVulkan.` prefix (F's itest
  entries) and 14 are the contract's unit cases (`PipeCatalogue` ×4, `ProgramArtifactsCodec` ×4, the
  five `*Emit.TheEmitterIsOneNeverDestroyedProcessSingleton`, `CompositeResolver.TheCompositeBandHas
  ExactlyOneDoor`). The report's 2711 / 0 / +123 are correct.
  *(A caution for whoever re-takes this: extracting names with `awk '{print $3}'` truncates the five
  `Shapes/DemoteFloat64EsslTest…` parameterised names at their first space and manufactures a
  spurious 5 removed / 5 added. Use `sed -n 's/^ *Test *#[0-9]*: //p'` and `LC_ALL=C`.)*
* **G5, re-derived.** `p4a_untouched_regions.sh 37da3c3a ecf9cb45` **rc 0**, 17 rows;
  `--self-test` **rc 0**, 3 positive + **8** negative controls, every one tripped and named; the
  exit contract is `2/2/2/2/2` for no args / bad `<ref-a>` / bad `<ref-b>` / 3 args /
  `--self-test extra`; `p3a_untouched_regions.sh` is still rc 0 both ways at this head.

---

## 4. The rebase report — `ecf9cb45` onto `feat/disaggregated` = `712c9467`

Run in a throwaway worktree (`git worktree add --detach … ecf9cb45`, then
`git rebase refs/heads/feat/disaggregated`). **`REBASE_RC=0`, 8/8 commits, no conflict, nothing to
resolve.** Head `01c0ea21`, the eight commits in order on top of `712c9467`.

The reason is structural rather than lucky: `git diff --name-only 2cb44039 refs/heads/feat/
disaggregated` and the same for F are **disjoint** — pipe moved `MG_Impl/Pipe/Tracker.h`,
`MG_Pipe/{MGPipeTypes.h, PipeApply.{h,cpp}, PipeFields.def}` and eight `MG_Test/Pipe/*Test.cpp`
(14 files, +4982/−116); F owns `MG_IntegrationTest/**`, `MagmaPipeArms.h`, `scripts/p4a_*.sh` and
`test.yml`. `comm -12` over the two lists is empty.

**ID-28's three named questions, answered on the rebased tree:**

| question | answer |
|---|---|
| `PeekPipeCompositeSlot*` vs c0b's `CompositeHighWater` API | Unchanged. `SlotAllocator.h:108` `CompositeHighWater()`, `:114` `CompositeLiveCount()`, `MGPipeHandles.h:100` `kMGPipeShaderCsoCompositeSlotBase` all present at `712c9467` with the same signatures; the three peeks are verbatim wrappers. c0c/c0d/c0e/wire did not touch `SlotAllocator.*` or the band constants |
| the itest CMake pins | Untouched by pipe — `MG_IntegrationTest/**` is not in pipe's changed-file list. The lanes' `0x1fff` / `0x1ff` / `0x9ff` / `0x5ff` / `0x7f` pins are still the phase constants they name |
| the negative-control scripts' targets after c0c renamed `MGPSurface::Pad0` → `TextureTarget` and wire changed `PipeApply`'s signatures | **Both scripts survive.** G7 targets `MGPSurface::Layered` (`MGPipeTypes.h:545` on the rebased tree — c0c renamed the *other* two bytes, `Pad0` → `Uint16 TextureTarget` at `:566`, and says so in the comment at `:549-551`) and `SamplerParameters::borderColorForm` (`MGPipeValueTypes.h:485`, untouched). Neither script names `PipeApply` at all. G5's seventeen rows are backend function/namespace names in `Managers.cpp`, `DirectGLES.cpp` and `Utils.cpp`, none of which pipe touched: **`p4a_untouched_regions.sh 37da3c3a 712c9467` is rc 0 with the same 17 shas, byte-identical to the `37da3c3a → ecf9cb45` listing** |

**The rebase is not just textually clean — I built and ran it.** Configured and built the rebased
worktree (Release/clang/Ninja, `MOBILEGL_PIPE_PUSH=ON`, integration tests on): **BUILD_RC=0**, and

| on `ecf9cb45` rebased onto `712c9467` | result |
|---|---|
| `ctest -L unit` | **1684/1684**, 0 failed — the same 1684 ID-28 records for pipe's post-merge |
| `ctest -R 'HandleRecycle\|ObjectSubsystemControl\|TextureUploadShape\|TextureParamsWithoutASamplerView'` | **165/165**, 0 failed |
| `p4a_untouched_regions.sh 37da3c3a HEAD` / `--self-test` | **rc 0** / **rc 0** |
| `p4a_descriptor_negative_control.sh build-push` | **rc 2**, both `could-not-run` — still the right answer, the emit headers are still the contract's stubs |
| `ctest -N` | **2759** (2711 + the 48 names wire's real emission cases add) |

So `PipeSlotPeek.cpp` still compiles against c0b's allocator on the integrated tree, the four new
lanes still register, and no probe flipped.

**What still moves at integration, and it is not nothing.** Wire's rework put real cases into seven
`MG_Test/Pipe/*EmitTest.cpp` suites, so (a) G7's "`N` matching test(s) before the patch" line changes
(and, better, the suites are now real, so the two controls will have something to make red as soon
as B and C land); (b) the unit counts, the `ctest -N` totals and the G14 delta all move without
anything F owns changing. That is `gates-v2.md` §7's last row and it is right. G7's verdict on the
rebased tree is still **rc 2, both `could-not-run`** — the emit headers are still the contract's
stubs at `712c9467`, which I checked directly.

I also confirmed that none of the itest **probes** flips at this integration: `MG_Impl/Pipe` on the
rebased tree still emits neither `FramebufferEmissions` nor `ClientTextureUploadEmissions`, and
`MG_Backend/**` still names none of the four subsystem constants — so `ObjectSubsystemControl`'s
emission case, `TextureUploadShape`'s client half, the six `HandleRecycle` P4a cases' Handles arm
and both refusal lanes keep the arming they have today. No surprise reds land with wire.

**Caveat.** F integrates **last** (ID-29: after D v2), so `feat/disaggregated` will have moved again
by then; this rebase says the shape is clean, not that the one at integration time will be. Section
6's numbers must be re-taken there regardless.

---

## 5. On ID-19's G9 — correctly absent from v2

ID-19 makes G9 a **WHITE-BOX** assertion and assigns it to "gates v3 / F's verification round";
ID-16, which specified *this* round, does not mention it. v2 does not contain it, and that is right,
not an omission. `gates-v2.md` §5's "Not fixed, and why" says the same thing about R1's residual gap
and points at D — also right: the gap is that the four public-GL cases catch *emitted-but-not-applied*
and cannot catch *deferred-to-first-view*, because the sample that observes the parameter is what
pushes it (`TextureParamsWithoutASamplerViewScenario.cpp:243-257`, and the mechanism v1 §3.4
derived, which I re-read and still agree with).

---

## 6. What F v3 must contain

1. **G9 as a white-box assertion (ID-19, brief section F).** The shape the gap dictates: the four
   existing public-GL cases stay as the end-to-end control, and each gains a reading taken **while
   the texture is still attachment-only — before the observing sample repairs the state**, through a
   new isolated harness translation unit (`MG_IntegrationTest/Harness/PipeApplyPeek.{h,cpp}`, for
   `PipeSlotPeek`'s stated reason: the `MG_Pipe` headers and the GL headers are not meant to meet in
   one TU). It needs **both** sides of the seam, because one alone reproduces the blind spot at a
   lower level:
   * **the applier's own params record** for that texture handle — that a `set_texture_params`
     record exists at the serial the client emitted, and that the field the case moved (Swizzle /
     `DepthStencilMode`) is in it;
   * **Espryt's applied state** for that texture's backend twin — that the driver-side value is that
     value **now**, not after the first sampler view is created;
   * and that **no sampler view exists for that texture yet**, which is what turns "applied" into
     "applied without one" and is the whole claim.

   Assertion shape: after the frame and *before* any sampling, (a) record present at the emitted
   serial, (b) twin's applied value equals it, (c) no sampler view for that handle. A peek that
   cannot look must `GTEST_SKIP` by name, exactly like `PipeSlotPeek` (pull build, Android hidden
   visibility), and it must arm on **package D's applier/backend state**, not on the emitter markers
   B and C set — they are different questions and a shared marker would re-create F-M5's shape.
2. **F-v2-m1** — make the refusal matcher check the direction (offset ordering), so the 0x5ff and
   0x9ff arms really are different assertions.
3. **F-v2-m2** — correct §4/§7: the 0x5ff arm is expected **green** once esprytobj v2 is in, and a
   red there is a finding about D's resolver.
4. **F-v2-m4** — delete or read `REPAIR_RC`.
5. **F-v2-m3** (optional) — call or delete `MagmaPipeAbaControlCoversKind`.
6. **The rebase and section 6, re-taken on the pipe of the day** — G1, the three `-L unit` runs, the
   pull/push name diff, the G14 delta, `-L integration-gpu` on both builds. The emission suites now
   carry real cases, so every one of those numbers moves for reasons outside this package.
7. **One sentence** on the interrupt: a terminal Ctrl-C exits **by SIGINT (130)** and prints no
   summary; the `(INTERRUPTED …)` wording at exit 2 belongs to the signalled-child path. Both are
   correct behaviour; §7's first row currently implies only the second exists.

Carried, unchanged: R1's second shape is D's verification round; wire's cube-face case is wire's.

---

## 7. What the integrator must re-run (delta to `gates-v2.md` §7)

`gates-v2.md` §7 stands. Amend two rows and add one:

| row | amendment |
|---|---|
| `ctest -R 'ObjectSubsystemControl.RefusedTexture'` as soon as esprytobj is integrated | **Expect GREEN, not red.** esprytobj `a8c8824e` already carries D-K2's fourth row; the sentence it emits matches the assertion (verified). A red here is a real finding (F-v2-m2) |
| the G7 row | A terminal Ctrl-C exits **130 with no summary**; exit 2 *with* the interruption wording means a child died on a signal the script itself never received. Both mean "nothing here is a verdict about the controls" |
| **new** | `bash scripts/p4a_untouched_regions.sh 37da3c3a HEAD` at every integration step, not only after D and E: it is rc 0 against `712c9467` today with the same 17 shas, which is the cheapest possible check that a merge did not quietly touch Espryt's protected regions |

---

## 8. Method note

`p4a_descriptor_negative_control.sh`'s "tripped" path was exercised with `cmake` and `ctest`
replaced by stubs on `PATH`, against a scaffolded pair of emit headers, in a private worktree. The
script itself was the shipped one and every line of its control flow ran; what was stubbed is the
builder and the runner. The composite-band experiment used a **real** build of the integration test
(`build-push`, Release/clang/Ninja, `MOBILEGL_PIPE_PUSH=ON`, llvmpipe) with a scaffold that mints and
frees band slots through `AllocateComposite` / `Free`. The SIGINT proof was launched through Python
with the disposition reset to `SIG_DFL` in a new session, which is the only way to measure the
script rather than the launcher (`gates-v2.md` §3.2, confirmed).

Both private worktrees, their build directories and all scratch files under `~/w7` have been
removed.
