// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
// MG_Pipe::MGPipeHandle for the P2 D12.5 memo table below. A header of constexpr constants,
// so the pull build gains nothing from it.
#include <MG_Pipe/MGPipeHandles.h>
#include <MG_Pipe/PipeApply.h>

#include "Config.h"
#include "MagmaPipeArms.h"
#include "VertexInputStateBuilder.h"
#include "MG_State/GLState/VertexArrayState/VertexArrayObject.h"
#include <Includes.h>
#include <atomic>
#include "../VkIncludes.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    class VertexInputStateFactory {
    public:
        using HashType = Uint64;

        enum class VertexStreamConversion : Uint8 {
            None = 0,
            Repack,
            ScaledIntegerToFloat32,
            // GL_DOUBLE source data narrowed to a tightly packed float32 stream: the fetch half
            // of the fp64 demotion the shader side already does unconditionally.
            Float64ToFloat32,
        };

        struct BackendVertexInputState {
            HashType hash = 0;
            // Hash of the resolved Vulkan vertex layout only (bindings, attributes,
            // unsupported mask) - NO buffer identities. `hash` mixes each bound
            // buffer's never-reused LIFETIME ID, so per-chunk VBOs mint a fresh
            // identity per buffer; keying pipelines on that minted one VkPipeline per
            // chunk section for an identical layout, defeating pipeline reuse and the
            // per-draw memo. Pipelines depend only on the layout, so they key on this
            // instead.
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

        static SizeT GetComponentSize(DataType type);
        // Tightly-packed byte size of one vertex element for this attribute: componentSize * size for
        // normal types, and 4 (one packed word) for the 2_10_10_10 types and GL_BGRA. Returns 0 for
        // an unknown/unsupported type.
        static SizeT GetAttributeByteSize(DataType type, Int size, Bool isBgra);
        // A wire VAO is a layout record plus a separate per-attribute buffer window.
        // No frontend VAO identity, address or memo participates in this layout.
        Bool BuildWireVertexInput(const MG_Pipe::MGPipeVertexElementsRecord& elements,
                                  const MG_Pipe::MGPipeApplierState& state, Uint32 activeMask,
                                  BackendVertexInputState& out) const;

    private:
        static VkFormat ToVkVertexFormat(DataType type, Int size, Bool normalized, Bool isInteger, Bool isBgra = false,
                                         Bool isLong = false);
        static Bool IsScaledIntegerVertexFormat(VkFormat format);
        static VkFormat ToFloat32VertexFormat(Int componentCount);
        Bool SupportsVertexBufferFormat(VkFormat format) const;

        const VulkanRendererConfig& m_config;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        // Physical-device format capabilities never change during this factory's
        // lifetime. The wire path rebuilds layouts without the legacy VAO memo.
        mutable UnorderedMap<VkFormat, Bool> m_wireVertexFormatSupport;
        static inline thread_local XXH64_state_t* m_hashState = XXH64_createState();
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
