// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DemotePointSizePass.h
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
            // Demotes gl_PointSize traffic in ONE tessellation or geometry module (or mirrors it
            // out of a vertex module) into an ordinary inter-stage float varying, for devices
            // that cannot host the built-in in those stages at all: no
            // EXT/OES_tessellation_point_size / geometry_point_size on the ES driver, and
            // shaderTessellationAndGeometryPointSize == VK_FALSE on the Vulkan one. Desktop GL
            // treats the built-in as an ordinary per-vertex output, so the programs this rescues
            // are legal GL - only the targets cannot spell them.
            //
            // What "demoted" means, precisely. In a tessellation/geometry stage every access
            // chain that reaches the PointSize member of a gl_PerVertex block (gl_in[i]
            // .gl_PointSize, gl_out[i].gl_PointSize, the non-arrayed output block's member) is
            // redirected onto a plain float varying at the caller-chosen location - an arrayed
            // Input for gl_in reads, an arrayed Output for TCS gl_out writes, a scalar Output
            // for the TES/GS output - and the TessellationPointSize / GeometryPointSize
            // capability is stripped. The gl_PerVertex STRUCT keeps its PointSize member,
            // declared and decorated but no longer accessed: that is exactly the shape glslang
            // produces for a program that never touches point size (it defers the capability to
            // first use). A standalone PointSize VARIABLE (never glslang's shape, but legal
            // SPIR-V) is demoted in place: BuiltIn swapped for the Location, and the variable
            // renamed to the carrier's name.
            //
            // "Declared but unaccessed" is only safe while the ES hop PRINTS by access, and
            // there is one shape where it does not. SPIRV-Cross redeclares the whole
            // gl_PerVertex output block for a CONTROL stage whose clip or cull distances are
            // live (should_force_emit_builtin_block), and that redeclaration walks the
            // struct's member DECORATIONS - so it would print "float gl_PointSize;" into a
            // block nothing touches any more, which an extension-less ES driver rejects
            // exactly as it rejected the access. That combination therefore DECLINES, below,
            // rather than shipping a module that is mutated and still lost. Every other
            // demoted shape leaves the member genuinely invisible to the ES hop, which is
            // what the pinned transpile assertions hold.
            //
            // A VERTEX module is never capability-limited (gl_PointSize is core there on both
            // targets), so it keeps its built-in untouched and, when the next stage consumes the
            // carrier, MIRRORS the built-in's value into the carrier at every return of the
            // entry function - the VS->TCS half of the chain.
            //
            // The VALUE is what survives: gl_in[].gl_PointSize reads and transform-feedback
            // captures see exactly what the upstream stage wrote. The RASTERIZED point size is
            // what does not - with the built-in unhosted, both targets rasterize such pipelines
            // at the default size 1.0 (Vulkan: the shaderTessellationAndGeometryPointSize
            // feature description; ES: PointSizeRange default) - so rasterization-verified
            // point_rendering tests keep failing honestly and nothing may be gated on them.
            //
            // Anything the pass cannot express - a whole gl_PerVertex struct load/store/copy, a
            // pointer that escapes into an opcode it cannot follow, an access-chain split across
            // two chains - DECLINES the module byte-identically, reported through the report
            // struct, so the caller keeps the existing honest refusal paths instead of shipping
            // a half-demoted program.
            //
            // One module per run; the PROGRAM-wide contract (every stage demoted or none, one
            // shared location, matching carrier names across each boundary) is owned by
            // ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram, the only caller.
            struct DemotePointSizeOptions {
                // The Location every carrier of this program uses; chosen by the caller past
                // every location any stage of the program already consumes.
                Uint32 location = 0;
                // Name for the arrayed Input carrier (empty forbids creating one: a module that
                // reads gl_in[].gl_PointSize with no name to give the carrier declines).
                String inputCarrierName;
                // Name for the Output carrier (scalar in VS/TES/GS, arrayed in TCS).
                String outputCarrierName;
                // Create the Output carrier even when this module never writes PointSize: the
                // next stage reads it (Vulkan requires every consumed input to be produced,
                // VUID-RuntimeSpirv-OpEntryPoint-08743), or a transform-feedback capture of
                // gl_PointSize binds to it. Such a carrier is SEEDED with 1.0 - GL's default
                // point size, and what an unhosted built-in rasterizes at - rather than left
                // unwritten: GL calls the value of an unwritten output undefined, but an ES
                // driver's front end DELETES a never-written output, and a capture naming a
                // deleted varying fails the link and takes the whole capture set with it. A
                // control stage cannot be seeded this way (the write is per-invocation) and
                // declines instead.
                Bool forceOutputCarrier = false;
            };

            struct DemotePointSizeReport {
                Bool declined = false;
                String declineReason;
                // The module reads incoming PointSize, so an Input carrier now exists - which
                // obliges the PREVIOUS stage to produce the matching Output carrier. The driver
                // walks the stages back-to-front off exactly this bit.
                Bool createdInputCarrier = false;
            };

            class DemotePointSizePass : public spvtools::opt::Pass {
            public:
                DemotePointSizePass(DemotePointSizeOptions options, DemotePointSizeReport* report)
                    : m_options(Move(options)), m_report(report) {}
                const char* name() const override { return "mobilegl-demote-point-size"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateDemotePointSizePass(
                    DemotePointSizeOptions options, DemotePointSizeReport* report);

            private:
                DemotePointSizeOptions m_options;
                DemotePointSizeReport* m_report;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
