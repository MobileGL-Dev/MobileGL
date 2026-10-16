# Contributing to MobileGL

These rules keep the code and its history readable. `scripts/check_text_hygiene.py` checks the ones listed under [Checks](#checks); the rest rely on review.

Commands below use `python3`. On Windows, run `python` instead.

## Commit messages

```
[Type] (Scope): content

- Detail 1
- Detail 2
```

- **Subject only by default.** Add `- ` detail lines only when a detail really needs to be in the commit message. Leave one blank line after the subject. A detail that wraps continues on at most two lines indented by two spaces. No paragraphs and no trailers.
- **Content** after `): ` is at most 72 characters of plain English in the imperative mood ("flush once per frame", not "flushes" or "flushed").
- **Type** is one or more of these, comma-separated (`[Fix, Test]`):
  - `Feat`: new behaviour.
  - `Fix`: corrects behaviour.
  - `Perf`: faster or lighter, same behaviour.
  - `Refactor`: restructures code, same behaviour.
  - `Test`: tests only.
  - `Docs`: documentation and comments only.
  - `Build`: CMake, Gradle, toolchains.
  - `CI`: workflows and CI scripts.
  - `Chore`: housekeeping such as headers, file moves and dead files.
  - `Tools`: developer tools under `tools/` and `scripts/`.
- **Scope** is each changed path without a leading `MobileGL/`, cut to its first two parts: `MG_Backend/DirectGLES`, `MG_Test/Wire`, `scripts/ci`, `docs/Disaggregated`.
  - A shorter path is used whole: `MG_Backend` for a file directly in a subsystem, `scripts` for a file directly in `scripts/`, and the file name for a file at the repository root (`CMakeLists.txt`).
  - A file directly in `MobileGL/` uses `MobileGL`, and a submodule uses its path (`3rdparty/glslang`).
  - List every scope the commit touches, comma-separated: `(MG_Backend/DirectGLES, MG_Test/Wire)`.
  - `python3 scripts/check_text_hygiene.py scope` prints the scope for the staged changes.
- **No** planning ids (phase or decision codes such as `P13` or `ID-54`), local paths, external project names or non-English text anywhere in the message.
- **Merges** use `[Merge] (area): what the merge takes`. The scope is free text there. Write the subject yourself, since git's default merge message fails the check. The commits a merge brings in are not checked again.
- **Reverts** use `[Fix] (Scope): revert <the reverted subject>`.

Example:

```
[Perf] (MG_Backend/DirectGLES): flush once per frame before the window pass
```

## Comments, strings and documentation

- Code, comments, log strings and commit messages are in English. Documentation may be in Chinese or English.
- A comment states what the code does now and why, in a few lines. Rationale longer than about 15 lines belongs in a design document under `docs/`, with a one-line pointer in the code.
- In code and comments, cite a symbol, file or section, never a line number: `ARCHITECTURE.md §1.3`, `VulkanRenderer::BeginFrame()`. Documents may keep `file:line` citations; the documentation citation check resolves them.
- State the rule, not its history. No "previously" or "after review", and no planning ids in comments, strings, identifiers or file names.
- Plain wording, without ALL-CAPS emphasis.
- In code and comments (C, C++, GLSL, CMake, shell, Python, YAML), do not name external projects such as Mesa, Zink or gallium; describe the technique instead. Naming the driver or hardware where a behaviour was observed (llvmpipe, ANGLE, Adreno, Mali) is fine, and so are file, package and extension names that contain these words (`50_mesa.json`, `GL_MESA_*`).

## Code

- Format the lines you change with `.clang-format`, using clang-format 23. Install it with `pip install 'clang-format==23.1.3'`; `git clang-format <base>` from the same package formats only the lines changed since `<base>`. `scripts/format_code.sh` reformats whole files, so do not use it on files you only touched.
- `.inc` files are fragments included in the middle of a `.cpp` file, inside a namespace, or are generated tables. clang-format cannot format them on their own, so the check skips them. Match the indentation of the file that includes them.
- Text files are UTF-8.
- These source files under `MobileGL/`, `scripts/` and `tools/` start with the license header:
  - C, C++, `.inc`, GLSL, Java and JavaScript modules, with `//` comments;
  - Python, shell, CMake modules and `CMakeLists.txt`, with `#` comments.
  - Generated files and files under another license are exempt. `python3 scripts/update-source-file-header.py` adds the header.
- Keep files to about 3,000 lines and functions to about 300 lines.
- Modules are layered. A file includes files of its own layer and of the layers below it, never above:
  1. the root headers (`Includes.h`, `Config.h`), `MG_Util` and `MG_State`;
  2. `MG_Pipe`;
  3. `MG_Backend`, `MG_Remote` and `MG_Impl`;
  4. the root `.cpp` files and `MG_Gbm`;
  5. `MG_Test`, `MG_IntegrationTest` and `MG_Benchmark`.
- Each backend is isolated. Files under `MG_Backend/DirectGLES/` or `MG_Backend/DirectVulkan/` are included only by the same backend, by the registry files directly in `MG_Backend/`, by the root `.cpp` files and by tests.
- Platform-specific APIs stay behind an abstraction, never raw in logic code.
- A fix that restores intended behaviour ships unconditionally. Add a switch only for a real trade-off.
- A cache keyed on object identity uses a monotonic lifetime id, never a GL name or a pointer, because both are recycled.

## What goes in the repository

- Source, tests, build files and maintained documentation.
- Generated code, only together with a CI step that regenerates it and fails on any difference.
- Binary test fixtures.
- Not raw logs, and not working papers such as agent briefs, review rounds and drafts. Keep those outside the repository and commit the conclusion: a design-document change, a `DEBTS.md` entry, a measurement under `docs/Performance`.
- `docs/archive/` keeps the planning notes and agent contracts of the disaggregation work as they were. It is frozen: no file there is added, edited or removed.

## Checks

`python3 scripts/check_text_hygiene.py ci` runs what CI runs. It checks the commits on the branch's own line of history since the check was added. The base branch it compares with is `feat/disaggregated`, which you can change with `TEXT_HYGIENE_BASE_BRANCH`.

These fail on any violation:

- commit messages, as described above;
- a new file under `docs/Disaggregated/notes/` or a new `.log` file (moving a file is not adding it);
- any change under `docs/archive/`;
- a missing or wrong license header;
- a text file that is not UTF-8;
- an external project name in a comment;
- a `file:line` citation in code that points past the end of the file;
- changed lines that clang-format would change (a line moved unchanged, the same commit removing its exact text elsewhere, is not a changed line).

These are ratchets: each total may only go down.

- planning ids in comments;
- comment lines beyond 15 in a block;
- ALL-CAPS comment lines;
- non-English comment lines;
- line-number citations in code.

`python3 scripts/check_text_hygiene.py tree --update-baseline` lowers `scripts/data/text_hygiene_baseline.json` after a cleanup. It never raises it, and CI rejects a raised baseline.

`python3 scripts/check_structure.py ci` checks three more ratchets over the C and C++ sources under `MobileGL/`, against `scripts/data/structure_baseline.json`:

- includes that break the layering or the backend isolation above;
- lines beyond 3,000 in a file;
- lines beyond 300 in a function body.

`python3 scripts/check_structure.py report` lists what each total counts, and `tree --update-baseline` lowers the baseline the same way.

Not checked by CI: the platform, switch and lifetime-id rules, planning ids outside comments, English outside comments, and the regeneration step for generated code.
