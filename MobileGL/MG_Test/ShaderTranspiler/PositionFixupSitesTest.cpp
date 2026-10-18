// MobileGL - MobileGL/MG_Test/ShaderTranspiler/PositionFixupSitesTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Magma's clip-space fix-up rewrites gl_Position in front of every vertex the last
// pre-rasterization stage produces. A geometry shader produces one at every emit it can execute,
// and the front end leaves helper functions in place, so an EmitVertex() inside a helper needs the
// fix-up too: without it those vertices reach the rasterizer un-remapped, and a GL depth below
// zero is clipped away. A vertex shader's helper return, on the other hand, does not end the
// invocation and must stay untouched.

#include <gtest/gtest.h>

#include <memory>

#include "Includes.h"
#include "Init.h"
#include <MG_Backend/DirectVulkan/Renderer/ProgramFactory.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <spirv-tools/libspirv.hpp>

#include "source/opt/build_module.h"
#include "source/opt/ir_context.h"
#include "spirv.hpp"

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;
using spvtools::opt::Instruction;
using spvtools::opt::IRContext;

namespace {
    using Bit = MG_Backend::DirectVulkan::ProgramFactory::CompileOptionBit;
    using CompileOptionFlags = MG_Backend::DirectVulkan::ProgramFactory::CompileOptionFlags;

    // One stage, linked and sanitized the way production hands it to the backend.
    Vector<Uint32> CompileStage(GLenum stage, const String& source) {
        using namespace MG_Util::ShaderTranspiler;
        MobileGL::Initialize();
        ShaderAttrib shaderAttrib{.shaderType = stage, .sourceStr = source};
        auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
        EXPECT_TRUE(shaderResult) << (shaderResult ? String{} : shaderResult.error().log);
        if (!shaderResult) return {};
        ProgramAttrib programAttrib{.shaders = {shaderResult.value()}};
        auto programResult = ShaderCompiler::LinkProgram(programAttrib);
        EXPECT_TRUE(programResult) << (programResult ? String{} : programResult.error().log);
        if (!programResult) return {};
        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {stage}, .program = *programResult.value()};
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        EXPECT_TRUE(binaryResult) << (binaryResult ? String{} : binaryResult.error().log);
        if (!binaryResult || binaryResult->empty()) return {};
        Vector<Uint32> module = binaryResult->front();
        EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(module, module, true, true));
        return module;
    }

    Vector<Uint32> FixUp(const Vector<Uint32>& spirv) {
        Vector<Uint32> out;
        EXPECT_TRUE(MG_Backend::DirectVulkan::TransformSpirvForVulkanPositionFix(
            spirv, out, CompileOptionFlags(Bit::PositionYFlip) | Bit::PositionZRemap));
        return out;
    }

    String ValidationError(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String error;
        tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
            error += message != nullptr ? message : "";
        });
        return tools.Validate(spirv) ? String{} : (error.empty() ? String{"invalid"} : error);
    }

    String Disassemble(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String text;
        EXPECT_TRUE(tools.Disassemble(spirv, &text));
        return text;
    }

    // Whether `pointerId` is an access chain to gl_PerVertex's Position member.
    Bool IsPositionPointer(IRContext* context, Uint32 pointerId) {
        auto* defUse = context->get_def_use_mgr();
        const Instruction* chain = defUse->GetDef(pointerId);
        if (chain == nullptr || chain->opcode() != spv::Op::OpAccessChain || chain->NumInOperands() != 2) return false;
        const Instruction* base = defUse->GetDef(chain->GetSingleWordInOperand(0));
        const Instruction* index = defUse->GetDef(chain->GetSingleWordInOperand(1));
        if (base == nullptr || base->opcode() != spv::Op::OpVariable || index == nullptr ||
            index->opcode() != spv::Op::OpConstant)
            return false;
        const Uint32 structId = defUse->GetDef(base->type_id())->GetSingleWordInOperand(1);
        const Uint32 member = index->GetSingleWordInOperand(0);
        for (const Instruction& annotation : context->annotations()) {
            if (annotation.opcode() == spv::Op::OpMemberDecorate && annotation.GetSingleWordInOperand(0) == structId &&
                annotation.GetSingleWordInOperand(1) == member &&
                annotation.GetSingleWordInOperand(2) == Uint32(spv::Decoration::BuiltIn) &&
                annotation.GetSingleWordInOperand(3) == Uint32(spv::BuiltIn::Position))
                return true;
        }
        return false;
    }

    // Whether `inst` is the fix-up's closing store: Position stored back with its y negated, the
    // y taken from a load of Position itself. The shader's own stores never have that shape.
    Bool IsFixupStore(IRContext* context, const Instruction& inst) {
        if (inst.opcode() != spv::Op::OpStore || !IsPositionPointer(context, inst.GetSingleWordInOperand(0)))
            return false;
        auto* defUse = context->get_def_use_mgr();
        const Instruction* value = defUse->GetDef(inst.GetSingleWordInOperand(1));
        if (value == nullptr || value->opcode() != spv::Op::OpCompositeConstruct || value->NumInOperands() != 4)
            return false;
        const Instruction* negatedY = defUse->GetDef(value->GetSingleWordInOperand(1));
        if (negatedY == nullptr || negatedY->opcode() != spv::Op::OpFNegate) return false;
        const Instruction* y = defUse->GetDef(negatedY->GetSingleWordInOperand(0));
        if (y == nullptr || y->opcode() != spv::Op::OpCompositeExtract) return false;
        const Instruction* loaded = defUse->GetDef(y->GetSingleWordInOperand(0));
        return loaded != nullptr && loaded->opcode() == spv::Op::OpLoad &&
               IsPositionPointer(context, loaded->GetSingleWordInOperand(0));
    }

    struct Sites {
        SizeT sites = 0;
        SizeT sitesInHelpers = 0;
        // Sites whose immediately preceding instruction is the fix-up's store.
        SizeT fixedSites = 0;
        SizeT fixupStores = 0;
        SizeT fixupStoresInHelpers = 0;
    };

    // Emits for a geometry shader, returns for any other stage.
    Sites Inspect(const Vector<Uint32>& spirv) {
        Sites sites;
        std::unique_ptr<IRContext> context =
            spvtools::BuildModule(SPV_ENV_VULKAN_1_1, nullptr, spirv.data(), spirv.size());
        EXPECT_NE(context, nullptr);
        if (context == nullptr) return sites;
        const Instruction& entryPoint = *context->module()->entry_points().begin();
        const Bool geometry =
            static_cast<spv::ExecutionModel>(entryPoint.GetSingleWordInOperand(0)) == spv::ExecutionModel::Geometry;
        const Uint32 entryFunctionId = entryPoint.GetSingleWordInOperand(1);
        for (auto& function : *context->module()) {
            const Bool helper = function.result_id() != entryFunctionId;
            for (auto& block : function) {
                const Instruction* previous = nullptr;
                for (const Instruction& inst : block) {
                    const Bool isSite = geometry ? (inst.opcode() == spv::Op::OpEmitVertex ||
                                                    inst.opcode() == spv::Op::OpEmitStreamVertex)
                                                 : inst.opcode() == spv::Op::OpReturn;
                    if (isSite) {
                        ++sites.sites;
                        if (helper) ++sites.sitesInHelpers;
                        if (previous != nullptr && IsFixupStore(context.get(), *previous)) ++sites.fixedSites;
                    }
                    if (IsFixupStore(context.get(), inst)) {
                        ++sites.fixupStores;
                        if (helper) ++sites.fixupStoresInHelpers;
                    }
                    previous = &inst;
                }
            }
        }
        return sites;
    }
} // namespace

// gsprobe's helper case: each corner is written and emitted inside the helper.
TEST(PositionFixupSitesTest, AnEmitInsideAHelperGetsTheFixup) {
    const auto spirv = CompileStage(GL_GEOMETRY_SHADER, R"(#version 410 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
void corner(int i) { gl_Position = vec4(gl_in[i].gl_Position.xy, -0.5, 1.0); EmitVertex(); }
void main() { corner(0); corner(1); corner(2); EndPrimitive(); }
)");
    ASSERT_FALSE(spirv.empty());
    const Sites before = Inspect(spirv);
    ASSERT_EQ(before.sites, 1u) << "the helper is expected to survive to the backend";
    ASSERT_EQ(before.sitesInHelpers, 1u);
    ASSERT_EQ(before.fixupStores, 0u);

    const auto fixed = FixUp(spirv);
    EXPECT_EQ(ValidationError(fixed), String{}) << Disassemble(fixed);
    const Sites after = Inspect(fixed);
    EXPECT_EQ(after.sites, 1u);
    EXPECT_EQ(after.fixedSites, 1u) << Disassemble(fixed);
    EXPECT_EQ(after.fixupStores, 1u);
}

// The value is written in main and only the emit sits in a helper.
TEST(PositionFixupSitesTest, AHelperThatOnlyEmitsGetsTheFixup) {
    const auto spirv = CompileStage(GL_GEOMETRY_SHADER, R"(#version 410 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
void emitIt() { EmitVertex(); }
void main() {
    for (int i = 0; i < 3; i++) { gl_Position = vec4(gl_in[i].gl_Position.xy, -0.5, 1.0); emitIt(); }
    EndPrimitive();
}
)");
    ASSERT_FALSE(spirv.empty());
    ASSERT_EQ(Inspect(spirv).sitesInHelpers, 1u);

    const auto fixed = FixUp(spirv);
    EXPECT_EQ(ValidationError(fixed), String{}) << Disassemble(fixed);
    const Sites after = Inspect(fixed);
    EXPECT_EQ(after.sites, 1u);
    EXPECT_EQ(after.fixedSites, 1u) << Disassemble(fixed);
    EXPECT_EQ(after.fixupStores, 1u);
}

// Emits in main and in a helper side by side: each one is fixed exactly once.
TEST(PositionFixupSitesTest, EmitsInMainAndInAHelperAreEachFixedOnce) {
    const auto spirv = CompileStage(GL_GEOMETRY_SHADER, R"(#version 410 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
void corner(int i) { gl_Position = gl_in[i].gl_Position; EmitVertex(); }
void main() {
    gl_Position = gl_in[0].gl_Position;
    EmitVertex();
    corner(1);
    corner(2);
    EndPrimitive();
}
)");
    ASSERT_FALSE(spirv.empty());
    const Sites before = Inspect(spirv);
    ASSERT_EQ(before.sites, 2u);
    ASSERT_EQ(before.sitesInHelpers, 1u);

    const auto fixed = FixUp(spirv);
    EXPECT_EQ(ValidationError(fixed), String{}) << Disassemble(fixed);
    const Sites after = Inspect(fixed);
    EXPECT_EQ(after.fixedSites, 2u) << Disassemble(fixed);
    EXPECT_EQ(after.fixupStores, 2u);
    EXPECT_EQ(after.fixupStoresInHelpers, 1u);
}

// The shape that always worked: every emit in main, each one still fixed.
TEST(PositionFixupSitesTest, EmitsInMainKeepTheirFixup) {
    const auto spirv = CompileStage(GL_GEOMETRY_SHADER, R"(#version 410 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;
void main() {
    for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position; EmitVertex(); }
    gl_Position = gl_in[0].gl_Position;
    EmitVertex();
    EndPrimitive();
}
)");
    ASSERT_FALSE(spirv.empty());
    const Sites before = Inspect(spirv);
    ASSERT_EQ(before.sites, 2u);
    ASSERT_EQ(before.sitesInHelpers, 0u);

    const auto fixed = FixUp(spirv);
    EXPECT_EQ(ValidationError(fixed), String{}) << Disassemble(fixed);
    const Sites after = Inspect(fixed);
    EXPECT_EQ(after.fixedSites, 2u) << Disassemble(fixed);
    EXPECT_EQ(after.fixupStores, 2u);
}

// A vertex shader that writes gl_Position in a helper: only main's return ends the invocation,
// so that is the only place the fix-up goes.
TEST(PositionFixupSitesTest, AVertexShaderHelperReturnIsNotAFixupSite) {
    const auto spirv = CompileStage(GL_VERTEX_SHADER, R"(#version 410 core
layout(location = 0) in vec2 pos;
void place(vec2 p) { gl_Position = vec4(p, -0.5, 1.0); }
void main() { place(pos); }
)");
    ASSERT_FALSE(spirv.empty());
    const Sites before = Inspect(spirv);
    ASSERT_EQ(before.sitesInHelpers, 1u) << "the helper is expected to survive to the backend";

    const auto fixed = FixUp(spirv);
    EXPECT_EQ(ValidationError(fixed), String{}) << Disassemble(fixed);
    const Sites after = Inspect(fixed);
    EXPECT_EQ(after.fixupStores, 1u) << Disassemble(fixed);
    EXPECT_EQ(after.fixupStoresInHelpers, 0u);
    EXPECT_EQ(after.fixedSites, 1u);
}
