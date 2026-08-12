// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/ProgramFactory.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "../VkIncludes.h"
#include "PipelineFactory.h"
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
            // Rewrites the fragment stage's implicit-LOD image samples to explicit LOD 0.
            // Only ever set for a draw whose every sampler binding is clamped to a single mip
            // level, which makes the two forms produce identical texels (the implicit lambda is
            // clamped into [minLod, maxLod] = [0, 0] regardless of derivatives or bias).
            ExplicitLod0Sampling = 1 << 5,
            // Decorates the last vertex-processing stage's captured varyings with
            // XfbBuffer/XfbStride/Offset (VK_EXT_transform_feedback). Set only for draws
            // recorded while GL transform feedback is active, so plain draws keep the
            // undecorated variant.
            XfbCapture = 1 << 6,
            // Rewrites the fragment stage's gl_FragCoord reads to GL's bottom-left window
            // origin. Vulkan's gl_FragCoord.y IS the framebuffer row being written, and the
            // default framebuffer's image is stored in display (top-left) order, so a shader
            // that reads gl_FragCoord there sees `height - y_GL`. Set together with
            // PositionYFlip (the two are the same fact about the same draws) except under a
            // quarter turn, which this renderer does not convert rectangles for either.
            FragCoordYFlip = 1 << 7,
        };
        using CompileOptionFlags = Flags<CompileOptionBit>;
        using HashType = Uint64;

        struct VkProgramObject {
            static constexpr Uint32 kMaxVertexInputLocations = 32;

            HashType hash = 0;
            Vector<VkPipelineShaderStageCreateInfo> stages;
            Vector<VkShaderModule> modules;
            // Parallel to stages; identifies the exact module bytes handed to the driver when a
            // pipeline creation fails. Sixteen bytes per stage instead of keeping the SPIR-V.
            Vector<ShaderStageSpirvDigest> stageSpirvDigests;

            // Layout data (previously in separate VkProgramLayout)
            VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            Vector<DescriptorBindingKind> bindingKinds;
            // The bindings this program actually declares, ascending. bindingKinds is sized to the
            // 256-binding cap while a real GL program uses 1-8, so the per-draw descriptor walk was
            // scanning 256 slots to find a handful. MUST stay ascending: Vulkan consumes
            // pDynamicOffsets in binding order and the writer pushes them in iteration order, so an
            // unordered list would silently mis-pair dynamic offsets with their uniform blocks.
            Vector<Uint32> activeBindings;
            Vector<Uint32> dynamicBindings;
            Vector<Int> uniformBlockIndexByBinding;
            // Descriptor count per binding (1 except for a descriptor ARRAY - a UBO or storage
            // block instance array, an image uniform array or a sampler uniform array - each of
            // which occupies one binding with descriptorCount = N).
            Vector<Uint16> bindingDescriptorCounts;
            // Per-element GL uniform block indices for arrayed UBO bindings (count > 1);
            // element 0 of a non-arrayed binding stays in uniformBlockIndexByBinding.
            UnorderedMap<Uint32, Vector<Int>> arrayedUniformBlockIndicesByBinding;
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
            // ReflectLayout found a descriptor it cannot describe and dropped it from the
            // layout. That leaves a layout the shader disagrees with, so this program must
            // never reach a draw: BindProgramUniformBuffers refuses outright, and the draw
            // setup skips the draw exactly as it does for any other bind failure. Dropping the
            // binding WITHOUT refusing the draw is what a shader reading an undeclared
            // descriptor looks like, and lavapipe segfaults inside the JIT-ed shader on it.
            // The reason was logged at MGLOG_I when the binding was declined.
            Bool declinedDescriptors = false;
            Int globalUboBinding = -1;
            Uint32 activeVertexInputLocationMask = 0;
            Array<GLenum, kMaxVertexInputLocations> vertexInputTypes{};
            Uint32 activeFragmentOutputLocationMask = 0;
            Array<GLenum, kMaxVertexInputLocations> fragmentOutputTypes{};
            ShaderStage rasterizationProducerStage = ShaderStage::Unknown;
            Uint32 producerOutputComponentCount = 0;
            Uint32 fragmentInputComponentCount = 0;
            // The fragment module declares the DepthReplacing execution mode (writes
            // gl_FragDepth); shader-computed depth is immune to the cross-pipeline
            // position-invariance quirk (see PipelineFactory::ShouldSuppressDepthWrite).
            Bool fragmentReplacesDepth = false;
            // Frame-boundary counter value of the last GetOrCreateProgram hit; drives
            // cache eviction (see OnFrameBoundary). Mutable: the draw snapshot's memoised
            // entry pointer re-stamps use through a const reference (StampProgramUse).
            mutable Uint64 lastUsedFrame = 0;

            static inline VkDevice s_device = VK_NULL_HANDLE;

            VkProgramObject() = default;
            VkProgramObject(const VkProgramObject&) = delete;
            VkProgramObject& operator=(const VkProgramObject&) = delete;
            VkProgramObject(VkProgramObject&& other) noexcept {
                hash = other.hash;
                stages = std::move(other.stages);
                modules = std::move(other.modules);
                // Must travel with `modules`: these digests name the SPIR-V those exact
                // shader modules were built from, and the pipeline-failure diagnostics
                // print the two together. Leaving it behind used to merely lose the
                // digests on a rehash; now that the cache is a robin-hood table, insertion
                // SWAPS two entries, and a field that no move touches stays behind in the
                // slot - pairing one program's modules with another program's digests, so
                // a pipeline failure would be reported against the wrong SPIR-V.
                stageSpirvDigests = std::move(other.stageSpirvDigests);
                descriptorSetLayout = other.descriptorSetLayout;
                pipelineLayout = other.pipelineLayout;
                bindingKinds = std::move(other.bindingKinds);
                activeBindings = std::move(other.activeBindings);
                dynamicBindings = std::move(other.dynamicBindings);
                uniformBlockIndexByBinding = std::move(other.uniformBlockIndexByBinding);
                bindingDescriptorCounts = std::move(other.bindingDescriptorCounts);
                arrayedUniformBlockIndicesByBinding = std::move(other.arrayedUniformBlockIndicesByBinding);
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
                declinedDescriptors = other.declinedDescriptors;
                globalUboBinding = other.globalUboBinding;
                activeVertexInputLocationMask = other.activeVertexInputLocationMask;
                vertexInputTypes = other.vertexInputTypes;
                activeFragmentOutputLocationMask = other.activeFragmentOutputLocationMask;
                fragmentOutputTypes = other.fragmentOutputTypes;
                rasterizationProducerStage = other.rasterizationProducerStage;
                producerOutputComponentCount = other.producerOutputComponentCount;
                fragmentInputComponentCount = other.fragmentInputComponentCount;
                fragmentReplacesDepth = other.fragmentReplacesDepth;
                lastUsedFrame = other.lastUsedFrame;
                other.hash = 0;
                other.descriptorSetLayout = VK_NULL_HANDLE;
                other.pipelineLayout = VK_NULL_HANDLE;
                other.hasStorageImages = false;
                other.declinedDescriptors = false;
                other.globalUboBinding = -1;
                other.activeVertexInputLocationMask = 0;
                other.activeFragmentOutputLocationMask = 0;
                other.rasterizationProducerStage = ShaderStage::Unknown;
                other.producerOutputComponentCount = 0;
                other.fragmentInputComponentCount = 0;
                other.fragmentReplacesDepth = false;
                other.lastUsedFrame = 0;
            }
            VkProgramObject& operator=(VkProgramObject&& other) noexcept {
                if (this == &other) {
                    return *this;
                }
                Destroy();
                hash = other.hash;
                stages = std::move(other.stages);
                modules = std::move(other.modules);
                stageSpirvDigests = std::move(other.stageSpirvDigests); // travels with `modules` - see the move ctor
                descriptorSetLayout = other.descriptorSetLayout;
                pipelineLayout = other.pipelineLayout;
                bindingKinds = std::move(other.bindingKinds);
                activeBindings = std::move(other.activeBindings);
                dynamicBindings = std::move(other.dynamicBindings);
                uniformBlockIndexByBinding = std::move(other.uniformBlockIndexByBinding);
                bindingDescriptorCounts = std::move(other.bindingDescriptorCounts);
                arrayedUniformBlockIndicesByBinding = std::move(other.arrayedUniformBlockIndicesByBinding);
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
                declinedDescriptors = other.declinedDescriptors;
                globalUboBinding = other.globalUboBinding;
                activeVertexInputLocationMask = other.activeVertexInputLocationMask;
                vertexInputTypes = other.vertexInputTypes;
                activeFragmentOutputLocationMask = other.activeFragmentOutputLocationMask;
                fragmentOutputTypes = other.fragmentOutputTypes;
                rasterizationProducerStage = other.rasterizationProducerStage;
                producerOutputComponentCount = other.producerOutputComponentCount;
                fragmentInputComponentCount = other.fragmentInputComponentCount;
                fragmentReplacesDepth = other.fragmentReplacesDepth;
                lastUsedFrame = other.lastUsedFrame;
                other.hash = 0;
                other.descriptorSetLayout = VK_NULL_HANDLE;
                other.pipelineLayout = VK_NULL_HANDLE;
                other.hasStorageImages = false;
                other.declinedDescriptors = false;
                other.globalUboBinding = -1;
                other.activeVertexInputLocationMask = 0;
                other.activeFragmentOutputLocationMask = 0;
                other.rasterizationProducerStage = ShaderStage::Unknown;
                other.producerOutputComponentCount = 0;
                other.fragmentInputComponentCount = 0;
                other.fragmentReplacesDepth = false;
                other.lastUsedFrame = 0;
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
                stageSpirvDigests.clear(); // the modules they describe are gone
            }
        };

        // Notified when the OnFrameBoundary sweep destroys an aged-out cache entry,
        // carrying the entry's content hash and the VkDescriptorSetLayout it owned.
        // Dependent caches (compute pipelines, PipelineFactory entries, UniformManager's
        // per-layout descriptor sets) must purge in the same step: after vkDestroy the
        // layout handle value may be recycled for an unrelated layout, and the program
        // hash may be re-inserted by a later rebuild of the same content.
        class IEvictionObserver {
        public:
            virtual ~IEvictionObserver() = default;
            virtual void OnProgramEvicted(HashType programHash, VkDescriptorSetLayout descriptorSetLayout) = 0;
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

        // The default framebuffer's current image height, baked as a literal into every
        // FragCoordYFlip variant (there is no push-constant or specialization channel here, and
        // adding one for a value that changes only on swapchain recreation would cost the draw
        // path more than a recompile costs a resize). It is therefore part of those variants'
        // identity: ComputeHash mixes it in when the bit is set, so a height change re-keys them
        // and leaves every other program's hash untouched. Setting a NEW height also bumps the
        // cache-structure epoch, because a caller holding a memoised VkProgramObject* would
        // otherwise keep using a module compiled against the old height.
        void SetDefaultFramebufferHeight(Uint32 height);
        Uint32 GetDefaultFramebufferHeight() const { return m_defaultFramebufferHeight; }

        // Bumped whenever m_cache's STRUCTURE changes (any insert or erase): the cache is
        // an open-addressing map holding entries by value, so both moves existing entries.
        // A caller that memoised a VkProgramObject* may keep dereferencing it only while
        // this is unchanged; on a bump it must re-run GetOrCreateProgram.
        Uint64 GetCacheStructureEpoch() const { return m_cacheStructureEpoch; }
        // A memoised entry pointer bypasses GetOrCreateProgram, whose per-lookup stamp is
        // what keeps an in-use entry out of OnFrameBoundary's idle sweep - so such a
        // caller must re-stamp the entry itself, at least once per frame boundary.
        void StampProgramUse(const VkProgramObject& entry) const { entry.lastUsedFrame = m_frameCounter; }

        // Observer may be null (no notifications). Not owned.
        void SetEvictionObserver(IEvictionObserver* observer) { m_evictionObserver = observer; }
        // Frame boundary hook: ages the program cache and evicts long-unused entries
        // (their command buffers retired many frames ago), mirroring
        // VkRenderPassManager::OnPresent's sweep.
        void OnFrameBoundary();

        static VkShaderStageFlagBits ToVkStage(ShaderStage stage);
        static VkFormat ConvertSpirvImageFormatToVkFormat(SpvImageFormat format);
        static SamplerNumericDomain UniformTypeToSamplerNumericDomain(GLenum glType);
        // True when any entry point declares the DepthReplacing execution mode, i.e. the
        // shader assigns gl_FragDepth. Exposed so the blended depth-write quirk's exemption
        // can be pinned by tests. A false negative loses the exemption, so such a shader is
        // stripped conservatively and forfeits its depth write.
        static Bool ReflectedFragmentReplacesDepth(const SpvReflectShaderModule& reflectModule);
        // True when an entry point reads the InstanceIndex builtin. Only gates a diagnostic:
        // without shaderDrawParameters such a shader cannot have gl_InstanceID rebased.
        static Bool ReflectedReadsInstanceIndexBuiltin(const SpvReflectShaderModule& reflectModule);

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
        // See SetDefaultFramebufferHeight. 0 means "not known yet"; the FragCoordYFlip bit is
        // never set before the swapchain exists, so no variant can be compiled against it.
        Uint32 m_defaultFramebufferHeight = 0;
        mutable ProgramLookupCache m_lastLookup;
        // Monotonic frame-boundary counter (bumped in OnFrameBoundary) for cache aging.
        Uint64 m_frameCounter = 0;
        // See GetCacheStructureEpoch(). Starts at 1 so a zero-initialized memo can never match.
        Uint64 m_cacheStructureEpoch = 1;
        IEvictionObserver* m_evictionObserver = nullptr;
        static inline XXH64_state_t* m_hashState = XXH64_createState();
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
