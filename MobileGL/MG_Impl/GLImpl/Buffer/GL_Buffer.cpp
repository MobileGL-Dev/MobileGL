// MobileGL - MobileGL/MG_Impl/GLImpl/Buffer/GL_Buffer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Buffer.h"
#include "Validators.h"
#include "../Texture/GL_Texture.h"
#include "../Getter/GL_Getter.h"
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Metrics/TextureMetrics.h>
#include <Config.h>
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
            GetNamedBufferSubData,
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
            case BufferOp::GetNamedBufferSubData:
                return "GetNamedBufferSubData";
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

        // The size of one cleared element, which is what offset and size must be multiples of
        // (GL 4.6 core 6.3). `internalformat` is restricted to the buffer-texture format table, and
        // `format`/`type` describe the client-side pattern, so both are validated here and the
        // caller only has to know how wide an element is.
        SizeT GetClearPatternSize(GLenum internalformat, GLenum format, GLenum type, BufferOp op) {
            if (!IsBufferTextureInternalFormat(internalformat)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", GetBufferOpName(op),
                        std::format("internalformat 0x{:X} is not one of the sized formats a buffer clear accepts.",
                                    internalformat)));
                return 0;
            }

            // Unlike internalformat, a bad format or type here is INVALID_VALUE rather than
            // INVALID_ENUM (GL 4.6 core 6.3) - the odd one out among the enum arguments.
            const TextureInputFormat inputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
            if (inputFormat == TextureInputFormat::Unknown) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 std::format("format 0x{:X} is not a pixel format.", format)));
                return 0;
            }

            const TexturePixelDataType pixelType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
            if (pixelType == TexturePixelDataType::Unknown) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 std::format("type 0x{:X} is not a pixel type.", type)));
                return 0;
            }

            const TextureInternalFormat internal =
                MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
            const SizeT elementSize = MG_Util::GetSizedInternalFormatSizeInBytes(internal);
            if (elementSize == 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", GetBufferOpName(op),
                                                 std::format("internalformat 0x{:X} has no known element size.",
                                                             internalformat)));
                return 0;
            }

            // The pattern is replicated verbatim, which is only the whole story while the client
            // layout already matches the internal format - the case every entry point in practice
            // uses, and the only one the conversion machinery here can express. Say so rather than
            // quietly writing a differently-sized pattern.
            const SizeT sourceSize = MG_Util::GetInputBytesPerPixel(inputFormat, pixelType);
            if (sourceSize != elementSize) {
                MGLOG_W("%s: clear pattern is %zu bytes but internalformat 0x%X stores %zu; "
                        "converting between them is not implemented",
                        GetBufferOpName(op), sourceSize, internalformat, elementSize);
            }
            return elementSize;
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

        // MOBILEGL_COHERENT_AS_FLUSH: rewrite a validated persistent FLUSH_EXPLICIT mapping
        // request to coherent semantics: the map becomes coherent-persistent (eligible for
        // the zero-copy backend map, otherwise synced wholesale at draw time), so its writes
        // reach the GPU without glFlushMappedBufferRange; flush calls on rewritten maps are
        // tolerated as no-ops by the FlushMappedBufferRange entry points. Non-persistent
        // maps keep spec FLUSH_EXPLICIT behavior on purpose: the GPU cannot read them while
        // mapped, so they gain nothing from the rewrite, and honoring only the app's flushed
        // subranges avoids clobbering GPU-written bytes elsewhere in the mapped range.
        // Runs after validation so the app's original access combination is what gets
        // validated (Coherent is injected without requiring GL_MAP_COHERENT_BIT storage).
        Flags<BufferMappingAccessBit> ApplyCoherentAsFlush(Flags<BufferMappingAccessBit> accessBits) {
            if (!MG_Config::Features.CoherentAsFlush) return accessBits;
            if (!(accessBits & BufferMappingAccessBit::FlushExplicit)) return accessBits;
            if (!(accessBits & BufferMappingAccessBit::Persistent)) return accessBits;
            accessBits = Flags<BufferMappingAccessBit>(
                accessBits.GetRaw() & ~static_cast<Uint>(BufferMappingAccessBit::FlushExplicit));
            accessBits |= BufferMappingAccessBit::Coherent;
            return accessBits;
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
                        *params = GL_READ_WRITE;
                    }
                } else {
                    // Initial value, and what glUnmapBuffer restores (GL 4.6 core table 6.2).
                    *params = GL_READ_WRITE;
                }
                break;
            case GL_BUFFER_ACCESS_FLAGS:
                // The MapBufferRange flags verbatim; glMapBuffer's access enum has already been
                // normalised into the same bits. Zero while the buffer is not mapped.
                *params = bufferObject->IsMapped()
                    ? static_cast<GLint>(
                          MG_Util::ConvertBufferMappingAccessToGLEnum(bufferObject->GetMappingAccess()))
                    : 0;
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
            // GL 3.3 core 2.9: names that do not correspond to an existing buffer are silently
            // ignored here, so probe with the non-recording query - the shared validator would
            // record INVALID_OPERATION, which is only correct on the bind path.
            if (!MG_State::pGLContext->ValidateBufferName(bufferName)) continue;
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
            // MOBILEGL_COHERENT_AS_FLUSH strips FLUSH_EXPLICIT from persistent maps at map
            // time (leaving Persistent|Coherent), so honor the app's flush on such a map as
            // a no-op: its writes reach the backend without explicit flushes. Other maps
            // keep the spec error.
            if (MG_Config::Features.CoherentAsFlush &&
                (mappingAccess & BufferMappingAccessBit::Persistent) &&
                (mappingAccess & BufferMappingAccessBit::Coherent)) {
                return;
            }
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
            {static_cast<SizeT>(offset), static_cast<SizeT>(offset + length)}, ApplyCoherentAsFlush(accessBits));
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
        // GL 4.6 6.5: the error is on OVERLAP with the mapped range, i.e. a half-open
        // intersection test. There used to be a second test below this one asking only
        // `offset + size >= mappedRange.start`, which rejects every write that starts
        // before a mapped tail as well - it made a legal disjoint glBufferSubData fail.
        if (bufferObject->IsMapped() && !(mappingAccess & BufferMappingAccessBit::Persistent) &&
            (offset < mappedRange.end) && (offset + size > mappedRange.start)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "BufferSubData_State",
                    "Cannot modify a non-persistently mapped buffer object."));
            return;
        }

        bufferObject->UploadSubData({(void*)data, (SizeT)size}, offset);
    }

    void GetBufferSubData_State(GLenum target, GLintptr offset, GLsizeiptr size, void* data) {
        MGLOG_D("%s: target = %s, offset = %d, size = %d, data = %p", __func__,
                MG_Util::ConvertGLEnumToString(target).c_str(), offset, size, data);
        if (!data) {
            // Match BufferSubData_State: a null pointer is a caller bug, not a GL-specified error.
            return;
        }

        if (size < 0 || offset < 0) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetBufferSubData_State",
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
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetBufferSubData_State",
                                             "Buffer target is bound to no buffer object."));
            return;
        }

        SizeT bufferSize = bufferObject->GetSize();
        if (static_cast<SizeT>(offset) + static_cast<SizeT>(size) > bufferSize) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetBufferSubData_State",
                                             "Offset and size exceed buffer size."));
            return;
        }

        if (bufferObject->IsMapped() &&
            !(bufferObject->GetMappingAccess() & BufferMappingAccessBit::Persistent)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetBufferSubData_State",
                                             "Cannot read from a buffer object mapped without GL_MAP_PERSISTENT_BIT."));
            return;
        }

        bufferObject->SyncGpuWrites();
        bufferObject->DownloadSubData(data, static_cast<SizeT>(offset), static_cast<SizeT>(size));
    }

    void GetNamedBufferSubData_State(GLuint buffer, GLintptr offset, GLsizeiptr size, void* data) {
        if (!data) {
            // Match GetBufferSubData_State: a null pointer is a caller bug, not a GL-specified error.
            return;
        }

        if (size < 0 || offset < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetNamedBufferSubData_State",
                                             "Offset and size must be non-negative."));
            return;
        }

        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::GetNamedBufferSubData);
        if (!bufferObject) return;

        if (static_cast<SizeT>(offset) + static_cast<SizeT>(size) > bufferObject->GetSize()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetNamedBufferSubData_State",
                                             "Offset and size exceed buffer size."));
            return;
        }

        if (bufferObject->IsMapped() &&
            !(bufferObject->GetMappingAccess() & BufferMappingAccessBit::Persistent)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetNamedBufferSubData_State",
                                             "Cannot read from a buffer object mapped without GL_MAP_PERSISTENT_BIT."));
            return;
        }

        bufferObject->SyncGpuWrites();
        bufferObject->DownloadSubData(data, static_cast<SizeT>(offset), static_cast<SizeT>(size));
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
        bufferObject->Respecify(size, data);
    }

    void BufferStorage_State(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
        // Error precedence: "no buffer is bound to target" outranks a bad size or bad
        // flags, so the binding has to be resolved before either is validated.
        auto bufferObject = GetBoundBufferObject(target, BufferOp::BufferStorage);
        if (!bufferObject) return;

        if (size <= 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BufferStorage_State", "Size must be positive."));
            return;
        }
        if (!ValidateStorageFlags(flags, BufferOp::BufferStorage)) return;

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
        // Same precedence as BufferStorage_State: the buffer-name error comes first.
        auto bufferObject = GetNamedBufferObject(buffer, BufferOp::NamedBufferStorage);
        if (!bufferObject) return;

        if (size <= 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "NamedBufferStorage_State", "Size must be positive."));
            return;
        }
        if (!ValidateStorageFlags(flags, BufferOp::NamedBufferStorage)) return;

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
        bufferObject->Respecify(size, data);
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
                                                ApplyCoherentAsFlush(accessBits));
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
        const auto namedMappingAccess = bufferObject->GetMappingAccess();
        if (!(namedMappingAccess & BufferMappingAccessBit::FlushExplicit)) {
            // See FlushMappedBufferRange_State: rewritten coherent-as-flush persistent maps
            // tolerate app flushes as no-ops.
            if (MG_Config::Features.CoherentAsFlush &&
                (namedMappingAccess & BufferMappingAccessBit::Persistent) &&
                (namedMappingAccess & BufferMappingAccessBit::Coherent)) {
                return;
            }
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
        if (!BufferImpl::ValidateBufferBindingPointIndex(bufferTarget, pointIndex)) return;
        if (bufferTarget == BufferTarget::TransformFeedback && MG_State::pGLContext->IsTransformFeedbackActive()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Transform feedback buffer bindings cannot change while transform "
                                             "feedback is active."));
            return;
        }
        MG_State::pGLContext->TouchBufferBindingPoint(bufferTarget, pointIndex);

        auto& point = MG_State::pGLContext->GetBufferBindingPoint(bufferTarget, pointIndex);
        SharedPtr<MG_State::GLState::BufferObject> bufferObject;
        if (buffer == 0) {
            point.Bind(nullptr);
            point.SetRange(Range1D(0, 0));
            GetBufferBindingSlot(bufferTarget).Bind(nullptr);
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
            point.SetRange(Range1D(0, bufferObject->GetSize()), false);
            MGLOG_D("%s: set range (0, %d)", __func__, bufferObject->GetSize());
        } else {
            point.ClearRange();
        }
        // The indexed bind also binds to the generic binding point of the same target
        // (GL 4.6 core 6.1.1). Callers rely on it: the texture_gather tests set up their
        // SSBO with BindBufferBase and then size it through glBufferData on the generic
        // target alone, which would otherwise raise GL_INVALID_OPERATION and leave the
        // buffer with no storage.
        GetBufferBindingSlot(bufferTarget).Bind(bufferObject);
    }

    // GL 4.6 core 6.1.1: the constraints glBindBufferRange puts on the (offset, size) pair.
    // Every one of them is INVALID_VALUE, and all of them are checked before a single piece
    // of state is written - a rejected bind must leave the binding point exactly as it was.
    // They apply only to a non-zero buffer: buffer 0 detaches the binding point and ignores
    // offset and size, which is also how glBindBuffersRange spells "reset this element"
    // (a NULL buffers array, or a zero entry inside one).
    static Bool ValidateBufferRangeOffsetAndSize(GLenum target, GLintptr offset, GLsizeiptr size,
                                                 const char* funcName, Bool hasBuffer = true) {
        if (hasBuffer && size <= 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName,
                                             std::format("size ({}) must be greater than zero.", size)));
            return false;
        }
        if (offset < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName,
                                             std::format("offset ({}) must not be negative.", offset)));
            return false;
        }
        // GL_UNIFORM_BUFFER and GL_SHADER_STORAGE_BUFFER each constrain the offset to their own
        // implementation-defined alignment, which glGetIntegerv already answers.
        GLenum alignmentQuery = GL_NONE;
        if (target == GL_SHADER_STORAGE_BUFFER) {
            alignmentQuery = GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT;
        } else if (target == GL_UNIFORM_BUFFER) {
            alignmentQuery = GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT;
        }
        if (alignmentQuery != GL_NONE) {
            GLint alignment = 0;
            GetIntegerv(alignmentQuery, &alignment);
            if (alignment > 0 && (offset % static_cast<GLintptr>(alignment)) != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", funcName,
                        std::format("offset ({}) must be a multiple of {} ({}).", offset,
                                    MG_Util::ConvertGLEnumToString(alignmentQuery), alignment)));
                return false;
            }
        }
        // GL 4.6 core 6.1.1 constrains the OFFSET to a multiple of four for both
        // TRANSFORM_FEEDBACK_BUFFER and ATOMIC_COUNTER_BUFFER (the atomic-counter one has no
        // queryable alignment pname, which is why it was missing here), and the SIZE only for
        // transform feedback, whose capture is written in whole 32-bit components. Extending the
        // size rule to atomic counters as well breaks a legal bind: the conformance suite splits
        // MAX_ATOMIC_COUNTER_BUFFER_SIZE evenly across the binding points and that quotient is
        // not required to land on four.
        if ((target == GL_TRANSFORM_FEEDBACK_BUFFER || target == GL_ATOMIC_COUNTER_BUFFER) && (offset % 4) != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName,
                                             std::format("offset ({}) must be a multiple of 4 for {}.", offset,
                                                         MG_Util::ConvertGLEnumToString(target))));
            return false;
        }
        if (target == GL_TRANSFORM_FEEDBACK_BUFFER && hasBuffer && (size % 4) != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", funcName,
                    std::format("size ({}) must be a multiple of 4 for GL_TRANSFORM_FEEDBACK_BUFFER.", size)));
            return false;
        }
        return true;
    }

    void BindBufferRange_State(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
        MGLOG_D("%s: target = %s, index = %u, buffer = %u, offset = %d, size = %d", __func__,
                MG_Util::ConvertGLEnumToString(target).c_str(), index, buffer, offset, size);
        BufferTarget bufferTarget = MG_Util::ConvertGLEnumToBufferTarget(target);
        if (!BufferImpl::ValidateBufferBindingPointTarget(bufferTarget)) return;
        if (!BufferImpl::ValidateBufferBindingPointIndex(bufferTarget, index)) return;
        // The target's alignment rules are a property of the BINDING POINT, not of the buffer,
        // so they apply even when buffer is zero - which is exactly how
        // KHR-GL43.shader_storage_buffer_object.negative-api-bind probes the SSBO alignment
        // (glBindBufferRange(SHADER_STORAGE_BUFFER, 0, 0, alignment - 1, 0)). Only the size
        // rules need a buffer, since buffer 0 detaches the binding point and ignores size.
        if (!ValidateBufferRangeOffsetAndSize(target, offset, size, __func__, /*hasBuffer: */ buffer != 0)) return;
        if (bufferTarget == BufferTarget::TransformFeedback && MG_State::pGLContext->IsTransformFeedbackActive()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Transform feedback buffer bindings cannot change while transform "
                                             "feedback is active."));
            return;
        }
        MG_State::pGLContext->TouchBufferBindingPoint(bufferTarget, index);

        auto& point = MG_State::pGLContext->GetBufferBindingPoint(bufferTarget, index);
        SharedPtr<MG_State::GLState::BufferObject> bufferObject;
        if (buffer == 0) {
            point.Bind(nullptr);
            point.SetRange(Range1D(0, 0));
            GetBufferBindingSlot(bufferTarget).Bind(nullptr);
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
        // Also the generic binding point, exactly as BindBufferBase (GL 4.6 core 6.1.1).
        GetBufferBindingSlot(bufferTarget).Bind(bufferObject);
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

    void GetNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, void* data) {
        GetNamedBufferSubData_State(buffer, offset, size, data);
    }

    void GetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data) {
        GetBufferSubData_State(target, offset, size, data);
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

    // ARB_multi_bind: defined by the spec as equivalent to a loop over the single-bind entry
    // points (with buffer 0 resetting the binding point) - but only AFTER an up-front check
    // of the whole [first, first + count) range. Looping straight into the single-bind entry
    // points reports the single-bind INVALID_VALUE for an out-of-range index instead of the
    // multi-bind INVALID_OPERATION, and binds the in-range prefix before failing.
    static Bool ValidateMultiBindBufferRange(GLenum target, GLuint first, GLsizei count, const char* funcName) {
        BufferTarget bufferTarget = MG_Util::ConvertGLEnumToBufferTarget(target);
        if (!BufferImpl::ValidateBufferBindingPointTarget(bufferTarget)) return false;
        return BufferImpl::ValidateBufferBindingPointRange(bufferTarget, first, count, funcName);
    }

    // ARB_multi_bind states the equivalence to a loop of single binds "except that ... buffers
    // will not be created if they do not exist": glBindBuffer instantiates a name glGenBuffers
    // merely reserved, glBindBuffers* must refuse it and raise INVALID_OPERATION instead
    // (KHR-GL44.multi_bind.errors_bind_buffers).
    //
    // Deliberately PER ELEMENT, not all-or-nothing: the equivalence the extension defines is a
    // loop, so a bad entry costs its own binding point and nothing else. Rejecting the whole
    // call instead cost multi_bind.functional_bind_buffers_base its bindings.
    static Bool IsExistingBufferForMultiBind(GLuint buffer, GLsizei index, const char* funcName) {
        if (buffer == 0 || MG_State::pGLContext->ValidateBufferObject(buffer)) return true;
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidOperation,
            MakeUnique<GenericErrorInfo>(
                "MG_Impl/GLImpl", funcName,
                std::format("buffers[{}] ({}) is not the name of an existing buffer object.", index, buffer)));
        return false;
    }

    void BindBuffersBase(GLenum target, GLuint first, GLsizei count, const GLuint* buffers) {
        if (!ValidateMultiBindBufferRange(target, first, count, __func__)) return;
        for (GLsizei i = 0; i < count; ++i) {
            const GLuint buffer = buffers ? buffers[i] : 0;
            if (!IsExistingBufferForMultiBind(buffer, i, __func__)) continue;
            BindBufferBase_State(target, first + i, buffer);
        }
    }

    // The (offset, size) constraints are the one part of glBindBuffersRange that stays
    // per-element: ARB_multi_bind checks them separately for each binding point, leaves that
    // point unchanged on failure, and still applies the remaining elements - which is exactly
    // what looping into BindBufferRange_State does. Only the [first, first + count) range is
    // an up-front, all-or-nothing check. Elements that name buffer 0 (or a NULL buffers array)
    // reset the binding point through BindBufferBase_State and carry no offset/size to check.
    void BindBuffersRange(GLenum target, GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets,
                          const GLsizeiptr* sizes) {
        if (!ValidateMultiBindBufferRange(target, first, count, __func__)) return;
        for (GLsizei i = 0; i < count; ++i) {
            if (buffers && !IsExistingBufferForMultiBind(buffers[i], i, __func__)) continue;
            if (!buffers || buffers[i] == 0) {
                BindBufferBase_State(target, first + i, 0);
            } else {
                BindBufferRange_State(target, first + i, buffers[i], offsets ? offsets[i] : 0, sizes ? sizes[i] : 0);
            }
        }
    }
} // namespace MobileGL::MG_Impl::GLImpl
