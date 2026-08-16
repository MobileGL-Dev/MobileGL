// MobileGL - MobileGL/MG_Test/Buffer/BufferTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <limits>

#include "Includes.h"
#include "Init.h"
#include <Config.h>
#include <MG_State/GLState/Core.h>

#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GetProcAddress.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>

using namespace MobileGL;

class BufferTest : public ::testing::Test {
protected:
    // GL error flags are sticky per error code and the context outlives an individual test in this
    // binary, so drain whatever an earlier test left pending - otherwise an error-code assertion
    // here reads someone else's error. Bounded: one flag per code, so this cannot hang the suite.
    static void DrainPendingGlErrors() {
        for (Int drained = 0; drained < 16 && MG_Impl::GLImpl::GetError() != GL_NO_ERROR; ++drained) {
        }
    }

    // The call under test must raise exactly the expected error and nothing more: a second pending
    // error means one entry point queued several, which GetError() would hand out at an unrelated
    // call site later on.
    static void ExpectSingleGlError(GLenum expected) {
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), expected);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "the call recorded more than one error";
    }

    void SetUp() override {
        MobileGL::Initialize();
        DrainPendingGlErrors();
    }

    void TearDown() override {
        // Attribute a leaked error to the test that caused it instead of to whoever runs next.
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "test left an unconsumed GL error behind";
    }
};

TEST_F(BufferTest, Binding) {
    Vector<Uint> bufferNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(3, bufferNames);
    auto& arraySlot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex);
    auto& indexSlot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Uniform);

    auto obj0 = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[0]);
    auto obj1 = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[1]);
    auto obj2 = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[2]);

    arraySlot.Bind(obj0);
    indexSlot.Bind(obj1);

    ASSERT_TRUE(arraySlot.GetBoundObject() == obj0);
    ASSERT_TRUE(indexSlot.GetBoundObject() == obj1);

    arraySlot.Bind(obj2);
    indexSlot.Bind(obj2);
    ASSERT_TRUE(arraySlot.GetBoundObject() == obj2);
    ASSERT_TRUE(indexSlot.GetBoundObject() == obj2);
}

TEST_F(BufferTest, PingPong) {
    auto& readSlot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::CopyRead);
    auto& writeSlot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::CopyWrite);
    {
        Vector<Uint> bufferNames;
        MobileGL::MG_State::pGLContext->GenBufferNames(1, bufferNames);
        auto bufObj = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[0]);

        writeSlot.Bind(bufObj);
        readSlot.Bind(bufObj);
    }

    auto bufWrite = writeSlot.GetBoundObject();

    Vector<Int> data{1, 2, 3, 4, 5};

    SizeT byteSize = data.size() * sizeof(Int);
    // Write data
    bufWrite->Resize(byteSize);
    DataPtr ptr{.data = data.data(), .size = byteSize};
    bufWrite->UploadData(ptr, 0);

    // Readback
    auto bufRead = readSlot.GetBoundObject();
    void* p = bufRead->AcquireMemory(true, true, false);
    Vector<Int> bufdata(data.size());
    memcpy(bufdata.data(), p, byteSize);
    ASSERT_EQ(data, bufdata);
    // Writes bump the change serial so backends can invalidate cached slices.
    ASSERT_GT(bufRead->GetChangeSerial(), 0u);
}

TEST_F(BufferTest, GenerateManyNames_NoPrematureCreation) {
    const SizeT largeCount = 100000; // generate tons of buffer names

    Vector<Uint> names;
    MobileGL::MG_State::pGLContext->GenBufferNames(largeCount, names);

    std::vector<SizeT> indices = {0, 600, 5000, 32768, 99999}; // only create a few buffer objects
    for (SizeT idx : indices) {
        GLuint name = names[idx];
        auto bufObj = MobileGL::MG_State::pGLContext->CreateBufferObject(name);
        auto& slot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Uniform);
        slot.Bind(bufObj);

        Vector<Int> data = {static_cast<Int>(idx + 1), static_cast<Int>(idx + 2)};
        SizeT byteSize = data.size() * sizeof(Int);
        bufObj->Resize(byteSize);
        DataPtr ptr{.data = data.data(), .size = byteSize};
        bufObj->UploadData(ptr, 0);

        Vector<Int> actual(data.size());
        void* p = bufObj->AcquireMemory(false, true, false);
        memcpy(actual.data(), p, byteSize);
        EXPECT_EQ(actual, data);
    }
}

// GL 3.3 core 2.9 name lifecycle. The same three rules are asserted per object family (see the
// texture/vertex-array/framebuffer/renderbuffer suites): a deleted or never-generated name is
// INVALID_OPERATION to bind, deleting one is silent, and a generated-but-never-bound reservation
// is still released so the name gets recycled.
TEST_F(BufferTest, DeleteOfUnknownOrAlreadyDeletedBufferNameIsSilent) {
    GLuint buffer = 0;
    MG_Impl::GLImpl::GenBuffers(1, &buffer);
    ASSERT_NE(buffer, 0u);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Double delete, name 0 and a never-generated name must all be ignored without an error.
    MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const GLuint unknownNames[] = {0u, std::numeric_limits<GLuint>::max()};
    MG_Impl::GLImpl::DeleteBuffers(2, unknownNames);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(BufferTest, DeleteGeneratedButUnboundBufferNameReleasesReservationAndBindFails) {
    GLuint buffer = 0;
    MG_Impl::GLImpl::GenBuffers(1, &buffer);
    ASSERT_NE(buffer, 0u);
    ASSERT_TRUE(MG_State::pGLContext->ValidateBufferName(buffer));

    MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_FALSE(MG_State::pGLContext->ValidateBufferName(buffer));

    MG_Impl::GLImpl::BindBuffer(GL_ARRAY_BUFFER, buffer);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    GLuint recycled = 0;
    MG_Impl::GLImpl::GenBuffers(1, &recycled);
    EXPECT_EQ(recycled, buffer);
}

TEST_F(BufferTest, BindNeverGeneratedBufferNameIsInvalidOperation) {
    // Not a small literal: other tests in this binary share the context and generate names in
    // bulk, so a low number may well be a legitimately reserved name here.
    MG_Impl::GLImpl::BindBuffer(GL_ARRAY_BUFFER, std::numeric_limits<GLuint>::max());
    ExpectSingleGlError(GL_INVALID_OPERATION);
}

TEST_F(BufferTest, AcquireMemory) {
    auto& slot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Uniform);
    Vector<Uint> bufferNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(1, bufferNames);
    auto bufObj = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[0]);
    slot.Bind(bufObj);
    Vector<Int> initData{10, 20, 30, 40, 50};
    SizeT byteSize = initData.size() * sizeof(Int);
    bufObj->Resize(byteSize);
    DataPtr ptr{.data = initData.data(), .size = byteSize};
    bufObj->UploadData(ptr, 0);
    const Uint64 baseSerial = bufObj->GetChangeSerial();
    Int* mappedPtr = static_cast<Int*>(bufObj->AcquireMemory(true, true, true));
    mappedPtr[0] = 100;
    mappedPtr[1] = 200;
    mappedPtr[2] = 300;
    bufObj->ReleaseMemory();
    Vector<Int> expected{100, 200, 300, 40, 50};
    Vector<Int> actual(5);
    void* p = bufObj->AcquireMemory(false, true, false);
    memcpy(actual.data(), p, byteSize);
    ASSERT_EQ(actual, expected);
    // Unmapping a write map flushes the mapped range and bumps the serial.
    ASSERT_GT(bufObj->GetChangeSerial(), baseSerial);
}

TEST_F(BufferTest, AcquireMemoryRangeWithoutExplicit) {
    auto& slot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Uniform);
    Vector<Uint> bufferNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(1, bufferNames);
    auto bufObj = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[0]);
    slot.Bind(bufObj);
    Vector<Int> initData{10, 20, 30, 40, 50};
    SizeT byteSize = initData.size() * sizeof(Int);
    bufObj->Resize(byteSize);
    DataPtr ptr{.data = initData.data(), .size = byteSize};
    bufObj->UploadData(ptr, 0);
    const Uint64 baseSerial = bufObj->GetChangeSerial();

    Range1D mapRange{.start = sizeof(Int), .end = sizeof(Int) * 4};
    Int* mappedPtr = static_cast<Int*>(bufObj->AcquireMemoryRange(mapRange, BufferMappingAccessBit::Write));
    mappedPtr[0] = 200;
    mappedPtr[1] = 300;
    bufObj->ReleaseMemory();
    Vector<Int> expected{10, 200, 300, 40, 50};
    Vector<Int> actual(5);
    void* p = bufObj->AcquireMemory(false, true, false);
    memcpy(actual.data(), p, byteSize);
    ASSERT_EQ(actual, expected);
    ASSERT_GT(bufObj->GetChangeSerial(), baseSerial);
}

TEST_F(BufferTest, AcquireMemoryRangeWithExplicit) {
    auto& slot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Uniform);
    Vector<Uint> bufferNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(1, bufferNames);
    auto bufObj = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[0]);
    slot.Bind(bufObj);

    Vector<Int> initData{10, 20, 30, 40, 50};
    SizeT byteSize = initData.size() * sizeof(Int);
    bufObj->Resize(byteSize);
    DataPtr ptr{.data = initData.data(), .size = byteSize};
    bufObj->UploadData(ptr, 0);

    const Uint64 baseSerial = bufObj->GetChangeSerial();

    Range1D mapRange{.start = sizeof(Int), .end = sizeof(Int) * 4};
    Int* mappedPtr = static_cast<Int*>(
        bufObj->AcquireMemoryRange(mapRange, BufferMappingAccessBit::Write | BufferMappingAccessBit::FlushExplicit));

    mappedPtr[0] = 200;
    mappedPtr[1] = 300;

    // Only the explicitly flushed range reaches the shadow (and the backend).
    bufObj->FlushMemoryRange(0, sizeof(Int));
    const Uint64 flushedSerial = bufObj->GetChangeSerial();
    ASSERT_GT(flushedSerial, baseSerial);

    // FlushExplicit unmap must not flush the rest of the mapped range.
    bufObj->ReleaseMemory();
    ASSERT_EQ(bufObj->GetChangeSerial(), flushedSerial);

    Vector<Int> expected{10, 200, 30, 40, 50};
    Vector<Int> actual(5);
    void* p = bufObj->AcquireMemory(false, true, false);
    memcpy(actual.data(), p, byteSize);
    ASSERT_EQ(actual, expected);
}

TEST_F(BufferTest, CopyBufferSubData) {
    auto& srcSlot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::CopyRead);
    auto& dstSlot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::CopyWrite);

    Vector<Uint> srcNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(1, srcNames);
    auto srcObj = MobileGL::MG_State::pGLContext->CreateBufferObject(srcNames[0]);
    srcSlot.Bind(srcObj);

    Vector<Int> srcData{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    SizeT srcSize = srcData.size() * sizeof(Int);
    srcObj->Resize(srcSize);
    DataPtr srcPtr{.data = srcData.data(), .size = srcSize};
    srcObj->UploadData(srcPtr, 0);

    Vector<Uint> dstNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(1, dstNames);
    auto dstObj = MobileGL::MG_State::pGLContext->CreateBufferObject(dstNames[0]);
    dstSlot.Bind(dstObj);

    Vector<Int> dstData(15, 0);
    SizeT dstSize = dstData.size() * sizeof(Int);
    dstObj->Resize(dstSize);
    DataPtr dstPtr{.data = dstData.data(), .size = dstSize};
    dstObj->UploadData(dstPtr, 0);

    const Uint64 srcSerial = srcObj->GetChangeSerial();
    const Uint64 dstSerial = dstObj->GetChangeSerial();

    dstObj->CopyDataFrom(srcObj, 2 * sizeof(Int), 5 * sizeof(Int), 4 * sizeof(Int));

    Vector<Int> expected{0, 0, 0, 0, 0, 3, 4, 5, 6, 0, 0, 0, 0, 0, 0};

    Vector<Int> actual(15);
    void* p = dstObj->AcquireMemory(false, true, false);
    memcpy(actual.data(), p, dstSize);

    ASSERT_EQ(actual, expected);

    // The copy mutates only the destination.
    ASSERT_GT(dstObj->GetChangeSerial(), dstSerial);
    ASSERT_EQ(srcObj->GetChangeSerial(), srcSerial);
}

TEST_F(BufferTest, GetBufferSubDataRoundTrip) {
    using namespace MobileGL::MG_Impl::GLImpl;
    GLuint buf;
    GenBuffers(1, &buf);
    BindBuffer(GL_ARRAY_BUFFER, buf);

    const Vector<Int> src{10, 20, 30, 40, 50, 60, 70, 80};
    const SizeT bytes = src.size() * sizeof(Int);
    BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), src.data(), GL_STATIC_DRAW);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // Read a middle range [2..6).
    Vector<Int> mid(4, -1);
    GetBufferSubData(GL_ARRAY_BUFFER, 2 * sizeof(Int), 4 * sizeof(Int), mid.data());
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_EQ(mid, (Vector<Int>{30, 40, 50, 60}));

    // Read the whole buffer back.
    Vector<Int> whole(src.size(), 0);
    GetBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), whole.data());
    EXPECT_EQ(whole, src);

    // Out-of-range range -> GL_INVALID_VALUE, destination untouched.
    Vector<Int> guard(2, 999);
    GetBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(bytes) - sizeof(Int), 2 * sizeof(Int), guard.data());
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    EXPECT_EQ(guard, (Vector<Int>{999, 999}));

    // Negative offset -> GL_INVALID_VALUE.
    GetBufferSubData(GL_ARRAY_BUFFER, -1, sizeof(Int), guard.data());
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
}

TEST_F(BufferTest, GetBufferSubDataNoBufferBound) {
    using namespace MobileGL::MG_Impl::GLImpl;
    BindBuffer(GL_ARRAY_BUFFER, 0); // ensure nothing is bound
    Int dst = 0;
    GetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(Int), &dst);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);
}

TEST_F(BufferTest, WriteWhileMapped) {
    auto& slot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::ShaderStorage);
    Vector<Uint> bufferNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(1, bufferNames);
    auto bufObj = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[0]);
    slot.Bind(bufObj);

    Vector<Int> initData(10, 0);
    SizeT byteSize = initData.size() * sizeof(Int);
    bufObj->Resize(byteSize);

    Int* mappedPtr = static_cast<Int*>(bufObj->AcquireMemory(true, true, true));

    for (int i = 0; i < 10; i++) {
        mappedPtr[i] = i * 10;
    }

    bufObj->ReleaseMemory();

    Vector<Int> expected{0, 10, 20, 30, 40, 50, 60, 70, 80, 90};
    Vector<Int> actual(10);
    void* p = bufObj->AcquireMemory(false, true, false);
    memcpy(actual.data(), p, byteSize);

    ASSERT_EQ(actual, expected);

    ASSERT_GT(bufObj->GetChangeSerial(), 0u);
}

TEST_F(BufferTest, PartialUpdate) {
    auto& slot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex);
    Vector<Uint> bufferNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(1, bufferNames);
    auto bufObj = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[0]);
    slot.Bind(bufObj);

    Vector<Int> initData{100, 200, 300, 400, 500};
    SizeT byteSize = initData.size() * sizeof(Int);
    bufObj->Resize(byteSize);
    DataPtr ptr{.data = initData.data(), .size = byteSize};
    bufObj->UploadData(ptr, 0);
    const Uint64 baseSerial = bufObj->GetChangeSerial();

    Vector<Int> update{999, 888};
    bufObj->UploadSubData({(void*)(update.data()), (SizeT)(update.size() * sizeof(Int))}, sizeof(Int));

    Vector<Int> expected{100, 999, 888, 400, 500};
    Vector<Int> actual(5);
    void* p = bufObj->AcquireMemory(false, true, false);
    memcpy(actual.data(), p, byteSize);

    ASSERT_EQ(actual, expected);

    ASSERT_GT(bufObj->GetChangeSerial(), baseSerial);
}

TEST_F(BufferTest, DeleteBufferObject) {
    Vector<Uint> bufferNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(1, bufferNames);
    auto& slot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex);
    auto bufObj = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[0]);
    slot.Bind(bufObj);
    ASSERT_TRUE(slot.GetBoundObject() == bufObj);
    MobileGL::MG_State::pGLContext->MarkBufferObjectForDeletion(bufferNames[0]);
    ASSERT_TRUE(slot.GetBoundObject() == nullptr);
    ASSERT_FALSE(MobileGL::MG_State::pGLContext->GetBufferObject(bufferNames[0]));
}

TEST_F(BufferTest, ParameterBufferBindingAndQuery) {
    Vector<Uint> bufferNames;
    MobileGL::MG_State::pGLContext->GenBufferNames(1, bufferNames);
    auto bufObj = MobileGL::MG_State::pGLContext->CreateBufferObject(bufferNames[0]);

    auto& slot = MobileGL::MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Parameter);
    slot.Bind(bufObj);

    GLint binding = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_PARAMETER_BUFFER_BINDING_ARB, &binding);
    EXPECT_EQ(binding, static_cast<GLint>(bufferNames[0]));

    slot.Bind(nullptr);
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_PARAMETER_BUFFER_BINDING_ARB, &binding);
    EXPECT_EQ(binding, 0);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(BufferTest, BindBufferBaseZeroUnbindsBindingPoint) {
    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    MobileGL::MG_Impl::GLImpl::BufferData(GL_SHADER_STORAGE_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);

    MobileGL::MG_Impl::GLImpl::BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, buffer);
    auto& point = MobileGL::MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, 2);
    ASSERT_NE(point.GetBoundObject(), nullptr);
    EXPECT_EQ(point.GetBoundObject()->GetExternalIndex(), buffer);

    MobileGL::MG_Impl::GLImpl::BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
    EXPECT_EQ(point.GetBoundObject(), nullptr);
    EXPECT_FALSE(MobileGL::MG_State::pGLContext->ValidateBufferObject(0));
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(BufferTest, BindBufferRangeZeroUnbindsBindingPoint) {
    // GL_SHADER_STORAGE_BUFFER offsets must be a multiple of
    // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, so the offset cannot be a literal.
    GLint ssboAlignment = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlignment);
    ASSERT_GT(ssboAlignment, 0);
    const GLintptr offset = ssboAlignment;
    const GLsizeiptr size = 8;

    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    MobileGL::MG_Impl::GLImpl::BufferData(GL_SHADER_STORAGE_BUFFER, offset + size, nullptr, GL_DYNAMIC_DRAW);

    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 3, buffer, offset, size);
    auto& point = MobileGL::MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, 3);
    ASSERT_NE(point.GetBoundObject(), nullptr);
    EXPECT_EQ(point.GetRange().start, static_cast<SizeT>(offset));
    EXPECT_EQ(point.GetRange().end, static_cast<SizeT>(offset + size));

    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 3, 0, 0, 0);
    EXPECT_EQ(point.GetBoundObject(), nullptr);
    EXPECT_FALSE(MobileGL::MG_State::pGLContext->ValidateBufferObject(0));
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// GL 4.6 core tables 23.4/23.5: *_BUFFER_START and *_BUFFER_SIZE report the (offset, size) pair
// glBindBufferRange was ASKED for. They are not clamped to the buffer's storage - a range may
// legally name bytes the buffer does not have, and glBufferData may resize the buffer afterwards
// without the binding's reported window moving. The size arm used to intersect the recorded range
// with the buffer's current size, so binding a range on a still-empty buffer (glGenBuffers with no
// glBufferData - exactly what KHR-GL43.shader_storage_buffer_object.basic-binding does) answered 0
// while START still answered the offset, an internally inconsistent pair no driver reports.
TEST_F(BufferTest, IndexedBufferSizeQueryReportsTheRequestedSizeNotTheBuffersStorage) {
    GLint ssboAlignment = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlignment);
    ASSERT_GT(ssboAlignment, 0);
    const GLintptr offset = ssboAlignment;
    const GLsizeiptr size = 512;

    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    // Deliberately no glBufferData: the name exists, the storage does not.
    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, buffer, offset, size);
    ASSERT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint start32 = 0;
    GLint size32 = 0;
    GLint64 start64 = 0;
    GLint64 size64 = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegeri_v(GL_SHADER_STORAGE_BUFFER_START, 1, &start32);
    MobileGL::MG_Impl::GLImpl::GetIntegeri_v(GL_SHADER_STORAGE_BUFFER_SIZE, 1, &size32);
    MobileGL::MG_Impl::GLImpl::GetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START, 1, &start64);
    MobileGL::MG_Impl::GLImpl::GetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, 1, &size64);
    EXPECT_EQ(start32, static_cast<GLint>(offset));
    EXPECT_EQ(size32, static_cast<GLint>(size));
    EXPECT_EQ(start64, static_cast<GLint64>(offset));
    EXPECT_EQ(size64, static_cast<GLint64>(size));

    // Giving the buffer storage afterwards does not move the window either way.
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    MobileGL::MG_Impl::GLImpl::BufferData(GL_SHADER_STORAGE_BUFFER, offset + size, nullptr, GL_DYNAMIC_DRAW);
    MobileGL::MG_Impl::GLImpl::GetIntegeri_v(GL_SHADER_STORAGE_BUFFER_SIZE, 1, &size32);
    EXPECT_EQ(size32, static_cast<GLint>(size));

    // glBindBufferBase binds the whole buffer and reports (0, 0), not the buffer's size.
    MobileGL::MG_Impl::GLImpl::BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffer);
    MobileGL::MG_Impl::GLImpl::GetIntegeri_v(GL_SHADER_STORAGE_BUFFER_START, 1, &start32);
    MobileGL::MG_Impl::GLImpl::GetIntegeri_v(GL_SHADER_STORAGE_BUFFER_SIZE, 1, &size32);
    EXPECT_EQ(start32, 0);
    EXPECT_EQ(size32, 0);

    MobileGL::MG_Impl::GLImpl::BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    MobileGL::MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(BufferTest, GetInteger64vMaxShaderStorageBlockSize) {
    GLint64 maxSsboBlockSize = 0;
    MobileGL::MG_Impl::GLImpl::GetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxSsboBlockSize);

    EXPECT_GT(maxSsboBlockSize, 0);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(BufferTest, CreateBuffersCreatesObjectsImmediately) {
    GLuint buffers[2] = {};
    MobileGL::MG_Impl::GLImpl::CreateBuffers(2, buffers);

    EXPECT_NE(buffers[0], 0u);
    EXPECT_NE(buffers[1], 0u);
    EXPECT_TRUE(MobileGL::MG_State::pGLContext->ValidateBufferObject(buffers[0]));
    EXPECT_TRUE(MobileGL::MG_State::pGLContext->ValidateBufferObject(buffers[1]));
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(BufferTest, CopyNamedBufferSubDataCopiesBetweenDSABuffers) {
    GLuint buffers[2] = {};
    MobileGL::MG_Impl::GLImpl::CreateBuffers(2, buffers);

    Vector<Uint8> src{1, 2, 3, 4, 5, 6};
    Vector<Uint8> dst(src.size(), 0);
    MobileGL::MG_Impl::GLImpl::NamedBufferData(buffers[0], src.size(), src.data(), GL_STATIC_DRAW);
    MobileGL::MG_Impl::GLImpl::NamedBufferData(buffers[1], dst.size(), dst.data(), GL_STATIC_DRAW);
    MobileGL::MG_Impl::GLImpl::CopyNamedBufferSubData(buffers[0], buffers[1], 1, 2, 3);

    Vector<Uint8> actual(dst.size());
    auto dstObject = MobileGL::MG_State::pGLContext->GetBufferObject(buffers[1]);
    Memcpy(actual.data(), dstObject->AcquireMemory(false, true, false), actual.size());
    EXPECT_EQ(actual, (Vector<Uint8>{0, 0, 2, 3, 4, 0}));
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(BufferTest, ClearNamedBufferDataZeroesStorage) {
    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::CreateBuffers(1, &buffer);

    Vector<Uint8> initial(8, 0x7F);
    MobileGL::MG_Impl::GLImpl::NamedBufferData(buffer, initial.size(), initial.data(), GL_STATIC_DRAW);
    MobileGL::MG_Impl::GLImpl::ClearNamedBufferData(buffer, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);

    Vector<Uint8> actual(initial.size(), 0xFF);
    auto bufferObject = MobileGL::MG_State::pGLContext->GetBufferObject(buffer);
    Memcpy(actual.data(), bufferObject->AcquireMemory(false, true, false), actual.size());
    EXPECT_EQ(actual, Vector<Uint8>(initial.size(), 0));
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(BufferTest, ClearNamedBufferSubDataRepeatsPattern) {
    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::CreateBuffers(1, &buffer);

    Vector<Uint32> initial{0, 0, 0, 0, 0};
    MobileGL::MG_Impl::GLImpl::NamedBufferData(buffer, initial.size() * sizeof(Uint32), initial.data(), GL_STATIC_DRAW);
    const Uint32 pattern = 0xAABBCCDDu;
    MobileGL::MG_Impl::GLImpl::ClearNamedBufferSubData(buffer, GL_R32UI, sizeof(Uint32), sizeof(Uint32) * 3,
                                                       GL_RED_INTEGER, GL_UNSIGNED_INT, &pattern);

    Vector<Uint32> actual(initial.size(), 0);
    auto bufferObject = MobileGL::MG_State::pGLContext->GetBufferObject(buffer);
    Memcpy(actual.data(), bufferObject->AcquireMemory(false, true, false), actual.size() * sizeof(Uint32));
    EXPECT_EQ(actual, (Vector<Uint32>{0, pattern, pattern, pattern, 0}));
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}
TEST_F(BufferTest, ClearBufferSubDataInitializesIrisStaticSsboRange) {
    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);

    Vector<Uint8> initial(32, 0x7F);
    MobileGL::MG_Impl::GLImpl::BufferData(
        GL_SHADER_STORAGE_BUFFER, initial.size(), initial.data(), GL_STATIC_DRAW);
    const GLbyte zero = 0;
    const auto clear = reinterpret_cast<PFNGLCLEARBUFFERSUBDATAPROC>(
        MobileGL::MG_Impl::GetProcAddress("glClearBufferSubData"));
    ASSERT_NE(clear, nullptr);
    clear(GL_SHADER_STORAGE_BUFFER, GL_R8, 4, 24, GL_RED, GL_BYTE, &zero);

    Vector<Uint8> actual(initial.size());
    auto bufferObject = MobileGL::MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    Memcpy(actual.data(), bufferObject->AcquireMemory(false, true, false), actual.size());
    EXPECT_EQ(actual, (Vector<Uint8>{0x7F, 0x7F, 0x7F, 0x7F,
                                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0x7F, 0x7F, 0x7F, 0x7F}));
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    MobileGL::MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

TEST_F(BufferTest, ClearBufferSubDataInitializesCompleteIrisStaticSsbo) {
    constexpr SizeT irisStaticSsboSize = 5'000'192;
    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);

    Vector<Uint8> initial(irisStaticSsboSize, 0x7F);
    MobileGL::MG_Impl::GLImpl::BufferData(
        GL_SHADER_STORAGE_BUFFER, initial.size(), initial.data(), GL_STATIC_DRAW);
    const GLbyte zero = 0;
    MobileGL::MG_Impl::GLImpl::ClearBufferSubData(
        GL_SHADER_STORAGE_BUFFER, GL_R8, 0, irisStaticSsboSize, GL_RED, GL_BYTE, &zero);

    Vector<Uint8> actual(irisStaticSsboSize);
    auto bufferObject = MobileGL::MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    Memcpy(actual.data(), bufferObject->AcquireMemory(false, true, false), actual.size());
    EXPECT_EQ(actual, Vector<Uint8>(irisStaticSsboSize, 0));
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    MobileGL::MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

TEST_F(BufferTest, ClearBufferDataConvertsOneClientPixelBeforeRepeatingIt) {
    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_ARRAY_BUFFER, buffer);

    Vector<Uint32> initial(4, 0u);
    MobileGL::MG_Impl::GLImpl::BufferData(GL_ARRAY_BUFFER, initial.size() * sizeof(Uint32), initial.data(),
                                           GL_STATIC_DRAW);
    const Uint8 value = 0xAB;
    MobileGL::MG_Impl::GLImpl::ClearBufferData(
        GL_ARRAY_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, &value);

    Vector<Uint32> actual(initial.size());
    auto bufferObject = MobileGL::MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    Memcpy(actual.data(), bufferObject->AcquireMemory(false, true, false), actual.size() * sizeof(Uint32));
    EXPECT_EQ(actual, Vector<Uint32>(initial.size(), value));
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_ARRAY_BUFFER, 0);
    MobileGL::MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

TEST_F(BufferTest, ClearBufferSubDataRejectsUnboundTarget) {
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    const GLbyte zero = 0;
    MobileGL::MG_Impl::GLImpl::ClearBufferSubData(
        GL_SHADER_STORAGE_BUFFER, GL_R8, 0, 1, GL_RED, GL_BYTE, &zero);
    ExpectSingleGlError(GL_INVALID_OPERATION);
}

TEST_F(BufferTest, ClearBufferDataRejectsInvalidPixelFormatTypePairs) {
    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_ARRAY_BUFFER, buffer);

    const Vector<Uint8> initial{0x7F, 0x7F};
    MobileGL::MG_Impl::GLImpl::BufferData(GL_ARRAY_BUFFER, initial.size(), initial.data(), GL_STATIC_DRAW);
    const Uint16 packed = 0;
    MobileGL::MG_Impl::GLImpl::ClearBufferData(
        GL_ARRAY_BUFFER, GL_R16, GL_RED, GL_UNSIGNED_SHORT_5_6_5, &packed);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MobileGL::MG_Impl::GLImpl::ClearBufferData(
        GL_ARRAY_BUFFER, GL_R16, GL_RED, GL_UNSIGNED_SHORT_5_6_5, nullptr);
    ExpectSingleGlError(GL_INVALID_VALUE);

    Vector<Uint8> actual(initial.size());
    auto bufferObject = MobileGL::MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    Memcpy(actual.data(), bufferObject->AcquireMemory(false, true, false), actual.size());
    EXPECT_EQ(actual, initial);

    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_ARRAY_BUFFER, 0);
    MobileGL::MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}


// GL 4.6 core 6.5: glBufferSubData fails only when the written range OVERLAPS the mapped range.
// A second, wrong test used to sit next to the correct one and reject any write whose end reached
// the start of the mapping - which killed every legal disjoint update in front of a mapped tail.
TEST_F(BufferTest, BufferSubDataRejectsOnlyRangesOverlappingTheMapping) {
    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_ARRAY_BUFFER, buffer);
    MobileGL::MG_Impl::GLImpl::BufferData(GL_ARRAY_BUFFER, 64, nullptr, GL_DYNAMIC_DRAW);
    ASSERT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    void* mapped = MobileGL::MG_Impl::GLImpl::MapBufferRange(GL_ARRAY_BUFFER, 32, 32, GL_MAP_WRITE_BIT);
    ASSERT_NE(mapped, nullptr);
    ASSERT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Entirely before the mapping: legal, and the bytes must land.
    const Uint32 payload[4] = {1u, 2u, 3u, 4u};
    MobileGL::MG_Impl::GLImpl::BufferSubData(GL_ARRAY_BUFFER, 0, sizeof(payload), payload);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Touching the first mapped byte: overlap, so INVALID_OPERATION.
    MobileGL::MG_Impl::GLImpl::BufferSubData(GL_ARRAY_BUFFER, 16, 32, payload);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    EXPECT_TRUE(MobileGL::MG_Impl::GLImpl::UnmapBuffer(GL_ARRAY_BUFFER));

    Vector<Uint32> actual(4, 0);
    auto bufferObject = MobileGL::MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    Memcpy(actual.data(), bufferObject->AcquireMemory(false, true, false), sizeof(payload));
    EXPECT_EQ(actual, (Vector<Uint32>{1u, 2u, 3u, 4u}));

    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_ARRAY_BUFFER, 0);
    MobileGL::MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

// GL 4.6 core 6.2: "no buffer bound to target" outranks a bad size or bad flags, so the binding has
// to be resolved before either is validated. It used to be checked last, which turned every
// unbound-target call into INVALID_VALUE.
TEST_F(BufferTest, BufferStorageReportsTheUnboundTargetBeforeSizeAndFlags) {
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_ARRAY_BUFFER, 0);
    DrainPendingGlErrors();

    // Both a zero size and a nonsense flag set are present; the unbound target still wins.
    MobileGL::MG_Impl::GLImpl::BufferStorage(GL_ARRAY_BUFFER, 0, nullptr, GL_MAP_PERSISTENT_BIT);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // With a buffer bound, the size check is reachable again.
    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_ARRAY_BUFFER, buffer);
    MobileGL::MG_Impl::GLImpl::BufferStorage(GL_ARRAY_BUFFER, 0, nullptr, GL_MAP_READ_BIT);
    ExpectSingleGlError(GL_INVALID_VALUE);

    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_ARRAY_BUFFER, 0);
    MobileGL::MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

// GL 4.6 core 6.1.1: glBindBufferRange on GL_SHADER_STORAGE_BUFFER must reject an offset that is
// not a multiple of GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT.
TEST_F(BufferTest, BindBufferRangeRejectsMisalignedShaderStorageOffset) {
    GLint ssboAlignment = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlignment);
    ASSERT_GT(ssboAlignment, 1) << "a 1-byte alignment cannot express a misaligned offset";

    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    MobileGL::MG_Impl::GLImpl::BufferData(GL_SHADER_STORAGE_BUFFER, ssboAlignment * 4, nullptr, GL_DYNAMIC_DRAW);
    ASSERT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, buffer, 1, ssboAlignment);
    ExpectSingleGlError(GL_INVALID_VALUE);
    auto& point = MobileGL::MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, 1);
    EXPECT_EQ(point.GetBoundObject(), nullptr) << "a rejected bind must not take effect";

    // The uniform target has its own alignment and must not inherit the SSBO rule's rejection.
    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, buffer, ssboAlignment, ssboAlignment);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_NE(point.GetBoundObject(), nullptr);

    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, 0, 0, 0);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    MobileGL::MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

// ARB_multi_bind: the [first, first + count) range is checked up front and reports
// INVALID_OPERATION - not the per-element INVALID_VALUE a naive loop over glBindBufferBase would
// produce, and nothing may be bound when it fails.
TEST_F(BufferTest, BindBuffersBaseChecksTheWholeRangeBeforeBindingAnything) {
    GLint maxBindings = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &maxBindings);
    ASSERT_GT(maxBindings, 1);

    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    MobileGL::MG_Impl::GLImpl::BufferData(GL_SHADER_STORAGE_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
    ASSERT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // first is in range but first + count is not: one error, of the multi-bind class.
    const GLuint first = static_cast<GLuint>(maxBindings - 1);
    const GLuint buffers[2] = {buffer, buffer};
    MobileGL::MG_Impl::GLImpl::BindBuffersBase(GL_SHADER_STORAGE_BUFFER, first, 2, buffers);
    ExpectSingleGlError(GL_INVALID_OPERATION);
    auto& point = MobileGL::MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, first);
    EXPECT_EQ(point.GetBoundObject(), nullptr) << "the in-range prefix must not be bound either";

    MobileGL::MG_Impl::GLImpl::BindBuffersRange(GL_SHADER_STORAGE_BUFFER, first, 2, buffers, nullptr, nullptr);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // A range that fits binds normally.
    MobileGL::MG_Impl::GLImpl::BindBuffersBase(GL_SHADER_STORAGE_BUFFER, first, 1, buffers);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    ASSERT_NE(point.GetBoundObject(), nullptr);
    EXPECT_EQ(point.GetBoundObject()->GetExternalIndex(), buffer);

    MobileGL::MG_Impl::GLImpl::BindBufferBase(GL_SHADER_STORAGE_BUFFER, first, 0);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    MobileGL::MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

// These limits were reachable only through glGetInteger64v (SSBO block size) or not at all (the
// atomic-counter pair), so glGetIntegerv answered them with INVALID_ENUM out of its default arm.
TEST_F(BufferTest, GetIntegervAnswersSsboAndAtomicCounterLimits) {
    GLint ssboBlockSize = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &ssboBlockSize);
    EXPECT_GT(ssboBlockSize, 0);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The 32-bit query saturates rather than truncating what glGetInteger64v reports.
    GLint64 ssboBlockSize64 = 0;
    MobileGL::MG_Impl::GLImpl::GetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &ssboBlockSize64);
    EXPECT_EQ(static_cast<GLint64>(ssboBlockSize), std::min<GLint64>(ssboBlockSize64, INT32_MAX));

    GLint atomicBindings = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS, &atomicBindings);
    EXPECT_GE(atomicBindings, 1);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint atomicBufferSize = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE, &atomicBufferSize);
    EXPECT_GE(atomicBufferSize, 32); // GL 4.6 table 23.63 minimum
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // KHR_debug requires these to be legal even while the debug entry points are stubs.
    GLint debugGroupDepth = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_MAX_DEBUG_GROUP_STACK_DEPTH, &debugGroupDepth);
    EXPECT_GE(debugGroupDepth, 64);
    GLint debugLoggedMessages = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_MAX_DEBUG_LOGGED_MESSAGES, &debugLoggedMessages);
    EXPECT_GE(debugLoggedMessages, 1);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// GL 4.6 core 6.1.1: glBindBufferRange validates the (offset, size) pair before it writes any
// state. Nothing validated either one, so a negative offset reached Range1D(offset, offset + size)
// - which has no ordering check of its own - and a zero or negative size installed an empty or
// backwards range on the binding point.
TEST_F(BufferTest, BindBufferRangeRejectsNegativeOffsetAndNonPositiveSize) {
    GLint ssboAlignment = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlignment);
    ASSERT_GT(ssboAlignment, 0);

    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    MobileGL::MG_Impl::GLImpl::BufferData(GL_SHADER_STORAGE_BUFFER, ssboAlignment * 4, nullptr, GL_DYNAMIC_DRAW);
    ASSERT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    auto& point = MobileGL::MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, 2);

    // A negative offset is INVALID_VALUE - including one that is a multiple of the alignment, which
    // the modulo gate alone waves through (-alignment % alignment == 0).
    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, buffer, -ssboAlignment, ssboAlignment);
    ExpectSingleGlError(GL_INVALID_VALUE);
    EXPECT_EQ(point.GetBoundObject(), nullptr) << "a rejected bind must not take effect";

    // size must be strictly positive.
    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, buffer, 0, 0);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, buffer, 0, -4);
    ExpectSingleGlError(GL_INVALID_VALUE);
    EXPECT_EQ(point.GetBoundObject(), nullptr);

    // The well-formed bind still goes through.
    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, buffer, ssboAlignment, ssboAlignment);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    ASSERT_NE(point.GetBoundObject(), nullptr);
    EXPECT_EQ(point.GetRange().start, static_cast<SizeT>(ssboAlignment));
    EXPECT_EQ(point.GetRange().end, static_cast<SizeT>(ssboAlignment * 2));

    // Buffer 0 detaches with offset and size ignored: the one case the size rule must not fire on,
    // and the shape glBindBuffersRange uses to reset an element.
    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, 0, 0, 0);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(point.GetBoundObject(), nullptr);

    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    MobileGL::MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

// GL 4.6 core 6.1.1 gives GL_UNIFORM_BUFFER its own offset alignment
// (GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT) and requires BOTH offset and size to be multiples of 4 on
// GL_TRANSFORM_FEEDBACK_BUFFER. Only the shader-storage half of the rule was implemented, so a
// misaligned uniform range bound happily.
TEST_F(BufferTest, BindBufferRangeEnforcesUniformAndTransformFeedbackAlignment) {
    GLint uboAlignment = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uboAlignment);
    ASSERT_GT(uboAlignment, 1) << "a 1-byte alignment cannot express a misaligned offset";

    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_UNIFORM_BUFFER, buffer);
    MobileGL::MG_Impl::GLImpl::BufferData(GL_UNIFORM_BUFFER, uboAlignment * 4, nullptr, GL_DYNAMIC_DRAW);
    ASSERT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    auto& uniformPoint = MobileGL::MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::Uniform, 1);
    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_UNIFORM_BUFFER, 1, buffer, 1, uboAlignment);
    ExpectSingleGlError(GL_INVALID_VALUE);
    EXPECT_EQ(uniformPoint.GetBoundObject(), nullptr) << "a misaligned uniform range must not bind";

    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_UNIFORM_BUFFER, 1, buffer, uboAlignment, uboAlignment);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_NE(uniformPoint.GetBoundObject(), nullptr);
    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_UNIFORM_BUFFER, 1, 0, 0, 0);
    EXPECT_EQ(uniformPoint.GetBoundObject(), nullptr);

    // Transform feedback captures 32-bit components: offset and size are both constrained, and the
    // size half has no analogue on any other target.
    auto& feedbackPoint =
        MobileGL::MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::TransformFeedback, 0);
    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer, 2, 4);
    ExpectSingleGlError(GL_INVALID_VALUE);
    EXPECT_EQ(feedbackPoint.GetBoundObject(), nullptr);
    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer, 4, 2);
    ExpectSingleGlError(GL_INVALID_VALUE);
    EXPECT_EQ(feedbackPoint.GetBoundObject(), nullptr);

    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer, 4, 4);
    EXPECT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_NE(feedbackPoint.GetBoundObject(), nullptr);

    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0, 0, 0);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_UNIFORM_BUFFER, 0);
    MobileGL::MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

// ARB_multi_bind checks offsets and sizes separately for each binding point: the offending element
// is left unchanged and reports INVALID_VALUE while every other element still binds. Only the
// [first, first + count) range is the up-front, all-or-nothing check - so glBindBuffersRange gets
// the new gates by looping over the single-bind entry point, and must keep going after one fails.
TEST_F(BufferTest, BindBuffersRangeAppliesTheOffsetAndSizeGatesPerElement) {
    GLint ssboAlignment = 0;
    MobileGL::MG_Impl::GLImpl::GetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlignment);
    ASSERT_GT(ssboAlignment, 1) << "a 1-byte alignment cannot express a misaligned offset";

    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    MobileGL::MG_Impl::GLImpl::BufferData(GL_SHADER_STORAGE_BUFFER, ssboAlignment * 8, nullptr, GL_DYNAMIC_DRAW);
    ASSERT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    auto& firstPoint = MobileGL::MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, 0);
    auto& secondPoint = MobileGL::MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, 1);
    const GLuint buffers[2] = {buffer, buffer};

    // Element 0 is misaligned; element 1 is well formed and must still be bound.
    const GLintptr misalignedOffsets[2] = {1, ssboAlignment};
    const GLsizeiptr sizes[2] = {ssboAlignment, ssboAlignment};
    MobileGL::MG_Impl::GLImpl::BindBuffersRange(GL_SHADER_STORAGE_BUFFER, 0, 2, buffers, misalignedOffsets, sizes);
    ExpectSingleGlError(GL_INVALID_VALUE);
    EXPECT_EQ(firstPoint.GetBoundObject(), nullptr) << "the rejected element must not bind";
    ASSERT_NE(secondPoint.GetBoundObject(), nullptr) << "a per-element error must not abort the rest of the range";
    EXPECT_EQ(secondPoint.GetRange().start, static_cast<SizeT>(ssboAlignment));

    MobileGL::MG_Impl::GLImpl::BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    ASSERT_EQ(MobileGL::MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Same for a non-positive size, on the other element this time.
    const GLintptr offsets[2] = {0, ssboAlignment};
    const GLsizeiptr badSizes[2] = {ssboAlignment, 0};
    MobileGL::MG_Impl::GLImpl::BindBuffersRange(GL_SHADER_STORAGE_BUFFER, 0, 2, buffers, offsets, badSizes);
    ExpectSingleGlError(GL_INVALID_VALUE);
    EXPECT_NE(firstPoint.GetBoundObject(), nullptr);
    EXPECT_EQ(secondPoint.GetBoundObject(), nullptr);

    MobileGL::MG_Impl::GLImpl::BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    MobileGL::MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

using namespace MobileGL::MG_Impl::GLImpl;

class GeneralBufferTest : public ::testing::Test {
protected:
    void SetUp() override { MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>(); }

    GLuint CreateBoundBuffer(GLenum target, GLsizeiptr size, GLenum usage) {
        GLuint buffer;
        GenBuffers(1, &buffer);
        BindBuffer(target, buffer);
        BufferData(target, size, nullptr, usage);
        EXPECT_EQ(GetError(), GL_NO_ERROR);
        return buffer;
    }
};

TEST_F(GeneralBufferTest, General_BufferLifecycle) {
    GLuint buffers[3];

    GenBuffers(3, buffers);
    EXPECT_NE(buffers[0], 0);
    EXPECT_NE(buffers[1], 0);
    EXPECT_NE(buffers[2], 0);
    EXPECT_NE(buffers[0], buffers[1]);

    GLuint deleteBuf = buffers[1];
    DeleteBuffers(1, &deleteBuf);

    BindBuffer(GL_ARRAY_BUFFER, deleteBuf);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_BufferDataOperations) {
    GLuint buffer = CreateBoundBuffer(GL_ARRAY_BUFFER, 100, GL_STATIC_DRAW);

    const char initData[100] = {0};
    char readBack[100];
    void* mapped = MapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY);
    ASSERT_NE(mapped, nullptr);
    memcpy(readBack, mapped, 100);
    EXPECT_EQ(memcmp(initData, readBack, 100), 0);
    UnmapBuffer(GL_ARRAY_BUFFER);

    const char subData[] = "TEST";
    BufferSubData(GL_ARRAY_BUFFER, 10, sizeof(subData), subData);
    mapped = MapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY);
    memcpy(readBack, (char*)mapped + 10, sizeof(subData));
    EXPECT_STREQ(readBack, "TEST");
    UnmapBuffer(GL_ARRAY_BUFFER);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_BufferCopy) {
    GLuint src = CreateBoundBuffer(GL_COPY_READ_BUFFER, 50, GL_STATIC_READ);
    GLuint dst = CreateBoundBuffer(GL_COPY_WRITE_BUFFER, 50, GL_STATIC_DRAW);

    const char srcData[] = "SOURCE_BUFFER_DATA";
    BufferSubData(GL_COPY_READ_BUFFER, 0, sizeof(srcData), srcData);

    void* srcMappedData = MapBuffer(GL_COPY_READ_BUFFER, GL_READ_ONLY);
    EXPECT_STREQ((char*)srcMappedData, "SOURCE_BUFFER_DATA");
    UnmapBuffer(GL_COPY_READ_BUFFER);

    CopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 10, sizeof(srcData));

    char result[50];
    void* mapped = MapBuffer(GL_COPY_WRITE_BUFFER, GL_READ_ONLY);
    memcpy(result, (char*)mapped + 10, sizeof(srcData));
    EXPECT_STREQ(result, "SOURCE_BUFFER_DATA");
    UnmapBuffer(GL_COPY_WRITE_BUFFER);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_BufferMapping) {
    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);
    Vector<char> data;
    data.resize(100, '\0');
    BufferData(GL_ARRAY_BUFFER, data.size(), data.data(), GL_DYNAMIC_DRAW);

    void* fullMap = MapBuffer(GL_ARRAY_BUFFER, GL_READ_WRITE);
    ASSERT_NE(fullMap, nullptr);
    strcpy((char*)fullMap, "   Full mapping test");
    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));

    void* partialMap = MapBufferRange(GL_ARRAY_BUFFER, 0, 30, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
    ASSERT_NE(partialMap, nullptr);
    strcpy((char*)partialMap, "Partial (not valid value)");

    FlushMappedBufferRange(GL_ARRAY_BUFFER, 0, 7);
    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));

    partialMap = MapBufferRange(GL_ARRAY_BUFFER, 20, 40, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
    ASSERT_NE(partialMap, nullptr);
    strcpy((char*)partialMap, ": modified data (not valid value)");

    FlushMappedBufferRange(GL_ARRAY_BUFFER, 0, 15);
    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));

    char verify[40];
    void* verifyMap = MapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY);
    memcpy(verify, (char*)verifyMap, 40);
    EXPECT_STREQ(verify, "Partial mapping test: modified data");
    UnmapBuffer(GL_ARRAY_BUFFER);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_InvalidOperations) {
    EXPECT_EQ(MapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY), nullptr); // GL_INVALID_OPERATION
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);

    GLuint buffer = CreateBoundBuffer(GL_ARRAY_BUFFER, 50, GL_STREAM_READ);

    EXPECT_EQ(MapBuffer(0xFFFFFFFF, GL_READ_ONLY), nullptr); // GL_INVALID_ENUM

    void* mapped = MapBuffer(GL_ARRAY_BUFFER, GL_READ_WRITE);
    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));
    EXPECT_FALSE(UnmapBuffer(GL_ARRAY_BUFFER)); // GL_INVALID_OPERATION

    EXPECT_EQ(GetError(), GL_INVALID_ENUM);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_MapFlags) {
    GLuint buffer = CreateBoundBuffer(GL_ARRAY_BUFFER, 100, GL_DYNAMIC_COPY);

    void* roMap = MapBufferRange(GL_ARRAY_BUFFER, 0, 100, GL_MAP_READ_BIT);
    ASSERT_NE(roMap, nullptr);

    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));

    void* nosyncMap = MapBufferRange(GL_ARRAY_BUFFER, 0, 100, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    ASSERT_NE(nosyncMap, nullptr);
    memset(nosyncMap, 0xAA, 100);
    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_BufferStorageQueriesImmutable) {
    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);

    const GLint initial[] = {1, 2, 3, 4};
    constexpr GLbitfield storageFlags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT;
    BufferStorage(GL_ARRAY_BUFFER, sizeof(initial), initial, storageFlags);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    GLint immutable = GL_FALSE;
    GLint reportedFlags = 0;
    GetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_IMMUTABLE_STORAGE, &immutable);
    GetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_STORAGE_FLAGS, &reportedFlags);
    EXPECT_EQ(immutable, GL_TRUE);
    EXPECT_EQ(reportedFlags, static_cast<GLint>(storageFlags));

    const GLint update = 42;
    BufferSubData(GL_ARRAY_BUFFER, sizeof(GLint), sizeof(update), &update);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    BufferData(GL_ARRAY_BUFFER, sizeof(initial), initial, GL_DYNAMIC_DRAW);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);
}

TEST_F(GeneralBufferTest, General_PersistentMapRequiresStorageFlags) {
    GLuint mutableBuffer = CreateBoundBuffer(GL_ARRAY_BUFFER, 64, GL_DYNAMIC_DRAW);
    (void)mutableBuffer;
    void* mapped = MapBufferRange(GL_ARRAY_BUFFER, 0, 16, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
    EXPECT_EQ(mapped, nullptr);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);

    GLuint storageBuffer = 0;
    GenBuffers(1, &storageBuffer);
    BindBuffer(GL_ARRAY_BUFFER, storageBuffer);
    BufferStorage(GL_ARRAY_BUFFER, 64, nullptr, GL_MAP_WRITE_BIT);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    mapped = MapBufferRange(GL_ARRAY_BUFFER, 0, 16, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
    EXPECT_EQ(mapped, nullptr);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);

    GLuint persistentBuffer = 0;
    GenBuffers(1, &persistentBuffer);
    BindBuffer(GL_ARRAY_BUFFER, persistentBuffer);
    BufferStorage(GL_ARRAY_BUFFER, 64, nullptr, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_CLIENT_STORAGE_BIT);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    mapped = MapBufferRange(GL_ARRAY_BUFFER, 0, 16, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
    ASSERT_NE(mapped, nullptr);
    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_PersistentCoherentWriteDirtyWithoutUnmap) {
    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);

    GLint initial[] = {10, 20, 30, 40};
    BufferStorage(GL_ARRAY_BUFFER, sizeof(initial), initial,
                  GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    const Uint64 baseSerial = bufferObject->GetChangeSerial();

    auto* mapped = static_cast<GLint*>(
        MapBufferRange(GL_ARRAY_BUFFER, 0, sizeof(initial),
                       GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT));
    ASSERT_NE(mapped, nullptr);
    mapped[2] = 1234;

    // Draw-time hook: pushes the persistently mapped write range to the backend.
    bufferObject->SyncPersistentMappedRange();
    EXPECT_GT(bufferObject->GetChangeSerial(), baseSerial);

    EXPECT_EQ(reinterpret_cast<const GLint*>(bufferObject->MappedData())[2], 1234);
    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_PersistentExplicitFlushOnlyDirtiesFlushedRange) {
    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);

    GLint initial[] = {10, 20, 30, 40};
    BufferStorage(GL_ARRAY_BUFFER, sizeof(initial), initial, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    const Uint64 baseSerial = bufferObject->GetChangeSerial();

    auto* mapped = static_cast<GLint*>(
        MapBufferRange(GL_ARRAY_BUFFER, 0, sizeof(initial),
                       GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT));
    ASSERT_NE(mapped, nullptr);
    mapped[1] = 200;
    mapped[3] = 400;

    // FlushExplicit persistent maps only reach the backend via explicit flushes.
    bufferObject->SyncPersistentMappedRange();
    EXPECT_EQ(bufferObject->GetChangeSerial(), baseSerial);

    FlushMappedBufferRange(GL_ARRAY_BUFFER, sizeof(GLint), sizeof(GLint));
    EXPECT_GT(bufferObject->GetChangeSerial(), baseSerial);

    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_NamedBufferStorageMappingWrappers) {
    GLuint buffer = 0;
    GenBuffers(1, &buffer);

    GLint initial[] = {1, 2, 3, 4};
    NamedBufferStorage(buffer, sizeof(initial), initial,
                       GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    GLint immutable = GL_FALSE;
    GetNamedBufferParameteriv(buffer, GL_BUFFER_IMMUTABLE_STORAGE, &immutable);
    EXPECT_EQ(immutable, GL_TRUE);

    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    const Uint64 baseSerial = bufferObject->GetChangeSerial();

    auto* mapped = static_cast<GLint*>(
        MapNamedBufferRange(buffer, 0, sizeof(initial),
                            GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT));
    ASSERT_NE(mapped, nullptr);
    mapped[0] = 99;

    void* mapPointer = nullptr;
    GetNamedBufferPointerv(buffer, GL_BUFFER_MAP_POINTER, &mapPointer);
    EXPECT_EQ(mapPointer, mapped);

    FlushMappedNamedBufferRange(buffer, 0, sizeof(GLint));
    EXPECT_GT(bufferObject->GetChangeSerial(), baseSerial);

    EXPECT_TRUE(UnmapNamedBuffer(buffer));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_GeneralTest_1) {
    GLuint buffers[3];
    GenBuffers(3, buffers);
    const GLuint vbo = buffers[0];
    const GLuint ibo = buffers[1];
    const GLuint staging = buffers[2];

    BindBuffer(GL_ARRAY_BUFFER, vbo);
    BufferData(GL_ARRAY_BUFFER, 64, nullptr, GL_STATIC_DRAW);

    const float vertexData[] = {0.1f, 0.2f, 0.3f, 1.0f};
    BufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertexData), vertexData);

    BindBuffer(GL_UNIFORM_BUFFER, ibo);
    const uint16_t indexData[] = {0, 1, 2, 3, 0};
    BufferData(GL_UNIFORM_BUFFER, sizeof(indexData), indexData, GL_STATIC_DRAW);

    const uint16_t newIndices[] = {4, 5};
    BufferSubData(GL_UNIFORM_BUFFER, 2 * sizeof(uint16_t), sizeof(newIndices), newIndices);

    BindBuffer(GL_COPY_READ_BUFFER, staging);
    const char stagingData[] = "StagingBufferData";
    BufferData(GL_COPY_READ_BUFFER, sizeof(stagingData), stagingData, GL_STREAM_COPY);

    CopyBufferSubData(GL_COPY_READ_BUFFER, GL_ARRAY_BUFFER, 0, 40, sizeof(stagingData));

    BindBuffer(GL_UNIFORM_BUFFER, ibo);
    void* fullMap = MapBuffer(GL_UNIFORM_BUFFER, GL_READ_WRITE);
    ASSERT_NE(fullMap, nullptr);

    uint16_t* indices = static_cast<uint16_t*>(fullMap);
    indices[0] = 10;

    EXPECT_TRUE(UnmapBuffer(GL_UNIFORM_BUFFER));

    BindBuffer(GL_ARRAY_BUFFER, vbo);
    void* partialMap = MapBufferRange(GL_ARRAY_BUFFER, 20, 8, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
    ASSERT_NE(partialMap, nullptr);

    const char partialWriteData[] = "PARTIAL"; // 7 chars + '\0' = 8 bytes
    memcpy(partialMap, partialWriteData, sizeof(partialWriteData));
    FlushMappedBufferRange(GL_ARRAY_BUFFER, 0, sizeof(partialWriteData));

    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));

    BindBuffer(GL_ARRAY_BUFFER, vbo);
    void* verifyMap = MapBufferRange(GL_ARRAY_BUFFER, 0, 64, GL_MAP_READ_BIT);
    ASSERT_NE(verifyMap, nullptr);

    const float* verts = static_cast<const float*>(verifyMap);
    EXPECT_FLOAT_EQ(verts[0], 0.1f);
    EXPECT_FLOAT_EQ(verts[1], 0.2f);

    const char* partialData = static_cast<const char*>(verifyMap) + 20;
    EXPECT_STREQ(partialData, "PARTIAL");

    const char* copiedData = static_cast<const char*>(verifyMap) + 40;
    EXPECT_STREQ(copiedData, "StagingBufferData");

    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));

    BindBuffer(GL_UNIFORM_BUFFER, ibo);
    void* iboMap = MapBuffer(GL_UNIFORM_BUFFER, GL_READ_ONLY);
    const uint16_t* finalIndices = static_cast<const uint16_t*>(iboMap);
    EXPECT_EQ(finalIndices[0], 10);
    EXPECT_EQ(finalIndices[2], 4);
    EXPECT_TRUE(UnmapBuffer(GL_UNIFORM_BUFFER));

    DeleteBuffers(1, &staging);

    BindBuffer(GL_COPY_READ_BUFFER, staging);
    GLenum err = GetError();
    EXPECT_EQ(err, GL_INVALID_OPERATION);

    GLuint toDelete[] = {vbo, ibo};
    DeleteBuffers(2, toDelete);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------
// Zero-copy persistent-coherent mapping via the PipeResource layer (regression
// guard for the GpuMemory OOM / rendering-corruption bug). A fake backend hands
// out a block of "GPU" memory from AcquirePersistentMap; the frontend adopts it
// as the buffer's storage. The test asserts (a) the app maps straight onto that
// GPU memory, (b) EVERY reader (MappedData(), the accessor all backend consumers
// now use) resolves to that same GPU memory rather than a stale shadow - the bug
// that corrupted UBO/vertex data - and (c) a long map/write/draw loop drives ZERO
// per-draw backend transfer ops.
namespace {
    struct ZeroCopyMockBackend {
        Vector<Uint8> gpu; // stand-in for host-visible coherent GPU storage
        int acquireMapCalls = 0;
        int subDataCalls = 0;
        int respecifyCalls = 0;
        int flushCalls = 0;
        Bool provideMap = true; // false => backend declines, exercising the shadow fallback
    };

    ZeroCopyMockBackend* g_zeroCopyMock = nullptr;

    void* ZeroCopyMock_AcquirePersistentMap(MG_State::GLState::BufferObject& bufferObject) {
        if (!g_zeroCopyMock || !g_zeroCopyMock->provideMap) return nullptr;
        if (g_zeroCopyMock->gpu.size() != bufferObject.GetSize()) {
            g_zeroCopyMock->gpu.assign(bufferObject.GetSize(), 0);
            // Seed from the shadow (still current: the frontend adopts only after we return).
            const Uint8* shadow = bufferObject.MappedData();
            if (shadow != nullptr && bufferObject.GetSize() > 0) {
                Memcpy(g_zeroCopyMock->gpu.data(), shadow, bufferObject.GetSize());
            }
        }
        ++g_zeroCopyMock->acquireMapCalls;
        return g_zeroCopyMock->gpu.data();
    }

    void ZeroCopyMock_Respecify(MG_State::GLState::BufferObject&) {
        if (g_zeroCopyMock) ++g_zeroCopyMock->respecifyCalls;
    }
    void ZeroCopyMock_SubData(MG_State::GLState::BufferObject&, SizeT, SizeT) {
        if (g_zeroCopyMock) ++g_zeroCopyMock->subDataCalls;
    }
    void ZeroCopyMock_Flush(MG_State::GLState::BufferObject&, Range1D, Flags<BufferMappingAccessBit>) {
        if (g_zeroCopyMock) ++g_zeroCopyMock->flushCalls;
    }
    void ZeroCopyMock_OnDestroy(SharedPtr<MG_State::GLState::BackendBufferResource>&&) {}

    const MG_State::GLState::BufferBackendOps kZeroCopyMockOps = {
        .Respecify = ZeroCopyMock_Respecify,
        .SubData = ZeroCopyMock_SubData,
        .FlushMappedRange = ZeroCopyMock_Flush,
        .OnDestroy = ZeroCopyMock_OnDestroy,
        .AcquirePersistentMap = ZeroCopyMock_AcquirePersistentMap,
    };

    struct ScopedBackendOps {
        explicit ScopedBackendOps(const MG_State::GLState::BufferBackendOps* ops) {
            MG_State::GLState::SetBufferBackendOps(ops);
        }
        ~ScopedBackendOps() { MG_State::GLState::SetBufferBackendOps(nullptr); }
    };
} // namespace

TEST_F(GeneralBufferTest, General_PersistentCoherentZeroCopyStressNoPerDrawReupload) {
    ZeroCopyMockBackend mock;
    g_zeroCopyMock = &mock;
    ScopedBackendOps scopedOps(&kZeroCopyMockOps);

    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);

    constexpr SizeT kCount = 4096; // 16 KiB of GLint - a "large" dynamic ring buffer
    Vector<GLint> initial(kCount, 0);
    BufferStorage(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(kCount * sizeof(GLint)), initial.data(),
                  GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    auto* mapped = static_cast<GLint*>(
        MapBufferRange(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(kCount * sizeof(GLint)),
                       GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT));
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(mock.acquireMapCalls, 1);

    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    EXPECT_TRUE(bufferObject->IsBackendPersistentMapped());
    // The app maps straight onto the backend's GPU storage...
    EXPECT_EQ(static_cast<void*>(mapped), static_cast<void*>(mock.gpu.data()));
    // ...and EVERY consumer (they all read MappedData() now) resolves to that same GPU
    // memory, not a stale shadow. This is the invariant whose violation corrupted UBOs.
    EXPECT_EQ(static_cast<const void*>(bufferObject->MappedData()), static_cast<const void*>(mock.gpu.data()));

    // Isolate the per-draw behavior: storage creation legitimately issued one Respecify.
    mock.subDataCalls = 0;
    mock.flushCalls = 0;
    mock.respecifyCalls = 0;

    constexpr int kFrames = 240;
    constexpr int kDrawsPerFrame = 64; // 15,360 draws total
    for (int frame = 0; frame < kFrames; ++frame) {
        for (int draw = 0; draw < kDrawsPerFrame; ++draw) {
            mapped[draw] = frame * 1000 + draw;    // MC writes through the coherent map
            bufferObject->SyncPersistentMappedRange(); // draw-time hook
        }
    }

    // The crux: across 15,360 draws, NOT ONE per-draw backend transfer.
    EXPECT_EQ(mock.acquireMapCalls, 1);
    EXPECT_EQ(mock.subDataCalls, 0);
    EXPECT_EQ(mock.flushCalls, 0);
    EXPECT_EQ(mock.respecifyCalls, 0);

    // The app's writes are coherently visible in the backend storage (no copy), and a
    // reader going through MappedData() sees them too.
    const auto* gpuInts = reinterpret_cast<const GLint*>(mock.gpu.data());
    const auto* viaMapped = reinterpret_cast<const GLint*>(bufferObject->MappedData());
    for (int draw = 0; draw < kDrawsPerFrame; ++draw) {
        EXPECT_EQ(mapped[draw], (kFrames - 1) * 1000 + draw);
        EXPECT_EQ(gpuInts[draw], (kFrames - 1) * 1000 + draw);
        EXPECT_EQ(viaMapped[draw], (kFrames - 1) * 1000 + draw);
    }

    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));
    EXPECT_EQ(mock.flushCalls, 0);
    EXPECT_EQ(mock.subDataCalls, 0);
    g_zeroCopyMock = nullptr;
}

TEST_F(GeneralBufferTest, General_PersistentCoherentFallbackSyncsPerDrawWhenBackendDeclines) {
    // Backend cannot back the map => the legacy CPU-shadow path must still be correct,
    // and this documents the behavior the fix removed (one whole-range transfer per draw),
    // proving the harness above would catch a regression (non-zero per-draw count).
    ZeroCopyMockBackend mock;
    mock.provideMap = false;
    g_zeroCopyMock = &mock;
    ScopedBackendOps scopedOps(&kZeroCopyMockOps);

    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);

    constexpr SizeT kCount = 256;
    Vector<GLint> initial(kCount, 0);
    BufferStorage(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(kCount * sizeof(GLint)), initial.data(),
                  GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    auto* mapped = static_cast<GLint*>(
        MapBufferRange(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(kCount * sizeof(GLint)),
                       GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT));
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(mock.acquireMapCalls, 0); // backend declined => shadow-backed map

    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    EXPECT_FALSE(bufferObject->IsBackendPersistentMapped());

    constexpr int kDraws = 100;
    for (int draw = 0; draw < kDraws; ++draw) {
        mapped[draw % kCount] = draw;
        bufferObject->SyncPersistentMappedRange();
    }
    // Legacy behavior: every draw pushed the whole range -> one SubData per draw.
    EXPECT_EQ(mock.subDataCalls, kDraws);

    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));
    g_zeroCopyMock = nullptr;
}

// ---------------------------------------------------------------------------
// MOBILEGL_COHERENT_AS_FLUSH: persistent FLUSH_EXPLICIT mapping requests are
// rewritten to coherent semantics, so apps that bind mapped ranges for GPU reads
// without ever calling glFlushMappedBufferRange (e.g. Flywheel's copy descriptors)
// still get their writes, and their flush calls stay error-free no-ops.
// Non-persistent maps keep spec FLUSH_EXPLICIT behavior.
namespace {
    struct ScopedCoherentAsFlush {
        ScopedCoherentAsFlush() { MG_Config::Features.CoherentAsFlush = true; }
        ~ScopedCoherentAsFlush() { MG_Config::Features.CoherentAsFlush = false; }
    };
} // namespace

TEST_F(GeneralBufferTest, General_CoherentAsFlush_NonPersistentMapKeepsExplicitFlushSemantics) {
    ScopedCoherentAsFlush scopedFeature;

    GLuint buffer = CreateBoundBuffer(GL_ARRAY_BUFFER, 64, GL_STATIC_DRAW);
    const char initial[16] = "0123456789ABCDE";
    BufferSubData(GL_ARRAY_BUFFER, 20, sizeof(initial), initial);

    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);

    auto* mapped =
        static_cast<char*>(MapBufferRange(GL_ARRAY_BUFFER, 20, 16, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT));
    ASSERT_NE(mapped, nullptr);
    // Non-persistent maps are not rewritten: the FLUSH_EXPLICIT contract stays.
    EXPECT_TRUE(bufferObject->GetMappingAccess() & BufferMappingAccessBit::FlushExplicit);

    memcpy(mapped, "PARTIAL", 8);
    memcpy(mapped + 8, "WRITTEN", 8);
    FlushMappedBufferRange(GL_ARRAY_BUFFER, 0, 8); // flush only the first half
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));

    // Only the flushed subrange reaches the shadow; the un-flushed half keeps its
    // previous contents (spec behavior, unchanged by the feature).
    char readBack[16] = {};
    GetBufferSubData(GL_ARRAY_BUFFER, 20, sizeof(readBack), readBack);
    EXPECT_EQ(memcmp(readBack, "PARTIAL", 8), 0);
    EXPECT_EQ(memcmp(readBack + 8, "89ABCDE", 8), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_FlushWithoutExplicitBitStillErrorsWhenFeatureOff) {
    GLuint buffer = CreateBoundBuffer(GL_ARRAY_BUFFER, 64, GL_STATIC_DRAW);
    (void)buffer;
    void* mapped = MapBufferRange(GL_ARRAY_BUFFER, 0, 16, GL_MAP_WRITE_BIT);
    ASSERT_NE(mapped, nullptr);
    FlushMappedBufferRange(GL_ARRAY_BUFFER, 0, 8);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);
    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));
}

TEST_F(GeneralBufferTest, General_CoherentAsFlush_PersistentMapSyncsWithoutExplicitFlush) {
    ScopedCoherentAsFlush scopedFeature;

    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);

    GLint initial[] = {10, 20, 30, 40};
    BufferStorage(GL_ARRAY_BUFFER, sizeof(initial), initial, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    const Uint64 baseSerial = bufferObject->GetChangeSerial();

    auto* mapped = static_cast<GLint*>(MapBufferRange(
        GL_ARRAY_BUFFER, 0, sizeof(initial), GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT));
    ASSERT_NE(mapped, nullptr);
    const auto access = bufferObject->GetMappingAccess();
    EXPECT_FALSE(access & BufferMappingAccessBit::FlushExplicit);
    EXPECT_TRUE(access & BufferMappingAccessBit::Coherent);

    mapped[1] = 200;
    // Un-flushed writes are picked up by the draw-time persistent sync - the coverage
    // the removed FLUSH_EXPLICIT dispatch hack (SyncMappedRangeForGpuRead) used to add.
    bufferObject->SyncPersistentMappedRange();
    EXPECT_GT(bufferObject->GetChangeSerial(), baseSerial);
    EXPECT_EQ(reinterpret_cast<const GLint*>(bufferObject->MappedData())[1], 200);

    FlushMappedBufferRange(GL_ARRAY_BUFFER, 0, sizeof(GLint));
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_CoherentAsFlush_DsaPersistentMapRewritesAndToleratesFlush) {
    ScopedCoherentAsFlush scopedFeature;

    GLuint buffer = 0;
    GenBuffers(1, &buffer);

    GLint initial[] = {1, 2, 3, 4};
    NamedBufferStorage(buffer, sizeof(initial), initial, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    const Uint64 baseSerial = bufferObject->GetChangeSerial();

    // The DSA map entry point applies the same rewrite as the bound-target one.
    auto* mapped = static_cast<GLint*>(MapNamedBufferRange(
        buffer, 0, sizeof(initial), GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT));
    ASSERT_NE(mapped, nullptr);
    const auto access = bufferObject->GetMappingAccess();
    EXPECT_FALSE(access & BufferMappingAccessBit::FlushExplicit);
    EXPECT_TRUE(access & BufferMappingAccessBit::Coherent);

    mapped[2] = 300;
    bufferObject->SyncPersistentMappedRange();
    EXPECT_GT(bufferObject->GetChangeSerial(), baseSerial);
    EXPECT_EQ(reinterpret_cast<const GLint*>(bufferObject->MappedData())[2], 300);

    // The DSA flush entry point tolerates the app's flush as a no-op too.
    FlushMappedNamedBufferRange(buffer, 0, sizeof(GLint));
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    EXPECT_TRUE(UnmapNamedBuffer(buffer));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(GeneralBufferTest, General_CoherentAsFlush_PersistentMapAdoptsZeroCopyBackendStorage) {
    ScopedCoherentAsFlush scopedFeature;
    ZeroCopyMockBackend mock;
    g_zeroCopyMock = &mock;
    ScopedBackendOps scopedOps(&kZeroCopyMockOps);

    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);

    constexpr SizeT kCount = 256;
    Vector<GLint> initial(kCount, 0);
    BufferStorage(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(kCount * sizeof(GLint)), initial.data(),
                  GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    // Flywheel-style map: FLUSH_EXPLICIT and never flushed. Under the feature it becomes
    // coherent-persistent and takes the zero-copy path: the app writes straight into the
    // backend's GPU storage, so nothing depends on flush calls.
    auto* mapped = static_cast<GLint*>(
        MapBufferRange(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(kCount * sizeof(GLint)),
                       GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT));
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(mock.acquireMapCalls, 1);

    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    EXPECT_TRUE(bufferObject->IsBackendPersistentMapped());
    EXPECT_EQ(static_cast<void*>(mapped), static_cast<void*>(mock.gpu.data()));

    mock.subDataCalls = 0;
    mock.flushCalls = 0;

    mapped[7] = 1234;
    bufferObject->SyncPersistentMappedRange(); // draw-time hook: nothing to transfer
    EXPECT_EQ(reinterpret_cast<const GLint*>(mock.gpu.data())[7], 1234);
    EXPECT_EQ(mock.subDataCalls, 0);
    EXPECT_EQ(mock.flushCalls, 0);

    EXPECT_TRUE(UnmapBuffer(GL_ARRAY_BUFFER));
    EXPECT_EQ(mock.flushCalls, 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    g_zeroCopyMock = nullptr;
}

// A buffer whose bytes the backend adopted into its own GPU memory - which is what
// EnsureGpuResidentStorage does for a transform-feedback capture target or a shader
// storage binding, so that MapBuffer/GetBufferSubData read real GPU results - and which
// the application then REDEFINES.
//
// The store the adopted mapping describes is the one being thrown away. Keeping that
// mapping across the redefinition is what let a transform feedback capture be written to
// one buffer and read back out of another: the backend replaced the storage (a
// respecification is the orphaning point) while the frontend went on resolving every read
// through a mapping of the storage it had just released. Two capture spans into one
// re-specified buffer came back empty from the second one onwards.
//
// So the mapping is handed back and the buffer returns to the CPU-shadow model until
// something asks for residency again. These pin all three parts of that: the adoption
// really is dropped, the new contents really do land where later reads resolve, and the
// backend really is told to respecify - it must not skip the storage, or its copy would
// keep the old bytes.
TEST_F(BufferTest, RedefiningAnAdoptedBufferHandsTheMappingBack) {
    ZeroCopyMockBackend mock;
    g_zeroCopyMock = &mock;
    ScopedBackendOps scopedOps(&kZeroCopyMockOps);

    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);
    const GLint before[4] = {1, 2, 3, 4};
    BufferData(GL_ARRAY_BUFFER, sizeof(before), before, GL_DYNAMIC_DRAW);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    // The backend adopts the bytes, exactly as a capture target or an SSBO binding does.
    ASSERT_TRUE(bufferObject->EnsureGpuResidentStorage());
    ASSERT_TRUE(bufferObject->IsBackendPersistentMapped());
    ASSERT_EQ(static_cast<const void*>(bufferObject->MappedData()),
              static_cast<const void*>(mock.gpu.data()));
    mock.respecifyCalls = 0;

    const GLint after[4] = {10, 20, 30, 40};
    BufferData(GL_ARRAY_BUFFER, sizeof(after), after, GL_DYNAMIC_DRAW);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    EXPECT_FALSE(bufferObject->IsBackendPersistentMapped());
    EXPECT_NE(static_cast<const void*>(bufferObject->MappedData()),
              static_cast<const void*>(mock.gpu.data()));
    EXPECT_EQ(std::memcmp(bufferObject->MappedData(), after, sizeof(after)), 0);
    // The backend has a separate copy again, so it must have been told to refresh it.
    EXPECT_EQ(mock.respecifyCalls, 1);

    g_zeroCopyMock = nullptr;
}

// The same redefinition at a LARGER size, which is the case nothing could paper over: the
// adopted mapping is exactly as big as the old store, so writing the new contents through
// it ran past the end of the backend allocation.
TEST_F(BufferTest, RedefiningAnAdoptedBufferAtANewSizeStaysInBounds) {
    ZeroCopyMockBackend mock;
    g_zeroCopyMock = &mock;
    ScopedBackendOps scopedOps(&kZeroCopyMockOps);

    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);
    const GLint small[2] = {1, 2};
    BufferData(GL_ARRAY_BUFFER, sizeof(small), small, GL_DYNAMIC_DRAW);
    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    ASSERT_TRUE(bufferObject->EnsureGpuResidentStorage());
    ASSERT_EQ(mock.gpu.size(), sizeof(small));

    const GLint large[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    BufferData(GL_ARRAY_BUFFER, sizeof(large), large, GL_DYNAMIC_DRAW);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    EXPECT_EQ(bufferObject->GetSize(), sizeof(large));
    EXPECT_FALSE(bufferObject->IsBackendPersistentMapped());
    EXPECT_EQ(std::memcmp(bufferObject->MappedData(), large, sizeof(large)), 0);
    // The old, smaller GPU block was not written through: still the old size, still the
    // old bytes.
    EXPECT_EQ(mock.gpu.size(), sizeof(small));
    EXPECT_EQ(std::memcmp(mock.gpu.data(), small, sizeof(small)), 0);

    // And residency can be taken again, now over the new store.
    ASSERT_TRUE(bufferObject->EnsureGpuResidentStorage());
    EXPECT_TRUE(bufferObject->IsBackendPersistentMapped());
    EXPECT_EQ(mock.gpu.size(), sizeof(large));
    EXPECT_EQ(std::memcmp(bufferObject->MappedData(), large, sizeof(large)), 0);

    g_zeroCopyMock = nullptr;
}

// glBufferStorage is the other way into a redefinition, and an adopted buffer can reach
// it: the adoption came from a binding rather than from an application map, so the buffer
// is still mutable and glBufferStorage is still legal on it.
TEST_F(BufferTest, ImmutableStorageOnAnAdoptedBufferHandsTheMappingBackToo) {
    ZeroCopyMockBackend mock;
    g_zeroCopyMock = &mock;
    ScopedBackendOps scopedOps(&kZeroCopyMockOps);

    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);
    const GLint before[4] = {1, 2, 3, 4};
    BufferData(GL_ARRAY_BUFFER, sizeof(before), before, GL_DYNAMIC_DRAW);
    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    ASSERT_TRUE(bufferObject->EnsureGpuResidentStorage());
    ASSERT_TRUE(bufferObject->IsBackendPersistentMapped());

    const GLint after[6] = {9, 8, 7, 6, 5, 4};
    BufferStorage(GL_ARRAY_BUFFER, sizeof(after), after, GL_MAP_READ_BIT);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    EXPECT_TRUE(bufferObject->IsImmutableStorage());
    EXPECT_FALSE(bufferObject->IsBackendPersistentMapped());
    EXPECT_EQ(bufferObject->GetSize(), sizeof(after));
    EXPECT_EQ(std::memcmp(bufferObject->MappedData(), after, sizeof(after)), 0);

    g_zeroCopyMock = nullptr;
}

// A redefinition to nothing. The backend declines residency for an empty store, so this
// is also the path where the mapping is given back and never retaken.
TEST_F(BufferTest, RedefiningAnAdoptedBufferToZeroBytesLeavesItOnTheShadow) {
    ZeroCopyMockBackend mock;
    g_zeroCopyMock = &mock;
    ScopedBackendOps scopedOps(&kZeroCopyMockOps);

    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);
    const GLint before[4] = {1, 2, 3, 4};
    BufferData(GL_ARRAY_BUFFER, sizeof(before), before, GL_DYNAMIC_DRAW);
    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    ASSERT_TRUE(bufferObject->EnsureGpuResidentStorage());
    ASSERT_TRUE(bufferObject->IsBackendPersistentMapped());

    BufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    EXPECT_EQ(bufferObject->GetSize(), 0u);
    EXPECT_FALSE(bufferObject->IsBackendPersistentMapped());
    EXPECT_FALSE(bufferObject->EnsureGpuResidentStorage()); // nothing to make resident

    // ...and it comes back to life on the next non-empty store.
    const GLint again[3] = {5, 6, 7};
    BufferData(GL_ARRAY_BUFFER, sizeof(again), again, GL_DYNAMIC_DRAW);
    ASSERT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_TRUE(bufferObject->EnsureGpuResidentStorage());
    EXPECT_EQ(std::memcmp(bufferObject->MappedData(), again, sizeof(again)), 0);

    g_zeroCopyMock = nullptr;
}

// The negative control for the four above: a backend that DECLINES to hand out a mapping
// leaves the buffer shadow-backed throughout, so a redefinition is just a redefinition -
// no adoption to give back, and the backend still gets its Respecify.
TEST_F(BufferTest, RedefiningANonAdoptedBufferIsUnchanged) {
    ZeroCopyMockBackend mock;
    mock.provideMap = false;
    g_zeroCopyMock = &mock;
    ScopedBackendOps scopedOps(&kZeroCopyMockOps);

    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);
    const GLint before[4] = {1, 2, 3, 4};
    BufferData(GL_ARRAY_BUFFER, sizeof(before), before, GL_DYNAMIC_DRAW);
    auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(bufferObject, nullptr);
    EXPECT_FALSE(bufferObject->EnsureGpuResidentStorage());
    EXPECT_FALSE(bufferObject->IsBackendPersistentMapped());
    mock.respecifyCalls = 0;

    const GLint after[4] = {10, 20, 30, 40};
    BufferData(GL_ARRAY_BUFFER, sizeof(after), after, GL_DYNAMIC_DRAW);
    ASSERT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_FALSE(bufferObject->IsBackendPersistentMapped());
    EXPECT_EQ(std::memcmp(bufferObject->MappedData(), after, sizeof(after)), 0);
    EXPECT_EQ(mock.respecifyCalls, 1);

    g_zeroCopyMock = nullptr;
}
