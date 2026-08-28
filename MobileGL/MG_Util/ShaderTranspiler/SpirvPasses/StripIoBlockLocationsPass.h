// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripIoBlockLocationsPass.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "source/opt/pass.h"
#include "spirv-tools/optimizer.hpp"

#include <Includes.h>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // Drops the Location (and Component) decoration from an inter-stage interface
            // BLOCK variable, so SPIRV-Cross emits `out FOO { ... } x;` instead of
            // `layout(location = N) out FOO { ... } x;`.
            //
            // WHY. On the Mali-G1-Ultra ES driver (r54p1), an interface block that carries an
            // explicit layout(location=) transports NOTHING across any boundary that involves
            // a tessellation or geometry stage. The stages compile, the program links with an
            // empty info log, the draw runs - and the consuming stage reads zeroes. The same
            // program with the qualifier removed from the blocks (and nothing else changed)
            // carries the payload correctly. Measured with no MobileGL in the process at all:
            // a bare EGL/GLES 3.2 program built from the five ESSL stages MobileGL emits for
            // KHR-GLxx.shading_language_420pack.length_of_vector_and_matrix_* reproduces it,
            // and a three-stage VS->GS->FS reduction isolates it to
            //   (block carries a location) AND (a tessellation or geometry stage is present).
            // A located block between a vertex and a fragment stage is fine on the same
            // driver, which is why the caller only arms this for programs that have one of
            // those stages.
            //
            // The locations are not the application's: these blocks carry no location in the
            // GLSL source at all (the 420pack cases declare none). glslang's cross-stage IO
            // resolver invents them, SPIRV-Cross prints them because ESSL >= 310 allows a
            // location on a block, and nothing downstream needs them - ES matches inter-stage
            // blocks by block name plus member sequence, which is exactly what
            // UniquifyIoBlockNamesPass keeps consistent across the program.
            //
            // WHAT. Only variables in Input/Output storage whose (array-unwrapped) pointee is
            // a Block-decorated struct. Plain varyings keep their locations - they work on
            // this driver and are how the fragment stage's inputs and outputs are matched -
            // and so do vertex attributes and fragment outputs, which are never blocks.
            // Builtin blocks (gl_PerVertex) are skipped; they carry no Location anyway.
            //
            // BOTH DECORATION LEVELS, because a block carries its location at exactly one of
            // them: on the VARIABLE when the cross-stage IO resolver assigned it (or the
            // application wrote `layout(location=) out Blk {...}`), and on the MEMBERS when the
            // application located those instead - in which case glslang puts nothing on the
            // variable at all and SPIRV-Cross suppresses the block-level qualifier in favour of
            // the member ones. A variable-only strip would silently pass that second shape by.
            // A struct reached by an interface variable whose direction is NOT armed keeps its
            // member decorations: they belong to the type, and taking them off would strip the
            // unarmed side too.
            //
            // The two directions are armed SEPARATELY by the caller, because an interface
            // whose other end lives in a DIFFERENT program (a separable program pipeline)
            // must keep its location: that is the only thing matching it there, and the other
            // program never saw this decision. In a monolithic program both ends are present
            // and both flags are set.
            //
            // DirectGLES only: DirectVulkan hands the module to the driver as SPIR-V, where
            // Location is how interfaces are matched and removing it would be a miscompile.
            class StripIoBlockLocationsPass final : public spvtools::opt::Pass {
            public:
                // `stripInputBlocks` covers the blocks this stage CONSUMES and
                // `stripOutputBlocks` the ones it PRODUCES. `strippedAny`, when non-null,
                // receives whether this stage actually had one, so the caller can decline the
                // re-serialised module when there was nothing to strip.
                StripIoBlockLocationsPass(Bool stripInputBlocks, Bool stripOutputBlocks,
                                          Bool* strippedAny = nullptr)
                    : m_stripInputBlocks(stripInputBlocks), m_stripOutputBlocks(stripOutputBlocks),
                      m_strippedAny(strippedAny) {}

                const char* name() const override { return "mobilegl-strip-io-block-locations"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateStripIoBlockLocationsPass(
                    Bool stripInputBlocks, Bool stripOutputBlocks, Bool* strippedAny);

            private:
                Bool m_stripInputBlocks = false;
                Bool m_stripOutputBlocks = false;
                Bool* m_strippedAny = nullptr;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
