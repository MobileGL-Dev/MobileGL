// MobileGL - MobileGL/MG_Util/Converters/MGToGL/BufferEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BufferEnumConverter.h"

namespace MobileGL {
    namespace MG_Util {
        GLenum ConvertBufferTargetToGLEnum(BufferTarget bufferTarget) {
            switch (bufferTarget) {
            case BufferTarget::Vertex:
                return GL_ARRAY_BUFFER;
            case BufferTarget::Index:
                return GL_ELEMENT_ARRAY_BUFFER;
            case BufferTarget::Uniform:
                return GL_UNIFORM_BUFFER;
            case BufferTarget::CopyRead:
                return GL_COPY_READ_BUFFER;
            case BufferTarget::CopyWrite:
                return GL_COPY_WRITE_BUFFER;
            case BufferTarget::PixelPack:
                return GL_PIXEL_PACK_BUFFER;
            case BufferTarget::PixelUnpack:
                return GL_PIXEL_UNPACK_BUFFER;
            case BufferTarget::Query:
                return GL_QUERY_BUFFER;
            case BufferTarget::Texture:
                return GL_TEXTURE_BUFFER;
            case BufferTarget::TransformFeedback:
                return GL_TRANSFORM_FEEDBACK_BUFFER;
            case BufferTarget::AtomicCounter:
                return GL_ATOMIC_COUNTER_BUFFER;
            case BufferTarget::DispatchIndirect:
                return GL_DISPATCH_INDIRECT_BUFFER;
            case BufferTarget::DrawIndirect:
                return GL_DRAW_INDIRECT_BUFFER;
            case BufferTarget::Parameter:
                return GL_PARAMETER_BUFFER_ARB;
            case BufferTarget::ShaderStorage:
                return GL_SHADER_STORAGE_BUFFER;
            case BufferTarget::Unknown:
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertBufferUsageToGLEnum(BufferUsage usage) {
            switch (usage) {
            case BufferUsage::StreamDraw:
                return GL_STREAM_DRAW;
            case BufferUsage::StreamRead:
                return GL_STREAM_READ;
            case BufferUsage::StreamCopy:
                return GL_STREAM_COPY;
            case BufferUsage::StaticDraw:
                return GL_STATIC_DRAW;
            case BufferUsage::StaticRead:
                return GL_STATIC_READ;
            case BufferUsage::StaticCopy:
                return GL_STATIC_COPY;
            case BufferUsage::DynamicDraw:
                return GL_DYNAMIC_DRAW;
            case BufferUsage::DynamicRead:
                return GL_DYNAMIC_READ;
            case BufferUsage::DynamicCopy:
                return GL_DYNAMIC_COPY;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLbitfield ConvertBufferMappingAccessToGLEnum(Flags<BufferMappingAccessBit> access) {
            GLbitfield result = 0;
            if (access & BufferMappingAccessBit::Read) result |= GL_MAP_READ_BIT;
            if (access & BufferMappingAccessBit::Write) result |= GL_MAP_WRITE_BIT;
            if (access & BufferMappingAccessBit::InvalidateRange) result |= GL_MAP_INVALIDATE_RANGE_BIT;
            if (access & BufferMappingAccessBit::InvalidateBuffer) result |= GL_MAP_INVALIDATE_BUFFER_BIT;
            if (access & BufferMappingAccessBit::FlushExplicit) result |= GL_MAP_FLUSH_EXPLICIT_BIT;
            if (access & BufferMappingAccessBit::Unsynchronized) result |= GL_MAP_UNSYNCHRONIZED_BIT;
            if (access & BufferMappingAccessBit::Persistent) result |= GL_MAP_PERSISTENT_BIT;
            if (access & BufferMappingAccessBit::Coherent) result |= GL_MAP_COHERENT_BIT;
            return result;
        }
    } // namespace MG_Util
} // namespace MobileGL
