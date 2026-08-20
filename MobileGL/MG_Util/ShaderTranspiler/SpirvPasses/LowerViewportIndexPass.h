// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LowerViewportIndexPass.h
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
            // ESSL has no core gl_ViewportIndex at any version - only GL_OES_viewport_array
            // introduces it - and SPIRV-Cross prints the identifier bare, requesting no extension
            // for it (contrast BuiltInLayer, which it backs with GL_NV_viewport_array2 on ES). On
            // a driver WITHOUT that extension the stage therefore fails to compile, DirectGLES
            // marks the program unusable and binds program 0 for it, and every draw silently
            // renders nothing while GL_LINK_STATUS still answers TRUE - the failure signature
            // KHR-GL4x.viewport_array reports as "expected N, got -1", i.e. the untouched upload.
            //
            // This pass demotes the ViewportIndex OUTPUT to a plain Private global named
            // mg_ViewportIndex, so the decompiled ESSL declares an ordinary global the shader
            // still writes and nothing reads. The program compiles and rendering degrades to
            // viewport 0 - which is the single-viewport behaviour MG_IntegrationTest's
            // ViewportArrayScenario already documents for this backend - instead of the whole
            // program becoming a no-op. Only meant for the DirectGLES transpile path; the Vulkan
            // backend keeps the native builtin and routes it for real.
            //
            // gl_Layer is deliberately NOT touched: BuiltIn Layer IS core in ESSL 3.20 geometry
            // shaders, and demoting it would break layered rendering that works today.
            class LowerViewportIndexPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "lower-viewport-index"; }
                Status Process() override;

                // Whether the module declares an output decorated BuiltIn ViewportIndex, i.e.
                // whether running this pass could change anything. Answered from a single parse so
                // the caller can skip the optimizer round trip entirely - which is every shader
                // but the handful that route viewports from the shader.
                static bool DeclaresViewportIndexBuiltin(const Vector<Uint32>& binary);

                // Same question answered from an already-built module, so one parse can feed
                // several gates (ShaderCompiler::ProbeSpirvGateFeatures).
                static bool DeclaresViewportIndexBuiltin(spvtools::opt::IRContext* context);

                static spvtools::Optimizer::PassToken CreateLowerViewportIndexPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
