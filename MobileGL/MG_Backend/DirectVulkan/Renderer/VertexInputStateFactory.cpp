// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VertexInputStateFactory.h"
#include "MG_Util/Converters/MGToStr/DataTypeConverter.h"
#include <utility>

namespace MobileGL::MG_Backend::DirectVulkan {
    VertexInputStateFactory::HashType VertexInputStateFactory::ComputeHash(
        const MG_State::GLState::VertexArrayObject& vao) const {
        XXHASH_VERIFY(XXH64_reset(m_hashState, m_config.CacheVersion));

        for (Int i = 0; i < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS; ++i) {
            const auto& attr = vao.GetAttribute(i);

            XXHASH_VERIFY(XXH64_update(m_hashState, &attr.Enabled, sizeof(attr.Enabled)));
            if (!attr.Enabled) {
                continue;
            }

            XXHASH_VERIFY(XXH64_update(m_hashState, &attr.Size, sizeof(attr.Size)));
            XXHASH_VERIFY(XXH64_update(m_hashState, &attr.Type, sizeof(attr.Type)));
            XXHASH_VERIFY(XXH64_update(m_hashState, &attr.Normalized, sizeof(attr.Normalized)));
            XXHASH_VERIFY(XXH64_update(m_hashState, &attr.Stride, sizeof(attr.Stride)));
            XXHASH_VERIFY(XXH64_update(m_hashState, &attr.Offset, sizeof(attr.Offset)));
            XXHASH_VERIFY(XXH64_update(m_hashState, &attr.IsInteger, sizeof(attr.IsInteger)));
            XXHASH_VERIFY(XXH64_update(m_hashState, &attr.IsBgra, sizeof(attr.IsBgra)));
            XXHASH_VERIFY(XXH64_update(m_hashState, &attr.Divisor, sizeof(attr.Divisor)));

            const SizeT bufferKey = reinterpret_cast<SizeT>(attr.Buffer.get());
            XXHASH_VERIFY(XXH64_update(m_hashState, &bufferKey, sizeof(bufferKey)));
        }

        return XXH64_digest(m_hashState);
    }

    VertexInputStateFactory::HashType VertexInputStateFactory::GetOrComputeHash(
        const MG_State::GLState::VertexArrayObject& vao) const {
        HashType hash = 0;
        if (!vao.GetBackendHashMemo(hash)) {
            hash = ComputeHash(vao);
            vao.SetBackendHashMemo(hash);
        }
        return hash;
    }

    const VertexInputStateFactory::BackendVertexInputState& VertexInputStateFactory::GetOrCreateVertexInputState(
        const MG_State::GLState::VertexArrayObject& vao) {
        return GetOrCreateVertexInputState(vao, GetOrComputeHash(vao));
    }

    const VertexInputStateFactory::BackendVertexInputState& VertexInputStateFactory::GetOrCreateVertexInputState(
        const MG_State::GLState::VertexArrayObject& vao, HashType hash) {
        auto it = m_cache.find(hash);
        if (it != m_cache.end()) {
            return it->second;
        }

        VertexInputStateBuilder builder;
        Vector<SizeT> bindingBufferKeys;
        Vector<SizeT> bindingBaseOffsets;
        Vector<Uint32> bindingAttributeLocations;
        Vector<Bool> bindingUsesClientMemory;
        Uint32 unsupportedAttribMask = 0;

        for (Uint32 location = 0; location < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS; ++location) {
            const auto& attr = vao.GetAttribute(location);
            if (!attr.Enabled) {
                continue;
            }

            const auto vkFormat = ToVkVertexFormat(attr.Type, attr.Size, attr.Normalized, attr.IsInteger, attr.IsBgra);
            if (vkFormat == VK_FORMAT_UNDEFINED) {
                MGLOG_E("Unsupported vertex attribute layout (location=%u, type=%s, size=%d): the array is "
                        "enabled but cannot be mapped to a VkFormat",
                        location, MG_Util::ConvertDataTypeToString(attr.Type).c_str(), attr.Size);
                unsupportedAttribMask |= (1u << location);
                continue;
            }

            const SizeT attribByteSize = GetAttributeByteSize(attr.Type, attr.Size, attr.IsBgra);
            if (attribByteSize == 0) {
                MGLOG_E("Vertex attribute with unknown component size (location=%u, type=%s): the array is "
                        "enabled but cannot be sized",
                        location, MG_Util::ConvertDataTypeToString(attr.Type).c_str());
                unsupportedAttribMask |= (1u << location);
                continue;
            }

            const Uint32 stride =
                attr.Stride > 0 ? static_cast<Uint32>(attr.Stride) : static_cast<Uint32>(attribByteSize);
            const VkVertexInputRate inputRate =
                (attr.Divisor == 0) ? VK_VERTEX_INPUT_RATE_VERTEX : VK_VERTEX_INPUT_RATE_INSTANCE;

            const SizeT bufferKey = reinterpret_cast<SizeT>(attr.Buffer.get());
            const Uint32 binding = static_cast<Uint32>(bindingBufferKeys.size());
            bindingBufferKeys.push_back(bufferKey);
            bindingBaseOffsets.push_back(attr.Buffer ? attr.Offset : 0);
            bindingAttributeLocations.push_back(location);
            bindingUsesClientMemory.push_back(attr.Buffer == nullptr);
            builder.AddBinding(binding, stride, inputRate);
            builder.AddAttribute(location, binding, vkFormat, 0);
        }

        const auto& state = builder.Build();

        auto& entry = m_cache[hash];
        entry.hash = hash;
        entry.bindings = builder.GetBindings();
        entry.attributes = builder.GetAttributes();
        entry.bindingBufferKeys = std::move(bindingBufferKeys);
        entry.bindingBaseOffsets = std::move(bindingBaseOffsets);
        entry.bindingAttributeLocations = std::move(bindingAttributeLocations);
        entry.bindingUsesClientMemory = std::move(bindingUsesClientMemory);
        entry.unsupportedAttribMask = unsupportedAttribMask;
        entry.state = state;
        entry.state.pVertexBindingDescriptions = entry.bindings.empty() ? nullptr : entry.bindings.data();
        entry.state.pVertexAttributeDescriptions = entry.attributes.empty() ? nullptr : entry.attributes.data();
        return entry;
    }

    VkFormat VertexInputStateFactory::ToVkVertexFormat(DataType type, Int size, Bool normalized, Bool isInteger,
                                                       Bool isBgra) {
        if (isBgra) {
            // GL_BGRA: four reversed-order components, always normalized (enforced at validation), only
            // legal with GL_UNSIGNED_BYTE or a 2_10_10_10 type. The reversed VkFormats put the
            // components back into R,G,B,A order for the shader.
            switch (type) {
            case DataType::Uint8:
                return VK_FORMAT_B8G8R8A8_UNORM;
            case DataType::Uint2101010Rev:
                return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            case DataType::Int2101010Rev:
                return VK_FORMAT_A2R10G10B10_SNORM_PACK32;
            default:
                return VK_FORMAT_UNDEFINED;
            }
        }
        switch (type) {
        case DataType::Uint2101010Rev:
            // Packed 2_10_10_10 travels the float-normalizing path only; size is always 4. SNORM/UNORM
            // normalize, SSCALED/USCALED cast the packed field to float.
            if (isInteger || size != 4) return VK_FORMAT_UNDEFINED;
            return normalized ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        case DataType::Int2101010Rev:
            if (isInteger || size != 4) return VK_FORMAT_UNDEFINED;
            return normalized ? VK_FORMAT_A2B10G10R10_SNORM_PACK32 : VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
        case DataType::Float32:
            switch (size) {
            case 1: return VK_FORMAT_R32_SFLOAT;
            case 2: return VK_FORMAT_R32G32_SFLOAT;
            case 3: return VK_FORMAT_R32G32B32_SFLOAT;
            case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Float16:
            // GL_HALF_FLOAT is a floating-point array type: it is never an integer attribute, and
            // GL_TRUE for `normalized` is ignored for float types rather than selecting a *NORM format.
            if (isInteger) return VK_FORMAT_UNDEFINED;
            switch (size) {
            case 1: return VK_FORMAT_R16_SFLOAT;
            case 2: return VK_FORMAT_R16G16_SFLOAT;
            case 3: return VK_FORMAT_R16G16B16_SFLOAT;
            case 4: return VK_FORMAT_R16G16B16A16_SFLOAT;
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Int32:
            if (!isInteger || normalized) return VK_FORMAT_UNDEFINED;
            switch (size) {
            case 1: return VK_FORMAT_R32_SINT;
            case 2: return VK_FORMAT_R32G32_SINT;
            case 3: return VK_FORMAT_R32G32B32_SINT;
            case 4: return VK_FORMAT_R32G32B32A32_SINT;
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Uint32:
            if (!isInteger || normalized) return VK_FORMAT_UNDEFINED;
            switch (size) {
            case 1: return VK_FORMAT_R32_UINT;
            case 2: return VK_FORMAT_R32G32_UINT;
            case 3: return VK_FORMAT_R32G32B32_UINT;
            case 4: return VK_FORMAT_R32G32B32A32_UINT;
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Int16:
            switch (size) {
            case 1:
                return isInteger ? VK_FORMAT_R16_SINT : (normalized ? VK_FORMAT_R16_SNORM : VK_FORMAT_R16_SSCALED);
            case 2:
                return isInteger ? VK_FORMAT_R16G16_SINT
                                 : (normalized ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_SSCALED);
            case 3:
                return isInteger ? VK_FORMAT_R16G16B16_SINT
                                 : (normalized ? VK_FORMAT_R16G16B16_SNORM : VK_FORMAT_R16G16B16_SSCALED);
            case 4:
                return isInteger ? VK_FORMAT_R16G16B16A16_SINT
                                 : (normalized ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_SSCALED);
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Uint16:
            switch (size) {
            case 1:
                return isInteger ? VK_FORMAT_R16_UINT : (normalized ? VK_FORMAT_R16_UNORM : VK_FORMAT_R16_USCALED);
            case 2:
                return isInteger ? VK_FORMAT_R16G16_UINT
                                 : (normalized ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R16G16_USCALED);
            case 3:
                return isInteger ? VK_FORMAT_R16G16B16_UINT
                                 : (normalized ? VK_FORMAT_R16G16B16_UNORM : VK_FORMAT_R16G16B16_USCALED);
            case 4:
                return isInteger ? VK_FORMAT_R16G16B16A16_UINT
                                 : (normalized ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R16G16B16A16_USCALED);
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Int8:
            switch (size) {
            case 1:
                return isInteger ? VK_FORMAT_R8_SINT : (normalized ? VK_FORMAT_R8_SNORM : VK_FORMAT_R8_SSCALED);
            case 2:
                return isInteger ? VK_FORMAT_R8G8_SINT
                                 : (normalized ? VK_FORMAT_R8G8_SNORM : VK_FORMAT_R8G8_SSCALED);
            case 3:
                return isInteger ? VK_FORMAT_R8G8B8_SINT
                                 : (normalized ? VK_FORMAT_R8G8B8_SNORM : VK_FORMAT_R8G8B8_SSCALED);
            case 4:
                return isInteger ? VK_FORMAT_R8G8B8A8_SINT
                                 : (normalized ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_SSCALED);
            default: return VK_FORMAT_UNDEFINED;
            }
        case DataType::Uint8:
            switch (size) {
            case 1:
                return isInteger ? VK_FORMAT_R8_UINT : (normalized ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8_USCALED);
            case 2:
                return isInteger ? VK_FORMAT_R8G8_UINT
                                 : (normalized ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8G8_USCALED);
            case 3:
                return isInteger ? VK_FORMAT_R8G8B8_UINT
                                 : (normalized ? VK_FORMAT_R8G8B8_UNORM : VK_FORMAT_R8G8B8_USCALED);
            case 4:
                return isInteger ? VK_FORMAT_R8G8B8A8_UINT
                                 : (normalized ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_USCALED);
            default: return VK_FORMAT_UNDEFINED;
            }
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    SizeT VertexInputStateFactory::GetComponentSize(DataType type) {
        switch (type) {
        case DataType::Int8:
        case DataType::Uint8:
            return 1;
        case DataType::Int16:
        case DataType::Uint16:
        case DataType::Float16:
            return 2;
        case DataType::Int32:
        case DataType::Uint32:
        case DataType::Float32:
        case DataType::Fixed32:
            return 4;
        case DataType::Float64:
            return 8;
        default:
            return 0;
        }
    }

    SizeT VertexInputStateFactory::GetAttributeByteSize(DataType type, Int size, Bool isBgra) {
        // The packed 2_10_10_10 types are a single 32-bit word for all 4 components; GL_BGRA is always
        // 4 components (GL_UNSIGNED_BYTE x4 = 4 bytes, or a packed word = 4 bytes) -- both are 4 bytes.
        if (type == DataType::Int2101010Rev || type == DataType::Uint2101010Rev || isBgra) {
            return 4;
        }
        const SizeT componentSize = GetComponentSize(type);
        return componentSize == 0 ? 0 : componentSize * static_cast<SizeT>(size);
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
