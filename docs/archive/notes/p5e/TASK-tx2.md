# Package tx2 — textures and samplers by handle and window (P5e wave 2)

Read in this order, all of it: `PACKAGE-PREAMBLE-P5E.md`, `INTEGRATOR-DECISIONS-P5E.md` (ID-86,
ID-93, ID-95 are yours), `BRIEF-P5E.md` (§0, §1, §2 "tx2", §5, §6 rulings 6, 14, 19),
`CONTRACT-P5E.md` §5.2 and §5.3, your scout report `scout-S2.md` (all of it), and the texture /
sampler rows of `kimi-audit.md` (rows 35-67) — report per row: retired / left barriered / not found.
Notes: `//wsl.localhost/Arch/home/swung/w7/notes/p5e/`.

Your slug is `tx2`; your tree is `~/w7/p5e-tx2` (branch `p5e/tx2`, based on the landed id head,
built). id landed `ResolveTextureTwin(MGPipeHandle)`; c0e declared `SyncTextureToBackendByHandle`
and `SyncMipmapsToBackendByHandle` with asserting bodies — you write them, and fb calls them.

## What "done" means for this family

Your scout's headline: the SAMPLER half is already run-ahead clean on its record arm
(`BindCurrentUnitSamplers` + `ResolveSamplerCsoTwin`) — that is the shape you bring to the TEXTURE
half. After this package: the per-draw unit work list is built from `st.BoundSamplerViews[]`
(handles), not from borrowed pointers into the frontend texture units; `PairingsIntact` and
`IsDrawSyncClean(frontend)` are replaced by `IsDrawSyncCleanByRecord(handle, record)` reading the
twin's own serials; every `HandleOf(frontendTexture)` re-entry is `ResolveTextureTwin(handle)`;
`GetTarget` / `IsTextureView` / the texbuffer backing all come from the descriptor; `GenerateMipmap`
resolves `VerbMipRes` and can drop its wait (ruling 14 — flip the row's `WaitClass` to `kWaitNone`
only if your arm is complete, else leave it and list it as trailing); the identity sampler family
(the apply-thread mint for a unit not covered by `bind_sampler_states`) is deleted and becomes a
named refusal.

## Scope

`DirectGLES.cpp`: `SyncTextureToBackendByHandle` (new), the unit epoch/capture pair (monolith-only;
the decline arm becomes a refusal), `UnitTextureSyncEntry`'s re-shape (push-only) and the unit half
of `SyncNeccessaryTextures`, `ResolveAndBindUnitTextures`, `BindCurrentUnitSamplers`,
`BindCurrentTextures`' memo key (ruling 6: `SamplerViewsSerial` AND pg's program key — pg states
its half in its report), the sampler pass (the mint deleted), `GenerateMipmap` by `VerbMipRes`, the
mip-shape `HandleOf` sites; trailing and NOT yours now: the CopyTex endpoint and `GetTexImage`
(barriered, P8/P9). `Managers.cpp`: `RequireImageBindableStorage`'s entry refusal,
`SyncMipmapsToBackend(handle, record)`, the texture registry re-entries, the sampler identity family
under transport, `ResolveSamplerCsoTwin` (your model). `Managers.h`: `IsDrawSyncCleanByRecord` and
the twin's serial members. `MG_Backend/DirectGLES/MipmapStorage.cpp`: the guard accessor list widened.
`MG_Remote/Server/PipeApplier.cpp`: ONE function `CheckUnitWindows(st)` called from `OnDrawVbo` and
`OnLaunchGrid` (ruling 19 / ID-95). Tests: `MG_Test/Wire/RemoteClientTest.cpp`, `MG_Test/SanityTest.cpp`
(+ an ABA sibling), keep `SampledSetStalenessScenario` and `TextureParamsWithoutASamplerViewScenario`.

## Rulings that bind you

- ID-95 / ruling 19 (A8, blocking for your family): the `set_sampler_views` window MUST cover
  `[0, MaxTouchedTextureUnit]`; the sink checks it at every draw/dispatch; narrower is
  `Fatal{ProtocolCorruption, "SetSamplerViews.Count"}`. The emitter already emits
  `Start=0/Count=maxTouched+1` — your job is to make the contract's promise checkable and checked.
- ID-86 / ruling 6: the memo key is both halves.
- Behaviour narrowing you must NAME in the report (G1 §): driving the work list off
  `BoundSamplerViews` syncs only the program-resolved texture per unit, where today the walk syncs
  every slot of every touched unit. Say what stops being synced and why that is correct (or refuse).
- Your scout's R4: the new clean gate reads both the wire's resync bits and the twin's own force
  flags — make "neither side clears the other's" explicit in code and in the report.
- Your scout's R5 (S1 boundary): the buffer-texture arm still hands a frontend `BufferObject` to
  `EnsureBufferResourceForHandle` for the emulated-persistent-map question — resolve it with
  `Desc.BufferForTexBuffer` or state precisely why it must stay.

## Red-once (execute, revert, quote)

1. Revert any handle arm → the widened `MipmapStorage` / buffer-arm guard aborts by accessor name.
2. Revert the window arm of `ResolveAndBindUnitTextures` → `strict` names
   `Fatal{UnmigratedPipeInput, "GetTextureUnitObject@DrawVbo"}`.
3. ABA: recycle a slot to `{s, g+1}` while the old entry is in the work list → the twin re-mints;
   skip the Gen reset → red.
4. Narrow the emitter's `Count` by one → `Fatal{ProtocolCorruption, "SetSamplerViews.Count"}`.
5. `GenerateMipmap` with the row unwaited and the unit walk restored →
   `Fatal{UnmigratedPipeInput, "GetActiveTextureUnit@..."}` (`F1WireScenario.GenerateMipmap*Pixels`).

## Gate

`build`, `unit`, `isplit`, `gens`; `strict` (record the markers that disappeared); `one` on the
texture/sampler scenarios you touch. If `PipeSlotPeek` (or an equivalent) can assert zero
registry-scope entries per frame for this family, add that assertion.
