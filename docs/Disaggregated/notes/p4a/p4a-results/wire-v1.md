# P4a package A — `w1`..`w4`, the applier bodies. Result, v1

Branch `refs/heads/p4a/wire`, worktree `/home/swung/w7/p4a-wire`, on top of the tag
`refs/tags/p4a/contract` = `08192d72`. **Four commits, two production files and seven test
files, +3147 / −49. Not pushed. Nothing else touched.**

| commit | message (single line, empty body, no attribution) |
|---|---|
| `86835b50` | `[Feat] (Pipe): apply the framebuffer record per bound target and the texture and renderbuffer resource calls into their own slot-indexed records` |
| `bc9abfbd` | `[Feat] (Pipe): apply sampler states, sampler views and the three unit sets into the server's own working state` |
| `0f990633` | `[Feat] (Pipe): apply the shader CSO's artefacts and the default uniform block without ever re-linking on the server` |
| `3c07c8c3` | `[Test] (Pipe): pin every P4a record's lifecycle, its bounds gate and what a make-current does and does not clear` |

```
 MobileGL/MG_Pipe/PipeApply.cpp                  | 1016 ++++++++++++++++++--
 MobileGL/MG_Pipe/PipeApply.h                    |   46 +-
 MobileGL/MG_Test/Pipe/CompositeResolverTest.cpp |  237 +++++
 MobileGL/MG_Test/Pipe/FramebufferEmitTest.cpp   |  249 +++++
 MobileGL/MG_Test/Pipe/ImageEmitTest.cpp         |  248 +++++
 MobileGL/MG_Test/Pipe/ProgramEmitTest.cpp       |  397 ++++++++
 MobileGL/MG_Test/Pipe/ResourceEmitTest.cpp      |  148 +++
 MobileGL/MG_Test/Pipe/SamplerEmitTest.cpp       |  444 +++++++++
 MobileGL/MG_Test/Pipe/TextureEmitTest.cpp       |  411 ++++++++
```

**Behaviour neutrality holds and is structural.** Nothing in this package registers a consumer
of any record: the four subsystem bits are absent from `kMGPipeWiredSubsystems`, every client
emitter beside them is still a stub, `MGPipeResourceOps` is unchanged at nine members, and no
backend reads the new state. `grep` over `MobileGL/` for the fifteen entry points returns hits
only in `MG_Pipe/PipeApply.{h,cpp}` and in `MG_Test/Pipe/*EmitTest.cpp`. The pull build is
byte-identical to `$BASE`'s (G1 0/0/0/0).

---

## 1. Verdict on the hard rules (after `3c07c8c3`; re-run unchanged after every commit)

| rule | result |
|---|---|
| **G1** `symbol_report --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` | rc 0, **0 added / 0 removed / 0 renamed / 0 resized** after each of the four commits |
| `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 (7 controls tripped) |
| `git diff --exit-code -- MobileGL/MG_Pipe/generated` | rc 0 |
| `gen_pipe_dirty_surface.py --check` / `--self-test` | rc 0 / rc 0, 27 negative controls, all tripped |
| `check_include_closure.py --mode both --compiler clang++ --self-test --require-all` | rc 0, 4 probes, 0 problems, 6 controls tripped |
| `p3a_untouched_regions.sh 37da3c3a HEAD` | rc 0, the eleven byte-identical |
| three builds compile (pull / push / verify) | all three, after every commit |
| `ctest -L unit` in all three | **1668 / 1668 in each**, 0 failed (1635 at the tag, +33) |
| ctest names pull == push | `diff` empty |
| no name removed vs `~/w7/p4a-before-ctest-names.txt` | empty; 46 added (the contract's 13 + this package's 33) |
| only A's files | yes — `MG_Pipe/PipeApply.{h,cpp}` and the seven `MG_Test/Pipe/*Test.cpp` C.7 gives A |
| four commits, exact messages, no attribution, not pushed, no other worktree | yes |
| `grep -rc 'pGLContext' MG_Backend` | only `MGPipe/PipeInputs.h:1`, pre-existing |

---

## 2. Per-entry-point behaviour: record, serial, bounds, refusal

The two verdicts are P3a's and the difference between them is the whole discipline:

* **a call naming a record this applier does not have is a DEFINED NO-OP that is COUNTED.**
  One legal sequence produces it (teardown → `MGPipeApplierReleaseObjectRecords` → `~Object` →
  death notices naming records already dropped), so it cannot be a wire; `MOBILEGL_ASSERT` is
  inert at INFO, so it cannot be only an assertion. `RefusedResourceCalls` takes the resource
  family (buffers, textures **and** renderbuffers — they are resource calls);
  `RefusedObjectCalls` takes the five object families (framebuffer, sampler, sampler view,
  program, texture params).
* **a record that would make the server index or allocate outside its own storage is
  `Fatal{ProtocolCorruption}`** with the record's identity in the line, and it is **not**
  counted as a refusal.

### 2.1 `set_framebuffer_state`

| | |
|---|---|
| storage | `DrawFramebuffer` / `ReadFramebuffer`, working state. `Target != Read` writes the draw record, `Target != Draw` writes the read record, so `Both` writes both. |
| serial | `FramebufferSerial`, one for the pair, `++` once per applied record (**a `Both` record is one record and bumps once**). |
| bounds | `Target >= MGPipeFramebufferTarget::Count` → Fatal. Every `DrawBuffers[i]` outside `[-1, kMGPipeMaxColorAttachments)` → Fatal (an index into the record's own `Color[]`). |
| refusal | **none, ever.** A framebuffer has a handle but no wire lifetime (D-I2), so there is no record to resolve; and the surfaces' `Res` handles are deliberately **not** resolved, because an attachment pins its texture through the frontend's own `SharedPtr` in monolith and a refusal here would enforce a lifetime rule monolith cannot need (D-I3). This entry point never touches `RefusedObjectCalls`. |

### 2.2 `resource_create` / `resource_respecify` / `resource_subdata` / `resource_destroy`

The four P3a entry points now **branch on the descriptor's `Target`** (on the handle's `Kind`
for the destroy, which carries no descriptor). The three tables are `Resources`,
`TextureResources`, `RenderbufferResources`; the record **type** is shared and the bound
(`kMGPipeMaxResourceSlots`) is shared, only the table is per kind.

| | |
|---|---|
| table selection | `Target == Buffer` → `Resources`; `Target == Renderbuffer` → `RenderbufferResources`; every other enumerator below `Count` → `TextureResources`; `>= Count` → Fatal. Destroy: `Kind` ∈ {Buffer, Texture, Renderbuffer}, anything else → Fatal. |
| create | starts the record over, `Gen` from the handle, `Live = true`, `Serial` stays **0**. Dispatches to `MGPipeResourceOps::Create` **only for a buffer**. |
| respecify | replaces `Desc` whole, `++Serial`, **clears `PendingUploads`** (§2.4), dispatches **only for a buffer**. |
| destroy | drops the record whole, keeps `Gen`, dispatches **only for a buffer** (the op table is the buffer family's). |
| bounds | `slot >= kMGPipeMaxResourceSlots` → Fatal naming the bound; the table is never grown by a corrupt slot. |
| refusal | `RefusedResourceCalls`, through `ResolveResourceIn`. |

### 2.3 `set_texture_params`

| | |
|---|---|
| storage | `MGPipeResourceRecord::Params` on the **texture** record, addressed by resource and independent of any binding. |
| serial | `ParamsSerial`, `++` per applied call. `Serial` (the storage serial) does **not** move — a parameter push is not a storage mutation. |
| bounds | `params.BuiltinSampler == kMGPipeNullHandle` → **Fatal** (§5, hazard H1). |
| not checked | the CSO the record names is **not resolved**: the sampler bit may legitimately be clear while the texture bit is set, so an unresolvable `BuiltinSampler` is an ordering fact, not a corrupt one. |
| refusal | `RefusedObjectCalls`. |

### 2.4 The sub-data validator, per target

`SubDataBoxFault` was **split in two** rather than widened; every statement it made is a
statement about the buffer convention and every one of them is false for a texture.

**Buffer half (`Target == kMGPipeResourceTargetBuffer`, unchanged from P3a):** offset above
`0x7FFFFFFF` → Fatal; `Level != 0` → Fatal; `RegionCount != 0` → Fatal; `Blob.Size != 0 &&
Blob.Size != MGPipeSubDataBufferSize(record)` → Fatal; then `BufferRangeFault` against
`Desc.Width`; then a non-empty write with `bytes == nullptr` → Fatal. Dispatches to
`MGPipeResourceOps::SubData` / `SubDataResident`.

**Texture half (every other `Target`):**

| rule | verdict |
|---|---|
| `Level >= kMGPipeMaxTextureLevels` (32) | Fatal |
| the union box has a negative origin, or `origin + extent > 0x7FFFFFFF` on any axis | Fatal |
| `RegionCount > kMGPipeMaxPendingUploadRegions` (256) | Fatal |
| `RegionCount != 0 && regions == nullptr` | Fatal |
| a region with a negative origin or an unencodable extent | Fatal |
| **a region not inside the union box** | Fatal |
| an empty box **and** zero regions ("no texels at all") | Fatal |
| `bytes == nullptr` | Fatal |
| more distinct `(UploadTarget, Level)` keys than `kMGPipeMaxPendingUploads` (256) | Fatal |

The region-containment rule is the one that matters: the union box **is** the union of the
regions, the server picks the upload shape from the pair, so a region outside the box means the
box misses its texels and the region writes where the box never said it would.

**Deliberately NOT checked, so a later reader does not add them back as an oversight:** (a) the
level against `Desc.Levels` — a mutable texture defines its levels one `glTexImage2D` at a
time, so the descriptor's level count is not an upper bound at every instant and a gate on it
would refuse a legal upload; (b) the box against the descriptor's extents — the record
addresses the **level's** coordinate system and a view remaps that space, so the arithmetic is
the storage owner's; (c) the blob — **no field of a texture record describes its own byte
length**, so the one Blob rule has nothing to cross-check and is inert here by construction.

**Pending-upload accumulation (D-D5).** Keyed `(UploadTarget, Level)` on the texture's record.
Boxes union; rect lists concatenate; and the moment either side says "box only", or the list
would outgrow `kMGPipeMaxPendingUploadRegions`, the entry becomes **box only** — never a
dropped region, because the box still covers every texel the dropped list named. That is the
frontend's own model one level up (`MipmapStorage` answers "0 rects" for everything it cannot
describe that way). An entry is never created empty, which is why an empty `Regions` after the
first contribution unambiguously means box-only. `++Serial` on acceptance, and the acceptance
is what the client reads before clearing its own dirty flag.

### 2.5 `create_sampler_state` / `delete_sampler_state`

| | |
|---|---|
| storage | `SamplerCsos[slot]`, `Params` by value **including `borderColorForm`**. |
| serial | a **fresh or recycled identity** starts over with `Serial = 0` (a create is not a mutation, and a fresh twin starting at 0 agrees without either side publishing); a **re-issue on a live identity** keeps the record and `++Serial`. |
| bounds | `Parameters.Size != 0 && != sizeof(SamplerParameters)` → Fatal; `parameters == nullptr` → Fatal; `slot >= kMGPipeMaxSamplerCsoSlots` → Fatal. |
| delete | drops the record whole, keeps `Gen`. **Does not sweep `BoundSamplerStates`** — that window is "the last set as received" and the client re-emits it at the next verb. |
| refusal | `RefusedObjectCalls`. |

### 2.6 `create_sampler_view` / `delete_sampler_view`

| | |
|---|---|
| storage | `SamplerViewCsos[slot]`, `View` verbatim, plus **the texture's `ViewCso` back-pointer**. |
| serial | the same fresh-vs-re-issue rule as the sampler CSO. A re-issue **does not rebind** anything. |
| bounds | `slot >= kMGPipeMaxSamplerViewSlots` → Fatal. `View.Target`, `MinLevel`/`NumLevels`/`MinLayer`/`NumLayers` are **not policed** (`MGPVertexBuffer::BindingIndex`'s precedent: the applier cannot justify that contract). |
| back-pointer | written through a **silent** `FindIn` (no refusal counted) — a view arriving before or without its texture record is legal in a mixed-bit lane. The delete clears it **only if the texture still names this view**. |
| refusal | `RefusedObjectCalls` on the delete. |

### 2.7 The three `kVarTail` unit sets

One shared body (`ApplyUnitWindow`), because they differ only in what an entry is.

| | |
|---|---|
| bounds | `Start + Count > 192` → **Fatal** (`kMGPipeMaxTextureUnits` for the two sampler sets, `kMGPipeMaxImageUnits` for the images; the two constants are equal and a case pins that). `Count != 0 && tail == nullptr` → Fatal. |
| storage | entries land at `Start + i` and **nowhere else**; `Start`/`Count` stored; **entries outside the window are not cleared** — the record is "the last set as received", and a set that names four units has said nothing about the other 188. |
| serials | `SamplerViewsSerial` / `SamplerStatesSerial` / `ShaderImagesSerial`, `++` per applied set, independently. |
| refusal | **none** — a set is working state and resolves no record. A **null handle in any tail entry is legal** everywhere (a unit the program does not resolve, a unit with no sampler object, an unbound unit) and the entry's own `Unit` field is deliberately unpoliced: the destination is `Start + i`, which the window already bounds. |
| empty set | applied, not refused, and it still moves the serial — a program with no images publishes one. |

### 2.8 `create_shader_state`, the two bindings, `bind_shader_state`, `set_global_constants`

| | |
|---|---|
| table | `MGPipeIsCompositeShaderSlot(slot)` routes to `CompositeShaderCsos[slot − base]`, bounded by the band's width; every other slot to `ShaderCsos[slot]`, bounded by **the band's base** (an ordinary program can never be handed a band slot, so a non-composite slot at or above the base is out of range by definition). A slot at or above `kMGPipeShaderCsoSlotLimit` → Fatal naming `kMGPipeMaxShaderCsoSlots`. **The server never learns a handle is a composite**; the split is two functions and nothing else branches on it. |
| create | fresh/recycled → record starts over, `Serial = 0`; re-issue on a live identity → `++Serial` **and the default uniform block is dropped** (image cleared, version back to the `~0u` sentinel, `++GlobalConstantsSerial`), because a relink replaces the layout the block was sized to. `Desc` stored; the artefacts are **not** stored (in monolith the backend reads the frontend's own archive). |
| create bounds | `link == nullptr || spirv == nullptr` → Fatal ("declares no blobs and carries no artefacts"); `GlobalUboSize > kMGPipeMaxGlobalConstantsBytes` (16 MiB) → Fatal. |
| bind / draw / dispatch | one `ProgramBindingSerial` for the three, `++` per applied call. A **null** handle is legal in all three and means "nothing bound" (and still bumps); a **dead** handle leaves the previous binding untouched and is counted. |
| delete | drops the record whole, keeps `Gen`, and **clears every one of the three bindings that named it** (unlike the unit sets: these are single handles resolved against the record just dropped), bumping `ProgramBindingSerial` only if one was actually cleared. The **second** delete — a composite's other release path — is a counted refusal and moves nothing. |
| `set_global_constants` | on the **program's** record. `Version == ~0u` → Fatal (the backends' never-uploaded sentinel); `Blob.Size != 0 && != Desc.GlobalUboSize` → Fatal; `GlobalUboSize != 0 && bytes == nullptr` → Fatal. Stores `GlobalUboSize` bytes, sets `GlobalConstantsVersion`, `++GlobalConstantsSerial`. `Serial` does not move — a block upload is not a relink. The allocation is bounded **at the create**, so this call can only ever allocate what the create already declared. |

### 2.9 `MGPipeApplierReset` / `MGPipeApplierReleaseObjectRecords`

Unchanged from the contract commit and re-pinned by four cases: the reset clears the two
framebuffer records, the three unit sets and the three program handles and **advances** all five
P4a serials; it does **not** touch texture/renderbuffer resources, sampler CSOs, sampler views
or shader CSOs — nor the `Params`, `ParamsSerial`, `ViewCso` and `PendingUploads` that ride on a
resource record. `ReleaseObjectRecords` is the only teardown scope and advances the serials
again for the same reason.

---

## 3. The verify-only codec round trip (D-H3)

`PipeApply.cpp` includes `<MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>` **inside
`#if MOBILEGL_PIPE_VERIFY`** — the only place the codec is called, and the coupling the
contract header already anticipated ("the verify build is the only place the codec runs, and it
runs from `PipeApply.cpp`"). `PipeApply.h` keeps its two forward declarations and reaches no
frontend header; the include-closure gate's four probes are unaffected (none of them names
`PipeApply.*`).

`PinProgramArchiveRoundTrip(desc, link, spirv)` runs **before the record is stored**, so a lane
that aborts aborts on the record that was wrong:

1. `EncodeProgramArtifacts(link, spirv, encoded)` — empty output for a program that has
   artefacts is a fault;
2. `DecodeProgramArtifacts(...)` returning false is a fault;
3. re-encoding the decoded pair and comparing the bytes — a mismatch is a fault (the codec's own
   suite reads the members back explicitly; what this adds is that it runs over **every real
   program the verify lane links** rather than over one hand-built instance);
4. `decodedLink.program != nullptr` is a fault — the live glslang `TProgram` is the one member
   the tables omit, and a decode that reconstructed one would be carrying the compiler front end
   across the boundary that exists to keep it on the client side.

The verdict is `MGP_TRIP_WIRE_REPORT` with `MGP_TRIP_WIRE_TAG("PipeVerifyDiffer")`, so a verify
or poison build writes `Fatal{PipeVerifyDiffer} program-archive create_shader_state {slot=…,
gen=…}: …` and aborts — the marker G4 greps the retrace logs for. **Zero cost in push and
pull**: the include, the function and the call site are all inside the verify guard.

Mutation **M41** (making the decode fail) is what proves the pin is live code rather than a
comment.

---

## 4. Deviations, each with its reason

**W1 — `MGPipeApplyResourceSubData` gained a trailing defaulted parameter,
`const MGPSubRegion* regions = nullptr`.** The contract's signature has no tail parameter, and
D-D5 is unimplementable without one: `ResourceSubData` has carried `kVarTail` since P2, the
applier's pending-upload set is `(UnionBox, RegionCount, Regions[])`, and the verify lane's
retain mode compares all three. The parameter is **trailing and defaulted**, so P3a's one call
site (`MG_Impl/Pipe/PipeFill.cpp:705`), every existing case and every other package compile
unchanged. **Package B must pass the tail** when it emits a texture upload; a record that
declares `RegionCount != 0` and passes no tail is refused. This is the only signature change in
the package.

**W2 — four new bounds constants in `PipeApply.h`.** `kMGPipeMaxTextureLevels = 32`,
`kMGPipeMaxPendingUploads = 256`, `kMGPipeMaxPendingUploadRegions = 256`,
`kMGPipeMaxGlobalConstantsBytes = 16 MiB`. Every one of them is a number that arrives inside a
payload and decides how much the applier allocates or how far it indexes; the slot bounds' own
argument ("never allocated by being named; a slot at or above the bound is
`Fatal{ProtocolCorruption}` and never a resize") applies to each unchanged. They are additive
and inside A's own file.

**W3 — `resource_destroy` dispatches to the backend only for a buffer**, and the same is true
of `resource_create` and `resource_respecify`. D-B1 says every non-buffer target "stores and
returns"; the destroy's guard was missing in the first draft of `w1` and
`ResourceEmit.NoTextureOrRenderbufferResourceCallReachesTheBackendOpTable` found it. The fix is
folded into `86835b50` where it belongs (the branch was rewritten before anything was pushed,
so the four-commit shape and the four exact messages are intact).

**W4 — `set_framebuffer_state` counts no refusal.** The contract's header text lists
"framebuffer" among the families `RefusedObjectCalls` covers. It cannot: D-I2 gives the
framebuffer a handle and no wire lifetime, so this call resolves no record and there is nothing
to refuse. Its only verdict is `Fatal{ProtocolCorruption}` on a malformed record. Recorded here
rather than by weakening D-I3 into a surface-handle refusal, which the brief forbids in as many
words.

**W5 — the P4a records' create/re-issue serial rule differs from
`create_vertex_elements`'s, deliberately.** P3a's vertex-elements record bumps
`ContentSerial` on *every* create including the first ("Serial 0 means never created"). The
three P4a record types follow the **contract header's** rule instead: a fresh or recycled
identity leaves `Serial` at 0 so a fresh backend twin agrees without either side publishing, and
only a re-issue on a live identity counts up. Both statements are in the tree; this package
implements the one written on the P4a records.

**W6 — a re-issued `create_shader_state` clears the default uniform block.** Not stated by the
brief either way. A relink replaces the block's layout, so an image sized to the old one is a
hazard for the consumer and the `~0u` sentinel is exactly the value that says "nothing has been
uploaded for this program". `++GlobalConstantsSerial` announces the clearing so a twin cannot
match what it uploaded before the relink.

**W7 — a respecify clears the record's `PendingUploads`.** Not stated by the brief. The pending
entries are boxes and rects in the coordinate system of a level the respecify has just
redefined; keeping one across a shrink would have Espryt upload past the end of the new store,
and nothing is lost because the frontend entry points that respecify a texture re-mark the
levels they define (`AllocateStorage` then `MarkStorageDirty`).

**W8 — `~/w7/p4a-trees2.log` never contained `REBUILT wire`, and is empty.** The rebuild
nevertheless happened and finished: `~/w7/p4a-wire-submodule.log` (11:24:24) and
`~/w7/p4a-wire-build-{linux,push,verify}-build.log` (11:24:48 / 11:25:12 / 11:25:34) each end on
the last link of a complete build (980, 991, 991 targets), no builder was still running, and a
no-op `cmake --build` over all three returned rc 0 before any edit was made. The package
proceeded on that evidence. **The summary line the orchestration script was to append is the
thing that is missing, not the work** — worth knowing before the next package waits on it.
Those four logs were deleted with the round's own (ID-5); the timestamps and byte counts above
are the record.

---

## 5. What B, C, D, E and F must know

**H1 — `MGPTextureParams::BuiltinSampler` may never be null, and bit 10 can be set while bit 11
is clear.** The contract makes a null built-in sampler `Fatal{ProtocolCorruption}` and this
package implements it. But D-K2's dependency table has **no** "bit 10 requires bit 11", and
says the mirror pair is fine — so a `MOBILEGL_PIPE_PUSH=0x5ff` lane (texture resources on,
samplers off) would emit `set_texture_params` while the sampler CSO family is switched off. If
B/C mint the built-in sampler's handle only from the sampler emitter, that lane aborts on the
first `glTexParameter`. **Either** the handle is minted whenever the texture family emits
(the allocator, not the CSO cache, is what mints it), **or** the integrator adds "bit 10
requires bit 11" to D-K2. This is the one cross-package hazard the package found and cannot fix
from inside `MG_Pipe`.

**Blob size rules, exactly.**

| record | rule |
|---|---|
| `MGPSubData` (buffer) | `Blob.Size` is 0, or exactly `MGPipeSubDataBufferSize(record)`. Anything else is Fatal. |
| `MGPSubData` (texture) | **inert.** No field describes the blob's byte length, so any value is accepted; the destination is bounded by the box and the regions. Leave it 0. |
| `MGPSamplerDesc::Parameters` | 0, or exactly `sizeof(SamplerParameters)` (100). Anything else is Fatal. The value itself travels through the companion pointer, which may never be null. |
| `MGPProgramDesc::Spirv[6]` + `Reflection` | **inert**, all seven. Nothing in the descriptor describes their lengths. Leave them 0 and pass the two artefact pointers; **both** non-null or the record is Fatal. |
| `MGPGlobalConstants::Blob` | 0, or exactly the program's `Desc.GlobalUboSize` as the create declared it. Anything else is Fatal. |

**Unit-set window semantics, exactly.** `Start + Count <= 192` or Fatal. Entries land at
`Start + i`; **entries outside the window are never cleared**, so an emitter that wants a unit
unbound must include it in the window carrying a null handle. `Count == 0` is legal and still
moves the serial. A null handle in any entry is legal. The entry's own `Unit` field is stored
verbatim and never compared against `Start + i` — if the emitter lets the two disagree, nothing
here will say so.

**Serial semantics for D and E.** All five P4a working serials (`FramebufferSerial`,
`SamplerViewsSerial`, `SamplerStatesSerial`, `ShaderImagesSerial`, `ProgramBindingSerial`) are
`MGGen`-class: they **only ever advance**, including across `MGPipeApplierReset` and
`MGPipeApplierReleaseObjectRecords`, so no value can ever recur and a twin may safely memoise
one. The per-record serials (`Serial`, `ParamsSerial`, `GlobalConstantsSerial`) restart with the
record, which is safe because the handle's `Gen` moves with it. **Every serial moves before
anything downstream is told.**

**What D reads, per kind.** `TextureResources[slot]` / `RenderbufferResources[slot]` for the
descriptor, the parameters (`Params` + `ParamsSerial`), the view handle (`ViewCso`) and the
pending uploads; `SamplerCsos[slot].Params`; `SamplerViewCsos[slot].View`;
`ShaderCsos[slot]` **or** `CompositeShaderCsos[slot − kMGPipeShaderCsoCompositeSlotBase]` — a
consumer that resolves a `ShaderCso` handle must use the same band split the applier does, and
`MGPipeIsCompositeShaderSlot` is the one predicate for it. **Espryt consumes and clears a
`PendingUploads` entry only where it actually uploads**; a bail must leave it intact, which is
the whole point of the set being server-side.

**For F.** The package adds **33** ctest names — `ResourceEmit.` +3, `FramebufferEmit.` +4,
`TextureEmit.` +6, `SamplerEmit.` +7, `ImageEmit.` +4, `ProgramEmit.` +6,
`CompositeResolver.` +3 — for 46 added against `~/w7/p4a-before-ctest-names.txt` in total (the
contract's 13 plus these 33). None is removed and every one of them is a visible SKIP in a pull
build, so `ctest -N` stays name-for-name identical between the pull and push trees.
The six suites' `main()` points `MOBILEGL_LOG_FILE_PATH` at a per-pid temp file, because the
refusal cases read the trip wire's line back out of it.

---

## 6. Verification transcript (on `3c07c8c3`, working tree clean)

```
$ python3 scripts/symbol_report.py --before ~/w7/p4a-before-libMobileGL.so \
      --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change \
      --fail-on-added-bytes 0
### Removed (0) _none_    ### Added (0) _none_
### Resized (0) _none_    ### Renamed only (same size) (0) _none_          rc=0

$ python3 scripts/gen_pipe.py --check
gen_pipe: inventory 477 rows: ... 0 UNMAPPED
gen_pipe: generated files are up to date                                  rc=0
$ python3 scripts/gen_pipe.py --self-test
gen_pipe: self-test: 7 negative-control trip(s), positive control OK      rc=0
$ git diff --exit-code -- MobileGL/MG_Pipe/generated                      rc=0

$ python3 scripts/gen_pipe_dirty_surface.py --check                       rc=0
$ python3 scripts/gen_pipe_dirty_surface.py --self-test
dirty-surface self-test: 27 negative controls, all tripped                rc=0

$ python3 scripts/check_include_closure.py --mode both --compiler clang++ \
      --self-test --require-all
include-closure: self-test: 6 negative-control trip(s), parser checks OK
include-closure: 4 probes, 0 skipped, 0 problem(s)                        rc=0

$ bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD
[p3a-untouched] the 11 ... functions are byte-identical                   rc=0

$ cmake --build build-linux  -j 12   ok
$ cmake --build build-push   -j 12   ok
$ cmake --build build-verify -j 12   ok
$ ctest --test-dir build-linux  -L unit -j 12   100% 1668/1668, 0 failed
$ ctest --test-dir build-push   -L unit -j 12   100% 1668/1668, 0 failed
$ ctest --test-dir build-verify -L unit -j 12   100% 1668/1668, 0 failed

$ diff <(build-linux names) <(build-push names)                           (empty)
$ comm -23 ~/w7/p4a-before-ctest-names.txt <(build-linux names)           (empty)
$ comm -13 ~/w7/p4a-before-ctest-names.txt <(build-linux names)           46 added
$ grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'
MobileGL/MG_Backend/MGPipe/PipeInputs.h:1     # pre-existing, unchanged
```

Two failures were found during the round and both were the gates doing their job:
`ResourceEmit.NoTextureOrRenderbufferResourceCallReachesTheBackendOpTable` (deviation W3, a real
defect in `w1`'s first draft) and one flake of `LogLevel.ProductionBuildKeepsErrorAndWarn` under
`-j 12` (green on re-run and on every subsequent full run; a pre-existing shared-temp-file race
in a suite this package does not touch).

---

## 7. Mutation evidence

**41 mutations, 41 red, measured.** Each was applied to `MobileGL/MG_Pipe/PipeApply.cpp` in
this worktree, the tree rebuilt, the family's suite run, and the file restored from the commit
(the working tree was verified clean before and after; no other worktree was created). The rule
the brief asks for — *for each entry point a case that fails if its body is deleted* — is
therefore measured rather than claimed.

| # | mutation | first case to go red |
|---|---|---|
| M1 | `set_framebuffer_state`: the draw store removed | `FramebufferEmit.ADrawRecordAndAReadRecordAreKeptApartAndBothWritesBoth` |
| M2 | `set_framebuffer_state`: the read store removed | the same |
| M3 | `set_framebuffer_state`: the serial bump removed | the same |
| M4 | `set_framebuffer_state`: the target gate removed | `FramebufferEmit.ATargetOutsideTheThreeBindingsIsRefusedNamingTheRecord` |
| M5 | `set_framebuffer_state`: the draw-buffer gate removed | `FramebufferEmit.ADrawBufferEntryOutsideTheRecordsOwnArrayIsRefusedRatherThanRead` |
| M6 | `resource_create`: every target routed to the buffer table | `ResourceEmit.TheThreeResourceKindsKeepTheirOwnSlotSpaceAndDoNotSeeEachOther` |
| M7 | `resource_destroy`: every kind routed to the buffer table | the same |
| M8 | `resource_destroy`: the texture dispatch guard removed | `ResourceEmit.NoTextureOrRenderbufferResourceCallReachesTheBackendOpTable` |
| M9 | `set_texture_params`: the store removed | `TextureEmit.ATexturesParametersLandOnItsOwnRecordAndMoveOnlyTheirOwnSerial` |
| M10 | `set_texture_params`: the serial bump removed | the same |
| M11 | `set_texture_params`: the null-sampler gate removed | `TextureEmit.ARecordWithNoBuiltinSamplerCsoIsRefusedNamingTheTexture` |
| M12 | `set_texture_params`: the resolver replaced by a silent find (the refusal stops being counted) | `TextureEmit.ATexturesParametersLandOnItsOwnRecordAndMoveOnlyTheirOwnSerial` |
| M13 | sub-data: the region-containment gate removed | `TextureEmit.TheSubDataValidatorRefusesALevelABoxAndARegionTheRecordCannotDescribe` |
| M14 | sub-data: the level bound removed | the same |
| M15 | sub-data: the encodable-box gate removed | the same |
| M16 | sub-data: the missing-tail gate removed | the same (**SegFault**, §7.1) |
| M17 | pending uploads: the box union removed | `TextureEmit.AnAccumulatedUploadUnionsItsBoxesAndCollapsesToTheBoxWhenARectListCannotDescribeIt` |
| M18 | pending uploads: the box-only collapse removed | the same |
| M19 | pending uploads: the accumulation removed | the same |
| M20 | respecify: the pending-upload clear removed | `TextureEmit.ARespecifyDropsThePendingUploadsAgainstTheStorageItReplaces` |
| M21 | `create_sampler_state`: the parameter store removed | `SamplerEmit.ACreateStoresTheParametersByValueAndAReissueOnALiveIdentityCountsUp` |
| M22 | `create_sampler_state`: the re-issue serial bump removed | the same |
| M23 | `create_sampler_state`: the blob-length gate removed | `SamplerEmit.ARecordThatDoesNotDescribeItsOwnParametersIsRefusedNamingTheLength` |
| M24 | `delete_sampler_state`: the record drop removed | `SamplerEmit.ADeleteDropsTheRecordAndAStaleNoticeIsCountedRatherThanSilentlyDropped` |
| M25 | `create_sampler_view`: the back-pointer write removed | `SamplerEmit.AViewIsReissuedOnTheSameHandleAndKeepsItsTexturesBackPointerInStep` |
| M26 | `delete_sampler_view`: the back-pointer clear removed | the same |
| M27 | the unit sets: the entry copy loop removed | `SamplerEmit.TheTwoUnitSetsLandInTheirWindowAndLeaveEverythingOutsideItAlone` |
| M28 | the unit sets: the window gate removed | `SamplerEmit.AUnitWindowPastTheMergedUnitSpaceIsRefusedRatherThanTruncated` |
| M29 | `set_sampler_views`: the serial bump removed | `SamplerEmit.TheTwoUnitSetsLandInTheirWindowAndLeaveEverythingOutsideItAlone` |
| M30 | `set_shader_images`: the serial bump removed | `ImageEmit.TheImageSetLandsInItsWindowWithEveryFieldTheShaderWasBuiltAgainst` |
| M31 | `create_shader_state`: the descriptor store removed | `ProgramEmit.ACreateStoresTheDescriptorAndARelinkCountsUpAndDropsTheBlockKeyedToTheOldLayout` |
| M32 | `create_shader_state`: the relink block clear removed | the same |
| M33 | `create_shader_state`: the artefact gate removed | `ProgramEmit.ACreateWithNoArtefactsAnOversizedBlockOrACorruptSlotIsRefusedNamingTheProgram` |
| M34 | `bind_shader_state`: the store removed | `ProgramEmit.TheThreeBindingsFollowTheirOwnHandleAndADeadOneLeavesThePreviousBindingStanding` |
| M35 | `set_draw_program`: the store removed | the same |
| M36 | `set_dispatch_program`: the store removed | the same |
| M37 | `delete_shader_state`: the binding sweep removed | `ProgramEmit.ADeleteDropsTheRecordAndClearsEveryBindingThatNamedIt` |
| M38 | `set_global_constants`: the image store removed | `ProgramEmit.ACreateStoresTheDescriptorAndARelinkCountsUpAndDropsTheBlockKeyedToTheOldLayout` |
| M39 | `set_global_constants`: the sentinel gate removed | `ProgramEmit.TheDefaultUniformBlockLandsOnTheProgramsRecordAndTheSentinelIsRefused` |
| M40 | the composite band routed into the ordinary table | `CompositeResolver.ACompositeRecordLandsInTheBandsOwnTableAndNeverGrowsTheOrdinaryOne` |
| M41 | **verify build**: the archive decode made to fail | three `ProgramEmit.*` cases, `Subprocess aborted` |

M1-M40 ran in `build-push`; **M41 ran in `build-verify`**, and it is the one that proves the
codec round trip is live code with a gate rather than a comment: with the decode failing, every
`ProgramEmit` case that creates a shader state dies on `Fatal{PipeVerifyDiffer}`.

### 7.1 A note on reading a mutation harness

Two of the forty-one (M16 and M41) kill the test **process** rather than failing an assertion —
M16 dereferences the tail it no longer refuses, M41 aborts inside the verify trip wire — so
`ctest` reports `***Exception: SegFault` and `Subprocess aborted` instead of `(Failed)`. A
harness that greps for `(Failed)` reads both as SURVIVED. They were re-run with the exit code as
the verdict (`rc=8` in both cases) and are red. **Worth knowing for anyone repeating this:
grepping ctest output for `(Failed)` silently under-reports exactly the mutations that matter
most.**

---

## 8. Left undone, and who owns it

- **Every emitter body** and the four `kMGPipeWired*Subsystem` constants — B and C, in their own
  headers. Nothing consumes a record until they land.
- **`CompositeResolver.h`** — still not created; wholly C's. This package's
  `CompositeResolverTest` cases are the applier's half of the band and are disjoint from C's.
- **`Resolve<Family>SubsystemArm()`**, D-C3's `MaxColorAttachments > 8` refusal, and the D-K2
  dependency refusals — backend-side, package D. **H1 above is an input to that work.**
- **The five `MGPipeUnmigratedEmulation` call sites** — D and E.
- **`p4a_untouched_regions.sh`**, `p4a_descriptor_negative_control.sh`, the itest phase-mask
  moves and `MagmaPipeArms.h` — package F.
- **The retain-mode comparison of the emitted `(UnionBox, RegionCount, Regions[])`** against the
  tracker's pre-clear set (part of G4): the applier's half exists — the pending set is
  accumulated and never recomputed — and the tracker's half is the client packages'.
