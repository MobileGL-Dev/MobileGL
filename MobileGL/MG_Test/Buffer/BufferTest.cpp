// MobileGL - MobileGL/MG_Test/Buffer/BufferTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"
#include <MG_State/GLState/Core.h>

#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>

using namespace MobileGL;

class BufferTest : public ::testing::Test {
protected:
    void SetUp() override { MobileGL::Initialize(); }

    void TearDown() override {}
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
    GLuint buffer = 0;
    MobileGL::MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MobileGL::MG_Impl::GLImpl::BindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    MobileGL::MG_Impl::GLImpl::BufferData(GL_SHADER_STORAGE_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);

    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 3, buffer, 4, 8);
    auto& point = MobileGL::MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, 3);
    ASSERT_NE(point.GetBoundObject(), nullptr);
    EXPECT_EQ(point.GetRange().start, 4);
    EXPECT_EQ(point.GetRange().end, 12);

    MobileGL::MG_Impl::GLImpl::BindBufferRange(GL_SHADER_STORAGE_BUFFER, 3, 0, 0, 0);
    EXPECT_EQ(point.GetBoundObject(), nullptr);
    EXPECT_FALSE(MobileGL::MG_State::pGLContext->ValidateBufferObject(0));
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

    const auto data = bufferObject->GetDataReadOnly();
    EXPECT_EQ(reinterpret_cast<const GLint*>(data->data())[2], 1234);
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
