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

        struct BackendVertexInputState {
            HashType hash = 0;
            Vector<VkVertexInputBindingDescription> bindings;
            Vector<VkVertexInputAttributeDescription> attributes;
            Vector<SizeT> bindingBufferKeys;
            Vector<SizeT> bindingBaseOffsets;
            Vector<Uint32> bindingAttributeLocations;
            Vector<Bool> bindingUsesClientMemory;
            // Locations whose array is ENABLED but whose GL format has no VkFormat mapping. They are
            // absent from `attributes`, so without this mask the draw path cannot tell them apart from
            // a genuinely disabled array and would silently feed the shader the current attribute value.
            Uint32 unsupportedAttribMask = 0;
            VkPipelineVertexInputStateCreateInfo state{
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
            };
        };

        explicit VertexInputStateFactory(const VulkanRendererConfig& config):
            m_config(config) {}
        ~VertexInputStateFactory() = default;
        VertexInputStateFactory(const VertexInputStateFactory&) = delete;

        HashType ComputeHash(const MG_State::GLState::VertexArrayObject& vao) const;
        // Memoized ComputeHash: reuses the VAO's cached hash while its config version
        // is unchanged. Use this on per-draw paths.
        HashType GetOrComputeHash(const MG_State::GLState::VertexArrayObject& vao) const;
        const BackendVertexInputState& GetOrCreateVertexInputState(
            const MG_State::GLState::VertexArrayObject& vao, HashType hash);
        const BackendVertexInputState& GetOrCreateVertexInputState(const MG_State::GLState::VertexArrayObject& vao);
        static SizeT GetComponentSize(DataType type);
        // Tightly-packed byte size of one vertex element for this attribute: componentSize * size for
        // normal types, and 4 (one packed word) for the 2_10_10_10 types and GL_BGRA. Returns 0 for
        // an unknown/unsupported type.
        static SizeT GetAttributeByteSize(DataType type, Int size, Bool isBgra);

    private:
        static VkFormat ToVkVertexFormat(DataType type, Int size, Bool normalized, Bool isInteger, Bool isBgra = false);

        const VulkanRendererConfig& m_config;
        UnorderedMap<HashType, BackendVertexInputState> m_cache;
        static inline XXH64_state_t* m_hashState = XXH64_createState();
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
