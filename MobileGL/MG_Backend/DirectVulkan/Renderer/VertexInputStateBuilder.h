// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateBuilder.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    class VertexInputStateBuilder {
    public:
        VertexInputStateBuilder();

        void Reset();
        VertexInputStateBuilder& AddBinding(Uint32 binding, Uint32 stride,
                                            VkVertexInputRate inputRate = VK_VERTEX_INPUT_RATE_VERTEX);
        VertexInputStateBuilder& AddAttribute(Uint32 location, Uint32 binding, VkFormat format, Uint32 offset);

        const VkPipelineVertexInputStateCreateInfo& Build();
        const Vector<VkVertexInputBindingDescription>& GetBindings() const;
        const Vector<VkVertexInputAttributeDescription>& GetAttributes() const;

    private:
        VkPipelineVertexInputStateCreateInfo m_state{};
        Vector<VkVertexInputBindingDescription> m_bindings;
        Vector<VkVertexInputAttributeDescription> m_attributes;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
