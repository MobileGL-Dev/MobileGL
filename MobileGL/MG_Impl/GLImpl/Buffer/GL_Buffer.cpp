// MobileGL - MobileGL/MG_Impl/GLImpl/Buffer/GL_Buffer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Buffer.h"
#include "Validators.h"
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToMG/BufferEnumConverter.h>
#include <MG_Util/Converters/MGToGL/BufferEnumConverter.h>

namespace MobileGL::MG_Impl::GLImpl {
    namespace {
        enum class BufferOp {
            GetBufferParameteriv,
            GetBufferParameteri64v,
            GetBufferPointerv,
            BufferStorage,
            CreateBuffers,
            NamedBufferStorage,
            NamedBufferData,
            NamedBufferSubData,
            CopyNamedBufferSubData,
            ClearNamedBufferData,
            ClearNamedBufferSubData,
            MapBufferRange,
            MapBuffer,
            MapNamedBuffer,
            MapNamedBufferRange,
            UnmapNamedBuffer,
            FlushMappedNamedBufferRange,
            GetNamedBufferParameteriv,
            GetNamedBufferParameteri64v,
            GetNamedBufferPointerv,
        };

        const char* GetBufferOpName(BufferOp op) {
            switch (op) {
            case BufferOp::GetBufferParameteriv:
                return "GetBufferParameteriv";
            case BufferOp::GetBufferParameteri64v:
                return "GetBufferParameteri64v";
            case BufferOp::GetBufferPointerv:
                return "GetBufferPointerv";
            case BufferOp::BufferStorage:
                return "BufferStorage";
            case BufferOp::CreateBuffers:
                return "CreateBuffers";
            case BufferOp::NamedBufferStorage:
                return "NamedBufferStorage";
            case BufferOp::NamedBufferData:
                return "NamedBufferData";
            case BufferOp::NamedBufferSubData:
                return "NamedBufferSubData";
            case BufferOp::CopyNamedBufferSubData:
                return "CopyNamedBufferSubData";
            case BufferOp::ClearNamedBufferData:
                return "ClearNamedBufferData";
            case BufferOp::ClearNamedBufferSubData:
                return "ClearNamedBufferSubData";
            case BufferOp::MapBufferRange:
                return "MapBufferRange";
            case BufferOp::MapBuffer:
                return "MapBuffer";
            case BufferOp::MapNamedBuffer:
                return "MapNamedBuffer";
            case BufferOp::MapNamedBufferRange:
                return "MapNamedBufferRange";
            case BufferOp::UnmapNamedBuffer:
                return "UnmapNamedBuffer";
            case BufferOp::FlushMappedNamedBufferRange:
                return "FlushMappedNamedBufferRange";
            case BufferOp::GetNamedBufferParameteriv:
                return "GetNamedBufferParameteriv";
            case BufferOp::GetNamedBufferParameteri64v:
                return "GetNamedBufferParameteri64v";
            case BufferOp::GetNamedBufferPointerv:
                return "GetNamedBufferPointerv";
            default:
                return "Buffer";
            }
        }

        SharedPtr<MG_State::GLState::BufferObject> GetNamedBufferObject(GLuint buffer, BufferOp op);

        SizeT GetClearPatternSize(GLenum internalformat, GLenum format, GLenum type, BufferOp op) {
            if (format != GL_RED_INTEGER) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 "Only GL_RED_INTEGER buffer clears are currently supported."));
                return 0;
            }

            if (internalformat == GL_R8UI && type == GL_UNSIGNED_BYTE) return sizeof(GLubyte);
            if (internalformat == GL_R32UI && type == GL_UNSIGNED_INT) return sizeof(GLuint);

            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                             std::format("Unsupported clear format tuple: internalformat=0x{:X}, "
                                                         "format=0x{:X}, type=0x{:X}",
                                                         internalformat, format, type)));
            return 0;
        }

        Bool ValidateBufferClearRange(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject, GLintptr offset,
                                      GLsizeiptr size, SizeT patternSize, BufferOp op) {
            if (offset < 0 || size < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 "Offset and size must be non-negative."));
                return false;
            }

            if (patternSize == 0 || (static_cast<SizeT>(offset) % patternSize) != 0 ||
                (static_cast<SizeT>(size) % patternSize) != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 "Offset and size must be aligned to the clear element size."));
                return false;
            }

            if (static_cast<SizeT>(offset) + static_cast<SizeT>(size) > bufferObject->GetSize()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 "Offset and size exceed buffer size."));
                return false;
            }

            if (bufferObject->IsMapped() && !(bufferObject->GetMappingAccess() & BufferMappingAccessBit::Persistent)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 "Cannot clear a non-persistently mapped buffer object."));
                return false;
            }

            return true;
        }

        void ClearNamedBufferRange_State(GLuint buffer, GLenum internalformat, GLintptr offset, GLsizeiptr size,
                                         GLenum format, GLenum type, const void* data, BufferOp op) {
            const SizeT patternSize = GetClearPatternSize(internalformat, format, type, op);
            if (patternSize == 0) return;

            auto bufferObject = GetNamedBufferObject(buffer, op);
            if (!bufferObject) return;
            if (!ValidateBufferClearRange(bufferObject, offset, size, patternSize, op)) return;
            if (size == 0) return;

            Vector<Uint8> clearData(static_cast<SizeT>(size));
            if (data) {
                const auto* pattern = static_cast<const Uint8*>(data);
                for (SizeT at = 0; at < clearData.size(); at += patternSize) {
                    Memcpy(clearData.data() + at, pattern, patternSize);
                }
            } else {
                Memset(clearData.data(), 0, clearData.size());
            }

            bufferObject->UploadSubData({clearData.data(), clearData.size()}, static_cast<SizeT>(offset));
        }

        auto& GetBufferBindingSlot(BufferTarget target) {
            if (target == BufferTarget::Index) {
                return MG_State::pGLContext->GetBoundVertexArray()->GetIndexBufferBindingSlot();
            }
            return MG_State::pGLContext->GetBufferBindingSlot(target);
        }

        SharedPtr<MG_State::GLState::BufferObject> GetBoundBufferObject(GLenum target, BufferOp op) {
            BufferTarget bufferTarget = MG_Util::ConvertGLEnumToBufferTarget(target);
            if (!BufferImpl::ValidateBufferTarget(bufferTarget)) return nullptr;

            auto& bindingSlot = GetBufferBindingSlot(bufferTarget);
            auto& bufferObject = bindingSlot.GetBoundObject();
            if (!bufferObject) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 "Buffer target is bound to no buffer object."));
                return nullptr;
            }
            return bufferObject;
        }

        SharedPtr<MG_State::GLState::BufferObject> GetNamedBufferObject(GLuint buffer, BufferOp op) {
            if (!BufferImpl::ValidateBufferName(buffer, false)) return nullptr;
            if (!MG_State::pGLContext->ValidateBufferObject(buffer)) {
                MG_State::pGLContext->CreateBufferObject(buffer);
            }
            auto& bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
            if (!bufferObject) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 std::format("Buffer object {} does not exist.", buffer)));
            }
            return bufferObject;
        }

        Bool ValidateStorageFlags(GLbitfield flags, BufferOp op) {
            constexpr GLbitfield validFlags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT |
                                              GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT | GL_CLIENT_STORAGE_BIT;
            if ((flags & ~validFlags) != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 std::format("Invalid buffer storage flags: 0x{:X}", flags)));
                return false;
            }

            if ((flags & GL_MAP_PERSISTENT_BIT) && !(flags & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT))) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 "GL_MAP_PERSISTENT_BIT requires GL_MAP_READ_BIT or GL_MAP_WRITE_BIT."));
                return false;
            }

            if ((flags & GL_MAP_COHERENT_BIT) && !(flags & GL_MAP_PERSISTENT_BIT)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 "GL_MAP_COHERENT_BIT requires GL_MAP_PERSISTENT_BIT."));
                return false;
            }
            return true;
        }

        Bool ValidateImmutableMapAccess(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject,
                                        Flags<BufferMappingAccessBit> accessBits, BufferOp op) {
            if (!bufferObject->IsImmutableStorage()) {
                if (accessBits & (BufferMappingAccessBit::Persistent | BufferMappingAccessBit::Coherent)) {
                    MG_State::pGLContext->RecordError(
                        ErrorCode::InvalidOperation,
                        MakeUnique<GenericErrorInfo>(
                            "MG_Impl/GLImpl", GetBufferOpName(op),
                            "Persistent or coherent mapping requires immutable buffer storage."));
                    return false;
                }
                return true;
            }

            const GLbitfield storageFlags = bufferObject->GetStorageFlags();
            if ((accessBits & BufferMappingAccessBit::Read) && !(storageFlags & GL_MAP_READ_BIT)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 "GL_MAP_READ_BIT is not allowed by buffer storage flags."));
                return false;
            }
            if ((accessBits & BufferMappingAccessBit::Write) && !(storageFlags & GL_MAP_WRITE_BIT)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 "GL_MAP_WRITE_BIT is not allowed by buffer storage flags."));
                return false;
            }
            if ((accessBits & BufferMappingAccessBit::Persistent) && !(storageFlags & GL_MAP_PERSISTENT_BIT)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 "GL_MAP_PERSISTENT_BIT is not allowed by buffer storage flags."));
                return false;
            }
            if ((accessBits & BufferMappingAccessBit::Coherent) && !(storageFlags & GL_MAP_COHERENT_BIT)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 "GL_MAP_COHERENT_BIT is not allowed by buffer storage flags."));
                return false;
            }
            return true;
        }

        void GetBufferParameteriv_Object(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject, GLenum pname,
                                         GLint* params, BufferOp op) {
            if (!params) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                                          "Params pointer cannot be null."));
                return;
            }

            switch (pname) {
            case GL_BUFFER_SIZE:
                *params = static_cast<GLint>(bufferObject->GetSize());
                break;
            case GL_BUFFER_USAGE:
                *params = (GLint)MG_Util::ConvertBufferUsageToGLEnum(bufferObject->GetUsage());
                break;
            case GL_BUFFER_ACCESS:
                if (bufferObject->IsMapped()) {
                    auto access = bufferObject->GetMappingAccess();
                    if (access & BufferMappingAccessBit::Read && access & BufferMappingAccessBit::Write) {
                        *params = GL_READ_WRITE;
                    } else if (access & BufferMappingAccessBit::Read) {
                        *params = GL_READ_ONLY;
                    } else if (access & BufferMappingAccessBit::Write) {
                        *params = GL_WRITE_ONLY;
                    } else {
                        *params = 0;
                    }
                } else {
                    *params = 0;
                }
                break;
            case GL_BUFFER_MAPPED:
                *params = bufferObject->IsMapped() ? GL_TRUE : GL_FALSE;
                break;
            case GL_BUFFER_IMMUTABLE_STORAGE:
                *params = bufferObject->IsImmutableStorage() ? GL_TRUE : GL_FALSE;
                break;
            case GL_BUFFER_STORAGE_FLAGS:
                *params = static_cast<GLint>(bufferObject->GetStorageFlags());
                break;
            case GL_BUFFER_MAP_OFFSET:
                *params = static_cast<GLint>(bufferObject->GetMappedRange().start);
                break;
            case GL_BUFFER_MAP_LENGTH:
                *params = static_cast<GLint>(bufferObject->GetMappedRange().end - bufferObject->GetMappedRange().start);
                break;
            default:
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                                         std::format("Invalid pname enum: 0x{:X}", pname)));
                break;
            }
        }

        void GetBufferParameteri64v_Object(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject, GLenum pname,
                                           GLint64* params, BufferOp op) {
            if (!params) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                                          "Params pointer cannot be null."));
                return;
            }

            switch (pname) {
            case GL_BUFFER_SIZE:
                *params = static_cast<GLint64>(bufferObject->GetSize());
                break;
            case GL_BUFFER_MAP_OFFSET:
                *params = static_cast<GLint64>(bufferObject->GetMappedRange().start);
                break;
            case GL_BUFFER_MAP_LENGTH:
                *params = static_cast<GLint64>(bufferObject->GetMappedRange().end - bufferObject->GetMappedRange().start);
                break;
            default: {
                GLint value = 0;
                GetBufferParameteriv_Object(bufferObject, pname, &value, op);
                *params = static_cast<GLint64>(value);
                break;
            }
            }
        }

        void GetBufferPointerv_Object(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject, GLenum pname,
                                      void** params, BufferOp op) {
            if (!params) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                                          "Params pointer cannot be null."));
                return;
            }
            if (pname != GL_BUFFER_MAP_POINTER) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                                         std::format("Invalid pname enum: 0x{:X}", pname)));
                return;
            }
            *params = bufferObject->GetMappedPointer();
        }
    } // namespace

    void GetBufferParameteriv_State(GLenum target, GLenum pname, GLint* params) {
        auto bufferObject = GetBoundBufferObject(target, BufferOp::GetBufferParameteriv);
        if (!bufferObject) return;
        GetBufferParameteriv_Object(bufferObject, pname, params, BufferOp::GetBufferParameteriv);
    }

    void GetBufferParameteri64v_State(GLenum target, GLenum pname, GLint64* params) {
        auto bufferObject = GetBoundBufferObject(target, BufferOp::GetBufferParameteri64v);
        if (!bufferObject) return;
        GetBufferParameteri64v_Object(bufferObject, pname, params, BufferOp::GetBufferParameteri64v);
    }

    void GetBufferPointerv_State(GLenum target, GLenum pname, void** params) {
        auto bufferObject = GetBoundBufferObject(target, BufferOp::GetBufferPointerv);
        if (!bufferObject) return;
        GetBufferPointerv_Object(bufferObject, pname, params, BufferOp::GetBufferPointerv);
    }

    void DeleteBuffers_State(GLsizei n, const GLuint* buffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteBuffers_State", "n must be non-negative."));
            return;
        }

        if (!buffers) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteBuffers_State",
                                                                           "Buffer names array cannot be null."));
            return;
        }

        for (SizeT i = 0; i < static_cast<SizeT>(n); ++i) {
            Uint bufferName = buffers[i];
            if (bufferName == 0) continue;
            if (!BufferImpl::ValidateBufferName(bufferName, true)) continue;
            MG_State::pGLContext->MarkBufferObjectForDeletion(bufferName);
        }
    }

    void FlushMappedBufferRange_State(GLenum target, GLintptr offset, GLsizeiptr length) {
        if (length < 0 || offset < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FlushMappedBufferRange_State",
                                                                      "Offset and length must be non-negative."));
            return;
        }

        BufferTarget bufferTarget = MG_Util::ConvertGLEnumToBufferTarget(target);
        if (!BufferImpl::ValidateBufferTarget(bufferTarget)) return;

        auto& bindingSlot = GetBufferBindingSlot(bufferTarget);

        auto& bufferObject = bindingSlot.GetBoundObject();
        if (!bufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FlushMappedBufferRange_State",
                                             "Buffer target is bound to no buffer object."));
            return;
        }

        if (!bufferObject->IsMapped()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FlushMappedBufferRange_State",
                                             "Cannot flush a buffer object that is not mapped."));
            return;
        }

        const auto mappedRange = bufferObject->GetMappedRange();
        if (static_cast<SizeT>(offset) + static_cast<SizeT>(length) > mappedRange.end - mappedRange.start) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FlushMappedBufferRange_State",
                                                                      "Offset and length exceed mapped range."));
            return;
        }

        auto mappingAccess = bufferObject->GetMappingAccess();
        if (!(mappingAccess & BufferMappingAccessBit::FlushExplicit)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "FlushMappedBufferRange_State",
                    "Cannot flush a buffer object that is not mapped with GL_MAP_FLUSH_EXPLICIT_BIT."));
            return;
        }

        bufferObject->FlushMemoryRange(static_cast<SizeT>(offset), static_cast<SizeT>(length));
    }

    GLboolean UnmapBuffer_State(GLenum target) {
        BufferTarget bufferTarget = MG_Util::ConvertGLEnumToBufferTarget(target);
        if (!BufferImpl::ValidateBufferTarget(bufferTarget)) return GL_FALSE;

        auto& bindingSlot = GetBufferBindingSlot(bufferTarget);

        auto& bufferObject = bindingSlot.GetBoundObject();
        if (!bufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "UnmapBuffer_State",
                                             "Buffer target is bound to no buffer object."));
            return GL_FALSE;
        }

        if (!bufferObject->IsMapped()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "UnmapBuffer_State",
                                             "Cannot unmap a buffer object that is not mapped."));
            return GL_FALSE;
        }

        bufferObject->ReleaseMemory();
        return GL_TRUE;
    }

    void* MapBufferRange_State(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
        if (length < 0 || offset < 0) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBufferRange_State",
                                                                           "Offset and length must be non-negative."));
            return nullptr;
        }

        if (length == 0) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBufferRange_State",
                                                                           "Length must be greater than zero."));
            return nullptr;
        }

        BufferTarget bufferTarget = MG_Util::ConvertGLEnumToBufferTarget(target);
        if (!BufferImpl::ValidateBufferTarget(bufferTarget)) return nullptr;

        auto& bindingSlot = GetBufferBindingSlot(bufferTarget);
        auto& bufferObject = bindingSlot.GetBoundObject();
        if (!bufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBufferRange_State",
                                             "Buffer target is bound to no buffer object."));
            return nullptr;
        }

        if (offset + length > bufferObject->GetSize()) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBufferRange_State",
                                                                           "Offset and length exceed buffer size."));
            return nullptr;
        }

        auto accessBits = MG_Util::ConvertGLEnumToBufferMappingAccess(access);
        if (!BufferImpl::ValidateBufferMappingAccess(accessBits)) return nullptr;

        if (!(accessBits & (BufferMappingAccessBit::Read | BufferMappingAccessBit::Write))) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBufferRange_State",
                                             "At least one of GL_MAP_READ_BIT or GL_MAP_WRITE_BIT must be set."));
            return nullptr;
        }

        if (accessBits & BufferMappingAccessBit::Read) {
            const auto invalidFlags = BufferMappingAccessBit::InvalidateRange |
                                      BufferMappingAccessBit::InvalidateBuffer | BufferMappingAccessBit::Unsynchronized;

            if (accessBits & invalidFlags) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", "MapBufferRange_State",
                        "GL_MAP_READ_BIT cannot be combined with invalidation or unsynchronized flags."));
                return nullptr;
            }
        }

        if (accessBits & BufferMappingAccessBit::FlushExplicit) {
            if (!(accessBits & BufferMappingAccessBit::Write)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBufferRange_State",
                                                 "GL_MAP_FLUSH_EXPLICIT_BIT requires GL_MAP_WRITE_BIT."));
                return nullptr;
            }
        }

        if ((accessBits & BufferMappingAccessBit::Persistent) && !(accessBits & (BufferMappingAccessBit::Read | BufferMappingAccessBit::Write))) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBufferRange_State",
                                             "GL_MAP_PERSISTENT_BIT requires GL_MAP_READ_BIT or GL_MAP_WRITE_BIT."));
            return nullptr;
        }

        if ((accessBits & BufferMappingAccessBit::Coherent) && !(accessBits & BufferMappingAccessBit::Persistent)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBufferRange_State",
                                             "GL_MAP_COHERENT_BIT requires GL_MAP_PERSISTENT_BIT."));
            return nullptr;
        }

        if (!ValidateImmutableMapAccess(bufferObject, accessBits, BufferOp::MapBufferRange)) return nullptr;

        if (bufferObject->IsMapped()) {
            const auto invalidateFlags =
                BufferMappingAccessBit::InvalidateRange | BufferMappingAccessBit::InvalidateBuffer;

            if (!(accessBits & invalidateFlags)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBufferRange_State",
                                                 "Cannot map a buffer object that is already mapped."));
                return nullptr;
            }
        }

        void* result = bufferObject->AcquireMemoryRange(
            {static_cast<SizeT>(offset), static_cast<SizeT>(offset + length)}, accessBits);
        if (!result) {
            MG_State::pGLContext->RecordError(
                ErrorCode::OutOfMemory,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBufferRange_State",
                                             "Failed to map buffer due to insufficient memory."));
            return nullptr;
        }
        return result;
    }

    void* MapBuffer_State(GLenum target, GLenum access) {
        Bool readable = access == GL_READ_ONLY || access == GL_READ_WRITE;
        Bool writable = access == GL_WRITE_ONLY || access == GL_READ_WRITE;
        if (access != GL_READ_ONLY && access != GL_WRITE_ONLY && access != GL_READ_WRITE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBuffer_State",
                                             "Access must be one of GL_READ_ONLY, GL_WRITE_ONLY, or GL_READ_WRITE."));
            return nullptr;
        }

        BufferTarget bufferTarget = MG_Util::ConvertGLEnumToBufferTarget(target);
        if (!BufferImpl::ValidateBufferTarget(bufferTarget)) return nullptr;
        auto& bindingSlot = GetBufferBindingSlot(bufferTarget);

        auto& bufferObject = bindingSlot.GetBoundObject();
        if (!bufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBuffer_State",
                                             "Buffer target is bound to no buffer object."));
            return nullptr;
        }

        if (bufferObject->IsMapped()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBuffer_State",
                                             "Cannot map a buffer object that is already mapped."));
            return nullptr;
        }

        Flags<BufferMappingAccessBit> accessBits = BufferMappingAccessBit::Null;
        if (readable) accessBits |= BufferMappingAccessBit::Read;
        if (writable) accessBits |= BufferMappingAccessBit::Write;
        if (!ValidateImmutableMapAccess(bufferObject, accessBits, BufferOp::MapBuffer)) return nullptr;

        void* result = bufferObject->AcquireMemory(true, readable, writable);
        if (!result) {
            MG_State::pGLContext->RecordError(
                ErrorCode::OutOfMemory,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapBuffer_State",
                                             "Failed to map buffer due to insufficient memory."));
            return nullptr;
        }
        return result;
    }

    void CopyBufferSubData_State(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset,
                                 GLsizeiptr size) {
        if (size < 0 || readOffset < 0 || writeOffset < 0) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyBufferSubData_State",
                                                                           "Offset and size must be non-negative."));
            return;
        }

        BufferTarget readBufferTarget = MG_Util::ConvertGLEnumToBufferTarget(readTarget);
        BufferTarget writeBufferTarget = MG_Util::ConvertGLEnumToBufferTarget(writeTarget);
        if (!BufferImpl::ValidateBufferTarget(readBufferTarget) || !BufferImpl::ValidateBufferTarget(writeBufferTarget))
            return;

        auto& readBindingSlot = GetBufferBindingSlot(readBufferTarget);
        auto& writeBindingSlot = GetBufferBindingSlot(writeBufferTarget);
        auto& readBufferObject = readBindingSlot.GetBoundObject();
        auto& writeBufferObject = writeBindingSlot.GetBoundObject();

        if (!readBufferObject || !writeBufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyBufferSubData_State",
                                             "One of the buffer targets is bound to no buffer object."));
            return;
        }

        if (readOffset + size > readBufferObject->GetSize() || writeOffset + size > writeBufferObject->GetSize()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyBufferSubData_State",
                                             "Offset and size must be within the bounds of the buffer objects."));
            return;
        }

        if (readBufferObject == writeBufferObject) {
            if ((readOffset <= writeOffset && readOffset + size > writeOffset) ||
                (writeOffset <= readOffset && writeOffset + size > readOffset)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyBufferSubData_State",
                                                 "Source and destination buffers overlap in the specified ranges."));
                return;
            }
        }

        auto isIllegallyMapped = [](const SharedPtr<MG_State::GLState::BufferObject>& buffer) {
            return buffer->IsMapped() && !(buffer->GetMappingAccess() & BufferMappingAccessBit::Persistent);
        };
        if (isIllegallyMapped(readBufferObject) || isIllegallyMapped(writeBufferObject)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyBufferSubData_State",
                                             "Cannot copy data from/to a mapped buffer object unless it was mapped "
                                             "with GL_MAP_PERSISTENT_BIT."));
            return;
        }

        writeBufferObject->CopyDataFrom(readBufferObject, readOffset, writeOffset, size);
    }

    void BufferSubData_State(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
        MGLOG_D("%s: target = %s, offset = %d, size = %d, data = %p", __func__,
                MG_Util::ConvertGLEnumToString(target).c_str(), offset, size, data);
        if (!data) {
            MG_State::pGLContext->RecordError(
                ErrorCode::NoError, // somehow OpenGL does not generate an error for this
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BufferSubData_State", "Data pointer cannot be null."));
            return;
        }

        if (size < 0 || offset < 0) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BufferSubData_State",
                                                                           "Offset and size must be non-negative."));
            return;
        }

        BufferTarget bufferTarget = MG_Util::ConvertGLEnumToBufferTarget(target);
        if (!BufferImpl::ValidateBufferTarget(bufferTarget)) return;
        auto& bindingSlot = GetBufferBindingSlot(bufferTarget);

        auto& bufferObject = bindingSlot.GetBoundObject();
        if (!bufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BufferSubData_State",
                                             "Buffer target is bound to no buffer object."));
            return;
        }

        if (bufferObject->IsImmutableStorage() && !(bufferObject->GetStorageFlags() & GL_DYNAMIC_STORAGE_BIT)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BufferSubData_State",
                                             "Immutable buffer storage was not created with GL_DYNAMIC_STORAGE_BIT."));
            return;
        }

        SizeT bufferSize = bufferObject->GetSize();
        if (static_cast<SizeT>(offset) + static_cast<SizeT>(size) > bufferSize) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BufferSubData_State",
                                             "Offset and size exceed buffer size."));
            return;
        }

        Range1D mappedRange = bufferObject->GetMappedRange();
        auto mappingAccess = bufferObject->GetMappingAccess();
        if (bufferObject->IsMapped() && !(mappingAccess & BufferMappingAccessBit::Persistent) &&
            (offset < mappedRange.end) && (offset + size > mappedRange.start)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "BufferSubData_State",
                    "Cannot modify a non-persistently mapped buffer object."));
            return;
        }

        if (bufferObject->IsMapped() && !(mappingAccess & BufferMappingAccessBit::Persistent)) {
            Range1D mappedRange = bufferObject->GetMappedRange();
            if (offset + size >= mappedRange.start) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BufferSubData_State",
                                                 "Cannot modify a mapped buffer object unless it was "
                                                 "mapped with GL_MAP_PERSISTENT_BIT."));
                return;
            }
        }

        bufferObject->UploadSubData({(void*)data, (SizeT)size}, offset);
    }

    void BufferData_State(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
        MGLOG_D("%s: %s, size = %d, data = %p, usage = %s", __func__, MG_Util::ConvertGLEnumToString(target).c_str(),
                size, data, MG_Util::ConvertGLEnumToString(usage).c_str());
        if (size < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BufferData_State", "Size must be non-negative."));
            return;
        }

        BufferUsage bufferUsage = MG_Util::ConvertGLEnumToBufferUsage(usage);
        if (!BufferImpl::ValidateBufferUsage(bufferUsage)) return;

        BufferTarget bufferTarget = MG_Util::ConvertGLEnumToBufferTarget(target);
        if (!BufferImpl::ValidateBufferTarget(bufferTarget)) return;
        auto& bindingSlot = GetBufferBindingSlot(bufferTarget);

        auto& bufferObject = bindingSlot.GetBoundObject();
        if (!bufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BufferData_State",
                                             "Buffer target is bound to no buffer object."));
            return;
        }

        if (bufferObject->IsImmutableStorage()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BufferData_State",
                                             "Cannot call glBufferData on immutable buffer storage."));
            return;
        }

        bufferObject->SetUsage(bufferUsage);
        bufferObject->Resize(size);
        if (data) {
            bufferObject->UploadData({(void*)data, (SizeT)size}, 0);
        }
    }

    void BufferStorage_State(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
        if (size <= 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BufferStorage_State", "Size must be positive."));
            return;
        }
        if (!ValidateStorageFlags(flags, BufferOp::BufferStorage)) return;

        auto bufferObject = GetBoundBufferObject(target, BufferOp::BufferStorage);
        if (!bufferObject) return;
        if (bufferObject->IsImmutableStorage()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BufferStorage_State",
                                             "Buffer already has immutable storage."));
            return;
        }
        bufferObject->AllocateImmutableStorage(static_cast<SizeT>(size), data, flags);
    }

    void CreateBuffers_State(GLsizei n, GLuint* buffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CreateBuffers_State", "Count must be non-negative."));
            return;
        }
        if (n > 0 && !buffers) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CreateBuffers_State",
                                             "Buffer output pointer cannot be null."));
            return;
        }

        Vector<Uint> bufferNames;
        MG_State::pGLContext->GenBufferNames(static_cast<SizeT>(n), bufferNames);
        for (GLsizei i = 0; i < n; ++i) {
            buffers[i] = bufferNames[i];
            MG_State::pGLContext->CreateBufferObject(bufferNames[i]);
        }
    }

    void NamedBufferStorage_State(GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags) {
        if (size <= 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "NamedBufferStorage_State", "Size must be positive."));
            return;
        }
        if (!ValidateStorageFlags(flags, BufferOp::NamedBufferStorage)) return;

        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::NamedBufferStorage);
        if (!bufferObject) return;
        if (bufferObject->IsImmutableStorage()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "NamedBufferStorage_State",
                                             "Buffer already has immutable storage."));
            return;
        }
        bufferObject->AllocateImmutableStorage(static_cast<SizeT>(size), data, flags);
    }

    void NamedBufferData_State(GLuint buffer, GLsizeiptr size, const void* data, GLenum usage) {
        if (size < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "NamedBufferData_State", "Size must be non-negative."));
            return;
        }

        BufferUsage bufferUsage = MG_Util::ConvertGLEnumToBufferUsage(usage);
        if (!BufferImpl::ValidateBufferUsage(bufferUsage)) return;

        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::NamedBufferData);
        if (!bufferObject) return;

        if (bufferObject->IsImmutableStorage()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "NamedBufferData_State",
                                             "Cannot call glNamedBufferData on immutable buffer storage."));
            return;
        }

        bufferObject->SetUsage(bufferUsage);
        bufferObject->Resize(size);
        if (data) {
            bufferObject->UploadData({(void*)data, (SizeT)size}, 0);
        }
    }

    void NamedBufferSubData_State(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data) {
        if (!data) {
            MG_State::pGLContext->RecordError(
                ErrorCode::NoError,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "NamedBufferSubData_State",
                                             "Data pointer cannot be null."));
            return;
        }
        if (size < 0 || offset < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "NamedBufferSubData_State",
                                             "Offset and size must be non-negative."));
            return;
        }

        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::NamedBufferSubData);
        if (!bufferObject) return;

        if (bufferObject->IsImmutableStorage() && !(bufferObject->GetStorageFlags() & GL_DYNAMIC_STORAGE_BIT)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "NamedBufferSubData_State",
                                             "Immutable buffer storage was not created with GL_DYNAMIC_STORAGE_BIT."));
            return;
        }
        if (static_cast<SizeT>(offset) + static_cast<SizeT>(size) > bufferObject->GetSize()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "NamedBufferSubData_State",
                                             "Offset and size exceed buffer size."));
            return;
        }
        const auto mappingAccess = bufferObject->GetMappingAccess();
        const auto mappedRange = bufferObject->GetMappedRange();
        if (bufferObject->IsMapped() && !(mappingAccess & BufferMappingAccessBit::Persistent) &&
            (offset < mappedRange.end) && (offset + size > mappedRange.start)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "NamedBufferSubData_State",
                                             "Cannot modify a non-persistently mapped buffer object."));
            return;
        }

        bufferObject->UploadSubData({(void*)data, (SizeT)size}, offset);
    }

    void CopyNamedBufferSubData_State(GLuint readBuffer, GLuint writeBuffer, GLintptr readOffset, GLintptr writeOffset,
                                      GLsizeiptr size) {
        if (size < 0 || readOffset < 0 || writeOffset < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyNamedBufferSubData_State",
                                             "Offset and size must be non-negative."));
            return;
        }

        auto readBufferObject = GetNamedBufferObject(readBuffer, BufferOp::CopyNamedBufferSubData);
        auto writeBufferObject = GetNamedBufferObject(writeBuffer, BufferOp::CopyNamedBufferSubData);
        if (!readBufferObject || !writeBufferObject) return;

        if (static_cast<SizeT>(readOffset) + static_cast<SizeT>(size) > readBufferObject->GetSize() ||
            static_cast<SizeT>(writeOffset) + static_cast<SizeT>(size) > writeBufferObject->GetSize()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyNamedBufferSubData_State",
                                             "Offset and size must be within the bounds of the buffer objects."));
            return;
        }

        if (readBufferObject == writeBufferObject) {
            if ((readOffset <= writeOffset && readOffset + size > writeOffset) ||
                (writeOffset <= readOffset && writeOffset + size > readOffset)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyNamedBufferSubData_State",
                                                 "Source and destination ranges overlap."));
                return;
            }
        }

        auto isIllegallyMapped = [](const SharedPtr<MG_State::GLState::BufferObject>& buffer) {
            return buffer->IsMapped() && !(buffer->GetMappingAccess() & BufferMappingAccessBit::Persistent);
        };
        if (isIllegallyMapped(readBufferObject) || isIllegallyMapped(writeBufferObject)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyNamedBufferSubData_State",
                                             "Cannot copy data from/to a non-persistently mapped buffer object."));
            return;
        }

        writeBufferObject->CopyDataFrom(readBufferObject, static_cast<SizeT>(readOffset),
                                        static_cast<SizeT>(writeOffset), static_cast<SizeT>(size));
    }

    void ClearNamedBufferData_State(GLuint buffer, GLenum internalformat, GLenum format, GLenum type, const void* data) {
        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::ClearNamedBufferData);
        if (!bufferObject) return;
        ClearNamedBufferRange_State(buffer, internalformat, 0, static_cast<GLsizeiptr>(bufferObject->GetSize()), format,
                                    type, data, BufferOp::ClearNamedBufferData);
    }

    void ClearNamedBufferSubData_State(GLuint buffer, GLenum internalformat, GLintptr offset, GLsizeiptr size,
                                       GLenum format, GLenum type, const void* data) {
        ClearNamedBufferRange_State(buffer, internalformat, offset, size, format, type, data,
                                    BufferOp::ClearNamedBufferSubData);
    }

    void* MapNamedBuffer_State(GLuint buffer, GLenum access) {
        Bool readable = access == GL_READ_ONLY || access == GL_READ_WRITE;
        Bool writable = access == GL_WRITE_ONLY || access == GL_READ_WRITE;
        if (access != GL_READ_ONLY && access != GL_WRITE_ONLY && access != GL_READ_WRITE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapNamedBuffer_State",
                                             "Access must be one of GL_READ_ONLY, GL_WRITE_ONLY, or GL_READ_WRITE."));
            return nullptr;
        }

        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::MapNamedBuffer);
        if (!bufferObject) return nullptr;
        if (bufferObject->IsMapped()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapNamedBuffer_State",
                                             "Cannot map a buffer object that is already mapped."));
            return nullptr;
        }

        Flags<BufferMappingAccessBit> accessBits = BufferMappingAccessBit::Null;
        if (readable) accessBits |= BufferMappingAccessBit::Read;
        if (writable) accessBits |= BufferMappingAccessBit::Write;
        if (!ValidateImmutableMapAccess(bufferObject, accessBits, BufferOp::MapNamedBuffer)) return nullptr;

        return bufferObject->AcquireMemory(true, readable, writable);
    }

    void* MapNamedBufferRange_State(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access) {
        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::MapNamedBufferRange);
        if (!bufferObject) return nullptr;

        if (length < 0 || offset < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapNamedBufferRange_State",
                                             "Offset and length must be non-negative."));
            return nullptr;
        }
        if (length == 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapNamedBufferRange_State",
                                             "Length must be greater than zero."));
            return nullptr;
        }
        if (static_cast<SizeT>(offset) + static_cast<SizeT>(length) > bufferObject->GetSize()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapNamedBufferRange_State",
                                             "Offset and length exceed buffer size."));
            return nullptr;
        }

        auto accessBits = MG_Util::ConvertGLEnumToBufferMappingAccess(access);
        if (!BufferImpl::ValidateBufferMappingAccess(accessBits)) return nullptr;
        if (!(accessBits & (BufferMappingAccessBit::Read | BufferMappingAccessBit::Write))) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapNamedBufferRange_State",
                                             "At least one of GL_MAP_READ_BIT or GL_MAP_WRITE_BIT must be set."));
            return nullptr;
        }
        if (accessBits & BufferMappingAccessBit::Read) {
            const auto invalidFlags = BufferMappingAccessBit::InvalidateRange |
                                      BufferMappingAccessBit::InvalidateBuffer | BufferMappingAccessBit::Unsynchronized;
            if (accessBits & invalidFlags) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapNamedBufferRange_State",
                                                 "GL_MAP_READ_BIT cannot be combined with invalidation or unsynchronized flags."));
                return nullptr;
            }
        }
        if ((accessBits & BufferMappingAccessBit::FlushExplicit) && !(accessBits & BufferMappingAccessBit::Write)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapNamedBufferRange_State",
                                             "GL_MAP_FLUSH_EXPLICIT_BIT requires GL_MAP_WRITE_BIT."));
            return nullptr;
        }
        if ((accessBits & BufferMappingAccessBit::Persistent) && !(accessBits & (BufferMappingAccessBit::Read | BufferMappingAccessBit::Write))) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapNamedBufferRange_State",
                                             "GL_MAP_PERSISTENT_BIT requires GL_MAP_READ_BIT or GL_MAP_WRITE_BIT."));
            return nullptr;
        }
        if ((accessBits & BufferMappingAccessBit::Coherent) && !(accessBits & BufferMappingAccessBit::Persistent)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapNamedBufferRange_State",
                                             "GL_MAP_COHERENT_BIT requires GL_MAP_PERSISTENT_BIT."));
            return nullptr;
        }
        if (!ValidateImmutableMapAccess(bufferObject, accessBits, BufferOp::MapNamedBufferRange)) return nullptr;

        if (bufferObject->IsMapped()) {
            const auto invalidateFlags =
                BufferMappingAccessBit::InvalidateRange | BufferMappingAccessBit::InvalidateBuffer;
            if (!(accessBits & invalidateFlags)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "MapNamedBufferRange_State",
                                                 "Cannot map a buffer object that is already mapped."));
                return nullptr;
            }
        }

        return bufferObject->AcquireMemoryRange({static_cast<SizeT>(offset), static_cast<SizeT>(offset + length)},
                                                accessBits);
    }

    GLboolean UnmapNamedBuffer_State(GLuint buffer) {
        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::UnmapNamedBuffer);
        if (!bufferObject) return GL_FALSE;
        if (!bufferObject->IsMapped()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "UnmapNamedBuffer_State",
                                             "Cannot unmap a buffer object that is not mapped."));
            return GL_FALSE;
        }
        bufferObject->ReleaseMemory();
        return GL_TRUE;
    }

    void FlushMappedNamedBufferRange_State(GLuint buffer, GLintptr offset, GLsizeiptr length) {
        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::FlushMappedNamedBufferRange);
        if (!bufferObject) return;
        if (length < 0 || offset < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FlushMappedNamedBufferRange_State",
                                             "Offset and length must be non-negative."));
            return;
        }
        if (!bufferObject->IsMapped()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FlushMappedNamedBufferRange_State",
                                             "Cannot flush a buffer object that is not mapped."));
            return;
        }
        const auto mappedRange = bufferObject->GetMappedRange();
        if (static_cast<SizeT>(offset) + static_cast<SizeT>(length) > mappedRange.end - mappedRange.start) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FlushMappedNamedBufferRange_State",
                                             "Offset and length exceed mapped range."));
            return;
        }
        if (!(bufferObject->GetMappingAccess() & BufferMappingAccessBit::FlushExplicit)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FlushMappedNamedBufferRange_State",
                                             "Cannot flush a buffer object that is not mapped with GL_MAP_FLUSH_EXPLICIT_BIT."));
            return;
        }
        bufferObject->FlushMemoryRange(static_cast<SizeT>(offset), static_cast<SizeT>(length));
    }

    void GetNamedBufferParameteriv_State(GLuint buffer, GLenum pname, GLint* params) {
        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::GetNamedBufferParameteriv);
        if (!bufferObject) return;
        GetBufferParameteriv_Object(bufferObject, pname, params, BufferOp::GetNamedBufferParameteriv);
    }

    void GetNamedBufferParameteri64v_State(GLuint buffer, GLenum pname, GLint64* params) {
        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::GetNamedBufferParameteri64v);
        if (!bufferObject) return;
        GetBufferParameteri64v_Object(bufferObject, pname, params, BufferOp::GetNamedBufferParameteri64v);
    }

    void GetNamedBufferPointerv_State(GLuint buffer, GLenum pname, void** params) {
        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::GetNamedBufferPointerv);
        if (!bufferObject) return;
        GetBufferPointerv_Object(bufferObject, pname, params, BufferOp::GetNamedBufferPointerv);
    }

    void BindBuffer_State(GLenum target, GLuint buffer) {
        if (!BufferImpl::ValidateBufferName(buffer, true)) return;
        BufferTarget bufferTarget = MG_Util::ConvertGLEnumToBufferTarget(target);
        if (!BufferImpl::ValidateBufferTarget(bufferTarget)) return;

        SharedPtr<MG_State::GLState::BufferObject> bufferObject;
        if (buffer != 0) {
            Bool doesBufferObjectCreated = MG_State::pGLContext->ValidateBufferObject(buffer);
            if (!doesBufferObjectCreated) {
                MG_State::pGLContext->CreateBufferObject(buffer);
            }
            bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
        }

        auto& bindingSlot = GetBufferBindingSlot(bufferTarget);
        bindingSlot.Bind(bufferObject);
        MGLOG_D("%s: bind buffer object %d -> %s", __func__, bufferObject ? bufferObject->GetExternalIndex() : 0,
                MG_Util::ConvertGLEnumToString(target).c_str());
    }

    void GenBuffers_State(GLsizei n, GLuint* buffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GenBuffers_State", "n must be non-negative"));
            return;
        }
        Vector<GLuint> bufferNames;
        MG_State::pGLContext->GenBufferNames(n, bufferNames);
        Memcpy(buffers, bufferNames.data(), n * sizeof(GLuint));
    }

    GLboolean IsBuffer_State(GLuint buffer) {
        if (buffer == 0) return GL_FALSE;
        return MG_State::pGLContext->ValidateBufferObject(buffer) ? GL_TRUE : GL_FALSE;
    }

    void BindBufferBase_State(GLenum target, GLuint pointIndex, GLuint buffer) {
        MGLOG_D("%s: target = %s, pointIndex = %u, buffer = %u", __func__,
                MG_Util::ConvertGLEnumToString(target).c_str(), pointIndex, buffer);
        BufferTarget bufferTarget = MG_Util::ConvertGLEnumToBufferTarget(target);
        if (!BufferImpl::ValidateBufferBindingPointTarget(bufferTarget)) return;

        auto& point = MG_State::pGLContext->GetBufferBindingPoint(bufferTarget, pointIndex);
        SharedPtr<MG_State::GLState::BufferObject> bufferObject;
        if (buffer == 0) {
            point.Bind(nullptr);
            point.SetRange(Range1D(0, 0));
            return;
        }

        if (!BufferImpl::ValidateBufferName(buffer, true)) return;

        Bool doesBufferObjectCreated = MG_State::pGLContext->ValidateBufferObject(buffer);
        if (!doesBufferObjectCreated) {
            MG_State::pGLContext->CreateBufferObject(buffer);
        }
        bufferObject = MG_State::pGLContext->GetBufferObject(buffer);

        point.Bind(bufferObject);
        if (bufferObject) {
            point.SetRange(Range1D(0, bufferObject->GetSize()));
            MGLOG_D("%s: set range (0, %d)", __func__, bufferObject->GetSize());
        } else {
            point.ClearRange();
        }
    }

    void BindBufferRange_State(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
        MGLOG_D("%s: target = %s, index = %u, buffer = %u, offset = %d, size = %d", __func__,
                MG_Util::ConvertGLEnumToString(target).c_str(), index, buffer, offset, size);
        BufferTarget bufferTarget = MG_Util::ConvertGLEnumToBufferTarget(target);
        if (!BufferImpl::ValidateBufferBindingPointTarget(bufferTarget)) return;

        auto& point = MG_State::pGLContext->GetBufferBindingPoint(bufferTarget, index);
        SharedPtr<MG_State::GLState::BufferObject> bufferObject;
        if (buffer == 0) {
            point.Bind(nullptr);
            point.SetRange(Range1D(0, 0));
            return;
        }

        if (!BufferImpl::ValidateBufferName(buffer, true)) return;

        Bool doesBufferObjectCreated = MG_State::pGLContext->ValidateBufferObject(buffer);
        if (!doesBufferObjectCreated) {
            MG_State::pGLContext->CreateBufferObject(buffer);
        }
        bufferObject = MG_State::pGLContext->GetBufferObject(buffer);

        point.Bind(bufferObject);
        if (bufferObject) {
            point.SetRange(Range1D(offset, offset + size));
        } else {
            point.ClearRange();
        }
    }

    /* @INSERTION_POINT:FUNCTION_IMPLEMENTATION@ */
    void GetBufferParameteriv(GLenum target, GLenum pname, GLint* params) {
        GetBufferParameteriv_State(target, pname, params);
    }

    void GetBufferPointerv(GLenum target, GLenum pname, void** params) {
        GetBufferPointerv_State(target, pname, params);
    }
    void GetBufferParameteri64v(GLenum target, GLenum pname, GLint64* params) {
        GetBufferParameteri64v_State(target, pname, params);
    }
    GLboolean IsBuffer(GLuint buffer) {
        return IsBuffer_State(buffer);
    }

    void DeleteBuffers(GLsizei n, const GLuint* buffers) {
        DeleteBuffers_State(n, buffers);
    }

    void FlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
        FlushMappedBufferRange_State(target, offset, length);
    }

    GLboolean UnmapBuffer(GLenum target) {
        return UnmapBuffer_State(target);
    }

    void* MapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
        return MapBufferRange_State(target, offset, length, access);
    }

    void* MapBuffer(GLenum target, GLenum access) {
        return MapBuffer_State(target, access);
    }

    void BufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
        BufferStorage_State(target, size, data, flags);
    }

    void NamedBufferStorage(GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags) {
        NamedBufferStorage_State(buffer, size, data, flags);
    }

    void CreateBuffers(GLsizei n, GLuint* buffers) {
        CreateBuffers_State(n, buffers);
    }

    void NamedBufferData(GLuint buffer, GLsizeiptr size, const void* data, GLenum usage) {
        NamedBufferData_State(buffer, size, data, usage);
    }

    void NamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data) {
        NamedBufferSubData_State(buffer, offset, size, data);
    }

    void CopyNamedBufferSubData(GLuint readBuffer, GLuint writeBuffer, GLintptr readOffset, GLintptr writeOffset,
                                GLsizeiptr size) {
        CopyNamedBufferSubData_State(readBuffer, writeBuffer, readOffset, writeOffset, size);
    }

    void ClearNamedBufferData(GLuint buffer, GLenum internalformat, GLenum format, GLenum type, const void* data) {
        ClearNamedBufferData_State(buffer, internalformat, format, type, data);
    }

    void ClearNamedBufferSubData(GLuint buffer, GLenum internalformat, GLintptr offset, GLsizeiptr size, GLenum format,
                                 GLenum type, const void* data) {
        ClearNamedBufferSubData_State(buffer, internalformat, offset, size, format, type, data);
    }

    void* MapNamedBuffer(GLuint buffer, GLenum access) {
        return MapNamedBuffer_State(buffer, access);
    }

    void* MapNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access) {
        return MapNamedBufferRange_State(buffer, offset, length, access);
    }

    GLboolean UnmapNamedBuffer(GLuint buffer) {
        return UnmapNamedBuffer_State(buffer);
    }

    void FlushMappedNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length) {
        FlushMappedNamedBufferRange_State(buffer, offset, length);
    }

    void GetNamedBufferParameteriv(GLuint buffer, GLenum pname, GLint* params) {
        GetNamedBufferParameteriv_State(buffer, pname, params);
    }

    void GetNamedBufferParameteri64v(GLuint buffer, GLenum pname, GLint64* params) {
        GetNamedBufferParameteri64v_State(buffer, pname, params);
    }

    void GetNamedBufferPointerv(GLuint buffer, GLenum pname, void** params) {
        GetNamedBufferPointerv_State(buffer, pname, params);
    }

    // FIXME: this should be a "backend" function
    void CopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset,
                           GLsizeiptr size) {
        CopyBufferSubData_State(readTarget, writeTarget, readOffset, writeOffset, size);
    }

    void BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
        BufferSubData_State(target, offset, size, data);
    }

    void BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
        BufferData_State(target, size, data, usage);
    }

    void BindBuffer(GLenum target, GLuint buffer) {
        BindBuffer_State(target, buffer);
    }

    void GenBuffers(GLsizei n, GLuint* buffers) {
        GenBuffers_State(n, buffers);
    }

    void BindBufferBase(GLenum target, GLuint index, GLuint buffer) {
        BindBufferBase_State(target, index, buffer);
    }

    void BindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
        BindBufferRange_State(target, index, buffer, offset, size);
    }
} // namespace MobileGL::MG_Impl::GLImpl
