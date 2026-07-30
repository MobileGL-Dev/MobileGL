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

            // The buffer's heap address is an identity component of the key: a freed
            // buffer's reused address can alias an old cache entry, but only under a
            // byte-identical attribute layout - and the entry payload is a pure function
            // of the hashed inputs, with the draw path re-resolving bindingBufferKeys
            // against the live VAO attribute pointers, so an aliased hit returns exactly
            // what a rebuild would. Address drift only grows the map; the OnFrameBoundary
            // aging sweep bounds that.
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
        // Per-draw fast path: the VAO carries a pointer to its resolved entry,
        // valid while its config version and the cache's eviction epoch both
        // match - no re-hash, no map lookup.
        const void* memoState = nullptr;
        Uint64 memoEpoch = 0;
        if (vao.GetBackendStateMemo(memoState, memoEpoch) && memoEpoch == m_evictionEpoch) {
            const auto* entry = static_cast<const BackendVertexInputState*>(memoState);
            entry->lastUsedFrameBoundary = m_frameBoundaryCounter;
            return *entry;
        }
        const BackendVertexInputState& entry = GetOrCreateVertexInputState(vao, GetOrComputeHash(vao));
        vao.SetBackendStateMemo(&entry, m_evictionEpoch);
        return entry;
    }

    const VertexInputStateFactory::BackendVertexInputState& VertexInputStateFactory::GetOrCreateVertexInputState(
        const MG_State::GLState::VertexArrayObject& vao, HashType hash) {
        auto it = m_cache.find(hash);
        if (it != m_cache.end()) {
            it->second->lastUsedFrameBoundary = m_frameBoundaryCounter;
            return *it->second;
        }

        VertexInputStateBuilder builder;
        Vector<SizeT> bindingBufferKeys;
        Vector<SizeT> bindingBaseOffsets;
        Vector<Uint32> bindingAttributeLocations;
        Vector<Bool> bindingUsesClientMemory;
        Vector<VertexStreamConversion> bindingConversions;
        Uint32 unsupportedAttribMask = 0;

        for (Uint32 location = 0; location < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS; ++location) {
            const auto& attr = vao.GetAttribute(location);
            if (!attr.Enabled) {
                continue;
            }

            const VkFormat sourceVkFormat =
                ToVkVertexFormat(attr.Type, attr.Size, attr.Normalized, attr.IsInteger, attr.IsBgra);
            if (sourceVkFormat == VK_FORMAT_UNDEFINED) {
                MGLOG_E("Unsupported vertex attribute layout (location=%u, type=%s, size=%d): the array is "
                        "enabled but cannot be mapped to a VkFormat",
                        location, MG_Util::ConvertDataTypeToString(attr.Type).c_str(), attr.Size);
                unsupportedAttribMask |= (1u << location);
                continue;
            }

            VkFormat vkFormat = sourceVkFormat;
            VertexStreamConversion conversion = VertexStreamConversion::None;
            if (!SupportsVertexBufferFormat(vkFormat)) {
                if (IsScaledIntegerVertexFormat(vkFormat)) {
                    const VkFormat fallbackFormat = ToFloat32VertexFormat(attr.Size);
                    if (fallbackFormat != VK_FORMAT_UNDEFINED && SupportsVertexBufferFormat(fallbackFormat)) {
                        vkFormat = fallbackFormat;
                        conversion = VertexStreamConversion::ScaledIntegerToFloat32;
                        MGLOG_W("Vertex attribute location=%u format=%d lacks "
                                "VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT; using float32 stream format=%d "
                                "(type=%s size=%d normalized=%s integer=%s)",
                                location, static_cast<Int>(sourceVkFormat), static_cast<Int>(vkFormat),
                                MG_Util::ConvertDataTypeToString(attr.Type).c_str(), attr.Size,
                                attr.Normalized ? "true" : "false", attr.IsInteger ? "true" : "false");
                    }
                }

                if (conversion == VertexStreamConversion::None) {
                    MGLOG_E("Unsupported Vulkan vertex format (location=%u, format=%d, type=%s, size=%d): "
                            "VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT is unavailable and no semantic fallback exists",
                            location, static_cast<Int>(sourceVkFormat),
                            MG_Util::ConvertDataTypeToString(attr.Type).c_str(), attr.Size);
                    unsupportedAttribMask |= (1u << location);
                    continue;
                }
            }

            const SizeT attribByteSize = GetAttributeByteSize(attr.Type, attr.Size, attr.IsBgra);
            if (attribByteSize == 0) {
                MGLOG_E("Vertex attribute with unknown component size (location=%u, type=%s): the array is "
                        "enabled but cannot be sized",
                        location, MG_Util::ConvertDataTypeToString(attr.Type).c_str());
                unsupportedAttribMask |= (1u << location);
                continue;
            }

            const Uint32 sourceStride =
                attr.Stride > 0 ? static_cast<Uint32>(attr.Stride) : static_cast<Uint32>(attribByteSize);
            const Bool packedAttribute = attr.Type == DataType::Int2101010Rev ||
                                         attr.Type == DataType::Uint2101010Rev;
            const SizeT requiredAlignment = packedAttribute ? attribByteSize : GetComponentSize(attr.Type);
            // For a client-memory array attr.Offset holds the raw client pointer, and the
            // draw path re-uploads the data to a 16-aligned transient slice with attribute
            // offset 0, so only the stride can violate Vulkan's fetch alignment there.
            const Bool clientMemoryAttribute = attr.Buffer == nullptr;
            if (conversion == VertexStreamConversion::None && requiredAlignment > 1 &&
                ((sourceStride % requiredAlignment) != 0 ||
                 (!clientMemoryAttribute && (attr.Offset % requiredAlignment) != 0))) {
                // GL accepts arbitrary byte strides and offsets. Core Vulkan vertex fetches do not
                // unless VK_EXT_legacy_vertex_attributes is available, so deinterleave this one
                // attribute into a tightly packed transient stream without changing its format.
                conversion = VertexStreamConversion::Repack;
                MGLOG_W("Vertex attribute location=%u uses Vulkan-incompatible alignment "
                        "(offset=%zu stride=%u required=%zu); using a tightly packed stream",
                        location, attr.Offset, sourceStride, requiredAlignment);
            }

            Uint32 stride = sourceStride;
            if (conversion == VertexStreamConversion::Repack) {
                stride = static_cast<Uint32>(attribByteSize);
            } else if (conversion == VertexStreamConversion::ScaledIntegerToFloat32) {
                stride = static_cast<Uint32>(attr.Size * static_cast<Int>(sizeof(Float)));
            }
            const VkVertexInputRate inputRate =
                (attr.Divisor == 0) ? VK_VERTEX_INPUT_RATE_VERTEX : VK_VERTEX_INPUT_RATE_INSTANCE;

            const SizeT bufferKey = reinterpret_cast<SizeT>(attr.Buffer.get());
            const Uint32 binding = static_cast<Uint32>(bindingBufferKeys.size());
            bindingBufferKeys.push_back(bufferKey);
            bindingBaseOffsets.push_back(attr.Buffer ? attr.Offset : 0);
            bindingAttributeLocations.push_back(location);
            bindingUsesClientMemory.push_back(attr.Buffer == nullptr);
            bindingConversions.push_back(conversion);
            builder.AddBinding(binding, stride, inputRate);
            builder.AddAttribute(location, binding, vkFormat, 0);
        }

        const auto& state = builder.Build();

        auto& slot = m_cache[hash];
        if (!slot) {
            slot = MakeUnique<BackendVertexInputState>();
        }
        BackendVertexInputState& entry = *slot;
        entry.hash = hash;
        entry.lastUsedFrameBoundary = m_frameBoundaryCounter;
        entry.bindings = builder.GetBindings();
        entry.attributes = builder.GetAttributes();
        // See the layoutHash declaration: hash only the resolved layout, never
        // buffer identities, so identical layouts across VAOs/buffers agree.
        XXHASH_VERIFY(XXH64_reset(m_hashState, 0));
        for (const auto& binding : entry.bindings) {
            XXHASH_VERIFY(XXH64_update(m_hashState, &binding.binding, sizeof(binding.binding)));
            XXHASH_VERIFY(XXH64_update(m_hashState, &binding.stride, sizeof(binding.stride)));
            XXHASH_VERIFY(XXH64_update(m_hashState, &binding.inputRate, sizeof(binding.inputRate)));
        }
        for (const auto& attribute : entry.attributes) {
            XXHASH_VERIFY(XXH64_update(m_hashState, &attribute.location, sizeof(attribute.location)));
            XXHASH_VERIFY(XXH64_update(m_hashState, &attribute.binding, sizeof(attribute.binding)));
            XXHASH_VERIFY(XXH64_update(m_hashState, &attribute.format, sizeof(attribute.format)));
            XXHASH_VERIFY(XXH64_update(m_hashState, &attribute.offset, sizeof(attribute.offset)));
        }
        XXHASH_VERIFY(XXH64_update(m_hashState, &unsupportedAttribMask, sizeof(unsupportedAttribMask)));
        entry.layoutHash = XXH64_digest(m_hashState);
        entry.bindingBufferKeys = std::move(bindingBufferKeys);
        entry.bindingBaseOffsets = std::move(bindingBaseOffsets);
        entry.bindingAttributeLocations = std::move(bindingAttributeLocations);
        entry.bindingUsesClientMemory = std::move(bindingUsesClientMemory);
        entry.bindingConversions = std::move(bindingConversions);
        entry.unsupportedAttribMask = unsupportedAttribMask;
        entry.state = state;
        entry.state.pVertexBindingDescriptions = entry.bindings.empty() ? nullptr : entry.bindings.data();
        entry.state.pVertexAttributeDescriptions = entry.attributes.empty() ? nullptr : entry.attributes.data();
        return entry;
    }

    void VertexInputStateFactory::OnFrameBoundary() {
        ++m_frameBoundaryCounter;

        // Sweep occasionally; evict entries whose last hit is far in the past.
        // Erasure happens only here, never mid-frame: the draw path holds a
        // reference into the current entry across its setup, and unordered_map
        // erase would invalidate it. Entries are CPU-side only, so no GPU-idle
        // proof is needed; an evicted entry that is used again is simply rebuilt
        // from the VAO state (same hash, same content).
        constexpr Uint64 kSweepInterval = 256;
        constexpr Uint64 kRetireAgeBoundaries = 1024;
        if ((m_frameBoundaryCounter % kSweepInterval) != 0) {
            return;
        }

        for (auto it = m_cache.begin(); it != m_cache.end();) {
            if (m_frameBoundaryCounter - it->second->lastUsedFrameBoundary > kRetireAgeBoundaries) {
                it = m_cache.erase(it);
                // Invalidate every VAO's state-pointer memo: the erased node's
                // address may be reused by a future insert.
                ++m_evictionEpoch;
            } else {
                ++it;
            }
        }
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

    Bool VertexInputStateFactory::IsScaledIntegerVertexFormat(VkFormat format) {
        switch (format) {
        case VK_FORMAT_R8_USCALED:
        case VK_FORMAT_R8_SSCALED:
        case VK_FORMAT_R8G8_USCALED:
        case VK_FORMAT_R8G8_SSCALED:
        case VK_FORMAT_R8G8B8_USCALED:
        case VK_FORMAT_R8G8B8_SSCALED:
        case VK_FORMAT_R8G8B8A8_USCALED:
        case VK_FORMAT_R8G8B8A8_SSCALED:
        case VK_FORMAT_R16_USCALED:
        case VK_FORMAT_R16_SSCALED:
        case VK_FORMAT_R16G16_USCALED:
        case VK_FORMAT_R16G16_SSCALED:
        case VK_FORMAT_R16G16B16_USCALED:
        case VK_FORMAT_R16G16B16_SSCALED:
        case VK_FORMAT_R16G16B16A16_USCALED:
        case VK_FORMAT_R16G16B16A16_SSCALED:
            return true;
        default:
            return false;
        }
    }

    VkFormat VertexInputStateFactory::ToFloat32VertexFormat(Int componentCount) {
        switch (componentCount) {
        case 1: return VK_FORMAT_R32_SFLOAT;
        case 2: return VK_FORMAT_R32G32_SFLOAT;
        case 3: return VK_FORMAT_R32G32B32_SFLOAT;
        case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
        }
    }

    Bool VertexInputStateFactory::SupportsVertexBufferFormat(VkFormat format) const {
        if (m_physicalDevice == VK_NULL_HANDLE || format == VK_FORMAT_UNDEFINED) {
            return false;
        }
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &properties);
        return (properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
