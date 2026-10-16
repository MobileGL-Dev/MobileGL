# c1-review-v2 — adversarial review of package c1, round 2 (`p5/c1` head `41992649`, work commit `02f15bb6`)

Reviewer: same-family. Worktree `~/w7/p5-c1`, reverted and clean at the end
(`git status --porcelain` empty, HEAD `41992649`). Every perturbation below was run in that
worktree and reverted with `git checkout --`; nothing was committed. Temp files
`~/w7/p5-c1rev-*`.

**Counts: 3 blockers, 8 majors, 6 minors.**

---

## 1. Findings, most severe first

### B1 — blocker — Five `MGPipeApply*` call sites survived R-17's conversion because they are taken **by address**, so four routed rows never reach the wire under split, and the applier runs on the GL thread outside the barrier

* **Where.** `MobileGL/MG_Impl/Pipe/PipeFill.cpp:1513`
  `Bool EmitDeleteIfPublished(MGPipeKind kind, MGPipeHandle handle, void (*apply)(const MGPHandleOnly&))`,
  body `apply(HandleOnly(kind, handle));` at `:1515`. Its five callers pass the **applier**:
  `:1536 &MGPipeApplyDeleteSamplerView`, `:1557 &MGPipeApplyResourceDestroy`,
  `:1598 &MGPipeApplyResourceDestroy`, `:1635 &MGPipeApplyDeleteSamplerState`,
  `:1656 &MGPipeApplyDeleteShaderState`.
* **Rule.** R-4 ("no slot may be null **and no slot may fall through to the driver**"), R-17
  (the 37 entry points are routed), R-1 / CONTRACT §4 table 3 (`g_applier` is
  **server-exclusive**; the GL thread may not mutate it while the apply thread runs).
* **Claim.** c1's conversion was a rename of `MGPipeApply<Name>(` **call expressions**; the five
  address-of forms have no `(` after the name and were missed, so four of the thirty-seven rows
  (`DeleteSamplerView`, `DeleteShaderState`, `DeleteSamplerState`, `ResourceDestroy` for textures
  and renderbuffers) still execute synchronously on the GL thread under split.
* **Corroboration inside c1's own tree.** `MGPipeRouteDeleteSamplerView` and
  `MGPipeRouteDeleteShaderState` are declared in `MG_Pipe/PipeRoute.h:357, :384` and have **zero
  callers anywhere** — I grepped the whole tree. A routed row whose route function nobody calls
  is the signature of exactly this defect.
* **Failure scenario.** Under `inproc`, `glDeleteTextures` / a program-cache eviction calls
  `MGPipeApplyResourceDestroy` on the GL thread while `mgl-srv-apply` is inside `ApplyOne`: two
  writers on `g_applier` with the barrier not consulted. The server never receives
  `resource_destroy`, so its resource record stays live while the client frees and recycles the
  handle — the ABA shape `HandleRecycleScenario` exists for, now with the two sides disagreeing
  about which generation is live. Under spawn (P6) the same call reaches a client process with
  `g_resourceOps == nullptr` and becomes a silent no-op: the server leaks every texture,
  renderbuffer, sampler CSO, sampler view and shader CSO for the life of the session.
* **How I confirmed it (ran, and reverted).** `~/w7/p5-c1rev-f0.sh`: with the integrator's
  `Init.cpp` one-liner applied, I inserted an `abort()` in `EmitDeleteIfPublished` guarded by
  `MG_Config::Transport != Monolith`, rebuilt `MobileGLIntegrationTest`, and ran
  `MOBILEGL_TRANSPORT=inproc ctest -L integration-split -j 4`:

  ```
  0% tests passed, 21 tests failed out of 21
  DirectGLES.Split.TriangleScenario.AVboBackedTriangleReachesReadPixels ... Subprocess aborted
  ```

  Baseline for the same binary is 14/21. So the unrouted path is reached on **every one of the
  21 split entries**, not in a corner. Reverted; `git status --porcelain` empty.
* **Fix.** The signatures already match (`MGPipeRoute*` take `const MGPHandleOnly&` and return
  `void`), so it is `&MGPipeRoute<Name>` at the five sites — plus a grep gate for
  `&MGPipeApply` in `MG_Impl/`, because the next one will be missed the same way.

### B2 — blocker — ID-49 is reported "ratified and implemented"; on this head the joint readback still mis-places the pixels and corrupts the heap, and the server half is not booked as a debt

* **Where.** `MobileGL/MG_Remote/Server/PipeApplier.cpp:163-201`
  (`ServerVerbSink::OnReadPixels`): it resizes `m_readbackScratch` to **exactly `info.DstSize`**
  and calls `table->GL.ReadPixels(...)` with the **live** pack state. There is no neutral-pack
  bracket: `grep -rn "PACK_ROW_LENGTH\|SetPixelStoreParameters\|neutral" MobileGL/MG_Remote/Server/`
  returns nothing. Its comment still states the *pre*-ID-49 contract verbatim: *"THE CLIENT
  DECLARES THE BYTE COUNT … DstSize is sized on the client from the same GL_PACK_* state the
  frontend owns"* (`:190-194`).
* **Rule.** ID-49 ("the server performs the read with NEUTRAL pack state … into a tight
  `w*h*bytesPerPixel` extent"), CONTRACT §2 row 23, R-2.
* **Claim.** c1 landed its half (`DstSize` is now the tight extent, `EmitTables.cpp:281, :292`)
  while the only consumer of `DstSize` still allocates exactly `DstSize` and lets the driver
  honour a non-neutral pack state. c1's change therefore makes the pre-existing overrun
  **strictly larger**: before, `PackedReadbackBytes` at least covered `ROW_LENGTH` and
  `ALIGNMENT`; the tight size covers neither.
* **Failure scenario.** Any `glReadPixels` under split with `GL_PACK_ROW_LENGTH` or
  `GL_PACK_SKIP_*` set writes past a heap allocation on `mgl-srv-apply`, and the client then
  scatters packed rows as if they were tight.
* **How I confirmed it (ran).**
  `cd ~/w7/p5-c1/build-split && MOBILEGL_TRANSPORT=inproc ctest -R '^DirectGLES.DepthStencilReadbackMatrixScenario.DepthReadbackHonoursThePackPixelStoreParameters$' --output-on-failure`:

  ```
  ../MobileGL/MG_IntegrationTest/Scenarios/DepthStencilReadbackMatrixScenario.cpp:661: Failure
  Expected equality of these values:
    written            Which is: 2
    rectWidth*rectHeight  Which is: 12
  only 2 of 12 destination pixels landed where GL_PACK_ROW_LENGTH/SKIP_* put them
  [  FAILED  ] DepthStencilReadbackMatrixScenario.DepthReadbackHonoursThePackPixelStoreParameters
  double free or corruption (out)
  2145 - ... (Subprocess aborted)
  ```

  gdb on the `ForcedDepthStencilEmulation` twin gives the same failure plus
  `free(): invalid next size (normal)` and `SIGABRT` inside Mesa on thread `mgl-srv-apply`.
  `DirectVulkan.DepthStencilReadbackMatrixScenario.DepthReadbackHonoursThePackPixelStoreParameters`
  and both `PixelStoreSweepScenario.DefaultStateSurvivesTheFullModeSweep` entries SEGFAULT.
  **This is the exact case ID-49 was written for** (`v1-codex-review` finding 1); it is still
  red, and neither c1-v2 §7 ("what v1, w1 and the integrator must provide") nor §10 (debts)
  mentions it.

### B3 — blocker — nothing in the tree observes the **client** arm's 37 rows; a dropped row keeps the monolith adapter and every gate stays green

* **Where.** `MobileGL/MG_Remote/Client/WireTables.cpp:436-480` (`InstallClientWireTables`) has
  exactly one caller (`ClientSession.cpp:574`) and **no test**:
  `grep -rn "InstallClientWireTables" MobileGL/` returns the definition, the declaration and
  that one call. `PipeCatalogueTest.ExactlyTheRoutedRowsAreInstalledAndTheRestAreStillNull`
  (`MG_Test/Pipe/PipeCatalogueTest.cpp:150-201`) calls `MGPipeInstallMonolithTables()` **itself**
  at `:162` and then counts non-null rows, so the 33/4/37 numbers it pins are the **monolith**
  install's.
* **Rule.** R-16 ("no test may construct the state it is meant to observe"), R-8's "right by
  accident" shape, R-4.
* **Claim.** c1-v2 §3 says "`PipeCatalogueTest` asserts both numbers, because a row moved out of
  the generated tables with its escape forgotten would otherwise read only as a smaller count
  while its call site took a null". That is true of the escape table and false of the client
  arm: a client row that is never overwritten is **non-null** and counts.
* **How I confirmed it (ran, and reverted).** `~/w7/p5-c1rev-f1.sh`: deleted
  `gMGPipeContext.SetVertexBuffers = &Wire_SetVertexBuffers;` from `InstallClientWireTables`
  (so every `set_vertex_buffers` in a split process runs the monolith adapter on the GL thread),
  rebuilt, ran:

  ```
  PipeCatalogueTest   [  PASSED  ] 32 tests.
  RemoteClientTest    [  PASSED  ] 26 tests.
  MOBILEGL_TRANSPORT=inproc ctest -L integration-split -j 4  ->  67% tests passed, 7 tests failed out of 21
  ```

  Identical to the unperturbed baseline, same seven failures. Reverted.
* **Why this matters more than one row.** It is the gate that would have caught **B1**. A
  sufficient control is cheap: after `InstallClientWireTables()`, assert that all 33 generated
  rows and all 4 escapes differ from `MGPipeMonolithScreen()/Context()/Escapes()`, and that the
  arm is `kClientWire`.

---

### M1 — major — `Fatal{BarrierViolation, "<slot>"}` cannot fire on this head, so ID-53's E1 control still has nothing to read

`ClientSession.cpp:692-698` is guarded by `ApplyThreadIsInsideApplier()`, which reads
`g_applyThreadInsideApplier` (`:801`). Its only writers are
`NoteApplyThreadEnteredApplier/LeftApplier` (`:804-809`), and
`grep -rn "ScopedApplierEntry\|NoteApplyThreadEnteredApplier" MobileGL/` finds **only the
definitions** (`ClientSession.h:124-130`) — zero call sites. c1-v2 §7.2 concedes this, but ID-53
names c1 as the owner of the string at `ClientSession.cpp:~635` and the red-check ships no
control over the arm at all, so it is a `Fatal` that has never been observed firing.

Ran, with the init patch applied:
```
MOBILEGL_TRANSPORT=inproc MOBILEGL_IPC_VERB_BARRIER=0 ctest -L integration-split -j 4
  ->  14% tests passed, 18 tests failed out of 21
grep -c BarrierViolation <that output>            ->  0
grep -rl BarrierViolation build-split/Testing/Temporary/   ->  (nothing)
```
(Barrier on, same binary: 14/21.) So the 18 reds are still the silent aborts t1 reported, and
E1 would report an unattributed red on the joint head exactly as ID-53 predicted.

### M2 — major — the readback never reads the reply's status or size, so a DECLINED or empty answer is indistinguishable from pixels

`EmitTables.cpp:314-333`: `Int32 status = 0;` is handed to both `EmitAndWait` calls and **never
examined**, and `EmitAndWait` does not hand the caller `replySize` at all. On the other side
`ServerVerbSink::OnReadPixels` answers `kStatusDeclined` when the table has no `GL.ReadPixels`
(`PipeApplier.cpp:176-181`), and `ReplySlotPool::Read` returns **true** for `header.Size == 0`
without touching `outBytes` (`Transport/ReplySlot.h:308-310`). So "the server did not read"
and "the application's buffer is full of pixels" are the same observation on the client. This is
the silently truncated picture ID-47's own comment says an SSIM comparison cannot see, arriving
through the status field instead of through truncation. Fix: require
`status == kStatusOk && replySize == tight` at `EmitReadPixels`, by name.

### M3 — major — `Fatal{ReplyTooLarge}` in `EmitAndWait` is unreachable, and the failure it was written for reports a different, false cause

`ClientSession.cpp:781-787` tests `replySize > replyBytes` — but `ReplySlotPool::Read` already
returns **false** in exactly that case (`ReplySlot.h:311-313`), so control never gets there:
`ClientSession.cpp:765-771` fires first with
`Fatal{ReplyMissing, "<op>"} - seq N carries kReplySlot and the server applied it, but its slot
does not stamp that seq`, which is a statement about the R-3 seq stamp for a failure that has
nothing to do with the stamp. One of the two branches is dead and the other lies about the
cause. (R-16: a check that cannot fail.)

### M4 — major — `SessionWait::ShutDown` is commented "Not a Fatal - teardown legitimately reaches here" and is an abort one frame later, and ERROR is handled three different ways across the five acceptance rows

`ClientSession.cpp:677` pre-sets `*statusOut = kStatusError`; the ShutDown arm (`:750-756`)
returns **without** overwriting it. `Wire_ResourceCreate` / `Wire_SetTextureParams` /
`Wire_ResourceSubData` (`WireTables.cpp:246, 259, 281`) then post that `2` into the mailbox and
`MGPipeTakeReplyBool` (`PipeRoute.cpp:136-142`) aborts `Fatal{ReplyError}`. So a server that went
away during teardown kills the client on its next acceptance row, contradicting the comment.
Meanwhile `Wire_Escape_ResourceRespecify` returns `status == 0`, i.e. **false**, for ERROR
(`WireTables.cpp:342`) and `Wire_Escape_MapPersistent` returns `nullptr` for ERROR
(`:383-391`) — CONTRACT §1's "ERROR is not an acceptance answer at all" is implemented as
*abort* on three rows and as *DECLINED* on two.

### M5 — major — `Fatal{InitialBytesNotCarried}` is not role-aware and can abort the server's own apply thread; and the split respecify branch gives the server role a path monolith does not have

`PipeFill.cpp:748-764` gates the follow-up on `MG_Remote::Client::ClientWireRecordsEmitted()`.
That counter is only incremented on the **client** path: every `Wire_*` emitter returns through
`RunsAsTheServerRole()` before `++g_emitted` (`WireTables.cpp:116-148, 200-211, 313-315, …`).
c1-v2 §4's own R-17.3 establishes that the apply thread reaches `ResourceRespecify` — that is
where `Fatal{BarrierTimeout, "ResourceRespecify"}` from `mgl-srv-apply` came from. A respecify
with `HasDefinedContent != 0` on that thread therefore takes the `Transport != Monolith` branch,
emits nothing countable, and aborts the server by name. Separately, on that same thread the
branch runs `respecify(nullptr)` **plus** a follow-up `resource_subdata` through the monolith
adapters, which is not what the monolith control arm does — the same objection c1 raises against
emitting the follow-up under monolith. The gate belongs behind `RunsAsTheServerRole()`.

### M6 — major — the red-check runner accepts any non-zero exit, scores a build break as a red, and never restores or re-runs the four integration controls

`~/w7/p5-c1-r2-redcheck.py`:
* verdict line: `red = rc != 0 or "[  FAILED  ]" in out or "PASSED  ] 0 tests" in out` — no
  perturbation asserts **its own** failure string. This is ID-46 finding 9 / codex C9's exact
  shape, assigned to v1 in ID-54 and not applied here; c1-v2 §8 does not mention it.
* `if not built: verdict, red = "BUILD-BREAK (also a red)", True` — a perturbation that does not
  compile counts as a working control.
* the restore loop is `for target, binary in {PCT, RCT, PWC, SAN}`; **`ITEST` is excluded**, and
  the preflight only *builds* it. So the four integration controls (ID47-b, R17, v1-seam, R13.3)
  have no green baseline run and no restored run: anything that makes the integration binary
  abort for an unrelated reason (a missing GPU, the init patch itself) scores all four RED.
  Given B1, `MOBILEGL_ITEST_REQUIRE_GPU=1` and the 30-second barrier, that is not hypothetical.
* control **R4** ("a missing answer is not silently false") replaces `if (g_reply.Slot == 0)`
  with `if (false)`; `TakeReply`'s *next* check (`g_reply.Slot != slot.Id`, `PipeRoute.cpp:113`)
  then aborts anyway with `Fatal{ReplyMismatched}`. The control goes red on a death-regex
  mismatch, never on the "silently false" behaviour it names.

### M7 — major — `ClientWireRecordsEmitted()` is not what arms the lane, contrary to `WireTables.h` and to the charter's item 9

`WireTables.h:49-53` says the counter "is t1's FOURTH arming fact … deliberately NOT the
encoder's EmitSeq: EmitSeq moves for the five class-B verbs too, so a lane that armed on it
would arm on a Clear and call the resource path proven." The harness arms on **EmitSeq**:
`MG_IntegrationTest/Harness/SplitRuntimePeek.cpp:50` reads
`session->Encoder().EmitSeq()` and `Harness/ScenarioFixture.h:85` is the assertion. Nothing in
the tree reads `ClientWireRecordsEmitted()` except `PipeFill.cpp`'s own self-check. So the clause
the charter told c1 to "make true" is still armed on the signal c1's own header calls
insufficient — and that is why B1 and B3 are invisible to all 21 entries.

### M8 — major — the pack-PBO destination of `glReadPixels` is neither wired nor refused

Charter item 7: *"The PBO destination half (fire-and-forget + client-side `MarkGpuWritten`) is
b1's design; wire it, do not redesign it."* `EmitReadPixels` (`EmitTables.cpp:268-334`) always
sets `info.Res = kMGPipeNullHandle` and `info.DstOffset = 0`, and passes `pixels` to
`EmitAndWait` as a host buffer of `tight` bytes. `GL_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp:3096`
(`ReadPixels_Backend`) calls the table slot **unconditionally**, and with a
`GL_PIXEL_PACK_BUFFER` bound `pixels` is a buffer **offset**, not an address. The client then
writes `tight` bytes at that offset. There is no refusal, no `Fatal`, and no debt entry in
c1-v1.md or c1-v2.md (`grep -i "pbo\|pixelpack"` finds nothing in either). Latent only because
no `MG_IntegrationTest` scenario binds a pack PBO (`grep -rl PIXEL_PACK_BUFFER MG_IntegrationTest/`
is empty) — the E2 retrace path is not so constrained.

---

### Minors

* **m1** — the four escaped rows leave their generated thunks pointing at null table entries with
  no guard: `MGP_ResourceRespecify`, `MGP_MapPersistent`, `MGP_ResourceFlushRange`,
  `MGP_CreateShaderState` (`generated/PipeThunks.inc`) call through a null
  `gMGPipeScreen/Context` member. `PipeCatalogueTest` *pins* those rows null, i.e. it pins the
  trap rather than closing it. A one-line `Fatal{RowIsAnEscape, "<name>"}` adapter installed in
  the generated row would make the obvious spelling say so.
* **m2** — after `ClientSession::Stop()` the 37 routed rows run the **monolith adapters**
  (`WireTables.cpp:482-488`) while the five class-B verb slots `Fatal{NoClientSession}`
  (`EmitTables.cpp`'s `RequireSession`). A GL call that races teardown therefore produces real
  driver side effects through one half of the table and an abort through the other; under spawn
  the monolith adapter has no `g_resourceOps` at all, which is R-8's silent-no-op shape.
  `Uninstall` also runs *before* `Stop()`'s `if (!m_started)` early return, so a failed `Start`
  that never installed anything still re-installs monolith — harmless today, but it means "the
  arm is monolith" no longer implies "no session was ever up".
* **m3** — two counters for the same fact with different storage and no test over the second:
  `MGPipeRepliesTaken()/Declined()` are `thread_local` (`PipeRoute.cpp:61`) while
  `ClientWireRecordsEmitted()/Declined()` are plain unsynchronised globals
  (`WireTables.cpp:70-71`).
* **m4** — rule A now *arms*, split-only, two cross-checks that were inert: the
  `SetGlobalConstants` length against the program's own `GlobalUboSize` (`PipeApply.cpp:2885`) and the texture
  `ResourceSubData` staged length. No split entry exercises either (TriangleScenario's program
  has no default uniform block and no split entry uploads a texture), so the first record that
  disagrees will do so in a lane nobody has run.
* **m5** — the generator change ships no negative control of its own; the nine `--self-test`
  trips are p1's pre-existing ones and none of them is about a `kHasBlob` row's new pair.
  `--check` does catch a stale `.inc` (I ran it), so this is small, but "five lines with no
  control" is the shape R-16 was written about.
* **m6** — `ReadbackPackStateIsTight` (`EmitTables.cpp:97-107`) ignores `pack.ImageHeight`, and
  the scatter's `rowPixels = pack.RowLength` path will make rows **overlap** when
  `0 < RowLength < width`. GL leaves that case to the implementation, but the client is the side
  that chose to re-derive 8.4.4 and should say so rather than memcpy into itself.

---

## 2. Claims in `c1-v2.md` the code does not support

1. **§4, "ID-49 … Ratified as issued and implemented"**, and "the depth/stencil reads the 21
   split entries touch take it with no special case". The server half does not exist on this head
   and the named case is red with heap corruption (**B2**).
2. **§6, "The remaining 7 are one root cause … That is the only thing between 14/21 and 21/21."**
   It is the only thing between 14/21 and 21/21 *for the 21 entries*, which is a much weaker
   statement than it reads: the 21 entries cannot see B1, B3, M2 or M8. I confirmed the
   `resource_flush_range` attribution itself (`PipeApply.cpp:1868-1873` faults on
   `record.Size != 0 && bytes == nullptr`; `PipeWireCodec.cpp:1777` passes `nullptr` by R-13.2) —
   that part is correct.
3. **§3, "`PipeCatalogueTest` asserts both numbers"** as a guard on the routing. It asserts the
   monolith install, which the test performs itself (**B3**).
4. **§1 table, "the call sites | 40 direct `MGPipeApply*` calls | 40 `MGPipeRoute*` calls"** and
   §3's "The call-site number was exactly right (40, measured)". Five sites take the applier's
   **address** and were not converted (**B1**); the census that produced "40" evidently matched
   `MGPipeApply<Name>(`.
5. **`WireTables.h:49-53` / charter item 9** — `ClientWireRecordsEmitted()` is described as the
   lane's fourth arming fact. The lane arms on `EmitSeq` (**M7**).
6. **§8, "20 / 20 RED"** is true of the runner's own verdict, but the runner's verdict is
   "non-zero exit or a build break" and four of the twenty are never restored or re-run
   (**M6**). "Every control asserts its own failure string" — R-16's wording — is not met by
   this harness; only the gtest cases it drives do that, and only some of them.
7. **§7.2** correctly flags that `ApplyThreadIsInsideApplier()` is inert, but §4/§8 present the
   R-1 barrier as gated. On this head `Fatal{BarrierViolation}` has never been observed firing
   and no control targets it (**M1**).

---

## 3. Verified as claimed — do not re-run these

* **G1.** `python3 scripts/symbol_report.py --before ~/w7/p5-before-libMobileGL.so --after
  ~/w7/p5-c1/build-linux/libMobileGL.so --threshold 0` →
  `.text 10806611 -> 10806611 (+0, +0.000%)`, `.data/.bss/.rodata +0`,
  `27814 -> 27814 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed`.
  Baseline `.READY` = `4595163c` (ID-44's). The two new CMake lines add nothing to the pull
  build: `nm --defined-only build-linux/libMobileGL.so | grep -ic MG_Remote` → `0`,
  `… | grep -ic PipeRoute` → `0`, `… | grep -ic MGPipeInstallMonolithTables` → `0`.
* **Generators.** `gen_pipe.py --check` → `generated files are up to date` (rc 0);
  `--self-test` → 9 negative-control trips, each printing the guard's own required message,
  plus the positive control (rc 0). **No `.def` file changed** (`git show 02f15bb6 --stat`
  lists only `gen_pipe.py`, `PipeTables.inc`, `PipeThunks.inc`).
* **The generator change is symmetric and minimal** — `scripts/gen_pipe.py:155-166`, five lines,
  `kHasBlob` gains `const void* blobBytes, Uint64 blobByteCount` exactly where `kVarTail` gains
  its pair; ten rows changed; `GetCaps` gains it and has no applier.
* **G2 monolith equivalence of the `kHasBlob` monolith adapters.** Each `MGP_MONO_BLOB` /
  `MGP_MONO_TAIL` / `Mono_*` body (`PipeRoute.cpp:177-310`) discards the new count and forwards
  the identical pointer to `MGPipeApply<Name>`; the only adapters that do more are the five that
  post a reply and `Mono_SetResidualValueState`'s size check. `MGPipeApplyVerifyCorruption` is
  untouched (`PipeFill.cpp:469` is the only surviving direct applier call expression).
* **`kReplySlot` is 14 rows and covers exactly the four acceptance rows + `MapPersistent`** —
  `generated/PipeWire.inc:166-234` rows 1,2,3,5,8,9,14,15,47,48,52,55,58,69.
* **The `PostReply` hunk in w1's `PipeWireCodec.cpp` is correct and scoped.** `noteAcceptance`
  (`:1334`) has exactly four callers — `:1368 ResourceCreate`, `:1387 ResourceRespecify`,
  `:1711 SetTextureParams`, `:1747 ResourceSubData` — and `PipeWireDecoder::PostReply`
  (`:1122-1140`) refuses any op without `kReplySlot` by name, so the flag table stays the single
  source of truth (ID-31).
* **The reply is read inside the barrier wait, keyed by seq, never re-derived.**
  `ClientSession::EmitAndWait` (`ClientSession.cpp:674-789`) is encode → publish → wait →
  `ReadReply(seq, …)`; `ownsReplySlot` comes from `MGPipeCallFlagsFor(op)` and **not** from the
  caller; with `MOBILEGL_IPC_VERB_BARRIER=0` a reply-slot row still waits (`:725`).
* **A stale reply cannot be read after a ring wrap.** `ReplySlotPool::Read`
  (`Transport/ReplySlot.h:267-294`) compares the slot's stamped `header.Seq` against the
  requested seq and refuses on disagreement; the mailbox adds a second check
  (`PipeRoute.cpp:113` `Fatal{ReplyMismatched}`).
* **ID-47 is s1's helper at the call site, not a copy.** `EmitTables.cpp:304-307` calls
  `session.RequireReadPixelsReplyFits(w, h, format, type, tight)`; the local
  `RefuseOversizeReadback` that `02f15bb6` introduced is **gone at HEAD** (the merge removed it),
  and `RemoteReadback.ExactlyTheCapacityPassesAndOneByteMoreIsRefusedByName`
  (`RemoteClientTest.cpp:410-449`) drives a real `ReplySlotPool` over a real mapping and asserts
  the message, the read's own dimensions and the byte count.
* **ID-49's scatter arithmetic itself is right** (the unit level). I ran the 4×3 RGBA8 /
  `ROW_LENGTH=8, SKIP_ROWS=1, SKIP_PIXELS=2` case against a `0xCD`-filled destination —
  `RemoteClientTest` `TheTightRowsAreScatteredWhereThePackStateSaysAndTheGapsAreLeftAlone`,
  `TheFastPathIsTakenExactlyWhenTheScatterWouldChangeNothing`,
  `DstSizeIsTheTightExtentAndNeverThePackedOne` — **26/26 passed**. Stride 32, first byte 40,
  12 pixels placed, gaps keep the sentinel. The arithmetic is not the problem; **B2** is that
  nothing on the wire delivers tight rows to it.
* **`ctest -L unit`, `build-split`: 100% tests passed, 0 failed out of 2025.** (I did not re-run
  the other four lanes.)
* **`MOBILEGL_TRANSPORT=inproc ctest -L integration-split -j 4`, with the `init_patch`:
  67% passed, 7 failed of 21** — c1's 14/21 reproduces exactly, same seven
  `PersistentCoherentMapScenario` entries.
* **The `resource_flush_range` attribution.** `PipeApply.cpp:1868-1873` and
  `PipeWireCodec.cpp:1777`, as c1 describes. c1's client half is landed
  (`PipeFill.cpp:846-866` stages exactly `[offset, size)` and never widens, per ID-37).
* **R-2 rule B on the escapes.** `Wire_Escape_ResourceFlushRange` ignores `bytes`
  (`WireTables.cpp:356`), `Wire_Escape_MapPersistent` ignores `size`/`seedBytes` (`:373-374`),
  `Wire_Escape_ResourceRespecify` aborts by name on a non-null `initialBytes` (`:317-324`), and
  `Wire_Escape_CreateShaderState` stages one `EncodeProgramArtifacts` archive and zeroes the six
  `Spirv[i]` blobrefs (`:419-428`). `Wire_ResourceSubData` / `Wire_BufferSubDataResident`
  overwrite the payload's `Blob` (which arrives holding a host address) with the staged run
  (`:274`, `:295`).
* **The three extra byte counts are right at their call sites.** `SetGlobalConstants` ←
  `program->GetUBOSize()` (`ProgramEmit.h:175, 199`); texture `ResourceSubData` ←
  `mipmap->GetMipmapByteSize(uploadTarget, level)`, which is the byte size of the same
  `MapMipmapData(uploadTarget, level)` run the regions' `SrcOffset`s index into, so mip levels
  and cube faces are keyed correctly (`TextureEmit.h:1219, 1223, 1283`); buffer
  `ResourceSubData` / `BufferSubDataResident` ← the walk's own `length`
  (`PipeFill.cpp:793, 829`), which is also what `MGPipeBuildSubDataRecord` writes into
  `Blob.Size` (`ResourceTracker.h:208-218`). The buffer half's `RegionCount` is 0 by
  construction, so `MGPipeRouteResourceSubData`'s defaulted `regions = nullptr` cannot
  under-supply a declared tail.

---

## 4. What I could not verify, and why

* **G2/G14 ctest-name comparison** (0 removed, 42 / 230 added). I did not re-take the baseline
  name lists; `~/w7/p5-c1-r2-names-*.txt` are c1's own. I did confirm the mechanism c1 describes
  — `PipeCatalogue.UninstalledTablesAreAllNull` is kept and narrowed rather than renamed
  (`PipeCatalogueTest.cpp:108-142`).
* **The other four build dirs' unit lanes** (1816 ×3, 2025 verify-split) and the
  `build-push` `integration-gpu` 1128/1128. Only `build-split` was re-run.
* **G5 / p3a / p4a untouched-region scripts** — not re-run.
* **The 20 red-check controls individually.** Re-running the harness is twenty rebuild cycles; I
  reviewed it statically (M6) and re-derived two of its controls by hand (B3's inverse, and the
  `EmitDeleteIfPublished` probe).
* **Whether B2's heap corruption also fires on the E2 / OpenRA retrace path.** No retrace or adb
  device was touched, per the brief.
* **Spawn-transport behaviour** for m2/B1's second half — P6 is not buildable here.
* The `inproc` `integration-gpu` census below is **with** the init patch compiled in; the source
  is reverted but `build-split`'s binaries were rebuilt from the reverted source afterwards
  (`~/w7/p5-c1rev-restore-build.log`).

---

## 5. The `inproc` `integration-gpu` census c1 did not report

`cd ~/w7/p5-c1/build-split && MOBILEGL_TRANSPORT=inproc ctest -L integration-gpu -j 8`,
init patch applied (`~/w7/p5-c1rev-igpu2.log`, 1149 entries):

| verdict | count |
|---|---|
| **passed** | **386** |
| **skipped** | **180** |
| **aborted** (`***Exception: Subprocess aborted`) | **556** |
| **failed** (`***Failed`) | **23** |
| **segfault** | **4** |
| timeout | 0 |

`49% tests passed, 583 tests failed out of 1149`.

**Read it honestly:** most of the 556 aborts are the documented P5 state — forcing `inproc` onto
the 1128 non-`Split` entries drives them into class C's `Fatal{UnmigratedVerb}`, which is
by design (CONTRACT §7). What is *not* by design is the tail:

* the **4 segfaults** are `DirectGLES`/`DirectVulkan`
  `PixelStoreSweepScenario.DefaultStateSurvivesTheFullModeSweep` and `DirectVulkan` /
  `DirectGLES.ForcedDepthStencilEmulation`
  `DepthStencilReadbackMatrixScenario.DepthReadbackHonoursThePackPixelStoreParameters`, plus the
  plain `DirectGLES` twin of the latter as an abort — i.e. **every pack-state readback entry in
  the tree**, which is **B2**;
* the `PixelStoreSweep` crashes are at *teardown*, in Mesa, reached through c1's new
  `BackendObject_Remote::ReleaseEGLSurface` → `Server::ServerReleaseEGLSurface` →
  `DestroyEGLContext` on `mgl-srv-apply` (gdb backtrace in `~/w7/p5-c1rev-bt.sh`'s output), with
  **~30 live `mgl-srv-apply` threads** in the process at the time — the apply threads of earlier
  contexts are not joined. That is CONTRACT §4's teardown order (Doorbell::Kill → bounded join)
  not holding across repeated context creation. It is v1's loop, but it is reached only through
  the seam c1 rewrote this round and neither report mentions it.

---

## 6. Verdict

**Needs more than one fix round.**

R-17's *shape* is right — the escape table, the reply mailbox, the role gate and the generator
change are all well argued and, where I could drive them directly, correct. But the routing is
not complete (**B1**: four rows and five call sites still call the applier on the GL thread,
reached on every one of the 21 split entries), the phase's own joint deliverable ID-49 is red
with heap corruption on the case it was written for and is reported as done (**B2**), and there
is no gate at either end that could have caught either — the one test that claims to pin "37"
pins the monolith install and stays green when a client row is deleted (**B3**).

B1 and B2 are code fixes of a few lines each. B3, M1, M6 and M7 are the reason they got this far,
and fixing only the first two would leave the next one exactly as invisible. The minimum for a
merge, in order:

1. route the five `&MGPipeApply*` sites and add a grep gate for `&MGPipeApply` under `MG_Impl/`;
2. land ID-49's server half (neutral pack bracket in `ServerVerbSink::OnReadPixels`) or revert
   `DstSize` to the packed extent until it lands — it must not ship half-done; re-run
   `DepthReadbackHonoursThePackPixelStoreParameters` and `PixelStoreSweepScenario` on both
   backends as the control;
3. a case that asserts the **client** arm's 37 rows differ from `MGPipeMonolith*()` and that
   `MGPipeInstalledArm() == kClientWire`, red-checked by deleting one row;
4. `EmitReadPixels` must check `status` and `replySize` by name (M2);
5. make the red-check assert each perturbation's own string and restore/re-run the integration
   controls (M6);
6. either arm `ScopedApplierEntry` or hand ID-53 a diagnostic E1 can read (M1).

---

## 7. Files this review cites (the rest are deleted)

- `~/w7/p5-c1rev-f0.sh` — B1's probe: abort inside `EmitDeleteIfPublished` under a non-monolith
  transport; log `~/w7/p5-c1rev-f0-lane.log` (21/21 aborted against a 14/21 baseline).
- `~/w7/p5-c1rev-f1.sh` — B3's perturbation: one client row dropped, everything still green.
- `~/w7/p5-c1rev-b0.sh` — the unit lane and the `MOBILEGL_IPC_VERB_BARRIER=0` pair for M1;
  logs `~/w7/p5-c1rev-unit-split.log`, `-isplit-on.log`, `-isplit-b0.log`.
- `~/w7/p5-c1rev-igpu.sh`, `~/w7/p5-c1rev-igpu2.log` — §5's `inproc` `integration-gpu` census.
- `~/w7/p5-c1rev-bt.sh` — the gdb backtraces for B2 and for the teardown crash.
- `~/w7/p5-c1rev-initpatch.py` — the integrator's merge-time `Init.cpp` one-liner, applied and
  reverted by each of the three scripts above.
