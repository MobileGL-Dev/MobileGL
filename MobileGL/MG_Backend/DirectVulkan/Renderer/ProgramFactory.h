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
            StorageImage,
            // GLSL `imageBuffer` - a buffer texture reached through an IMAGE unit rather than a
            // texture unit. Vulkan spells it VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, which is a
            // VkBufferView like UniformTexelBuffer and not a VkImageView like StorageImage: it is
            // the one image uniform whose descriptor is a buffer. Appended, never inserted -
            // DescriptorKeyHash mixes the enumerator's value.
            StorageTexelBuffer
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
            // Replaces the vertex stage's gl_BaseVertex reads with zero. GL defines the builtin
            // as zero for every drawing command that has no baseVertex parameter - all the
            // DrawArrays forms - while Vulkan's BaseVertex reports firstVertex there. Set only
            // for a non-indexed draw whose program actually reads the builtin, so nothing else
            // acquires a second program/pipeline variant. See ZeroBaseVertexPass.
            ZeroBaseVertex = 1 << 8,
        };
        using CompileOptionFlags = Flags<CompileOptionBit>;
        using HashType = Uint64;

        struct UpdateAfterBindLimits {
            Bool enabled = false;
            Uint32 maxPerStageSamplers = 0;
            Uint32 maxPerStageUniformBuffers = 0;
            Uint32 maxPerStageStorageBuffers = 0;
            Uint32 maxPerStageSampledImages = 0;
            Uint32 maxPerStageStorageImages = 0;
            Uint32 maxPerStageResources = 0;
            Uint32 maxSetSamplers = 0;
            Uint32 maxSetUniformBuffers = 0;
            Uint32 maxSetUniformBuffersDynamic = 0;
            Uint32 maxSetStorageBuffers = 0;
            Uint32 maxSetStorageBuffersDynamic = 0;
            Uint32 maxSetSampledImages = 0;
            Uint32 maxSetStorageImages = 0;
        };

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
            // True only when this layout passed every descriptor-indexing feature and
            // update-after-bind limit gate at reflection time. It controls both the
            // layout/binding flags and the pool class used by UniformManager.
            Bool usesUpdateAfterBind = false;
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
            // Shared by StorageImage and StorageTexelBuffer bindings: a binding is one kind or
            // the other, never both, and both need exactly the same thing - the format the
            // shader declared, so the per-draw resolve can tell a typed declaration from a
            // formatless one. Kept as one pair rather than two so the move operations below
            // cannot drift out of sync with a field that only one kind populates.
            Vector<VkFormat> storageImageFormatByBinding;
            Vector<Bool> storageImageUsesBindingFormatByBinding;
            Vector<String> storageBlockNameByBinding;
            Vector<Int> storageBlockIndexByBinding;
            // Set once during ReflectLayout so the per-draw path can skip the whole
            // storage-image preparation for the overwhelming majority of programs.
            Bool hasStorageImages = false;
            // Something about this program's descriptors could not be resolved - an opaque
            // uniform array whose elements have no addressable uniform locations (the
            // multi-dimensional case), or a binding remap that failed outright. The binding
            // STAYS DECLARED in the descriptor set layout; declining is done here, by refusing
            // every draw, and BindProgramUniformBuffers returns false so the draw setup skips
            // the draw exactly as it does for any other bind failure.
            //
            // Keeping the layout intact is the load-bearing half. Shrinking it instead - which
            // is what the first cut of this did - leaves the shader reading a descriptor the
            // layout never declared, and lavapipe segfaults on that inside PIPELINE CREATION,
            // in a JIT worker thread, before any draw runs where a refusal could help. The
            // reason was logged once at MGLOG_I when the descriptor was declined.
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
            // The vertex module declares the BaseVertex builtin. Selects the ZeroBaseVertex
            // program variant for non-indexed draws, and is deliberately a property of the
            // PROGRAM rather than of the variant: the zeroed variant leaves the variable
            // declared, so both variants answer the same and the draw path can ask either.
            Bool readsBaseVertexBuiltin = false;
            // Some pre-rasterization stage assigns gl_ViewportIndex. Its pipeline declares
            // viewportCount = the renderer's rasterizable viewport count instead of 1, and its
            // draws push the whole viewport/scissor array; every other program keeps the
            // single-viewport fast path untouched. Part of the program's identity (folded into
            // the pipeline hash through programHash), so no memo can serve the wrong shape.
            Bool writesViewportIndexBuiltin = false;
            // This program has a tessellation EVALUATION stage and no tessellation CONTROL
            // stage. GL allows that (4.6 core 11.2.2: with no control shader the input patch
            // is passed through unmodified, the output patch size is PATCH_VERTICES, and the
            // levels come from the PATCH_DEFAULT_*_LEVEL state); Vulkan does not - either both
            // tessellation stages are present or neither
            // (VUID-VkGraphicsPipelineCreateInfo-pStages-00730). So the draw path has to supply
            // the pass-through stage GL describes; see GetOrCreatePassthroughTessControlStage.
            Bool needsPassthroughTessControl = false;
            // ...and the pass-through this renderer can synthesize carries gl_Position and
            // nothing else, so it is only correct when the evaluation stage's inputs are
            // built-ins. A user-defined varying would arrive at the evaluation stage
            // UNWRITTEN once a control stage sits between it and the vertex stage, which is
            // silently wrong pixels rather than a crash - so those programs are declined
            // instead (PipelineFactory::CreatePipeline refuses the pipeline and the draw is
            // skipped). See ReflectPassthroughTessControlNeed.
            Bool passthroughTessControlEmulatable = false;
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
                usesUpdateAfterBind = other.usesUpdateAfterBind;
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
                readsBaseVertexBuiltin = other.readsBaseVertexBuiltin;
                writesViewportIndexBuiltin = other.writesViewportIndexBuiltin;
                needsPassthroughTessControl = other.needsPassthroughTessControl;
                passthroughTessControlEmulatable = other.passthroughTessControlEmulatable;
                lastUsedFrame = other.lastUsedFrame;
                other.hash = 0;
                other.descriptorSetLayout = VK_NULL_HANDLE;
                other.usesUpdateAfterBind = false;
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
                other.readsBaseVertexBuiltin = false;
                other.writesViewportIndexBuiltin = false;
                other.needsPassthroughTessControl = false;
                other.passthroughTessControlEmulatable = false;
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
                usesUpdateAfterBind = other.usesUpdateAfterBind;
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
                readsBaseVertexBuiltin = other.readsBaseVertexBuiltin;
                writesViewportIndexBuiltin = other.writesViewportIndexBuiltin;
                needsPassthroughTessControl = other.needsPassthroughTessControl;
                passthroughTessControlEmulatable = other.passthroughTessControlEmulatable;
                lastUsedFrame = other.lastUsedFrame;
                other.hash = 0;
                other.descriptorSetLayout = VK_NULL_HANDLE;
                other.usesUpdateAfterBind = false;
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
                other.readsBaseVertexBuiltin = false;
                other.writesViewportIndexBuiltin = false;
                other.needsPassthroughTessControl = false;
                other.passthroughTessControlEmulatable = false;
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

        // How this factory's compute modules implement GL_KHR_shader_subgroup. Computed
        // once at renderer initialization (SubgroupSupportPolicy.h + the device's
        // subgroup properties) so lowering can never disagree with the advertised
        // capabilities. Native subgroup operations always execute natively; the two
        // repair passes patch modules AROUND them, and the emulation only replaces them
        // on opted-in devices with no subgroup support at all.
        struct SubgroupLoweringPolicy {
            Bool emulateSubgroups = false;      // MOBILEGL_MAGMA_EMULATE_SUBGROUP, no-native-support devices
            Bool fixIterationRPSubgroupScratch = false; // patch iterationRP's under-declared scratch
            Bool fixIterationRPBarrier = false; // repair Program 203's shared-scratch race
            Bool deriveNumSubgroups = false;    // repair the NumSubgroups builtin
            Bool requireFullSubgroups = false;  // computeFullSubgroups enabled on the device
            Uint32 nativeSubgroupSize = 0;
            // Full-subgroup launches are bounded by this device limit; a dispatch whose
            // workgroup needs more subgroups than this cannot request the flag.
            Uint32 maxComputeWorkgroupSubgroups = 0;
            // VkPhysicalDeviceLimits::maxComputeSharedMemorySize; bounds the scratch the
            // emulation pass may add (0 falls back to the Vulkan minimum, 16384).
            Uint32 maxComputeSharedMemoryBytes = 0;
        };

        explicit ProgramFactory(VkDevice device, const VulkanRendererConfig& config, Uint32 maxBindings,
                                Bool shaderDrawParametersEnabled,
                                Bool unformattedFloatStorageImagesEnabled,
                                Bool enableSpirvValidation,
                                UpdateAfterBindLimits updateAfterBindLimits,
                                SubgroupLoweringPolicy subgroupPolicy)
            : m_device(device), m_maxBindings(maxBindings), m_config(config),
              m_shaderDrawParametersEnabled(shaderDrawParametersEnabled),
              m_unformattedFloatStorageImagesEnabled(unformattedFloatStorageImagesEnabled),
              m_enableSpirvValidation(enableSpirvValidation),
              m_updateAfterBindLimits(updateAfterBindLimits),
              m_subgroupPolicy(subgroupPolicy) {
            VkProgramObject::s_device = device;
        }
        // Destroys the pass-through tessellation control modules. Runs while the device is
        // still alive for the same reason ~VkProgramObject's does: this factory outlives
        // nothing that owns the device.
        ~ProgramFactory();
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
        // The same question for an IMAGE uniform (`image2D`, `uimageBuffer`, ...), which the
        // sampler form above deliberately does not answer. Kept separate rather than folded in
        // because the two are asked in different places for different reasons: a sampler's domain
        // decides a sampled VIEW format, an image's decides what a placeholder descriptor for an
        // UNBOUND image unit must be (see UniformManager::AcquireUnboundTexelBufferView and
        // GetUnboundStorageImageTexture) - a formatless `writeonly` declaration reflects no
        // format at all, and the numeric domain is then the only thing that constrains it.
        static SamplerNumericDomain UniformTypeToImageNumericDomain(GLenum glType);
        // True when any entry point declares the DepthReplacing execution mode, i.e. the
        // shader assigns gl_FragDepth. Exposed so the blended depth-write quirk's exemption
        // can be pinned by tests. A false negative loses the exemption, so such a shader is
        // stripped conservatively and forfeits its depth write.
        static Bool ReflectedFragmentReplacesDepth(const SpvReflectShaderModule& reflectModule);
        // True when an entry point reads the InstanceIndex builtin. Only gates a diagnostic:
        // without shaderDrawParameters such a shader cannot have gl_InstanceID rebased.
        static Bool ReflectedReadsInstanceIndexBuiltin(const SpvReflectShaderModule& reflectModule);
        // True when an entry point declares the BaseVertex builtin, i.e. when a non-indexed
        // draw with this program has to take the ZeroBaseVertex variant.
        static Bool ReflectedReadsBaseVertexBuiltin(const SpvReflectShaderModule& reflectModule);
        // Shared by the two above: does any entry point list an input variable decorated with
        // this builtin?
        static Bool ReflectedDeclaresInputBuiltin(const SpvReflectShaderModule& reflectModule, SpvBuiltIn builtin);
        // True when an entry point writes the ViewportIndex builtin (gl_ViewportIndex), i.e. when
        // the program can route primitives to a viewport other than 0 and its pipeline therefore
        // has to declare more than one. Asks about OUTPUT variables because that is the direction
        // a pre-rasterization stage declares it in.
        static Bool ReflectedWritesViewportIndexBuiltin(const SpvReflectShaderModule& reflectModule);
        static Bool ReflectedDeclaresOutputBuiltin(const SpvReflectShaderModule& reflectModule, SpvBuiltIn builtin);

        // The pass-through tessellation control stage GL 4.6 core 11.2.2 describes for a
        // program that has an evaluation stage and no control stage, for an input patch of
        // `patchVertices` control points. Returned BY VALUE (a stage description is a POD, and
        // the cache below is a rehashing map, so a pointer into it would not survive the next
        // distinct patch size). `.module == VK_NULL_HANDLE` means the stage could not be built:
        // the caller then has no control stage to inject, and CreatePipeline refuses the
        // pipeline rather than handing the driver a half-tessellated one.
        //
        // Keyed on the patch size AND the six default tessellation levels, because GL takes the
        // output patch size from PATCH_VERTICES and the levels from PATCH_DEFAULT_OUTER_LEVEL /
        // PATCH_DEFAULT_INNER_LEVEL, all of which are draw state rather than link state - the CTS
        // case that motivated this links at the default 3 and draws at 4. The pipeline cache
        // re-keys on the same three inputs, so the module a pipeline was built with is part of
        // that pipeline's identity. Compiling is bounded by the number of distinct
        // (size, levels) combinations a program draws with - one or two in practice - and only
        // ever happens for the rare program that has no control stage at all.
        VkPipelineShaderStageCreateInfo GetOrCreatePassthroughTessControlStage(Uint32 patchVertices,
                                                                              const FloatVec4& defaultOuterLevel,
                                                                              const FloatVec2& defaultInnerLevel);

        // Source of the module above. Exposed for tests: the generated GLSL is the whole
        // contract with the evaluation stage, so it is worth pinning independently of a device.
        static String BuildPassthroughTessControlSource(Uint32 patchVertices, const FloatVec4& defaultOuterLevel,
                                                        const FloatVec2& defaultInnerLevel);

        // The identity of one such module: everything the generator bakes in, folded into a
        // 64-bit key over the raw bits (so -0.0 and +0.0 key apart, which is harmless, and NaN
        // keys to itself, which is what matters). Shared with PipelineFactory, which mixes the
        // same value into the pipeline hash so a pipeline can never be handed a module built for
        // different levels.
        static Uint64 ComputePassthroughTessControlKey(Uint32 patchVertices, const FloatVec4& defaultOuterLevel,
                                                       const FloatVec2& defaultInnerLevel);

    private:
        struct ProgramLookupCache {
            const MG_State::GLState::ProgramObject* program = nullptr;
            Uint32 backendStateVersion = 0;
            CompileOptionFlags flags{};
            HashType hash = 0;
        };

        static TextureTarget UniformTypeToTextureTarget(GLenum glType);
        // `stages` is ALWAYS ProgramObject::GetLinkedShaderStages() - one entry per module of
        // `spirv`, at the same index. Taking the stages rather than the shader objects is what
        // keeps the program's live attach list, which is a longer and differently-indexed list
        // the moment a glAttachShader lands after the link, from being passed here by mistake.
        void ReflectVertexInputs(const Vector<ShaderStage>& stages,
                     const Vector<Vector<Uint>>& spirv,
                     VkProgramObject& entry) const;
        void ReflectViewportIndexUsage(const Vector<ShaderStage>& stages,
                                       const Vector<Vector<Uint>>& spirv,
                                       VkProgramObject& entry) const;
        void ReflectFragmentOutputs(const Vector<ShaderStage>& stages,
                        const Vector<Vector<Uint>>& spirv,
                        VkProgramObject& entry) const;
        void ReflectLayout(const MG_State::GLState::ProgramObject& program, const Vector<Vector<Uint>>& spirv,
                           VkProgramObject& entry) const;
        // Fills needsPassthroughTessControl / passthroughTessControlEmulatable off the linked
        // modules. Const and reflection-only: it decides nothing about the pipeline, it only
        // records what the evaluation stage's input interface is made of.
        void ReflectPassthroughTessControlNeed(const Vector<ShaderStage>& stages,
                                               const Vector<Vector<Uint>>& spirv,
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
        // Startup snapshot used only by internally synthesized shader modules, which do not
        // originate from a ProgramLinkTask.
        Bool m_enableSpirvValidation = false;
        // Device feature and limit gate resolved before vkCreateDevice. Keeping it in
        // the factory lets each reflected layout choose ordinary descriptors when its
        // own counts would exceed the update-after-bind budget.
        UpdateAfterBindLimits m_updateAfterBindLimits{};
        SubgroupLoweringPolicy m_subgroupPolicy{};
        // See SetDefaultFramebufferHeight. 0 means "not known yet"; the FragCoordYFlip bit is
        // never set before the swapchain exists, so no variant can be compiled against it.
        Uint32 m_defaultFramebufferHeight = 0;
        mutable ProgramLookupCache m_lastLookup;
        // Monotonic frame-boundary counter (bumped in OnFrameBoundary) for cache aging.
        Uint64 m_frameCounter = 0;
        // See GetCacheStructureEpoch(). Starts at 1 so a zero-initialized memo can never match.
        Uint64 m_cacheStructureEpoch = 1;
        IEvictionObserver* m_evictionObserver = nullptr;
        // Pass-through tessellation control stages by the identity of what was compiled into
        // them - the input patch size and the six default tessellation levels, folded into one
        // 64-bit key by ComputePassthroughTessControlKey (the levels are float state, so the map
        // cannot simply be keyed on the patch size any more). A failed build is cached as
        // VK_NULL_HANDLE so a broken generator costs one compile, not one per draw.
        //
        // Hard-capped, because the key is application-controlled: glPatchParameterfv clamps
        // nothing, so an application that recomputes a level per frame mints a new key per frame.
        // Reaching the cap destroys every module and starts over (see the flush in
        // GetOrCreatePassthroughTessControlStage); the cap is far above what any program that
        // holds its levels still will ever need.
        static constexpr SizeT kMaxPassthroughTessControlStages = 64;
        UnorderedMap<Uint64, VkPipelineShaderStageCreateInfo> m_passthroughTessControlStages;
        static inline XXH64_state_t* m_hashState = XXH64_createState();
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
