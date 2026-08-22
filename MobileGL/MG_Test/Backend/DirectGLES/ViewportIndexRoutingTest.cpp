// MobileGL - MobileGL/MG_Test/Backend/DirectGLES/ViewportIndexRoutingTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The ESSL half of the gl_ViewportIndex routing emulation (MG_Backend/DirectGLES/Managers.cpp).
// GLES has one viewport, one scissor rectangle and one depth range where GL 4.1 has sixteen of
// each selected per primitive, and the target device has no GL_OES_viewport_array to borrow, so
// DirectGLES turns the builtin into an ordinary flat varying and gives the fragment stage a gate
// the draw path replays against.
//
// Both passes are pure String -> String over what SPIRV-Cross emits once LowerViewportIndexPass
// has demoted the builtin, so no GL context and no driver: the shapes they have to survive - and
// the ones they must refuse - can be pinned here rather than only on a device. What they cannot
// pin is that the routing produces the right pixels; that is
// MG_IntegrationTest/Scenarios/ViewportArrayScenario.cpp, which runs the same claim through both
// backends.

#include <gtest/gtest.h>

#include <MG_Backend/DirectGLES/Managers.h>

using MobileGL::Bool;
using MobileGL::String;
using MobileGL::MG_Backend::DirectGLES::InjectViewportIndexPassGate;
using MobileGL::MG_Backend::DirectGLES::PromoteViewportIndexGlobalToVarying;

namespace {
    Bool Contains(const String& haystack, const String& needle) {
        return haystack.find(needle) != String::npos;
    }

    // What SPIRV-Cross hands the backend for a geometry stage after LowerViewportIndexPass has
    // demoted gl_ViewportIndex: a plain file-scope global the shader still writes and which, until
    // this pass runs, nothing anywhere reads.
    constexpr const char* kLoweredGeometryShader = R"(#version 320 es
layout(invocations = 16, points) in;
layout(max_vertices = 4, triangle_strip) out;

layout(location = 0) flat out int gsIndex;
int mg_ViewportIndex;

void main()
{
    gsIndex = gl_InvocationID;
    mg_ViewportIndex = gl_InvocationID;
    gl_Position = vec4(-1.0, -1.0, 0.0, 1.0);
    EmitVertex();
    EndPrimitive();
}
)";

    constexpr const char* kFragmentShader = R"(#version 320 es
precision mediump float;
precision highp int;

layout(location = 0) flat in int gsIndex;
layout(location = 0) out highp vec4 fragColor;

void main()
{
    fragColor = vec4(float(gsIndex));
}
)";
} // namespace

// The promotion itself. The declaration becomes an interface variable and the STORE is left
// exactly where it was - the pass must not touch the body, because the body is the application's.
TEST(ViewportIndexRoutingTest, TheDemotedGlobalBecomesAFlatVarying) {
    String source = kLoweredGeometryShader;
    ASSERT_TRUE(PromoteViewportIndexGlobalToVarying(source)) << source;

    EXPECT_TRUE(Contains(source, "flat out highp int mg_ViewportIndex;")) << source;
    EXPECT_FALSE(Contains(source, "\nint mg_ViewportIndex;")) << source;
    EXPECT_TRUE(Contains(source, "    mg_ViewportIndex = gl_InvocationID;")) << source;
}

// FLAT is the semantics and not a hint: GL takes a primitive's viewport index from its provoking
// vertex, and flat interpolation is what delivers that. An interpolated integer would not even
// compile in ESSL, so losing the qualifier fails loudly - but silently losing it to a `smooth`
// rewrite somewhere downstream would route by whichever vertex the rasterizer felt like.
TEST(ViewportIndexRoutingTest, ThePromotedVaryingIsFlatAndCarriesNoExplicitLocation) {
    String source = kLoweredGeometryShader;
    ASSERT_TRUE(PromoteViewportIndexGlobalToVarying(source));

    const size_t declPos = source.find("flat out highp int mg_ViewportIndex;");
    ASSERT_NE(declPos, String::npos) << source;
    // No layout(location = N): the two stages are transpiled independently and cannot agree on a
    // number, so the varying is matched by NAME. A location that appeared here would have to
    // appear identically in the fragment stage, which nothing can guarantee.
    const size_t lineStart = source.rfind('\n', declPos);
    const String declLine = source.substr(lineStart + 1, declPos - lineStart - 1);
    EXPECT_EQ(declLine, "") << "the declaration must start its own line, with no layout qualifier";
}

// A precision-qualified declaration is the same declaration. SPIRV-Cross prints one or the other
// depending on what the module carried, and a pass that only matched the bare form would leave
// half the drivers unrouted while reporting success.
TEST(ViewportIndexRoutingTest, APrecisionQualifiedDeclarationIsPromotedToo) {
    String source = "#version 320 es\nhighp int mg_ViewportIndex;\nvoid main() { mg_ViewportIndex = 3; }\n";
    ASSERT_TRUE(PromoteViewportIndexGlobalToVarying(source)) << source;
    EXPECT_TRUE(Contains(source, "flat out highp int mg_ViewportIndex;")) << source;
}

// A stage that never routed must come out byte-identical, because every stage of every program on
// this backend goes through the pass.
TEST(ViewportIndexRoutingTest, AStageWithoutTheGlobalIsUntouched) {
    const String before = kFragmentShader;
    String source = before;
    EXPECT_FALSE(PromoteViewportIndexGlobalToVarying(source));
    EXPECT_EQ(source, before);
}

// The one shape that would silently break a shader: a name that ends in mg_ViewportIndex but is
// not the declaration. Only a declaration starting its own line may be rewritten.
TEST(ViewportIndexRoutingTest, ADeclarationThatIsNotAtLineStartIsRefused) {
    const String before = "#version 320 es\nuniform highp int mg_ViewportIndex;\nvoid main() {}\n";
    String source = before;
    EXPECT_FALSE(PromoteViewportIndexGlobalToVarying(source));
    EXPECT_EQ(source, before);
}

// The fragment gate. Three things have to be true at once: the varying and the uniform are
// declared, the application's entry point survives under a new name, and the new entry point
// discards on a mask miss and calls the old one otherwise.
TEST(ViewportIndexRoutingTest, TheFragmentGateWrapsTheEntryPoint) {
    String source = kFragmentShader;
    ASSERT_TRUE(InjectViewportIndexPassGate(source)) << source;

    EXPECT_TRUE(Contains(source, "flat in highp int mg_ViewportIndex;")) << source;
    EXPECT_TRUE(Contains(source, "uniform highp int mg_ViewportPassMask;")) << source;
    EXPECT_TRUE(Contains(source, "void mg_ViewportGatedMain()")) << source;
    EXPECT_TRUE(Contains(source, "discard;")) << source;
    EXPECT_TRUE(Contains(source, "mg_ViewportGatedMain();")) << source;
    // The application's body is not edited, only renamed.
    EXPECT_TRUE(Contains(source, "    fragColor = vec4(float(gsIndex));")) << source;
    // Exactly one entry point remains, and it is the wrapper.
    EXPECT_EQ(source.find("void main()"), source.rfind("void main()")) << source;
}

// The shift operand has to be clamped. GL leaves a gl_ViewportIndex outside [0, MAX_VIEWPORTS)
// undefined and the emulation is free to pick anything, but an ESSL shift by >= 32 is undefined
// in a way that can take the whole draw with it - so the gate must not be able to reach one.
TEST(ViewportIndexRoutingTest, TheGateClampsTheShiftIntoRange) {
    String source = kFragmentShader;
    ASSERT_TRUE(InjectViewportIndexPassGate(source));
    EXPECT_TRUE(Contains(source, "mg_ViewportPassMask >> (mg_ViewportIndex & 15)")) << source;
}

// A fragment stage that READS gl_ViewportIndex has no ESSL spelling for it either, and the
// routing varying is exactly the value it wanted. This is the only place the read can be repaired
// - LowerViewportIndexPass deliberately demotes outputs only, because a demoted input would
// answer from an undefined global.
TEST(ViewportIndexRoutingTest, AFragmentStageReadOfTheBuiltinIsRedirectedOntoTheVarying) {
    String source = R"(#version 320 es
precision highp int;
layout(location = 0) out highp vec4 fragColor;
void main()
{
    fragColor = vec4(float(gl_ViewportIndex));
}
)";
    ASSERT_TRUE(InjectViewportIndexPassGate(source)) << source;
    EXPECT_FALSE(Contains(source, "gl_ViewportIndex")) << source;
    EXPECT_TRUE(Contains(source, "fragColor = vec4(float(mg_ViewportIndex));")) << source;
}

// A stage the pass declines must reach the driver exactly as it arrived, not half-rewritten.
// The caller logs the decline and the program still renders - unrouted, which is the old
// behaviour - so a partially edited source here would turn a degradation into a broken shader.
TEST(ViewportIndexRoutingTest, AStageWithNoEntryPointIsDeclinedWithoutBeingEdited) {
    const String before = "#version 320 es\nprecision highp int;\nhighp int f() { return gl_ViewportIndex; }\n";
    String source = before;
    EXPECT_FALSE(InjectViewportIndexPassGate(source));
    EXPECT_EQ(source, before);
}
