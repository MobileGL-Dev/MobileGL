# Package fb — framebuffers, attachments, images, blit from records (P5e wave 2)

Read in this order, all of it: `PACKAGE-PREAMBLE-P5E.md`, `INTEGRATOR-DECISIONS-P5E.md` (ID-86,
ID-94 are yours), `BRIEF-P5E.md` (§0, §1, §2 "fb", §5, §6 rulings 7, 15, 16), `CONTRACT-P5E.md`
§5.4, your scout report `scout-S3.md` (all of it), and the framebuffer / image rows of
`kimi-audit.md` (rows 68-90) — report per row: retired / left barriered / not found. Notes:
`//wsl.localhost/Arch/home/swung/w7/notes/p5e/`.

Your slug is `fb`; your tree is `~/w7/p5e-fb` (branch `p5e/fb`, based on the landed id head, built).
You CALL tx2's `SyncTextureToBackendByHandle` / `SyncMipmapsToBackendByHandle` (declared by c0e with
asserting bodies; tx2 writes them in parallel) — code against the declarations and say so in the
report; the integrator resolves the order at landing.

## What "done" means for this family

The FBO and RBO twins resolve from the record (`st.BoundFramebuffer[t]` →
`FramebufferRecords[handle]`), never from the binding slot or the frontend object; the draw-FBO and
read-FBO attachment lists are keyed by `{record.Fbo, ContentHash}` with surface handles instead of
borrowed attachment `SharedPtr` slots — this is the one unconditional per-draw live read of your
family, so it is the package's core; `SyncAttachmentSurface` takes an `MGPSurface`, not a frontend
attachment; the named blit resolves both endpoints from the two records; images come from
`BoundShaderImages[]` with `Access` DECODED (ruling 16 / ID-94: the client already encodes it, the
server comment is stale) and the sweep gate reads applier serials (ruling 7 / ID-86:
`ShaderImagesSerial`, `TextureShutterSerial`, `ContextSerial`, `g_backendContextGeneration` — keep
the window/high-water union); `MarkWritableImageBufferTexturesGpuWritten` is deleted under a
transport (one edit paired with sb's deletion of the backend GPU-write marks — coordinate in the
reports).

## Scope

`DirectGLES.cpp`: the doors, the list keys, `SyncReadFramebufferTextureAttachments`, the draw-FBO
list, the image block (`SyncImageTextureBinding(const MGPImageView&)`, `ResolveShaderImageRecord`
deleted, the sweep gate), `SyncCurrentFBO*`, `BindCurrentFBO` / `SyncAndBind*` / `ForceBindCurrentFBO`,
`Clear`'s FBO lines, both blit arms, the detach walk + the new reverse index,
`SyncRenderbufferObjectToBackend` by handle. `Managers.cpp` / `Managers.h`: the FBO/RBO twin bodies,
the reverse index (texture → FBO slots) tx2 needs for detach. Tests:
`MG_Test/Pipe/FramebufferEmitTest.cpp` (the server twin of the existing client case),
`MG_Test/Pipe/ImageEmitTest.cpp` (the access constants pinned on BOTH sides), and a new
default-framebuffer-resize re-emit case (your scout verified the resize does re-emit — pin it).

## Rulings that bind you

- ID-94 / ruling 16: `MGPImageView::Access` IS encoded client-side; move the decode to
  `MGPipeValueTypes.h` (c0e landed the enum) and delete the stale server comment.
- ID-86 / ruling 7: the sweep gate is the four applier/backend serials, never a backend re-mint
  counter; keep the union of the record window with the image-unit high-water mark.
- Your scout's open question on `SyncReadFramebufferTextureAttachments` being ungated on the
  framebuffer subsystem bit: its retirement changes behaviour in BOTH arms of the object A/B — name
  the delta in the report and re-baseline that A/B rather than hiding it.
- The renderbuffer cross-check that runs a client-allocator `HandleOf` on the apply thread under
  split: fix it as part of this package and say whether it was a latent correctness bug.

## Red-once (execute, revert, quote)

1. `SyncAttachmentSurface` takes the frontend attachment again → `strict` names
   `Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot"}` on `SnormAttachmentScenario`.
2. The draw-FBO list key back to the pointer compare → the id guard's `Fatal{RoleViolation, "MGPipeSlots"}`
   (id's hook forces "unbarriered").
3. Map the image `Access` 1↔2 → `FormatlessImageBakeScenario` / `ImageLoadStoreSsoScenario` red plus
   the unit case.
4. The named blit back on the frontend objects → `CopyImageLayeredScenario`,
   `LayeredAttachmentShapeScenario`.
5. Keep the per-point version memo as a second gate → `CrossFrameBufferScenario` / the P4a seam
   scenario must stay green WITHOUT it.

## Gate

`build`, `unit`, `isplit`, `gens`; `strict` (record the markers that disappeared); `one` on the FBO,
blit and image scenarios you touch; the seam greps your scout listed must come back silent. Say
explicitly whether a blit is still barriered after your change (your scout argued it need not be —
the contract's predicate decides, and ra owns the flip).
