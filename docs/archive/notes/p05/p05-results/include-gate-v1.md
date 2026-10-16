# P0.5 package `p05-include-gate` — result (v1)

Tree `~/w7/p05-include-gate`, branch `p05/include-gate`, based on `feat/disaggregated @ 6672778b`.
Nothing pushed. Neither P0.5 header exists in this tree, so probes A and B are SKIP by design (D9).

## 1. Commits

| sha | subject |
|---|---|
| `fe3dc1dd` | `[CI] (Purity): add scripts/check_include_closure.py - the -H include-closure gate for the P0.5 headers with an always-on negative control, wired as a unit ctest and the include-graph-check job` |
| `2318f6ae` | `[Tooling] (Purity): add scripts/symbol_report.py - per-symbol nm/.text attribution between two libMobileGL.so builds` |

`git diff --stat 6672778b HEAD`:

```
 .github/workflows/test.yml             |  26 ++
 MobileGL/MG_Test/CMakeLists.txt        |   3 +
 MobileGL/MG_Test/Purity/CMakeLists.txt |  15 +
 scripts/check_include_closure.py       | 659 +++++++++++++++++++++++++++++++++
 scripts/symbol_report.py               | 386 +++++++++++++++++++
 5 files changed, 1089 insertions(+)
```

Exactly the five files the B.4 ownership table assigns to this package. Working tree clean apart
from the untracked `build-linux/`. All new/modified files are LF (`grep -lU $'\r'` empty).

Absolute paths:
- `//wsl.localhost/Arch/home/swung/w7/p05-include-gate/scripts/check_include_closure.py`
- `//wsl.localhost/Arch/home/swung/w7/p05-include-gate/scripts/symbol_report.py`
- `//wsl.localhost/Arch/home/swung/w7/p05-include-gate/MobileGL/MG_Test/Purity/CMakeLists.txt`
- `//wsl.localhost/Arch/home/swung/w7/p05-include-gate/MobileGL/MG_Test/CMakeLists.txt`
- `//wsl.localhost/Arch/home/swung/w7/p05-include-gate/.github/workflows/test.yml`

## 2. Verification — every command and its actual output

Environment: WSL Arch, `python3` 3.14.4, `/usr/sbin/clang++` = clang 22.1.6, `/usr/sbin/g++` = GCC
16.1.1. `clang++-20` does not exist in WSL, so the local runs used clang 22 and g++ 16; the CI job
still pins `clang++-20` as the brief specifies.

### 2.1 `python3 scripts/check_include_closure.py --mode both --self-test` → exit 0

```
include-closure: compiler: /usr/sbin/clang++
include-closure: value-header SKIP  header not present yet: MobileGL/MG_Pipe/MGPipeValueTypes.h
include-closure: artifacts-header SKIP  header not present yet: MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h
include-closure: wire-header OK 2 headers in closure, 0 forbidden (mode=text, cxx=-)
include-closure: wire-header OK 42 headers in closure, 0 forbidden (mode=clang, cxx=/usr/sbin/clang++)
include-closure: wire-header SELF-CONTAINED OK (-fsyntax-only, cxx=/usr/sbin/clang++)
include-closure: self-test: negative controls (must trip) in mode(s) text, clang
include-closure:     self-test parser: trailer discarded, `../` normalised and matched
include-closure:     self-test control-1 (independent of P0.5) [text]: tripped, 9 violation(s), MobileGL/MG_State/GLState/RenderState/RenderState.h at depth 1
include-closure:     self-test control-1 (independent of P0.5) [clang]: tripped, 9 violation(s), MobileGL/MG_State/GLState/RenderState/RenderState.h at depth 1
include-closure: self-test: 2 negative-control trip(s), parser checks OK
include-closure: 3 probes, 2 skipped, 0 problem(s)
```

This is the required shape: **A SKIP, B SKIP, C OK, self-test OK, exit 0**. The two modes agreed on
the wire-header violation set (empty), so the `--mode both` disagreement check stayed silent.

### 2.2 `--mode text --self-test` (what the ctest runs) → exit 0

Same three probe lines, `self-test: 1 negative-control trip(s), parser checks OK`,
`3 probes, 2 skipped, 0 problem(s)`. No compiler and no submodules touched.

### 2.3 `--mode both --self-test --require-all` → **exit 1**, names both missing headers

```
include-closure: value-header SKIP  header not present yet: MobileGL/MG_Pipe/MGPipeValueTypes.h
include-closure: value-header SKIP is a failure under --require-all: MobileGL/MG_Pipe/MGPipeValueTypes.h does not exist
include-closure: artifacts-header SKIP  header not present yet: MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h
include-closure: artifacts-header SKIP is a failure under --require-all: MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h does not exist
include-closure: wire-header OK 2 headers in closure, 0 forbidden (mode=text, cxx=-)
include-closure: wire-header OK 42 headers in closure, 0 forbidden (mode=clang, cxx=/usr/sbin/clang++)
include-closure: wire-header SELF-CONTAINED OK (-fsyntax-only, cxx=/usr/sbin/clang++)
...
include-closure: 3 probes, 2 skipped, 2 problem(s)
::error::include-closure gate failed: 2 problem(s); see the chains above
exit=1
```

### 2.4 `--mode both --compiler g++ --self-test` → exit 0 (trailer-line handling)

```
include-closure: compiler: /usr/sbin/g++
include-closure: wire-header OK 33 headers in closure, 0 forbidden (mode=clang, cxx=/usr/sbin/g++)
include-closure: wire-header SELF-CONTAINED OK (-fsyntax-only, cxx=/usr/sbin/g++)
include-closure: self-test: 2 negative-control trip(s), parser checks OK
include-closure: 3 probes, 2 skipped, 0 problem(s)
```

g++ appends a `Multiple include guards may be useful for:` paragraph of bare `/usr/include/...`
paths to its `-H` output (confirmed by hand: 702 stderr lines, of which the trailer is ~40). The
`^(\.+) (.*)$` filter discards it, and `--self-test`'s canned-transcript check pins that behaviour
independently of which compiler is installed.

### 2.5 Red-for-its-reason (the manual negative control on the one live probe)

Temporarily added `#include <MG_State/GLState/RenderState/RenderState.h>` to
`MobileGL/MG_Remote/Transport/ITransport.h`, then `python3 scripts/check_include_closure.py
--mode both --probe wire-header --self-test`:

```
include-closure: wire-header FORBIDDEN 76 headers in closure, 1 forbidden (mode=text, cxx=-)
include-closure:     forbidden: MobileGL/Includes.h (rule MobileGL/Includes.h) reached by:
include-closure:     MobileGL/MG_Remote/Transport/ITransport.h
include-closure:       MobileGL/MG_State/GLState/RenderState/RenderState.h
include-closure:         MobileGL/Includes.h
include-closure: wire-header FORBIDDEN 672 headers in closure, 1 forbidden (mode=clang, cxx=/usr/sbin/clang++)
include-closure:     forbidden: MobileGL/Includes.h (rule MobileGL/Includes.h) reached by:
include-closure:     MobileGL/MG_Remote/Transport/ITransport.h
include-closure:       MobileGL/MG_State/GLState/RenderState/RenderState.h
include-closure:         MobileGL/Includes.h
include-closure: 1 probes, 0 skipped, 1 problem(s)
::error::include-closure gate failed: 1 problem(s); see the chains above
exit=1
```

Both modes went red with an identical three-hop chain; the closure grew 2 → 76 (text) and 42 → 672
(clang). Reverted with `git checkout --`; `git status --porcelain` for that path is empty.

### 2.6 Other flag paths exercised

- `--probe wire-header --json /tmp/p05g.json`: exit 0, JSON carries `modes`, per-mode `results`
  (`headers` 2 / 42, `forbidden` 0), `skipped`, `problems`, `self_test`, `require_all`.
- `--probe nope`: `::error::unknown probe(s): nope`, exit 1.
- `--compile-commands build-linux/compile_commands.json --mode clang --probe wire-header`:
  `flags from build-linux/compile_commands.json (27 tokens), cwd=/home/swung/w7/p05-include-gate/build-linux`,
  then `wire-header OK 42 headers in closure, 0 forbidden` — the same closure as the default flags,
  which is the point of having measured the defaults.
- Manifest assertion: `REQUIRED_PROBE_NAMES = ("value-header", "artifacts-header")` is checked
  before anything else runs, so deleting a probe is red.

### 2.7 ctest wiring

```
$ cmake -S . -B build-linux                  # exit 0
$ ctest --test-dir build-linux -R MobileGLPurity --output-on-failure
    Start 1064: MobileGLPurity.IncludeClosure
1/1 Test #1064: MobileGLPurity.IncludeClosure ....   Passed    0.08 sec
100% tests passed, 0 tests failed out of 1
Label Time Summary:
unit    =   0.08 sec*proc (1 test)

$ ctest --test-dir build-linux -L unit -N | grep -c MobileGLPurity
1
```

Full unit suite (`ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure -j 8`):

```
100% tests passed, 0 tests failed out of 1459
Label Time Summary: unit = 106.08 sec*proc (1459 tests)
Total Test time (real) = 13.42 sec
```

Test-name diff against the read-only reference tree `~/w7/pipe`:

```
comm -23 before after   ->  (empty; no test name lost)
comm -13 before after   ->  MobileGLPurity.IncludeClosure
1327 before / 1328 after
```

### 2.8 `.github/workflows/test.yml`

`python -c "import yaml,sys; yaml.safe_load(open(sys.argv[1]))"` was run with the **Windows**
interpreter against a copy of `//wsl.localhost/Arch/home/swung/w7/p05-include-gate/.github/workflows/test.yml`
(WSL's python has no `yaml`):

```
parsed OK; jobs: 13
order around flatc-check: ['integration', 'flatc-check', 'include-graph-check', 'benchmark']
include-graph-check present: True
name: Include-closure purity gate
runs-on: ubuntu-latest
steps:
- name: Checkout repo
  uses: actions/checkout@v6
- name: Check out the three header submodules the closure needs
  run: git submodule update --init include/ska 3rdparty/xxHash 3rdparty/Vulkan-Headers
- name: Install clang
  run: sudo apt-get update && sudo apt-get install -y clang-20
- name: Include-closure assertions and negative control
  run: python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test
```

Inserted immediately after `flatc-check` and before `benchmark`, as B.3 requires. Triggers
untouched (D10).

### 2.9 `scripts/symbol_report.py`

`python3 scripts/symbol_report.py --self-test` → `symbol-report: self-test: OK (2 canned
transcripts, 5 buckets)`, exit 0. The canned pair pins removed / added / resized / renamed-only /
unchanged, including that a de-nested member folds to *renamed*, not to added+removed.

Two real builds — `~/w7/pipe/build-linux/libMobileGL.so` (read-only reference) vs this tree's
`build-linux/libMobileGL.so`, both Release / `/usr/sbin/clang++` / LTO off:

```
symbol-report: before: /home/swung/w7/pipe/build-linux/libMobileGL.so (19101056 bytes on disk)
symbol-report: after : build-linux/libMobileGL.so (19101056 bytes on disk)
symbol-report: .text 10792579 -> 10792579 (+0, +0.000%)
symbol-report: .data 76824 -> 76824 (+0)
symbol-report: .bss 1296872 -> 1296872 (+0)
symbol-report: .rodata 1534938 -> 1534938 (+0)
symbol-report: Total 17209819 -> 17209819 (+0)
symbol-report: 27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
symbol-report: 27060 -> 27060 normalised names, 27060 unchanged (name, size and mangling all identical)
```

**0 added / 0 removed / 0 resized / 0 renamed** on an unchanged tree, as expected (this package
adds no C++). `--markdown` output was also written and checked.

Synthetic `--strip-scope` rename case (two purpose-built .so files, `N::Outer::Inner::f` de-nested
to `N::Inner::f`, plus an untouched `N::Stable`):

```
without --strip-scope : 17 -> 17 defined symbols: 1 added, 1 removed, 0 resized, 0 renamed
with    --strip-scope 'N::Outer::' :
                        17 -> 17 defined symbols: 0 added, 0 removed, 0 resized, 1 renamed, 16 unchanged

### Renamed only (same size) (1)
| normalised symbol | before mangling | after mangling |
| N::Inner::f(int)  | _ZN1N5Outer5Inner1fEi | _ZN1N5Inner1fEi |
```

That is exactly the shape the `p05-program-artifacts` package's report is supposed to have.

## 3. Deviations from the brief, with reasons

1. **`--strip-scope` semantics.** The brief describes it as a textual substitution that "folds
   `ProgramObject::LinkArtifacts` → `LinkArtifacts`". A literal *delete the whole prefix* would
   break the fold: the before side becomes `LinkArtifacts::Reset()` while the after side is
   `MobileGL::MG_State::GLState::LinkArtifacts::Reset()`, so the two would land in the
   removed/added buckets — the opposite of the intent. Implemented as a **de-nesting**:
   `--strip-scope 'A::B::C::'` rewrites every `A::B::C::X` to `A::B::X` (it deletes only the final
   class component), which produces the stated fold and works inside template arguments too. The
   behaviour is documented in `--help` and pinned by `--self-test`; C.4's command line is unchanged.
2. **One SKIP line per probe, not one per mode.** The brief's summary-line template carries a
   `mode=` field, but whether a header exists is mode-independent; printing it twice under
   `--mode both` was noise and double-counted the skip. A SKIP is now emitted once, before either
   mode runs, and counted once. `--require-all` adds its own explicit line naming the header.
3. **The mode-disagreement check is applied to probes only, not to the self-test controls.** For a
   probe, a text/clang divergence is a real signal and fails the run (implemented, prints both
   sets). For the negative controls, whose TUs deliberately drag in the whole `MG_State` tree, text
   mode's blindness to `#if` can legitimately produce a different violation *set* from clang's; the
   self-test therefore requires each control to trip **in every enabled mode** with the expected
   depth-1 path, which is the property that matters, rather than comparing sets.
4. **Extra output line in `symbol_report.py`.** `27799 -> 27799 defined symbols` and
   `27060 unchanged` are different denominators (several mangled symbols fold to one normalised
   demangled name). Added an explicit `N -> M normalised names` line so the two numbers cannot be
   misread as 739 lost symbols.
5. **Local compiler.** `clang++-20` does not exist in WSL, so the local `--mode both` runs used
   clang 22 (default resolution order) and g++ 16 (explicit `--compiler g++`). The CI job pins
   `clang++-20` exactly as the brief specifies; the default compiler search order in the script is
   `$CXX, clang++-20, clang++, c++, g++` as written.
6. **Cosmetic YAML.** The CI job got a blank line between `runs-on:` and `steps:` and a longer
   comment block than the brief's snippet, to match the voice and spacing of the neighbouring
   `flatc-check` / `benchmark` jobs. Steps and commands are verbatim from the brief.

No other deviation. `--fail-on-added-bytes` is accepted and documented as reserved (spec says
"reserved for a future hard gate"), `Allow` entries require a printed `Reason`, and the
`--json` / `--markdown` / `--only-names` / `--threshold` / `--rename-map` flags are all implemented.

## 4. Unfinished / handed to the integrator

- **The `--require-all` ratchet (C.3) is deliberately not flipped.** Both call sites
  (`MobileGL/MG_Test/Purity/CMakeLists.txt` `add_test` COMMAND and the `include-graph-check` step
  in `test.yml`) are one appended flag away; verified today that the flag produces exit 1 with a
  message naming both headers.
- **Probes A and B have never run against a real header** — they cannot, in this tree. Their
  forbidden lists, the `glslang::` ≤ 2 text limit, self-test controls 2 and 3 and the
  "third `glslang::` token" text-limit control are all coded and arm themselves automatically the
  moment `MGPipeValueTypes.h` / `ProgramArtifacts.h` appear. The integrator should see A flip to OK
  after merging `p05/value-types` and B after `p05/program-artifacts` (C.1), and should treat an
  all-SKIP run at that point as a failure.
- **CI has not run** (D10): `feat/disaggregated` is not a push trigger, so the new job needs
  `gh workflow run test.yml --ref feat/disaggregated` from the integrator, who records the URL.
- **The `gl44to46` caselist run** owed by `ROADMAP.md:34` is device time and out of this package.
- Nothing was pushed; both commits are local on `p05/include-gate`.
