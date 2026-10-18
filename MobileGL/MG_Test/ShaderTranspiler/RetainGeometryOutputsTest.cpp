// MobileGL - MobileGL/MG_Test/ShaderTranspiler/RetainGeometryOutputsTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// RetainGeometryOutputsPass makes a geometry output keep its last written value across
// EmitVertex(), as desktop drivers do. What has to hold: an output written before an earlier emit
// is copied from its Private shadow into the real output right before every emit and is otherwise
// never touched directly; outputs written before every emit, and stages that write everything
// before every emit, stay as they were; unwritten builtin members stay inactive; the module stays
// valid Vulkan SPIR-V whose ESSL translation compiles; and the sanitize chain both backends consume
// runs it.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/RetainGeometryOutputsPass.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>

#include "source/opt/build_module.h"
#include "source/opt/ir_context.h"
#include "spirv.hpp"

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::RetainGeometryOutputsPass;
using MobileGL::MG_Util::ShaderTranspiler::SessionUsageBit;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;
using MobileGL::MG_Util::ShaderTranspiler::SpvcSession;
using spvtools::opt::Instruction;
using spvtools::opt::IRContext;

namespace {
    Vector<Uint32> CompileStage(GLenum stage, const String& source) {
        using namespace MG_Util::ShaderTranspiler;
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
        return binaryResult->front();
    }

    Vector<Uint32> CompileGeometry(const String& source) {
        return CompileStage(GL_GEOMETRY_SHADER, source);
    }

    Vector<Uint32> Retain(const Vector<Uint32>& spirv) {
        spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);
        optimizer.RegisterPass(RetainGeometryOutputsPass::CreateRetainGeometryOutputsPass());
        spvtools::OptimizerOptions options;
        options.set_run_validator(false);
        Vector<Uint32> out;
        EXPECT_TRUE(optimizer.Run(spirv.data(), spirv.size(), &out, options));
        return out;
    }

    Bool Validates(const Vector<Uint32>& spirv, String& messages) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        tools.SetMessageConsumer([&messages](spv_message_level_t, const char*, const spv_position_t&,
                                             const char* message) { messages += String(message) + "\n"; });
        return tools.Validate(spirv);
    }

    String Transpile(const Vector<Uint32>& spirv) {
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        auto essl = ShaderCompiler::DecompileShader(session);
        EXPECT_TRUE(essl) << (essl ? String{} : essl.error().log);
        return essl ? essl.value() : String{};
    }

    // Whether the ESSL SPIRV-Cross printed is accepted by an ES front end.
    Bool EsslCompiles(const String& essl, String& log) {
        glslang::TShader shader(EShLangGeometry);
        const char* strings[] = {essl.c_str()};
        shader.setStrings(strings, 1);
        const Bool ok = shader.parse(GetDefaultResources(), 320, EEsProfile, false, false, EShMsgDefault);
        log = shader.getInfoLog();
        return ok;
    }

    std::unique_ptr<IRContext> Parse(const Vector<Uint32>& spirv) {
        return spvtools::BuildModule(SPV_ENV_VULKAN_1_1, nullptr, spirv.data(), spirv.size());
    }

    // The OpVariable whose OpName is `name`, or 0.
    Uint32 VariableNamed(IRContext* context, const String& name) {
        for (const Instruction& debugInst : context->debugs2()) {
            if (debugInst.opcode() != spv::Op::OpName || debugInst.GetInOperand(1).AsString() != name) continue;
            const Instruction* def = context->get_def_use_mgr()->GetDef(debugInst.GetSingleWordInOperand(0));
            if (def != nullptr && def->opcode() == spv::Op::OpVariable) return def->result_id();
        }
        return 0;
    }

    spv::StorageClass StorageClassOf(IRContext* context, Uint32 variableId) {
        return static_cast<spv::StorageClass>(
            context->get_def_use_mgr()->GetDef(variableId)->GetSingleWordInOperand(0));
    }

    // Whether `pointerId` addresses the whole of `variableId` (member < 0) or exactly its member.
    Bool Addresses(IRContext* context, Uint32 pointerId, Uint32 variableId, Int member) {
        if (member < 0) return pointerId == variableId;
        const Instruction* chain = context->get_def_use_mgr()->GetDef(pointerId);
        if (chain == nullptr || chain->opcode() != spv::Op::OpAccessChain || chain->NumInOperands() != 2 ||
            chain->GetSingleWordInOperand(0) != variableId)
            return false;
        const Instruction* index = context->get_def_use_mgr()->GetDef(chain->GetSingleWordInOperand(1));
        return index != nullptr && index->opcode() == spv::Op::OpConstant &&
               index->GetSingleWordInOperand(0) == Uint32(member);
    }

    struct EmitSites {
        SizeT emits = 0;
        // Emits whose own straight-line run (since the previous emit or the block start) stores the output.
        SizeT emitsPrecededByStore = 0;
        SizeT storesToOutput = 0;
        SizeT loadsFromOutput = 0;
    };

    EmitSites Inspect(IRContext* context, Uint32 variableId, Int member = -1) {
        EmitSites sites;
        for (auto& function : *context->module()) {
            for (auto& block : function) {
                Bool storedSinceEmit = false;
                for (Instruction& inst : block) {
                    if (inst.opcode() == spv::Op::OpEmitVertex || inst.opcode() == spv::Op::OpEmitStreamVertex) {
                        ++sites.emits;
                        if (storedSinceEmit) ++sites.emitsPrecededByStore;
                        storedSinceEmit = false;
                    } else if (inst.opcode() == spv::Op::OpStore &&
                               Addresses(context, inst.GetSingleWordInOperand(0), variableId, member)) {
                        ++sites.storesToOutput;
                        storedSinceEmit = true;
                    } else if (inst.opcode() == spv::Op::OpLoad &&
                               Addresses(context, inst.GetSingleWordInOperand(0), variableId, member)) {
                        ++sites.loadsFromOutput;
                    }
                }
            }
        }
        return sites;
    }

    Uint32 PerVertexBlock(IRContext* context) {
        for (const Instruction& global : context->types_values()) {
            if (global.opcode() != spv::Op::OpVariable ||
                static_cast<spv::StorageClass>(global.GetSingleWordInOperand(0)) != spv::StorageClass::Output)
                continue;
            const Instruction* pointer = context->get_def_use_mgr()->GetDef(global.type_id());
            const Instruction* pointee = context->get_def_use_mgr()->GetDef(pointer->GetSingleWordInOperand(1));
            if (pointee->opcode() == spv::Op::OpTypeStruct) return global.result_id();
        }
        return 0;
    }

    void ExpectValidAndCompilable(const Vector<Uint32>& spirv) {
        String messages;
        EXPECT_TRUE(Validates(spirv, messages)) << messages;
        const String essl = Transpile(spirv);
        ASSERT_FALSE(essl.empty());
        String log;
        EXPECT_TRUE(EsslCompiles(essl, log)) << log << "\n" << essl;
    }

    // gsprobe's case 3 and iterationT's gbuffers_terrain.gsh: a flat output written (and read
    // back) once, before the loop that emits; the provoking vertex is the third.
    const char* const kFlatBeforeLoop = R"(#version 410 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
in vec2 v_pos[];
flat out float res;
out vec2 p;
void main() {
    res = abs(v_pos[0].x) + 1.0;
    res = exp2(round(log2(res)));
    for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position; p = v_pos[i]; EmitVertex(); }
    EndPrimitive();
}
)";

    // Everything written before every emit, one output through both arms of a branch.
    const char* const kWritesEverythingPerEmit = R"(#version 410 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
in vec2 v_pos[];
flat out float res;
out vec2 p;
void main() {
    for (int i = 0; i < 3; i++) {
        if (v_pos[i].x > 0.0) res = 1.0; else res = 2.0;
        gl_Position = gl_in[i].gl_Position;
        p = v_pos[i];
        EmitVertex();
    }
    EndPrimitive();
}
)";

    // gl_PointSize written once in a gl_PerVertex that also declares gl_ClipDistance, plus the
    // layered-rendering idiom of a gl_Layer set before the emit loop.
    const char* const kBuiltinsBeforeLoop = R"(#version 410 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
void main() {
    gl_PointSize = 4.0;
    gl_Layer = 2;
    for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position; EmitVertex(); }
}
)";

    // The emit lives in a helper; main writes the flat output once and calls it per vertex.
    const char* const kEmitInHelper = R"(#version 410 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
flat out float res;
void emitCorner(int i) { gl_Position = gl_in[i].gl_Position; EmitVertex(); }
void main() {
    res = 42.0;
    emitCorner(0);
    emitCorner(1);
    emitCorner(2);
}
)";

    // Fully written before every emit, but from its own value before the previous emit.
    const char* const kReadBackAfterEmit = R"(#version 410 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
flat out float res;
void main() {
    res = 1.0;
    for (int i = 0; i < 3; i++) { res = res * 2.0; gl_Position = gl_in[i].gl_Position; EmitVertex(); }
}
)";

    // A partially written output: .w once, .xyz per vertex.
    const char* const kPartialWrites = R"(#version 410 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
out vec4 c;
void main() {
    c.w = 0.25;
    for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position; c.xyz = vec3(float(i)); EmitVertex(); }
}
)";

    // A user output block: only the member written before the loop is carried.
    const char* const kUserBlock = R"(#version 410 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
out Data { flat float res; vec2 p; } vOut;
void main() {
    vOut.res = 42.0;
    for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position; vOut.p = vec2(float(i)); EmitVertex(); }
}
)";

    const char* const kStreams = R"(#version 410 core
layout(points) in;
layout(points, max_vertices = 4) out;
layout(stream = 0) out float a;
layout(stream = 1) out float b;
void main() {
    a = 1.0;
    b = 2.0;
    for (int i = 0; i < 2; i++) {
        gl_Position = gl_in[0].gl_Position;
        EmitStreamVertex(0);
        EmitStreamVertex(1);
    }
}
)";

    // The flat-before-loop shape plus a storage write after the emits, so DeferGeometryEmitsPass
    // has something to defer.
    const char* const kFlatBeforeLoopWithMemoryWrite = R"(#version 430 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(std430, binding = 0) buffer Counter { uint n; };
flat out float res;
void main() {
    res = 42.0;
    for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position; EmitVertex(); }
    atomicAdd(n, 1u);
}
)";

    const char* const kVertex = R"(#version 410 core
out float res;
void main() { res = 1.0; gl_Position = vec4(0.0); }
)";
} // namespace

TEST(RetainGeometryOutputs, AFlatOutputWrittenBeforeTheLoopIsCopiedBeforeEveryEmit) {
    const Vector<Uint32> spirv = CompileGeometry(kFlatBeforeLoop);
    ASSERT_FALSE(spirv.empty());
    const Vector<Uint32> out = Retain(spirv);
    ASSERT_NE(out, spirv);
    ExpectValidAndCompilable(out);

    auto context = Parse(out);
    ASSERT_NE(context, nullptr);
    const Uint32 res = VariableNamed(context.get(), "res");
    const Uint32 shadow = VariableNamed(context.get(), "mg_retained_res");
    ASSERT_NE(res, 0u);
    ASSERT_NE(shadow, 0u);
    EXPECT_EQ(StorageClassOf(context.get(), res), spv::StorageClass::Output);
    EXPECT_EQ(StorageClassOf(context.get(), shadow), spv::StorageClass::Private);

    // The real output is written only by the copy in front of the one emit, and never read.
    const EmitSites atOutput = Inspect(context.get(), res);
    EXPECT_EQ(atOutput.emits, 1u);
    EXPECT_EQ(atOutput.emitsPrecededByStore, 1u);
    EXPECT_EQ(atOutput.storesToOutput, 1u);
    EXPECT_EQ(atOutput.loadsFromOutput, 0u);
    // The shader's own two writes and its read-back moved to the shadow.
    const EmitSites atShadow = Inspect(context.get(), shadow);
    EXPECT_EQ(atShadow.storesToOutput, 2u);
    EXPECT_GE(atShadow.loadsFromOutput, 2u);

    // `p` is written before every emit, so it is not carried.
    EXPECT_EQ(VariableNamed(context.get(), "mg_retained_p"), 0u);
    EXPECT_EQ(VariableNamed(context.get(), "mg_retained_Position"), 0u);
}

TEST(RetainGeometryOutputs, AStageThatWritesEveryOutputBeforeEveryEmitIsLeftAlone) {
    const Vector<Uint32> spirv = CompileGeometry(kWritesEverythingPerEmit);
    ASSERT_FALSE(spirv.empty());
    EXPECT_EQ(Retain(spirv), spirv);
}

TEST(RetainGeometryOutputs, ANonGeometryStageIsLeftAlone) {
    const Vector<Uint32> spirv = CompileStage(GL_VERTEX_SHADER, kVertex);
    ASSERT_FALSE(spirv.empty());
    EXPECT_EQ(Retain(spirv), spirv);
}

TEST(RetainGeometryOutputs, OnlyTheWrittenBuiltinMembersAreCarried) {
    const Vector<Uint32> spirv = CompileGeometry(kBuiltinsBeforeLoop);
    ASSERT_FALSE(spirv.empty());
    const Vector<Uint32> out = Retain(spirv);
    ASSERT_NE(out, spirv);
    String messages;
    EXPECT_TRUE(Validates(out, messages)) << messages;

    auto context = Parse(out);
    ASSERT_NE(context, nullptr);
    EXPECT_NE(VariableNamed(context.get(), "mg_retained_PointSize"), 0u);
    EXPECT_NE(VariableNamed(context.get(), "mg_retained_Layer"), 0u);
    EXPECT_EQ(VariableNamed(context.get(), "mg_retained_Position"), 0u);
    EXPECT_EQ(VariableNamed(context.get(), "mg_retained_ClipDistance"), 0u);

    const Uint32 perVertex = PerVertexBlock(context.get());
    ASSERT_NE(perVertex, 0u);
    const EmitSites pointSize = Inspect(context.get(), perVertex, 1);
    EXPECT_EQ(pointSize.emitsPrecededByStore, pointSize.emits);
    EXPECT_EQ(pointSize.storesToOutput, pointSize.emits);
    const EmitSites layer = Inspect(context.get(), VariableNamed(context.get(), "gl_Layer"));
    EXPECT_EQ(layer.emitsPrecededByStore, layer.emits);
    EXPECT_EQ(layer.storesToOutput, layer.emits);

    const String essl = Transpile(out);
    ASSERT_FALSE(essl.empty());
    EXPECT_EQ(essl.find("gl_ClipDistance"), String::npos) << "an unwritten builtin member became active\n" << essl;
    EXPECT_NE(essl.find("gl_Layer = "), String::npos) << essl;
}

TEST(RetainGeometryOutputs, OnlyTheWrittenUserBlockMemberIsCarried) {
    const Vector<Uint32> spirv = CompileGeometry(kUserBlock);
    ASSERT_FALSE(spirv.empty());
    const Vector<Uint32> out = Retain(spirv);
    ASSERT_NE(out, spirv);
    ExpectValidAndCompilable(out);

    auto context = Parse(out);
    ASSERT_NE(context, nullptr);
    EXPECT_NE(VariableNamed(context.get(), "mg_retained_res"), 0u);
    EXPECT_EQ(VariableNamed(context.get(), "mg_retained_p"), 0u);
    const Uint32 block = VariableNamed(context.get(), "vOut");
    ASSERT_NE(block, 0u);
    const EmitSites res = Inspect(context.get(), block, 0);
    EXPECT_EQ(res.emitsPrecededByStore, res.emits);
    EXPECT_EQ(res.storesToOutput, res.emits);
}

TEST(RetainGeometryOutputs, AnEmitInsideAHelperGetsTheCopyToo) {
    const Vector<Uint32> spirv = CompileGeometry(kEmitInHelper);
    ASSERT_FALSE(spirv.empty());
    const Vector<Uint32> out = Retain(spirv);
    ASSERT_NE(out, spirv);
    ExpectValidAndCompilable(out);

    auto context = Parse(out);
    ASSERT_NE(context, nullptr);
    const EmitSites sites = Inspect(context.get(), VariableNamed(context.get(), "res"));
    EXPECT_EQ(sites.emits, 1u);
    EXPECT_EQ(sites.emitsPrecededByStore, 1u);
    EXPECT_EQ(sites.storesToOutput, 1u);
}

TEST(RetainGeometryOutputs, AnOutputReadBackAfterAnEmitIsCarried) {
    const Vector<Uint32> spirv = CompileGeometry(kReadBackAfterEmit);
    ASSERT_FALSE(spirv.empty());
    const Vector<Uint32> out = Retain(spirv);
    ASSERT_NE(out, spirv);
    ExpectValidAndCompilable(out);

    auto context = Parse(out);
    ASSERT_NE(context, nullptr);
    const EmitSites sites = Inspect(context.get(), VariableNamed(context.get(), "res"));
    EXPECT_EQ(sites.storesToOutput, sites.emits);
    EXPECT_EQ(sites.loadsFromOutput, 0u) << "the read-back must see the shadow, not the emitted output";
}

TEST(RetainGeometryOutputs, APartiallyWrittenOutputIsCarriedWhole) {
    const Vector<Uint32> spirv = CompileGeometry(kPartialWrites);
    ASSERT_FALSE(spirv.empty());
    const Vector<Uint32> out = Retain(spirv);
    ASSERT_NE(out, spirv);
    ExpectValidAndCompilable(out);

    auto context = Parse(out);
    ASSERT_NE(context, nullptr);
    const EmitSites sites = Inspect(context.get(), VariableNamed(context.get(), "c"));
    EXPECT_EQ(sites.emitsPrecededByStore, sites.emits);
    EXPECT_EQ(sites.storesToOutput, sites.emits);
    EXPECT_NE(VariableNamed(context.get(), "mg_retained_c"), 0u);
}

TEST(RetainGeometryOutputs, StreamEmitsGetTheCopyToo) {
    const Vector<Uint32> spirv = CompileGeometry(kStreams);
    ASSERT_FALSE(spirv.empty());
    const Vector<Uint32> out = Retain(spirv);
    ASSERT_NE(out, spirv);
    String messages;
    EXPECT_TRUE(Validates(out, messages)) << messages;

    auto context = Parse(out);
    ASSERT_NE(context, nullptr);
    for (const char* name : {"a", "b"}) {
        const EmitSites sites = Inspect(context.get(), VariableNamed(context.get(), name));
        EXPECT_EQ(sites.emits, 2u) << name;
        EXPECT_EQ(sites.emitsPrecededByStore, 2u) << name;
        EXPECT_EQ(sites.storesToOutput, 2u) << name;
    }
}

TEST(RetainGeometryOutputs, TheDeferredEmitReplayCarriesTheRetainedValue) {
    const Vector<Uint32> spirv = CompileGeometry(kFlatBeforeLoopWithMemoryWrite);
    ASSERT_FALSE(spirv.empty());
    const Vector<Uint32> retained = Retain(spirv);
    ASSERT_NE(retained, spirv);
    Vector<uint32_t> deferredSpirv;
    bool deferred = false;
    ASSERT_TRUE(ShaderCompiler::DeferGeometryEmitsForEssl(retained, deferredSpirv, deferred, true));
    ASSERT_TRUE(deferred);
    ExpectValidAndCompilable(deferredSpirv);

    // The snapshot taken at each recorded emit reads `res` after the copy, so the replay restores it.
    auto context = Parse(deferredSpirv);
    ASSERT_NE(context, nullptr);
    const EmitSites sites = Inspect(context.get(), VariableNamed(context.get(), "res"));
    EXPECT_EQ(sites.emits, 1u);
    EXPECT_EQ(sites.emitsPrecededByStore, 1u);
    EXPECT_GE(sites.loadsFromOutput, 1u);
}

TEST(RetainGeometryOutputs, TheSanitizeChainBothBackendsConsumeRunsIt) {
    const Vector<Uint32> spirv = CompileGeometry(kFlatBeforeLoop);
    ASSERT_FALSE(spirv.empty());
    Vector<uint32_t> sanitized;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(spirv, sanitized, true, true));
    ExpectValidAndCompilable(sanitized);

    auto context = Parse(sanitized);
    ASSERT_NE(context, nullptr);
    ASSERT_NE(VariableNamed(context.get(), "mg_retained_res"), 0u);
    const EmitSites sites = Inspect(context.get(), VariableNamed(context.get(), "res"));
    EXPECT_EQ(sites.emitsPrecededByStore, sites.emits);
    EXPECT_EQ(sites.storesToOutput, sites.emits);
}
