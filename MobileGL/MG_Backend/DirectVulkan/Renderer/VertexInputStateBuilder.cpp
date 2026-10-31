// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateBuilder.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VertexInputStateBuilder.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    VertexInputStateBuilder::VertexInputStateBuilder() {
        Reset();
    }

    void VertexInputStateBuilder::Reset() {
        m_bindings.clear();
        m_attributes.clear();

        m_state = {};
        m_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        m_state.vertexBindingDescriptionCount = 0;
        m_state.pVertexBindingDescriptions = nullptr;
        m_state.vertexAttributeDescriptionCount = 0;
        m_state.pVertexAttributeDescriptions = nullptr;
    }

    VertexInputStateBuilder& VertexInputStateBuilder::AddBinding(Uint32 binding, Uint32 stride,
                                                                 VkVertexInputRate inputRate) {
        VkVertexInputBindingDescription desc{};
        desc.binding = binding;
        desc.stride = stride;
        desc.inputRate = inputRate;
        m_bindings.push_back(desc);
        return *this;
    }

    VertexInputStateBuilder& VertexInputStateBuilder::AddAttribute(Uint32 location, Uint32 binding, VkFormat format,
                                                                   Uint32 offset) {
        VkVertexInputAttributeDescription desc{};
        desc.location = location;
        desc.binding = binding;
        desc.format = format;
        desc.offset = offset;
        m_attributes.push_back(desc);
        return *this;
    }

    const VkPipelineVertexInputStateCreateInfo& VertexInputStateBuilder::Build() {
        m_state.vertexBindingDescriptionCount = static_cast<Uint32>(m_bindings.size());
        m_state.pVertexBindingDescriptions = m_bindings.empty() ? nullptr : m_bindings.data();
        m_state.vertexAttributeDescriptionCount = static_cast<Uint32>(m_attributes.size());
        m_state.pVertexAttributeDescriptions = m_attributes.empty() ? nullptr : m_attributes.data();
        return m_state;
    }

    const Vector<VkVertexInputBindingDescription>& VertexInputStateBuilder::GetBindings() const {
        return m_bindings;
    }

    const Vector<VkVertexInputAttributeDescription>& VertexInputStateBuilder::GetAttributes() const {
        return m_attributes;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
