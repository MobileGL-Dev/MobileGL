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

        for (Uint32 location = 0; location < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS; ++location) {
            const auto& attr = vao.GetAttribute(location);
            if (!attr.Enabled) {
                continue;
            }

            const auto vkFormat = ToVkVertexFormat(attr.Type, attr.Size, attr.Normalized, attr.IsInteger);
            if (vkFormat == VK_FORMAT_UNDEFINED) {
                MGLOG_D("Skipping unsupported vertex attribute layout (location=%u, type=%s, size=%d)",
                        location, MG_Util::ConvertDataTypeToString(attr.Type).c_str(), attr.Size);
                continue;
            }

            const SizeT componentSize = GetComponentSize(attr.Type);
            if (componentSize == 0) {
                MGLOG_D("Skipping vertex attribute with unknown component size (location=%u, type=%s)",
                        location, MG_Util::ConvertDataTypeToString(attr.Type).c_str());
                continue;
            }

            const Uint32 stride = attr.Stride > 0
                                      ? static_cast<Uint32>(attr.Stride)
                                      : static_cast<Uint32>(componentSize * static_cast<SizeT>(attr.Size));
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
        entry.state = state;
        entry.state.pVertexBindingDescriptions = entry.bindings.empty() ? nullptr : entry.bindings.data();
        entry.state.pVertexAttributeDescriptions = entry.attributes.empty() ? nullptr : entry.attributes.data();
        return entry;
    }

    VkFormat VertexInputStateFactory::ToVkVertexFormat(DataType type, Int size, Bool normalized, Bool isInteger) {
        switch (type) {
        case DataType::Float32:
            switch (size) {
            case 1: return VK_FORMAT_R32_SFLOAT;
            case 2: return VK_FORMAT_R32G32_SFLOAT;
            case 3: return VK_FORMAT_R32G32B32_SFLOAT;
            case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
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
} // namespace MobileGL::MG_Backend::DirectVulkan
