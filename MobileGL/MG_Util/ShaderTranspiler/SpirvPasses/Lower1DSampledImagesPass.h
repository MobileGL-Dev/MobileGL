// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/Lower1DSampledImagesPass.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "spirv-tools/optimizer.hpp"
#include "source/opt/pass.h"

#include <Includes.h>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // The SAMPLED-image half of the 1D story. Lower1DArrayImagesPass owns the storage
            // half and says there, correctly for what it needed, that SPIRV-Cross's SAMPLER path
            // "already handles the 1D-array shape correctly and must be left to it". That is true
            // of the COORDINATE and false of everything else the lookup carries.
            //
            // ES has no 1D texture, so SPIRV-Cross emits a 1D sampler as a 2D one - `case Dim1D:
            // res += options.es ? "2D" : "1D"` - and fakes the missing coordinate component at
            // each call site (spirv_glsl.cpp, the `imgtype.image.dim == Dim1D && options.es`
            // branches: `vec2(coord, 0.0)` non-arrayed, `vec3(coord.x, 0.0, coord.y)` arrayed,
            // which is the same (u, 0, layer) the 2D-array texture actually stores). But the
            // OFFSET operand and the two GRADIENT operands are printed straight through with
            // their original 1D arity:
            //
            //     if (args.offset)   { ...; farg_str += bitcast_expression(SPIRType::Int, args.offset); }
            //     if (args.grad_x || args.grad_y) { ...; farg_str += to_expression(args.grad_x); ... }
            //
            // So a `textureLodOffset(sampler1DArray, vec2, float, int)` comes out as
            // `textureLodOffset(sampler2DArray, vec3, float, int)`, for which ESSL has no
            // overload, and the driver answers "'textureLodOffset' : no matching overloaded
            // function found". That loses the stage, and with it the program - which is how ONE
            // sampler1DArray lookup took down the nine-sampler compute shader of
            // KHR-GL43.compute_shader.resource-texture, whose dispatch then silently did nothing
            // and left the SSBO reading back the zeros the test uploaded.
            //
            // Observed failing on an Adreno 830 by isolating each form: textureOffset,
            // textureLodOffset and texelFetchOffset on both sampler1D and sampler1DArray, and
            // textureGrad on sampler1DArray. The same shaders with a 2D sampler compile, so the
            // functions exist - only the argument arity is wrong.
            //
            // WHY NOT PATCH SPIRV-CROSS. 3rdparty/SPIRV-Cross is a submodule pinned to KhronosGroup
            // upstream, not to a MobileGL fork (contrast 3rdparty/glslang), so an in-tree edit
            // would live outside this repository's history.
            //
            // WHY NOT WIDEN JUST THE OPERANDS. Emitting an ivec2 offset against a type still
            // declared Dim1D is an INVALID module, not a clever shortcut: the validator computes
            // the required arity from the image's own Dim (validate_image.cpp, GetPlaneCoordSize
            // -> "Expected Image Operand Offset to have 1 component") and would latch a failure on
            // every validating lane. So the type has to move too, and once it does the coordinate
            // has to move with it - which is what this pass does, in the module, before
            // SPIRV-Cross ever applies its own emulation.
            //
            // The rewrite is exactly SPIRV-Cross's own, restated on the SPIR-V side so that
            // coordinate, offset and gradient are all widened by one piece of code: a zero is
            // INSERTED AT COMPONENT 1 of each. That single rule is right for every shape, because
            // a 1D coordinate lays out as [u][array layer][proj q] and the plane occupies index 0
            // alone - so (u) -> (u, 0), (u, layer) -> (u, 0, layer) and (u, q) -> (u, 0, q) all
            // fall out of it, and so do the scalar offset -> ivec2 and the scalar gradients ->
            // vec2. The Dref value is a separate SPIR-V operand rather than a coordinate
            // component, so the shadow forms need nothing extra.
            //
            // NO CROSS-STAGE HAZARD, and this is the one place this pass is on firmer ground than
            // its storage-image sibling, whose header records the opposite as a known limitation.
            // That pass can rewrite uimage1DArray to uimage2DArray in one stage and decline in
            // another, and the two then spell the SAME uniform `uimage2D` and `uimage2DArray` and
            // the ES link fails on a type mismatch. Here the two spellings COINCIDE: SPIRV-Cross
            // prints Dim1D as "2D" on ES already, so a stage this pass rewrote and a stage it left
            // alone both declare `sampler2D` / `sampler2DArray`. Partial application across a
            // program's stages is therefore invisible at the interface.
            //
            // Deliberately narrow, on three axes - the sibling's reasoning, applied to this
            // resource:
            //
            //   * SAMPLED images only (Sampled == 1). Storage images are the sibling's.
            //   * Only when the module actually carries an Offset, ConstOffset or Grad operand on
            //     a 1D sampled image, i.e. only where SPIRV-Cross's emission is ALREADY broken.
            //     A shader that only calls texture()/textureLod()/texelFetch() on a sampler1D
            //     keeps taking SPIRV-Cross's own (correct) output byte for byte, so this pass has
            //     no way to regress it. The gate is decided per arrayed-ness, matching the two
            //     distinct OpTypeImage declarations glslang emits.
            //   * ESSL only. Vulkan has VK_IMAGE_VIEW_TYPE_1D natively and the offset and gradient
            //     arities are the ones the module already spells, so DirectVulkan must see the
            //     module unchanged.
            //
            // A size query on a covered image is DECLINED rather than half-translated, for the
            // sibling's reason: textureSize(sampler1D) yields an int and textureSize(sampler2D) an
            // ivec2, so rewriting the type while leaving the query would hand the shader a value of
            // the wrong shape. Refusing leaves the module byte for byte and is no worse than today.
            //
            // Every decline is decided BEFORE anything is rewritten - the pass plans the whole
            // edit, and only then applies it - so there is no state in which it has half-converted
            // a module and then given up. Anything it does not recognise reaching one of these
            // images (a gather, an unexpected image opcode) is a decline, not a guess.
            class Lower1DSampledImagesPass final : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-lower-1d-sampled-images"; }
                Status Process() override;

                // Whether a module carries the shape this pass exists for: a 1D SAMPLED image
                // reached by a lookup with an Offset, ConstOffset or Grad operand. One parse
                // answers it, and the answer is no for very nearly every shader - the common path
                // must not build an Optimizer at all.
                static bool BinaryHasOffsetOrGrad1DSampledImage(const Vector<Uint32>& binary);

                static spvtools::Optimizer::PassToken CreateLower1DSampledImagesPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
