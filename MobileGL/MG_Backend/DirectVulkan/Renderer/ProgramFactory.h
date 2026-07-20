// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/ProgramFactory.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "../VkIncludes.h"
#include "MG_State/GLState/ProgramState/ProgramObject.h"
#include "MG_State/GLState/ProgramState/ShaderObject.h"
#include "MG_State/GLState/TextureState/TextureEnum.h"

#include <Includes.h>
#include <spirv_reflect.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    enum class SamplerNumericDomain : Uint8 {
        Unknown = 0,
        Float,
        SignedInteger,
        UnsignedInteger,
    };

    class ProgramFactory {
    public:
        enum class DescriptorBindingKind : Uint8 {
            None = 0,
            UniformBufferDynamic,
            CombinedImageSampler,
            UniformTexelBuffer,
            StorageBuffer,
            StorageImage
        };

        enum class CompileOptionBit : Uint {
            None = 0,
            PositionYFlip = 1 << 0,
            PositionZRemap = 1 << 1,
            SurfaceRotate90 = 1 << 2,
            SurfaceRotate180 = 1 << 3,
            SurfaceRotate270 = 1 << 4,
        };
        using CompileOptionFlags = Flags<CompileOptionBit>;
        using HashType = Uint64;

        struct VkProgramObject {
            static constexpr Uint32 kMaxVertexInputLocations = 32;

            HashType hash = 0;
            Vector<VkPipelineShaderStageCreateInfo> stages;
            Vector<VkShaderModule> modules;

            // Layout data (previously in separate VkProgramLayout)
            VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            Vector<DescriptorBindingKind> bindingKinds;
            Vector<Uint32> dynamicBindings;
            Vector<Int> uniformBlockIndexByBinding;
            Vector<String> samplerNameByBinding;
            Vector<Int> samplerUniformLocationByBinding;
            Vector<TextureTarget> samplerTextureTargetByBinding;
            Vector<SamplerNumericDomain> samplerNumericDomainByBinding;
            Vector<VkFormat> storageImageFormatByBinding;
            Vector<Bool> storageImageUsesBindingFormatByBinding;
            Vector<String> storageBlockNameByBinding;
            Vector<Int> storageBlockIndexByBinding;
            // Set once during ReflectLayout so the per-draw path can skip the whole
            // storage-image preparation for the overwhelming majority of programs.
            Bool hasStorageImages = false;
            Int globalUboBinding = -1;
            Uint32 activeVertexInputLocationMask = 0;
            Array<GLenum, kMaxVertexInputLocations> vertexInputTypes{};
            Uint32 activeFragmentOutputLocationMask = 0;
            Array<GLenum, kMaxVertexInputLocations> fragmentOutputTypes{};
            ShaderStage rasterizationProducerStage = ShaderStage::Unknown;
            Uint32 producerOutputComponentCount = 0;
            Uint32 fragmentInputComponentCount = 0;

            static inline VkDevice s_device = VK_NULL_HANDLE;

            VkProgramObject() = default;
            VkProgramObject(const VkProgramObject&) = delete;
            VkProgramObject& operator=(const VkProgramObject&) = delete;
            VkProgramObject(VkProgramObject&& other) noexcept {
                hash = other.hash;
                stages = std::move(other.stages);
                modules = std::move(other.modules);
                descriptorSetLayout = other.descriptorSetLayout;
                pipelineLayout = other.pipelineLayout;
                bindingKinds = std::move(other.bindingKinds);
                dynamicBindings = std::move(other.dynamicBindings);
                uniformBlockIndexByBinding = std::move(other.uniformBlockIndexByBinding);
                samplerNameByBinding = std::move(other.samplerNameByBinding);
                samplerUniformLocationByBinding = std::move(other.samplerUniformLocationByBinding);
                samplerTextureTargetByBinding = std::move(other.samplerTextureTargetByBinding);
                samplerNumericDomainByBinding = std::move(other.samplerNumericDomainByBinding);
                storageImageFormatByBinding = std::move(other.storageImageFormatByBinding);
                storageImageUsesBindingFormatByBinding =
                    std::move(other.storageImageUsesBindingFormatByBinding);
                storageBlockNameByBinding = std::move(other.storageBlockNameByBinding);
                storageBlockIndexByBinding = std::move(other.storageBlockIndexByBinding);
                hasStorageImages = other.hasStorageImages;
                globalUboBinding = other.globalUboBinding;
                activeVertexInputLocationMask = other.activeVertexInputLocationMask;
                vertexInputTypes = other.vertexInputTypes;
                activeFragmentOutputLocationMask = other.activeFragmentOutputLocationMask;
                fragmentOutputTypes = other.fragmentOutputTypes;
                rasterizationProducerStage = other.rasterizationProducerStage;
                producerOutputComponentCount = other.producerOutputComponentCount;
                fragmentInputComponentCount = other.fragmentInputComponentCount;
                other.hash = 0;
                other.descriptorSetLayout = VK_NULL_HANDLE;
                other.pipelineLayout = VK_NULL_HANDLE;
                other.hasStorageImages = false;
                other.globalUboBinding = -1;
                other.activeVertexInputLocationMask = 0;
                other.activeFragmentOutputLocationMask = 0;
                other.rasterizationProducerStage = ShaderStage::Unknown;
                other.producerOutputComponentCount = 0;
                other.fragmentInputComponentCount = 0;
            }
            VkProgramObject& operator=(VkProgramObject&& other) noexcept {
                if (this == &other) {
                    return *this;
                }
                Destroy();
                hash = other.hash;
                stages = std::move(other.stages);
                modules = std::move(other.modules);
                descriptorSetLayout = other.descriptorSetLayout;
                pipelineLayout = other.pipelineLayout;
                bindingKinds = std::move(other.bindingKinds);
                dynamicBindings = std::move(other.dynamicBindings);
                uniformBlockIndexByBinding = std::move(other.uniformBlockIndexByBinding);
                samplerNameByBinding = std::move(other.samplerNameByBinding);
                samplerUniformLocationByBinding = std::move(other.samplerUniformLocationByBinding);
                samplerTextureTargetByBinding = std::move(other.samplerTextureTargetByBinding);
                samplerNumericDomainByBinding = std::move(other.samplerNumericDomainByBinding);
                storageImageFormatByBinding = std::move(other.storageImageFormatByBinding);
                storageImageUsesBindingFormatByBinding =
                    std::move(other.storageImageUsesBindingFormatByBinding);
                storageBlockNameByBinding = std::move(other.storageBlockNameByBinding);
                storageBlockIndexByBinding = std::move(other.storageBlockIndexByBinding);
                hasStorageImages = other.hasStorageImages;
                globalUboBinding = other.globalUboBinding;
                activeVertexInputLocationMask = other.activeVertexInputLocationMask;
                vertexInputTypes = other.vertexInputTypes;
                activeFragmentOutputLocationMask = other.activeFragmentOutputLocationMask;
                fragmentOutputTypes = other.fragmentOutputTypes;
                rasterizationProducerStage = other.rasterizationProducerStage;
                producerOutputComponentCount = other.producerOutputComponentCount;
                fragmentInputComponentCount = other.fragmentInputComponentCount;
                other.hash = 0;
                other.descriptorSetLayout = VK_NULL_HANDLE;
                other.pipelineLayout = VK_NULL_HANDLE;
                other.hasStorageImages = false;
                other.globalUboBinding = -1;
                other.activeVertexInputLocationMask = 0;
                other.activeFragmentOutputLocationMask = 0;
                other.rasterizationProducerStage = ShaderStage::Unknown;
                other.producerOutputComponentCount = 0;
                other.fragmentInputComponentCount = 0;
                return *this;
            }

            ~VkProgramObject() {
                Destroy();
            }

        private:
            void Destroy() {
                if (s_device != VK_NULL_HANDLE) {
                    if (pipelineLayout != VK_NULL_HANDLE) {
                        vkDestroyPipelineLayout(s_device, pipelineLayout, nullptr);
                        pipelineLayout = VK_NULL_HANDLE;
                    }
                    if (descriptorSetLayout != VK_NULL_HANDLE) {
                        vkDestroyDescriptorSetLayout(s_device, descriptorSetLayout, nullptr);
                        descriptorSetLayout = VK_NULL_HANDLE;
                    }
                    for (auto module : modules) {
                        if (module != VK_NULL_HANDLE) {
                            vkDestroyShaderModule(s_device, module, nullptr);
                        }
                    }
                }
                modules.clear();
                stages.clear();
            }
        };

        explicit ProgramFactory(VkDevice device, const VulkanRendererConfig& config, Uint32 maxBindings = 16,
                                Bool shaderDrawParametersEnabled = false,
                                Bool unformattedFloatStorageImagesEnabled = false)
            : m_device(device), m_maxBindings(maxBindings), m_config(config),
              m_shaderDrawParametersEnabled(shaderDrawParametersEnabled),
              m_unformattedFloatStorageImagesEnabled(unformattedFloatStorageImagesEnabled) {
            VkProgramObject::s_device = device;
        }
        ~ProgramFactory() = default;
        ProgramFactory(const ProgramFactory&) = delete;

        HashType ComputeHash(const MG_State::GLState::ProgramObject& program, CompileOptionFlags flags) const;
        const VkProgramObject& GetOrCreateProgram(
            const MG_State::GLState::ProgramObject& program, CompileOptionFlags flags);

        static VkShaderStageFlagBits ToVkStage(ShaderStage stage);
        static VkFormat ConvertSpirvImageFormatToVkFormat(SpvImageFormat format);
        static SamplerNumericDomain UniformTypeToSamplerNumericDomain(GLenum glType);

    private:
        struct ProgramLookupCache {
            const MG_State::GLState::ProgramObject* program = nullptr;
            Uint32 backendStateVersion = 0;
            CompileOptionFlags flags{};
            HashType hash = 0;
        };

        static TextureTarget UniformTypeToTextureTarget(GLenum glType);
        void ReflectVertexInputs(const Vector<SharedPtr<MG_State::GLState::ShaderObject>>& shaders,
                     const Vector<Vector<Uint>>& spirv,
                     VkProgramObject& entry) const;
        void ReflectFragmentOutputs(const Vector<SharedPtr<MG_State::GLState::ShaderObject>>& shaders,
                        const Vector<Vector<Uint>>& spirv,
                        VkProgramObject& entry) const;
        void ReflectLayout(const MG_State::GLState::ProgramObject& program, const Vector<Vector<Uint>>& spirv,
                           VkProgramObject& entry) const;

        VkDevice m_device = VK_NULL_HANDLE;
        Uint32 m_maxBindings = 0;
        UnorderedMap<HashType, VkProgramObject> m_cache;
        const VulkanRendererConfig& m_config;
        // True when the device enabled shaderDrawParameters; gates the InstanceIndex rebase pass
        // (which needs the DrawParameters capability / gl_BaseInstance builtin).
        Bool m_shaderDrawParametersEnabled = false;
        // True only when the logical device enabled both
        // shaderStorageImageReadWithoutFormat and shaderStorageImageWriteWithoutFormat.
        Bool m_unformattedFloatStorageImagesEnabled = false;
        mutable ProgramLookupCache m_lastLookup;
        static inline XXH64_state_t* m_hashState = XXH64_createState();
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
