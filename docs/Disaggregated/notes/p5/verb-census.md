# verb-census.md — which `GlobalBackendFunctionsTable` slots the reduced path actually reaches

**Status: STATIC, but not an estimate for two of the three targets.** R-4 asks for a census run
against an all-`Fatal` emit table; that table does not exist until package c1 lands, so this was
derived by reading the dispatch chain from every GL entry point down to
`gBackendFunctionsTable.GL.*`. For **OpenRA the call set is measured, not guessed** — the
fixture turned out to be hydrated locally and was dumped with the local `apitrace`. **c1 must
still confirm the whole thing by running the real all-Fatal table**, because a static read
cannot see a slot reached from a branch it did not know to follow.

Base `feat/disaggregated @ a29807cc`, read in `~/w7/p5-c0`. No GitHub fetch, no LFS.

---

## The answer

**Six slots, out of 71.** Five `GLFunctionsTable` entries plus `Present`:

| slot | A: ClearThenReadPixels | B: Triangle | D: OpenRA | reached by |
|---|---|---|---|---|
| `Clear` | ✔ (all 5 cases) | ✔ | ✔ (128 calls) | `glClear` |
| `DrawArrays` | ✔ (4 of 5) | ✔ | ✔ (3,648 calls) | `glDrawArrays` |
| `ReadPixels` | ✔ (all 5) | ✔ | ✔ (harness, not the trace) | `glReadPixels` |
| `BlitFramebuffer` | ✔ (cases 3, 4) | — | — | `glBlitFramebuffer` |
| `GetIntegeri_v` | ✔ (4 of 5) | ✔ | ✔ (once) | **the first `glCompileShader` of a context**, not any verb |
| `Present` | ✔ (all 5) | **only if it calls `EndFrame()`** | ✔ (128) | `eglSwapBuffers`, **through the EGL path, not through GLImpl** |

The other **65 slots** take `Fatal{UnmigratedVerb, "<slot>"}`.

This is **at the bottom of R-4's predicted 6–15**, and it is smaller than the brief's guessed
minimum set for reasons worth knowing rather than celebrating — see the four traps below.

---

## Four things this census found that the guess would have got wrong

**1. `Flush` and `Finish` are not slots and reach nothing.** R-4's predicted minimum set names
`Flush`. There is no `Flush` entry in `GLFunctionsTable` at all, and
`MG_Impl/GLImpl/Exporting/Definitions.cpp:111-112` makes both `glFinish()` and `glFlush()`
**literally empty bodies** — one `MGLOG_D` and a return. A `TriangleScenario` that calls
`glFlush` to order its readback is ordering nothing. If it needs a real sync it must use
`glFenceSync` + `glClientWaitSync`, which would pull in three more slots (`FenceSync`,
`ClientWaitSync`, `DeleteSync`); the readback itself is the cheaper ordering point.

> **RULING R-15 (integrator, after this census): a getter-shaped slot — one whose answer is a
> static property of the server's device — is ANSWERED LOCALLY FROM THE CAPS MIRROR, never
> emitted and never `Fatal`.** That puts `GetIntegeri_v` and `IsTimerQuerySupported` in class A
> below, leaves five slots emitted and 64 `Fatal`, and the gate already exists
> (`AdvertisedLimitsScenario.ComputeWorkGroupLimitsAreTheCapsBlocksAnswer`). The full three-class
> split, and the 41 null-checks that are really capability probes, are in `CONTRACT-P5.md §7` —
> **c1 reads it there rather than re-deriving it.**

**2. `GetIntegeri_v` is reached by compiling a shader, not by drawing.** It has no GL-verb
caller on this path at all: `MG_Util/ShaderTranspiler/CompileEnv.cpp:134-138`
(`CaptureCompileEnv`) reads it, reached lazily from `MG_State/GLState/Core.cpp:39`
(`GLContext::GetCompileEnv()`) on the **first shader compile of a context**. So *every* target
that compiles a shader touches it, whatever its entry-point set looks like — and an emit table
that raises `Fatal{UnmigratedVerb}` on it aborts on the first `glCompileShader` of every
scenario. It must have a real emitter (or be answered from the caps mirror, which is the better
answer: the six compute limits it carries already ride in `MGPCaps::Dynamic`, see CONTRACT §1).

**3. `Present` and `SetSwapInterval` have ZERO call sites in `MG_Impl`.** Both are called only
from `MG_Backend/BackendObject.cpp:396` and `:403`, reached through
`MG_Impl/EGLImpl/EGLImpl.cpp:178` / `:448`. An emit table built by mirroring the 89 GLImpl call
sites covers neither, and `Present` is on the reduced path in two of the three targets. The seam
for those two is the EGL path — which is the same seam v1 is already rerouting to the apply
thread, so they belong together.

**4. `ReadPixels` has a second, invisible caller.** Besides `glReadPixels`
(`GL_Framebuffer.cpp:3095`), the frontend fallback behind `glCopyTexSubImage*` calls it —
`CopyReadFramebufferIntoMipmapRegion`, `GL_Texture.cpp:1048`, calling at `:1084`. Not on the
reduced path, but it is the class of thing that makes "this scenario has no readback" false.
Two siblings: `GenerateMipmap` can fire from a `glTexImage*` when `GL_GENERATE_MIPMAP` is set
(`GL_Texture.cpp:1679` → `:1688`), and `DrawArrays` is reached by `glDrawTransformFeedback*`
(`GL_Drawing.cpp:1710`, `:1712`) with no `glDrawArrays` in sight.

---

## Per target

### A — `DirectGLES.Split.ClearThenReadPixelsScenario` (exists, 5 cases)

`MG_IntegrationTest/Scenarios/ClearThenReadPixelsScenario.cpp`, 335 lines.

| case (line) | slots |
|---|---|
| `ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels` (80) | Clear, DrawArrays, ReadPixels, GetIntegeri_v, Present |
| `ClearWithNoDrawIsVisibleToASubRectReadback` (143) | same |
| `ABlitIntoTheDefaultFramebufferSurvivesAnEarlierClear` (191) | + **BlitFramebuffer** |
| `AMultisampleResolveBlitIntoTheDefaultFramebufferKeepsItsOrientation` (241) | Clear, **BlitFramebuffer**, ReadPixels, Present — **no shader and no draw**, so it is the one case that does *not* need `DrawArrays` or `GetIntegeri_v` |
| `ADrawIntoTheDefaultFramebufferSurvivesAnEarlierClear` (308) | Clear, DrawArrays, ReadPixels, GetIntegeri_v, Present |

`glBlitFramebuffer` reaches **`BlitFramebuffer`**, *not* `BlitNamedFramebuffer` — the DSA slot is
reachable only from `glBlitNamedFramebuffer` (`GL_Framebuffer.cpp:3413` → `:2708`). Easy to
conflate; they are separate slots and only one is on this path.

### B — `DirectGLES.Split.TriangleScenario` (does not exist; t1 writes it)

Spec: VBO create + upload, compile + link, VAO, `glClear`, `glDrawArrays` (VBO-backed, **no
client-array indices**), `glFlush`, `glReadPixels`.

Slots: **`Clear`, `DrawArrays`, `ReadPixels`, `GetIntegeri_v`** — and `Present` **only if the
scenario ends with `HeadlessGL::EndFrame()`**. The spec as written does not, which would make B
a strict subset of A minus `Present`. **Decide that deliberately**: a reduced path whose
"minimal" case exercises fewer slots than its predecessor is fine, but it should be a choice.

Everything else the scenario calls reaches no verb slot: the buffer, VAO, program and texture
traffic is the `MG_Impl/Pipe` → `MGPipeApply*` family (37 entry points), which `EmitTables.h`
says this table deliberately does not cover, and the rest is answered in the frontend.
`glGetProgramiv` is deliberately not a slot (`BackendObject.h:206-212`).

### D — OpenRA trace, SSIM ≥ 0.99 — **measured**

`tools/trace_replay/trace_cases.json:14-28`: `openra.trace`, target call 31,249, 640×480, crop
(1,1,638,478), threshold 0.99, 180 s.

**The fixture is hydrated in this tree and is the only one that is** — `openra.tgz` is 7,232,355
bytes with gzip magic, `openra.0000031249.png` is 403,752 bytes. Every other `.tgz` there is a
130–134-byte LFS pointer. Nothing was fetched.

`apitrace dump` over the whole trace: **70,465 calls, 51 distinct entry points** (46 GL + 5
GLX). Up to call 31,249 the *set* is identical and the counts scale about 4:1.

The only entry points that reach a verb slot:

| entry point | calls (whole trace) | slot |
|---|---|---|
| `glDrawArrays` | 3,648 | `DrawArrays` |
| `glClear` | 128 | `Clear` |
| `glXSwapBuffers` → the harness's `eglSwapBuffers` (`tools/trace_replay/apitrace_glws_egl.cpp:317`) | 128 | `Present` |
| first `glCompileShader` | 14 total, 1 capture | `GetIntegeri_v` |

Plus `ReadPixels`, contributed by the **snapshot harness** rather than by OpenRA:
`tools/trace_replay/apitrace_fbo_dump.cpp:383`. (That file also has a `glGetTexImage` arm at
`:337`/`:349` for texture-attachment snapshots, which would reach `GetTexImage`; the
default-framebuffer path takes the `glReadPixels` arm.)

**OpenRA is the thinnest of the three targets, not the widest.** Its bulk — 20,055
`glVertexAttribPointer`, 7,296 `glBlendEquation`, 6,685 `glBindBuffer`, 4,866 `glUseProgram`,
4,828 `glBindTexture`, 3,030 `glBufferSubData`, all program and texture traffic — is entirely
the `MGPipeApply*` family. It widens the **site** set and the **record** set enormously and the
**verb** set not at all, which is exactly what `scout-unmigrated-census:§3` predicted for the
field set and is here confirmed for the slot set. The framebuffer work (calls 1348–1382) never
blits or clears an attachment through the table.

---

## Blast radius of the 65 Fatal slots

Of the 69 GL slots, **28 are called with no null check at all** and 41 are guarded.

- For the **28 unguarded** — the 18 draw entries, the five `ClearBuffer*`/`Clear`,
  `BlitFramebuffer`, `CopyTexImage2D`, `CopyTexSubImage2D`, `GenerateMipmap`, `ReadPixels` — a
  `Fatal` slot is **strictly better than today**: a null there is already an immediate crash
  with no diagnostic.
- For the **41 guarded**, a `Fatal` slot is a real behaviour change, from "degrade quietly" to
  "abort". The families that degrade today are sync (`FenceSync` → always-signaled),
  query (`IsQueryResultAvailable` → reported available), `GetTexImage`/`GetTextureImage` (CPU
  fallback), and transform feedback (silently skipped). **That change is R-4's intent** — a
  silent degrade is how a split lane produces the right picture for the wrong reason — but it is
  worth naming, because those are also the families most likely to be hit by an integration test
  that is not on the reduced path.

## Counting notes

- **89** direct `gBackendFunctionsTable.GL.*` call sites in `MG_Impl/`, not the 91 the scout
  reported; **0** of them are in `MG_Impl/Pipe/` (the two hits there are comments about the
  `MGP_FILL` convention). Distribution: `GL_Drawing.cpp` 36, `GL_Query.cpp` 22,
  `GL_Framebuffer.cpp` 11, `GL_Texture.cpp` 10, `GL_Sync.cpp` 6, `GL_Getter.cpp` 3,
  `GL_Program.cpp` 1.
- **No** GL slot has zero `MG_Impl` call sites. The zero-call-site result belongs to `Present`
  and `SetSwapInterval` (trap 3) and to `GetIntegeri_v`'s out-of-tree caller (trap 2).
- `PrefersCpuXfbPrimitiveAccounting` is not a verb: one non-test reader, `GL_Query.cpp:221`.
