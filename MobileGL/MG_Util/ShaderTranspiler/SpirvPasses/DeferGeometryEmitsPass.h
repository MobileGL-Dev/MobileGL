// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DeferGeometryEmitsPass.h
// Copyright (c) 2026 MobileGL-Dev
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
            // A geometry shader's memory writes, issued after its EmitVertex() calls, moved IN
            // FRONT of every emit - for an ES driver that silently drops shader storage writes a
            // geometry invocation makes after emitting (SelfTest::GeometryStageSsboWriteAfterEmitDropped,
            // measured on Adreno; KHR-GL4x.shader_storage_buffer_object.basic-atomic-case1/3 is the
            // CTS shape: EmitVertex() first, then a chain of atomics on a geometry-stage buffer).
            //
            // The emits are DEFERRED rather than the writes hoisted, because only the emits can be
            // moved without changing what the shader computes: an emit reads nothing but the
            // output variables' values at that moment, and those are snapshotted.
            //   * The original entry function becomes an ordinary function (mg_geometryBody), and a
            //     new entry point calls it and then replays the vertices - so a `return` anywhere
            //     in the original body, however deep, still ends up at the replay.
            //   * Each OpEmitVertex becomes a copy of every output the shader writes into a Private
            //     array sized by the OutputVertices execution mode (max_vertices), plus a bump of
            //     the vertex count; each OpEndPrimitive marks the last recorded vertex as the end
            //     of a strip. Both are straight-line code - no new control flow inside the body.
            //   * The replay loop restores each vertex's outputs, emits it, and ends the strip
            //     where the body did.
            // Only outputs the shader STORES to are carried (a block member it never writes, such
            // as gl_ClipDistance, would otherwise become an active builtin the ESSL stage cannot
            // declare). Emits past max_vertices are undefined in GL; they overwrite the last slot.
            //
            // Declines (leaves the module untouched) when: the module is not a geometry stage, it
            // has no emit or no memory write at all, it uses vertex streams, a block output is
            // indexed dynamically at its top level, or the snapshot would exceed a per-invocation
            // budget. Only for the DirectGLES transpile path on a driver the POST measured.
            class DeferGeometryEmitsPass : public spvtools::opt::Pass {
            public:
                explicit DeferGeometryEmitsPass(Bool* outDeferred = nullptr) : m_outDeferred(outDeferred) {}
                const char* name() const override { return "defer-geometry-emits"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateDeferGeometryEmitsPass(Bool* outDeferred = nullptr);

            private:
                Bool* m_outDeferred = nullptr;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
