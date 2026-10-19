// MobileGL - MobileGL/MG_Backend/DirectGLES/NarrowIntegerSeeds.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "IntegerAttributePlan.h"

#include <Includes.h>
#include <MG_Pipe/MGPipeHandles.h>
#include <MG_Pipe/PipeApply.h>

// The program half of IntegerAttributePlan: the two width uniforms a program carries once some
// draw has fed it 8/16-bit data of the other signedness, and the per-draw plan that drives them.
//
// A program is built with the seeds only when it needs them ("armed"), so a program that never
// meets such data keeps its ESSL byte for byte. Arming is sticky for the program object's
// lifetime, so data alternating in signedness rebuilds it at most once and from then on moves two
// uniforms. A program that captures transform feedback and has integer inputs is armed at its
// first build, so arming never relinks a program in the middle of a capture.
//
// Every failure falls back to the values the driver gives today: a build whose ESSL lacks either
// uniform, or whose link dropped one, is "refused" and its draws keep the application's type for
// 8/16-bit data; a build that fails outright disables arming for the object, and it is rebuilt
// without the seeds.
namespace MobileGL::MG_Backend::DirectGLES {
    struct NarrowIntegerSeedState {
        Bool wanted = false;
        Bool disabled = false;
        // What the last build ran with and what it verified.
        Bool built = false;
        Bool declared = false;
        Bool available = false;
        Int widths8Location = -1;
        Int widths16Location = -1;
        // The values the linked program holds; a link resets them to 0.
        Uint32 pushed8 = 0;
        Uint32 pushed16 = 0;

        Bool Target() const { return wanted && !disabled; }
        // The program's next build has to change its arming.
        Bool Stale() const { return Target() != built; }
        // An armed build that could not give the program working seeds.
        Bool Refused() const { return disabled || (built && !available); }
    };

    // The program's 32-bit int and uint vertex inputs, memoized on the record.
    IntegerVertexInputMasks IntegerVertexInputsOf(const MG_Pipe::MGPipeShaderCsoRecord& record);

    // The draw's plan, from the program's record and the bound vertex-elements record. All zero for
    // a program without integer inputs, which costs two loads.
    IntegerAttributePlan ResolveIntegerAttributePlanForDraw(MG_Pipe::MGPipeHandle program);
    // After the program sync: true when this draw's build refused the seeds the plan counted on,
    // in which case the plan has dropped them and the vertex array must be synced again.
    Bool DropRefusedNarrowIntegerSeeds(IntegerAttributePlan& plan, MG_Pipe::MGPipeHandle program);
    // After Use(): the two widths on the program the draw runs, when they differ from what it holds.
    void PushNarrowIntegerWidthsForDraw(const IntegerAttributePlan& plan, MG_Pipe::MGPipeHandle program);

    // The build's three steps: reset at its start, note each stage's final ESSL, verify after the link.
    void BeginNarrowIntegerSeedBuild(NarrowIntegerSeedState& state);
    void NoteNarrowIntegerSeedSource(NarrowIntegerSeedState& state, const String& source, GLenum shaderType);
    void FinishNarrowIntegerSeedBuild(NarrowIntegerSeedState& state, GLuint program, Bool programUsable);

    // The armed vertex stage's SPIR-V step (after the array-input split): `spirv` itself unless
    // the pass rewrote it into `storage`.
    const Vector<unsigned int>* SeedNarrowIntegerVertexInputsForEssl(Bool armed, const Vector<unsigned int>& spirv,
                                                                     Vector<unsigned int>& storage,
                                                                     Bool enableSpirvValidation);

    // The two width globals of a seeded vertex stage, as `uniform highp uint`.
    String PromoteNarrowIntegerWidthGlobals(String source);

    namespace VertexArrayImpl {
        const MG_Pipe::MGPipeVertexElementsRecord* BoundVertexElementsRecord(const MG_Pipe::MGPipeApplierState& st);
    } // namespace VertexArrayImpl
} // namespace MobileGL::MG_Backend::DirectGLES
