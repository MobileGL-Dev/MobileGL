// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenXfbInterfaceBlocksPass.h
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

#include <set>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // Replaces a named interface BLOCK whose members transform feedback captures with
            // one free-standing variable per member, named "<BlockName>_<MemberName>", and
            // demotes the block itself to a Private shadow that the entry point copies into
            // (Input) or out of (Output). The block's own body code is untouched: every
            // OpAccessChain into it - dynamically indexed ones included - keeps working
            // against the shadow.
            //
            // WHY. The Adreno ES driver accepts "BlockName.member" in
            // glTransformFeedbackVaryings, links, and reports the names straight back from
            // glGetTransformFeedbackVarying - and then captures NONE of them: the recorded
            // stream holds gl_Position in slot 0 and leaves the rest of every vertex's record
            // untouched. Proven on an Adreno 830 by writing a recognisable gl_Position into
            // the KHR-GL43.vertex_attrib_binding.basic-input capture program: the value landed
            // in the slot that had asked for StageData.attrib[0]. The same program with the
            // block flattened into a plain output array captures every member correctly, which
            // is what this pass produces. Requesting the members under the ESSL instance-name
            // spelling ("vs_out.attrib[0]") instead makes the driver fail the link outright, so
            // the capture list cannot be spelled around the defect - only the declaration can.
            //
            // The rename is deterministic and derived only from names that GLSL interface
            // matching already requires both sides of a stage boundary to agree on (block name
            // and member names), so running this pass over EVERY stage of a program with the
            // same block set keeps a producer and its consumer matched.
            //
            // DirectGLES only: Vulkan captures by xfb_offset, never by name.
            class FlattenXfbInterfaceBlocksPass : public spvtools::opt::Pass {
            public:
                // `blockNames` are block TYPE names, i.e. the "StageData" of
                // "StageData.attrib[0]". `flattenedBlockNames` receives the subset this pass
                // actually rewrote, so the caller can rename exactly those capture requests
                // and leave the rest spelled as the application wrote them.
                FlattenXfbInterfaceBlocksPass(const std::set<String>& blockNames,
                                              std::set<String>* flattenedBlockNames)
                    : m_blockNames(blockNames), m_flattenedBlockNames(flattenedBlockNames) {}

                const char* name() const override { return "mobilegl-flatten-xfb-interface-blocks"; }
                Status Process() override;

                // The member of `blockName` spelled by a capture request, as this pass names
                // it: "StageData.attrib[0]" -> "StageData_attrib[0]". Returns false when the
                // name does not address a member of a flattened block.
                static Bool RewriteCaptureName(const String& captureName, const std::set<String>& flattenedBlockNames,
                                               String& outName);

                static spvtools::Optimizer::PassToken CreateFlattenXfbInterfaceBlocksPass(
                    const std::set<String>& blockNames, std::set<String>* flattenedBlockNames);

            private:
                std::set<String> m_blockNames;
                std::set<String>* m_flattenedBlockNames = nullptr;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
