// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "Config.h"
#include "VertexInputStateBuilder.h"
#include "MG_State/GLState/VertexArrayState/VertexArrayObject.h"
#include <Includes.h>
#include "../VkIncludes.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    class VertexInputStateFactory {
    public:
        using HashType = Uint64;

        enum class VertexStreamConversion : Uint8 {
            None = 0,
            Repack,
            ScaledIntegerToFloat32,
        };

        struct BackendVertexInputState {
            HashType hash = 0;
            // Hash of the resolved Vulkan vertex layout only (bindings, attributes,
            // unsupported mask) - NO buffer identities. `hash` mixes buffer heap
            // addresses so per-chunk VBOs mint a fresh identity per buffer; keying
            // pipelines on that minted one VkPipeline per chunk section for an
            // identical layout, defeating pipeline reuse and the per-draw memo.
            // Pipelines depend only on the layout, so they key on this instead.
            HashType layoutHash = 0;
            // Frame boundary of the last cache hit; entries idle past the
            // OnFrameBoundary retirement age are evicted (CPU heap only).
            // Mutable: the VAO's state-pointer memo fast path stamps it through
            // a const entry reference.
            mutable Uint64 lastUsedFrameBoundary = 0;
            Vector<VkVertexInputBindingDescription> bindings;
            Vector<VkVertexInputAttributeDescription> attributes;
            Vector<SizeT> bindingBufferKeys;
            Vector<SizeT> bindingBaseOffsets;
            Vector<Uint32> bindingAttributeLocations;
            Vector<Bool> bindingUsesClientMemory;
            Vector<VertexStreamConversion> bindingConversions;
            // Locations whose array is ENABLED but whose GL format has no VkFormat mapping. They are
            // absent from `attributes`, so without this mask the draw path cannot tell them apart from
            // a genuinely disabled array and would silently feed the shader the current attribute value.
            Uint32 unsupportedAttribMask = 0;
            // Bitmask of `attributes[i].location` - the draw path needs it up to
            // three times per draw, so it is baked once at build time.
            Uint32 attributeLocationMask = 0;
            // Per-binding glVertexAttribDivisor values other than 1. Vulkan's instance input
            // rate advances once per instance and nothing else, so anything else has to be
            // stated through VK_EXT_vertex_attribute_divisor. Empty when every instanced
            // binding uses divisor 1, which is what the plain input rate already means.
            Vector<VkVertexInputBindingDivisorDescriptionEXT> bindingDivisors;
            VkPipelineVertexInputDivisorStateCreateInfoEXT divisorState{
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT
            };
            VkPipelineVertexInputStateCreateInfo state{
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
            };
        };

        VertexInputStateFactory(const VulkanRendererConfig& config, VkPhysicalDevice physicalDevice):
            m_config(config), m_physicalDevice(physicalDevice) {}
        ~VertexInputStateFactory() = default;
        VertexInputStateFactory(const VertexInputStateFactory&) = delete;

        HashType ComputeHash(const MG_State::GLState::VertexArrayObject& vao) const;
        // Memoized ComputeHash: reuses the VAO's cached hash while its config version
        // is unchanged. Use this on per-draw paths.
        HashType GetOrComputeHash(const MG_State::GLState::VertexArrayObject& vao) const;
        const BackendVertexInputState& GetOrCreateVertexInputState(
            const MG_State::GLState::VertexArrayObject& vao, HashType hash);
        const BackendVertexInputState& GetOrCreateVertexInputState(const MG_State::GLState::VertexArrayObject& vao);
        // Frame boundary hook: ages the cache and evicts entries not hit for many
        // frames. The key mixes buffer heap addresses, so buffer/VAO churn keeps
        // minting fresh keys; without eviction the map grows for the whole session.
        // Entries hold no Vulkan handles (pipeline creation copies the descriptions)
        // and the draw path's entry reference never spans a frame boundary, so
        // eviction here needs no GPU-idle proof. Self-gated: one counter bump and
        // compare except on sweep boundaries.
        void OnFrameBoundary();
        static SizeT GetComponentSize(DataType type);
        // Tightly-packed byte size of one vertex element for this attribute: componentSize * size for
        // normal types, and 4 (one packed word) for the 2_10_10_10 types and GL_BGRA. Returns 0 for
        // an unknown/unsupported type.
        static SizeT GetAttributeByteSize(DataType type, Int size, Bool isBgra);

    private:
        static VkFormat ToVkVertexFormat(DataType type, Int size, Bool normalized, Bool isInteger, Bool isBgra = false,
                                         Bool isLong = false);
        static Bool IsScaledIntegerVertexFormat(VkFormat format);
        static VkFormat ToFloat32VertexFormat(Int componentCount);
        Bool SupportsVertexBufferFormat(VkFormat format) const;

        const VulkanRendererConfig& m_config;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        // Values are heap-allocated: FastSTL::unordered_map is open-addressing,
        // so INSERT invalidates references to stored values. The draw path (and
        // the VAOs' state-pointer memos) hold entry pointers across inserts;
        // only the unique_ptr cell moves, never the pointee.
        UnorderedMap<HashType, UniquePtr<BackendVertexInputState>> m_cache;
        // Monotonic frame-boundary counter (bumped in OnFrameBoundary) for cache aging.
        Uint64 m_frameBoundaryCounter = 0;
        // Bumped whenever any cache entry is erased. VAOs memo a raw pointer to
        // their heap-allocated entry (stable across map insert/rehash by
        // construction); a memo is honored only while its recorded epoch
        // matches, so an evicted entry can never be dereferenced through a
        // stale memo.
        Uint64 m_evictionEpoch = 1;
        static inline XXH64_state_t* m_hashState = XXH64_createState();
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
