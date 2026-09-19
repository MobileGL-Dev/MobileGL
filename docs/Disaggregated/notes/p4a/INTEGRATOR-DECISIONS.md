# P4a integrator decisions (feat/disaggregated, 2026-09-08)

## ID-1 Base ref `37da3c3a` (= P3a docs commit; code fde5fda3), baselines captured there
`BRIEF-P4A.md` was verified at 37da3c3a. `~/w7/p4a-before-*` (pull lib, ctest names, RenderStateImpl sha,
dirty-surface) are the pull build of 37da3c3a; P4a's admitted-resize set is EMPTY (G1 0/0/0/0 after every merge).
P3a's eleven G5 functions stay byte-identical (`scripts/p3a_untouched_regions.sh 37da3c3a HEAD`); P4a names its
own byte-identical set per the brief's D-N (package F adds the script).

## ID-2 Order: contract -> wire -> clientfb -> clientsp -> esprytobj -> esprytdraw -> gates
Every package branches from the tag `p4a/contract`; branch refs are spelled `refs/heads/...` (the tag makes the
short name ambiguous). Each Espryt package gets a real-path verification round on the integrated tree after the
client packages land (P3a found three seam defects that way), then a focused re-review; a final whole-diff
review closes the phase. wire is behaviour-neutral (nothing consumes its records until Espryt lands).

## ID-3 Tools (all phase-parameterised, in ~/w7/notes/tools)
`wsl_tree.sh p4a <slug> <base> [verify]`, `wsl_integrate.sh p4a <slug>...`, `wsl_capture_baseline.sh p4a`,
`wsl_p3a_gate.sh` -> `wsl_p4a_gate.sh` (integrator writes it per the brief's D.3 delta), `wsl_build_trace_apks.sh
p4a` (Release + debug-signed; prints the lib size - Release .text ~9.4 MB, ID-17 of P3a), `p3a_ab.sh` ->
`p4a_ab.sh` with the previous mask 0x1fff-vs-0x1ff arms via `p3a_ab_7f.sh`'s shape, `ab_reduce3.py`,
`wsl_p3a_bench.sh` -> `wsl_p4a_bench.sh` (T2 = 0x1ff arm). Package agents never touch ~/w7/pipe.

## ID-4 Models and rounds
Package agents and reviewers run on opus (user rule 2026-09-08). One adversarial review per package, at most
two rework rounds; declared minors do not block integration.

## ID-5 Durable locations
Brief `~/w7/notes/p4a/BRIEF-P4A.md`; scouts `~/w7/notes/p4a/scout-*.md`; results and reviews
`~/w7/notes/p4a/p4a-results/<package>-v<N>.md` / `<package>-review-v<N>.md`; A/B `~/w7/notes/p4a/ab*`; bench
`~/w7/notes/p4a/bench`. Package logs `~/w7/p4a-<slug>-*.log` are deleted by their owner when the round ends.

## ID-6 Performance is recorded, not gated (user, 2026-09-08)
Against MEASUREMENTS.md §20's Release baseline (pull / 0x7f / 0x1ff); P4a adds the 0x1fff arm. Correctness
gates stay hard. Device A/B on Release APKs only; the create-indirect and rd12/Magma/Adreno fixtures stay out.

## ID-7 Working over UNC and WSL
Edit worktrees through `//wsl.localhost/Arch/home/swung/w7/p4a-<slug>/...` (files yes, mkdir no). Commands
through a script file `<scratchpad>/wsl/<name>.sh` (LF) run as `MSYS_NO_PATHCONV=1 wsl.exe -d Arch -- bash
/mnt/c/<scratchpad>/wsl/<name>.sh`; never pass ~ or $vars inline; foreground under 8 minutes, longer via
`setsid nohup ... &` + log polling; never delete the shared `wsl` scratchpad directory, only your own files.

## ID-8 Rulings carried from P3a that bind every P4a package
- Exit-order rule (P3a ID-18/21): no frontend destructor may run from an exit handler into pipe/backend state.
  Every new static holder of frontend SharedPtrs is leak-at-exit storage; every new singleton reachable from a
  destructor is never-destroyed. Prove with `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` on both lanes.
- Backend-neutral death paths from day one (P3a final-review C-1): the client emits delete/destroy and frees the
  slot from the frontend destructor for EVERY kind it mints; a backend death notice is a redundant second path
  and must be idempotent. A leak test per new kind, run on the DirectVulkan lane too.
- Stale-generation refusal in every GetOrCreate(handle) (P3a contract-review M2).
- The ownership table lists generated/.def interfaces; a package that retires a .def row must not leave another
  referencing the enumerator (P2's bb2a236d trap) - the contract owns the enum-coupled PipeFill.cpp block.
- Files C.7 gives to nobody are granted explicitly by the integrator when a package needs them (GLImpl entry
  points, MG_Test fixtures such as BufferTest's ScopedBackendOps that must scope BOTH op tables).
- ColorOS install dialog = topResumedActivity InstallGuideActivity (not mCurrentFocus); tapping mid-install drops
  adb for seconds; the Xiaomi needs cpuss-0-0 < 42 C before pinning; a crash resets the GPU pwrlevel range.

## ID-9 Contract integrated: feat/disaggregated = `08192d72` (pushed) = the tag `p4a/contract`
Fast-forwarded (pipe was at 37da3c3a): pull symbols 0/0/0/0, unit 1635 x 3, G2 0, G14 0 removed / +13, P3a's
eleven byte-identical. contract-v1 deviations the packages must know: **D1** c0 CREATED the five emit headers
MG_Impl/Pipe/{Framebuffer,Texture,Sampler,Image,Program}Emit.h with a stub emitter and `kMGPipeWired*Subsystem
= 0` each - packages B (Framebuffer/Texture) and C (Sampler/Image/Program) own those headers' BODIES and never
touch PipeFill.cpp; the wired bit is added in the commit that gives the emitter its body. **D13** the ShaderCso
composite band has its own dense table in the allocator and the applier (band base 983040; a slot-indexed
vector would cost ~236 MB per composite). **D5** six death helpers, one per kind (the texture helper releases
the sampler view, not the built-in sampler CSO, which has its own lifetime). **D2** MGPipeResourceTarget has a
13th enumerator TexRect. **D6** two UNDECIDED dirty-surface rows (UseProgram/NEW_SHADER, BindVertexArray/
NEW_VERTEX_ELEMENTS) are marked with the tool's reason, not silenced. **D7** BindProgramPipelineObject is
kPulledEveryVerb. **D16** clang++-20 does not exist on the box; the gate spells `clang++`. Contract review v1
in flight; the six sibling trees were reset to the tag and are rebuilding (~/w7/p4a-trees2.log).

## ID-10 Contract review v1 = ACCEPT WITH MAJORS; c0b in flight on p4a/contract, M2 goes to wire
Six majors: M1 the birth half of the MG_State coupling is missing (PipeMutation.h declares only the six death
helpers; the death helpers hard-code published=false) so B/C cannot emit create/respecify/params without
touching A's files; M2 MGPipeApplyResourceSubData has no MGPSubRegion tail (D-D3/D-D5 unreachable); M3 wants()
ignores kMGPipeWiredSubsystems so P4a's emitters are live under 0x1fff before their bodies exist; M4 the
sampler-view death helper skips the death notice; M5 HighWater(ShaderCso) returns the band top (leak test
vacuous); M6 check_include_closure.py exits 0 without a compiler. Disposition: M1/M3/M4/M5/M6 = commit(s) c0b on
p4a/contract by a fix agent touching ONLY PipeMutation.h, PipeFill.{h,cpp}, SlotAllocator.*, the closure script
and an A-owned test (no overlap with the six in-flight packages); M2 = wire's rework (it owns PipeApply after
the tag). The tag stays at c0; B..F rebase onto pipe (c0 + c0b) at their rework/verification round; B/C v1 will
show M1-driven deviations that the rework reconciles against contract-v2.md's exact birth-hook declarations.

## ID-11 c0b integrated: feat/disaggregated = `2cb44039` (pushed); wire v1 landed on p4a/wire
c0b (32033d69 + 2cb44039): PipeMutation.h now declares the birth half (4 mints, 9 emissions, a 3-call
publication latch read by both halves; `if constexpr` seams keyed on each family's wired constant forward to
the entry points B/C must provide - contract-v2.md lists them exactly); wants() consults kMGPipeWiredSubsystems;
the sampler-view helper raises the notice; composites are counted apart (CompositeHighWater/LiveCount/FreeCount);
check_include_closure.py validates --compiler in every mode (test.yml:774's clang++-20 is fine in CI, where
clang-20 is installed; the local gate spells clang++). On pipe: G1 0/0/0/0, unit 1636 x 3, G2 0, G14 0/+14.
wire v1 (86835b50 / bc9abfbd / 0f990633 / 3c07c8c3): W1 closes M2 with a defaulted trailing `const MGPSubRegion*`
tail; W3 fixed a real destroy-dispatch defect; hazard H1 (BuiltinSampler == null is Fatal but D-K2 allows bit 10
without bit 11) awaits the wire review's assignment (contract dependency rule vs client minting). Every package
rework/verification round rebases onto refs/heads/feat/disaggregated (c0 + c0b).

## ID-12 esprytobj v1 landed (`9bb95335`, six commits on p4a/esprytobj); four contract seams ruled, c0c in flight
- **DV-3 (`MGPSubData::Target`)**: the brief's D-D3 names BOTH halves (resource target + cube-face upload target).
  clientfb v1 packs `low byte = MGPipeResourceTarget, high byte = TextureUploadTarget`; wire's `SubDataNamesABuffer`
  keys on the whole field `== 0`; `TextureUploadTarget::Texture1D` is 0, so the bare enumerator Espryt v1 matches on
  would collide with Buffer. RULING: the packed form is the contract encoding. c0c moves `MGPipePackSubDataTarget` /
  `MGPipeSubDataResourceTargetOf` / `MGPipeSubDataUploadTargetOf` into MGPipeTypes.h (Uint32 arguments, backend-
  neutral); B's rework deletes its copies; D's rework decodes the HIGH BYTE at every `PendingUploads` match
  (`FindPipeTextureUpload` and the consume site); wire is unchanged (it matches the field verbatim).
- **DV-2 (`DepthStencilMode`)**: 0 = GL_DEPTH_COMPONENT, 1 = GL_STENCIL_INDEX; both sides already agree. c0c mints
  `kMGPipeDepthStencilModeDepth/Stencil` in MGPipeTypes.h; B's rework drops its copies (the GLenum->byte helper is B's).
- **DV-4 (`MGPSurface::Kind`)**: the numeric `MGPipeKind::{None,Texture,Renderbuffer}` values (what B emits). c0c mints
  `kMGPipeSurfaceKindNone/Texture/Renderbuffer`; B's rework drops its copies.
- **DV-5 (`MGPSurface::Pad0`)** -> `Uint16 TextureTarget` = `static_cast<Uint16>(MobileGL::TextureTarget)`, 0xFFFF
  (= Unknown) on every non-texture point, consulted only when `Kind == Texture`; listed in `MGP_FIELDS_MGPSurface`;
  size stays 24. B's rework fills it; D's rework moves the four cross-object masks onto it; E's `SyncAttachmentObject`
  reads it.
- DV-1/DV-7/DV-8 accepted as C.7 boundary notes (E owns the resolution sites); DV-6 harmless as argued; DV-9 macros
  accepted as G1-forced (the review checks `#undef` hygiene and pull-text identity); DV-10/11/12 fine. The 183 named
  "no applier record" refusals on the default arm are the expected pre-client shape and must go to ZERO on the
  integrated tree (a residual one is a seam defect, P3a's lesson).
- c0c = one commit on p4a/contract by a fix agent touching only MGPipeTypes.h + PipeFields.def (+ an A-owned unit
  test); integrated (fast-forward) BEFORE wire; every package rework rebases onto it.

## ID-13 clientsp v1 landed (`ecd5b2e7`, four commits on p4a/clientsp); the LFS fixture trap
- Every lane green on its tree (G1 0/0/0/0, unit x3, integration-gpu 966/966 on three arms, verify 844/844 zero
  Fatal under tcache_count=0, retrace 79/79, +38 test names). Its findings F2 (wants() ignores the wired constants)
  and F3 (death helpers hard-code published=false) are contract-review M3/M1, CLOSED by c0b (contract-v2.md): the
  rework rebases onto pipe and reconciles against the exact birth-hook declarations, nothing more. F1 (member-wise
  assignment of SamplerParameters leaves padding unspecified; the cache rejected its own entries, hit rate zero,
  suite-order dependent) is a real bug the design predicted; the fix is memset+memcpy and is pinned - the review
  verifies it. **LFS trap**: the 12:24 WSL restart left tools/trace_replay/fixtures/*.tgz as LFS POINTERS (132 bytes)
  in the wire / esprytobj / esprytdraw / gates trees (clientfb and contract were fine); a retrace on such a tree reads
  2/79. I ran `git lfs checkout` in wire/esprytobj/gates (pointers 0 afterwards; esprytdraw and pipe were already
  clean). Every private worktree a reviewer creates must run `git lfs checkout` before any retrace.

## ID-14 The built-in sampler seam between B and C is ruled: B acquires it through C's cache
clientfb v1 (TextureEmit.h ~636) mints `MGPipeSlots().Acquire(MGPipeKind::SamplerCso, sampler->GetLifetimeId())`
per sampler OBJECT; clientsp content-addresses sampler CSOs (D-F1, `MGPipeSamplerCsoCache::Acquire(params,
payloadBytes)`, SamplerEmit.h ~186 names this as B's seam) and c0b's hook comment says the handle rule for this kind
is THE EMITTER'S. B's handle would never receive a create_sampler_state record, so on the integrated tree every
texture's params would be refused (or, with a null, Fatal{ProtocolCorruption}) - the seam C called the highest
post-rebase risk. RULING: B's rework obtains `MGPTextureParams::BuiltinSampler` from C's cache seam (the exact call
is named by the two reviews; TextureEmit.h may include SamplerEmit.h - the include-closure gate must be re-run) and
re-emits set_texture_params whenever the built-in sampler's parameters change (the content-addressed handle changes
with content; D-E2's SamplerResync covers the re-sync). B keeps NO per-texture death path for the sampler CSO: LRU
eviction in C's cache is the only delete_sampler_state emitter (contract D5/M4 unchanged). Hazard H1 (wire): the
Fatal on a null BuiltinSampler stays; the D-K2 "bit 10 without bit 11" arm must therefore be refused at the CONTRACT
dependency rule (texture bit implies sampler bit), which is package F's 0x9ff-style dependency-refusal scenario.

## ID-15 wire review v1 = REWORK (C1 + five minors); H1 becomes a fourth D-K2 dependency row; esprytdraw v1 landed
- wire C1 (PipeApply.cpp:1485): resource_respecify clears the WHOLE PendingUploads vector, but AllocateStorage /
  MarkStorageDirty are per (uploadTarget, level), so defining level 1 (or glGenerateMipmap) after level 0's accepted
  upload was kept by Espryt's incomplete-texture bail loses level 0's texels silently. Fix as specified: scope the
  clear to the redefined level via a trailing defaulted pointer (null = whole resource), one case + one mutation.
  Minors m1-m5 (ReleaseObjectRecords leaves framebuffer records + three unit windows populated - contract-review m2
  assigned to wire; the PipeApply.h:422-428 framebuffer-refusal promise; the Renderbuffer enumerator accepted as an
  upload target; no acceptance signal to the emitter; unmap_persistent's inert kind check) are fixed or declared in
  the same rework. W1-W6/W8 accepted; W7 rejected (= C1). Rework rebases onto refs/heads/feat/disaggregated.
- H1 ruling (refines ID-14): the brief's D-K2 says "bit 10 without 11 is fine"; that sentence is WRONG for P4a as
  built - MGPTextureParams::BuiltinSampler is a SamplerCso handle, only bit 11 mints sampler CSOs (c0b's four
  unconditional mints deliberately exclude it), and a null there is Fatal. D-K2 gains a FOURTH row: **bit 10 requires
  bit 11**. Sites: PipeFill.cpp:1062-1069's precedent (contract, folded into c0c), Managers.cpp:2393-2410's mirror in
  the texture family's Resolve*SubsystemArm (package D's rework), and package F's dependency-refusal scenario gains
  the 0x5ff-shaped arm. Client-side minting of the built-in sampler is REJECTED (it would run the content-addressed
  cache in the arm meant to exclude it and crosses the B/C boundary).
- esprytdraw v1 (98bdfa3e..0b00f7f0, five commits, DirectGLES.cpp only): every arm green on its tree because its
  handle arms DECLINE TO THE PRE-HANDLE ARM when the applier holds no record (a serial is never an "emitted" test -
  MGPipeApplierReset advances every P4a serial unconditionally). On the integrated tree that shape is the silent
  fallback P3a's lesson forbids: a missing record must be a LOUD refusal (MGLOG_E_ONCE + decline to draw nothing /
  Fatal per family), never a quiet legacy read. The review judges every decline site; E's verification round flips
  them once B/C records exist. Its closing of D-E3's READ-attachment gap from SyncNeccessaryTextures may make G9's
  red-before obtainable (R1 of the gates review) - the E review says whether.
- ID-15 correction on WHERE the fourth D-K2 row lives: the precedent is SERVER-side (Managers.cpp ~2426, Espryt's
  ResolveVertexInputSubsystemArm refuses bit 8 without bit 7 and runs the legacy arm; PipeFill.cpp ~1585 only
  documents that the client's calls then land in RefusedResourceCalls). So "bit 10 requires bit 11" is implemented
  in package D's texture-family Resolve*SubsystemArm (its rework), pinned by package F's dependency-refusal scenario
  (a 0x5ff-shaped arm), and recorded in the brief/docs D-K2 table. c0c's scope is unchanged (no contract code).

## ID-16 gates review v1 = ACCEPT WITH MAJORS (six, all local); gates rework v2 launched
F-M1/M2/M3 G7 (`scripts/p4a_descriptor_negative_control.sh`): the borderColorForm patch writes 0 into a scoped enum
(never compiles), the patcher's print pollutes the captured verdict, PATCHED_HEADER is set inside a command
substitution so repair() on EXIT/INT/TERM is a no-op (a SIGINT leaves the patch in the tree) - all hidden because the
script exits at step 0 today. F-M4 the composite leak case reads HighWater(ShaderCso) which c0b redefined; use
`PeekPipeCompositeSlotHighWater` (contract-v2 §4.3/§7.6). F-M5 (R2) the ABA-flip probe needs both regexes in ONE
file - make it directory-wide so the flip cannot be forgotten. F-M6 the 0x9ff refusal matches lowercase "sampler"
anywhere in the log - match the exact refusal line. R1 partial: the TextureParamsWithoutASamplerView net catches
emitted-but-not-applied, not deferred-to-first-view; package D's verification-round probe must cover the second.
G5 (`p4a_untouched_regions.sh`) survived four attacks incl. a tail-perturbation probe - accepted as is. Rework v2
also: rebase onto refs/heads/feat/disaggregated, add the ID-15 0x5ff-shaped arm (bit 10 without bit 11 must be
refused by Espryt once D's rework lands - the scenario is written now, expected to SKIP-visibly until then), keep
R3 (four commits fine).

## ID-17 c0c integrated (pipe = `17db7598`, post-merge checks running); clientsp review v1 = REWORK; c0d for Tracker.h
- clientsp C-M1 (CompositeResolver.h:138/104): Reset() clears Live and the reuse branch returns before restoring
  it, so after any make-current ReleaseEntry early-returns forever - fix + a test that drives release THROUGH the
  resolver after a Reset. C-M2 (ProgramEmit.h:168 vs :200): the (Cso, Version) latch for the default uniform block is
  never invalidated when a re-issued create_shader_state clears the applier's block (wire W6) - a failed relink of a
  bound program leaves a zeroed block; invalidate the latch on every re-issue. C-M3 (SamplerEmit.h:299): LRU eviction
  invalidates sampler CSO handles that texture params records still name, and nothing re-emits. RULING: the cache
  keeps a REFERENCE COUNT per entry (B's Acquire for a texture's built-in sampler takes a ref; texture death or a
  parameter change that re-acquires releases it; bound sampler states hold one while bound); LRU evicts only
  unreferenced entries; when every entry is pinned the cache mints beyond 256 and counts it (OverCapacityMints, a
  recorded number, not a gate). B's rework uses Acquire/Release exactly as C's v2 report spells them.
- Two cross-package under-fires the review found in A's Tracker.h: (1) glBindSampler bumps TextureBindGeneration
  (bit 12) not SamplingResolutionGeneration (bit 13), so bind_sampler_states never fires for a sampler bind; (2) bits
  6/7/8 read GetCurrentProgram(), null under SSO, so a re-composited pipeline never gets a ShaderCso handle. Both are
  contract defects -> c0d on p4a/contract (Tracker.h + an A-owned unit test), integrated before wire v2.
- Ten minors + 15 deviations judged in the review; the c0b reconciliation list (3 missing entry points, 4 latch
  calls, 2 stale comments) is part of the rework. clientsp rework rebases onto pipe (17db7598 + c0d when it lands).

## ID-18 clientfb review v1 = REWORK (four majors, nine minors); two of them are wire-side rules; B's rework waits for wire v2
- M1 (TextureEmit.h:939): m_regions is built and counted but never passed - wire W1's defaulted `const MGPSubRegion*`
  tail must receive it; every scattered upload otherwise declares N regions and supplies none. Fix + a unit case
  that asserts the applier's stored region list equals the emitted one.
- M2: SamplerObject::BumpVersion (the 13th MGP_NOTE_AGGREGATE(TextureParams) site) has no hook, so MinLod/MaxLod/
  LodBias go stale on glTexParameterf; it is ALSO the re-emit hook ID-14/ID-17 need when the built-in sampler's
  content changes (re-Acquire from C's cache, Release the old handle, re-emit set_texture_params). The GL_Texture.cpp
  grant exists for this - B takes it.
- M3: the client clears its dirty flag on DISPATCH, not on ACCEPTANCE (D-D5 step 1), even when the if-constexpr
  discarded the call. RULING: the applier returns acceptance. wire v2 was asked to implement m4 "if small, else
  declare"; if v2 declares, a wire v3 adds `Bool` acceptance returns to MGPipeApplyResourceSubData / Respecify /
  Create (default-tail-compatible; the wire path is untouched because gen_pipe never parses PipeApply.h - wire
  review W1). B clears a level's flags only when the call returned accepted.
- M4: the sticky BindMask/ImageBindableHint reach the applier only on a LATER respecify, which an immutable texture
  never has. RULING: a mask change after allocation emits a resource_respecify whose storage-defining fields equal
  the stored descriptor; the applier treats such a record as a METADATA UPDATE - no reallocation ack, no
  PendingUploads clear (this refines C1's level-scoped rule: identical storage fields clear nothing) - and Espryt's
  twin re-derives the storage flags from the new mask on its next sync (recreate only where the backend needs it).
  Wire side = the same v3 (or v2 if the agent read this in time); Espryt side = D's verification round.
- Nine minors: cube face resolved to targets[0], draw-buffer tokens >= 8 escape D-C3's refusal (must refuse), a
  dangling Entry&, unpublished entries never retired so a recycled slot inherits the mask (must retire) - fix all
  four; declare the rest with reasons. D4's blocked flip verified: kMGPipeWiredTextureSubsystem flips to 1 in the
  rework (one line) with the named expected-failing set re-run. The rework rebases onto pipe AFTER wire v2 is
  integrated (it builds against the applier's tail and acceptance returns); c0c's helper deletions + TextureTarget
  fill and ID-14/ID-17's Acquire/Release calls (clientsp-v2.md spells the API) are in the same round.

## ID-19 esprytobj review v1 = REWORK (1 critical, 4 majors); esprytdraw review v1 = REWORK (3 majors + 1 for D); the framebuffer record becomes PER OBJECT
- CRITICAL (esprytobj, Managers.cpp:8216/8450): the applier holds only the two BOUND-target records (wire
  DrawFramebuffer/ReadFramebuffer) and B emits only for bound slots, so on the handle arm every DSA entry point
  (BlitNamedFramebuffer, the four ClearNamedFramebuffer*) blits or clears into a driver FBO that never got its
  attachments; SyncToBackend binds and returns on a mismatch. RULING (design, the phase's main correction):
  the framebuffer record is keyed by the FRAMEBUFFER HANDLE. (a) c0e (contract, after c0d in the same tree):
  `MGPipeFramebufferTarget::Named = 3` = "this record describes the framebuffer it names; no binding changes",
  comment on MGPFramebufferState stating the per-object table and that Draw/Read/Both records ALSO set the bound
  handle(s); D-K2 fourth row and G9-as-white-box recorded in the brief/docs there too. (b) wire v3:
  `MGPipeApplier().FramebufferRecords` slot-indexed by the handle's slot (gen checked on lookup - stale gen refuses;
  framebuffers have no wire lifetime, D-I2, so a slot is simply overwritten by its successor's record),
  `MGPipeApplier().BoundFramebuffer[Draw|Read]` = handles; set_framebuffer_state Draw/Read/Both writes the record at
  Fbo's slot and the bound handle(s); Named writes the record only; the FramebufferSerial advances on every write;
  ReleaseObjectRecords clears the table and the bound handles (wire m1). `DrawFramebuffer`/`ReadFramebuffer` become
  accessors resolving through the bound handle so E's SyncCurrentFBOByRecord keeps its shape. (c) B v2: emit a
  Named record at every DSA entry point that hands a framebuffer to Espryt by name (Blit/ClearNamed*, and the
  DSA attachment/drawbuffer/readbuffer setters at their validate point) - "any framebuffer Espryt is about to
  receive by name has a record". (d) D v2: PushedFramebufferRecord(target, fbo) -> FramebufferRecordFor(handle);
  the bound-target question is answered by BoundFramebuffer[t] == handle.
- esprytobj majors: M-1 the server's own re-dirty (RequireImageBindableStorage, :5127) is invisible to the pending
  set -> the twin re-arms the record's PendingUploads itself (server-side state); M-2 pushedStorage->Desc has zero
  readers - d2 must allocate storage FROM the descriptor (that is the step's purpose); M-3 SyncAttachmentObject
  (:7947) is D's file -> D does the attachment resolution incl. the TextureTarget masks (ID-12 DV-5); M-4 the two
  registry methods and AdoptTwinByHandle with no caller are removed or wired. Ten minors fixed or declared. DV-9's
  wording corrected (38 non-semantic preprocessed-line differences: __LINE__, parens, reflow - accepted).
- esprytdraw majors (E v2, launched now, rebased onto pipe): (1) SyncImageTextureBindings:2527 narrows the DRIVER
  sweep to the record's window and returns, so a unit outside it keeps a deleted texture name - sweep the full
  driver range; (2) BindCurrentUnitSamplers:4361 `continue`s outside the window instead of unbinding - restore the
  `else UnbindSampler`; (3) the image (:2369) and program (:3699) seam checks are silent - log the same line the
  framebuffer one logs; (4->D) InvalidateFramebufferHandleArmMemos belongs inside InvalidateFramebufferBindingCache.
  Decline sites: 27 classified; the two SILENT ones (SyncCurrentFBOByRecord:2709 half-described applier;
  ResolveGlobalConstantsRecord:3701 short block image falling back to MapUBO = protocol corruption) become loud
  NOW; the rest flip per the review's table at E's verification round on the integrated tree.
- G9: not obtainable through public GL (readback emulation sets DEPTH_STENCIL_TEXTURE_MODE itself; IsDrawSyncClean
  pushes params on first sample) -> F's G9 becomes a WHITE-BOX assertion (gates v3 / F's verification round).
- Order now: c0d -> c0e -> wire v3 (v2 + table + acceptance returns + metadata respecify) -> B v2 -> C v2 (running)
  -> D v2 (needs wire v3's table to build) -> E verification -> D verification -> F v3.

## ID-20 c0c integrated and pushed: feat/disaggregated = `17db7598` (origin)
Post-merge on pipe: G1 0/0/0/0, unit 1638 x 2 (pull/push), P3a's eleven byte-identical, names pull == push 2604,
0 removed / +16 vs baseline. Tool note: the post-merge name comparison needs LC_ALL=C on both sides (the baseline
was sorted under a different locale; the unfixed comm reported 236/252 - a false alarm). Detached WSL jobs must be
launched `setsid nohup bash <script> < /dev/null > log 2>&1 & disown` and verified with pgrep - with only stdout
redirected the job died before writing a line.

## ID-21 wire v2 landed (`6f263284`, five commits, rebased on 2cb44039); wire v3 + c0e launched in parallel
wire v2: C1 fixed as specified (trailing defaulted `const MGPRespecifiedLevel*`, null = whole resource); m4 done -
`MGPipeApplyResourceSubData` returns Bool; m1/m2/m3/m5 fixed; unit 1673 x 3, applier suites 125/125, names +51,
five mutations each red on its own case. m3's packed-Target decode is LOCAL (c0c had not landed) - retired at the
rebase onto 17db7598. wire v3 (same agent lineage, new round): rebase onto 17db7598; ID-19(b) the per-object
`FramebufferRecords` table + `BoundFramebuffer[Draw|Read]` + `DrawFramebuffer()/ReadFramebuffer()` accessors;
ID-18 M3 acceptance Bool on Respecify and Create too; ID-18 M4 metadata respecify (storage-defining fields equal
the stored descriptor -> no ack, no PendingUploads clear, masks updated). `Named` = 3 comes from c0e; until it
lands wire v3 accepts 3 through a declared local constant that wire's verification round retires. The wire v2
re-review is FOLDED into the v3 review (one review over v2+v3). c0e runs in its own worktree `~/w7/p4a-c0e`
(branch p4a/c0e from 17db7598) in parallel with c0d (Tracker.h, disjoint files); integration order c0d -> c0e
(rebased) -> wire v3.

## ID-22 c0d integrated: pipe = `9ea44389` (post-merge running, push follows); brief amendments recorded
c0d (Tracker.h + TrackerTest.cpp): bit 13's shutter now also reads TextureBindGeneration (a texture bind changes
the sampler-state set through the built-in sampler; the shutter, not the wants() gate, because the dirty-surface
table is derived from the shutter); an `else if (pipeline)` arm drives bits 6/7/8/14 from the pipeline name + per-
stage {lifetimeId, linkVersion} + the four mirror counters (fields read directly - the two helpers made the bits
UNRESOLVED and broke self-test control 6b). Both UNDECIDED rows stay marked. Gates: symbols 0/0/0/0, unit 1642 x 3,
names +20, dirty-surface self-test 27 controls, integration-gpu DirectGLES 491 identical before/after.
Brief amendments (appended to BRIEF-P4A.md as section F): D-K2 fourth row "bit 10 requires bit 11" (server-side,
D's resolver; F's 0x5ff arm); G9 is a WHITE-BOX assertion (public GL cannot observe the gap: the readback emulation
sets DEPTH_STENCIL_TEXTURE_MODE itself and IsDrawSyncClean pushes params on first sample); D-C2's "per bound target"
becomes "per framebuffer object + two bound handles, Named = 3" (ID-19); D-D5's "clear on dispatch" reads "clear on
ACCEPTANCE" (ID-18 M3); a mask-only respecify is a metadata update (ID-18 M4).
- ID-22 addendum: c0d post-merge on pipe G1 0/0/0/0, unit 1642 x 2, G5 rc 0, names 2608 == 2608, 0 removed / +20;
  pushed: origin feat/disaggregated = `9ea44389`.

## ID-23 esprytdraw v2 landed (`9b8441a6`, five rework commits, rebased on 17db7598)
MAJOR-1 sweep = [0, min(max(Start+Count, g_imageUnitHighWaterMark), unitCount)); MAJOR-2 UnbindSampler outside the
window; MAJOR-3 all three seams share the stem "does not describe the binding it names" (the image seam latches
to the validate point so the eager glBindImageTexture funnel does not spam - DEV-14, ACCEPTED); the two silent
sites: F2 loud when exactly one target is recorded, P7 Fatal{ProtocolCorruption} on poison/verify and loud-once
elsewhere (DEV-15, ACCEPTED - no unconditional abort on a shipped draw path); D-K2's fourth row is in E's arm
too (D's resolver stays the owner); a v1 pGLContext token in a comment fixed (G13 = 1 file). Gates: G1 0/0/0/0,
unit 1638 x 2, integration-gpu 491/491 on three arms, family 497/497 x 3, LEGACY_MEMOS=OFF build rc 0. The focused
re-review of v2 is FOLDED into E's verification-round review on the integrated tree (after B v2 / C v2 / D v2).

## ID-24 wire v3 landed (`70e742a3`); wire v2+v3 review, B v2 and D v2 launched on a TEMPORARY merged base
wire v3: FramebufferRecords (slot-indexed, gen-checked, StaleFramebufferRecordLookups), BoundFramebuffer[Draw|Read],
FramebufferRecordFor(handle) / DrawFramebuffer() / ReadFramebuffer() accessors, kMGPipeFramebufferTargetNamed = 3
local (retired by a static_assert when c0e lands), Bool on Create/Respecify, metadata respecify (W10: buffers
excluded, wider field set; W11 the level-pointer clause) - unit 1680 x 3, applier 130/130, names +58, nine
mutations red. Because B v2 and D v2 must build against wire's applier before wire is integrated, both were launched
on `refs/heads/feat/disaggregated` (9ea44389) + `git merge refs/heads/p4a/wire` (+ p4a/c0e when it exists) as a
TEMPORARY base; the integrator's rebase onto the pipe that contains wire/c0e drops those merge commits (identical
patch-ids are skipped). D v2 does NOT merge B/C (its verification round follows their integration). Integration
order stays c0e -> wire (with the Named-constant retirement edit done by the integrator in the wire tree after its
rebase, unit-tested, one commit) -> C v2 -> B v2 -> D v2 -> E verification -> D verification -> F v3.

## ID-25 c0e integrated: pipe = `e8502a61` (= 11446f35 rebased onto c0d; post-merge running, push follows)
c0e: `MGPipeFramebufferTarget::Named = 3` (Count 4), the per-object-table comment on the enum and MGPFramebufferState
(only Target is binding-specific; ReadSurface = that framebuffer's own read buffer under every Target), the
metadata-respecify note beside MGPipeResourceRespecifyNeedsAck naming the storage-defining set (metadata = BindMask,
ImageBindableHint, GlNameForDiag; HasDefinedContent is storage-defining). Sizes 304 / 88 unchanged; G1 0/0/0/0;
unit 1638 x 2 on its base. The contract is now c0..c0e; the brief's section F carries the docs half.
- ID-25 addendum: post-merge on pipe G1 0/0/0/0, unit 1642 x 2, G5 rc 0, names 2608 == 2608, 0 removed / +20;
  pushed: origin feat/disaggregated = `e8502a61`.

## ID-26 clientsp v2 landed (`00a86d2a`, three rework commits on 17db7598); focused re-review launched
C-M1 Live/Fresh split in the composite resolver; C-M2 the constants-key latch invalidated on re-issue + a counted
refusal in the program emitter; C-M3 per ID-17 ref-counted cache. B-facing API (clientsp-v2.md): `#include
<MG_Impl/Pipe/SamplerEmit.h>`; `MGPipeSamplerCsoCacheInstance().Acquire(sampler->GetAllSamplerParameters(), bytes)`
(bytes is a Uint64& lvalue) -> handle + ref; `.Release(previous)` on re-acquire / texture death; B deletes its
`MGPipeSlots().Acquire(SamplerCso, ...)`. Gates on its base: G1 0/0/0/0, unit 1668 x 3, names +46, emit suites
34/34, integration-gpu 966/966, verify 844/844, retrace 79/79, four negative controls red. C does not contain c0d
(9ea44389) or c0e - the rebase at integration brings them; the re-review names the tests to re-run.

## ID-27 wire review v2 = ACCEPT WITH MAJORS (merge not blocked); wire integrating with the Named-constant retirement
Verified by the review: C1's three-arm clear (metadata -> whole-resource -> keyed erase, precedence right), the
per-object table + gen check ({0,1} admitted, {0,0} faults), acceptance Bools on every path with counters intact,
M4's field set == c0e's ruling, m1/m2/m3/m5 fixed, no bare packed decodes, no new static holders; three mutations
re-killed plus two of the reviewer's own. RULINGS: MAJOR-1 (stored State.Target is now the LAST emission's target,
Named included, so E's `record.Target == Both` skip at DirectGLES.cpp:2983 degrades to a redundant per-frame sync
after a DSA Named record) -> E's verification round replaces that test with `BoundFramebuffer[Draw] ==
BoundFramebuffer[Read]` (the applier's own fact). MINOR-1 (the UploadTarget half of the level-scoped erase key has
no gate - deleting it is green on all 74 tests) -> wire's verification round adds the cube-face case. The coarse
FramebufferSerial memo key (no per-record serial) is RECORDED for the optimisation list (perf, not gated).
Integration: p4a/wire rebased onto e8502a61 (head 8f1eaafa before the retirement commit), the local
kMGPipeFramebufferTargetNamed replaced by MGPipeFramebufferTarget::Named everywhere incl. PipeApply.cpp:2148/2153
(`>= Count` refusal) and the three test sites; the integrator's retirement commit is unit-tested and G1-checked
in the wire tree before the fast-forward.

## ID-28 wire integrated and pushed: feat/disaggregated = `712c9467` (origin)
Twelve wire commits rebased onto e8502a61 + the integrator's retirement commit 712c9467. Post-merge on pipe: G1
0/0/0/0, unit 1684 x 2, P3a's eleven byte-identical, names 2650 == 2650, 0 removed / +62 vs baseline. The applier
is now real on pipe: B v2 / D v2 (built on the temporary merged base) rebase onto this head at integration - wire's
rebased commits carry the same patch-ids so the temporary merges drop out; if a rebase reports conflicts in
PipeApply.* the integrator resolves toward pipe. Next: C v2 (after its re-review) -> B v2 -> D v2.

## ID-29 gates v2 landed (`ecf9cb45`, four rework commits on 2cb44039); focused re-review launched
All six majors proved on a scaffold: G7 exits 0 naming Layered and borderColorForm ({} not 0; verdict via a global);
SIGINT mid-rebuild -> repair ran, exit 130, tree clean; the composite leak case reads PeekPipeCompositeSlot
{HighWater,LiveCount,BandBase} and went red on all three live-allocator lanes when one composite leaked; the ABA
probe is directory-wide; the 0x9ff refusal matches one ERROR line naming both bits; the 0x5ff arm SKIPs by name.
Twelve minors fixed, F-m13 declared. Gates on its base: G1 0/0/0/0, unit 1636 x 3, names 2711 == 2711, +123,
p4a_untouched 17 rows + 8 controls, integration-gpu 1075/1075 both builds. NOT rebased on 712c9467 (moved mid-
round); the re-review reports the rebase; F integrates LAST (after D v2) and F v3 adds the white-box G9 (ID-19)
and wire's cube-face case is wire's own.

## ID-30 clientsp re-review v2 = REWORK (one item, demonstrated); C v3 launched on 712c9467
Every v1 item closed (C-M2 the one-line invalidation; C-M3 exactly ID-17 - one reference per binding, ABA closed;
all eight c0b items at the source; C-m7's refutation upheld). NEW C2-M1: the composite resolver is a process
singleton keyed on the pipeline GL NAME, but names are per context (Core.h:658-659), so a make-current between two
contexts holding the same name makes Observe emit delete_shader_state, clear the latch and free the OTHER
context's live composite - probe red on v2, green with v1's Reset() restored; c0d makes it reachable. RULING: key
the entry on (context identity, name) using the SAME context identity the rest of the pipe client already uses
(no second notion); a destroyed context's entries released exactly once; the probe becomes a unit case + mutation.
Three minors fixed in the same round. This is C's second rework (the limit); C integrates after v3 on the gates'
strength plus the probe - no third review round unless v3 changes shape.
- CI: Test lane GREEN on 2cb44039 (c0b) and on 17db7598 (c0c, two runs); 9ea44389 / e8502a61 / 712c9467 queued
  behind the runner backlog (monitor re-armed).

## ID-31 gates re-review v2 = ACCEPT WITH MINORS; F integrates last, F v3 at that turn
Re-proved by the reviewer: G7 tripped + exit 0; SIGINT -> repaired, tree clean, died by signal; the child>=128
path exits 2; one leaked band slot -> 3 failed of 8 on exactly the three live-allocator lanes (983042->983090);
the ABA conjunction directory-wide with no false positive on any in-flight tree; the F-M6 matcher matches Espryt
v2's exact sentence and rejects four near-misses; G5 17 rows + 8 controls rc 0 also on 712c9467; G14 0 removed /
+123; rebase onto 712c9467 clean (8/8, zero overlap; built: unit 1684, P4a families 165/165, ctest -N 2759).
F v3 owes: ID-19's white-box G9 (shape in the review), F-v2-m1 the symmetric refusal matcher (roles-swapped claim
unchecked), F-v2-m2 the 0x5ff arm goes GREEN not red at integration (esprytobj v2 already carries the row - fix the
wording), F-v2-m3 MagmaPipeAbaControlCoversKind uncalled, F-v2-m4 REPAIR_RC write-only, and a re-take of section 6
on the integrated tree.

## ID-32 esprytobj v2 landed (`ef6beb28`, seven rework commits on a temporary base); focused re-review launched
Base = 9ea44389 + merge p4a/wire 70e742a3 + merge p4a/c0e 11446f35 + TEMPORARY commit cce81d19 (retires wire's
stand-in Named constant so the merged base compiles) - the integrator drops all of it with
`git rebase --onto refs/heads/feat/disaggregated cce81d19` (replays only the seven rework commits onto 712c9467,
which already carries the retirement). Closed: the CRITICAL (FramebufferRecordFor + BoundFramebuffer[], refusal
before the bind), M-1 (server re-dirty arms the applier's PendingUploads), M-2 (storage from Desc + metadata
respecify), M-3/M-4 (attachments + four masks from MGPSurface; AdoptTwinByHandle wired; ReleaseByHandle removed),
ID-12, ID-15's row, E's item (4) handshake, the white-box G9 probe (green, red under mutation), ten minors. Gates:
G1 0/0/0/0 at all eight commits, unit 1686 x 2, D-N scripts rc 0, DirectVulkan 475/475, default arm 189/491 =
183 named refusals + 6 armless (0x1ff pin) - the pre-client shape; 0x1ff and 0 arms 6/491 identical sets.

## ID-33 clientsp v3 landed (`92dffb81`) and integrated: pipe = `92dffb81` (post-merge running, push follows)
C v3 on 712c9467 (four MG_Test/Pipe files conflicted with wire's ae1a1c50 - resolved as an overlap-checked union,
0 of wire's 20 cases lost): C2-M1 keyed on (ContextId = ctx.GetTextureContextId(), the tree's never-reused per-
context id, PipelineName); a destroyed context's entries released once via ~ProgramObject's death path, Reset()
sweeps stranded ones (counter Sweeps); Fresh/HandleFor deleted; minors C2-m2/m3, nits. NEW defect fixed in C's
four test files (79b58198): the forked refusal drives unlinked the log while Initialize() held it open (child
wrote a deleted inode) - 6 verify-build failures -> 0. Gates on 712c9467: G1 0/0/0/0, unit 1717 x 3, names
2683 == 2683 / +95, emit 57/57 x 3, integration-gpu 491/491 default + 0x1ff (966/966 whole label), verify
844/844 zero Fatal, retrace 79/79. Integrated per ID-30 without a third review round (v3 kept its shape).
- Tool note (ID-33): the integrator's one-off scripts in the shared scratchpad wsl dir were deleted by an agent's
  cleanup (only p4a_integrate_clientsp.sh survived). The post-merge check now lives durably as
  `~/w7/notes/tools/wsl_p4a_postmerge.sh` + `wsl_p4a_postmerge_bg.sh` (detached launcher); integrator scripts go
  in notes/tools from now on, the scratchpad is for agents.
- ID-33 addendum: post-merge on pipe G1 0/0/0/0, unit 1717 x 2, G5 rc 0, names 2683 == 2683, 0 removed / +95;
  pushed: origin feat/disaggregated = `92dffb81`. Next: B v2 (its result + focused review) -> D v2.

## ID-34 esprytobj re-review v2 = ACCEPT WITH MINORS; integration recipe corrected; two items owed at D's verification round
All v1 items and rulings closed (FramebufferRecordFor + refusal above Bind, no record.Target test anywhere; packed-
key re-arm decoded both directions; seven storage macros incl. buffer-texture/multisample; the four masks off
MGPSurface with the D-N shas identical; G9 probe green x4). ID-32's `rebase --onto cce81d19` is WRONG (it drops D's
six v1 commits, which sit below the temporary merges). CORRECT RECIPE (verified clean by the reviewer, builds, unit
1686 x 2, G1 0/0/0/0, 189/6/6 with identical arms, DirectVulkan 475/475): on a branch from pipe, `git cherry-pick
9ea44389..8338c18f` (D's six v1 commits as replayed on the temporary base) then `git cherry-pick cce81d19..ef6beb28`
(the seven rework commits). Owed before/at D's verification round: N-1 the refusal line's "left unbound" is false
(SyncAndBindFramebufferObject and BindCurrentFBO bind anyway - reword); N-2 the refusal census names six of eight
shapes - sampler (:12174) and renderbuffer (:12418) missing; the "zero refusals" check on the integrated tree must
count all EIGHT families. Seven further minors recorded for the round (AdoptTwinByHandle's refusals unreachable,
the remint-pull marker on the prevention path, raw SrcOffset, the silent empty-surface arm, high-byte-vs-packed key
asymmetry, unread HasDefinedContent, G9 in-function deferral only). D integrates AFTER B v2 (ID-2 order).

## ID-35 clientfb v2 landed (`d1215fbc`); B and D integrate BACK-TO-BACK, the push waits for D
B's temporary base = 9ea44389 + wire 70e742a3 + c0e 11446f35 + clientsp 00a86d2a (a fourth merge - the ID-14/ID-17
cache call cannot compile against the contract's SamplerEmit.h stub; accepted). All ten items done: M1 region
tail, M2 three GL_Texture.cpp hooks + both-versions latch, M3 acceptance-gated clears, M4 metadata respecify,
ID-19(c) 16 Named sites in GL_Framebuffer.cpp (granted per ID-8), c0b/c0c reconciliation, D4 flipped with an EMPTY
expected-failing set, 6 minors fixed / 3 declared. Gates: G1 0/0/0/0, unit 1756 x 2, names +134, emit 148/148,
integration-gpu 0x1ff and 0 = 491/491, default 444/491, retrace(0x1ff) 79/79. RULING on its finding (1): the 47
default-arm failures / 109 verify / retrace 0 at the default mask have one proved cause - bit 10's acceptance-
gated dirty clear with NO server consumer (Espryt twins are not on B's base; 0x3ff/0x9ff/0x11ff are 491/491 and
removing only the clear gives 491/491). That is the designed dependency, not a defect: D lands immediately after
B; pipe's post-merge after B runs G1/unit/G5/G14 only; the full lanes and the PUSH happen after D. Finding (2) the
6 verify-lane unit failures are wire's refusal-drive cases fixed by C v3 (79b58198), absent from B's base - expected
to vanish on 92dffb81 (the re-review checks). B's replay onto pipe = the D-shaped recipe (v1 commits as they sit
on the temporary base, then the rework commits, merges skipped); the re-review verifies it and records any
test-file union.

## ID-36 clientfb re-review v2 = ACCEPT WITH MINORS; B and D landing back-to-back (detached run)
Verified by replay onto 92dffb81: cherry-pick 5e87d3d2 5fac004a e2bf183a a390bb1a d013b7ec 4ec43194 d1215fbc,
SKIP 549ef30b (B's own retirement of wire's stand-in constant - already on pipe); one empty-base conflict in
e2bf183a (FramebufferEmitTest.cpp / TextureEmitTest.cpp) resolved as a union with B's block FIRST (theirs-first;
ours-first compiles but breaks 4ec43194); B's files byte-identical to d1215fbc afterwards. Lanes on the replayed
tree: unit 1759 x 3, emit 152/152, gpu default 444/491 (the 47 = B's appendix, D is the missing consumer - an
independent negative control confirmed no MG_Backend file reads PendingUploads/TextureResources), 0x1ff and 0x3ff
491/491; the six verify failures gone. Minors n1-n5 recorded (n2 framebuffer records latch on dispatch = wire's;
n3 sampler-CSO refs released only on slot recycle - D/E verification rounds watch the counters). Tooling:
`notes/tools/wsl_p4a_integrate_replay.sh <slug> <range>...` (+ UNION_THEIRS_FIRST=1 with
`union_theirs_first.py`), `wsl_p4a_land_bd.sh` chains B -> D -> post-merge into ~/w7/p4a-land-bd.log.

## ID-37 B replay: the theirs-first union was wrong for the second conflict; reconciled from d1215fbc instead
The replay hit e2bf183a (FramebufferEmitTest.cpp) AND 4ec43194 (TextureEmitTest.cpp) - not the reviewer's single
e2bf183a in both - and the blind union of the second duplicated a block (redefinition errors). Rule: after replaying
a client package whose test suites share a file with wire's cases, take the package's FINAL version of those files
(d1215fbc is a pure superset of pipe's: +817 / +566, zero deletions) and re-apply the Named-spelling retirement;
that is what `wsl_p4a_land_bd2.sh` does, as one reconciliation commit on top of the seven replayed ones, unit- and
suite-tested before the fast-forward. CI: Test lane GREEN on 92dffb81 (clientsp v3).

## ID-38 B and D landed: pipe = `17396216` (B = seven replayed + reconciliation 568d587a; D = thirteen replayed); quick gate running; NOT pushed yet
Post-merge on pipe: G1 0/0/0/0, unit 1761 x 2, P3a's eleven byte-identical, names 2727 == 2727, 0 removed / +139.
The push waits for the quick gate's verdict (the six armless `.Handles` aborts from the 0x1ff itest pins are F's
and stay until F lands - a known transient, so the decision is: push after F v3 lands unless the gate is otherwise
clean, in which case push now and let CI show the six). Verification rounds launched on the integrated tree:
E (p4a/esprytdraw rebased onto 17396216: the 27 decline-site flips, ID-27's BoundFramebuffer comparison, the
assumptions reconciled against D v2's real API, the family lanes, the eight-family refusal census) and D (new
worktree ~/w7/p4a-esprytobj3 on branch p4a/esprytobj3 from 17396216: N-1 wording, N-2 eight-family census, the
seven minors, the verification checklist, and the first real-path run whose default arm must reach ZERO named
refusals - a residual is a seam defect). F v3 (rebase, white-box G9, four minors, wire's cube-face erase-key case
under an integrator grant, section-6 re-take) follows E/D; then the final whole-diff review, the full gate, APKs,
device A/B, bench, docs.

## ID-39 quick gate on 17396216: push default arm 74/966 red = 66 DirectVulkan + 6 pinned HandleRecycle + 2 DirectGLES sampler seams; c0f launched
Static half clean (three builds rc 0, unit 1761 x 3, pull integration-gpu 966/966, emitter/CSO suites 166/166,
G2 0, G14 +139, dirty-surface 27 controls). Push arm at 0x1fff: 74 failed - ALL 66 DirectVulkan failures are
texture-upload-shaped (ImageTargetKind loads, CopyImage*, NonCoreImageFormat, TextureView, PackedWordReadback,
GuiBatch, LayeredAttachment*, ClearTexImage...). ROOT CAUSE (B's own finding (1) applied to Magma): the P4a texture
family has NO "a backend registered the consumer" gate - P3a's buffers have one (PipeFill.cpp ~656:
MGPipeResourceSubsystemEnabled() = bit 7 AND a backend registered MGPipeResourceOps; Magma registers none, so
buffers stay legacy there) - so with kMGPipeWiredTextureSubsystem = 1 the client emits, the applier ACCEPTS, the
client clears its dirty flags on acceptance, and Magma's legacy path finds nothing to upload. RULING (c0f, one
commit on a branch from 17396216 by a fix agent with an integrator grant on BOTH files): (a) PipeFill.cpp - the
four P4a families' enable predicates and wants() rows require bit N AND wired AND "a backend registered
MGPipeResourceOps" (the existing per-backend signal; the texture family rides the resource rows per D-D1, and the
framebuffer/sampler/program families name texture handles so they follow); no emission at all on a backend
without the consumer; (b) PipeApply.cpp - belt: every P4a-family entry point returns accepted = false and counts
RefusedNoConsumer when no ops are registered, so a stray record can never clear a client flag. Tests: a unit case
per half; the DirectVulkan lane must return to all-green. The two DirectGLES failures (SampledSetStaleness
...ASamplerObjectCompletesTheTexture, IntegerBorderColor ...SamplerParameterIivBorderColourSurvives...) are the
B/C/D built-in-sampler seam - the D and E verification rounds classify them. The six HandleRecycle are the 0x1ff
pins (F). No push until c0f + F land.
- ID-39 addendum (gate complete): 0x1ff / 0x7f / 0 arms = 6/966 (the pinned HandleRecycle aborts only, so every
  DirectVulkan case passes with bit 10 off - the diagnosis holds); ESPRYT_DISABLE_INVALIDATE_FLUSH=1 = the same 74
  (not flush-related); family regex 51/497 = the same DirectVulkan set; integration-verify 68/844 = the same 66 + 2
  with ZERO Fatal; G7 and P3a-G7 controls trip and recover; CSO/ResourceSubsystem/ObjectSubsystem controls 10/10;
  G9's scenario and p4a_untouched_regions.sh are F's (land with it). G13 lists MG_State names in PipeApply.h at
  :37/:40 (forward declarations) and :999-:1000 (LinkArtifacts/SpirvArtifacts pointers on the shader-state entry
  point, D-H3's "the server reads the frontend's archive") - both OUTSIDE the ops block; accepted.

## ID-40 c0f integrated: pipe = `29d51ab9` (post-merge running; still NOT pushed - F v3 next)
c0f (PipeFill.{h,cpp} + PipeApply.{h,cpp} + tests, one commit): `kMGPipeP4aFamilySubsystems` + `P4aFamilyHasItsConsumer()`
joined into FamilyIsLive / wants() / the DrainTextureSubData gate / the residual `supplied`; `MGPipeP4aFamilyEmits`
exported; the applier's `NoP4aConsumer()` counts `RefusedNoConsumer` at 15 entry points (silent - on Magma it is
the designed state); death paths not belted; g_resourceOps untouched by Reset/ReleaseObjectRecords so gate and
belt cannot disagree mid-frame. Gates on its base: symbols 0/0/0/0, unit 1762/1763, DirectVulkan 475/475 (from
409/475), DirectGLES 483/491 = the gate's exact eight unchanged. Mutations reported (walk-side conjuncts alone are
defence-in-depth). TextureEmitTest/FramebufferEmitTest register an empty ops table in main() (consumer-present arm).
E and D verification rounds run on 17396216-based trees and rebase onto this head at their integration (no file
overlap with c0f).
- ID-40 addendum: c0f's post-merge showed G2 pull 2728 != push 2729 - the no-consumer texture case lacked its
  pull-build skip twin (TextureEmitTest.cpp's MGL_TEXTURE_EMIT_CLIENT_TEST_LIST). Integrator commits 245daca0
  (twin added, but a nested-heredoc escape wrote a literal backslash-n and the PULL build of the suite did not
  compile - the fast-forward went through on a green push suite only) and ee31944b (the fix): both suites 34/34,
  the name present in both builds; pipe = `ee31944b`, post-merge re-running. Lesson: never edit sources through a
  quoted heredoc inside a quoted heredoc; write the editing script to its own file, and gate a fast-forward on
  BOTH builds' compile, not one.

## ID-41 D verification round landed: pipe = `588b2277` (p4a/esprytobj3 56453191 rebased onto ee31944b); c0g launched
On 17396216 the default arm reached 485/491 - the six pinned `.Handles` aborts only - and the EIGHT-family refusal
census is ZERO over 477 per-test logs (`notes/tools/wsl_p4a_refusal_census.sh`: the console sink is compiled out
and the file sink truncates, so the census runs one ctest per name with its own log); StaleFramebufferRecordLookups
0; G9 4/4; verify zero Fatal; DirectVulkan still 409/475 on that tree = c0f, already on pipe. Seam defects: S-1
FIXED in D (BackendSamplerObject::SyncToBackend resolved the record by the twin's IDENTITY handle while C content-
addresses - every bound sampler's params refused, the two DirectGLES sampler scenarios of ID-39) - D now takes the
handle from the caller (push-only defaulted parameter, G1-safe) with a loud fallback; E owes ONE LINE at
DirectGLES.cpp:4021 (pass the handle, `#if MOBILEGL_PIPE_PUSH` guarded) - proved to close it, reverted, applied at
E's integration if E's round did not. S-2 = c0f (landed). S-3 (contract, c0g now): D-K2's fourth row refuses on
the SERVER but the client already cleared its flags, so 0x7ff = 438/491 - the client's texture family gate must
require every D-K2 dependency bit of the family (bit 11 and bit 7) as well as the consumer; generalised: a family
is live on the client only if all of its dependency bits are set (PipeFill.cpp ~885). S-4 fixed (ParamsSerial == 0
read as a malformed null BuiltinSampler). Follow-up recorded: G4's retain-mode comparison cannot run -
PipeTexelRetainMb has no reader (post-P4a item).

## ID-42 c0g integrated (post-merge running): the client's D-K2 dependency table
c0g (`29bd4d2c` on p4a/c0f, rebased on 588b2277): `kMGPipeP4aFamilyDependencies` (bit 9 -> 10; bit 10 -> 7|11;
bit 11 -> 10; bit 12 -> none; NON-transitive, mirroring Espryt's resolvers bit for bit; mirror pairs stated;
five static_asserts) consulted at c0f's four sites; every mask arm (0x7ff 438 -> 485, 0x1fff/0x3ff/0x5ff/0x9ff/
0xdff all 485/491 with the identical pinned-six list) and DirectVulkan 475/475; unit 1766 x 2, names 2732 == 2732;
M2 mutation fails to compile (static_assert). NOTE: TextureScope/FramebufferScope fixtures were arming masks
D-K2 forbids (no bit 7) - both now arm kMGPipeSubsystemResources.
- ID-42 addendum: post-merge on pipe 29bd4d2c G1 0/0/0/0, unit 1766 x 2, G5 rc 0, names 2732 == 2732, 0 removed /
  +144. Still unpushed (E round + F v3 pending). (A comment-condensing request was queued here and then
  CANCELLED by the user.)

## ID-43 E verification round landed on p4a/esprytdraw (`0cd0d1ec`, four commits on ee31944b); landing with two integrator patches; fable seam audit launched
E: A4 + wire v3 accessors, ID-27's bound-handle comparison (no `record.Target` reader left), F2/F4/F5/F7 loud, the
remaining flips (T2/I2/S2/P2/P3/P4/P6; T2/S2 scoped to draws touching a unit). Default arm 483/491 on ee31944b
= the pinned six + the two sampler scenarios (fixed by D3's S-1 once E's line lands), 0x7ff 438 (= c0g),
DirectVulkan 475/475, verify 842/844 zero Fatal, unit 1763 x 3, G1 0/0/0/0, both untouched scripts rc 0.
Findings: SD-0 (E's own regression, bisected to commit e3 and to the `Level` field): NewShaderImages' shutter
mixes only textureContent/textureParams/programImages, none of which moves when glBindImageTexture re-binds
the same texture with a different level, so the applier keeps the previous bind's Level and e3 (the first reader
of the field) paints the wrong mip (create-indirect ssim 0.887) - VERIFIED four-line patch for Tracker.h (mix
ctx.GetTextureBindGeneration() in, the NewSamplers shape) - applied by the integrator (contract grant) together
with S-1's DirectGLES.cpp:4021 call-site line (`wsl_p4a_land_e.sh` / `p4a_e_patches.py`, one commit on top of
E's rebased four, gated on both builds + unit + the two sampler scenarios + the create-indirect retrace + the
dirty-surface check). SD-1/SD-3 ("C never emits create_sampler_state"; built-in sampler CSO refuses x3) = the
S-1 seam as seen from E's base (no D3 there) - re-checked after landing. SD-4: set_shader_images never emitted
for imageBuffer x5 -> C's (final-review fix list). SD-5 = c0g. The ctest -V census is a FALSE ZERO (console
sink compiled out) - D's per-test-log tool is the valid one, and its phrase list is short by two shapes whose
sentence straddles a string-literal line break (:7584, :7895) - fix in the census tool. Handed up: retrace
62/79 at the default mask WITHOUT E on ee31944b (17 Iris shader-pack traces wrong, no refusal) - D3's S-1 is
the prime suspect (shader packs are sampler-heavy); the fable seam audit (user rule 2026-09-08: too many
reworks -> a fable research agent) verifies on the landed head.
- TRAP (E, cost two runs): `git lfs checkout` is DESTRUCTIVE on this box when the LFS content is not local - it
  replaces every real fixture with its 130-byte pointer (retrace 1/79, ssim=None). The ONLY safe recipe is
  wsl_tree.sh's: copy from ~/w7/pipe/tools/trace_replay/fixtures/ + `git update-index --assume-unchanged`; and
  `git reset --hard` undoes assume-unchanged, so re-copy immediately before every retrace. ID-13's instruction
  to reviewers is WITHDRAWN.
- ID-43 addendum, USER RULE (2026-09-08, binding for every agent): GitHub LFS can hit its quota - do not pull LFS at
  all unless a fixture is genuinely needed; when it is, FIRST copy from the hydrated local copy
  (~/w7/pipe/tools/trace_replay/fixtures/ + `git update-index --assume-unchanged`, wsl_tree.sh's recipe), SECOND
  use the CI script `.github/scripts/fetch-trace-fixture-lfs.sh <case> [dir]` (mirrors git.hit.moe then
  repo.miawa.cn before any LFS fallback). `git lfs checkout` / `git lfs pull` are forbidden in package and review
  trees. Pointer test: the first 80 bytes start with `version https://git-lfs.github.com/spec/v1`.

## ID-44 E landed with the two integrator patches: pipe = `41b79050` (E's four rebased on 29bd4d2c + 41b79050)
The patch commit (S-1's DirectGLES.cpp call site under `#if MOBILEGL_PIPE_PUSH`, passing
MGPipeApplier().BoundSamplerStates[unit]; SD-0's NewShaderImages shutter mixing ctx.GetTextureBindGeneration()):
dirty-surface --check rc 0 (no table change), G1 0/0/0/0, unit 1766 x 2, the two sampler scenarios' suites 16/16,
create-indirect retrace 2/2 (was ssim 0.887 on E's tree). Post-merge on pipe: G1 0, unit 1766 x 2, G5 rc 0, names
2732 == 2732, 0 removed / +144. Remaining before the push: F v3 (pins, G9 white-box, minors) + the full gate with
the three retrace sweeps on the final tree; the fable seam audit feeds the final whole-diff review.
- USER RULE (2026-09-08): a package that needs more than two rework rounds goes to a fable agent (a read-only
  root-cause study or the round itself), not another opus rework. In P4a that threshold was crossed by the
  contract (c0b..c0g) - the fable seam audit is the response; any package needing a v4 goes to fable directly.

## ID-45 fable seam audit (fable-seam-audit.md): four seams to fix before close, three recorded; Iris = S-1, now 20/20; fable fix round launched
Proven (class 6 = a setter changes a record field no emitter-read shutter sees; class 2 = identity vs content):
F-3 set_framebuffer_state inlines each attachment's InternalFormat at emission (FramebufferEmit.h:115-144); a
texture/renderbuffer respecify WHILE ATTACHED moves TextureContent/TextureParams, never bit 9's aggregate, so D's
four cross-object masks (Managers.cpp:8678-8723, :9328-9353) read a stale surface while the legacy arm reads the
frontend at the same re-sync - public GL: glTexImage2D(RGB8) -> attach -> draw -> glTexImage2D(RGBA8) -> alpha
diverges; no scenario. F-1/F-1b bit 12's shutter (Tracker.h:494) is textureContent xor bindGen but EmitSamplerViews
resolves the set for the CURRENT PROGRAM (SamplerEmit.h:742-767): a glUseProgram alone never re-emits; E's epoch
(DirectGLES.cpp:1868) keys the texture sync list on it -> an empty-slot bind under P1 then a switch to P2 leaves
the texture never synced, no refusal. F-4 BindCurrentUnitSamplers' record arm FindByHandle(csoHandle) on the
identity-keyed twin registry (DirectGLES.cpp:4847) never finds - latent until the pre-handle program pass is
retired (S-1 one loop over). F-2 bit 14's plain-program arm mixes a per-program counter without the lifetime id
(program switch invisible; loud-ish fallback). Recorded for later: SD-5's non-transitive dependency table on both
sides, three encodings living only in package headers, the latch-before-acceptance of set_texture_params.
Iris: bisect ee31944b 3/20 -> 1e8cdc59 (D3's S-1) 20/20; every failing log carried "sampler N has no applier
record" - the backend-minted raw-depth-fetch sampler refused by the identity lookup; E's "zero refusals" was
itest-only. 41b79050: 20/20, zero refusal lines. Brief root causes: D-K4's "shutters are already computed" (no
record-field -> setter -> shutter table), no per-kind handle-rule table for BOTH sides, dependency rules stated
about masks not emissions, no census recipe for retrace runs -> the P4b brief states all four up front.
Per the user's rule (rework > 2 -> fable): ONE fable fix agent takes F-1/F-1b, F-2, F-3, F-4 + E's SD-4
(imageBuffer set_shader_images) + the census tool's two missing phrases, on a branch from 41b79050, with an
integrator grant on Tracker.h / SamplerEmit.h / ImageEmit.h / FramebufferEmit.h / DirectGLES.cpp / Managers.cpp
and a NEW scenario file (to stay clear of F v3's files). The final whole-diff review will be fable as well.

## ID-46 gates v3 landed: pipe = `4678519f` (15 commits on 41b79050; post-merge running); fable fix round launched from it
F v3: 0x1fff pins (the six `.Handles` aborts closed), G9's white-box half (new Harness/PipeApplyPeek.{h,cpp};
mutation 4/4 red), wire MINOR-1's cube-face case, c0f's belt pinned on both backends, F-v2-m1..m4. On 41b79050:
integration-gpu 1079/1079 at 0x1fff / 0x1ff / 0 and pull (DirectVulkan 540/540, DirectGLES 539/539), verify
884/884 zero Fatal, unit 1767 x 3, G1 0/0/0/0, G2 2846 == 2846, G14 +258, G5 17 rows + 8 controls, all three
negative controls rc 0, HandleRecycle 144/180/144 + .Handles 36/36, ObjectSubsystemControl 14/14. Four defects
the gates caught during the round were fixed upstream already (0x5ff emit disagreement = c0g, the G2-breaking
c0f name = ee31944b, the two-case-wider sampler seam = S-1, D-E3's read-attachment gap = E). The fable fix round
(p4a/fablefix from 4678519f: F-1/F-1b, F-2, F-3, F-4, SD-4, the census phrases, the shutter table comment) is the
last code round; then the full gate with the three retrace sweeps, the push, the fable whole-diff review, APKs,
device A/B, bench, docs.
- ID-46 addendum: post-merge on pipe 4678519f G1 0/0/0/0, unit 1767 x 2, G5 rc 0, names 2846 == 2846, 0 removed /
  +258. Worktrees left: p4a-c0f (c0g's branch), p4a-esprytdraw, p4a-gates, p4a-fablefix (in flight).
- USER (2026-09-08): after P4a closes (gate, push, final review, APKs, device A/B, bench, docs) STOP - do not start P5.

## ID-47 fable fix round landed: pipe = `6035c9d7` = the P4a CODE HEAD; full gate + fable whole-diff review + Release APK build launched in parallel
fablefix (seven commits from 4678519f): F-3 the framebuffer aggregate bumped from PipePublishDescriptor (texture +
renderbuffer, no counter added) + the pre-handle arm's in-place regeneration / renderbuffer re-storage bump compiled
PUSH-ONLY (G1 caught the pull-side version as 0/0/2/0 - the pre-P4a monolith hole stays in the pull build and is
RECOMMENDED AS A SEPARATE dev COMMIT after P4a; the texture cases decline by name there, the renderbuffer case
asserts on the handle arm only); F-1/F-1b bit 12 mixes program identity x link x backend state (pipeline-aware) +
the params aggregate (F-1b reproduced black 144/144, closed without widening E's epoch); F-2/SD-4 bit 14's plain
arm mixes lifetime id x link version (SD-4 was this, not reflection); F-4 ResolveSamplerCsoTwin on the content-
addressed record shared by the record arm and the program pass; the record-field -> setter -> shutter table in
Tracker.h; the census tool's two straddling shapes. On 6035c9d7: G1 0/0/0/0, both untouched scripts rc 0 (+8
controls), gen_pipe / dirty-surface (27 controls) / include closure rc 0, unit 1772 x 3, G2 2863 == 2863, G14
+275, integration-gpu 1091/1091 at 0x1fff / 0x1ff / 0 / pull, DirectVulkan 546/546, verify 896/896 zero Fatal,
retrace 79/79, retrace census 0 on 13 needles, itest census 0. Deviations accepted: its own peek TU (second CMake
line, F's file untouched); seven commits. USER: after P4a closes, STOP (no P5).

## ID-48 full gate on 6035c9d7 green through verify; APKs built; device A/B running; the Oppo install flow fixed (pipe = `a38bdab4`, tools only)
Gate so far: builds x3 rc 0; G1 0/0/0/0; G5 P3a eleven + P4a 17 rows (8 controls) rc 0; HandleRecycle (verify)
180/180; emitter/CSO suites 172/172; G2 0; G14 +275; unit 1772 x 3; pull integration-gpu 1091/1091; push at
0x1fff / 0 / 0x1ff / 0x7f / ESPRYT_DISABLE_INVALIDATE_FLUSH=1 all 1091/1091 (the "rerun 53" lines are a stale
LastTestsFailed.log - nothing failed on the first pass); family 497/497; G9 8/8; TextureUploadShape 3/3
(recorded); three negative controls rc 0 (G7 exit 0 naming Layered + borderColorForm); CSO/Resource/
ObjectSubsystemControl 24/24 + 0x9ff arm 14/14; controls 184/184; integration-verify 896/896, zero Fatal. The
three retrace sweeps are running. APKs: ~/w7/notes/p4a/apk/trace-{pull,push}.apk (8504077 / 8622861 bytes,
debug-signed, MGPipe strings 3 / 107, mpr present in push) - Release size class (not the -O0 trap).
Device A/B: Xiaomi running (session pinned 21:38:16 after cool-down; 26.3 GLES pull nofinish p50 10.76 ms). Oppo's
first attempt died the P3a way - the tapper hit the ColorOS dialog while the streamed install was in flight and adb
dropped (device not found for runs 2/3). FIX: `MOBILEGL_TRACE_SKIP_INSTALL=1` in android-plugin/trace-replay-ci.sh
(pipe a38bdab4, tools-only commit on top of the gated 6035c9d7; the Windows runner tree fast-forwarded with
GIT_LFS_SKIP_SMUDGE=1) and, on the Oppo only, the A/B script installs each arm itself (tapper handles the dialog,
`adb wait-for-device`, `pm path` poll up to 90 s) before running the case with the install skipped. Both A/B
scripts also gained the Xiaomi cool-down wait (< 42 C), the rd12/Magma-on-Xiaomi skip (ID-6) and the tapper.
- ID-48 addendum (tooling lesson): `pkill -f` from the Git Bash tool does NOT kill a detached A/B script on
  Windows (the old Oppo run kept going, holding the device lock while the restarted one waited); enumerate with
  PowerShell `Get-CimInstance Win32_Process` filtered on the command line (excluding the query's own process) and
  kill with `taskkill /PID <pid> /T /F`; a `wmic ... like '%<pattern>%'` query from the Bash tool matches the tool's
  own shell command line and killing it aborts the tool call. Remove the device lock dir by hand afterwards.
- A/B note: Xiaomi rd12 GLES push nofinish ran with pin=1/1 (DRIFT before and after - the thermal clamp, ID-8);
  p50 12.94 vs pull 8.24 ms is NOT a valid pair - re-run the rd12 push arms on the Xiaomi after a cool-down at the
  end of the session (single-case re-run, package left installed); ab_reduce3.py must exclude drifted samples.

## ID-49 FULL GATE GREEN on 6035c9d7; pushed: origin feat/disaggregated = `a38bdab4` (= 6035c9d7 + the tools-only harness commit)
Gate: every part green (ID-48's static + lanes) plus the three retrace sweeps - verify 79/79 with 79 armed and zero
Fatal, push 79/79, G3b named 12/12. Device A/B: Xiaomi session 1 finished with the thermal clamp drifting every
sample from rd12 push onwards (pin 1/1) - 14 of 36 samples clean; the scripts gained a per-case cool-down (< 42 C,
up to 15 min) + re-pin and a clean-pin skip, and session 2 is redoing the 22 drifted samples. Oppo: session 2 (pre-
install flow) running. Desktop bench (`wsl_p4a_bench.sh`, idle box) launched; the fable final review is relaunched
(the first attempt died at start on an API ECONNRESET) with its build/mutation experiments deferred until the bench
is done so the bench stays on an idle box.
- A/B tooling (ID-49 addendum): the Xiaomi cool-down loop timed out once at a reading below the threshold (the
  raw `su -c cat` output is not always clean digits right after a reboot); both scripts now read the zone with
  `tr -cd "0-9"` and compare `${T:-99999} -lt 42000`. Session 2 killed by PID (PowerShell), lock released,
  session 3 launched (log ab-35d0befa-3.log); already-clean samples are skipped, drifted ones redone.

## ID-50 desktop DriverBench on 6035c9d7 (recorded, not gated; ~/w7/notes/p4a/bench/driverbench.{csv,md})
mc_vanilla_draw ns/draw: espryt T1 = push(0x1fff) - pull = +1269.7; T2 = push(0x1ff) - pull = +1135.8; T1 - T2
(what P4a added) = +133.9; PIPE_PUSH=0 - pull = +697.8; no-CSO-addressing - push = +276.6. magma T1 = +139.1;
T2 = -191.7; T1 - T2 = +330.8 (Magma emits nothing for P4a families after c0f - the delta is the tracker's
extra shutter work plus noise; the pull/push spread on magma is within its repeat noise). mc_state_toggle:
espryt pull 24695 / push 26265 (+1570), magma 36225 / 37149 (+923); mc_pass_switch: espryt +7695, magma -5609.
Compared with P3a's table (espryt T1 +1047.7, T2 +348.5): the P3a-boundary arm itself moved (+1136 vs +349)
because the 0x1ff arm now also runs P4a's tracker/shutter work with the families off - to be stated in
MEASUREMENTS.md as a caveat (T2 is no longer a pure P3a number).

## ID-51 device A/B, Xiaomi (Adreno 830) main pairs complete - 36/36 clean pins after session 3; Release APKs; push - pull p50 (ms, nofinish / finish)
26.3 GLES +1.67 / +1.57 (10.76 -> 12.43; +15%); 26.3 Magma +0.91 / +0.86 (Magma emits nothing for P4a families
after c0f: this is the P2+P3a boundary + the tracker); rd12 GLES +2.97 / +3.03 (8.24 -> 11.20; +36% - the
largest; the 0x1ff arm will say how much is P4a's); sodium GLES +0.06 / +0.16, sodium Magma -0.42 / +0.08;
vanilla GLES +0.50 / +0.49 (2.34 -> 2.84; +21%); vanilla Magma +0.69 / -0.16 (noisy); iris-bsl GLES +0.64 /
-0.36 (p50 ~1.3-1.9 ms with 300+ ms p99 spikes on both arms - the shader-pack compile stalls dominate). Oppo main
session running (pre-install flow OK); Xiaomi 0x1ff arm running; then the three-arm reduce (ab_reduce3) and the
per-case MGPipe stats. Performance is recorded, not gated (user rule); rd12's +36% goes on the optimisation list
with the P3a +17-19 pts it stacks on.

## ID-52 final whole-diff review (fable) = REWORK: two criticals + one major; fable fix round launched (B is past two reworks)
C-1 (critical, TextureEmit.h:955-960): every respecify calls MGPipeApplyResourceRespecify(desc, nullptr) - no
emitter builds an MGPRespecifiedLevel - so a per-level glTexImage*D / glGenerateMipmap grow takes the whole-
resource arm (PipeApply.cpp:1684) and drops every accepted-but-unconsumed pending upload while the client flag
is already clear: `L0; draw(other); L1; draw(T)` and `L0; draw(other); glGenerateMipmap; draw(T)` read 100% black
at 0x1fff, red at 0x1ff (three controls red on both) - wire-v3 §5 item 5 required B to pass the level and no review
checked it. C-2 (critical): a dead-but-unrecycled texture handle still resolves to the freed ITextureObject*
(ResolveTexture checks GenOfSlot, which moves only at re-acquire; the death helper does not forward to the emitter;
the drain list survives death): `glTexImage2D; glDeleteTextures; draw` -> SIGABRT "pure virtual method called" on
the handle arm. M-A (major): no producer of kMGPipeBindSampler / kMGPipeBindShaderImage - ImageBindableHint is
always 0, D-A4's remint mitigation undelivered, ROADMAP.md:85's number unmeasurable. Minors: F-7 latch-before-
acceptance for set_texture_params; Config.h omits the 10 -> 11 dependency row; "ORed" vs replace-whole comment;
F-6 encodings in package headers; the CPU three-channel mipmap re-arm gap; G9 blind when row + mark drop together.
Verified green by the review: G1 at TU granularity, G5 red on a statement mutation, G7 both controls, G9 red on a
decidable row, first-attempt mutations red, TEMPORARY test.yml:9-12 trigger still present (remove before dev), all
D.5 placeholders unfilled (§6). RULING: one fable fix round on a branch from a38bdab4 - C-1 (B passes the level;
a unit case + the two llvmpipe scenarios as itest), C-2 (death path: the death helper forwards to the emitter,
the drain list drops the dead entry, ResolveTexture refuses a dead slot; the delete-then-draw scenario), M-A (the
bind-bit producers at glBindSampler / glBindImageTexture and wherever D-A4 names; ImageBindableHint measurable),
the cheap minors; then the review's §8 re-runs + quick gate + push retrace, a focused fable re-review, and the
device A/B GLES pairs redone on APKs built from the fixed head (C-1 drops uploads, which biases the push arm).
- CI: Test lane GREEN on a38bdab4 (the pushed P4a head + harness switch).

## ID-53 Xiaomi 0x1ff arm complete; Oppo session hung on a re-install - hardened and relaunched (session 3)
Xiaomi push(0x1ff) p50 nofinish (ms): 26.3 GLES 11.86 (pull 10.76 / push 12.43 -> P4a's own share +0.57 of +1.67),
26.3 Magma 11.53 (= its 0x1fff arm, no P4a consumer), rd12 GLES 10.84 (pull 8.24 / push 11.20 -> P4a +0.36 of
+2.96; the P2+P3a boundary carries +2.60), sodium GLES 1.40 / Magma 1.01, vanilla GLES 2.72 (pull 2.34 / push 2.84
-> P4a +0.12 of +0.50), vanilla Magma 1.15, iris-bsl GLES 1.86 / Magma 0.78. Oppo session 2 hung at 21:49:45 in
the pre-install `adb install -r` of the push arm - the ColorOS dialog for a REPLACE install was never tapped (the
tapper matched InstallGuideActivity only) and the streamed install session dangled for 1.5 h with the device
back on its launcher. Hardening (both scripts): the tapper matches InstallGuide|PackageInstaller|installer|appdetail;
the pre-install runs under `timeout 150` with three attempts (Back key + wait-for-device between them). The hung
tree killed by PID (54088 adb + 48100/48936/48268), lock released, mixed results moved to 3B159D009VZ00000-old;
session 3 launched (log ab-3B159D009VZ00000-2.log). Monitors for the dead logs are left idle.

## ID-54 Xiaomi three-arm table (Release APKs from 6035c9d7, pre-finalfix; nofinish p50 ms: pull / 0x1ff / 0x1fff, d P3a-boundary / d P4a total)
26.3 GLES 10.76 / 11.86 / 12.43 (+10.2% / +15.5%); 26.3 Magma 10.67 / 11.53 / 11.58 (+8.0% / +8.5%); rd12 GLES
8.24 / 10.84 / 11.20 (+31.6% / +36.0%); sodium GLES 1.38 / 1.40 / 1.44 (+1.3% / +4.3%); vanilla GLES 2.34 / 2.72 /
2.84 (+16.4% / +21.4%); iris-bsl GLES 1.30 / 1.86 / 1.95 (+42.7% / +49.2% on a ~1.5 ms p50 with 317 ms p99 compile
stalls - shape-dominated). Magma rows other than 26.3 are noise-dominated at these frame times (sodium Magma push
0.52 vs 1ff 1.01; vanilla Magma pull-nofinish 1.05 vs pull-finish 1.97) and are reported with that caveat; Magma
emits nothing for P4a families (c0f), so its "P4a" arm is the tracker only. So P4a's own share on Espryt is
+0.5-0.6 ms on 26.3 and +0.1-0.4 ms elsewhere; the P2+P3a boundary carries the rest (rd12 +2.6 ms). Raw results
and the table copied to ~/w7/notes/p4a/ab/prefix-a38bdab4/ (tool: ab_reduce3_p4a.py, the P3a reducer re-labelled
for push1ff). These are the PRE-finalfix numbers: the GLES pairs will be re-measured on APKs built from the fixed
head (C-1 dropped uploads on the push arm, which flatters it).

## ID-55 final fix round integrated: pipe = `8c458cd5` (five commits; post-merge running; fable re-review running; APKs rebuilding)
finalfix (from a38bdab4): 173f1dd2 C-1 (storage entry points state their scope - one level / chain cut / whole
resource - the emitter builds MGPRespecifiedLevel with the drain's packed target, per-level calls never deduped,
the applier drops exactly the named level), a690032f C-2 (every death helper forwards to its emitter between the
wire delete and the free; ResolveTexture refuses a dead slot loudly; sticky-mask producers stamp the generation;
delete-then-use pinned per kind, for a recycled slot, under MALLOC_PERTURB_ on both backends), 9f60aadc M-A (the
SAMPLER bit from view resolution, SHADER_IMAGE at glBindImageTexture through a contract door; ImageBindableHint
precedes the first sync; `tex-remint-pulls` = ROADMAP:85's number), c2c6a655 minors (F-7 latch-on-acceptance with
a Bool return, Config.h row, the comment), 8c458cd5 (census residual: default textures born before consumer
registration heal from their first set_texture_params - the re-review judges whether that is a legitimate late
birth). Gate on 8c458cd5 (the agent's): G1 0/0/0/0, G5 x4 rc 0, gen tools rc 0, unit 1785 x 3, names 2902 ==
2902, +314, integration-gpu 1117/1117 on seven arms (DirectVulkan 559/559), verify 920/920 zero Fatal, retrace
79/79, retrace census 0 on 16 needles, itest census 0/13, negative controls tripped. Declared m-4 (P4b), m-5,
m-6, m-7 benign, m-8 not reproduced. No Co-Authored-By anywhere (user rule re-confirmed 2026-09-09). Pre-fix APKs
kept at ~/w7/notes/p4a/apk-prefix/; the Release APKs are being rebuilt from 8c458cd5 for the GLES A/B redo.
Lesson (memory subagent-parked-notifications): a "finished" notification whose result says "waiting on the
build/gate" is a PARKED agent, not a dead one - the continuation agent launched for it was stopped.
- ID-55 addendum: post-merge on pipe G1 0/0/0/0, unit 1785 x 2, G5 rc 0, names 2902 == 2902, 0 removed / +314;
  pushed: origin feat/disaggregated = `8c458cd5`; the Windows runner tree fast-forwarded to it (LFS smudge off).

## ID-56 STOPPED HERE by the user (2026-09-09 ~01:20): origin feat/disaggregated = `8c458cd5`
Done: contract c0..c0g, wire v3, clientsp v3, clientfb v2, esprytobj v3, esprytdraw v3 (+ the two integrator
patches), gates v3, the fable seam-audit fix round, the final-review fix round - all integrated, the full gate
green on 6035c9d7 and the fix agent's own full gate green on 8c458cd5 (itest 1117/1117 on seven arms, verify
920/920, retrace 79/79, census 0); CI green through a38bdab4 (8c458cd5's run pending); Release APKs from 6035c9d7;
device three-arm A/B on both devices (pre-fix APKs) in ~/w7/notes/p4a/ab/prefix-a38bdab4/ + three-arm-prefix.md;
desktop bench in ~/w7/notes/p4a/bench/. NOT done, by the user's decision to stop: the fable focused re-review of the
five fix commits (stopped mid-read), the GLES A/B redo on post-fix APKs (the rebuild was stopped; the pre-fix
numbers flatter the push arm slightly because C-1 dropped uploads), docs D.5 (README status line, ROADMAP P4a row,
MEASUREMENTS §22+ - all placeholders unfilled), the TEMPORARY CI trigger branch in test.yml/apk.yml (must be
removed before merging to dev), worktrees p4a-{c0f,esprytdraw,fablefix,finalfix,gates} and 47 logs under ~/w7
left in place. Known open items are listed in the closing summary to the user and in final-review-v1.md §recorded,
fable-seam-audit.md §D, finalfix-v1.md §declared.

## ID-57 P4a 收尾（2026-09-11，新设备）：docs D.5 写作中，红米上重测
用户接了一台**红米 `2f7cbe2e`**（Redmi M332BF "warsaw"，SM8750 / Adreno 830v2、Magisk root、**带主动风扇**）并规定：**之后实验只用它，其他 adb 设备一律不碰**；同时指出本分支的活应当在 `MobileGL-disagg` worktree（feat/disaggregated）而不是主 dev 树上做——dev 树上那两处调试 `fprintf` 插桩已按用户指示丢弃。
- **设备工装**：`pin_device.sh` 加了 `2f7cbe2e` 条目。同 SoC 同 OPP（`1958400`/`1555200` 两个战役定频点都在），唯一差别是 policy6 的 stock 上限 3072000（旧机 2841600）。**GPU 定频点是 1050 MHz 不是 1100**：厂商把 `kgsl-3d0/thermal_pwrlevel` 永久钉在 1，`max_gpuclk` 因此 1050，root 写 0 无效（33 °C + 风扇 16k rpm 下验证过）——旧小米那个间歇性的"pwrlevel 0 但 gpuclk=1050 DRIFT"几乎肯定就是它。
- **风扇**：`/sys/class/xm_power/hw_monitor/pwm_fan`，A/B 全程 level 2（~14.5k rpm）恒定——它是常量不是处理项，两臂同风量。降温从 20–30 分钟变成 <1 分钟。档位表与两个坑（101–104 debug 锁存要写 100 退出、`pwm_duty` 是绕过档位表的原始旋钮、状态只能看 `real_speed`）记在 memory `fcl-device-testing`。
- **新 A/B 驱动**：`notes/tools/p4a_ab_redmi.sh`（序列号硬保护，只认 `2f7cbe2e`；风扇常开；无安装点击器——这台 `adb install` 直接过；**不跳过 rd12/Magma**）。实测：rd12 在 DirectVulkan 上 **rc=1 照样崩**，和旧小米一致，确认是 `dev` 侧问题不是设备个例。
- **APK 重建自 `8c458cd5`**：pull 8504077 B（与修复前同字节，正是 G1 要的 pull 不动）、push 8626957 B / 109 条 MGPipe 串。修复前那对存 `~/w7/notes/p4a/apk-prefix/`。
- **补测**：79 例 retrace 带 `MOBILEGL_PIPE_STATS=1` 各跑一遍 push/pull 臂，为的是 ROADMAP 开放问题 2（`trp` = 780 窗口里 **2 次**，只在两条 Iris 光影用例的 DirectGLES 侧）、§20 读法 (5)（新的 `csob-blob` 字节类：DirectGLES 27 例中位 **2928 B/帧**、rd12 峰值 **1.06 MB/帧**、DirectVulkan 恒 0 = 消费者门的量化）与 D.4.3 的上传形状头条。DriverBench 在 `8c458cd5` 上重跑（原 ID-50 的数出自 `6035c9d7`）。
- **docs D.5 进度**：README 状态行与文件地图、ROADMAP 的 P4a 行/P3b-P4b 行/39 天检查点/通用纪律（补上"不顺手修 dev bug"，让课题 17 的 `ROADMAP.md:7` 引用成真）/开放问题 2、8、17、ARCHITECTURE 十二处、MEASUREMENTS 读法 (5) 与 `g_uploadRing` 旁的第二条同类项、新增 §22（五部分门两个头对照）与 §23（**缝的分类**——九类，每类给症状与预防，是这一波最值钱的产出）均已写；§24（设备三臂表 + p99 + 上传形状）与 §25（T1/T2/T3）等测完补。`check_doc_citations.py` 97 处引用 0 问题。
- **清理踩坑**：`rm ~/w7/p4a-*.log` 把两个**正在被写**的 retrace 日志删了，导致以日志标记为条件的 bench 等待者永远等待（输出目录无损）；改成 `while pgrep -f retrace_gate...; do sleep 30; done` 等进程。已记进 memory `temp-file-hygiene`。
- ID-57 addendum: **P4a 收官，docs D.5 已推送：origin feat/disaggregated = `a29807cc`**（`8c458cd5` 代码 + 一条 docs 提交，citation lint 100 处 0 问题，无任何 attribution 行）。红米三臂表与原始结果在 `~/w7/notes/p4a/ab-redmi/`（40/40 干净 pin），bench 在 `~/w7/notes/p4a/bench/`（重跑于 8c458cd5）。**桌面 bench 分辨不出 P4a 自身**（两轮 T1−T2 = +133.9 与 +11.4 ns/draw，而两臂绝对值各自漂 ~200 ns），可引用的是 T1 ≈ +1.1 µs/draw 的总边界与设备侧三臂表。设备侧 P4a 自身 = +3.4～+5.7 个百分点（0.05–0.38 ms/帧），Magma 两臂差在噪声内 = c0f 消费者门的读数。上传形状 pull/push 全语料只差 +2（正好是 2 次 trp）。**留给优化阶段的第一条线索**：设备上 26.3 Espryt 的 `sve` ≈ draw 数（9143/9138），桌面同 fixture 只有 ~0.07/draw。
