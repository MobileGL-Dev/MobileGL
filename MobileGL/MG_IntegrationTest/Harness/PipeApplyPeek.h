// MobileGL - MobileGL/MG_IntegrationTest/Harness/PipeApplyPeek.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The APPLIER's texture-parameter record, ESPRYT's applied value for the same texture, and
// whether that texture has a sampler view yet. Three readings taken from a scenario, for gate
// G9's WHITE-BOX half.
//
// WHY A WHITE-BOX HALF EXISTS AT ALL (ID-19, brief section F, gates review R1). G9's public-GL
// cases in TextureParamsWithoutASamplerViewScenario.cpp catch "the parameter never reached the
// driver". They CANNOT catch "the parameter reached the driver LATE", because a texture
// parameter's only public-GL observable is a SAMPLE and the sample is itself what repairs an
// unsynced parameter: it puts the texture on the unit list, and that walk pushes the parameters
// for anything whose params serial moved. A backend that deferred every attachment-only
// texture's parameters to the first sampler view would be green on all four of those cases,
// forever, on every tree. The distinction only exists on the inside, so the reading has to be
// taken there - while the texture is still attachment-only, before any sample.
//
// A SEPARATE TRANSLATION UNIT for PipeSlotPeek.h's reason, verbatim: the scenario sources
// include the GL headers with prototypes and MobileGL's umbrella header is not meant to meet
// them in one file. This one goes further than PipeSlotPeek and includes Espryt's own
// Managers.h, which is exactly why it may not be anywhere near a scenario's GL headers.
//
// EVERY ENTRY POINT RETURNS false, TOUCHING NOTHING, WHERE IT CANNOT LOOK, and a caller that
// gets false has learned NOTHING - "could not look" is not "was applied". Out of reach means:
// a PULL build (there is no applier: it is `#if MOBILEGL_PIPE_PUSH`); Android, where this module
// links the shipping libMobileGL.so built -fvisibility=hidden and no internal symbol resolves;
// a backend that is not DirectGLES (Espryt is the subject; Magma answers the same GL question
// through P7's own paths); and, for the record peek, a mask whose texture-resource bit is off,
// where no record exists to find because nothing was ever emitted.

#pragma once

namespace MGITest {

    // ---- the applier's set_texture_params record for one GL texture name ------------------
    //
    // ADDRESSED BY GL NAME, and the search key is MGPResourceDesc::GlNameForDiag. That field is
    // diagnostics-only by contract - never an identity, never a memo key (MGPipeTypes.h) - and
    // this is a diagnostic: a test harness looking for the record a named GL object produced.
    // The alternative would be to ask the CLIENT emitter for the texture's handle, and the
    // review is explicit that this probe must arm on package D's applier/backend state and not
    // on the emitter markers B and C set: they are different questions, and a shared marker
    // would re-create the shape review F-M5 was raised about.
    struct PipeTextureParamsRecordPeek {
        // The handle the record sits at, so a caller can print it.
        unsigned Slot;
        unsigned Gen;
        // set_texture_params' own serial. 0 means the record exists (the resource was created)
        // but NO set_texture_params has ever been applied to it - which is a different finding
        // from "no record", and the two must not be merged.
        unsigned long long ParamsSerial;
        // MGPTextureParams::Swizzle[4], translated to the GL enums the application passed to
        // glTextureParameteri (GL_ZERO / GL_ONE / GL_RED / GL_GREEN / GL_BLUE / GL_ALPHA), so
        // the scenario compares what it set against what the record carries in ONE vocabulary
        // and neither side has to know the other's encoding.
        int Swizzle[4];
        // MGPTextureParams::DepthStencilMode, translated the same way: GL_DEPTH_COMPONENT or
        // GL_STENCIL_INDEX.
        int DepthStencilMode;
    };

    bool PeekPipeTextureParamsRecord(unsigned glTextureName, PipeTextureParamsRecordPeek* out);

    // ---- Espryt's APPLIED value for the same texture --------------------------------------
    //
    // Read from the DRIVER, through the twin's own ES name, because "applied" means the driver
    // was told - the same thing package D's white-box unit probe asserts against its mocked
    // driver (esprytobj-v2 (9)). The current binding on the ACTIVE unit is saved and restored
    // around the query and no unit is switched, so Espryt's binding shadow still describes
    // reality afterwards: nothing is perturbed for it to be stale about.
    //
    // `glTarget` is the texture's GL target (only GL_TEXTURE_2D is supported today; any other
    // target returns false rather than guessing a binding query).
    struct EsprytAppliedTextureParamsPeek {
        // The driver name Espryt minted for this texture, for the caller's message.
        unsigned BackendTextureId;
        int Swizzle[4];
        int DepthStencilMode;
        // False when the driver rejected the depth/stencil query - a non-depth texture, or an ES
        // level without GL_DEPTH_STENCIL_TEXTURE_MODE. The swizzle half is still valid.
        bool DepthStencilModeIsReadable;
    };

    bool PeekEsprytAppliedTextureParams(unsigned glTextureName, unsigned glTarget,
                                        EsprytAppliedTextureParamsPeek* out);

    // ---- and the claim that makes the two above mean anything ------------------------------
    //
    // Whether Espryt holds a SAMPLER VIEW twin for this texture. This is the assertion the
    // public-GL cases cannot make, because making it there would create the view. `*outExists`
    // is written only on true.
    bool PeekEsprytHasSamplerViewForTexture(unsigned glTextureName, bool* outExists);

    // ---- c0f's belt, for the ObjectSubsystemControl arms -----------------------------------
    //
    // MGPipeApplierState::RefusedNoConsumer: the number of P4a-family entry points that were
    // refused because no backend had registered MGPipeResourceOps. On a backend WITH a consumer
    // it must never move; on one without (Magma, ID-39/ID-40) the client's own gate is supposed
    // to stop the emission before the belt is reached, so it must never move there either. A
    // non-zero delta says the gate and the belt disagreed, which is the whole point of having
    // both. Reset by MGPipeApplierReset, so a caller reads it as a DELTA and treats a value that
    // went DOWN as "the applier was reset, count everything since as `after`".
    bool PeekPipeApplierRefusedNoConsumer(unsigned long long* outCount);

} // namespace MGITest
