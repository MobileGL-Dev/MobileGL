// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RenameBuiltinShadowingFunctionsPass.h
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
            // Desktop GLSL lets a shader redefine a builtin function (round, fma, ...) and
            // shadow it; ESSL 3.x forbids the redefinition, so when SPIRV-Cross re-emits the
            // function under its original OpName a strict ES driver rejects the shader with a
            // redefinition error. Prefix the OpName of every user-defined function whose base
            // name collides with an ESSL builtin (plus the min3/max3 trinary extension names)
            // with "mg_". Renaming a user function is always semantics-preserving: its
            // definition and every call site go through the same result id, while calls to
            // the real builtin never resolve to a user function id in SPIR-V.
            //
            // This is the BACKSTOP half of the rename. The primary half is the lexical
            // RenameBuiltinShadowingFunctions in ShaderSourceProcessor, which has to run
            // before the parse - glslang's relaxed parse rejects some shadowing overload
            // shapes outright, and a shadowed builtin may itself need an extension the
            // declared #version does not enable. This pass catches what a lexical scan
            // cannot see (macro-expanded definitions) and is idempotent: an already
            // renamed mg_* name is not in the builtin table.
            //
            // Both halves share MG_Util/ShaderTranspiler/EsslBuiltinFunctionNames.h, so the
            // covered name set cannot drift between them.
            class RenameBuiltinShadowingFunctionsPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "rename-builtin-shadowing-functions"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateRenameBuiltinShadowingFunctionsPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
