# Adversarial review of package E — `p2/gates` — v3

Tree `~/w7/p2-gates` @ `aa0fdeef`, eleven commits on `refs/tags/p2/contract` (`9c6a8a25`).
Reviewed against `BRIEF-P2.md` §A (G1–G14), §B (D1–D20), §C.4, §C.5, §D and §E, and against the
implementer's `gates-v3.md`. Everything below was re-run independently in `~/w7/p2-gates`; nothing
was fixed and the tree is left as found (`git status --porcelain` → `?? build-retrace/`, the same
line it carried on arrival; `MGPipeRenderStateSpans.{h,cpp}` sha256 re-checked identical after the
one script run that patches them).

**Verdict: NOT APPROVED — 1 major, 8 minors.**

The major is new in this round: it was introduced by `aa0fdeef`, the last commit, and it is
invisible to every check the implementer ran because all of them invoke the affected scripts as
`bash <script>`.

---

## 1. What I re-ran and what it said

| gate | command (in `~/w7/p2-gates`) | observed | matches gates-v3 |
|---|---|---|---|
| G1 | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text +160`; resized = `RenderState::RenderState()` +148, `SetCapability` +77, `IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 | yes (C-2 confirmed: the 4th is the contract's, not E's) |
| G2 | `ctest -N` (extracted with `sed -n 's/^ *Test *#[0-9]*: //p'`), pull vs push | 2401 names each, `diff` **empty** | yes |
| G2 | `ctest --test-dir build-linux -L integration-gpu --no-tests=error` **serial**, then `build-push` | `100% tests passed, 0 tests failed out of 912` **in both** | yes |
| G5 | `RenderStateImpl` sha at contract / HEAD / `~/w7/p2-before-syncrenderstate.sha` | all three `d8fd1c48…0efe27` | yes |
| G7 | `bash scripts/g7_negative_control.sh build-push` | rc **2**, refusing with "that test is package A's" — the *missing-test* arm works | yes |
| G7 | `bash scripts/g7_negative_control.sh build-push --verify-patch-only` | rc **0**; patch applied, `build-push` rebuilt, restored, rebuilt again; both spans files sha-identical afterwards | yes (mechanism independently confirmed on the contract's own header) |
| G8/G12 | `ctest --test-dir build-{linux,push} -R 'HandleRecycle\|CsoContentAddressing' --no-tests=error -j 4` | 34 entries each, `100% passed`; **13 run for real** (the 6 Legacy cases + 7 `TheReproducerRecyclesEveryName`), 21 skip with a named reason | yes |
| G8/G12 | same in `build-verify` | 44 entries, `100% passed` | yes |
| G12 | `ctest --test-dir build-bench -R DriverBench --no-tests=error` | 2 passed; `DriverBench zzz_not_a_case` → **rc 2** + case list; `DriverBench mc_state_toggle` → `mc_state_toggle,120,46,1.520,33042.8,657.9` | yes |
| G9 | `python3 scripts/gen_pipe_dirty_surface.py --check` / `--self-test` | **rc 2** — `unrecognized arguments` (package B's flag) | yes, D-7 |
| G13 | `grep -rc pGLContext MobileGL/MG_Backend \| grep -v ':0$'` | empty | yes |
| G13 | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)` | yes |
| G13 | `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0; `git status` clean afterwards | yes |
| G14 | `comm -23` baseline (2363, sorted) vs this tree's pull build | **0 removed, 38 added** (34 E's + 4 the contract's stubs) | yes |
| — | `ctest -L unit --no-tests=error -j 8` in `build-{linux,push,verify}` | `100% tests passed, 0 failed out of 1489` × 3 | yes |
| — | ownership: `git diff --stat refs/tags/p2/contract..HEAD -- MG_Pipe/ MG_Test/ MG_State/ MG_Backend/ MG_Impl/ MG_Util/ scripts/gen_pipe*.py CMakeLists.txt Config.h ConfigLoader.cpp` | **empty**; fixtures diff 0 files; 18 files touched, all E's per C.5 | yes |
| — | commit hygiene | no `Co-Authored-By` / `Signed-off-by` / `Generated with`; all eleven subjects `[Type] (Scope): …` | yes |

**C-1 independently confirmed.** `ctest --test-dir build-linux -N | grep -cE '^\s+Test #'` → **1402**;
the correct `sed` extraction → **2401**. The brief's §A G14 command and its baseline recipe both drop
every test numbered 1–999. The implementer's diagnosis is right and the integrator must fix §A.

**Mechanism checks the implementer claimed, re-derived rather than taken on trust:**

* the G7 demotion really demotes. `MGPipeRenderStateSpans.h:104` is
  `constexpr Bool MGPipeRenderStateChunkIsPipeline(SizeT index) { return (index % 2) == 1; }` and the
  boundary array is `kMGPipeRenderStateChunkBoundaries` (`:58`), so inserting boundaries at
  `ColorMasks` (549) and `FramebufferSrgbEnabled` (581) splits global chunk 3 `[312,584)` into
  3 `[312,549)` pipeline / 4 `[549,581)` **dynamic** / 5 `[581,584)` pipeline. `ColorMasks` is 32 bytes,
  which is exactly the `396 → 364` / `772 → 804` the patch relaxes. All eight patch anchors exist
  exactly once in the contract's own files (verified by `grep -cF`), so the control is not silently
  keyed to a tree it can no longer patch.
* the CSO channel really exists and is unconditional inside the push arm:
  `MG_Util/Metrics/PipeStats.cpp:421-428` emits `cso[csom=… csob=…]` inside `#if MOBILEGL_PIPE_PUSH`
  with no zero-suppression, so `ASSERT_TRUE(window.found)`'s message is accurate.
* `0x7f` / `0x800000000000007f` parse. `ConfigLoader.cpp:172-196`'s `QueryEnvUint64` accepts an
  explicit `0x` prefix (and rejects octal and `-`), and `MGPipe.h:83` is
  `kMGPipeBehaviourNoCsoContentAddressing = 1ull << 63`, `:85` `kMGPipeSubsystemsMigratedAtP2 = 0x7full`.
  The two CSO lane masks are therefore the ones D.4.5 specifies.
* `build-verify` really is a push build for the new lanes: `CMakeLists.txt:469-471` sets
  `MOBILEGL_PIPE_PUSH ON` as a normal variable when `MOBILEGL_PIPE_VERIFY` is on, and
  `grep -rho 'MGITEST_PIPE_PUSH_BUILD=1' build-verify/MobileGL/MG_IntegrationTest/ | wc -l` → **24**
  (0 in `build-linux`). So the new CI step is not asserting into a pull runtime.
* the new CI step's artifact really carries the unit binaries: the top-level
  `build-verify/CTestTestfile.cmake` `subdirs("MobileGL/MG_Test")` directly (no intermediate
  `MobileGL/CTestTestfile.cmake` is needed), which is what the packaging step at
  `test.yml:456-471` tars. `build-linux` is built `-DMOBILEGL_BUILD_BENCHMARK=ON` (`test.yml:99`) and
  the `benchmark` job runs `ctest -V -C Release -L benchmark` against it (`test.yml:806`), so
  `DriverBenchStateToggle` genuinely runs in CI.
* the four capability probes' negative verdicts really print. A forced reconfigure of `build-push`
  emitted all four `-- Integration tests: no … will SKIP` lines, so a permanently-false "yes" is at
  least visible at configure time.

---

## 2. MAJOR

### M-1 — `tools/device_bench/{bench.sh,session.sh}` lost their executable bit in `aa0fdeef`, and the README's own documented invocation now fails

`aa0fdeef` (this round's last commit, the one that fixed minors 6/7/8/11/12) rewrote both scripts
through a path that dropped the mode. The change is committed, not a worktree artefact
(`core.fileMode=true` in this worktree and in `~/w7/pipe`):

```
$ git -C ~/w7/p2-gates ls-tree refs/tags/p2/contract -- tools/device_bench/
100755 blob …  tools/device_bench/bench.sh
100755 blob …  tools/device_bench/profile.sh
100755 blob …  tools/device_bench/session.sh

$ git -C ~/w7/p2-gates ls-tree HEAD -- tools/device_bench/
100644 blob …  tools/device_bench/bench.sh      <-- was 100755
100755 blob …  tools/device_bench/profile.sh
100644 blob …  tools/device_bench/session.sh    <-- was 100755
```

Per-commit walk (`git ls-tree <c> -- <file>` over `git rev-list refs/tags/p2/contract..HEAD`): both
files are `100755` through `fbfb85af` and `100644` at `aa0fdeef`. `profile.sh`, which `aa0fdeef` did
not touch, kept `100755` — so this is not a tree-wide or filesystem effect, it is these two files.

Reproduction and observed output:

```
$ cd ~/w7/p2-gates && ./tools/device_bench/bench.sh --device devices/odinlite.env --backend magma
bash: ./tools/device_bench/bench.sh: Permission denied      rc=126
$ cd ~/w7/p2-gates/tools/device_bench && ./bench.sh --help
bash: ./bench.sh: Permission denied
```

That is precisely the invocation the file's own README documents, three lines of it, in the
unchanged block immediately below E's new "Devices" table:

* `tools/device_bench/README.md:62` `./bench.sh --device devices/odinlite.env --backend magma          # 30 samples, 180 s warmup`
* `tools/device_bench/README.md:63-64` — the espryt and mobileglues variants.

Why this is a major rather than cosmetic:

1. These two scripts are the harness for **D.4.2 / G11**, the paired two-device A/B that is
   GO/NO-GO items 3 and 4 and, per §E's risk table, "the schedule's long pole". The operator's first
   command under a 25-minute device lock is `./bench.sh …` and it fails with `Permission denied`.
2. It is **undeclared**. It is not in gates-v3 §5 (deviations), not in §6 (tree-vs-brief), not in §8
   (unfinished). The implementer did not know.
3. It is **invisible to the package's own verification**, which is the reason it survived: every
   command in gates-v3 §4.1/§4.2/§3-minor-7/§3-minor-8 invokes the scripts as `bash bench.sh …` /
   `bash session.sh …`, which ignores the mode. I reproduced all four of those guard checks and they
   all pass under `bash` — the guards are fine; only the mode is wrong.
4. It contradicts the package's own stated standard. E's whole argument for the
   `PROFILE_VERIFIED` guard is that "the pin path is silent when it is wrong"; a measurement harness
   that cannot be executed as documented is the same class of defect one step earlier.

Fix is one command (`git update-index --chmod=+x tools/device_bench/bench.sh tools/device_bench/session.sh`
and amend/append a commit), plus a re-check that no other file in the eleven commits changed mode.

---

## 3. Minors

**m-1 — the `AbaControl` arm SKIPs on this tree, so the artefact §C.4 makes E responsible for does not exist.**
Brief §C.4:661 says E "writes `HandleRecycleScenario` **first**, so its `AbaControl` arm is proven red
before C and D exist — that is the '重键前红' evidence, recorded with the run log", and §C.4's
verification block (:683-684) expects `.Legacy` **and** `.AbaControl` to *pass* on the contract tree
with only `.Handles` skipping. D18:435 likewise makes `AbaControl` "green by asserting the
corruption". Observed here: `DirectVulkan.HandleRecycle.AbaControl.HandleRecycleScenario.{AVertexArray…,ATexture…,AFramebuffer…}`
all **Skipped** (`MGITEST_HANDLE_ABA_IMPLEMENTED` unset — `grep -rho MGITEST_HANDLE_ABA_IMPLEMENTED build-verify/… | wc -l` → 0),
and only `TheReproducerRecyclesEveryName` runs in that lane. The implementer is right that the knob's
consumer lives in `MG_Backend/DirectVulkan/**`, which C.5:708 gives to package D — the brief is
internally inconsistent and E cannot close it. Declared as D-1 with the integrator action named.
Kept as a minor, but the integrator must not read E's merge as having produced
`~/w7/p2-handlerecycle-before.log`; it has to be taken on the contract tree after D lands the knob
and before D enables the re-key, or ROADMAP.md:18's "重键前红" has no evidence at all.

**m-2 — E imposes an identifier contract on packages C and D that is nowhere in the brief and that C and D have not been told.**
`MG_IntegrationTest/CMakeLists.txt:406-452` decides whether G8's `Handles` and `AbaControl` arms
assert at all by grepping the owning package's directory for a literal:
`kMGPipeSubsystemEsprytSlots` under `MG_Backend/DirectGLES`, `kMGPipeSubsystemMagmaVertexInput` and
`PipeHandleAbaControl` under `MG_Backend/DirectVulkan`, `RenderStateCso(Mints|Binds)` under
`MG_Impl/Pipe`. Nothing in §B or §C requires C or D to *name* those symbols — a backend that gates its
arm on `MG_Config::Features.PipePush & (1ull << 5)`, or that reads the bit through a helper in a
directory it does not own, satisfies D13/D14 and leaves the arm a permanent skip **with a reason that
has become false**, which is exactly the failure mode this file's own header says it exists to
prevent. gates-v3 §7 records the requirement as a note to the integrator; that is not the same as
telling C and D. Mitigation is real but weak (a configure-time `message(STATUS)` nobody gates on).
Integrator action: state the contract to C and D before they land, or replace the source probe with
a runtime one (the library already knows whether the subsystem bit steered anything).

**m-3 — 53 backend source files are now `CMAKE_CONFIGURE_DEPENDS`, so every edit to any DirectGLES/DirectVulkan source forces a full CMake re-configure.**
`mgl_itest_probe_for_symbol` (`MG_IntegrationTest/CMakeLists.txt:87-99`) appends every file it globs
to `CMAKE_CONFIGURE_DEPENDS`. Counts here: DirectGLES 10, DirectVulkan 43 (probed twice), MG_Impl/Pipe 4.
Reproduced: `touch MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp && cmake --build build-push`
prints `Re-running CMake…` and `Configuring done (3.8s) / Generating done (0.1s)` plus a
`gtest_discover_tests` re-run over ~2400 entries, before compiling one file. Packages C and D iterate
on exactly these files. It is declared in the comment as intentional ("an edit to one re-runs it"),
so it is a minor, but it is a per-edit tax on the two packages on the critical path and it is not
recorded in gates-v3 §5.

**m-4 — `PASS_REGULAR_EXPRESSION` weakened the pre-existing `DriverBench` entry's exit-code check.**
`MG_Benchmark/Driver/CMakeLists.txt:30-32` adds a `PASS_REGULAR_EXPRESSION` to the entry that
previously had none. As E's own comment records, ctest then ignores `retVal`
(`success = retVal == 0 || !RequiredRegularExpressions.empty()`). So `DriverBench draw_tiny` now
passes on a run that prints its row and then crashes or exits non-zero — a case that was red before
`718818f1`. The trade buys a much stronger "the case still ran" check and is defensible, but it is a
net loss on one axis of an existing gate and is not declared. (`DriverBenchStateToggle` is new, so
only the pre-existing entry is affected.)

**m-5 — a global setting was left changed on a shared campaign device.**
gates-v3 §5 D-12 declares an unlocked ~25-second run of `session.sh` against `35d0befa` — the GL4.6
Wave-7 Adreno phone — with FCL launched, `am force-stop` and `svc power stayon false` cleaned up, but
`settings put global fan_mode 3` **"was left as the script set it"**. That device is shared with a
campaign whose measurements are thermal-window-sensitive (`perf-test-protocol`: 40 °C gate,
sport-fan discipline). Integrator action: restore `fan_mode` on `35d0befa` and tell the Wave-7 owner
which window was touched. Also note the lock protocol in §D.4.2:843 was not followed for that run.

**m-6 — `pipe-gates` is red on this branch in isolation, and the branch carries the gate that makes it so.**
`.github/workflows/test.yml:1595-1598` now runs `gen_pipe_dirty_surface.py --check && --self-test`;
both flags belong to package B and both exit **rc 2** here (`unrecognized arguments: --check`).
D.1's order (contract → tracker → magma → espryt → gates) fixes it, but until `tracker` has landed
`gh workflow run test.yml` on any tree carrying `gates` is a guaranteed red. Declared as D-7; repeated
here because it is an ordering constraint the integrator must honour, not a matter of taste.

**m-7 — G3, G4, G6, G10, G11 are unrun and D.2's artefact unproduced, so E's merge closes none of them.**
gates-v3 §8 declares all of these. They are correctly out of E's reach (E builds no library source,
G11 needs two devices, G6/G10 are A's and B's tests). Recorded so the integrator does not read
"package E approved" as "the acceptance gate is met": after E lands, G3/G4/G6/G9/G10/G11 and the D.2
log are still entirely outstanding.

**m-8 — `test.yml` was never machine-parsed.**
gates-v3 §8 says so; I could not close it either (no `pyyaml`, no `pip`, no `yq` in this WSL). The
three new steps' indentation and `env:` blocks match their siblings by inspection and the only new
block scalars are plain `run:` lines, so the risk is low — but the file is E's and a YAML error in it
takes the whole CI matrix down. Integrator should parse it once before the first `gh workflow run`.

---

## 4. Hunts that came back clean

Recorded because their absence is part of the verdict.

* **A gate that is green because it never ran.** Every new lane fails closed: both new CI steps carry
  `--no-tests=error`; `g7_negative_control.sh:93-103` refuses (rc 2) rather than reading "no test
  matched" as a trip; `CsoContentAddressingScenario.cpp:279-295` asserts the plumbing (`window.found`)
  and the denominator (`binds >= 16`) **before** either ratio, so a lane whose stats channel never
  opened reds instead of passing; `HandleRecycleScenario.cpp:257-263` `FAIL()`s on an unrecognised
  `MGITEST_HANDLE_ARM` rather than falling through to the Legacy assertion. The one structural hole
  is m-2's source probe, and it is mitigated by a printed verdict.
* **A test that cannot fail.** `TheReproducerRecyclesEveryName` (`:413-448`) asserts the name recycle
  with `EXPECT_EQ`, not a skip, in every lane including the two ambient ones — 7 entries that run for
  real here. The three ABA cases skip *before* asserting when the names are not recycled
  (`:494-498`, `:545-548`, `:606-612`), which is the "inconclusive, not proven" shape, and the
  `AbaControl` arm asserts the corruption so a reproducer that stops reproducing goes red
  (`ExpectPixelsFor`, `:239-250`). The consequence — that arm can red an always-on `integration-gpu`
  lane for an allocator reason — is now written in two places (`:43-54` and at the skip), which was
  v2's minor 9 and is properly closed.
* **Instrumentation on the hot path.** Nothing was added to `libMobileGL.so`: G1 is 0 added /
  0 removed and the four resized symbols are the contract's. The only new clock is one
  `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` per *frame* in `android-plugin/app/src/trace/cpp/trace_benchmark.cpp:38-48,113-120`,
  which is what D.4.1:829-832 asks for, outside the library, and read *before* the wall clock so the
  syscall's cost lands in the visible number.
* **A memo or suppressor serving a stale answer / a dirty bit never cleared / a handle re-key.**
  Not applicable to package E: it touches no library source (`git diff --stat … -- MG_Pipe/ MG_State/
  MG_Backend/ MG_Impl/ MG_Util/` is empty). The chunk-table partition, the subset hash and the setter
  derivation are package A's and were not re-derived here beyond the five-setter check the G7 patch
  implies (`ColorMasks` 32 B moving 396→364 / 772→804 is arithmetically consistent with the header's
  `offsetof`-computed table).
* **Ownership and fixtures.** 18 files, all E's per C.5; 0 fixture files touched; the two
  *uncommitted* experiments (D-10's `git apply` of package A's diff, D-13's throwaway probes) are
  declared and the tree is clean.
* **Deviations from D1–D20.** D17 (no tracker timer) honoured; D18's three arms exist and are
  always-on registrations (m-1 is about one of them having nothing to assert *yet*, not about it
  being absent); D19's control matches the specified break; D20's stdio rule holds — the new
  `fprintf(stderr, …)` in `MG_Benchmark/Driver/DriverBench.c:477-497` is outside the `pipe-gates`
  grep's two directories and outside the shipped library. The declared deviations D-2…D-13 are
  each defensible and each recorded.

---

## 5. What the integrator must carry forward

1. Fix M-1 before merge (`--chmod=+x` on both scripts) and re-check no other mode changed.
2. Fix §A's G14 command and the baseline recipe (C-1): `sed -n 's/^ *Test *#[0-9]*: //p'`, never
   `grep -E '^\s+Test #'`.
3. G1 as written in §A is already violated by the contract commit (C-2, four resized symbols, the
   fourth `_GLOBAL__sub_I_DirectGLES.cpp` −9 B). Either widen G1's admitted set to four with the
   attribution, or bounce it to package A. It is not E's.
4. Land `tracker` before `gates` (m-6), and take `~/w7/p2-handlerecycle-before.log` on the contract
   tree after D lands the knob and before D enables the re-key (m-1).
5. Tell packages C and D the identifier contract in m-2.
6. Restore `fan_mode` on `35d0befa` (m-5).
