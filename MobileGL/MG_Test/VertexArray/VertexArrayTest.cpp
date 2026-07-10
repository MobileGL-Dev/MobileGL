// MobileGL - MobileGL/MG_Test/VertexArray/VertexArrayTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"

#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/VertexArray/GL_VertexArray.h>
#include <MG_Impl/GLImpl/VertexArray/Validators.h>
#include <MG_State/GLState/Core.h>

using namespace MobileGL;

class VertexArrayTest : public ::testing::Test {
protected:
    SharedPtr<MG_State::GLState::BufferObject> CreateTestVBO() {
        Vector<Uint> bufferNames;
        MobileGL::MG_State::pGLContext->GenBufferNames(1, bufferNames);
        auto vbo = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[0]);
        MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex).Bind(vbo);

        Vector<float> vertexData = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f};
        SizeT byteSize = vertexData.size() * sizeof(float);
        vbo->Resize(byteSize);
        DataPtr ptr{.data = vertexData.data(), .size = byteSize};
        vbo->UploadData(ptr, 0);

        return vbo;
    }
    void SetUp() override { MobileGL::Initialize(); }

    void TearDown() override {}
};

TEST_F(VertexArrayTest, GenerateAndBindVAO) {
    Vector<Uint> vaoNames;
    MobileGL::MG_State::pGLContext->GenVertexArrayNames(2, vaoNames);
    auto vao0 = MobileGL::MG_State::pGLContext->CreateVertexArrayObject(vaoNames[0]);
    auto vao1 = MobileGL::MG_State::pGLContext->CreateVertexArrayObject(vaoNames[1]);

    MobileGL::MG_State::pGLContext->BindVertexArray(vaoNames[0]);
    ASSERT_EQ(MobileGL::MG_State::pGLContext->GetBoundVertexArray(), vao0);

    MobileGL::MG_State::pGLContext->BindVertexArray(vaoNames[1]);
    ASSERT_EQ(MobileGL::MG_State::pGLContext->GetBoundVertexArray(), vao1);

    // MobileGL::MG_State::pGLContext->BindVertexArray(0);
    // ASSERT_EQ(MobileGL::MG_State::pGLContext->GetBoundVertexArray(), nullptr);
    // Do not detect if it supports default VAO
}

TEST_F(VertexArrayTest, VertexAttributeSetup) {
    Vector<Uint> vaoNames;
    MobileGL::MG_State::pGLContext->GenVertexArrayNames(1, vaoNames);
    auto vao = MobileGL::MG_State::pGLContext->CreateVertexArrayObject(vaoNames[0]);
    MobileGL::MG_State::pGLContext->BindVertexArray(vaoNames[0]);

    auto vbo = CreateTestVBO();

    vao->EnableAttribute(0);
    vao->SetAttributeFormat(0, 4, DataType::Float32, false, 8 * sizeof(float), 0, false);
    vao->BindAttributeBuffer(0, vbo);

    vao->EnableAttribute(1);
    vao->SetAttributeFormat(1, 4, DataType::Float32, false, 8 * sizeof(float), 4 * sizeof(float), false);
    vao->BindAttributeBuffer(1, vbo);

    const auto& attr0 = vao->GetAttribute(0);
    ASSERT_TRUE(attr0.Enabled);
    ASSERT_EQ(attr0.Size, 4);
    ASSERT_EQ(attr0.Type, DataType::Float32);
    ASSERT_EQ(attr0.Stride, 8 * sizeof(float));
    ASSERT_EQ(attr0.Offset, 0);
    ASSERT_EQ(attr0.Buffer, vbo);

    const auto& attr1 = vao->GetAttribute(1);
    ASSERT_TRUE(attr1.Enabled);
    ASSERT_EQ(attr1.Offset, 4 * sizeof(float));

    vao->DisableAttribute(1);
    ASSERT_FALSE(vao->IsAttributeEnabled(1));
}

TEST_F(VertexArrayTest, IndexBufferBinding) {
    Vector<Uint> vaoNames;
    MobileGL::MG_State::pGLContext->GenVertexArrayNames(1, vaoNames);
    auto vao = MobileGL::MG_State::pGLContext->CreateVertexArrayObject(vaoNames[0]);
    MobileGL::MG_State::pGLContext->BindVertexArray(vaoNames[0]);

    Vector<Uint> bufferNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(1, bufferNames);
    auto ebo = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[0]);
    MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Index).Bind(ebo);

    Vector<Uint> indices = {0, 1, 2};
    SizeT byteSize = indices.size() * sizeof(Uint);
    ebo->Resize(byteSize);
    DataPtr ptr{.data = indices.data(), .size = byteSize};
    ebo->UploadData(ptr, 0);

    MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Index).Bind(ebo);
    ASSERT_EQ(MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Index).GetBoundObject(), ebo);

    Vector<Uint> newEboNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(1, newEboNames);
    auto newEbo = MobileGL::MG_State::pGLContext->CreateBufferObject(newEboNames[0]);
    MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Index).Bind(newEbo);
    ASSERT_EQ(MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Index).GetBoundObject(), newEbo);
}

TEST_F(VertexArrayTest, DeleteVAO) {
    Vector<Uint> vaoNames;
    MobileGL::MG_State::pGLContext->GenVertexArrayNames(1, vaoNames);
    auto vao = MobileGL::MG_State::pGLContext->CreateVertexArrayObject(vaoNames[0]);

    MobileGL::MG_State::pGLContext->BindVertexArray(vaoNames[0]);
    ASSERT_EQ(MobileGL::MG_State::pGLContext->GetBoundVertexArray(), vao);

    MobileGL::MG_State::pGLContext->MarkVertexArrayForDeletion(vaoNames[0]);

    ASSERT_FALSE(MobileGL::MG_State::pGLContext->ValidateVertexArrayObject(vaoNames[0]));
    ASSERT_EQ(MobileGL::MG_State::pGLContext->GetVertexArrayObject(vaoNames[0]), nullptr);
    const auto boundVao = MobileGL::MG_State::pGLContext->GetBoundVertexArray();
    ASSERT_NE(boundVao, nullptr);
    ASSERT_EQ(boundVao->GetExternalIndex(), 0u);
}

TEST_F(VertexArrayTest, ValidateNamesAndObjects) {
    const Uint count = 5;
    Vector<Uint> vaoNames;
    MobileGL::MG_State::pGLContext->GenVertexArrayNames(count, vaoNames);

    for (Uint i = 0; i < count; i++) {
        ASSERT_TRUE(MobileGL::MG_State::pGLContext->ValidateVertexArrayName(vaoNames[i]));
        ASSERT_FALSE(MobileGL::MG_State::pGLContext->ValidateVertexArrayObject(vaoNames[i]));
    }

    for (Uint i = 0; i < count; i += 2) {
        MobileGL::MG_State::pGLContext->CreateVertexArrayObject(vaoNames[i]);
        ASSERT_TRUE(MobileGL::MG_State::pGLContext->ValidateVertexArrayObject(vaoNames[i]));
    }

    for (Uint i = 1; i < count; i += 2) {
        MobileGL::MG_State::pGLContext->MarkVertexArrayForDeletion(vaoNames[i]);
        ASSERT_FALSE(MobileGL::MG_State::pGLContext->ValidateVertexArrayName(vaoNames[i]));
    }
}

TEST_F(VertexArrayTest, MultipleAttributes) {
    Vector<Uint> vaoNames;
    MobileGL::MG_State::pGLContext->GenVertexArrayNames(1, vaoNames);
    auto vao = MobileGL::MG_State::pGLContext->CreateVertexArrayObject(vaoNames[0]);
    MobileGL::MG_State::pGLContext->BindVertexArray(vaoNames[0]);

    auto vboPos = CreateTestVBO();
    Vector<Uint> vboNormalNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(1, vboNormalNames);
    auto vboNormal = MobileGL::MG_State::pGLContext->CreateBufferObject(vboNormalNames[0]);

    Vector<float> normals(12, 0.5f);
    SizeT byteSize = normals.size() * sizeof(float);
    vboNormal->Resize(byteSize);
    DataPtr ptr{.data = normals.data(), .size = byteSize};
    vboNormal->UploadData(ptr, 0);

    vao->EnableAttribute(0);
    vao->SetAttributeFormat(0, 3, DataType::Float32, false, 3 * sizeof(float), 0, false);
    vao->BindAttributeBuffer(0, vboPos);

    vao->EnableAttribute(1);
    vao->SetAttributeFormat(1, 3, DataType::Float32, true, 3 * sizeof(float), 0, false);
    vao->BindAttributeBuffer(1, vboNormal);

    vao->EnableAttribute(2);
    vao->SetAttributeFormat(2, 4, DataType::Uint8, true, 4 * sizeof(Uint8), 0, false);

    const auto& attr0 = vao->GetAttribute(0);
    ASSERT_EQ(attr0.Size, 3);
    ASSERT_EQ(attr0.Buffer, vboPos);

    const auto& attr1 = vao->GetAttribute(1);
    ASSERT_TRUE(attr1.Normalized);
    ASSERT_EQ(attr1.Buffer, vboNormal);

    const auto& attr2 = vao->GetAttribute(2);
    ASSERT_EQ(attr2.Type, DataType::Uint8);
    ASSERT_EQ(attr2.Buffer, nullptr);

    vao->DisableAttribute(1);
    vao->EnableAttribute(1);
    ASSERT_TRUE(vao->IsAttributeEnabled(1));
}

TEST_F(VertexArrayTest, BoundVAOPreservesState) {
    Vector<Uint> vaoNames;
    MobileGL::MG_State::pGLContext->GenVertexArrayNames(2, vaoNames);
    auto vao1 = MobileGL::MG_State::pGLContext->CreateVertexArrayObject(vaoNames[0]);
    auto vao2 = MobileGL::MG_State::pGLContext->CreateVertexArrayObject(vaoNames[1]);

    auto vbo = CreateTestVBO();

    MobileGL::MG_State::pGLContext->BindVertexArray(vaoNames[0]);
    vao1->EnableAttribute(0);
    vao1->SetAttributeFormat(0, 4, DataType::Float32, false, 0, 0, false);
    vao1->BindAttributeBuffer(0, vbo);

    MobileGL::MG_State::pGLContext->BindVertexArray(vaoNames[1]);
    vao2->EnableAttribute(1);
    vao2->SetAttributeFormat(1, 3, DataType::Float32, true, 0, 0, false);

    MobileGL::MG_State::pGLContext->BindVertexArray(vaoNames[0]);

    const auto& attr = vao1->GetAttribute(0);
    ASSERT_TRUE(attr.Enabled);
    ASSERT_EQ(attr.Size, 4);
    ASSERT_EQ(attr.Buffer, vbo);

    ASSERT_FALSE(vao2->IsAttributeEnabled(0));
}

// The current-value array must cover the full attribute capacity. It used to be sized 16 while the
// DirectVulkan draw path indexed it with shader locations up to 31, reading past the end; the only
// guard was MOBILEGL_ASSERT, which expands to nothing outside debug builds.
TEST_F(VertexArrayTest, CurrentVertexAttributeStorageCoversFullCapacity) {
    constexpr Uint capacity = static_cast<Uint>(MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS);
    const Uint highIndex = capacity - 1;

    MG_State::pGLContext->SetCurrentVertexAttributeFloat(highIndex, {1.0f, 2.0f, 3.0f, 4.0f});
    const auto& stored = MG_State::pGLContext->GetCurrentVertexAttribute(highIndex);
    EXPECT_FLOAT_EQ(stored.floatValue[0], 1.0f);
    EXPECT_FLOAT_EQ(stored.floatValue[3], 4.0f);

    // Neighbouring slots keep the GL default of (0, 0, 0, 1).
    const auto& untouched = MG_State::pGLContext->GetCurrentVertexAttribute(highIndex - 1);
    EXPECT_FLOAT_EQ(untouched.floatValue[0], 0.0f);
    EXPECT_FLOAT_EQ(untouched.floatValue[3], 1.0f);

    // Out-of-range access must be bounded at runtime, not just asserted in debug builds.
    const auto& outOfRange = MG_State::pGLContext->GetCurrentVertexAttribute(capacity);
    EXPECT_FLOAT_EQ(outOfRange.floatValue[0], 0.0f);
    EXPECT_FLOAT_EQ(outOfRange.floatValue[3], 1.0f);

    MG_State::pGLContext->SetCurrentVertexAttributeFloat(capacity, {9.0f, 9.0f, 9.0f, 9.0f});
    EXPECT_FLOAT_EQ(MG_State::pGLContext->GetCurrentVertexAttribute(highIndex).floatValue[0], 1.0f);
}

// A binding point the backend cannot address as an attribute must be rejected: the default mapping
// is the identity, so accepting it would resolve into an attribute index the backend then rejects on
// every draw.
TEST_F(VertexArrayTest, VertexBindingIndexIsBoundedByTheAdvertisedAttribLimit) {
    Vector<Uint> vaoNames;
    MG_State::pGLContext->GenVertexArrayNames(1, vaoNames);
    MG_State::pGLContext->CreateVertexArrayObject(vaoNames[0]);
    MG_State::pGLContext->BindVertexArray(vaoNames[0]);
    MG_State::pGLContext->ClearErrors();

    const GLuint outOfRange = MG_Impl::GLImpl::VertexArrayImpl::GetMaxVertexAttribs();
    MG_Impl::GLImpl::VertexAttribBinding(0, outOfRange);
    EXPECT_TRUE(MG_State::pGLContext->HasGLError());
}

// The default attribute -> binding-point mapping is the identity. It used to be a 16-element literal
// list, so every attribute at or above 16 silently resolved against binding point 0 instead.
TEST_F(VertexArrayTest, DefaultAttributeBindingIsIdentityAcrossFullCapacity) {
    Vector<Uint> vaoNames;
    MG_State::pGLContext->GenVertexArrayNames(1, vaoNames);
    auto vao = MG_State::pGLContext->CreateVertexArrayObject(vaoNames[0]);
    MG_State::pGLContext->BindVertexArray(vaoNames[0]);

    auto vbo = CreateTestVBO();

    constexpr Uint kHighAttrib = 20;
    static_assert(kHighAttrib >= 16, "must exceed the old 16-entry identity list");
    static_assert(kHighAttrib < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS);
    static_assert(kHighAttrib < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIB_BINDINGS);

    // Binding point kHighAttrib must feed attribute kHighAttrib with no explicit SetAttributeBinding.
    vao->SetAttributeFormatSeparate(kHighAttrib, 3, DataType::Float32, false, false, 12);
    vao->SetBindingBuffer(kHighAttrib, vbo, 16, 24);

    const auto& attr = vao->GetAttribute(kHighAttrib);
    EXPECT_EQ(attr.Buffer, vbo);
    EXPECT_EQ(attr.Stride, 24);
    EXPECT_EQ(attr.Offset, 28u); // binding offset (16) + attribute relative offset (12)

    // Attribute 0 must not have been dragged along by binding point kHighAttrib.
    EXPECT_EQ(vao->GetAttribute(0).Buffer, nullptr);
}

using namespace MobileGL::MG_Impl::GLImpl;

class GeneralVertexArrayTest : public ::testing::Test {
protected:
    void SetUp() override { MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>(); }

    void TearDown() override {
    }

    GLuint CreateVAO() {
        GLuint vao;
        GenVertexArrays(1, &vao);
        BindVertexArray(vao);
        return vao;
    }

    GLuint CreateVBO(GLenum target, GLsizeiptr size, const void* data = nullptr) {
        GLuint vbo;
        GenBuffers(1, &vbo);
        BindBuffer(target, vbo);
        BufferData(target, size, data, GL_STATIC_DRAW);
        return vbo;
    }
};

TEST_F(GeneralVertexArrayTest, General_VAOLifecycle) {
    GLuint vaos[3];

    GenVertexArrays(3, vaos);
    EXPECT_NE(vaos[0], 0);
    EXPECT_NE(vaos[1], 0);
    EXPECT_NE(vaos[2], 0);
    EXPECT_NE(vaos[0], vaos[1]);

    BindVertexArray(vaos[0]);
    EXPECT_EQ(IsVertexArray(vaos[0]), GL_TRUE);

    GLuint deleteVao = vaos[1];
    DeleteVertexArrays(1, &deleteVao);

    BindVertexArray(deleteVao);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);

    DeleteVertexArrays(1, &vaos[0]);
    DeleteVertexArrays(1, &vaos[2]);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_CreateVertexArraysCreatesObjectsWithoutChangingBinding) {
    GLuint bound = CreateVAO();
    auto boundObj = MG_State::pGLContext->GetBoundVertexArray();
    ASSERT_NE(boundObj, nullptr);

    GLuint vaos[2] = {};
    CreateVertexArrays(2, vaos);

    EXPECT_NE(vaos[0], 0u);
    EXPECT_NE(vaos[1], 0u);
    EXPECT_NE(vaos[0], vaos[1]);
    EXPECT_EQ(IsVertexArray(vaos[0]), GL_TRUE);
    EXPECT_EQ(IsVertexArray(vaos[1]), GL_TRUE);
    EXPECT_EQ(MG_State::pGLContext->GetBoundVertexArray(), boundObj);

    DeleteVertexArrays(2, vaos);
    DeleteVertexArrays(1, &bound);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_CreateVertexArraysRejectsNegativeCount) {
    GLuint vao = 0;
    CreateVertexArrays(-1, &vao);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
}

TEST_F(GeneralVertexArrayTest, General_DirectStateAccessConfiguresNamedVAOWithoutChangingBinding) {
    GLuint bound = CreateVAO();
    auto boundObj = MG_State::pGLContext->GetBoundVertexArray();
    ASSERT_NE(boundObj, nullptr);

    GLuint vao = 0;
    CreateVertexArrays(1, &vao);

    GLuint vertexBuffer = 0;
    GLuint indexBuffer = 0;
    CreateBuffers(1, &vertexBuffer);
    CreateBuffers(1, &indexBuffer);
    NamedBufferData(vertexBuffer, 256, nullptr, GL_STATIC_DRAW);
    NamedBufferData(indexBuffer, 128, nullptr, GL_STATIC_DRAW);

    VertexArrayVertexBuffer(vao, 2, vertexBuffer, 16, 24);
    VertexArrayAttribFormat(vao, 2, 3, GL_FLOAT, GL_TRUE, 12);
    EnableVertexArrayAttrib(vao, 2);
    VertexArrayElementBuffer(vao, indexBuffer);

    auto vaoObj = MG_State::pGLContext->GetVertexArrayObject(vao);
    ASSERT_NE(vaoObj, nullptr);

    const auto& attr = vaoObj->GetAttribute(2);
    EXPECT_TRUE(attr.Enabled);
    EXPECT_EQ(attr.Size, 3);
    EXPECT_EQ(attr.Type, DataType::Float32);
    EXPECT_TRUE(attr.Normalized);
    EXPECT_FALSE(attr.IsInteger);
    EXPECT_EQ(attr.Stride, 24);
    // The flat attribute view holds the resolved effective offset:
    // binding offset (16) + attribute relative offset (12).
    EXPECT_EQ(attr.Offset, 28);
    EXPECT_EQ(attr.Buffer, MG_State::pGLContext->GetBufferObject(vertexBuffer));
    EXPECT_EQ(vaoObj->GetIndexBufferBindingSlot().GetBoundObject(), MG_State::pGLContext->GetBufferObject(indexBuffer));
    EXPECT_EQ(MG_State::pGLContext->GetBoundVertexArray(), boundObj);

    DeleteVertexArrays(1, &vao);
    DeleteVertexArrays(1, &bound);
    GLuint buffers[] = {vertexBuffer, indexBuffer};
    DeleteBuffers(2, buffers);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_DirectStateAccessIntegerAttribAndUnbindElementBuffer) {
    GLuint vao = 0;
    GLuint indexBuffer = 0;
    CreateVertexArrays(1, &vao);
    CreateBuffers(1, &indexBuffer);

    VertexArrayAttribIFormat(vao, 1, 4, GL_UNSIGNED_INT, 8);
    EnableVertexArrayAttrib(vao, 1);
    VertexArrayElementBuffer(vao, indexBuffer);
    VertexArrayElementBuffer(vao, 0);

    auto vaoObj = MG_State::pGLContext->GetVertexArrayObject(vao);
    ASSERT_NE(vaoObj, nullptr);

    const auto& attr = vaoObj->GetAttribute(1);
    EXPECT_TRUE(attr.Enabled);
    EXPECT_EQ(attr.Size, 4);
    EXPECT_EQ(attr.Type, DataType::Uint32);
    EXPECT_TRUE(attr.IsInteger);
    EXPECT_FALSE(attr.Normalized);
    EXPECT_EQ(attr.Offset, 8);
    EXPECT_EQ(vaoObj->GetIndexBufferBindingSlot().GetBoundObject(), nullptr);

    DeleteVertexArrays(1, &vao);
    DeleteBuffers(1, &indexBuffer);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_VertexAttributeConfiguration) {
    GLuint vao = CreateVAO();
    GLuint vbo = CreateVBO(GL_ARRAY_BUFFER, 128);

    VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    EnableVertexAttribArray(0);

    VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    EnableVertexAttribArray(1);

    auto vaoObj = MG_State::pGLContext->GetVertexArrayObject(vao);
    ASSERT_NE(vaoObj, nullptr);

    const auto& attr0 = vaoObj->GetAttribute(0);
    EXPECT_TRUE(attr0.Enabled);
    EXPECT_EQ(attr0.Size, 3);
    EXPECT_EQ(attr0.Type, DataType::Float32);
    EXPECT_EQ(attr0.Offset, 0);

    const auto& attr1 = vaoObj->GetAttribute(1);
    EXPECT_TRUE(attr1.Enabled);
    EXPECT_EQ(attr1.Offset, 3 * sizeof(float));

    DisableVertexAttribArray(1);
    EXPECT_FALSE(vaoObj->GetAttribute(1).Enabled);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_IndexBufferBinding) {
    GLuint vao = CreateVAO();
    GLuint ebo = CreateVBO(GL_ELEMENT_ARRAY_BUFFER, 256);

    auto vaoObj = MG_State::pGLContext->GetVertexArrayObject(vao);
    ASSERT_NE(vaoObj, nullptr);

    GLuint newEbo;
    GenBuffers(1, &newEbo);
    BindBuffer(GL_ELEMENT_ARRAY_BUFFER, newEbo);

    EXPECT_EQ(MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Index).GetBoundObject(),
              MG_State::pGLContext->GetBufferObject(newEbo));

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_ElementArrayBufferBindingIsVaoLocalAndZeroUnbinds) {
    GLuint vao1 = CreateVAO();

    GLuint ebo1;
    GenBuffers(1, &ebo1);
    BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo1);

    GLint binding = -1;
    GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &binding);
    EXPECT_EQ(binding, static_cast<GLint>(ebo1));

    auto vaoObj1 = MG_State::pGLContext->GetVertexArrayObject(vao1);
    ASSERT_NE(vaoObj1, nullptr);
    EXPECT_EQ(vaoObj1->GetIndexBufferBindingSlot().GetBoundObject(), MG_State::pGLContext->GetBufferObject(ebo1));

    GLuint vao2 = CreateVAO();
    auto vaoObj2 = MG_State::pGLContext->GetVertexArrayObject(vao2);
    ASSERT_NE(vaoObj2, nullptr);
    GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &binding);
    EXPECT_EQ(binding, 0);
    EXPECT_EQ(vaoObj2->GetIndexBufferBindingSlot().GetBoundObject(), nullptr);

    BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_EQ(MG_State::pGLContext->GetBufferObject(0), nullptr);
    EXPECT_EQ(IsBuffer(0), GL_FALSE);

    BindVertexArray(vao1);
    GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &binding);
    EXPECT_EQ(binding, static_cast<GLint>(ebo1));

    BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &binding);
    EXPECT_EQ(binding, 0);
    EXPECT_EQ(vaoObj1->GetIndexBufferBindingSlot().GetBoundObject(), nullptr);
    EXPECT_EQ(MG_State::pGLContext->GetBufferObject(0), nullptr);
    EXPECT_EQ(IsBuffer(0), GL_FALSE);

    DeleteVertexArrays(1, &vao1);
    DeleteVertexArrays(1, &vao2);
    DeleteBuffers(1, &ebo1);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_ClientSideVertexAttribPointerIsAccepted) {
    GLuint vao = CreateVAO();
    BindBuffer(GL_ARRAY_BUFFER, 0);

    float vertices[] = {0.0f, 0.0f, 1.0f, 1.0f};
    VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    EnableVertexAttribArray(0);

    auto vaoObj = MG_State::pGLContext->GetVertexArrayObject(vao);
    ASSERT_NE(vaoObj, nullptr);

    const auto& attr = vaoObj->GetAttribute(0);
    EXPECT_TRUE(attr.Enabled);
    EXPECT_EQ(attr.Buffer, nullptr);
    EXPECT_EQ(attr.Offset, reinterpret_cast<SizeT>(vertices));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_IntegerAttributes) {
    GLuint vao = CreateVAO();
    GLuint vbo = CreateVBO(GL_ARRAY_BUFFER, 128);

    VertexAttribIPointer(2, 4, GL_UNSIGNED_INT, sizeof(GLuint) * 8, (void*)(sizeof(GLuint) * 4));
    EnableVertexAttribArray(2);

    auto vaoObj = MG_State::pGLContext->GetVertexArrayObject(vao);
    const auto& attr = vaoObj->GetAttribute(2);
    EXPECT_TRUE(attr.Enabled);
    EXPECT_EQ(attr.Size, 4);
    EXPECT_EQ(attr.Type, DataType::Uint32);
    EXPECT_EQ(attr.Offset, sizeof(GLuint) * 4);
    EXPECT_TRUE(attr.IsInteger);
    EXPECT_FALSE(attr.Normalized);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_StatePreservation) {
    GLuint vao1 = CreateVAO();
    GLuint vbo1 = CreateVBO(GL_ARRAY_BUFFER, 64);

    VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    EnableVertexAttribArray(0);

    GLuint vao2;
    GenVertexArrays(1, &vao2);
    BindVertexArray(vao2);
    GLuint vbo2 = CreateVBO(GL_ARRAY_BUFFER, 128);
    VertexAttribPointer(1, 3, GL_FLOAT, GL_TRUE, 0, nullptr);
    EnableVertexAttribArray(1);

    BindVertexArray(vao1);

    auto vaoObj1 = MG_State::pGLContext->GetVertexArrayObject(vao1);
    EXPECT_TRUE(vaoObj1->IsAttributeEnabled(0));
    EXPECT_FALSE(vaoObj1->IsAttributeEnabled(1));

    auto vaoObj2 = MG_State::pGLContext->GetVertexArrayObject(vao2);
    EXPECT_TRUE(vaoObj2->IsAttributeEnabled(1));
    EXPECT_FALSE(vaoObj2->IsAttributeEnabled(0));

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_ErrorConditions) {
    ASSERT_NE(MG_State::pGLContext->GetBoundVertexArray(), nullptr);
    BindBuffer(GL_ARRAY_BUFFER, 0);
    VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    GLuint vao = CreateVAO();
    EnableVertexAttribArray(MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);

    BindBuffer(GL_ARRAY_BUFFER, 0);
    VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    GLuint vbo = CreateVBO(GL_ARRAY_BUFFER, 64);
    VertexAttribPointer(0, 3, 0xFFFFFFFF, GL_FALSE, 0, nullptr);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);

    VertexAttribPointer(0, 5, GL_FLOAT, GL_FALSE, 0, nullptr);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_ComplexUsage) {
    GLuint vao1 = CreateVAO();
    GLuint vboPos = CreateVBO(GL_ARRAY_BUFFER, 256);
    GLuint vboColor = CreateVBO(GL_ARRAY_BUFFER, 128);
    GLuint ebo1 = CreateVBO(GL_ELEMENT_ARRAY_BUFFER, 64);

    VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    EnableVertexAttribArray(0);

    BindBuffer(GL_ARRAY_BUFFER, vboColor);
    VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, nullptr);
    EnableVertexAttribArray(1);

    GLuint vao2;
    GenVertexArrays(1, &vao2);
    BindVertexArray(vao2);
    GLuint vboNormal = CreateVBO(GL_ARRAY_BUFFER, 192);
    GLuint ebo2 = CreateVBO(GL_ELEMENT_ARRAY_BUFFER, 96);

    VertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    EnableVertexAttribArray(2);

    BindVertexArray(vao1);
    auto vaoObj1 = MG_State::pGLContext->GetVertexArrayObject(vao1);
    EXPECT_TRUE(vaoObj1->IsAttributeEnabled(0));
    EXPECT_TRUE(vaoObj1->IsAttributeEnabled(1));
    EXPECT_FALSE(vaoObj1->IsAttributeEnabled(2));
    EXPECT_NE(MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Index).GetBoundObject(), nullptr);

    BindVertexArray(vao2);
    auto vaoObj2 = MG_State::pGLContext->GetVertexArrayObject(vao2);
    EXPECT_TRUE(vaoObj2->IsAttributeEnabled(2));
    EXPECT_FALSE(vaoObj2->IsAttributeEnabled(0));
    EXPECT_NE(MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Index).GetBoundObject(), nullptr);

    DeleteVertexArrays(1, &vao1);
    DeleteVertexArrays(1, &vao2);
    GLuint buffers[] = {vboPos, vboColor, ebo1, vboNormal, ebo2};
    DeleteBuffers(5, buffers);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_DeleteBoundVAO) {
    GLuint vao = CreateVAO();
    GLuint vbo = CreateVBO(GL_ARRAY_BUFFER, 64);
    VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    EnableVertexAttribArray(0);

    DeleteVertexArrays(1, &vao);

    const auto boundVao = MG_State::pGLContext->GetBoundVertexArray();
    ASSERT_NE(boundVao, nullptr);
    EXPECT_EQ(boundVao->GetExternalIndex(), 0u);
    EXPECT_EQ(MG_State::pGLContext->GetVertexArrayObject(vao), nullptr);

    VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralVertexArrayTest, General_ElementBufferBindingPoint) {
    GLuint vao1, vao2;
    GenVertexArrays(1, &vao1);
    GenVertexArrays(1, &vao2);

    GLuint ebo1, ebo2;
    GenBuffers(1, &ebo1);
    GenBuffers(1, &ebo2);

    BindVertexArray(vao1);
    BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo1);

    BindVertexArray(vao2);
    BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo2);

    auto vaoObj1 = MG_State::pGLContext->GetVertexArrayObject(vao1);
    auto vaoObj2 = MG_State::pGLContext->GetVertexArrayObject(vao2);
    EXPECT_EQ(vaoObj1->GetIndexBufferBindingSlot().GetBoundObject(), MG_State::pGLContext->GetBufferObject(ebo1));
    EXPECT_EQ(vaoObj2->GetIndexBufferBindingSlot().GetBoundObject(), MG_State::pGLContext->GetBufferObject(ebo2));

    BindVertexArray(vao1);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo2);

    EXPECT_EQ(MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Index).GetBoundObject(),
              MG_State::pGLContext->GetBufferObject(ebo2));

    BindVertexArray(vao2);
    EXPECT_EQ(MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Index).GetBoundObject(),
              MG_State::pGLContext->GetBufferObject(ebo2));

    BindVertexArray(0);
    // BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo1);
    // EXPECT_EQ(GetError(), GL_INVALID_OPERATION);
    // Do not detect if it supports default VAO

    BindVertexArray(vao1);
    DeleteVertexArrays(1, &vao1);

    BindVertexArray(vao1);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    EXPECT_EQ(MG_State::pGLContext->GetVertexArrayObject(vao1).get(), nullptr);

    BindVertexArray(vao2);
    DeleteBuffers(1, &ebo2);

    EXPECT_EQ(vaoObj2->GetIndexBufferBindingSlot().GetBoundObject().get(), nullptr);

    GLuint ebo3;
    GenBuffers(1, &ebo3);
    BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo3);

    EXPECT_EQ(vaoObj2->GetIndexBufferBindingSlot().GetBoundObject(), MG_State::pGLContext->GetBufferObject(ebo3));

    DeleteVertexArrays(1, &vao2);
    DeleteBuffers(1, &ebo1);
    DeleteBuffers(1, &ebo3);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// GL_CURRENT_VERTEX_ATTRIB is per-context state that exists for every index below
// GL_MAX_VERTEX_ATTRIBS, and defaults to (0, 0, 0, 1).
TEST_F(GeneralVertexArrayTest, General_CurrentVertexAttribRoundTripsAtHighestLegalIndex) {
    CreateVAO();
    const GLuint highIndex = VertexArrayImpl::GetMaxVertexAttribs() - 1;
    ASSERT_GT(highIndex, 0u);

    VertexAttrib4f(highIndex, 1.0f, 2.0f, 3.0f, 4.0f);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    GLfloat values[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    GetVertexAttribfv(highIndex, GL_CURRENT_VERTEX_ATTRIB, values);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_FLOAT_EQ(values[0], 1.0f);
    EXPECT_FLOAT_EQ(values[1], 2.0f);
    EXPECT_FLOAT_EQ(values[2], 3.0f);
    EXPECT_FLOAT_EQ(values[3], 4.0f);

    // Untouched attributes keep the GL default of (0, 0, 0, 1).
    GLfloat defaults[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    GetVertexAttribfv(highIndex - 1, GL_CURRENT_VERTEX_ATTRIB, defaults);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_FLOAT_EQ(defaults[0], 0.0f);
    EXPECT_FLOAT_EQ(defaults[1], 0.0f);
    EXPECT_FLOAT_EQ(defaults[2], 0.0f);
    EXPECT_FLOAT_EQ(defaults[3], 1.0f);
}

// glVertexAttrib{1,2,3}f fill the components the caller omitted with (0, 0, 1).
TEST_F(GeneralVertexArrayTest, General_CurrentVertexAttribFillsOmittedComponents) {
    CreateVAO();

    VertexAttrib1f(1, 7.0f);
    GLfloat one[4] = {};
    GetVertexAttribfv(1, GL_CURRENT_VERTEX_ATTRIB, one);
    EXPECT_FLOAT_EQ(one[0], 7.0f);
    EXPECT_FLOAT_EQ(one[1], 0.0f);
    EXPECT_FLOAT_EQ(one[2], 0.0f);
    EXPECT_FLOAT_EQ(one[3], 1.0f);

    VertexAttrib2f(2, 7.0f, 8.0f);
    GLfloat two[4] = {};
    GetVertexAttribfv(2, GL_CURRENT_VERTEX_ATTRIB, two);
    EXPECT_FLOAT_EQ(two[1], 8.0f);
    EXPECT_FLOAT_EQ(two[2], 0.0f);
    EXPECT_FLOAT_EQ(two[3], 1.0f);

    VertexAttrib3f(3, 7.0f, 8.0f, 9.0f);
    GLfloat three[4] = {};
    GetVertexAttribfv(3, GL_CURRENT_VERTEX_ATTRIB, three);
    EXPECT_FLOAT_EQ(three[2], 9.0f);
    EXPECT_FLOAT_EQ(three[3], 1.0f);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The integer current-value views must survive a round trip without going through float.
TEST_F(GeneralVertexArrayTest, General_CurrentVertexAttribIntegerRoundTrip) {
    CreateVAO();

    VertexAttribI4i(1, -5, 6, -7, 8);
    GLint signedValues[4] = {};
    GetVertexAttribIiv(1, GL_CURRENT_VERTEX_ATTRIB, signedValues);
    EXPECT_EQ(signedValues[0], -5);
    EXPECT_EQ(signedValues[1], 6);
    EXPECT_EQ(signedValues[2], -7);
    EXPECT_EQ(signedValues[3], 8);

    VertexAttribI4ui(2, 10u, 20u, 30u, 40u);
    GLuint unsignedValues[4] = {};
    GetVertexAttribIuiv(2, GL_CURRENT_VERTEX_ATTRIB, unsignedValues);
    EXPECT_EQ(unsignedValues[0], 10u);
    EXPECT_EQ(unsignedValues[3], 40u);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The GL_CURRENT_VERTEX_ATTRIB branch returned before any index validation, so an out-of-range
// index silently read past the current-value array instead of raising GL_INVALID_VALUE.
TEST_F(GeneralVertexArrayTest, General_CurrentVertexAttribQueryRejectsOutOfRangeIndex) {
    CreateVAO();
    const GLuint outOfRange = VertexArrayImpl::GetMaxVertexAttribs();

    GLfloat floats[4] = {-1.0f, -2.0f, -3.0f, -4.0f};
    GetVertexAttribfv(outOfRange, GL_CURRENT_VERTEX_ATTRIB, floats);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    EXPECT_FLOAT_EQ(floats[0], -1.0f);
    EXPECT_FLOAT_EQ(floats[3], -4.0f);

    GLint ints[4] = {-1, -2, -3, -4};
    GetVertexAttribiv(outOfRange, GL_CURRENT_VERTEX_ATTRIB, ints);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    EXPECT_EQ(ints[0], -1);

    GLint signedInts[4] = {-1, -2, -3, -4};
    GetVertexAttribIiv(outOfRange, GL_CURRENT_VERTEX_ATTRIB, signedInts);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    EXPECT_EQ(signedInts[0], -1);

    GLuint uints[4] = {1u, 2u, 3u, 4u};
    GetVertexAttribIuiv(outOfRange, GL_CURRENT_VERTEX_ATTRIB, uints);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    EXPECT_EQ(uints[0], 1u);
}

// ---- newly-implemented glVertexAttrib* current-value setter funnels -------------------------------

// Family A: the d/s/bv/iv/uiv/usv forms are value-preserving, NOT normalized; and the short/scalar
// forms fill omitted components with (0,0,1).
TEST_F(GeneralVertexArrayTest, CurrentAttrib_NonNormalizedValuePreserving) {
    CreateVAO();
    GLfloat out[4];

    // 3-component short: 32767 must stay 32767.0f (proves no normalization), w filled to 1.
    VertexAttrib3s(1, -5, 0, 32767);
    GetVertexAttribfv(1, GL_CURRENT_VERTEX_ATTRIB, out);
    EXPECT_FLOAT_EQ(out[0], -5.0f);
    EXPECT_FLOAT_EQ(out[1], 0.0f);
    EXPECT_FLOAT_EQ(out[2], 32767.0f);
    EXPECT_FLOAT_EQ(out[3], 1.0f);

    // 4-component byte vector: w comes from v[3], not forced to 1.
    const GLbyte bytes[4] = {1, 2, 3, 4};
    VertexAttrib4bv(1, bytes);
    GetVertexAttribfv(1, GL_CURRENT_VERTEX_ATTRIB, out);
    EXPECT_FLOAT_EQ(out[0], 1.0f);
    EXPECT_FLOAT_EQ(out[3], 4.0f);

    // ushort 65535 must NOT normalize to 1.0.
    const GLushort ushorts[4] = {65535, 0, 0, 0};
    VertexAttrib4usv(1, ushorts);
    GetVertexAttribfv(1, GL_CURRENT_VERTEX_ATTRIB, out);
    EXPECT_FLOAT_EQ(out[0], 65535.0f);

    // 1-component double: fills (0,0,1).
    VertexAttrib1d(1, 0.5);
    GetVertexAttribfv(1, GL_CURRENT_VERTEX_ATTRIB, out);
    EXPECT_FLOAT_EQ(out[0], 0.5f);
    EXPECT_FLOAT_EQ(out[1], 0.0f);
    EXPECT_FLOAT_EQ(out[2], 0.0f);
    EXPECT_FLOAT_EQ(out[3], 1.0f);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Family B: GL 3.3 Core signed normalization is (2c+1)/(2^b-1) -- maps the full range to exactly
// [-1,1] (byte -128 -> -1, 127 -> +1) and cannot represent 0 exactly (0 -> 1/255). This is the test
// that fails against the GL 4.2 c/(2^(b-1)-1) rule.
TEST_F(GeneralVertexArrayTest, CurrentAttrib_SignedNormalizedUsesGl33Formula) {
    CreateVAO();
    GLfloat out[4];

    const GLbyte extremes[4] = {-128, 127, 0, 127};
    VertexAttrib4Nbv(1, extremes);
    GetVertexAttribfv(1, GL_CURRENT_VERTEX_ATTRIB, out);
    EXPECT_FLOAT_EQ(out[0], -1.0f);              // exact, no clamp
    EXPECT_FLOAT_EQ(out[1], 1.0f);               // exact
    EXPECT_FLOAT_EQ(out[2], 1.0f / 255.0f);      // 0 -> 1/255, NOT 0.0
    EXPECT_FLOAT_EQ(out[3], 1.0f);

    const GLshort sExtremes[4] = {-32768, 32767, 0, 0};
    VertexAttrib4Nsv(1, sExtremes);
    GetVertexAttribfv(1, GL_CURRENT_VERTEX_ATTRIB, out);
    EXPECT_FLOAT_EQ(out[0], -1.0f);
    EXPECT_FLOAT_EQ(out[1], 1.0f);

    // 32-bit signed: endpoints must be exact -- fails if computed in float or if 2*INT_MAX overflows.
    const GLint iExtremes[4] = {INT_MIN, INT_MAX, 0, 0};
    VertexAttrib4Niv(1, iExtremes);
    GetVertexAttribfv(1, GL_CURRENT_VERTEX_ATTRIB, out);
    EXPECT_FLOAT_EQ(out[0], -1.0f);
    EXPECT_FLOAT_EQ(out[1], 1.0f);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Family B unsigned normalization is unchanged across versions: c/(2^b-1), 0 -> 0, max -> 1.
TEST_F(GeneralVertexArrayTest, CurrentAttrib_UnsignedNormalized) {
    CreateVAO();
    GLfloat out[4];

    const GLushort us[4] = {0, 65535, 0, 0};
    VertexAttrib4Nusv(1, us);
    GetVertexAttribfv(1, GL_CURRENT_VERTEX_ATTRIB, out);
    EXPECT_FLOAT_EQ(out[0], 0.0f);
    EXPECT_FLOAT_EQ(out[1], 1.0f);

    // 32-bit unsigned endpoints must be exact (needs double divisor).
    const GLuint ui[4] = {0u, 0xFFFFFFFFu, 0u, 0u};
    VertexAttrib4Nuiv(1, ui);
    GetVertexAttribfv(1, GL_CURRENT_VERTEX_ATTRIB, out);
    EXPECT_FLOAT_EQ(out[0], 0.0f);
    EXPECT_FLOAT_EQ(out[1], 1.0f);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Family C: the I* forms write the INTEGER view verbatim (no float round-trip), fill w with integer 1,
// and route signed/unsigned to the right setter.
TEST_F(GeneralVertexArrayTest, CurrentAttrib_IntegerFunnels) {
    CreateVAO();

    // Scalar signed: w must be the integer 1, not 0.
    VertexAttribI1i(1, 7);
    GLint iv[4];
    GetVertexAttribIiv(1, GL_CURRENT_VERTEX_ATTRIB, iv);
    EXPECT_EQ(iv[0], 7);
    EXPECT_EQ(iv[1], 0);
    EXPECT_EQ(iv[2], 0);
    EXPECT_EQ(iv[3], 1);

    // INT_MAX must survive verbatim -- would corrupt to 2147483648 through the float view.
    const GLint big[4] = {INT_MAX, 0, 0, 0};
    VertexAttribI4iv(1, big);
    GetVertexAttribIiv(1, GL_CURRENT_VERTEX_ATTRIB, iv);
    EXPECT_EQ(iv[0], INT_MAX);

    // I4bv sign-extends into the signed view.
    const GLbyte sb[4] = {-100, 1, 2, 3};
    VertexAttribI4bv(1, sb);
    GetVertexAttribIiv(1, GL_CURRENT_VERTEX_ATTRIB, iv);
    EXPECT_EQ(iv[0], -100);

    // I4ubv zero-extends into the UNSIGNED view (not the normalized-float 4Nubv path).
    const GLubyte ub[4] = {200, 0, 0, 0};
    VertexAttribI4ubv(1, ub);
    GLuint uv[4];
    GetVertexAttribIuiv(1, GL_CURRENT_VERTEX_ATTRIB, uv);
    EXPECT_EQ(uv[0], 200u);

    // I2uiv reads exactly 2 elements; w == 1u.
    const GLuint two[2] = {5u, 6u};
    VertexAttribI2uiv(1, two);
    GetVertexAttribIuiv(1, GL_CURRENT_VERTEX_ATTRIB, uv);
    EXPECT_EQ(uv[0], 5u);
    EXPECT_EQ(uv[1], 6u);
    EXPECT_EQ(uv[2], 0u);
    EXPECT_EQ(uv[3], 1u);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Family D: glGetVertexAttribdv reports the float-view current value as four doubles, needs no bound
// VAO, and enforces the same error rules as its float sibling.
TEST_F(GeneralVertexArrayTest, CurrentAttrib_GetVertexAttribdv) {
    CreateVAO();

    VertexAttrib4f(1, 0.25f, 0.5f, 0.75f, 1.0f);
    GLdouble d[4] = {-1.0, -1.0, -1.0, -1.0};
    GetVertexAttribdv(1, GL_CURRENT_VERTEX_ATTRIB, d);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_DOUBLE_EQ(d[0], 0.25);
    EXPECT_DOUBLE_EQ(d[1], 0.5);
    EXPECT_DOUBLE_EQ(d[2], 0.75);
    EXPECT_DOUBLE_EQ(d[3], 1.0);

    // Null params -> GL_INVALID_VALUE, nothing written.
    GetVertexAttribdv(1, GL_CURRENT_VERTEX_ATTRIB, nullptr);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);

    // Out-of-range index -> GL_INVALID_VALUE, params untouched.
    GLdouble d2[4] = {9.0, 9.0, 9.0, 9.0};
    GetVertexAttribdv(VertexArrayImpl::GetMaxVertexAttribs(), GL_CURRENT_VERTEX_ATTRIB, d2);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    EXPECT_DOUBLE_EQ(d2[0], 9.0);
}

