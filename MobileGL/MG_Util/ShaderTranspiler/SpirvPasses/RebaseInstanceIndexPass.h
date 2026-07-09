// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RebaseInstanceIndexPass.h
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
            // glslang's relaxed-Vulkan mode aliases GL's gl_InstanceID to Vulkan's
            // gl_InstanceIndex. That is wrong for OpenGL semantics: GL's gl_InstanceID is
            // zero-based (excludes baseInstance) while Vulkan's InstanceIndex includes the
            // draw's firstInstance. On the DirectVulkan backend any draw with baseInstance != 0
            // (Flywheel's glMultiDrawElementsIndirect, glDrawElementsInstancedBaseInstance)
            // therefore feeds shaders an InstanceID offset by baseInstance. This pass rebases
            // every load of the InstanceIndex builtin to (InstanceIndex - BaseInstance), the
            // same lowering Zink/DXVK use, so the value seen by the shader matches GL's
            // zero-based gl_InstanceID. Vulkan backend only - the DirectGLES transpile path
            // already receives a zero-based gl_InstanceID from SPIRV-Cross.
            class RebaseInstanceIndexPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "rebase-instance-index"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateRebaseInstanceIndexPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
