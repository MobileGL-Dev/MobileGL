// MobileGL - MobileGL/MG_IntegrationTest/Harness/P4aSeamPeek.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The three readings P4aSeamAuditScenario.cpp takes from the inside, for the two seams the fable
// seam audit proved that PUBLIC GL CANNOT SEE: F-4 (the record arm's sampler bind is a permanent
// no-op, hidden by the pre-handle program pass binding the same values) and F-2 / SD-4 (the
// shader-image window does not follow a program switch, hidden by the server's window/high-water
// union taking the pre-handle bind for the units outside it). Both are correct pictures over a
// permanent silent fallback, which is precisely the class ROADMAP.md:20 says a gate has to be
// able to make red - and the only place the difference exists is inside.
//
// A SEPARATE TRANSLATION UNIT for PipeApplyPeek.h's reason, verbatim: this file includes
// Espryt's own Managers.h, which may not meet a scenario's GL headers in one file. It is NOT
// PipeApplyPeek.cpp because that file is package F's (gates v3) and this round may not edit it.
//
// EVERY ENTRY POINT RETURNS false, TOUCHING NOTHING, WHERE IT CANNOT LOOK - a pull build, Android,
// a backend that is not Espryt - and a caller that gets false has learned NOTHING: "could not
// look" is not "was bound". The scenario declines the reading BY NAME and keeps its public-GL
// half, which is the shape TextureParamsWithoutASamplerViewScenario.cpp argues for.

#pragma once

namespace MGITest {

    // ---- is Espryt's sampler family on its HANDLE arm in this process? -------------------
    //
    // The gate for every other reading here. True only on DirectGLES, in a push build, with
    // Espryt's own resolver answering "handle" for kMGPipeSubsystemSamplers (bit 11 set and its
    // dependency satisfied) - i.e. exactly when bind_sampler_states / set_shader_images are
    // consumed, so a white-box assertion about them can be red for its own reason and for no
    // other. Written only on true.
    bool PeekEsprytSamplerHandleArmIsLive(bool* outLive);

    // ---- the applier's shader-image window, as last received ------------------------------
    //
    // MGPipeApplierState::ShaderImageStart / ShaderImageCount / ShaderImagesSerial. Count is
    // "how many units set_shader_images last described" - 0 means the set has NEVER arrived
    // (MGPipeApplierReset advances the serial whether or not anything was emitted, so the serial
    // is not that test). Push build only.
    struct PipeShaderImageWindowPeek {
        unsigned Start;
        unsigned Count;
        unsigned long long Serial;
    };

    bool PeekPipeShaderImageWindow(PipeShaderImageWindowPeek* out);

    // ---- which driver sampler a texture unit is bound to, and whose twin it is -------------
    //
    // For F-4. `BoundSamplerId` is the ES sampler name Espryt's own binding shadow says unit
    // `unit` carries (0 = none). `CsoHandleSlot/Gen` is bind_sampler_states' handle for the unit,
    // `CsoTwinSamplerId` the ES name of the twin Espryt holds AT THAT HANDLE (0 = no twin at the
    // content-addressed slot - the F-4 shape), and `IdentityTwinSamplerId` the ES name of a twin
    // keyed on the frontend SamplerObject named `glSamplerName` (0 = none). On a correct handle
    // arm the unit's driver sampler IS the CSO twin. Push build, DirectGLES only.
    struct EsprytUnitSamplerPeek {
        unsigned BoundSamplerId;
        unsigned CsoHandleSlot;
        unsigned CsoHandleGen;
        bool UnitInsideWindow;
        unsigned CsoTwinSamplerId;
        unsigned IdentityTwinSamplerId;
    };

    bool PeekEsprytUnitSampler(unsigned unit, unsigned glSamplerName, EsprytUnitSamplerPeek* out);

} // namespace MGITest
