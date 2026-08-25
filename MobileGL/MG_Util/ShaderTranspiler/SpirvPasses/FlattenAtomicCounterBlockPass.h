// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenAtomicCounterBlockPass.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "spirv-tools/optimizer.hpp"
#include "source/opt/pass.h"

#include <Includes.h>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // glslang's relaxed parse lowers every atomic_uint onto a synthesized storage block
            // named gl_AtomicCounterBlock_<GL binding>, and it PRESERVES the application's
            // `layout(offset = N)` as the member's SPIR-V Offset decoration. A block whose first
            // member sits at offset 8 is not expressible in std140 or std430 - both put member 0
            // at offset 0 - and GLSL ES has no member layout(offset=), so SPIRV-Cross refuses the
            // whole stage rather than emit something wrong:
            //
            //     Push constant block cannot be expressed as neither std430 nor std140.
            //     ES-targets do not support GL_ARB_enhanced_layouts.
            //
            // (The message says "push constant"; the variable is StorageClass Uniform. Do not
            // chase push constants.) The stage never reaches the driver, the program links short
            // of it with an EMPTY driver info log, and the dispatch no-ops while the frontend
            // still reports the link status glslang published - KHR-GL43.compute_shader.resources
            // -atomic-counter's non-zero-offset sibling, and the shape every conformance case
            // that declares `layout(binding = B, offset = N)` takes.
            //
            // The repair is to make the offsets DISAPPEAR rather than to move the buffer. Each
            // atomic-counter block is collapsed into ONE `uint` array covering the same byte
            // window, member 0 at offset 0 with ArrayStride 4 - a layout std430 expresses
            // exactly - and every access is re-indexed to the element that used to be at its
            // byte offset. `counters[k]` declared at offset 8 becomes element (2 + k) of the
            // array, i.e. byte 8 + 4k, which is the byte the application's counter buffer really
            // holds.
            //
            // Why not simply rebase the offsets to zero and bind the buffer 8 bytes in: because
            // glBindBufferRange's offset must be a multiple of
            // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, which is 64 on Adreno 830 and no smaller
            // than 32 on the other targets. A byte offset of 8 cannot be expressed as a binding
            // on any of them, so the correction has to live in the shader's indexing, where it
            // costs nothing.
            //
            // A block that is ALREADY laid out naturally - which is every shader that omits the
            // offset qualifier, and so very nearly all of them - is left byte-identical: the
            // detection below is the gate, and KHR-GL43.compute_shader.resource-atomic-counter
            // (offset 0) is the latch that the no-op case stays a no-op.
            //
            // Declines the whole block, leaving it untouched, on any shape it cannot re-index
            // exactly: a member that is not `uint` or an array of `uint` with stride 4, an offset
            // that is not a multiple of 4, a byte window past what GL_MAX_ATOMIC_COUNTER_BUFFER
            // _SIZE allows, a member with no Offset decoration at all, or an access chain that
            // stops at the member (a pointer handed to a function) rather than reaching a
            // counter.
            //
            // DirectGLES transpile path only. DirectVulkan takes the block's declared offsets
            // natively through an explicitly-laid-out descriptor and must see them unchanged.
            class FlattenAtomicCounterBlockPass final : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-flatten-atomic-counter-block"; }
                Status Process() override;

                // The detection half, on a serialized module: true when the module declares an
                // atomic-counter block whose member offsets are not already the natural std430
                // packing, i.e. whether this pass could change anything. One BuildModule, no
                // serialization, so the ~every shader that declares no counter (or declares one
                // at offset 0) pays no optimizer round trip.
                static bool BinaryHasOffsetAtomicCounterBlock(const Vector<Uint32>& binary);

                static spvtools::Optimizer::PassToken CreateFlattenAtomicCounterBlockPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
