// MobileGL - MobileGL/MG_Util/SelfTest/DriverBugProbes.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>

namespace MobileGL::MG_Util::SelfTest {
    // ===================== KNOWN DRIVER BUGS =====================
    //
    // THIS IS THE DESIGNATED HOME FOR DRIVER-CAPABILITY LIES.
    //
    // The rest of the POST suite answers a different question: does the extension exist, and
    // does a simple probe show it working. The entries here are not extension questions at
    // all - they are CORE functionality that a driver advertises, accepts without error, and
    // then does not perform. Nothing in an extension string or a limit query says so, which
    // is exactly why each one needs its own executable probe.
    //
    // The inventory comes from CAMPAIGN FINDINGS, not from anything the driver reports.
    //
    // EVERY PROBE MUST CARRY A CONTROL. The geometry entry below is why the rule is written
    // down: the same defect was first characterised as "this driver drops all geometry-stage
    // storage-buffer writes", which would have justified withdrawing
    // GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS entirely. A control showed geometry-stage writes
    // land perfectly well when they precede EmitVertex(), so the limit is not a lie and
    // withdrawing it would have broken shaders that work today. A probe without a control
    // measures a symptom and invites exactly that over-correction.
    //
    // ADDING A SIBLING IS ONE FUNCTION: write an `Optional<DriverBugFinding> ProbeXxx(gl)`
    // that returns nullopt when the driver is not affected, and add it to the table in
    // CollectGlesKnownDriverBugs().

    // What MobileGL can do about a bug this device HAS. There is deliberately no "not
    // affected" member: a driver that passes the probe produces no finding at all, so the
    // report only ever lists bugs actually present on this device.
    enum class DriverBugVerdict : Uint8 {
        // A MobileGL quirk repairs or substitutes for the defect and the application sees
        // correct behaviour.
        Fixed,
        // There is no substitute. `detail` says what MobileGL does defensively instead, and
        // what an application can still rely on.
        Unfixable,
    };

    struct DriverBugFinding {
        // Short name of the bug, not of the feature.
        String name;
        DriverBugVerdict verdict = DriverBugVerdict::Unfixable;
        // One line: what the driver does wrong, and what MobileGL does about it.
        String detail;
    };

    // What the located-interface-block probe measured.
    struct LocatedIoBlockMeasurement {
        // The driver delivers nothing through an inter-stage interface block that carries an
        // explicit layout(location=) once a geometry stage is in the pipeline. The only field
        // any caller's behaviour depends on.
        Bool detected = false;
        // ...and it does the same WITHOUT a geometry stage, i.e. between a vertex and a
        // fragment stage. False on the device this was characterised on, and reported because
        // DirectGLES's repair is scoped to tessellation/geometry programs: a driver that
        // answered true here would be losing block payloads the repair does not reach.
        Bool alsoAffectsVertexToFragment = false;
    };

    // Draws one full-viewport triangle through VS+GS+FS whose two interface blocks carry an
    // explicit layout(location = 0), and reports whether the payload the vertex stage wrote
    // reached the fragment stage.
    //
    // The Mali-G1-Ultra ES driver (r54p1) delivers ZEROES: the stages compile, the program
    // links with an empty info log, the draw runs without error, and the block is empty. It is
    // the whole of the KHR-GLxx.shading_language_420pack interface-block group's failures on
    // that device, and of a further 21 tessellation and geometry bodies beside it.
    //
    // TWO CONTROLS, and the first is why this is a LOCATION finding rather than a block one:
    // (1) the identical three-stage program with the qualifier removed from both blocks must
    // deliver its payload - without that, "this driver cannot carry an interface block through
    // a geometry stage" would be the claim, which is false and would justify flattening every
    // block on the device; and (2) a two-stage vertex-to-fragment program with a LOCATED block
    // is measured separately, because that one works on the affected driver and is what scopes
    // the repair to programs with a tessellation or geometry stage.
    //
    // Returns `detected` false when an entry point is missing, when the driver has no geometry
    // stage, or when the unlocated control fails - an inconclusive probe must never be reported
    // as a bug, and must never arm the repair. Restores every piece of GL state it touches.
    LocatedIoBlockMeasurement ProbeLocatedIoBlocksLosePayload(const MG_External::GLESFunctionsTable& gl);

    // ProbeLocatedIoBlocksLosePayload(), evaluated at most once per process.
    const LocatedIoBlockMeasurement& LocatedIoBlocksLosePayload(const MG_External::GLESFunctionsTable& gl);

    // Blits one layer of an RGBA8 2D array onto another array's layer 1 and reports whether the
    // copy landed where it was asked to. Returns true only when the destination layer is ignored
    // while the control lands correctly.
    //
    // Adreno 830 writes to layer 0 whatever layer the DRAW framebuffer's
    // glFramebufferTextureLayer attachment names, for colour and depth alike, and raises no
    // error. Everything else about the layer works on the same driver, which is what makes this
    // a blit defect rather than a layered-attachment one.
    //
    // THE CONTROL is the same blit onto destination layer 0. It passes on every implementation
    // that can blit between array layers at all, and because the value it looks for exists only
    // on the SOURCE's layer 1 it also proves the source layer is honoured - so a driver with no
    // working glFramebufferTextureLayer reaches no verdict instead of being reported as this.
    //
    // Returns false when an entry point is missing, when the probe's own framebuffers come back
    // incomplete, or when the control fails. Restores every piece of GL state it touches.
    Bool ProbeBlitIgnoresDestinationArrayLayer(const MG_External::GLESFunctionsTable& gl);

    // ProbeBlitIgnoresDestinationArrayLayer(), evaluated at most once per process.
    Bool BlitIgnoresDestinationArrayLayer(const MG_External::GLESFunctionsTable& gl);

    // What the vertex-input location probe measured. The ceiling is reported rather than
    // hard-coded: it is a driver property, and a clamp derived from a number measured on some
    // other device is exactly the hard-coded vendor quirk this file exists to avoid.
    struct VertexInputLocationCeilingMeasurement {
        Bool detected = false;
        // GL_MAX_VERTEX_ATTRIBS as the driver answers it.
        Int advertisedMaxVertexAttribs = 0;
        // How many locations `layout(location = N)` on a vertex input actually accepts, i.e. the
        // highest N that compiles plus one. Equal to advertisedMaxVertexAttribs when the driver
        // is not affected, and when the probe reached no verdict - so a caller can clamp to it
        // unconditionally and an inconclusive probe changes nothing.
        Int usableLocations = 0;
        // Whether glBindAttribLocation(advertisedMaxVertexAttribs - 1) still links and resolves.
        // Only measured when `detected`; see the second control in the .cpp for why it decides
        // what the finding is allowed to claim.
        Bool bindAttribLocationReachesAdvertisedMax = false;
        // The first line of the driver's compile log for a refused declaration, so the report
        // quotes the driver rather than paraphrasing it.
        String driverMessage;
    };

    // Compiles `layout(location = N) in vec4` on its own at a series of N and finds the highest
    // one the driver's ESSL compiler accepts.
    //
    // Adreno 830 advertises GL_MAX_VERTEX_ATTRIBS = 32 and then refuses the qualifier for every
    // N >= 16 ("the location is not within attribute range [0, MAX_ATTRIBUTES-1]"), for float and
    // integer inputs alike - so half the attributes it advertises cannot be declared. MobileGL
    // emits vertex inputs as layout qualifiers, which makes the advertised count a promise it
    // cannot keep; the measured ceiling is what it advertises instead.
    //
    // TWO CONTROLS. Location 0 must compile, or the probe has measured its own failure rather
    // than the driver's. And glBindAttribLocation at the advertised maximum is tried separately,
    // because that is what separates "only the layout qualifier is capped" (which is what this
    // driver does) from "the attributes are not there at all" - two findings that justify the
    // same clamp but very different report text.
    //
    // Compile-only, and bisected: one shader compile on a conforming driver, about seven on an
    // affected one. Returns a measurement with `detected` false and `usableLocations` equal to
    // the advertised count when an entry point is missing or a control fails, so an
    // inconclusive probe never withdraws anything.
    VertexInputLocationCeilingMeasurement ProbeExplicitVertexInputLocationCeiling(
        const MG_External::GLESFunctionsTable& gl);

    // ProbeExplicitVertexInputLocationCeiling(), evaluated at most once per process.
    const VertexInputLocationCeilingMeasurement& ExplicitVertexInputLocationCeiling(
        const MG_External::GLESFunctionsTable& gl);

    // Draws one point through VS+GS+FS whose geometry stage writes two storage buffers: one
    // BEFORE its EmitVertex()/EndPrimitive() and one AFTER. Returns true only when the
    // before-emit write lands and the after-emit write does not.
    //
    // The before-emit write is the control, and it is the whole point of the probe. Adreno 830
    // discards geometry-stage storage writes issued after the last emit while performing the
    // identical write issued before it (measured both ways, and for both point and triangle
    // geometry shaders, so the primitive shape is not the variable). Reading only the
    // after-emit half would say "geometry storage writes do not work on this driver", which is
    // false and would justify withdrawing a limit applications legitimately use.
    //
    // Deterministic by construction - the write either reaches memory or the driver
    // structurally discards it - so the answer is latched, not sampled. Returns false when the
    // driver advertises no geometry storage blocks, when an entry point is missing, or when
    // anything about the probe fails to set up: an inconclusive probe must never be reported
    // as a bug. Restores every piece of GL state it touches.
    Bool ProbeGeometryStageSsboWriteAfterEmitDropped(const MG_External::GLESFunctionsTable& gl);

    // ProbeGeometryStageSsboWriteAfterEmitDropped(), evaluated at most once per process.
    Bool GeometryStageSsboWriteAfterEmitDropped(const MG_External::GLESFunctionsTable& gl);

    // Samples one R32F GL_TEXTURE_2D_MULTISAMPLE texel through a swizzled alpha channel, twice,
    // with a separately linked program each time. Returns true only when the swizzled read goes
    // wrong while every control read stays right.
    //
    // Adreno 830 returns uninitialised memory - a different value every run - for
    // texelFetch(sampler2DMS, ..., sampleIndex != 0).w on an R32F multisample texture whose
    // GL_TEXTURE_SWIZZLE_A is not the default, from the SECOND such program in the context
    // onward. The first program reads correctly, which is why the probe links two.
    //
    // THREE CONTROLS, each identical to the subject but for one variable, and all three must
    // read correctly for a wrong subject to count: (1) the same fetch with
    // GL_TEXTURE_SWIZZLE_A left at its default, (2) the same fetch at sample index 0, and
    // (3) the same swizzled texture read through .x instead of .w. Without them a driver that
    // simply cannot render R32F, or cannot sample multisample textures at all, would be
    // reported as having this very specific corruption.
    //
    // Returns false when the driver cannot host the shape (no multisample R32F colour target,
    // fewer than two samples, a missing entry point, an incomplete framebuffer): an
    // inconclusive probe must never be reported as a bug. Restores every piece of GL state it
    // touches.
    Bool ProbeR32FMultisampleSwizzleCorruption(const MG_External::GLESFunctionsTable& gl);

    // ProbeR32FMultisampleSwizzleCorruption(), evaluated at most once per process.
    Bool R32FMultisampleSwizzleCorrupted(const MG_External::GLESFunctionsTable& gl);

    // What the image-location budget probe measured. `detected` is the only field the verdict
    // depends on; the rest exist so the report can say what the shape was instead of asserting
    // a number that was true on one device in one campaign.
    struct ImageLocationBudgetMeasurement {
        Bool detected = false;
        // Image uniforms declared per stage in both the subject and the control - one more than
        // GL_MAX_GEOMETRY_IMAGE_UNIFORMS, which is the smallest of the three stages' budgets.
        Int perStageImageUniforms = 0;
        // Distinct uniform NAMES in the subject (per-stage-unique) and in the control (shared).
        Int subjectDistinctNames = 0;
        Int controlDistinctNames = 0;
        // The first line of the driver's info log for the failing link, so the report quotes the
        // driver rather than paraphrasing it.
        String driverMessage;
    };

    // Links the same three-stage (vertex, geometry, fragment) program twice: once with every
    // stage naming its image uniforms uniquely, once with all three stages sharing one set of
    // names. Both declare the same number of image uniforms per stage, on the same bindings,
    // with the same qualifier and the same stores - the names are the only difference.
    //
    // Adreno 830 charges its image-location budget per distinct NAME, so the shared-name program
    // links while the per-stage-named one is rejected with "Image location or component exceeds
    // max allowed", even though nothing about the image USAGE changed. That is what makes the
    // shared-name link the control: it proves the driver can host this exact amount of image
    // work and that only the naming moved the answer.
    //
    // `detected` is false unless the subject fails AND the control links. Both failing means the
    // shape is simply too large for the driver (an honest refusal); both linking means the
    // driver does not have this bug.
    ImageLocationBudgetMeasurement ProbeImageLocationPerNameBudget(const MG_External::GLESFunctionsTable& gl);

    // ProbeImageLocationPerNameBudget(), evaluated at most once per process.
    const ImageLocationBudgetMeasurement& ImageLocationPerNameBudget(const MG_External::GLESFunctionsTable& gl);

    // Draws one quad whose vertex stage stores to a `coherent writeonly` image and whose
    // fragment stage reads the same image declared `coherent readonly` under the SAME name, then
    // checks every fragment saw the store. Returns true only when the same-name program loses
    // the store while the different-name control keeps it.
    //
    // Adreno 830 merges the two declarations into one uniform and silently discards the writing
    // stage's stores. The control is the identical pair of shaders with the two halves renamed -
    // exactly what MobileGL's image-uniform repair emits - which keeps every store. Without it
    // the probe would be indistinguishable from "this driver cannot store to images from the
    // vertex stage", which is a different and much larger claim.
    //
    // Returns false when the driver advertises no vertex-stage image uniforms, when an entry
    // point is missing, or when the setup fails.
    Bool ProbeCrossStageImageQualifierMergeDropsWrites(const MG_External::GLESFunctionsTable& gl);

    // ProbeCrossStageImageQualifierMergeDropsWrites(), evaluated at most once per process.
    Bool CrossStageImageQualifierMergeDropsWrites(const MG_External::GLESFunctionsTable& gl);

    // What the image coherency probe measured. The residual is reported rather than hard-coded:
    // it is a rate, it differs between devices, and a report that quotes a number measured
    // somewhere else is worse than no number at all.
    struct ImageCoherencyResidualMeasurement {
        Bool detected = false;
        // Texels the STRONGEST in-shader shape missed - that is what makes the defect unfixable.
        Int mismatchedTexels = 0;
        // Texels the shape MobileGL emits today missed, on the same driver in the same run. It
        // is what applications actually get, and it is not always the same number.
        Int emittedShapeMismatchedTexels = 0;
        Int totalTexels = 0;
    };

    // Counts the texels whose dependent imageLoad() did not observe the imageStore() that
    // precedes it in the same fragment invocation.
    //
    // THE SUBJECT IS THE STRONGEST SHAPE THE LANGUAGE OFFERS - a `coherent volatile`
    // readonly/writeonly pair on one binding with BOTH memoryBarrierImage() and memoryBarrier()
    // between the store and the read - and that choice is the whole reason the row can say
    // "unfixable". Probing only the shape MobileGL emits today (`coherent` plus
    // memoryBarrierImage()) reports a bug on drivers where simply adding `volatile` makes the
    // read correct, which is a defect MobileGL could fix rather than one it cannot: measured on
    // Mesa llvmpipe, the emitted shape misses every texel while the `volatile` shape misses
    // none. Only a driver that fails even the strongest shape has no in-shader substitute left.
    //
    // The control is the same dependency split across TWO draws with a glMemoryBarrier and a
    // glFinish between them. It separates "this driver cannot make image writes visible at all"
    // (control also dirty - a far worse defect, and the probe declines to call it this one) from
    // the finding, which is about ordering inside one invocation.
    //
    // `detected` is false unless the strongest shape is dirty AND the control is clean. The
    // shape MobileGL emits is measured either way, so the report can say what applications get.
    ImageCoherencyResidualMeasurement ProbeImageWriteReadCoherencyResidual(
        const MG_External::GLESFunctionsTable& gl);

    // ProbeImageWriteReadCoherencyResidual(), evaluated at most once per process.
    const ImageCoherencyResidualMeasurement& ImageWriteReadCoherencyResidual(
        const MG_External::GLESFunctionsTable& gl);

    // Copies one known GL_UNSIGNED_SHORT_5_5_5_1 word out of both mip levels of a GL_RGB5_A1
    // 2D array into plain 2D images with glCopyImageSubData and reads the landed texels back
    // - for SEVERAL ALLOCATION RECIPES of the same array. Returns true only when a copy from
    // any level of any recipe delivers the word's 5_5_5_1 <-> 1_5_5_5_REV field-order mirror
    // while the controls below hold.
    //
    // The affected Mali stores SOME 16-bit packed allocations (RGB565 / RGB5_A1 / RGBA4) with
    // their fields packed from the other end of the word. The mirrored layout is an
    // ALLOCATION property, not a mip-level one - the failing device delivers the mirror from
    // level 0 and level 1 alike, which is what vetoed the first deployment's "level 0 is the
    // clean control" design - and WHICH allocations get it is a heuristic keying on texture
    // state during the uploads and on context history, measured to INVERT between a raw
    // standalone context and MobileGL's live one. No single allocation recipe is therefore
    // entitled to speak for the layout: the probe builds the CTS's failing shape (three-level
    // chains both endpoints: 30/15/7 x12 array, 7/3/1 plain, FUNCTIONAL_TEST_N_LEVELS = 3)
    // with three recipes - uploads-first (the minted-backend-texture order), params-first,
    // and the CTS copy-test shape (MAX_LEVEL bounded, MIN_FILTER left mipmapped-default) -
    // in situ, in the very context the application's copies run in. Uploads and readbacks
    // decode each image's own layout consistently, so nothing but a raw texel-block move can
    // see the divergence - which is exactly what glCopyImageSubData is defined to be, and why
    // the whole KHR-GL4x.copy_image rgb5/rgb5_a1/rgba4 x *2d_array* matrix fails there while
    // every other suite touching these formats passes.
    //
    // CONTROLS. The machinery: an identical copy between two SAME-shape plain-2D images,
    // which share a layout whatever it is, so it must deliver the word on any driver that can
    // run copy_image on these formats - a driver that cannot reaches no verdict instead of
    // being reported as this. And per recipe, the array's own round trip: a direct FBO
    // readback of its level 1 must answer the word, or the UPLOAD is what corrupts - a
    // different defect. The subjects must also match the mirror PREDICTION, not merely differ
    // from the word - a copy that delivered anything else is a different defect and reaches
    // no verdict either. Restores every piece of GL state it touches.
    Bool ProbeCopyImageMirrorsPacked16FieldOrder(const MG_External::GLESFunctionsTable& gl);

    // ProbeCopyImageMirrorsPacked16FieldOrder(), evaluated at most once per process. The
    // DirectGLES format normalization consults this to decide whether the three 16-bit packed
    // normalized formats must be stored as 8-bit-per-channel ES storage (see
    // PixelFormatNormalizeOptionBit::WidenPacked16Norm).
    Bool CopyImageMirrorsPacked16FieldOrder(const MG_External::GLESFunctionsTable& gl);

    // Every known driver bug this GLES driver actually has. Bugs it does not have are absent,
    // so an unaffected device renders an empty section rather than a wall of "not affected".
    Vector<DriverBugFinding> CollectGlesKnownDriverBugs(const MG_External::GLESFunctionsTable& gl);
} // namespace MobileGL::MG_Util::SelfTest
