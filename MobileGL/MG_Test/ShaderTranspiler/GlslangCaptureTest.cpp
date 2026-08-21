// MobileGL - MobileGL/MG_Test/ShaderTranspiler/GlslangCaptureTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// WHAT SURVIVES MOBILEGL'S PARSE, ASKED OF GLSLANG ITSELF.
//
// Every shader is parsed as an EShClientVulkan client under
// setEnvInputVulkanRulesRelaxed(), which destroys some of the GL declarations MobileGL
// still has to answer for. Which ones it destroys - and WHERE - decides whether a piece of
// information can be captured from glslang at all or has to be reconstructed. That question
// used to be answered by comments; these cases answer it by running the real pipeline and
// reading the real qualifiers back.
//
// The probe drives ShaderCompiler::CompileShader (the production parse configuration, byte
// for byte) and then the production mapIO, with a resolver that snapshots every entity's
// qualifier AT THE COLLECT CALLBACK - which is before glslang's IO mapper writes its
// auto-assigned bindings back into the types (iomapper.cpp:240). That callback is the last
// moment at which "the shader declared this" and "glslang invented this" are still
// different statements.

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/Converters/GLToGlslang/ProgramEnumConverter.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>
#include <MG_Util/ShaderTranspiler/glslang/TMglGlslIoResolver.h>

using namespace MobileGL;
using namespace MobileGL::MG_Util::ShaderTranspiler;

namespace {
    // One entity as the collect callback sees it.
    struct ProbedEntity {
        Bool hasBinding = false;
        Uint binding = 0;
        Bool hasLocation = false;
        Int location = 0;
        Bool isBlock = false;
        Bool isBufferBlock = false;
        Bool isSampler = false;
    };

    // A pass-through resolver that records instead of deciding. It derives from the SAME
    // base MobileGL ships (TDefaultGlslIoResolver) so the callbacks fire in the same order
    // and with the same arguments the production resolver sees.
    class ProbeResolver : public glslang::TDefaultGlslIoResolver {
    public:
        explicit ProbeResolver(const glslang::TProgram& program, const EShLanguage stage)
            : TDefaultGlslIoResolver(*program.getIntermediate(stage)) {}

        void reserverResourceSlot(glslang::TVarEntryInfo& ent, TInfoSink& infoSink) override {
            Record(ent);
            TDefaultGlslIoResolver::reserverResourceSlot(ent, infoSink);
        }

        void reserverStorageSlot(glslang::TVarEntryInfo& ent, TInfoSink& infoSink) override {
            Record(ent);
            TDefaultGlslIoResolver::reserverStorageSlot(ent, infoSink);
        }

        std::map<String, ProbedEntity> probed;

    private:
        void Record(const glslang::TVarEntryInfo& ent) {
            const glslang::TType& type = ent.symbol->getType();
            const glslang::TQualifier& qualifier = type.getQualifier();
            ProbedEntity& record = probed[ent.symbol->getAccessName().c_str()];
            record.hasBinding = qualifier.hasBinding();
            record.binding = qualifier.hasBinding() ? qualifier.layoutBinding : 0u;
            record.hasLocation = qualifier.hasLocation();
            record.location = qualifier.hasLocation() ? static_cast<Int>(qualifier.layoutLocation) : -1;
            record.isBlock = type.getBasicType() == glslang::EbtBlock;
            record.isBufferBlock = record.isBlock && qualifier.storage == glslang::EvqBuffer;
            record.isSampler = type.getBasicType() == glslang::EbtSampler;
        }
    };

    // Parses `source` exactly as production does, links it, and returns what the collect
    // callback saw. Fails the calling test (through the ASSERT_* the caller applies to the
    // optional) rather than throwing.
    std::optional<std::map<String, ProbedEntity>> ProbeShader(const GLenum stage, const String& source,
                                                              String& outLog) {
        ShaderAttrib shaderAttrib{.shaderType = stage, .sourceStr = source};
        auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
        if (!shaderResult) {
            outLog = shaderResult.error().log;
            return std::nullopt;
        }

        auto program = MakeShared<glslang::TProgram>();
        program->addShader(shaderResult.value().get());
        if (!program->link(EShMsgDefault)) {
            outLog = program->getInfoLog();
            return std::nullopt;
        }

        const EShLanguage lang = MG_Util::ConvertGLEnumToEShLanguage(stage);
        ProbeResolver resolver(*program, lang);
        auto ioMapper = UniquePtr<glslang::TIoMapper>(glslang::GetGlslIoMapper());
        if (!program->mapIO(&resolver, ioMapper.get())) {
            outLog = program->getInfoLog();
            return std::nullopt;
        }
        return resolver.probed;
    }

    // What ONE production link captures, taken through the real entry points rather than
    // through a probe: ShaderCompiler::CompileShader and ShaderCompiler::LinkProgram with the
    // same ProgramAttrib ProgramLinkTask builds. `captureEnabled` false leaves both OUT
    // pointers null, which is the negative control every capture case below pairs itself with.
    struct LinkCapture {
        Bool linked = false;
        String log;
        UnorderedMap<String, Uint> opaqueBindings;
        std::set<String> storageBlocksWithoutBinding;
        UnorderedMap<String, Int> uniformLocations;
    };

    LinkCapture CaptureFromLink(const Vector<Pair<GLenum, String>>& stages, const Bool captureEnabled = true) {
        LinkCapture capture;
        ProgramAttrib programAttrib;
        for (const auto& [stage, source] : stages) {
            ShaderAttrib shaderAttrib{.shaderType = stage, .sourceStr = source};
            auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
            if (!shaderResult) {
                capture.log = shaderResult.error().log;
                return capture;
            }
            for (const auto& [name, location] : CollectExplicitUniformLocations(*shaderResult.value())) {
                capture.uniformLocations.emplace(name, location);
            }
            programAttrib.shaders.push_back(shaderResult.value());
        }

        if (captureEnabled) {
            programAttrib.explicitOpaqueUniformBindings = &capture.opaqueBindings;
            programAttrib.storageBlocksWithoutBinding = &capture.storageBlocksWithoutBinding;
        }

        auto programResult = ShaderCompiler::LinkProgram(programAttrib);
        if (!programResult) {
            capture.log = programResult.error().log;
            return capture;
        }
        capture.linked = true;
        return capture;
    }

    LinkCapture CaptureFromCompute(const String& source, const Bool captureEnabled = true) {
        return CaptureFromLink({{GL_COMPUTE_SHADER, source}}, captureEnabled);
    }
} // namespace

class GlslangCaptureProbeTest : public ::testing::Test {
protected:
    void SetUp() override { MobileGL::Initialize(); }
};

// THE HEADLINE ANSWER, and it contradicts what ExtractExplicitOpaqueBindings' header claimed
// for years ("the Vulkan-client relaxed parse strips these before mapIO can capture them").
//
// A PLAIN sampler/image uniform never enters vkRelaxedRemapUniformVariable's body at all: the
// guard at ParseHelper.cpp:8255-8259 admits only types that containsNonOpaque(), atomic_uint,
// or a sampler inside a STRUCT. So the binding is still on the qualifier when the IO mapper
// collects it, and it is glslang - not a lexer - that knows the answer.
//
// The default-block uniform LOCATION is the opposite verdict, and this case pins both halves
// side by side so neither can be assumed from the other: it is stripped inside that same
// function (ParseHelper.cpp:8261-8263, `layoutLocation = layoutLocationEnd`), which is why
// recovering it needs a snapshot taken INSIDE glslang rather than a resolver callback.
TEST_F(GlslangCaptureProbeTest, OpaqueBindingsSurviveTheRelaxedParseButPlainUniformLocationsDoNot) {
    const String source = R"(#version 430 core
layout(local_size_x = 1) in;
layout(binding = 3) uniform sampler2D probeSampler;
layout(binding = 5, rgba32f) uniform image2D probeImage;
layout(location = 7) uniform vec4 probeUniform;
layout(std430, binding = 2) buffer BoundBlock { uint bound; } boundInstance;
layout(std430) buffer UnboundBlock { uint unbound; } unboundInstance;
void main() {
  unboundInstance.unbound = boundInstance.bound + uint(texture(probeSampler, vec2(0)).x) +
                            uint(imageLoad(probeImage, ivec2(0)).x) + uint(probeUniform.x);
}
)";

    String log;
    const auto probed = ProbeShader(GL_COMPUTE_SHADER, source, log);
    ASSERT_TRUE(probed.has_value()) << log;

    ASSERT_TRUE(probed->contains("probeSampler"));
    EXPECT_TRUE(probed->at("probeSampler").hasBinding)
        << "a plain sampler's layout(binding=) is NOT stripped by the relaxed parse";
    EXPECT_EQ(probed->at("probeSampler").binding, 3u);

    ASSERT_TRUE(probed->contains("probeImage"));
    EXPECT_TRUE(probed->at("probeImage").hasBinding)
        << "images take the same path as samplers (both are EbtSampler)";
    EXPECT_EQ(probed->at("probeImage").binding, 5u);

    // The default-block uniform is gone from the entity list entirely - it was swept into
    // MGL_GLOBAL_UBO - and even if it were here it would carry layoutLocationEnd. That is
    // exactly why the location capture has to happen inside glslang.
    if (probed->contains("probeUniform")) {
        EXPECT_FALSE(probed->at("probeUniform").hasLocation)
            << "vkRelaxedRemapUniformVariable strips a default-block uniform's location";
    }
}

// The other half of the storage-block question: at the collect callback, "declared no
// binding" is still distinguishable from "glslang picked one", which is what makes
// TMglGlslIoResolver the right place to recover GL's binding-0 default. Ten lines later
// (iomapper.cpp:240) both blocks carry a number and nothing can tell them apart.
TEST_F(GlslangCaptureProbeTest, StorageBlockBindingPresenceIsStillTruthfulAtTheCollectCallback) {
    const String source = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 2) buffer BoundBlock { uint bound; } boundInstance;
layout(std430) buffer UnboundBlock { uint unbound; } unboundInstance;
layout(std140) uniform UniformBlock { uint u; } uniformInstance;
void main() { unboundInstance.unbound = boundInstance.bound + uniformInstance.u; }
)";

    String log;
    const auto probed = ProbeShader(GL_COMPUTE_SHADER, source, log);
    ASSERT_TRUE(probed.has_value()) << log;

    ASSERT_TRUE(probed->contains("BoundBlock"));
    EXPECT_TRUE(probed->at("BoundBlock").isBufferBlock);
    EXPECT_TRUE(probed->at("BoundBlock").hasBinding);
    EXPECT_EQ(probed->at("BoundBlock").binding, 2u);

    ASSERT_TRUE(probed->contains("UnboundBlock"));
    EXPECT_TRUE(probed->at("UnboundBlock").isBufferBlock);
    EXPECT_FALSE(probed->at("UnboundBlock").hasBinding)
        << "an unqualified storage block must still read as unqualified here";

    // A uniform block is a different binding space with its own default path; the capture
    // must be able to tell the two apart, which storage == EvqBuffer does.
    ASSERT_TRUE(probed->contains("UniformBlock"));
    EXPECT_FALSE(probed->at("UniformBlock").isBufferBlock);
}

// ===========================================================================================
// THE CAPTURES THEMSELVES.
//
// Every case below is the SCENARIO of a scan these captures replaced, re-pointed at the new
// mechanism. Keeping the scenarios is the point: the interesting inputs were found the
// expensive way (a production regression, a CTS failure), and they are still the inputs that
// decide whether the recovery is right - what changed is only who answers.
//
// Every capture also has a NEGATIVE CONTROL: the same shader with the capture switched off,
// asserting the answer disappears. Without one, a case that passes proves only that SOMETHING
// produced the number.
// ===========================================================================================

// KHR-GL43.explicit_uniform_location.uniform-loc-nondecimal: GLSL integer literals are C-style,
// so layout(location = 0xA) is 10 and layout(location = 010) is OCTAL 8. The lexical extractor
// this replaces had to implement that rule itself, got it wrong for both spellings, and was
// then fixed - twice. glslang has always had it, because it is the GLSL lexer.
TEST_F(GlslangCaptureProbeTest, UniformLocationsCarryNonDecimalIntegerLiterals) {
    const String source = R"(#version 430 core
layout(location = 0xA) uniform vec4 hexLower;
layout(location = 0X1f) uniform vec4 hexUpper;
layout(location = 010) uniform vec4 octal;
layout(location = 3u) uniform vec4 unsignedSuffix;
layout(location = 0x2) uniform float hexArray[0x3];
void main() {
  gl_Position = hexLower + hexUpper + octal + unsignedSuffix + vec4(hexArray[2]);
}
)";

    ShaderAttrib shaderAttrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = source};
    auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
    ASSERT_TRUE(shaderResult) << shaderResult.error().log;

    const UnorderedMap<String, Int> locations = CollectExplicitUniformLocations(*shaderResult.value());
    ASSERT_EQ(locations.count("hexLower"), 1u);
    EXPECT_EQ(locations.at("hexLower"), 10);
    ASSERT_EQ(locations.count("hexUpper"), 1u);
    EXPECT_EQ(locations.at("hexUpper"), 31);
    ASSERT_EQ(locations.count("octal"), 1u);
    EXPECT_EQ(locations.at("octal"), 8) << "a leading zero is octal in GLSL, not decimal";
    ASSERT_EQ(locations.count("unsignedSuffix"), 1u);
    EXPECT_EQ(locations.at("unsignedSuffix"), 3);
    ASSERT_EQ(locations.count("hexArray"), 1u);
    EXPECT_EQ(locations.at("hexArray"), 2);
}

// The counterweight the lexical version needed a rule for: a location that is not an integer
// literal at all. glslang REJECTS those outright rather than skipping them, which is what GLSL
// says should happen - the scanner could only decline to record them and let the declaration
// compile with no location.
TEST_F(GlslangCaptureProbeTest, ANonIntegralUniformLocationIsRejectedRatherThanIgnored) {
    ShaderAttrib attrib{.shaderType = GL_VERTEX_SHADER,
                        .sourceStr = "#version 430 core\nlayout(location = 1.0) uniform vec4 notAnInteger;\n"
                                     "void main() { gl_Position = notAnInteger; }\n"};
    EXPECT_FALSE(ShaderCompiler::CompileShader(attrib).has_value())
        << "a float location is a compile-time error, not a declaration without a location";
}

// KHR-GL43.explicit_uniform_location.uniform-loc-array-of-arrays: glslang reflects
// `float u[2][3]` as "u[0][0]" and "u[1][0]", and the location assigner resolves such a name by
// stripping the single trailing "[0]" - so the map has to answer "u[1]", not just "u". The
// synthesized keys are the one piece of the old extractor that survived the migration, because
// they are a REFLECTION-NAME mapping rather than a reading of the source.
TEST_F(GlslangCaptureProbeTest, UniformLocationsExpandArrayOfArraysElements) {
    const String source = R"(#version 430 core
layout(location = 2) uniform float two_d[2][3];
layout(location = 20) uniform float three_d[2][2][4];
layout(location = 40) uniform float one_d[3];
void main() { gl_Position = vec4(two_d[1][2] + three_d[1][1][3] + one_d[2]); }
)";

    ShaderAttrib shaderAttrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = source};
    auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
    ASSERT_TRUE(shaderResult) << shaderResult.error().log;
    const UnorderedMap<String, Int> locations = CollectExplicitUniformLocations(*shaderResult.value());

    // The root entry is unchanged - the synthesized keys are additional, never a replacement.
    ASSERT_EQ(locations.count("two_d"), 1u);
    EXPECT_EQ(locations.at("two_d"), 2);
    // One key per outer index, each starting a run of the innermost dimension (3 here).
    ASSERT_EQ(locations.count("two_d[0]"), 1u);
    EXPECT_EQ(locations.at("two_d[0]"), 2);
    ASSERT_EQ(locations.count("two_d[1]"), 1u);
    EXPECT_EQ(locations.at("two_d[1]"), 5);

    // Three dimensions: glslang expands all but the innermost, so both outer indices are spelled.
    ASSERT_EQ(locations.count("three_d"), 1u);
    EXPECT_EQ(locations.at("three_d"), 20);
    ASSERT_EQ(locations.count("three_d[0][0]"), 1u);
    EXPECT_EQ(locations.at("three_d[0][0]"), 20);
    ASSERT_EQ(locations.count("three_d[0][1]"), 1u);
    EXPECT_EQ(locations.at("three_d[0][1]"), 24);
    ASSERT_EQ(locations.count("three_d[1][0]"), 1u);
    EXPECT_EQ(locations.at("three_d[1][0]"), 28);
    ASSERT_EQ(locations.count("three_d[1][1]"), 1u);
    EXPECT_EQ(locations.at("three_d[1][1]"), 32);

    // A 1-D array needs no expansion: stripping "[0]" already reaches the root.
    ASSERT_EQ(locations.count("one_d"), 1u);
    EXPECT_EQ(locations.at("one_d"), 40);
    EXPECT_EQ(locations.count("one_d[0]"), 0u);
}

// A DELIBERATE BEHAVIOUR CHANGE, recorded here because it is the one place the migration does
// not reproduce the old answer.
//
// The lexical extractor advanced the location across the declarators of one statement, so
// `layout(location = 50) uniform float first[3], second;` gave second = 53. GLSL has no such
// rule: 4.60 4.4 says a layout qualifier applies to THE DECLARATION, i.e. identically to every
// declarator in it, and 4.4.3 then makes two uniforms sharing a location an error. glslang - the
// reference front end - assigns 50 to both, and the CTS never exercises the form at all (its
// generator emits one uniform per declaration, es31cExplicitUniformLocationTest.cpp
// streamDefinition). The advance was an invention of the scanner; this is what the parser says.
TEST_F(GlslangCaptureProbeTest, EveryDeclaratorOfOneStatementCarriesTheQualifiersLocation) {
    const String source = R"(#version 430 core
layout(location = 50) uniform float first[0x3], second;
void main() { gl_Position = vec4(first[2] + second); }
)";

    ShaderAttrib shaderAttrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = source};
    auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
    ASSERT_TRUE(shaderResult) << shaderResult.error().log;
    const UnorderedMap<String, Int> locations = CollectExplicitUniformLocations(*shaderResult.value());

    ASSERT_EQ(locations.count("first"), 1u);
    EXPECT_EQ(locations.at("first"), 50) << "0x3 is three elements, not zero and not three hundred";
    ASSERT_EQ(locations.count("second"), 1u);
    EXPECT_EQ(locations.at("second"), 50);
}

// THE NEGATIVE CONTROL for the uniform-location capture: nothing else in the parsed module
// knows the number. If the snapshot inside vkRelaxedRemapUniformVariable were removed, this is
// the state the location assigner would be left with - no qualifier, no reflection entry, and
// therefore a first-fit location that has nothing to do with what the shader declared.
TEST_F(GlslangCaptureProbeTest, WithoutTheSnapshotAPlainUniformsLocationIsNowhereInTheModule) {
    const String source = R"(#version 430 core
layout(location = 7) uniform vec4 tint;
void main() { gl_Position = tint; }
)";

    ShaderAttrib shaderAttrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = source};
    auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
    ASSERT_TRUE(shaderResult) << shaderResult.error().log;

    // The capture, on.
    const UnorderedMap<String, Int> locations = CollectExplicitUniformLocations(*shaderResult.value());
    ASSERT_EQ(locations.count("tint"), 1u);
    EXPECT_EQ(locations.at("tint"), 7);

    // The capture, off - i.e. everything the module itself can still say. The uniform is a
    // member of MGL_GLOBAL_UBO by now, and no symbol in the module carries location 7.
    auto program = MakeShared<glslang::TProgram>();
    program->addShader(shaderResult.value().get());
    ASSERT_TRUE(program->link(EShMsgDefault)) << program->getInfoLog();
    ASSERT_TRUE(program->buildReflection(EShReflectionStrictArraySuffix | EShReflectionBasicArraySuffix |
                                         EShReflectionAllBlockVariables | EShReflectionSharedStd140UBO));
    for (Int i = 0; i < program->getNumUniformVariables(); ++i) {
        const auto& uniform = program->getUniform(i);
        if (uniform.name != "tint") continue;
        // Copied out: layoutLocationEnd is a static const with no out-of-line definition, so
        // binding it to EXPECT_EQ's const reference would ODR-use it and fail to link.
        const Uint noLocation = glslang::TQualifier::layoutLocationEnd;
        EXPECT_EQ(uniform.layoutLocation(), noLocation)
            << "if reflection could answer this, the glslang patch would be unnecessary";
    }
}

// KHR-GL43.explicit_uniform_location: layout(binding = 0x2) on a sampler is its initial texture
// unit. Same scenario the lexical extractor carried, now answered by the IO resolver - which
// gets the C-style literal rules for free, and sees a binding no scanner could have read.
TEST_F(GlslangCaptureProbeTest, OpaqueBindingsAreCapturedIncludingNonDecimalAndMacroSpellings) {
    const String source = R"(#version 430 core
#define UNIT_FROM_A_MACRO 5
layout(local_size_x = 1) in;
layout(binding = 0x2) uniform sampler2D hexUnit;
layout(binding = 012) uniform sampler2D octalUnit;
layout(binding = 1u) uniform sampler2D suffixedUnit;
layout(binding = UNIT_FROM_A_MACRO) uniform sampler2D macroUnit;
layout(binding = 6) uniform sampler2D arrayUnits[3];
uniform sampler2D noUnit;
layout(std430, binding = 0) buffer Out { vec4 v; } o;
void main() {
  o.v = texture(hexUnit, vec2(0)) + texture(octalUnit, vec2(0)) + texture(suffixedUnit, vec2(0)) +
        texture(macroUnit, vec2(0)) + texture(arrayUnits[1], vec2(0)) + texture(noUnit, vec2(0));
}
)";

    const LinkCapture capture = CaptureFromCompute(source);
    ASSERT_TRUE(capture.linked) << capture.log;

    ASSERT_EQ(capture.opaqueBindings.count("hexUnit"), 1u);
    EXPECT_EQ(capture.opaqueBindings.at("hexUnit"), 2u);
    ASSERT_EQ(capture.opaqueBindings.count("octalUnit"), 1u);
    EXPECT_EQ(capture.opaqueBindings.at("octalUnit"), 10u) << "012 is octal ten, not twelve";
    ASSERT_EQ(capture.opaqueBindings.count("suffixedUnit"), 1u);
    EXPECT_EQ(capture.opaqueBindings.at("suffixedUnit"), 1u);
    // The whole reason the capture moved: the AST sees expanded text.
    ASSERT_EQ(capture.opaqueBindings.count("macroUnit"), 1u)
        << "a unit spelled as a macro is a declared unit like any other";
    EXPECT_EQ(capture.opaqueBindings.at("macroUnit"), 5u);
    // An array is keyed by its declared name, which is what the reflection lookup strips "[0]"
    // to reach.
    ASSERT_EQ(capture.opaqueBindings.count("arrayUnits"), 1u);
    EXPECT_EQ(capture.opaqueBindings.at("arrayUnits"), 6u);
    // Reported POSITIVELY: a sampler that declared no unit must not appear at all, or it would
    // be given one it never asked for.
    EXPECT_EQ(capture.opaqueBindings.count("noUnit"), 0u);
}

// THE NEGATIVE CONTROL for the opaque-binding capture.
TEST_F(GlslangCaptureProbeTest, OpaqueBindingsDisappearWhenTheResolverCaptureIsOff) {
    const String source = R"(#version 430 core
layout(local_size_x = 1) in;
layout(binding = 3) uniform sampler2D unit;
layout(std430, binding = 0) buffer Out { vec4 v; } o;
void main() { o.v = texture(unit, vec2(0)); }
)";

    ASSERT_EQ(CaptureFromCompute(source).opaqueBindings.count("unit"), 1u);
    const LinkCapture off = CaptureFromCompute(source, /*captureEnabled=*/false);
    ASSERT_TRUE(off.linked) << off.log;
    EXPECT_TRUE(off.opaqueBindings.empty())
        << "nothing but the resolver fills this map; a non-empty result would mean the capture "
           "is being shadowed by a leftover path";
}

// KHR-GL43.compute_shader.resource-ubo's own shape: an unqualified storage block alongside the
// uniform blocks whose presence is what pushes it off binding 0. GL 4.3 core 7.8 puts such a
// block on binding ZERO; by the time reflection is built glslang has invented a number and
// written it into the qualifier, so this capture is the only surviving record.
TEST_F(GlslangCaptureProbeTest, UnqualifiedStorageBlocksAreNamedAndQualifiedOnesAreNot) {
    const String source = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std140) uniform InputBuffer { vec4 data[4]; } g_in_buffer[12];
layout(std430) buffer OutputBuffer { vec4 data0[4]; } g_out_buffer;
layout(std430, binding = 3) buffer BoundBlock { vec4 data1[4]; } g_bound;
layout(binding = 5, std430) buffer BoundFirst { vec4 data2[4]; } g_bound_first;
void main() {
  g_out_buffer.data0[0] = g_in_buffer[0].data[0] + g_bound.data1[0] + g_bound_first.data2[0];
}
)";

    const LinkCapture capture = CaptureFromCompute(source);
    ASSERT_TRUE(capture.linked) << capture.log;

    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("OutputBuffer"), 1u)
        << "the block the test binds at 0 with glBindBufferBase must be recognised";
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("BoundBlock"), 0u)
        << "a declared binding must never be defaulted away";
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("BoundFirst"), 0u)
        << "the binding may appear anywhere in the layout list, not only last";
    // A UNIFORM block is a different binding space with its own glUniformBlockBinding path, and
    // its default is already handled where uniformBlockBinding is seeded. Naming it here would
    // make the seeder default a resource it does not own.
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("InputBuffer"), 0u)
        << "uniform blocks are out of scope";
}

// The capture must not mistake a buffer-typed SAMPLER or a member qualifier for a block, and
// memory qualifiers in either order must not cost a block its binding - the dangerous
// direction, because a false positive here DEFAULTS AWAY a binding the shader really declared.
TEST_F(GlslangCaptureProbeTest, StorageBlockCaptureSurvivesMemoryQualifiersAndIgnoresBufferSamplers) {
    const String source = R"(#version 430 core
layout(local_size_x = 1) in;
uniform samplerBuffer texelSampler;
layout(std430, binding = 1) coherent restrict buffer AfterLayout { uint a; } afterLayout;
readonly layout(std430, binding = 2) buffer BeforeLayout { uint b; } beforeLayout;
writeonly buffer NoBindingAtAll { uint c; } noBinding;
void main() { noBinding.c = afterLayout.a + beforeLayout.b + uint(texelFetch(texelSampler, 0).x); }
)";

    const LinkCapture capture = CaptureFromCompute(source);
    ASSERT_TRUE(capture.linked) << capture.log;

    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("AfterLayout"), 0u)
        << "coherent/restrict must not break the qualifier run and lose the binding";
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("BeforeLayout"), 0u)
        << "a qualifier may precede the layout list too";
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("NoBindingAtAll"), 1u)
        << "a memory-qualified block with no binding is still an unqualified block";
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("texelSampler"), 0u)
        << "a samplerBuffer is not a buffer block";
}

// THE REGRESSION THIS MIGRATION EXISTS FOR (7de7cfc6,
// minecraft-1.21.1-neoforge-create-indirect-in-world, both backends). Flywheel's indirect
// engine spells every storage-block binding as a macro, and the scan that used to answer this
// question ran on MACRO-UNEXPANDED text: MobileGL's preprocessing rewrites the source, it does
// not run the C preprocessor, so `binding = _FLW_MODEL_BUFFER_BINDING` reached the scanner
// verbatim and "no integer literal" was read as "no binding". All eight blocks were defaulted
// onto binding 0 at once, aliased there, and the engine drew nothing.
//
// It passes here for a structural reason rather than a grammatical one: the IO mapper sees the
// declaration the PARSER built, and the parser ran the preprocessor first. No rule about macro
// spellings exists anywhere in this path, and none can be forgotten.
TEST_F(GlslangCaptureProbeTest, AMacroSpelledStorageBlockBindingIsADeclaredBinding) {
    // Flywheel's own shape, verbatim in structure: the binding is a macro, the block carries
    // memory qualifiers, and the macro's definition is still sitting in the text above it.
    const String source = R"(#version 460 core
#define _FLW_MODEL_BUFFER_BINDING 3
#define _FLW_DRAW_BUFFER_BINDING 4
#define FLW_BINDING binding = 2
#define SSBO_QUALIFIER layout(std430, binding = 6) restrict
layout(local_size_x = 32) in;
layout(std430, binding = _FLW_MODEL_BUFFER_BINDING) restrict readonly buffer ModelBuffer {
    uint models[];
};
layout(std430, binding = _FLW_DRAW_BUFFER_BINDING) restrict buffer DrawBuffer {
    uint draws[];
};
layout(std430, FLW_BINDING) buffer EntryMacro { uint a; } entryMacro;
SSBO_QUALIFIER buffer RunMacro { uint b; } runMacro;
layout(std430, row_major) buffer PlainLayout { uint c; } plainLayout;
layout(std430) buffer ReallyUnqualified { uint u; } reallyUnqualified;
void main() {
  draws[0] = models[0] + reallyUnqualified.u + entryMacro.a + runMacro.b + plainLayout.c;
}
)";

    const LinkCapture capture = CaptureFromCompute(source);
    ASSERT_TRUE(capture.linked) << capture.log;

    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("ModelBuffer"), 0u)
        << "a binding spelled as a macro is still a declared binding, never an absent one";
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("DrawBuffer"), 0u)
        << "every block of the engine would otherwise be defaulted onto 0 together";
    // The two shapes the scanner could only treat as DOUBT - a macro standing in for a whole
    // layout entry, and one standing in for the whole qualifier run - are now ordinary.
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("EntryMacro"), 0u)
        << "a macro that expands to `binding = N` declares a binding";
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("RunMacro"), 0u)
        << "a macro standing in for the whole qualifier run carries its binding too";
    // The counterweight: doubt must not swallow the layout identifiers a buffer block legally
    // carries, or nothing would ever be defaulted again.
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("PlainLayout"), 1u)
        << "std430/row_major are layout identifiers, not bindings";
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("ReallyUnqualified"), 1u)
        << "a block that truly declares no binding is still recognised in the same shader";
}

// THE NEGATIVE CONTROL for the storage-block capture.
TEST_F(GlslangCaptureProbeTest, UnqualifiedStorageBlocksDisappearWhenTheResolverCaptureIsOff) {
    const String source = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430) buffer Unbound { uint u; } unbound;
void main() { unbound.u = 1u; }
)";

    ASSERT_EQ(CaptureFromCompute(source).storageBlocksWithoutBinding.count("Unbound"), 1u);
    const LinkCapture off = CaptureFromCompute(source, /*captureEnabled=*/false);
    ASSERT_TRUE(off.linked) << off.log;
    EXPECT_TRUE(off.storageBlocksWithoutBinding.empty())
        << "nothing but the resolver fills this set; a non-empty result would mean the capture "
           "is being shadowed by a leftover path";
}

// A block declared in two stages contributes ONCE, and the capture is a union across them -
// which is what one resolver serving the whole program gives for free. GLSL requires every
// stage that declares a block to declare it identically, so the stages cannot disagree.
TEST_F(GlslangCaptureProbeTest, TheStorageBlockCaptureIsAUnionAcrossStages) {
    const String vertex = R"(#version 430 core
layout(std430) buffer SharedBlock { uint u; } sharedInstance;
layout(std430) buffer VertexOnly { uint v; } vertexOnly;
void main() { gl_Position = vec4(float(sharedInstance.u + vertexOnly.v)); }
)";
    const String fragment = R"(#version 430 core
layout(std430) buffer SharedBlock { uint u; } sharedInstance;
layout(std430, binding = 4) buffer FragmentBound { uint f; } fragmentBound;
out vec4 colour;
void main() { colour = vec4(float(sharedInstance.u + fragmentBound.f)); }
)";

    const LinkCapture capture =
        CaptureFromLink({{GL_VERTEX_SHADER, vertex}, {GL_FRAGMENT_SHADER, fragment}});
    ASSERT_TRUE(capture.linked) << capture.log;

    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("SharedBlock"), 1u);
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("VertexOnly"), 1u);
    EXPECT_EQ(capture.storageBlocksWithoutBinding.count("FragmentBound"), 0u);
}

// KHR-GL43.shader_atomic_counters.negative-offset-1: an atomic counter at a misaligned offset,
// or one whose last byte passes GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE, is a COMPILE-time error
// (GL 4.6 core 7.7). glslang enforces the alignment rule in fixOffset(), which the relaxed
// parse never reaches - vkRelaxedRemapUniformVariable folds the counter into a synthesized
// block and returns from declareVariable() first - and it never enforced the size ceiling at
// all. Both now run at that fold (ParseHelper.cpp atomicCounterOffsetCheck), so a violation is
// an ordinary parse failure.
TEST_F(GlslangCaptureProbeTest, AtomicCounterOffsetRulesAreRaisedByTheParse) {
    const auto compiles = [](const String& body) {
        const String source = "#version 430 core\n" + body + "void main() {}\n";
        ShaderAttrib attrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = source};
        return ShaderCompiler::CompileShader(attrib).has_value();
    };
    const String maxSize = std::to_string(MAX_ATOMIC_COUNTER_BUFFER_SIZE);
    const String lastLegal = std::to_string(MAX_ATOMIC_COUNTER_BUFFER_SIZE - 4);

    // The boundary itself: the last counter that still fits, and the first that does not.
    EXPECT_TRUE(compiles("layout(binding = 0, offset = " + lastLegal + ") uniform atomic_uint c;\n"));
    EXPECT_FALSE(compiles("layout(binding = 0, offset = " + maxSize + ") uniform atomic_uint c;\n"));

    // An array occupies one word per element, so what has to fit is the LAST one.
    EXPECT_TRUE(compiles("layout(binding = 0, offset = " + std::to_string(MAX_ATOMIC_COUNTER_BUFFER_SIZE - 16) +
                         ") uniform atomic_uint c[4];\n"));
    EXPECT_FALSE(compiles("layout(binding = 0, offset = " + std::to_string(MAX_ATOMIC_COUNTER_BUFFER_SIZE - 8) +
                          ") uniform atomic_uint c[4];\n"));

    // An offset that is not a multiple of 4, and one that is.
    EXPECT_FALSE(compiles("layout(binding = 0, offset = 2) uniform atomic_uint c;\n"));
    EXPECT_TRUE(compiles("layout(binding = 0, offset = 8) uniform atomic_uint c;\n"));

    // A counter with no explicit offset has nothing to judge, and neither has a shader with no
    // counter at all.
    EXPECT_TRUE(compiles("layout(binding = 0) uniform atomic_uint c;\n"));
    EXPECT_TRUE(compiles(""));

    // The gain over the scan this replaces: an array sized by a constant EXPRESSION, and an
    // offset spelled as a macro, are now both judged. The scanner declined both - it read
    // unexpanded text and only understood integer literals.
    EXPECT_FALSE(compiles("const int kCount = 4;\nlayout(binding = 0, offset = " +
                          std::to_string(MAX_ATOMIC_COUNTER_BUFFER_SIZE - 8) +
                          ") uniform atomic_uint c[kCount];\n"));
    EXPECT_FALSE(compiles("#define BAD_OFFSET 2\nlayout(binding = 0, offset = BAD_OFFSET) uniform atomic_uint c;\n"));

    // The counterweight: `offset` as an ordinary identifier is not a layout qualifier, and an
    // offset qualifier on an unrelated declaration must not reach the counter.
    EXPECT_TRUE(compiles("layout(binding = 0) uniform atomic_uint c;\nconst int offset = 99999;\n"));
}
