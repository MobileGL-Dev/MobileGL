// MobileGL - MobileGL/MG_Impl/GLImpl/Texture/GL_Texture.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Texture.h"
#include "Config.h"
#include "MG_State/GLState/TextureState/TextureObject2D.h"
#include "MG_Util/Types.h"
#include "Validators.h"
#include "ProxyTexture.h"

#include <MG_State/GLState/Core.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_Util/Metrics/TextureMetrics.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Util/Texture/PixelStoreProcessor.h>
#include <MG_Util/Texture/TextureFormatProcessor.h>
#include <MG_Util/Classifiers/TextureEnumClassifier.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_Impl/GLImpl/Framebuffer/Validators.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>

namespace MobileGL::MG_Impl::GLImpl {
    static SharedPtr<MG_State::GLState::ITextureObject> nullTextureObject;
    static UnorderedMap<Uint, Bool> g_autoGenerateMipmapByTextureId;

    Bool GetTexParameteriv_State(GLenum target, GLenum pname, GLint* params);

    namespace {
        void SetTextureBorderColorFromFloats(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                             const GLfloat* params) {
            textureObject->SetBorderColor(FloatVec4(params[0], params[1], params[2], params[3]));
        }

        void SetTextureBorderColorFromInts(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                           const GLint* params) {
            constexpr Float kSignedIntToFloat = 1.0f / 2147483647.0f;
            textureObject->SetBorderColor(FloatVec4(static_cast<Float>(params[0]) * kSignedIntToFloat,
                                                    static_cast<Float>(params[1]) * kSignedIntToFloat,
                                                    static_cast<Float>(params[2]) * kSignedIntToFloat,
                                                    static_cast<Float>(params[3]) * kSignedIntToFloat));
        }

        void SetTextureBorderColorFromIntegerInts(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                  const GLint* params) {
            textureObject->SetBorderColorI(IntVec4(params[0], params[1], params[2], params[3]));
        }

        void SetTextureBorderColorFromUnsignedInts(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                   const GLuint* params) {
            textureObject->SetBorderColorUI(UintVec4(params[0], params[1], params[2], params[3]));
        }

        Bool SetTextureSwizzleParamsFromInts(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                             const GLint* params, const char* caller) {
            Vec4<TextureSwizzleParam> swizzleParams;
            for (int i = 0; i < 4; ++i) {
                swizzleParams[i] = MG_Util::ConvertGLEnumToTextureSwizzleParam(params[i]);
                if (TextureSwizzleParam::Unknown == swizzleParams[i]) {
                    MG_State::pGLContext->RecordError(
                        ErrorCode::InvalidEnum,
                        MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "`params` is not valid."));
                    return false;
                }
            }
            textureObject->SetSwizzleParamRGBA(swizzleParams);
            return true;
        }

        Bool ValidateMaxAnisotropy(Float maxAnisotropy, const char* caller) {
            if (maxAnisotropy >= 1.0f) return true;

            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "GL_TEXTURE_MAX_ANISOTROPY_EXT must be at least 1.0."));
            return false;
        }

        // DSA emulation: the by-name entry points are implemented by putting the named texture
        // on the active unit's slot for their target, running the classic bound-texture code,
        // then putting the previous binding back.
        //
        // Both of those binds are REAL changes to "which texture is bound at this unit" for as
        // long as `fn` runs, so both have to move the texture bind generation. Backends memoise
        // per-unit work keyed on that generation and BORROW the binding slot (they hold a
        // pointer to the slot's shared_ptr, not a copy); a slot swap the generation never saw
        // let such a memo replay texture A's backend twin against texture B now sitting in the
        // slot - which re-specified A's backend storage with B's shape and silently destroyed
        // A's GPU-rendered contents (Minecraft's lightmap, blanked by a by-name upload to an
        // Iris shadow map, which then discarded every glyph).
        //
        // The generation is bumped directly rather than through NoteTextureUnitTouched because
        // the touched-unit HIGH-WATER MARK must NOT move: glActiveTexture does not advance it,
        // so a DSA-only app would otherwise have every later draw walk up to the highest unit it
        // ever aimed a by-name call at. Not advancing it is also sufficient - a unit above the
        // mark is outside every memo's coverage and outside the epoch walk, so nothing can
        // observe the transient swap there; at or below it, the bump is exactly what makes the
        // epoch re-derive. Bumping only on a real change keeps the very common redundant case (a
        // by-name call on the texture already bound to the active unit) free.
        //
        // The restore is a scope guard because `fn` can throw (the unsupported-state paths use
        // THROW_EXCEPTION): leaking the temporary binding would leave the wrong texture bound to
        // a live unit for the rest of the context's life.
        template <typename Fn>
        void WithTemporarilyBoundNamedTexture(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                             Fn&& fn) {
            if (!textureObject) return;

            const Int activeUnitIndex = MG_State::pGLContext->GetActiveTextureUnit();
            auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(activeUnitIndex);
            auto& bindingSlot = activeUnit.GetBindingSlot(textureObject->GetTarget());
            const auto previousBinding = bindingSlot.GetBoundObject();

            using SlotType = std::remove_reference_t<decltype(bindingSlot)>;
            class ScopedSlotRestore {
            public:
                ScopedSlotRestore(SlotType& slot, SharedPtr<MG_State::GLState::ITextureObject> previous)
                    : m_slot(slot), m_previous(Move(previous)) {}
                ~ScopedSlotRestore() {
                    if (m_slot.Bind(m_previous)) {
                        MG_State::pGLContext->BumpTextureBindGeneration();
                    }
                }
                ScopedSlotRestore(const ScopedSlotRestore&) = delete;
                ScopedSlotRestore& operator=(const ScopedSlotRestore&) = delete;

            private:
                SlotType& m_slot;
                SharedPtr<MG_State::GLState::ITextureObject> m_previous;
            };

            if (bindingSlot.Bind(textureObject)) {
                MG_State::pGLContext->BumpTextureBindGeneration();
            }
            ScopedSlotRestore restore(bindingSlot, previousBinding);

            fn(MG_Util::ConvertTextureTargetToGLEnum(textureObject->GetTarget()));
        }

        SizeT ComputeTextureStorageByteSize(TextureInternalFormat textureInternalFormat, GLsizei width, GLsizei height,
                                            GLsizei depth) {
            GLenum realInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(textureInternalFormat);
            GLenum realFormat = GL_RGBA;
            GLenum realType = GL_UNSIGNED_BYTE;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(
                realInternalFormat, PixelFormatNormalizeOptionBit::None, &realInternalFormat, &realFormat, &realType);
            return static_cast<SizeT>(width) * static_cast<SizeT>(height) * static_cast<SizeT>(depth) *
                   MG_Util::GetInternalBytesPerPixel(textureInternalFormat,
                                                     MG_Util::ConvertGLEnumToTexturePixelDataType(realType));
        }

        Bool IsSizedTextureStorageInternalFormat(TextureInternalFormat textureInternalFormat) {
            switch (textureInternalFormat) {
            case TextureInternalFormat::Red:
            case TextureInternalFormat::RG:
            case TextureInternalFormat::RGB:
            case TextureInternalFormat::RGBA:
            case TextureInternalFormat::DepthComponent:
            case TextureInternalFormat::DepthStencil:
            case TextureInternalFormat::Unknown:
                return false;
            default:
                return true;
            }
        }

        Bool ValidateTextureStorageInternalFormat(TextureInternalFormat textureInternalFormat, const char* caller) {
            // TODO: Replace this sized-format filter with the full ARB_texture_storage legal-format table.
            if (!IsSizedTextureStorageInternalFormat(textureInternalFormat)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "TexStorage requires a sized internal format."));
                return false;
            }
            return TextureImpl::ValidateTextureInternalFormat(textureInternalFormat);
        }

        Bool ValidateTextureMutable(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                    const char* caller) {
            if (!textureObject || !textureObject->IsImmutable()) return true;

            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Immutable texture storage cannot be redefined."));
            return false;
        }

        GLint GetTextureComponentType(TextureInternalFormat textureInternalFormat, GLint size, Bool depthComponent,
                                      Bool stencilComponent) {
            if (size <= 0) return GL_NONE;
            if (stencilComponent) return GL_UNSIGNED_INT;
            if (depthComponent) {
                return (textureInternalFormat == TextureInternalFormat::DepthComponent32F ||
                        textureInternalFormat == TextureInternalFormat::Depth32FStencil8)
                           ? GL_FLOAT
                           : GL_UNSIGNED_NORMALIZED;
            }
            switch (textureInternalFormat) {
            case TextureInternalFormat::R8I:
            case TextureInternalFormat::R16I:
            case TextureInternalFormat::R32I:
            case TextureInternalFormat::RG8I:
            case TextureInternalFormat::RG16I:
            case TextureInternalFormat::RG32I:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGBA8I:
            case TextureInternalFormat::RGBA16I:
            case TextureInternalFormat::RGBA32I:
                return GL_INT;
            case TextureInternalFormat::R8UI:
            case TextureInternalFormat::R16UI:
            case TextureInternalFormat::R32UI:
            case TextureInternalFormat::RG8UI:
            case TextureInternalFormat::RG16UI:
            case TextureInternalFormat::RG32UI:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32UI:
            case TextureInternalFormat::RGBA8UI:
            case TextureInternalFormat::RGBA16UI:
            case TextureInternalFormat::RGBA32UI:
            case TextureInternalFormat::RGB10A2UI:
                return GL_UNSIGNED_INT;
            case TextureInternalFormat::R16F:
            case TextureInternalFormat::RG16F:
            case TextureInternalFormat::RGB16F:
            case TextureInternalFormat::RGBA16F:
            case TextureInternalFormat::R32F:
            case TextureInternalFormat::RG32F:
            case TextureInternalFormat::RGB32F:
            case TextureInternalFormat::RGBA32F:
            case TextureInternalFormat::R11FG11FB10F:
            case TextureInternalFormat::RGB9E5:
                return GL_FLOAT;
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::RGBA16Snorm:
                return GL_SIGNED_NORMALIZED;
            default:
                return GL_UNSIGNED_NORMALIZED;
            }
        }

        // TextureInternalFormat has no compressed enumerator and CompressedTexImage* rejects every
        // compressed format up front, so no texture image MobileGL holds can be compressed. Written
        // as a predicate rather than a literal false so both level-parameter getters stay in step
        // once compressed formats do land.
        // GL 4.6 core 8.11 asks "is *this level* stored compressed", not "is the texture's internal
        // format a compressed one", and here the two genuinely differ: a compressed internalformat
        // handed to glTexImage2D resolves to the uncompressed storage that backs it (see
        // ConvertGLEnumToTextureInternalFormat), so the texture's format enum can never answer yes.
        // The only levels stored compressed are the ones glCompressedTexImage* shadowed verbatim,
        // which is exactly what the per-level compressed format records.
        //
        // No level-count guard on purpose: TextureObject2DCube::GetMipmapLevelCount() reports face
        // zero's chain only, so a count check would answer GL_NONE for a compressed image on any
        // other face - precisely the per-face independence the storage layer provides. MipmapStorage's
        // own getters already bounds-check per target and return GL_NONE for an unallocated level.
        GLenum GetCompressedLevelFormat(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                        TextureUploadTarget uploadTarget, GLint level) {
            if (!textureObject || level < 0) return GL_NONE;
            const auto* textureMipmapObject = MG_State::GLState::AsMipmapTexture(textureObject.get());
            if (!textureMipmapObject) return GL_NONE;
            return textureMipmapObject->GetMipmapCompressedFormat(uploadTarget, static_cast<Uint>(level));
        }

        GLint GetTextureLevelComponentParameter(TextureInternalFormat textureInternalFormat, GLenum pname) {
            const ComponentSizes componentSizes = MG_Util::GetComponentSizesForInternalFormat(textureInternalFormat);
            switch (pname) {
            case GL_TEXTURE_RED_SIZE:
                return componentSizes.Red;
            case GL_TEXTURE_GREEN_SIZE:
                return componentSizes.Green;
            case GL_TEXTURE_BLUE_SIZE:
                return componentSizes.Blue;
            case GL_TEXTURE_ALPHA_SIZE:
                return componentSizes.Alpha;
            case GL_TEXTURE_DEPTH_SIZE:
                return componentSizes.Depth;
            case GL_TEXTURE_STENCIL_SIZE:
                return componentSizes.Stencil;
            case GL_TEXTURE_RED_TYPE:
                return GetTextureComponentType(textureInternalFormat, componentSizes.Red, false, false);
            case GL_TEXTURE_GREEN_TYPE:
                return GetTextureComponentType(textureInternalFormat, componentSizes.Green, false, false);
            case GL_TEXTURE_BLUE_TYPE:
                return GetTextureComponentType(textureInternalFormat, componentSizes.Blue, false, false);
            case GL_TEXTURE_ALPHA_TYPE:
                return GetTextureComponentType(textureInternalFormat, componentSizes.Alpha, false, false);
            case GL_TEXTURE_DEPTH_TYPE:
                return GetTextureComponentType(textureInternalFormat, componentSizes.Depth, true, false);
            default:
                MOBILEGL_ASSERT(false, "Invalid texture level component pname: %d", pname);
                return 0;
            }
        }

        Bool IsValidImageTextureFormat(GLenum format) {
            switch (format) {
            case GL_RGBA32F:
            case GL_RGBA16F:
            case GL_RG32F:
            case GL_RG16F:
            case GL_R11F_G11F_B10F:
            case GL_R32F:
            case GL_R16F:
            case GL_RGBA32UI:
            case GL_RGBA16UI:
            case GL_RGB10_A2UI:
            case GL_RGBA8UI:
            case GL_RG32UI:
            case GL_RG16UI:
            case GL_RG8UI:
            case GL_R32UI:
            case GL_R16UI:
            case GL_R8UI:
            case GL_RGBA32I:
            case GL_RGBA16I:
            case GL_RGBA8I:
            case GL_RG32I:
            case GL_RG16I:
            case GL_RG8I:
            case GL_R32I:
            case GL_R16I:
            case GL_R8I:
            case GL_RGBA16:
            case GL_RGB10_A2:
            case GL_RGBA8:
            case GL_RG16:
            case GL_RG8:
            case GL_R16:
            case GL_R8:
            case GL_RGBA16_SNORM:
            case GL_RGBA8_SNORM:
            case GL_RG16_SNORM:
            case GL_RG8_SNORM:
            case GL_R16_SNORM:
            case GL_R8_SNORM:
                return true;
            default:
                return false;
            }
        }

        GLuint GetAdvertisedImageUnitCount() {
            return static_cast<GLuint>(std::min<GLint>(
                MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxImageUnits,
                MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS));
        }

        // Array targets store their layer count in z; layers never participate in mip
        // reduction (GL 3.3 §3.8.14), only true 3D textures halve their depth per level.
        Bool DepthParticipatesInMipmapping(TextureTarget target) {
            return target == TextureTarget::Texture3D;
        }

        // Which targets each glTextureStorage*D accepts (GL 4.6 core 8.19). A texture whose target
        // belongs to a different one of the three is the wrong object, not a bad argument, so it is
        // INVALID_OPERATION.
        Bool IsTextureStorageTargetForDimension(TextureTarget target, int dimension) {
            switch (dimension) {
            case 1:
                return target == TextureTarget::Texture1D;
            case 2:
                return target == TextureTarget::Texture2D || target == TextureTarget::Texture1DArray ||
                       target == TextureTarget::TextureRectangle || target == TextureTarget::TextureCubeMap;
            case 3:
                return target == TextureTarget::Texture3D || target == TextureTarget::Texture2DArray ||
                       target == TextureTarget::TextureCubeMapArray;
            default:
                return false;
            }
        }

        // The longest mip chain the level-0 size admits. A 1D array keeps its layer count in
        // height, so unlike a 2D texture its height takes no part in the reduction.
        Uint ComputeFullMipmapLevelCount(const IntVec3& baseTexelSize, Bool depthMips);

        Uint MaxTextureStorageLevels(TextureTarget target, GLsizei width, GLsizei height, GLsizei depth) {
            const Int mipHeight = (target == TextureTarget::Texture1DArray) ? 1 : std::max<Int>(height, 1);
            return ComputeFullMipmapLevelCount({std::max<Int>(width, 1), mipHeight, std::max<Int>(depth, 1)},
                                               DepthParticipatesInMipmapping(target));
        }

        Uint ComputeFullMipmapLevelCount(const IntVec3& baseTexelSize, Bool depthMips) {
            Int maxDimension = std::max<Int>(
                baseTexelSize.x(),
                std::max<Int>(baseTexelSize.y(), depthMips ? std::max<Int>(baseTexelSize.z(), 1) : 1));
            Uint mipLevelCount = 1;
            while (maxDimension > 1) {
                maxDimension = std::max<Int>(maxDimension / 2, 1);
                ++mipLevelCount;
            }
            return mipLevelCount;
        }

        IntVec3 ComputeMipmapTexelSize(const IntVec3& baseTexelSize, Uint relativeLevel, Bool depthMips) {
            return {
                std::max<Int>(baseTexelSize.x() >> static_cast<Int>(relativeLevel), 1),
                std::max<Int>(baseTexelSize.y() >> static_cast<Int>(relativeLevel), 1),
                depthMips ? std::max<Int>(baseTexelSize.z() >> static_cast<Int>(relativeLevel), 1)
                          : std::max<Int>(baseTexelSize.z(), 1),
            };
        }

        Bool EnsureGeneratedMipmapStorageAllocated(
            MG_State::GLState::TextureObjectMipmap& texture,
            TextureUploadTarget uploadTarget) {
            const Uint existingLevelCount = texture.GetMipmapLevelCount();
            if (existingLevelCount == 0) {
                return false;
            }

            const IntVec3 baseTexelSize = texture.GetMipmapTexelSize(uploadTarget, 0);
            const SizeT baseByteSize = texture.GetMipmapByteSize(uploadTarget, 0);
            const SizeT baseTexelCount = static_cast<SizeT>(baseTexelSize.x()) *
                                         static_cast<SizeT>(baseTexelSize.y()) *
                                         static_cast<SizeT>(baseTexelSize.z());
            if (baseTexelSize.x() <= 0 || baseTexelSize.y() <= 0 || baseTexelSize.z() <= 0 ||
                baseByteSize == 0 || baseTexelCount == 0 || (baseByteSize % baseTexelCount) != 0) {
                return false;
            }

            const SizeT bytesPerTexel = baseByteSize / baseTexelCount;
            const Bool depthMips = DepthParticipatesInMipmapping(texture.GetTarget());
            const Uint requiredLevelCount = ComputeFullMipmapLevelCount(baseTexelSize, depthMips);
            for (Uint level = 1; level < requiredLevelCount; ++level) {
                const IntVec3 levelTexelSize = ComputeMipmapTexelSize(baseTexelSize, level, depthMips);
                const SizeT levelByteSize = bytesPerTexel * static_cast<SizeT>(levelTexelSize.x()) *
                                            static_cast<SizeT>(levelTexelSize.y()) *
                                            static_cast<SizeT>(levelTexelSize.z());
                texture.AllocateStorage(uploadTarget, level, {levelTexelSize, levelByteSize});
                texture.MarkStorageDirty(uploadTarget, level, false);
            }
            // glGenerateMipmap defines exactly levels 0..requiredLevelCount-1. AllocateStorage only
            // grows, so a previously longer chain (a bigger base image before respecification) would
            // otherwise keep a tail of stale levels here and read as incomplete.
            texture.TruncateMipmapLevels(uploadTarget, requiredLevelCount);
            // Mip generation grows/regenerates the level set on the GPU without marking any CPU
            // level dirty (MarkStorageDirty(...,false) above). Bump the content version so the
            // backend re-syncs: a cached sampled VkImageView built for the pre-generate level
            // range would otherwise stay stale and clamp LOD>0 sampling to mip 0.
            texture.BumpContentVersion();
            return true;
        }

        void EnsureGeneratedMipmapStorageAllocated(MG_State::GLState::TextureObjectMipmap& texture) {
            for (const TextureUploadTarget uploadTarget : texture.GetUploadTargets()) {
                EnsureGeneratedMipmapStorageAllocated(texture, uploadTarget);
            }
        }

        Bool IsMultisampleTextureTarget(TextureTarget target) {
            return target == TextureTarget::Texture2DMultisample ||
                   target == TextureTarget::Texture2DMultisampleArray;
        }

        // The largest count the backend actually probed for this format on this target, or 0 when
        // it has no answer for the pair. Both backends build the list in descending order.
        Int GetProbedMaxTextureSamples(TextureTarget textureTarget, TextureInternalFormat textureInternalFormat) {
            if (MG_Backend::pActiveBackendObject == nullptr) {
                return 0;
            }
            const SizeT targetIndex = MG_Backend::GetFormatCapabilityTargetIndex(textureTarget);
            const SizeT formatIndex = static_cast<SizeT>(textureInternalFormat);
            if (targetIndex >= MG_Backend::kFormatCapabilityTargetCount ||
                formatIndex >= MG_Backend::kFormatCapabilityFormatCount) {
                return 0;
            }
            const auto& sampleCounts =
                MG_Backend::pActiveBackendObject->GetFormatCapabilities().SampleCounts[targetIndex][formatIndex];
            return sampleCounts.empty() ? 0 : sampleCounts.front();
        }

        // The ceiling the frontend enforces, which must never be lower than the one MobileGL
        // advertises: the CTS - and real applications - read GL_MAX_SAMPLES once and hand that
        // exact count to glTexImage*Multisample for every format. Answering 4 there and then
        // rejecting 4 here because the ES driver reports GL_MAX_INTEGER_SAMPLES 1 (Adreno) is a
        // self-inconsistency, not a spec-mandated error. The backends clamp the count they hand
        // the driver; the shadow state keeps reporting what the application asked for.
        Int GetMaxSupportedTextureSamples(TextureTarget textureTarget,
                                          TextureInternalFormat textureInternalFormat) {
            if (MG_Backend::pActiveBackendObject == nullptr) {
                return std::numeric_limits<Int>::max();
            }

            const Int advertisedMaxSamples = GetAdvertisedMaxSamples();
            // glGetInternalformativ(GL_SAMPLES) is answered from this very list (GetInternalformativ
            // below), and GL 4.6 core 8.8 makes that query the definition of the per-format
            // maximum - validating against anything else is how the two answers drifted apart.
            const Int probedMaxSamples = GetProbedMaxTextureSamples(textureTarget, textureInternalFormat);
            if (probedMaxSamples > 0) {
                return std::max(probedMaxSamples, advertisedMaxSamples);
            }

            const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
            if (MG_Util::IsDepthFormatInternalFormat(textureInternalFormat) ||
                MG_Util::IsStencilFormatInternalFormat(textureInternalFormat)) {
                return std::max(dynamicParameters.MaxDepthTextureSamples, advertisedMaxSamples);
            }

            GLenum normalizedInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(textureInternalFormat);
            GLenum normalizedFormat = GL_RGBA;
            GLenum normalizedType = GL_UNSIGNED_BYTE;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(
                normalizedInternalFormat, PixelFormatNormalizeOptionBit::None, &normalizedInternalFormat,
                &normalizedFormat, &normalizedType);
            const Bool isIntegerFormat = normalizedFormat == GL_RED_INTEGER || normalizedFormat == GL_RG_INTEGER ||
                                         normalizedFormat == GL_RGB_INTEGER || normalizedFormat == GL_RGBA_INTEGER;
            return std::max(isIntegerFormat ? dynamicParameters.MaxIntegerSamples
                                            : dynamicParameters.MaxColorTextureSamples,
                            advertisedMaxSamples);
        }

        Bool ValidateTextureMultisampleStorage(TextureTarget textureTarget, GLsizei samples, GLsizei width,
                                               GLsizei height, GLsizei depth, TextureInternalFormat textureInternalFormat,
                                               const char* caller) {
            if (!IsMultisampleTextureTarget(textureTarget)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "Target is not a multisample texture target."));
                return false;
            }
            if (samples <= 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Sample count must be positive."));
                return false;
            }
            if (width < 0 || height < 0 || depth < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture size must be non-negative."));
                return false;
            }
            if (textureTarget == TextureTarget::Texture2DMultisample && depth != 1) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "2D multisample textures must use depth 1."));
                return false;
            }
            // Zero layers is NOT an error for multisample arrays: depth == 0 (like width/height
            // == 0) deallocates the image - GL 4.5 8.8 only raises INVALID_VALUE for negative
            // dimensions, and GL CTS's per-case state reset (gluStateReset) clears the default
            // GL_TEXTURE_2D_MULTISAMPLE_ARRAY texture with glTexImage3DMultisample(..., 0, 0, 0).

            const Int maxSamples = GetMaxSupportedTextureSamples(textureTarget, textureInternalFormat);
            if (samples > maxSamples) {
                // GL specifies INVALID_OPERATION - not INVALID_VALUE - when the sample count
                // exceeds what the format supports, and the native Adreno driver agrees.
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", caller,
                        std::format("Sample count {} exceeds the supported maximum {} for this texture format.",
                                    samples, maxSamples)));
                return false;
            }
            return true;
        }

        void AllocateMultisampleTextureStorage(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                               TextureUploadTarget textureUploadTarget,
                                               TextureInternalFormat textureInternalFormat, GLsizei samples,
                                               GLsizei width, GLsizei height, GLsizei depth,
                                               GLboolean fixedsamplelocations) {
            MOBILEGL_ASSERT(textureObject != nullptr, "AllocateMultisampleTextureStorage requires a texture object");
            MOBILEGL_ASSERT(textureObject->GetStorageType() == TextureStorageType::Mipmap,
                            "AllocateMultisampleTextureStorage requires mipmap-backed storage");

            auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
            // GL 4.6 core 8.8: a zero-sized image DEALLOCATES the image rather than defining an
            // empty one. Only the multisample pair cares, and it cares a great deal: the CTS's
            // per-case state reset clears both DEFAULT multisample textures this way on every
            // texture unit, and a "defined" 0x0 default texture stops being skipped by
            // IsUndefinedDefaultTexture - it then joins the per-draw sync and bind passes on
            // every unit the reset touched, and reaches an ES glTexStorage*Multisample(..., 0, 0)
            // that ES 3.1 8.19 makes INVALID_VALUE on every driver there is. A proxy target holds
            // no image at all, only the query result, so it keeps recording what was asked for.
            if ((width <= 0 || height <= 0 || depth <= 0) &&
                !TextureImpl::IsProxyTextureTarget(textureUploadTarget)) {
                textureObject->SetInternalFormat(TextureInternalFormat::Unknown);
                textureMipmapObject->TruncateMipmapLevels(textureUploadTarget, 0);
                return;
            }
            textureObject->SetInternalFormat(textureInternalFormat);
            textureObject->SetSamples(samples);
            textureObject->SetFixedSampleLocations(fixedsamplelocations == GL_TRUE);
            textureMipmapObject->AllocateStorage(textureUploadTarget, 0, {{width, height, depth}, 0});
            // Multisample textures are single-level by definition, so a name that previously held a
            // mip chain must not keep its tail now that AllocateStorage only grows.
            textureMipmapObject->TruncateMipmapLevels(textureUploadTarget, 1);
            textureMipmapObject->MarkStorageDirty(textureUploadTarget, 0, false);
        }

        // Redefining level 0 of a texture that already had a base image drops the rest of the chain,
        // which is exactly what AllocateLevel used to do implicitly for every level. Keeping that
        // behaviour for level 0 - and only for level 0 - is what makes the grow-only change safe:
        // any level-0 respecification leaves the chain in precisely the state it would have had
        // before, while an upload to level N no longer destroys the levels beneath it.
        //
        // Why it has to be *every* level-0 respecification and not just a size change: Minecraft's
        // Mipmap Levels setting rebuilds the block atlas at the SAME dimensions with a different
        // level count. A size-only test would leave the old tail in place, and because Mojang
        // terminates its chains with a 0x0 level the result is the zero-then-nonzero pattern that
        // IsComplete() rejects (TextureObject.cpp) - whereupon DirectGLES skips syncing the texture
        // entirely (Managers.cpp) and the atlas samples black.
        //
        // The "already has a base image" test is what lets the fix work at all: a level that was
        // never written reads back as {0,0,0}, so building a chain top-down - upload level N first,
        // then level 0 - must not discard the levels just uploaded. That ordering is what
        // KHR-GL33.texture_repeat_mode does.
        // Scoped to the respecified upload target only, which is what AllocateLevel already did.
        // Cube maps keep six independent chains while reporting a single level count (face +X), so
        // respecifying a face other than +X can leave the count longer than that face - but that
        // asymmetry predates this change and widening the truncation to all six faces would destroy
        // mip data for faces the application never touched. Left alone deliberately.
        void DiscardMipmapChainOnBaseRespecification(MG_State::GLState::TextureObjectMipmap* texture,
                                                     TextureUploadTarget uploadTarget, Uint level) {
            if (level != 0) return;

            const IntVec3 existingBaseSize = texture->GetMipmapTexelSize(uploadTarget, 0);
            const Bool hasExistingBaseImage =
                existingBaseSize.x() > 0 && existingBaseSize.y() > 0 && existingBaseSize.z() > 0;
            if (!hasExistingBaseImage) return;

            texture->TruncateMipmapLevels(uploadTarget, 1);
        }

        // The compressed internalformat is not one this stack can store (see
        // MG_Util::GetCompressedFormatInfo for the accepted set: the RGTC/BPTC/ETC2-EAC formats core
        // GL requires). GL_INVALID_ENUM is the specified error for an unsupported compressed format -
        // unlike THROW_UNIMPL_EXCEPTION, which unwinds a C++ exception through the C GL ABI and takes
        // the process down. Still the only outcome for the 1D/3D and sub-image entry points, which
        // have no compressed upload path yet.
        void RecordUnsupportedCompressedFormat(const char* caller) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Compressed texture formats are not supported."));
        }

        // glGetTexLevelParameter{i,f}v answers WIDTH/HEIGHT/DEPTH out of the mipmap chain. The only
        // other storage type the state layer knows is GL_TEXTURE_BUFFER (TextureStorageType is
        // {Mipmap, Buffer}), whose level geometry this stack does not track yet. Report that instead
        // of throwing: THROW_UNIMPL_EXCEPTION unwinds a C++ exception through the C GL ABI and takes
        // the process down, which is never an acceptable answer to a query - see the same reasoning
        // above for the compressed-format path.
        void RecordUnsupportedLevelQueryStorage(const char* caller, GLenum pname) {
            MGLOG_W_ONCE("%s: glGetTexLevelParameter(pname=%s) is not implemented for texture-buffer "
                    "storage; recording GL_INVALID_OPERATION instead of terminating",
                    caller, MG_Util::ConvertGLEnumToString(pname).c_str());
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    "Level queries are not supported for texture-buffer storage."));
        }
    } // namespace

    const SharedPtr<MG_State::GLState::ITextureObject>& GetTextureObjectByName(GLuint texture, const char* caller) {
        auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             std::format("Texture object {} does not exist.", texture)));
            return nullTextureObject;
        }
        return textureObject;
    }

    namespace {
        void RecordClearTextureError(const char* caller, ErrorCode code, const String& message) {
            MG_State::pGLContext->RecordError(
                code, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, message));
        }

        SharedPtr<MG_State::GLState::TextureObjectMipmap> GetClearTextureObject(GLuint texture, GLint level,
                                                                                const char* caller) {
            if (texture == 0) {
                RecordClearTextureError(caller, ErrorCode::InvalidOperation,
                                        "Clear texture operations require a non-zero texture name.");
                return nullptr;
            }

            auto textureObject = GetTextureObjectByName(texture, caller);
            if (!textureObject) return nullptr;
            if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
                RecordClearTextureError(caller, ErrorCode::InvalidOperation,
                                        "Buffer textures cannot be cleared with glClearTexImage.");
                return nullptr;
            }

            auto mipmapTexture = std::static_pointer_cast<MG_State::GLState::TextureObjectMipmap>(textureObject);
            if (level < 0) {
                RecordClearTextureError(caller, ErrorCode::InvalidValue,
                                        std::format("Texture level {} is negative.", level));
                return nullptr;
            }
            // ARB_clear_texture: clearing an image that was never defined by TexImage*/
            // TexStorage* is INVALID_OPERATION, not INVALID_VALUE.
            if (static_cast<Uint>(level) >= mipmapTexture->GetMipmapLevelCount()) {
                RecordClearTextureError(caller, ErrorCode::InvalidOperation,
                                        std::format("Texture level {} is not defined.", level));
                return nullptr;
            }
            return mipmapTexture;
        }

        Bool BuildClearPixel(const SharedPtr<MG_State::GLState::TextureObjectMipmap>& textureObject,
                             GLenum format, GLenum type, const void* data, Vector<Uint8>& clearPixel) {
            const TextureInputFormat inputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
            const TexturePixelDataType inputType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
            if (!TextureImpl::ValidateTextureInputFormat(inputFormat) ||
                !TextureImpl::ValidateTexturePixelDataType(inputType) ||
                !TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(
                    inputFormat, textureObject->GetFormat(), inputType)) {
                return false;
            }

            clearPixel.clear();
            if (data == nullptr) {
                // ARB_clear_texture defines a null clear value as all zeroes. Keeping the
                // pattern empty lets the region writer use a fast memset path.
                return true;
            }

            PixelStoreParameters clearPixelStore{};
            clearPixelStore.Alignment = 1;
            SizeT clearPixelSize = 0;
            void* converted = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
                data, clearPixelStore, textureObject->GetFormat(), inputFormat, inputType,
                {1, 1, 1}, false, clearPixelSize);
            if (!converted || clearPixelSize == 0) {
                if (converted) free(converted);
                return false;
            }

            clearPixel.resize(clearPixelSize);
            Memcpy(clearPixel.data(), converted, clearPixelSize);
            free(converted);
            return true;
        }

        // Writes the clear into the CPU shadow and marks the whole level dirty, exactly like
        // TexSubImage*_State does. Shared limitation of the level-granular shadow sync: the
        // shadow does not reflect GPU-side writes (FBO rendering, imageStore), so a PARTIAL
        // clear of a GPU-written level re-uploads stale shadow bytes outside the region on
        // the next sync. Full-level clears (glClearTexImage, or a sub-clear covering the
        // level) rewrite the entire shadow and are always correct.
        Bool ClearMipmapRegion(const SharedPtr<MG_State::GLState::TextureObjectMipmap>& textureObject,
                               TextureUploadTarget uploadTarget, GLint level,
                               GLint xoffset, GLint yoffset, GLint zoffset,
                               GLsizei width, GLsizei height, GLsizei depth,
                               const Vector<Uint8>& clearPixel, const char* caller) {
            const IntVec3 texelSize = textureObject->GetMipmapTexelSize(uploadTarget, static_cast<Uint>(level));
            if (texelSize.x() <= 0 || texelSize.y() <= 0 || texelSize.z() <= 0) {
                RecordClearTextureError(caller, ErrorCode::InvalidOperation,
                                        "The requested texture level has no storage.");
                return false;
            }
            if (xoffset < 0 || yoffset < 0 || zoffset < 0 ||
                width < 0 || height < 0 || depth < 0 ||
                width > texelSize.x() - xoffset ||
                height > texelSize.y() - yoffset ||
                depth > texelSize.z() - zoffset) {
                RecordClearTextureError(caller, ErrorCode::InvalidValue,
                                        "The clear region lies outside the requested texture level.");
                return false;
            }
            if (width == 0 || height == 0 || depth == 0) return true;

            const SizeT texelCount = static_cast<SizeT>(texelSize.x()) *
                                     static_cast<SizeT>(texelSize.y()) *
                                     static_cast<SizeT>(texelSize.z());
            const SizeT byteSize = textureObject->GetMipmapByteSize(uploadTarget, static_cast<Uint>(level));
            if (byteSize == 0 || byteSize % texelCount != 0) {
                RecordClearTextureError(caller, ErrorCode::InvalidOperation,
                                        "The requested texture storage cannot be cleared.");
                return false;
            }

            const SizeT bytesPerTexel = byteSize / texelCount;
            if (!clearPixel.empty() && clearPixel.size() != bytesPerTexel) {
                RecordClearTextureError(
                    caller, ErrorCode::InvalidOperation,
                    std::format("Converted clear value is {} bytes, but the texture stores {} bytes per texel.",
                                clearPixel.size(), bytesPerTexel));
                return false;
            }

            auto* destination = static_cast<Uint8*>(
                textureObject->MapMipmapData(uploadTarget, static_cast<Uint>(level)));
            if (!destination) {
                RecordClearTextureError(caller, ErrorCode::InvalidOperation,
                                        "The requested texture level could not be mapped.");
                return false;
            }

            const SizeT fullRowBytes = static_cast<SizeT>(texelSize.x()) * bytesPerTexel;
            const SizeT fullSliceBytes = static_cast<SizeT>(texelSize.y()) * fullRowBytes;
            const SizeT clearRowBytes = static_cast<SizeT>(width) * bytesPerTexel;
            Uint8* firstClearRow = nullptr;

            for (GLsizei z = 0; z < depth; ++z) {
                for (GLsizei y = 0; y < height; ++y) {
                    Uint8* row = destination +
                                 static_cast<SizeT>(zoffset + z) * fullSliceBytes +
                                 static_cast<SizeT>(yoffset + y) * fullRowBytes +
                                 static_cast<SizeT>(xoffset) * bytesPerTexel;
                    if (firstClearRow) {
                        Memcpy(row, firstClearRow, clearRowBytes);
                        continue;
                    }

                    firstClearRow = row;
                    if (clearPixel.empty()) {
                        Memset(row, 0, clearRowBytes);
                        continue;
                    }

                    Memcpy(row, clearPixel.data(), bytesPerTexel);
                    SizeT filled = bytesPerTexel;
                    while (filled < clearRowBytes) {
                        const SizeT copySize = std::min(filled, clearRowBytes - filled);
                        Memcpy(row + filled, row, copySize);
                        filled += copySize;
                    }
                }
            }

            textureObject->MarkStorageDirty(uploadTarget, static_cast<Uint>(level), true);
            return true;
        }
        // GL 4.6 core 8.6: CopyTexSubImage* is not affected by pixel-store state or by a bound
        // pack buffer, but the backend readback this borrows honours both. Neutralise them for the
        // duration of the read and put them back afterwards.
        class ScopedNeutralPackState {
        public:
            ScopedNeutralPackState() {
                for (SizeT i = 0; i < kParams.size(); ++i) {
                    m_saved[i] = MG_State::pGLContext->GetPixelStoreParam(kParams[i]);
                    MG_State::pGLContext->SetPixelStoreParam(kParams[i], i == 0 ? 1 : 0);
                }
                auto& slot = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack);
                m_savedPackBuffer = slot.GetBoundObject();
                slot.Bind(nullptr);
            }
            ~ScopedNeutralPackState() {
                for (SizeT i = 0; i < kParams.size(); ++i) {
                    MG_State::pGLContext->SetPixelStoreParam(kParams[i], m_saved[i]);
                }
                MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).Bind(m_savedPackBuffer);
            }

        private:
            // PackAlignment must be first: it is the one that resets to 1 rather than 0.
            static constexpr Array<PixelStoreParam, 8> kParams{
                PixelStoreParam::PackAlignment,  PixelStoreParam::PackRowLength,
                PixelStoreParam::PackImageHeight, PixelStoreParam::PackSkipRows,
                PixelStoreParam::PackSkipPixels, PixelStoreParam::PackSkipImages,
                PixelStoreParam::PackSwapBytes,  PixelStoreParam::PackLSBFirst};
            Array<Int, 8> m_saved{};
            SharedPtr<MG_State::GLState::BufferObject> m_savedPackBuffer;
        };

        // The copy half of glCopyTexSubImage*: read the region out of the read framebuffer and write
        // it into the destination level's CPU storage. Done in the frontend because that storage is
        // where a texture's contents actually live - the backends sync from it - so this needs no
        // 1D or 3D blit, which neither backend has.
        Bool CopyReadFramebufferIntoMipmapRegion(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                 TextureUploadTarget uploadTarget, GLint level, GLint xoffset,
                                                 GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width,
                                                 GLsizei height, const char* caller) {
            if (width <= 0 || height <= 0) return true;
            auto* mipmapTexture = MG_State::GLState::AsMipmapTexture(textureObject.get());
            if (!mipmapTexture) return false;

            const auto texelSize = mipmapTexture->GetMipmapTexelSize(uploadTarget, static_cast<Uint>(level));
            const SizeT texelCount = static_cast<SizeT>(texelSize.x()) * static_cast<SizeT>(texelSize.y()) *
                                     static_cast<SizeT>(texelSize.z());
            const SizeT byteSize = mipmapTexture->GetMipmapByteSize(uploadTarget, static_cast<Uint>(level));
            if (texelCount == 0 || byteSize == 0 || byteSize % texelCount != 0) return false;
            const SizeT bytesPerTexel = byteSize / texelCount;

            // Read in the destination's own canonical client layout, so the bytes land in storage
            // without a second conversion.
            const GLenum glInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(textureObject->GetFormat());
            GLenum realInternalFormat = glInternalFormat;
            GLenum format = GL_RGBA;
            GLenum type = GL_UNSIGNED_BYTE;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(glInternalFormat, PixelFormatNormalizeOptionBit::None,
                                                                  &realInternalFormat, &format, &type);
            const SizeT readBytesPerTexel =
                MG_Util::GetInputBytesPerPixel(MG_Util::ConvertGLEnumToTextureInputFormat(format),
                                               MG_Util::ConvertGLEnumToTexturePixelDataType(type));
            if (readBytesPerTexel != bytesPerTexel) {
                MGLOG_W_ONCE("%s: cannot copy into a %zu-byte texel from a %zu-byte readback layout", caller,
                        bytesPerTexel, readBytesPerTexel);
                return false;
            }

            Vector<Uint8> scratch(static_cast<SizeT>(width) * static_cast<SizeT>(height) * bytesPerTexel);
            {
                ScopedNeutralPackState neutralPack;
                MG_Backend::gBackendFunctionsTable.GL.ReadPixels(x, y, width, height, format, type, scratch.data());
            }

            auto* destination =
                static_cast<Uint8*>(mipmapTexture->MapMipmapData(uploadTarget, static_cast<Uint>(level)));
            if (!destination) return false;

            const SizeT fullRowBytes = static_cast<SizeT>(texelSize.x()) * bytesPerTexel;
            const SizeT fullSliceBytes = static_cast<SizeT>(texelSize.y()) * fullRowBytes;
            const SizeT copyRowBytes = static_cast<SizeT>(width) * bytesPerTexel;
            for (GLsizei row = 0; row < height; ++row) {
                Uint8* dst = destination + static_cast<SizeT>(zoffset) * fullSliceBytes +
                             static_cast<SizeT>(yoffset + row) * fullRowBytes +
                             static_cast<SizeT>(xoffset) * bytesPerTexel;
                Memcpy(dst, scratch.data() + static_cast<SizeT>(row) * copyRowBytes, copyRowBytes);
            }
            mipmapTexture->MarkStorageDirty(uploadTarget, static_cast<Uint>(level), true);
            return true;
        }
    } // namespace

    void ClearTexImage(GLuint texture, GLint level, GLenum format, GLenum type, const void* data) {
        auto textureObject = GetClearTextureObject(texture, level, __func__);
        if (!textureObject) return;

        Vector<Uint8> clearPixel;
        if (!BuildClearPixel(textureObject, format, type, data, clearPixel)) return;

        for (TextureUploadTarget uploadTarget : textureObject->GetUploadTargets()) {
            const IntVec3 size = textureObject->GetMipmapTexelSize(uploadTarget, static_cast<Uint>(level));
            if (!ClearMipmapRegion(textureObject, uploadTarget, level, 0, 0, 0,
                                   size.x(), size.y(), size.z(), clearPixel, __func__)) {
                return;
            }
        }
    }

    void ClearTexSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                          GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                          const void* data) {
        auto textureObject = GetClearTextureObject(texture, level, __func__);
        if (!textureObject) return;

        Vector<Uint8> clearPixel;
        if (!BuildClearPixel(textureObject, format, type, data, clearPixel)) return;

        const auto& uploadTargets = textureObject->GetUploadTargets();
        if (textureObject->GetTarget() == TextureTarget::TextureCubeMap) {
            if (zoffset < 0 || depth < 0 ||
                static_cast<SizeT>(zoffset) > uploadTargets.size() ||
                static_cast<SizeT>(depth) > uploadTargets.size() - static_cast<SizeT>(zoffset)) {
                RecordClearTextureError(__func__, ErrorCode::InvalidValue,
                                        "The cube-map clear region selects invalid faces.");
                return;
            }
            for (GLsizei face = 0; face < depth; ++face) {
                if (!ClearMipmapRegion(textureObject, uploadTargets[static_cast<SizeT>(zoffset + face)], level,
                                       xoffset, yoffset, 0, width, height, 1, clearPixel, __func__)) {
                    return;
                }
            }
            return;
        }

        if (uploadTargets.empty()) {
            RecordClearTextureError(__func__, ErrorCode::InvalidOperation,
                                    "The requested texture has no upload target.");
            return;
        }
        ClearMipmapRegion(textureObject, uploadTargets.front(), level, xoffset, yoffset, zoffset,
                          width, height, depth, clearPixel, __func__);
    }

    Bool ValidateTextureParameterForTarget(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                           GLenum pname, GLint param, const char* caller) {
        const auto target = textureObject->GetTarget();
        if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT && !ValidateMaxAnisotropy(param, caller)) {
            return false;
        }
        if ((pname == GL_TEXTURE_BASE_LEVEL || pname == GL_TEXTURE_MAX_LEVEL) && param < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture level parameter must be non-negative."));
            return false;
        }

        if ((target == TextureTarget::Texture2DMultisample ||
             target == TextureTarget::Texture2DMultisampleArray) &&
            pname == GL_TEXTURE_BASE_LEVEL && param != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Multisample texture base level must be zero."));
            return false;
        }

        if (target == TextureTarget::TextureRectangle && pname == GL_TEXTURE_BASE_LEVEL && param != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Rectangle texture base level must be zero."));
            return false;
        }

        if ((target == TextureTarget::Texture2DMultisample ||
             target == TextureTarget::Texture2DMultisampleArray) &&
            (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == GL_TEXTURE_WRAP_R ||
             pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER || pname == GL_TEXTURE_MIN_LOD ||
             pname == GL_TEXTURE_MAX_LOD || pname == GL_TEXTURE_LOD_BIAS || pname == GL_TEXTURE_COMPARE_MODE ||
             pname == GL_TEXTURE_COMPARE_FUNC || pname == GL_TEXTURE_BORDER_COLOR ||
             pname == GL_TEXTURE_MAX_ANISOTROPY_EXT)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Sampler state is invalid for multisample textures."));
            return false;
        }

        if (target == TextureTarget::TextureRectangle) {
            if ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) &&
                (param == GL_MIRROR_CLAMP_TO_EDGE || param == GL_MIRRORED_REPEAT || param == GL_REPEAT)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "Invalid wrap mode for rectangle texture."));
                return false;
            }
            if (pname == GL_TEXTURE_MIN_FILTER && param != GL_NEAREST && param != GL_LINEAR) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "Invalid min filter for rectangle texture."));
                return false;
            }
        }

        return true;
    }

    TextureUploadTarget GetPrimaryUploadTarget(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject) {
        if (!textureObject) return TextureUploadTarget::Unknown;
        const auto& uploadTargets = textureObject->GetUploadTargets();
        return uploadTargets.empty() ? TextureUploadTarget::Unknown : uploadTargets[0];
    }

    void TextureParameterObject_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, GLenum pname,
                                      GLint param, const char* caller) {
        if (!textureObject) return;
        if (!ValidateTextureParameterForTarget(textureObject, pname, param, caller)) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            textureObject->GetSamplerObject()->SetMagFilter(MG_Util::ConvertGLEnumToSamplerFilterMode(param));
            break;
        case GL_TEXTURE_MIN_FILTER:
            textureObject->GetSamplerObject()->SetMinFilter(MG_Util::ConvertGLEnumToSamplerFilterMode(param));
            textureObject->GetSamplerObject()->SetMipmapMode(MG_Util::ConvertGLEnumToSamplerMipmapMode(param));
            break;
        case GL_TEXTURE_MIN_LOD: {
            Float maxLod = textureObject->GetSamplerObject()->GetMaxLod();
            textureObject->GetSamplerObject()->SetLodRange(param, maxLod);
            break;
        }
        case GL_TEXTURE_MAX_LOD: {
            Float minLod = textureObject->GetSamplerObject()->GetMinLod();
            textureObject->GetSamplerObject()->SetLodRange(minLod, param);
            break;
        }
        case GL_TEXTURE_BASE_LEVEL:
            textureObject->SetBaseLevel(param);
            break;
        case GL_TEXTURE_MAX_LEVEL:
            textureObject->SetMaxLevel(param);
            break;
        case GL_TEXTURE_SWIZZLE_R:
        case GL_TEXTURE_SWIZZLE_G:
        case GL_TEXTURE_SWIZZLE_B:
        case GL_TEXTURE_SWIZZLE_A: {
            auto swizzleParam = MG_Util::ConvertGLEnumPnameToTextureSwizzleParam(pname);
            auto swizzleValue = MG_Util::ConvertGLEnumToTextureSwizzleParam(param);
            if (swizzleValue == TextureSwizzleParam::Unknown) {
                // GL CTS texture_swizzle.api_errors: single-value TexParameter* with a value outside
                // [RED, GREEN, BLUE, ALPHA, ZERO, ONE] must raise GL_INVALID_ENUM.
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Invalid texture swizzle value."));
                return;
            }
            textureObject->SetSwizzleParam(swizzleParam, swizzleValue);
            break;
        }
        case GL_TEXTURE_WRAP_S:
            textureObject->GetSamplerObject()->SetWrapS(MG_Util::ConvertGLEnumToSamplerWrapMode(param));
            break;
        case GL_TEXTURE_WRAP_T:
            textureObject->GetSamplerObject()->SetWrapT(MG_Util::ConvertGLEnumToSamplerWrapMode(param));
            break;
        case GL_TEXTURE_WRAP_R:
            textureObject->GetSamplerObject()->SetWrapR(MG_Util::ConvertGLEnumToSamplerWrapMode(param));
            break;
        case GL_TEXTURE_COMPARE_MODE:
            textureObject->GetSamplerObject()->SetCompareMode(MG_Util::ConvertGLEnumToSamplerCompareMode(param));
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            textureObject->GetSamplerObject()->SetSamplerCompareFunc(MG_Util::ConvertGLEnumToSamplerCompareFunc(param));
            break;
        case GL_TEXTURE_LOD_BIAS:
            textureObject->GetSamplerObject()->SetLodBias((GLfloat)param);
            break;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            textureObject->GetSamplerObject()->SetMaxAnisotropy(static_cast<GLfloat>(param));
            break;
        case GL_GENERATE_MIPMAP:
            g_autoGenerateMipmapByTextureId[textureObject->GetExternalIndex()] = (param != GL_FALSE);
            break;
        case GL_DEPTH_STENCIL_TEXTURE_MODE:
            if (param != GL_DEPTH_COMPONENT && param != GL_STENCIL_INDEX) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "Invalid GL_DEPTH_STENCIL_TEXTURE_MODE value."));
                return;
            }
            textureObject->SetDepthStencilTextureMode(static_cast<GLenum>(param));
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("pname {} is not a valid texture parameter.", MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
    }

    void TextureParameterObjectf_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, GLenum pname,
                                       GLfloat param, const char* caller) {
        if (!textureObject) return;
        if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT && !ValidateMaxAnisotropy(param, caller)) return;
        const GLint validationParam =
            pname == GL_TEXTURE_MAX_ANISOTROPY_EXT ? 1 : static_cast<GLint>(param);
        if (!ValidateTextureParameterForTarget(textureObject, pname, validationParam, caller)) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            textureObject->GetSamplerObject()->SetMagFilter(MG_Util::ConvertGLEnumToSamplerFilterMode((GLenum)param));
            break;
        case GL_TEXTURE_MIN_FILTER:
            textureObject->GetSamplerObject()->SetMinFilter(MG_Util::ConvertGLEnumToSamplerFilterMode((GLenum)param));
            textureObject->GetSamplerObject()->SetMipmapMode(MG_Util::ConvertGLEnumToSamplerMipmapMode((GLenum)param));
            break;
        case GL_TEXTURE_MIN_LOD: {
            Float maxLod = textureObject->GetSamplerObject()->GetMaxLod();
            textureObject->GetSamplerObject()->SetLodRange(param, maxLod);
            break;
        }
        case GL_TEXTURE_MAX_LOD: {
            Float minLod = textureObject->GetSamplerObject()->GetMinLod();
            textureObject->GetSamplerObject()->SetLodRange(minLod, param);
            break;
        }
        case GL_TEXTURE_BASE_LEVEL:
            textureObject->SetBaseLevel((Uint)param);
            break;
        case GL_TEXTURE_MAX_LEVEL:
            textureObject->SetMaxLevel((Uint)param);
            break;
        case GL_TEXTURE_WRAP_S:
            textureObject->GetSamplerObject()->SetWrapS(MG_Util::ConvertGLEnumToSamplerWrapMode((GLenum)param));
            break;
        case GL_TEXTURE_WRAP_T:
            textureObject->GetSamplerObject()->SetWrapT(MG_Util::ConvertGLEnumToSamplerWrapMode((GLenum)param));
            break;
        case GL_TEXTURE_WRAP_R:
            textureObject->GetSamplerObject()->SetWrapR(MG_Util::ConvertGLEnumToSamplerWrapMode((GLenum)param));
            break;
        case GL_TEXTURE_COMPARE_MODE:
            textureObject->GetSamplerObject()->SetCompareMode(
                MG_Util::ConvertGLEnumToSamplerCompareMode((GLenum)param));
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            textureObject->GetSamplerObject()->SetSamplerCompareFunc(
                MG_Util::ConvertGLEnumToSamplerCompareFunc((GLenum)param));
            break;
        case GL_TEXTURE_LOD_BIAS:
            textureObject->GetSamplerObject()->SetLodBias(param);
            break;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            textureObject->GetSamplerObject()->SetMaxAnisotropy(param);
            break;
        case GL_GENERATE_MIPMAP:
            g_autoGenerateMipmapByTextureId[textureObject->GetExternalIndex()] = (param != 0.0f);
            break;
        case GL_DEPTH_STENCIL_TEXTURE_MODE:
            if (param != GL_DEPTH_COMPONENT && param != GL_STENCIL_INDEX) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "Invalid GL_DEPTH_STENCIL_TEXTURE_MODE value."));
                return;
            }
            textureObject->SetDepthStencilTextureMode(static_cast<GLenum>(param));
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("pname {} is not a valid texture parameter.", MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
    }

    void GetTextureParameterObjectiv_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                           GLenum pname, GLint* params, const char* caller) {
        if (!textureObject || !params) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            *params = (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(textureObject->GetSamplerObject()->GetMagFilter(),
                                                                       SamplerMipmapMode::None);
            break;
        case GL_TEXTURE_MIN_FILTER:
            *params = (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(
                textureObject->GetSamplerObject()->GetMinFilter(), textureObject->GetSamplerObject()->GetMipmapMode());
            break;
        case GL_TEXTURE_MIN_LOD:
            *params = static_cast<GLint>(textureObject->GetSamplerObject()->GetMinLod());
            break;
        case GL_TEXTURE_MAX_LOD:
            *params = static_cast<GLint>(textureObject->GetSamplerObject()->GetMaxLod());
            break;
        case GL_TEXTURE_BASE_LEVEL:
            *params = static_cast<GLint>(textureObject->GetLevelRange().x());
            break;
        case GL_TEXTURE_MAX_LEVEL:
            *params = static_cast<GLint>(textureObject->GetLevelRange().y());
            break;
        case GL_TEXTURE_IMMUTABLE_FORMAT:
            *params = textureObject->IsImmutable() ? GL_TRUE : GL_FALSE;
            break;
        case GL_TEXTURE_IMMUTABLE_LEVELS:
            *params = static_cast<GLint>(textureObject->GetImmutableLevels());
            break;
        case GL_TEXTURE_WRAP_S:
            *params = (GLint)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapS());
            break;
        case GL_TEXTURE_WRAP_T:
            *params = (GLint)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapT());
            break;
        case GL_TEXTURE_WRAP_R:
            *params = (GLint)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapR());
            break;
        case GL_TEXTURE_COMPARE_MODE:
            *params =
                (GLint)MG_Util::ConvertSamplerCompareModeToGLEnum(textureObject->GetSamplerObject()->GetCompareMode());
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            *params = (GLint)MG_Util::ConvertSamplerCompareFuncToGLEnum(
                textureObject->GetSamplerObject()->GetSamplerCompareFunc());
            break;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            *params = static_cast<GLint>(textureObject->GetSamplerObject()->GetMaxAnisotropy());
            break;
        case GL_DEPTH_STENCIL_TEXTURE_MODE:
            *params = static_cast<GLint>(textureObject->GetDepthStencilTextureMode());
            break;
        case GL_TEXTURE_LOD_BIAS:
            *params = static_cast<GLint>(textureObject->GetSamplerObject()->GetLodBias());
            break;
        case GL_TEXTURE_SWIZZLE_R:
        case GL_TEXTURE_SWIZZLE_G:
        case GL_TEXTURE_SWIZZLE_B:
        case GL_TEXTURE_SWIZZLE_A: {
            const auto component = static_cast<SizeT>(pname - GL_TEXTURE_SWIZZLE_R);
            *params = static_cast<GLint>(
                MG_Util::ConvertTextureSwizzleParamToGLEnum(textureObject->GetAllSwizzleParams()[component]));
            break;
        }
        case GL_TEXTURE_TARGET:
            *params = static_cast<GLint>(MG_Util::ConvertTextureTargetToGLEnum(textureObject->GetTarget()));
            break;
        case GL_IMAGE_FORMAT_COMPATIBILITY_TYPE:
            *params = GL_IMAGE_FORMAT_COMPATIBILITY_BY_SIZE;
            break;
        // Texture views are not implemented; a texture that is not a view reports the defaults
        // GL 4.6 core table 23.17 gives (0 layers/levels of offset, and its own extent).
        case GL_TEXTURE_VIEW_MIN_LEVEL:
        case GL_TEXTURE_VIEW_MIN_LAYER:
            *params = 0;
            break;
        case GL_TEXTURE_VIEW_NUM_LEVELS:
        case GL_TEXTURE_VIEW_NUM_LAYERS:
            *params = 0;
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("pname {} is not a valid texture parameter.",
                                MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
    }

    const SharedPtr<MG_State::GLState::ITextureObject>& GetTextureObjectByTarget(
        TextureUploadTarget textureUploadTarget, TextureTarget textureTarget) {
        if (TextureImpl::IsProxyTextureTarget(textureUploadTarget)) {
            auto& textureObject =
                TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget);
            if (!TextureImpl::ValidateTextureObject(textureObject)) return nullTextureObject;
            return textureObject;
        } else {
            auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            auto& textureObject = bindingSlot.GetBoundObject();
            if (!TextureImpl::ValidateTextureObject(textureObject)) return nullTextureObject;
            return textureObject;
        }
    }

    // Texture-parameter lookups must not raise GL_INVALID_OPERATION when the default texture
    // (name 0) is bound: glTexParameter* on default textures is legal GL (the GL CTS state reset
    // sets swizzles/levels on texture 0 for every unit x target and expects glGetError() to stay
    // clean). Name 0 resolves to the target's real default texture object, so parameters set on
    // it are stored and queryable like on any texture.
    const SharedPtr<MG_State::GLState::ITextureObject>& GetTextureObjectByTargetForParameter(
        TextureUploadTarget textureUploadTarget, TextureTarget textureTarget) {
        if (TextureImpl::IsProxyTextureTarget(textureUploadTarget)) {
            return TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget);
        }
        if (textureTarget == TextureTarget::Unknown) {
            return nullTextureObject;
        }
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        return activeUnit.GetBindingSlot(textureTarget).GetBoundObject();
    }

    void GenerateMipmap_Backend(GLenum target) {
        MG_Backend::gBackendFunctionsTable.GL.GenerateMipmap(target);
    }

    void MaybeAutoGenerateMipmap(GLenum target, const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                 Bool isProxy, GLint level) {
        if (isProxy || level != 0 || !textureObject) {
            return;
        }
        const auto it = g_autoGenerateMipmapByTextureId.find(textureObject->GetExternalIndex());
        if (it == g_autoGenerateMipmapByTextureId.end() || !it->second) {
            return;
        }
        GenerateMipmap_Backend(target);
    }

    // GL 4.6 core 8.5: sourcing an upload from a bound PIXEL_UNPACK_BUFFER adds three
    // INVALID_OPERATION conditions that do not exist for client memory. `pixels` is a byte offset
    // into that buffer, not a pointer. Returns true when no unpack buffer is bound, so every caller
    // can run it unconditionally.
    Bool ValidatePixelUnpackBufferSource(const void* pixels, TextureInputFormat inputFormat,
                                         TexturePixelDataType dataType, IntVec3 dimension, const char* caller) {
        const auto& unpackBuffer =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (!unpackBuffer) return true;

        // Persistent mappings remain legal transfer sources, as on the pack side in ReadPixels.
        if (unpackBuffer->IsMapped() && !(unpackBuffer->GetMappingAccess() & BufferMappingAccessBit::Persistent)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Pixel unpack buffer is currently mapped."));
            return false;
        }

        const SizeT offset = reinterpret_cast<SizeT>(pixels);
        const SizeT typeSize = MG_Util::GetTexturePixelDataTypeSize(dataType);
        if (typeSize != 0 && (offset % typeSize) != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Pixel unpack buffer offset must be a multiple of the size of a datum "
                                             "of the given type."));
            return false;
        }

        // The tightly packed span is the smallest the unpack can read, so a request that overruns
        // even this one certainly overruns the store; pixel store parameters only ever widen it.
        const SizeT bufferSize = unpackBuffer->GetSize();
        const SizeT required = MG_Util::CalculateInputTextureImageSize(inputFormat, dataType, dimension);
        if (offset > bufferSize || required > bufferSize - offset) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Unpacking would read past the end of the pixel unpack buffer."));
            return false;
        }

        return true;
    }

    // The same rules for the COMPRESSED entry points, whose payload size is the imageSize the
    // caller passed rather than something derived from a (format, type) pair - and which have no
    // datum size, so the alignment rule above does not apply to them. Shared by
    // glCompressedTexImage2D and glCompressedTexSubImage2D so the two cannot drift; the point
    // that is easy to get wrong and that KHR-GL44.buffer_storage.map_persistent_texture exists to
    // check is the first one: a PERSISTENT mapping stays a legal transfer source.
    Bool ValidateCompressedUnpackBufferSource(const void* data, SizeT imageSize, const char* caller) {
        const auto& unpackBuffer =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (!unpackBuffer) return true;

        if (unpackBuffer->IsMapped() && !(unpackBuffer->GetMappingAccess() & BufferMappingAccessBit::Persistent)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Pixel unpack buffer is currently mapped."));
            return false;
        }

        const SizeT offset = reinterpret_cast<SizeT>(data);
        const SizeT bufferSize = unpackBuffer->GetSize();
        if (offset > bufferSize || imageSize > bufferSize - offset) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Unpacking would read past the end of the pixel unpack buffer."));
            return false;
        }
        return true;
    }

    // Where a compressed upload reads its blocks from: `data` is an offset into the bound unpack
    // buffer when there is one, and a client pointer otherwise. Only meaningful once
    // ValidateCompressedUnpackBufferSource has passed. Null means there is nothing to read, which
    // GL leaves undefined and which callers must not dereference.
    const void* CompressedUnpackSource(const void* data) {
        const auto& unpackBuffer =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (!unpackBuffer) return data;
        return reinterpret_cast<const char*>(unpackBuffer->MappedData()) + reinterpret_cast<SizeT>(data);
    }

    void TexSubImage3D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                             GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels) {
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);

        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, height)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, depth)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!TextureImpl::ValidateTextureSubImageOffsets(textureObject, xoffset, width, yoffset, height, zoffset,
                                                         depth))
            return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureObject->GetFormat(),
                                                                           texturePixelDataType))
            return;
        if (!ValidatePixelUnpackBufferSource(pixels, textureInputFormat, texturePixelDataType, {width, height, depth},
                                             __func__))
            return;

        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (static_cast<Uint>(level) >= textureMipmapObject->GetMipmapLevelCount()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture level is out of range."));
            return;
        }

        const void* originalPixels = pixels;
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->MappedData()) +
                             reinterpret_cast<SizeT>(pixels);
        }
        if (!originalPixels) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "No data supplied from pixels parameter and no PBO bound."));
            return;
        }

        SizeT inputSize = 0;
        void* processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, MG_State::pGLContext->GetPixelStoreParameters(true), textureObject->GetFormat(),
            textureInputFormat, texturePixelDataType, {width, height, depth}, false, inputSize);
        if (!processedPixels || inputSize == 0) {
            if (processedPixels) free(processedPixels);
            return;
        }

        const auto texelSize = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level);
        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureObject->GetFormat(), texturePixelDataType);
        const SizeT srcRowSize = static_cast<SizeT>(width) * internalBpp;
        const SizeT srcSliceSize = static_cast<SizeT>(height) * srcRowSize;
        const SizeT destRowSize = static_cast<SizeT>(texelSize.x()) * internalBpp;
        const SizeT destSliceSize = static_cast<SizeT>(texelSize.y()) * destRowSize;

        if (xoffset + width > static_cast<GLsizei>(texelSize.x()) ||
            yoffset + height > static_cast<GLsizei>(texelSize.y()) ||
            zoffset + depth > static_cast<GLsizei>(texelSize.z())) {
            MGLOG_E_ONCE("TexSubImage3D_State: Specified region exceeds texture level dimensions");
            free(processedPixels);
            return;
        }

        const auto* srcData = static_cast<const Uint8*>(processedPixels);
        Uint8* destData = static_cast<Uint8*>(textureMipmapObject->MapMipmapData(textureUploadTarget, level));
        if (destData) {
            for (GLsizei z = 0; z < depth; ++z) {
                for (GLsizei y = 0; y < height; ++y) {
                    const SizeT destRowOffset =
                        static_cast<SizeT>(zoffset + z) * destSliceSize +
                        static_cast<SizeT>(yoffset + y) * destRowSize +
                        static_cast<SizeT>(xoffset) * internalBpp;
                    const SizeT srcRowOffset =
                        static_cast<SizeT>(z) * srcSliceSize + static_cast<SizeT>(y) * srcRowSize;
                    Memcpy(destData + destRowOffset, srcData + srcRowOffset, srcRowSize);
                }
            }
        }

        free(processedPixels);
        textureMipmapObject->MarkStorageDirtyRegion(textureUploadTarget, level, {xoffset, yoffset, zoffset},
                                                    {width, height, depth});
        MaybeAutoGenerateMipmap(target, textureObject, false, level);
    }

    void TexSubImage2D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                             GLenum format, GLenum type, const void* pixels) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        //            TextureInternalFormat textureInternalFormat =
        //            MG_Util::ConvertGLEnumToTextureInternalFormat(format);
        MGLOG_D("TexSubImage2D_State: target = %s, level = %d, (%d, %d), format = %s, pixels = %p",
                MG_Util::ConvertGLEnumToString(target).c_str(), level, width, height,
                MG_Util::ConvertTextureInputFormatToString(textureInputFormat).c_str(), pixels);
        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, height)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, 1)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        if (!ValidatePixelUnpackBufferSource(pixels, textureInputFormat, texturePixelDataType, {width, height, 1},
                                             __func__))
            return;

        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        TextureInternalFormat textureInternalFormat = textureObject->GetFormat();
        MGLOG_D("%s: working on texture %d", __func__, textureObject->GetExternalIndex());

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureSubImageOffsets(textureObject, xoffset, width, yoffset, height)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureInternalFormat,
                                                                           texturePixelDataType))
            return;

        // ======================= Processing ================================
        // Texture object here should always be an object with mipmap
        // Assert this for extra safety.
        // This should automatically compiled out in release,
        // so that we don't take the perf hit of dyn-cast.
        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (static_cast<Uint>(level) >= textureMipmapObject->GetMipmapLevelCount()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture level is out of range."));
            return;
        }
        auto texelSize = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level);

        SizeT inputSize = 0;

        const void* originalPixels = pixels;

        // PBO
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            MGLOG_D("TexSubImage2D_State: Using Pixel Unpack Buffer Object ID: %u",
                    pixelUnpackBufferObject->GetExternalIndex());
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->MappedData()) +
                             reinterpret_cast<SizeT>(pixels);
        }

        if (!originalPixels) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "No data supplied from pixels parameter and no PBO bound."));
            return;
        }
        const auto& unpackParams = MG_State::pGLContext->GetPixelStoreParameters(true);
        void* processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, unpackParams, textureInternalFormat, textureInputFormat, texturePixelDataType,
            {width, height, 1}, false, inputSize);

        if (!processedPixels || inputSize == 0) {
            MGLOG_E_ONCE("TexSubImage2D_State: Failed to process pixel data for TexSubImage2D, width: %d, height: %d", width,
                    height);
            if (processedPixels) free(processedPixels);
            return;
        }

        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureInternalFormat, texturePixelDataType);

        const SizeT srcRowSize = static_cast<SizeT>(width) * internalBpp;
        const SizeT srcStride = srcRowSize;
        const SizeT destRowSize = static_cast<SizeT>(texelSize.x()) * internalBpp;

        if (xoffset + width > static_cast<GLsizei>(texelSize.x()) ||
            yoffset + height > static_cast<GLsizei>(texelSize.y())) {
            MGLOG_E_ONCE("TexSubImage2D_State: Specified region exceeds texture dimensions");
            free(processedPixels);
            return;
        }

        const auto* srcData = static_cast<const Uint8*>(processedPixels);
        Uint8* destData = static_cast<Uint8*>(textureMipmapObject->MapMipmapData(textureUploadTarget, level));

        if (destData) {
            for (GLsizei y = 0; y < height; y++) {
                const SizeT destRowOffset = (yoffset + y) * destRowSize + xoffset * internalBpp;
                const SizeT srcRowOffset = y * srcStride;
                Memcpy(destData + destRowOffset, srcData + srcRowOffset, srcRowSize);
            }
        }

        free(processedPixels);

        MGLOG_D("%s: mark mip %d as dirty", __func__, level);
        textureMipmapObject->MarkStorageDirtyRegion(textureUploadTarget, level, {xoffset, yoffset, 0},
                                                    {width, height, 1});
        MaybeAutoGenerateMipmap(target, textureObject, false, level);
    }

    void TexSubImage1D_State(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                             const GLvoid* pixels) {
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);

        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, 1)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, 1, 1)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!TextureImpl::ValidateTextureSubImageOffsets(textureObject, xoffset, width)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureObject->GetFormat(),
                                                                           texturePixelDataType))
            return;
        if (!ValidatePixelUnpackBufferSource(pixels, textureInputFormat, texturePixelDataType, {width, 1, 1}, __func__))
            return;

        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        const void* originalPixels = pixels;
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->MappedData()) +
                             reinterpret_cast<SizeT>(pixels);
        }
        if (!originalPixels) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "No data supplied from pixels parameter and no PBO bound."));
            return;
        }

        SizeT inputSize = 0;
        void* processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, MG_State::pGLContext->GetPixelStoreParameters(true), textureObject->GetFormat(),
            textureInputFormat, texturePixelDataType, {width, 1, 1}, false, inputSize);
        if (!processedPixels || inputSize == 0) {
            if (processedPixels) free(processedPixels);
            return;
        }

        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureObject->GetFormat(), texturePixelDataType);
        const SizeT copySize = static_cast<SizeT>(width) * internalBpp;
        Uint8* destData = static_cast<Uint8*>(textureMipmapObject->MapMipmapData(textureUploadTarget, level));
        if (destData) {
            Memcpy(destData + static_cast<SizeT>(xoffset) * internalBpp, processedPixels, copySize);
        }

        free(processedPixels);
        textureMipmapObject->MarkStorageDirtyRegion(textureUploadTarget, level, {xoffset, 0, 0}, {width, 1, 1});
        MaybeAutoGenerateMipmap(target, textureObject, false, level);
    }

    // TexParameteriv/TexParameterfv are introduced in OpenGL 4.0, so do not support them for now.
    void TexParameterf_State(GLenum target, GLenum pname, GLfloat param) {

        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ======================= Processing ================================
        auto& textureObject = GetTextureObjectByTargetForParameter(textureUploadTarget, textureTarget);
        if (!textureObject) return;

        // The per-object setters run this before writing, and glTexParameterfv/glTextureParameterfv
        // funnel everything that is not a vector pname down here - so without it the float forms of
        // the setter accepted sampler state on a multisample texture, a mipmapping filter on a
        // rectangle texture and a negative base level, all of which the integer forms rejected.
        const GLint validationParam = pname == GL_TEXTURE_MAX_ANISOTROPY_EXT ? 1 : static_cast<GLint>(param);
        if (!ValidateTextureParameterForTarget(textureObject, pname, validationParam, __func__)) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            textureObject->GetSamplerObject()->SetMagFilter(MG_Util::ConvertGLEnumToSamplerFilterMode((GLenum)param));
            break;
        case GL_TEXTURE_MIN_FILTER:
            textureObject->GetSamplerObject()->SetMinFilter(MG_Util::ConvertGLEnumToSamplerFilterMode((GLenum)param));
            textureObject->GetSamplerObject()->SetMipmapMode(MG_Util::ConvertGLEnumToSamplerMipmapMode((GLenum)param));
            break;
        case GL_TEXTURE_MIN_LOD: {
            Float maxLod = textureObject->GetSamplerObject()->GetMaxLod();
            textureObject->GetSamplerObject()->SetLodRange(param, maxLod);
            break;
        }
        case GL_TEXTURE_MAX_LOD: {
            Float minLod = textureObject->GetSamplerObject()->GetMinLod();
            textureObject->GetSamplerObject()->SetLodRange(minLod, param);
            break;
        }
        case GL_TEXTURE_BASE_LEVEL:
            textureObject->SetBaseLevel((Uint)param);
            break;
        case GL_TEXTURE_MAX_LEVEL:
            textureObject->SetMaxLevel((Uint)param);
            break;
        case GL_TEXTURE_SWIZZLE_R:
        case GL_TEXTURE_SWIZZLE_G:
        case GL_TEXTURE_SWIZZLE_B:
        case GL_TEXTURE_SWIZZLE_A: {
            auto swizzleParam = MG_Util::ConvertGLEnumPnameToTextureSwizzleParam(pname);
            auto swizzleValue = MG_Util::ConvertGLEnumToTextureSwizzleParam((GLenum)param);
            if (swizzleValue == TextureSwizzleParam::Unknown) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Invalid texture swizzle value."));
                return;
            }
            textureObject->SetSwizzleParam(swizzleParam, swizzleValue);
            break;
        }
        case GL_TEXTURE_WRAP_S:
            textureObject->GetSamplerObject()->SetWrapS(MG_Util::ConvertGLEnumToSamplerWrapMode((GLenum)param));
            break;
        case GL_TEXTURE_WRAP_T:
            textureObject->GetSamplerObject()->SetWrapT(MG_Util::ConvertGLEnumToSamplerWrapMode((GLenum)param));
            break;
        case GL_TEXTURE_WRAP_R:
            textureObject->GetSamplerObject()->SetWrapR(MG_Util::ConvertGLEnumToSamplerWrapMode((GLenum)param));
            break;
        case GL_TEXTURE_COMPARE_MODE:
            textureObject->GetSamplerObject()->SetCompareMode(
                MG_Util::ConvertGLEnumToSamplerCompareMode((GLenum)param));
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            textureObject->GetSamplerObject()->SetSamplerCompareFunc(
                MG_Util::ConvertGLEnumToSamplerCompareFunc((GLenum)param));
            break;
        case GL_TEXTURE_LOD_BIAS:
            textureObject->GetSamplerObject()->SetLodBias(param);
            break;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            if (!ValidateMaxAnisotropy(param, __func__)) return;
            textureObject->GetSamplerObject()->SetMaxAnisotropy(param);
            break;
        case GL_GENERATE_MIPMAP:
            g_autoGenerateMipmapByTextureId[textureObject->GetExternalIndex()] = (param != 0.0f);
            break;
        case GL_TEXTURE_SWIZZLE_RGBA:
            // Not supported in this function
        case GL_TEXTURE_BORDER_COLOR:
            // Not supported in this function
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    std::format("pname {} is not a valid texture parameter.", MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
    }

    void TexParameteri_State(GLenum target, GLenum pname, GLint param) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ======================= Processing ================================
        auto& textureObject = GetTextureObjectByTargetForParameter(textureUploadTarget, textureTarget);
        if (!textureObject) return;

        TextureParameterObject_State(textureObject, pname, param, __func__);
    }

    // Quick and dirty TexParameter*v implementation to make NeoForge happy.
    // TODO: implement the missing part
    void TexParameterfv_State(GLenum target, GLenum pname, const GLfloat* params) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR: {
            // ======================= Converting ================================
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

            // ======================= Processing ================================
            auto& textureObject = GetTextureObjectByTargetForParameter(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            SetTextureBorderColorFromFloats(textureObject, params);
            break;
        }
        case GL_TEXTURE_SWIZZLE_RGBA: {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& textureObject = GetTextureObjectByTargetForParameter(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            GLint signedParams[4] = {static_cast<GLint>(params[0]), static_cast<GLint>(params[1]),
                                     static_cast<GLint>(params[2]), static_cast<GLint>(params[3])};
            if (!SetTextureSwizzleParamsFromInts(textureObject, signedParams, __func__)) {
                return;
            }
            break;
        }
        default:
            TexParameterf_State(target, pname, *params);
            break;
        }
    }

    void TexParameteriv_State(GLenum target, GLenum pname, const GLint* params) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR: {
            // ======================= Converting ================================
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

            // ======================= Processing ================================
            auto& textureObject = GetTextureObjectByTargetForParameter(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            SetTextureBorderColorFromInts(textureObject, params);
            break;
        }
        case GL_TEXTURE_SWIZZLE_RGBA: {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& textureObject = GetTextureObjectByTargetForParameter(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            if (!SetTextureSwizzleParamsFromInts(textureObject, params, __func__)) {
                return;
            }
            break;
        }
        default:
            TexParameteri_State(target, pname, *params);
            break;
        }
    }

    void TexParameterIiv_State(GLenum target, GLenum pname, const GLint* params) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR: {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& textureObject = GetTextureObjectByTargetForParameter(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            SetTextureBorderColorFromIntegerInts(textureObject, params);
            break;
        }
        case GL_TEXTURE_SWIZZLE_RGBA: {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& textureObject = GetTextureObjectByTargetForParameter(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            GLint signedParams[4] = {static_cast<GLint>(params[0]), static_cast<GLint>(params[1]),
                                     static_cast<GLint>(params[2]), static_cast<GLint>(params[3])};
            if (!SetTextureSwizzleParamsFromInts(textureObject, signedParams, __func__)) {
                return;
            }
            break;
        }
        default:
            TexParameteri_State(target, pname, *params);
            break;
        }
    }

    void TexParameterIuiv_State(GLenum target, GLenum pname, const GLuint* params) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR: {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& textureObject = GetTextureObjectByTargetForParameter(textureUploadTarget, textureTarget);
            if (!textureObject) return;
            SetTextureBorderColorFromUnsignedInts(textureObject, params);
            break;
        }
        case GL_TEXTURE_SWIZZLE_RGBA: {
            // ======================= Converting ================================
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

            // ======================= Processing ================================
            auto& textureObject = GetTextureObjectByTargetForParameter(textureUploadTarget, textureTarget);
            if (!textureObject) return;

            Vec4<TextureSwizzleParam> swizzleParams;
            for (int i = 0; i < 4; i++) {
                swizzleParams[i] = MG_Util::ConvertGLEnumToTextureSwizzleParam(static_cast<GLint>(params[i]));
                if (TextureSwizzleParam::Unknown == swizzleParams[i]) {
                    MG_State::pGLContext->RecordError(
                        ErrorCode::InvalidEnum,
                        MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`params` is not valid."));
                    return;
                }
            }
            textureObject->SetSwizzleParamRGBA(swizzleParams);
            break;
        }
        default:
            TexParameteri_State(target, pname, static_cast<GLint>(*params));
            break;
        }
    }

    Bool TexImage3DMultisample_State(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);

        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return false;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return false;
        if (textureTarget != TextureTarget::Texture2DMultisampleArray) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Target must be GL_TEXTURE_2D_MULTISAMPLE_ARRAY or its proxy."));
            return false;
        }

        textureInternalFormat = MG_Util::ConvertInternalFormatToSized(textureInternalFormat, TextureInputFormat::RGBA,
                                                                      TexturePixelDataType::UnsignedByte);
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return false;
        if (!ValidateTextureMultisampleStorage(textureTarget, samples, width, height, depth, textureInternalFormat,
                                               __func__))
            return false;

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        const Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();
        // Name 0 resolves to the target's default texture object - a real texture this call
        // (re)specifies like any other; the slot is never empty anymore.
        if (!TextureImpl::ValidateTextureObject(textureObject)) return false;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return false;
        }

        AllocateMultisampleTextureStorage(textureObject, textureUploadTarget, textureInternalFormat, samples, width,
                                          height, depth, fixedsamplelocations);
        return true;
    }

    Bool TexImage2DMultisample_State(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLboolean fixedsamplelocations) {
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);

        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return false;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return false;
        if (textureTarget != TextureTarget::Texture2DMultisample) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Target must be GL_TEXTURE_2D_MULTISAMPLE or its proxy."));
            return false;
        }

        textureInternalFormat = MG_Util::ConvertInternalFormatToSized(textureInternalFormat, TextureInputFormat::RGBA,
                                                                      TexturePixelDataType::UnsignedByte);
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return false;
        if (!ValidateTextureMultisampleStorage(textureTarget, samples, width, height, 1, textureInternalFormat,
                                               __func__))
            return false;

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        const Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();
        // Name 0 resolves to the target's default texture object - a real texture this call
        // (re)specifies like any other; the slot is never empty anymore.
        if (!TextureImpl::ValidateTextureObject(textureObject)) return false;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return false;
        }

        AllocateMultisampleTextureStorage(textureObject, textureUploadTarget, textureInternalFormat, samples, width,
                                          height, 1, fixedsamplelocations);
        return true;
    }

    void TexImage3D_State(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                          GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels) {
        MGLOG_D(
            "%s called with target: %s, level: %d, internalformat: %s, width: %d, height: %d, depth: %d, "
            "border: %d, format: %s, type: %s (%u), pixels: %p",
            __func__,
            MG_Util::ConvertTextureUploadTargetToString(MG_Util::ConvertGLEnumToTextureUploadTarget(target)).c_str(),
            level,
            MG_Util::ConvertTextureInternalFormatToString(MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat))
                .c_str(),
            width, height, depth, border,
            MG_Util::ConvertTextureInputFormatToString(MG_Util::ConvertGLEnumToTextureInputFormat(format)).c_str(),
            MG_Util::ConvertTexturePixelDataTypeToString(MG_Util::ConvertGLEnumToTexturePixelDataType(type)).c_str(),
            type, pixels);
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, height)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, depth)) return;
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (!TextureImpl::ValidateTextureBorderNumber(border)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureInternalFormat,
                                                                           texturePixelDataType))
            return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        // Depth and depth-stencil formats are not three-dimensional in core GL (2D-array targets are fine).
        if ((textureUploadTarget == TextureUploadTarget::Texture3D ||
             textureUploadTarget == TextureUploadTarget::ProxyTexture3D) &&
            (textureInputFormat == TextureInputFormat::DepthComponent ||
             textureInputFormat == TextureInputFormat::DepthStencil)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Depth formats are invalid for 3D texture targets"));
            return;
        }

        // RGTC is a 2D-only compression scheme, so a 3D target rejects it. This has to be tested on
        // the raw enum: the RGTC formats resolve to plain R8/RG8/SNORM storage on the way in (see
        // GLToMG's TextureEnumConverter), so once the internal format is converted there is nothing
        // left to distinguish them from an ordinary one- or two-channel upload.
        if ((textureUploadTarget == TextureUploadTarget::Texture3D ||
             textureUploadTarget == TextureUploadTarget::ProxyTexture3D) &&
            (internalformat == GL_COMPRESSED_RED_RGTC1 || internalformat == GL_COMPRESSED_SIGNED_RED_RGTC1 ||
             internalformat == GL_COMPRESSED_RG_RGTC2 || internalformat == GL_COMPRESSED_SIGNED_RG_RGTC2)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "RGTC compressed formats are invalid for 3D texture targets"));
            return;
        }

        // TODO: GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the
        // GL_PIXEL_UNPACK_BUFFER target and the buffer object's data store is currently mapped.
        // GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the GL_PIXEL_UNPACK_BUFFER
        // target and the data would be unpacked from the buffer object such that the memory reads required would
        // exceed the data store size.
        // GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the GL_PIXEL_UNPACK_BUFFER
        // target and data is not evenly divisible into the number of bytes needed to store in memory a datum
        // indicated by type.
        // ======================= Processing ================================
        textureInternalFormat =
            MG_Util::ConvertInternalFormatToSized(textureInternalFormat, textureInputFormat, texturePixelDataType);
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        // ===================== Error Checking ==============================
        // Name 0 resolves to the target's default texture object - a real texture this call
        // (re)specifies like any other; the slot is never empty anymore.
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!ValidateTextureMutable(textureObject, __func__)) return;

        // ======================= Processing ================================
        if (internalformat == GL_ALPHA || format == GL_ALPHA) {
            textureObject->SetSwizzleParamRGBA({TextureSwizzleParam::Zero, TextureSwizzleParam::Zero,
                                                TextureSwizzleParam::Zero, TextureSwizzleParam::Red});
        }

        SizeT imageSize = 0;
        const SizeT inputBpp = MG_Util::GetInputBytesPerPixel(textureInputFormat, texturePixelDataType);
        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureInternalFormat, texturePixelDataType);
        const SizeT internalBytes = width * height * depth * internalBpp;

        textureObject->SetInternalFormat(textureInternalFormat);

        const void* originalPixels = pixels;

        // PBO
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            MGLOG_D("%s: Using Pixel Unpack Buffer Object ID: %u", __func__,
                    pixelUnpackBufferObject->GetExternalIndex());
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->MappedData()) +
                             reinterpret_cast<SizeT>(pixels);
        }

        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        // Allocate in TextureObject
        if (isProxy) {
            MGLOG_D("%s: isProxy = true, not allocating", __func__);
        } else {
            DiscardMipmapChainOnBaseRespecification(textureMipmapObject, textureUploadTarget, level);
            textureMipmapObject->AllocateStorage(textureUploadTarget, level, {{width, height, depth}, internalBytes});
            // The same specific-compressed-format tag glTexImage2D records (see TexImage2D_State):
            // GL 4.6 core 8.5 commits the level to that format, so GL_TEXTURE_COMPRESSED and
            // GL_TEXTURE_INTERNAL_FORMAT must report it - and, less obviously, glCopyImageSubData
            // sizes the level's texel BLOCK from it. Without the tag a GL_COMPRESSED_RG_RGTC2
            // array level measured as the RG8 storage it resolved to, 2 bytes instead of 16, and
            // the copy-compatibility rule refused a pairing 18.3.2 requires. AllocateStorage above
            // clears the tag, so this has to follow it.
            const auto compressedInfo = MG_Util::GetCompressedFormatInfo(static_cast<GLenum>(internalformat));
            if (compressedInfo.blockWidth != 0) {
                textureMipmapObject->SetMipmapCompressedImage(
                    textureUploadTarget, level, static_cast<GLenum>(internalformat), nullptr,
                    MG_Util::CalculateCompressedTextureImageSize(compressedInfo, {width, height, depth}));
            }
        }

        if (!originalPixels) {
            MGLOG_D("%s: No input pixel and no PBO bound, no pixel transfer", __func__);
            return;
        }

        void* processedPixels = nullptr;
        processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, MG_State::pGLContext->GetPixelStoreParameters(true), textureInternalFormat,
            textureInputFormat, texturePixelDataType, {width, height, depth}, false, imageSize);

        if (processedPixels && imageSize > 0) {
            if (imageSize != internalBytes) {
                MGLOG_W_ONCE("%s: Processed pixel data size (%zu) does not match expected size (%zu). "
                        "This may indicate an alignment or processing issue.",
                        __func__, imageSize, internalBytes);
            }

            const SizeT copySize = std::min(imageSize, internalBytes);
            DataPtr texelInput{processedPixels, copySize};
            textureMipmapObject->UpdateMipmapSubData(textureUploadTarget, level, texelInput);
        }

        textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);

        free(processedPixels);
        MaybeAutoGenerateMipmap(target, textureObject, isProxy, level);
    }

    void TexImage2D_State(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                          GLenum format, GLenum type, const void* pixels) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        MGLOG_D("%s called with target: %s (%s), level: %d, internalformat: %s (%s), width: %d, height: %d, "
                "border: %d, format: %s (%s), type: %s (%s), pixels: %p",
                __func__, MG_Util::ConvertTextureUploadTargetToString(textureUploadTarget).c_str(),
                MG_Util::ConvertGLEnumToString(target).c_str(), level,
                MG_Util::ConvertTextureInternalFormatToString(textureInternalFormat).c_str(),
                MG_Util::ConvertGLEnumToString(internalformat).c_str(), width, height, border,
                MG_Util::ConvertTextureInputFormatToString(textureInputFormat).c_str(),
                MG_Util::ConvertGLEnumToString(format).c_str(),
                MG_Util::ConvertTexturePixelDataTypeToString(texturePixelDataType).c_str(),
                MG_Util::ConvertGLEnumToString(type).c_str(), pixels);
        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, height)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, 1)) return;
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (!TextureImpl::ValidateTextureBorderNumber(border)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureInternalFormat,
                                                                           texturePixelDataType))
            return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        // TODO: GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the
        // GL_PIXEL_UNPACK_BUFFER target and the buffer object's data store is currently mapped.
        // GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the GL_PIXEL_UNPACK_BUFFER
        // target and the data would be unpacked from the buffer object such that the memory reads required would
        // exceed the data store size.
        // GL_INVALID_OPERATION is generated if a non-zero buffer object name is bound to the GL_PIXEL_UNPACK_BUFFER
        // target and data is not evenly divisible into the number of bytes needed to store in memory a datum
        // indicated by type.

        // ======================= Processing ================================
        textureInternalFormat =
            MG_Util::ConvertInternalFormatToSized(textureInternalFormat, textureInputFormat, texturePixelDataType);
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        // ===================== Error Checking ==============================
        // Name 0 resolves to the target's default texture object - a real texture this call
        // (re)specifies like any other; the slot is never empty anymore.
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!ValidateTextureMutable(textureObject, __func__)) return;

        // ======================= Processing ================================
        if (internalformat == GL_ALPHA || format == GL_ALPHA) {
            textureObject->SetSwizzleParamRGBA({TextureSwizzleParam::Zero, TextureSwizzleParam::Zero,
                                                TextureSwizzleParam::Zero, TextureSwizzleParam::Red});
        }

        SizeT imageSize = 0;
        const SizeT inputBpp = MG_Util::GetInputBytesPerPixel(textureInputFormat, texturePixelDataType);
        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureInternalFormat, texturePixelDataType);
        const SizeT internalBytes = width * height * internalBpp;

        MGLOG_D("%s: working on texture %d", __func__, textureObject->GetExternalIndex());

        MGLOG_D("%s: texture object had internal format %s, new format %s", __func__,
                MG_Util::ConvertTextureInternalFormatToString(textureObject->GetFormat()).c_str(),
                MG_Util::ConvertTextureInternalFormatToString(textureInternalFormat).c_str());
        textureObject->SetInternalFormat(textureInternalFormat);

        const void* originalPixels = pixels;

        // PBO
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            MGLOG_D("%s: Using Pixel Unpack Buffer Object ID: %u", __func__,
                    pixelUnpackBufferObject->GetExternalIndex());
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->MappedData()) +
                             reinterpret_cast<SizeT>(pixels);
        }

        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        // Allocate in TextureObject
        if (isProxy) {
            MGLOG_D("%s: isProxy = true, not allocating", __func__);
        } else {
            MGLOG_D("%s: Allocating %d bytes at mip %d", __func__, internalBytes, level);
            DiscardMipmapChainOnBaseRespecification(textureMipmapObject, textureUploadTarget, level);
            textureMipmapObject->AllocateStorage(textureUploadTarget, level,
                                                 {{width, height, 1}, internalBytes});
            // GL 4.6 core 8.5: a SPECIFIC compressed internalformat (unlike a generic
            // GL_COMPRESSED_* one, where the implementation is free to choose) commits the
            // level to that format - GL_TEXTURE_COMPRESSED must then answer true for it and
            // GL_TEXTURE_INTERNAL_FORMAT must report it, which is how an application asks for
            // the size to hand glCompressedTexSubImage2D afterwards. Only the tag and the size
            // are recorded: there is no BC/ETC codec here, so the texel shadow keeps the
            // uncompressed storage this format resolved to (which is also what lets the level
            // sample as the application's texels), and the compressed image the tag describes
            // is zero-filled - the one reproducible answer glGetCompressedTexImage can give for
            // an image nothing ever compressed. AllocateStorage above clears the tag, so this
            // has to follow it.
            const auto compressedInfo = MG_Util::GetCompressedFormatInfo(static_cast<GLenum>(internalformat));
            if (compressedInfo.blockWidth != 0) {
                textureMipmapObject->SetMipmapCompressedImage(
                    textureUploadTarget, level, static_cast<GLenum>(internalformat), nullptr,
                    MG_Util::CalculateCompressedTextureImageSize(compressedInfo, {width, height, 1}));
            }
        }

        if (!originalPixels) {
            MGLOG_D("%s: No input pixel and no PBO bound, no pixel transfer", __func__);
            return;
        }

        void* processedPixels = nullptr;
        processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, MG_State::pGLContext->GetPixelStoreParameters(true), textureInternalFormat,
            textureInputFormat, texturePixelDataType, {width, height, 1}, false, imageSize);

        if (processedPixels && imageSize > 0) {
            if (imageSize != internalBytes) {
                MGLOG_W_ONCE("TexImage2D_State: Processed pixel data size (%zu) does not match expected size (%zu). "
                        "This may indicate an alignment or processing issue.",
                        imageSize, internalBytes);
            }

            const SizeT copySize = std::min(imageSize, internalBytes);
            DataPtr texelInput{processedPixels, copySize};
            textureMipmapObject->UpdateMipmapSubData(textureUploadTarget, level, texelInput);
        }

        free(processedPixels);

        MGLOG_D("%s: mark mip %d as dirty", __func__, level);
        textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);
        MaybeAutoGenerateMipmap(target, textureObject, isProxy, level);
    }

    void TexImage1D_State(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLint border, GLenum format,
                          GLenum type, const GLvoid* pixels) {
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalFormat);

        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, 1)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, 1, 1)) return;
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        if (!TextureImpl::ValidateTextureBorderNumber(border)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureInternalFormat,
                                                                           texturePixelDataType))
            return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        textureInternalFormat =
            MG_Util::ConvertInternalFormatToSized(textureInternalFormat, textureInputFormat, texturePixelDataType);
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();
        // Name 0 resolves to the target's default texture object - a real texture this call
        // (re)specifies like any other; the slot is never empty anymore.
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!ValidateTextureMutable(textureObject, __func__)) return;

        if (internalFormat == GL_ALPHA || format == GL_ALPHA) {
            textureObject->SetSwizzleParamRGBA({TextureSwizzleParam::Zero, TextureSwizzleParam::Zero,
                                                TextureSwizzleParam::Zero, TextureSwizzleParam::Red});
        }

        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureInternalFormat, texturePixelDataType);
        const SizeT internalBytes = static_cast<SizeT>(width) * internalBpp;
        textureObject->SetInternalFormat(textureInternalFormat);

        const void* originalPixels = pixels;
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->MappedData()) +
                             reinterpret_cast<SizeT>(pixels);
        }

        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (!isProxy) {
            DiscardMipmapChainOnBaseRespecification(textureMipmapObject, textureUploadTarget, level);
            textureMipmapObject->AllocateStorage(textureUploadTarget, level, {{width, 1, 1}, internalBytes});
        }

        if (!originalPixels) {
            return;
        }

        SizeT imageSize = 0;
        void* processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, MG_State::pGLContext->GetPixelStoreParameters(true), textureInternalFormat,
            textureInputFormat, texturePixelDataType, {width, 1, 1}, false, imageSize);
        if (processedPixels && imageSize > 0) {
            DataPtr texelInput{processedPixels, std::min(imageSize, internalBytes)};
            textureMipmapObject->UpdateMipmapSubData(textureUploadTarget, level, texelInput);
        }

        free(processedPixels);
        textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);
        MaybeAutoGenerateMipmap(target, textureObject, isProxy, level);
    }

    // The work glTexBuffer[Range] and glTextureBuffer[Range] all share once the texture has been
    // resolved - by binding for the target forms, by name for the DSA ones. `size` is
    // kWholeBuffer for the non-Range entry points, which attach the buffer as it grows rather
    // than freezing the size it happens to have now.
    // The sized internal formats a buffer texture accepts (GL 4.6 core table 8.16). This is a much
    // shorter list than the renderable or texturable formats, so it cannot be inferred from either.
    Bool IsBufferTextureInternalFormat(GLenum internalformat) {
        switch (internalformat) {
        case GL_R8:
        case GL_R16:
        case GL_R16F:
        case GL_R32F:
        case GL_R8I:
        case GL_R16I:
        case GL_R32I:
        case GL_R8UI:
        case GL_R16UI:
        case GL_R32UI:
        case GL_RG8:
        case GL_RG16:
        case GL_RG16F:
        case GL_RG32F:
        case GL_RG8I:
        case GL_RG16I:
        case GL_RG32I:
        case GL_RG8UI:
        case GL_RG16UI:
        case GL_RG32UI:
        case GL_RGB32F:
        case GL_RGB32I:
        case GL_RGB32UI:
        case GL_RGBA8:
        case GL_RGBA16:
        case GL_RGBA16F:
        case GL_RGBA32F:
        case GL_RGBA8I:
        case GL_RGBA16I:
        case GL_RGBA32I:
        case GL_RGBA8UI:
        case GL_RGBA16UI:
        case GL_RGBA32UI:
            return true;
        default:
            return false;
        }
    }

    static void AttachBufferToTexture(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                      GLenum internalformat, GLuint buffer, GLintptr offset, SizeT size,
                                      const char* caller) {
        using MG_State::GLState::TextureObjectBuffer;
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        if (!IsBufferTextureInternalFormat(internalformat)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("internalformat 0x{:X} is not one of the sized formats a buffer texture accepts.",
                                internalformat)));
            return;
        }
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;

        auto& bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
        if (buffer != 0 && !bufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "`buffer` is not zero and is not the name of an existing buffer object."));
            return;
        }
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Buffer) {
            // A texture whose target is something else is a wrong object, not a wrong token
            // (GL 4.6 core 8.9).
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "The effective target of `texture` is not `GL_TEXTURE_BUFFER`."));
            return;
        }
        if (size != TextureObjectBuffer::kWholeBuffer) {
            // GL 4.6 core 8.9: offset must be non-negative and aligned to
            // GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT, and size must be positive.
            if (offset < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "offset must be non-negative."));
                return;
            }
            if (size == 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "size must be greater than zero."));
                return;
            }
            const Int alignment = std::max(
                1, MG_Backend::pActiveBackendObject->GetDynamicParameters().TextureBufferOffsetAlignment);
            if (offset % alignment != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "offset is not a multiple of GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT."));
                return;
            }
            // The range has to lie inside the buffer that is being attached. Detaching (buffer
            // zero) carries no range to check.
            if (bufferObject && static_cast<SizeT>(offset) + size > bufferObject->GetSize()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "offset + size is greater than the buffer object's GL_BUFFER_SIZE."));
                return;
            }
        }

        auto* texBufferObject = static_cast<TextureObjectBuffer*>(textureObject.get());
        texBufferObject->GetBufferBindingSlot().Bind(bufferObject);
        texBufferObject->SetBufferRange(static_cast<SizeT>(offset < 0 ? 0 : offset), size);
        texBufferObject->SetInternalFormat(textureInternalFormat);
    }

    void TexBuffer_State(GLenum target, GLenum internalformat, GLuint buffer) {
        // ======================= Converting ================================
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        // TODO: make sure `internalformat` is in one of supported format for TexBuffer
        // GL 3.3 core 3.8.5: buffer zero detaches any buffer from the buffer texture - only a
        // nonzero name that is not an existing buffer object is an error. This is reachable on
        // the default buffer texture (bound whenever texture 0 is bound to GL_TEXTURE_BUFFER),
        // which the GL CTS state reset detaches with glTexBuffer(..., 0) after every case.
        auto& bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
        if (buffer != 0 && !bufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "`buffer` is not zero and is not the name of an existing buffer object."));
            return;
        }

        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();

        // ===================== Error Checking ==============================
        // Name 0 is the default buffer texture - a real object the (de)attach operates on, not a
        // silent no-op; the slot is never empty now that every unit/target holds its default.
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Buffer) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "The effective target of `texture` is not `GL_TEXTURE_BUFFER`."));
            return;
        }

        // ======================= Processing ================================
        // Now we can rest assured, and down-cast texture object to texture buffer
        auto* texBufferObject = static_cast<MG_State::GLState::TextureObjectBuffer*>(textureObject.get());
        auto& bufferSlot = texBufferObject->GetBufferBindingSlot();
        bufferSlot.Bind(bufferObject);

        texBufferObject->SetBufferRange(0, MG_State::GLState::TextureObjectBuffer::kWholeBuffer);
        texBufferObject->SetInternalFormat(textureInternalFormat);
    }

    GLboolean IsTexture_State(GLuint texture) {
        // ======================= Processing ================================
        // GL 3.3 core 6.1.4: IsTexture generates no error - an unknown, deleted or merely reserved
        // name is just GL_FALSE. Probing with the recording validator (as every other Is* entry
        // point already avoids doing) would leave a spurious INVALID_VALUE behind.
        return MG_State::pGLContext->ValidateTextureObject(texture) ? GL_TRUE : GL_FALSE;
    }

    void GetTexParameterIuiv_State(GLenum target, GLenum pname, GLuint* params) {
        if (params == nullptr) return;

        if (pname == GL_TEXTURE_BORDER_COLOR) {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
            auto& textureObject =
                isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                        : bindingSlot.GetBoundObject();

            if (!TextureImpl::ValidateTextureObject(textureObject)) return;

            const auto& borderColor = textureObject->GetBorderColorUI();
            params[0] = borderColor.x();
            params[1] = borderColor.y();
            params[2] = borderColor.z();
            params[3] = borderColor.w();
            return;
        }

        GLint signedParams[4] = {0, 0, 0, 0};
        if (!GetTexParameteriv_State(target, pname, signedParams)) return;
        const int componentCount = pname == GL_TEXTURE_BORDER_COLOR || pname == GL_TEXTURE_SWIZZLE_RGBA ? 4 : 1;
        for (int i = 0; i < componentCount; ++i) {
            params[i] = static_cast<GLuint>(signedParams[i]);
        }
    }

    void GetTexParameterIiv_State(GLenum target, GLenum pname, GLint* params) {
        if (params == nullptr) return;

        if (pname == GL_TEXTURE_BORDER_COLOR) {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
            TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
            Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
            auto& textureObject =
                isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                        : bindingSlot.GetBoundObject();

            if (!TextureImpl::ValidateTextureObject(textureObject)) return;

            const auto& borderColor = textureObject->GetBorderColorI();
            params[0] = borderColor.x();
            params[1] = borderColor.y();
            params[2] = borderColor.z();
            params[3] = borderColor.w();
            return;
        }

        GetTexParameteriv_State(target, pname, params);
    }

    Bool GetTexParameteriv_State(GLenum target, GLenum pname, GLint* params) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        if (!TextureImpl::ValidateTextureObject(textureObject)) return false;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(
                    textureObject->GetSamplerObject()->GetMagFilter(), SamplerMipmapMode::None);
            }
            break;
        case GL_TEXTURE_MIN_FILTER:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(
                    textureObject->GetSamplerObject()->GetMinFilter(),
                    textureObject->GetSamplerObject()->GetMipmapMode());
            }
            break;
        case GL_TEXTURE_MIN_LOD:
            if (params) {
                *params = static_cast<GLint>(textureObject->GetSamplerObject()->GetMinLod());
            }
            break;
        case GL_TEXTURE_MAX_LOD:
            if (params) {
                *params = static_cast<GLint>(textureObject->GetSamplerObject()->GetMaxLod());
            }
            break;
        case GL_TEXTURE_BASE_LEVEL:
            if (params) {
                *params = static_cast<GLint>(textureObject->GetLevelRange().x());
            }
            break;
        case GL_TEXTURE_MAX_LEVEL:
            if (params) {
                *params = static_cast<GLint>(textureObject->GetLevelRange().y());
            }
            break;
        case GL_TEXTURE_IMMUTABLE_FORMAT:
            if (params) {
                *params = textureObject->IsImmutable() ? GL_TRUE : GL_FALSE;
            }
            break;
        case GL_TEXTURE_IMMUTABLE_LEVELS:
            if (params) {
                *params = static_cast<GLint>(textureObject->GetImmutableLevels());
            }
            break;
        case GL_TEXTURE_BORDER_COLOR:
            if (params) {
                const auto& borderColor = textureObject->GetBorderColor();
                params[0] = static_cast<GLint>(borderColor.x());
                params[1] = static_cast<GLint>(borderColor.y());
                params[2] = static_cast<GLint>(borderColor.z());
                params[3] = static_cast<GLint>(borderColor.w());
            }
            break;
        case GL_TEXTURE_SWIZZLE_RGBA:
            if (params) {
                const auto& swizzleParams = textureObject->GetAllSwizzleParams();
                params[0] = static_cast<GLint>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[0]));
                params[1] = static_cast<GLint>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[1]));
                params[2] = static_cast<GLint>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[2]));
                params[3] = static_cast<GLint>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[3]));
            }
            break;
        case GL_TEXTURE_SWIZZLE_R:
            if (params) {
                *params = static_cast<GLint>(
                    MG_Util::ConvertTextureSwizzleParamToGLEnum(textureObject->GetSwizzleParam(TextureSwizzleParam::Red)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_G:
            if (params) {
                *params = static_cast<GLint>(MG_Util::ConvertTextureSwizzleParamToGLEnum(
                    textureObject->GetSwizzleParam(TextureSwizzleParam::Green)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_B:
            if (params) {
                *params = static_cast<GLint>(
                    MG_Util::ConvertTextureSwizzleParamToGLEnum(textureObject->GetSwizzleParam(TextureSwizzleParam::Blue)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_A:
            if (params) {
                *params = static_cast<GLint>(MG_Util::ConvertTextureSwizzleParamToGLEnum(
                    textureObject->GetSwizzleParam(TextureSwizzleParam::Alpha)));
            }
            break;
        case GL_TEXTURE_WRAP_S:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapS());
            }
            break;
        case GL_TEXTURE_WRAP_T:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapT());
            }
            break;
        case GL_TEXTURE_WRAP_R:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapR());
            }
            break;
        case GL_TEXTURE_COMPARE_MODE:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerCompareModeToGLEnum(
                    textureObject->GetSamplerObject()->GetCompareMode());
            }
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            if (params) {
                *params = (GLint)MG_Util::ConvertSamplerCompareFuncToGLEnum(
                    textureObject->GetSamplerObject()->GetSamplerCompareFunc());
            }
            break;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            if (params) {
                *params = static_cast<GLint>(textureObject->GetSamplerObject()->GetMaxAnisotropy());
            }
            break;
        case GL_IMAGE_FORMAT_COMPATIBILITY_TYPE:
            if (params) {
                *params = GL_IMAGE_FORMAT_COMPATIBILITY_BY_SIZE;
            }
            break;
        case GL_DEPTH_STENCIL_TEXTURE_MODE:
            if (params) {
                *params = static_cast<GLint>(textureObject->GetDepthStencilTextureMode());
            }
            break;
        case GL_TEXTURE_LOD_BIAS:
            if (params) {
                *params = static_cast<GLint>(textureObject->GetSamplerObject()->GetLodBias());
            }
            break;
        default:
            MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexParameteriv_State",
                                                                           "pname is not a valid texture parameter."));
            return false;
        }

        return true;
    }

    void GetTexParameterfv_State(GLenum target, GLenum pname, GLfloat* params) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
            if (params) {
                *params = (GLfloat)MG_Util::ConvertSamplerFilterModeToGLEnum(
                    textureObject->GetSamplerObject()->GetMagFilter(), SamplerMipmapMode::None);
            }
            break;
        case GL_TEXTURE_MIN_FILTER:
            if (params) {
                *params = (GLfloat)MG_Util::ConvertSamplerFilterModeToGLEnum(
                    textureObject->GetSamplerObject()->GetMinFilter(),
                    textureObject->GetSamplerObject()->GetMipmapMode());
            }
            break;
        case GL_TEXTURE_MIN_LOD:
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetSamplerObject()->GetMinLod());
            }
            break;
        case GL_TEXTURE_MAX_LOD:
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetSamplerObject()->GetMaxLod());
            }
            break;
        case GL_TEXTURE_BASE_LEVEL:
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetLevelRange().x());
            }
            break;
        case GL_TEXTURE_MAX_LEVEL:
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetLevelRange().y());
            }
            break;
        case GL_TEXTURE_IMMUTABLE_FORMAT:
            if (params) {
                *params = textureObject->IsImmutable() ? 1.0f : 0.0f;
            }
            break;
        case GL_TEXTURE_IMMUTABLE_LEVELS:
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetImmutableLevels());
            }
            break;
        case GL_TEXTURE_BORDER_COLOR:
            if (params) {
                const auto& borderColor = textureObject->GetBorderColor();
                params[0] = borderColor.x();
                params[1] = borderColor.y();
                params[2] = borderColor.z();
                params[3] = borderColor.w();
            }
            break;
        case GL_TEXTURE_SWIZZLE_R:
            if (params) {
                *params = static_cast<GLfloat>(
                    MG_Util::ConvertTextureSwizzleParamToGLEnum(textureObject->GetSwizzleParam(TextureSwizzleParam::Red)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_G:
            if (params) {
                *params = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(
                    textureObject->GetSwizzleParam(TextureSwizzleParam::Green)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_B:
            if (params) {
                *params = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(
                    textureObject->GetSwizzleParam(TextureSwizzleParam::Blue)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_A:
            if (params) {
                *params = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(
                    textureObject->GetSwizzleParam(TextureSwizzleParam::Alpha)));
            }
            break;
        case GL_TEXTURE_SWIZZLE_RGBA:
            if (params) {
                const auto& swizzleParams = textureObject->GetAllSwizzleParams();
                params[0] = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[0]));
                params[1] = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[1]));
                params[2] = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[2]));
                params[3] = static_cast<GLfloat>(MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams[3]));
            }
            break;
        case GL_TEXTURE_WRAP_S:
            if (params) {
                *params =
                    (GLfloat)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapS());
            }
            break;
        case GL_TEXTURE_WRAP_T:
            if (params) {
                *params =
                    (GLfloat)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapT());
            }
            break;
        case GL_TEXTURE_WRAP_R:
            if (params) {
                *params =
                    (GLfloat)MG_Util::ConvertSamplerWrapModeToGLEnum(textureObject->GetSamplerObject()->GetWrapR());
            }
            break;
        case GL_TEXTURE_COMPARE_MODE:
            if (params) {
                *params = (GLfloat)MG_Util::ConvertSamplerCompareModeToGLEnum(
                    textureObject->GetSamplerObject()->GetCompareMode());
            }
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            if (params) {
                *params = (GLfloat)MG_Util::ConvertSamplerCompareFuncToGLEnum(
                    textureObject->GetSamplerObject()->GetSamplerCompareFunc());
            }
            break;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            if (params) {
                *params = textureObject->GetSamplerObject()->GetMaxAnisotropy();
            }
            break;
        case GL_DEPTH_STENCIL_TEXTURE_MODE:
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetDepthStencilTextureMode());
            }
            break;
        case GL_TEXTURE_LOD_BIAS:
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetSamplerObject()->GetLodBias());
            }
            break;
        default:
            MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexParameterfv_State",
                                                                           "pname is not a valid texture parameter."));
            return;
        }
    }

    void GetTexLevelParameteriv_State(GLenum target, GLint level, GLenum pname, GLint* params) {
        MGLOG_D("GetTexLevelParameteriv_State called");
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        switch (pname) {
        case GL_TEXTURE_WIDTH:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level).x();
                    break;
                }
                default:
                    RecordUnsupportedLevelQueryStorage("GetTexLevelParameteriv_State", pname);
                    break;
                }
            }
            break;
        case GL_TEXTURE_HEIGHT:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level).y();
                    break;
                }
                default:
                    RecordUnsupportedLevelQueryStorage("GetTexLevelParameteriv_State", pname);
                    break;
                }
            }
            break;
        case GL_TEXTURE_DEPTH:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level).z();
                    break;
                }
                default:
                    RecordUnsupportedLevelQueryStorage("GetTexLevelParameteriv_State", pname);
                    break;
                }
            }
            break;
        case GL_TEXTURE_INTERNAL_FORMAT:
            if (params) {
                // A level stored compressed must report the token it was given, not the
                // uncompressed format backing it (GL 4.6 core 8.11). glCompressedTexImage2D sets
                // that tag, and so does a glTexImage2D given a SPECIFIC compressed internalformat;
                // every other level answers with its resolved storage format.
                const GLenum compressedFormat = GetCompressedLevelFormat(textureObject, textureUploadTarget, level);
                *params = (compressedFormat != GL_NONE)
                              ? (GLint)compressedFormat
                              : (GLint)MG_Util::ConvertTextureInternalFormatToGLEnum(textureObject->GetFormat());
            }
            break;
        case GL_TEXTURE_SAMPLES:
            if (params) {
                *params = textureObject->GetSamples();
            }
            break;
        case GL_TEXTURE_FIXED_SAMPLE_LOCATIONS:
            if (params) {
                *params = textureObject->HasFixedSampleLocations() ? GL_TRUE : GL_FALSE;
            }
            break;
        case GL_TEXTURE_RED_TYPE:
        case GL_TEXTURE_GREEN_TYPE:
        case GL_TEXTURE_BLUE_TYPE:
        case GL_TEXTURE_ALPHA_TYPE:
        case GL_TEXTURE_DEPTH_TYPE:
        case GL_TEXTURE_RED_SIZE:
        case GL_TEXTURE_GREEN_SIZE:
        case GL_TEXTURE_BLUE_SIZE:
        case GL_TEXTURE_ALPHA_SIZE:
        case GL_TEXTURE_DEPTH_SIZE:
        case GL_TEXTURE_STENCIL_SIZE:
            if (params) {
                *params = GetTextureLevelComponentParameter(textureObject->GetFormat(), pname);
            }
            break;
        case GL_TEXTURE_COMPRESSED:
            if (params) {
                *params =
                    (GetCompressedLevelFormat(textureObject, textureUploadTarget, level) != GL_NONE) ? GL_TRUE
                                                                                                     : GL_FALSE;
            }
            break;
        case GL_TEXTURE_COMPRESSED_IMAGE_SIZE: {
            // GL 4.6 core 8.11: there is no compressed size to report for an image whose internal
            // format is uncompressed, nor for a proxy target, and the query is INVALID_OPERATION
            // rather than a zero.
            if (isProxy || GetCompressedLevelFormat(textureObject, textureUploadTarget, level) == GL_NONE) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", "GetTexLevelParameteriv_State",
                        "GL_TEXTURE_COMPRESSED_IMAGE_SIZE needs a compressed, non-proxy texture image."));
                return;
            }
            if (params) {
                const auto* textureMipmapObject = MG_State::GLState::AsMipmapTexture(textureObject.get());
                *params = static_cast<GLint>(
                    textureMipmapObject->GetMipmapCompressedByteSize(textureUploadTarget, static_cast<Uint>(level)));
            }
            break;
        }
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexLevelParameteriv_State",
                                                                     "pname is not a valid texture level parameter."));
            return;
        }
        if (params) MGLOG_D("returned %u", *params);
    }

    void GetTexLevelParameterfv_State(GLenum target, GLint level, GLenum pname, GLfloat* params) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;

        // ======================= Processing ================================
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        if (!TextureImpl::ValidateTextureObject(textureObject)) return;

        switch (pname) {
        case GL_TEXTURE_WIDTH:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = (GLfloat)textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level).x();
                    break;
                }
                default:
                    RecordUnsupportedLevelQueryStorage("GetTexLevelParameterfv_State", pname);
                    break;
                }
            }
            break;
        case GL_TEXTURE_HEIGHT:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = (GLfloat)textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level).y();
                    break;
                }
                default:
                    RecordUnsupportedLevelQueryStorage("GetTexLevelParameterfv_State", pname);
                    break;
                }
            }
            break;
        case GL_TEXTURE_DEPTH:
            if (params) {
                switch (textureObject->GetStorageType()) {
                case TextureStorageType::Mipmap: {
                    const auto textureMipmapObject =
                        static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
                    *params = (GLfloat)textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level).z();
                    break;
                }
                default:
                    RecordUnsupportedLevelQueryStorage("GetTexLevelParameterfv_State", pname);
                    break;
                }
            }
            break;
        case GL_TEXTURE_INTERNAL_FORMAT:
            if (params) {
                // A level stored compressed must report the token it was given, not the
                // uncompressed format backing it (GL 4.6 core 8.11). glCompressedTexImage2D sets
                // that tag, and so does a glTexImage2D given a SPECIFIC compressed internalformat;
                // every other level answers with its resolved storage format.
                const GLenum compressedFormat = GetCompressedLevelFormat(textureObject, textureUploadTarget, level);
                *params = (GLfloat)((compressedFormat != GL_NONE)
                                        ? compressedFormat
                                        : MG_Util::ConvertTextureInternalFormatToGLEnum(textureObject->GetFormat()));
            }
            break;
        case GL_TEXTURE_SAMPLES:
            if (params) {
                *params = static_cast<GLfloat>(textureObject->GetSamples());
            }
            break;
        case GL_TEXTURE_FIXED_SAMPLE_LOCATIONS:
            if (params) {
                *params = textureObject->HasFixedSampleLocations() ? 1.0f : 0.0f;
            }
            break;
        case GL_TEXTURE_RED_TYPE:
        case GL_TEXTURE_GREEN_TYPE:
        case GL_TEXTURE_BLUE_TYPE:
        case GL_TEXTURE_ALPHA_TYPE:
        case GL_TEXTURE_DEPTH_TYPE:
        case GL_TEXTURE_RED_SIZE:
        case GL_TEXTURE_GREEN_SIZE:
        case GL_TEXTURE_BLUE_SIZE:
        case GL_TEXTURE_ALPHA_SIZE:
        case GL_TEXTURE_DEPTH_SIZE:
        case GL_TEXTURE_STENCIL_SIZE:
            if (params) {
                *params = static_cast<GLfloat>(GetTextureLevelComponentParameter(textureObject->GetFormat(), pname));
            }
            break;
        case GL_TEXTURE_COMPRESSED:
            if (params) {
                *params =
                    (GetCompressedLevelFormat(textureObject, textureUploadTarget, level) != GL_NONE) ? 1.0f : 0.0f;
            }
            break;
        case GL_TEXTURE_COMPRESSED_IMAGE_SIZE: {
            // See GetTexLevelParameteriv_State: uncompressed images and proxy targets have no
            // compressed size to report, so GL 4.6 core 8.11 makes the query an error.
            if (isProxy || GetCompressedLevelFormat(textureObject, textureUploadTarget, level) == GL_NONE) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", "GetTexLevelParameterfv_State",
                        "GL_TEXTURE_COMPRESSED_IMAGE_SIZE needs a compressed, non-proxy texture image."));
                return;
            }
            if (params) {
                const auto* textureMipmapObject = MG_State::GLState::AsMipmapTexture(textureObject.get());
                *params = static_cast<GLfloat>(
                    textureMipmapObject->GetMipmapCompressedByteSize(textureUploadTarget, static_cast<Uint>(level)));
            }
            break;
        }
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexLevelParameterfv_State",
                                                                     "pname is not a valid texture level parameter."));
            return;
        }
    }

    // The half glGetCompressedTexImage and glGetCompressedTextureImage share, factored out for the
    // same reason ValidateTextureImageQuery was: the by-name entry point must not drift away from
    // the by-target one's rules. bufSize < 0 means "no destination-size argument" - the by-target
    // form has none (GL 4.6 core 8.11 has the caller size it from GL_TEXTURE_COMPRESSED_IMAGE_SIZE),
    // so only the DSA form passes a real bound.
    void CopyCompressedTextureImageToClientOrPBO(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                 TextureUploadTarget uploadTarget, GLint level, GLsizei bufSize,
                                                 void* pixels, const char* caller) {
        if (GetCompressedLevelFormat(textureObject, uploadTarget, level) == GL_NONE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Texture level is not stored in a compressed format."));
            return;
        }

        const auto* textureMipmapObject = MG_State::GLState::AsMipmapTexture(textureObject.get());
        const SizeT imageSize =
            textureMipmapObject->GetMipmapCompressedByteSize(uploadTarget, static_cast<Uint>(level));
        const void* src = textureMipmapObject->MapMipmapCompressedImage(uploadTarget, static_cast<Uint>(level));
        if (!src || imageSize == 0) return;

        if (bufSize >= 0 && static_cast<SizeT>(bufSize) < imageSize) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Destination buffer is too small."));
            return;
        }

        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
        if (pixelPackBufferObject) {
            if (pixelPackBufferObject->IsMapped()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Pixel pack buffer is currently mapped."));
                return;
            }
            const SizeT offset = reinterpret_cast<SizeT>(pixels);
            const SizeT bufferSize = pixelPackBufferObject->GetSize();
            if (offset > bufferSize || imageSize > bufferSize - offset) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "Packing would write past the end of the pixel pack buffer."));
                return;
            }
            pixelPackBufferObject->UploadSubData({const_cast<void*>(src), imageSize}, offset);
            return;
        }

        // No pixel-store packing here on purpose: GL 4.6 core 8.11 says the pixel storage modes are
        // ignored for a compressed image, which is also the only way the round trip stays byte-exact.
        if (pixels) Memcpy(pixels, src, imageSize);
    }

    void GetCompressedTexImage_State(GLenum target, GLint level, void* img) {
        const auto textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        // ValidateTextureUploadTarget records InvalidEnum itself; wrapping it in a second RecordError
        // would report one failure twice.
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        CopyCompressedTextureImageToClientOrPBO(textureObject, textureUploadTarget, level, -1, img, __func__);
    }

    void GenTextures_State(GLsizei n, GLuint* textures) {
        // ===================== Error Checking ==============================
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GenTextures_State", "n must be non-negative"));
            return;
        }

        // ======================= Processing ================================
        Vector<Uint> textureNames;
        MG_State::pGLContext->GenTextureNames(n, textureNames);
        Memcpy(textures, textureNames.data(), n * sizeof(GLuint));
    }

    void DeleteTextures_State(GLsizei n, const GLuint* textures) {
        // ===================== Error Checking ==============================
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteTextures_State", "n must be non-negative."));
            return;
        }

        if (!textures) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteTextures_State",
                                                                           "Texture names array cannot be null."));
            return;
        }

        // ======================= Processing ================================
        for (SizeT i = 0; i < static_cast<SizeT>(n); ++i) {
            Uint textureName = textures[i];
            if (textureName == 0) continue;
            if (!MG_State::pGLContext->ValidateTextureName(textureName)) continue;
            MG_State::pGLContext->MarkTextureObjectForDeletion(textureName);
        }
    }

    void CopyTexSubImage3D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x,
                                 GLint y, GLsizei width, GLsizei height) {
        // TODO: implement
    }

    // What the three CopyTextureSubImage forms check in common (GL 4.6 core 8.6), once the caller
    // has rejected an effective target its own form does not accept: the destination region has to
    // lie inside the level, and the read framebuffer has to be able to supply pixels at all.
    Bool ValidateCopyTextureSubImage(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, GLint level,
                                     GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height,
                                     GLsizei depth, const char* caller) {
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return false;
        if (width < 0 || height < 0 || depth < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Copy dimensions must be non-negative."));
            return false;
        }
        if (!TextureImpl::ValidateTextureSubImageOffsets(textureObject, xoffset, width, yoffset, height, zoffset,
                                                         depth)) {
            return false;
        }
        return FramebufferImpl::ValidateReadFramebufferForCopy(caller);
    }

    void CopyTexSubImage2D_Backend(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                                   GLsizei width, GLsizei height) {
        MG_Backend::gBackendFunctionsTable.GL.CopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
    }

    void CopyImageSubData_Backend(const MG_Backend::CopyImageEndpoint& src,
                                  GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                  const MG_Backend::CopyImageEndpoint& dst,
                                  GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                  GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        auto copyImageSubData = MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData;
        if (!copyImageSubData) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support image-to-image copies."));
            return;
        }
        copyImageSubData(src, srcTarget, srcLevel, srcX, srcY, srcZ, dst, dstTarget, dstLevel, dstX,
                         dstY, dstZ, srcWidth, srcHeight, srcDepth);
    }

    namespace {
        // The eleven targets GL 4.6 core 18.3.2 accepts. GL_TEXTURE_BUFFER, the six cube FACE
        // enums and every PROXY enum all convert to a TextureTarget this frontend recognises,
        // so ValidateTextureTarget lets them through; here they are INVALID_ENUM.
        Bool ValidateCopyImageTarget(GLenum target, const char* endpointName) {
            switch (target) {
            case GL_RENDERBUFFER:
            case GL_TEXTURE_1D:
            case GL_TEXTURE_1D_ARRAY:
            case GL_TEXTURE_2D:
            case GL_TEXTURE_2D_ARRAY:
            case GL_TEXTURE_2D_MULTISAMPLE:
            case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
            case GL_TEXTURE_3D:
            case GL_TEXTURE_CUBE_MAP:
            case GL_TEXTURE_CUBE_MAP_ARRAY:
            case GL_TEXTURE_RECTANGLE:
                return true;
            default:
                break;
            }
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateCopyImageSubData_State",
                    std::format("{} is not a target glCopyImageSubData accepts as the {}.",
                                MG_Util::ConvertGLEnumToString(target), endpointName)));
            return false;
        }

        IntVec3 GetCopyImageLevelSize(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                      TextureUploadTarget uploadTarget, GLint level) {
            const auto* mipmapTexture = MG_State::GLState::AsMipmapTexture(textureObject.get());
            if (!mipmapTexture) return textureObject->GetBaseSize();
            return mipmapTexture->GetMipmapTexelSize(uploadTarget, static_cast<Uint>(level));
        }

        // glCopyImageSubData names an object that must already exist, and GL 4.6 core 18.3.2
        // spells the failure INVALID_VALUE - "if either name does not correspond to a valid
        // object". The shared ValidateTextureObject says INVALID_OPERATION, which is right for
        // the ~30 entry points that reach it through a BOUND object (where the name was never
        // in question and the fault is the binding), so this is a local rule rather than a
        // change to the helper.
        Bool ValidateCopyImageObjectExists(const MG_Backend::CopyImageEndpoint& endpoint,
                                           const char* endpointName) {
            if (endpoint.Exists()) return true;
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateCopyImageSubData_State",
                    std::format("The {} name does not correspond to an existing image object.", endpointName)));
            return false;
        }

        // Same split for the target/object disagreement: GL 4.6 core 18.3.2 makes a target that
        // does not match the object INVALID_ENUM, where the shared uniformity helper records
        // INVALID_OPERATION for the upload paths that share it.
        Bool ValidateCopyImageTargetMatchesObject(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                  TextureTarget target, const char* endpointName) {
            if (!textureObject || textureObject->GetTarget() == target) return true;
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateCopyImageSubData_State",
                    std::format("The {} target {} does not match the target the object was created with ({}).",
                                endpointName, MG_Util::ConvertTextureTargetToString(target),
                                MG_Util::ConvertTextureTargetToString(textureObject->GetTarget()))));
            return false;
        }

        // ---- The questions ValidateCopyImageSubData_State asks of one endpoint. ---------------
        // A renderbuffer answers all of them directly: it has exactly one image, no mip chain and
        // no sampler state, and it carries its own internal format and extent.

        Int GetCopyImageEndpointSamples(const MG_Backend::CopyImageEndpoint& endpoint) {
            if (endpoint.IsRenderbuffer()) return endpoint.Renderbuffer->GetSamples();
            return endpoint.Texture->GetSamples();
        }

        TextureInternalFormat GetCopyImageEndpointFormat(const MG_Backend::CopyImageEndpoint& endpoint) {
            if (endpoint.IsRenderbuffer()) return endpoint.Renderbuffer->GetInternalFormat();
            return endpoint.Texture->GetFormat();
        }

        // A renderbuffer has level 0 and nothing else, and the failure is the same INVALID_VALUE
        // ValidateTextureLevelExists records for a level a texture does not have.
        Bool ValidateCopyImageEndpointLevelExists(const MG_Backend::CopyImageEndpoint& endpoint, GLint level,
                                                  const char* caller) {
            if (!endpoint.IsRenderbuffer()) {
                return TextureImpl::ValidateTextureLevelExists(endpoint.Texture, level, caller);
            }
            if (level == 0) return true;
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "A renderbuffer has only level 0."));
            return false;
        }

        // Targets with no mip chain have q == level_base by definition (GL 4.6 core 8.17), so no
        // minification filter can make them mipmap incomplete - while the shared predicate derives
        // q from the base level's size alone and would call a 16x16 multisample image incomplete.
        Bool CopyImageTargetHasMipmapChain(TextureTarget target) {
            switch (target) {
            case TextureTarget::TextureRectangle:
            case TextureTarget::TextureBuffer:
            case TextureTarget::Texture2DMultisample:
            case TextureTarget::Texture2DMultisampleArray:
                return false;
            default:
                return true;
            }
        }

        Bool IsCopyImageEndpointComplete(const MG_Backend::CopyImageEndpoint& endpoint) {
            // A renderbuffer is complete exactly when it has storage - there is nothing else it
            // could be missing.
            if (endpoint.IsRenderbuffer()) return endpoint.Renderbuffer->IsAllocated();
            const auto* texture = endpoint.Texture.get();
            if (!texture) return false;
            // 18.3.2 asks for TEXTURE completeness, which GL 4.6 core 8.17 defines to include the
            // MIP CHAIN whenever the minification filter samples it - and ITextureObject::
            // IsComplete() only answers the storage half (an internal format, and no zero-size
            // level in the middle of the chain). A texture with level 0 alone and the default
            // NEAREST_MIPMAP_LINEAR filter is incomplete, which is exactly how
            // KHR-GL43.copy_image.incomplete_tex builds its subject.
            //
            // The filter is the texture's OWN: copy-image never goes through a texture unit, so no
            // sampler object is in play. An immutable texture is unaffected - glTexStorage clamps
            // TEXTURE_MAX_LEVEL to levels-1, which is what makes a single-level immutable texture
            // mipmap complete under any filter.
            const auto& sampler = texture->GetSamplerObject();
            const Bool mipmapped = CopyImageTargetHasMipmapChain(texture->GetTarget()) && sampler &&
                                   sampler->GetMipmapMode() != SamplerMipmapMode::None;
            return MG_State::GLState::IsMipmapCompleteForFilter(texture, mipmapped);
        }

        GLenum GetCopyImageEndpointCompressedFormat(const MG_Backend::CopyImageEndpoint& endpoint,
                                                    TextureUploadTarget uploadTarget, GLint level) {
            if (endpoint.IsRenderbuffer()) return GL_NONE;
            return GetCompressedLevelFormat(endpoint.Texture, uploadTarget, level);
        }

        IntVec3 GetCopyImageEndpointLevelSize(const MG_Backend::CopyImageEndpoint& endpoint,
                                              TextureUploadTarget uploadTarget, GLint level) {
            if (endpoint.IsRenderbuffer()) {
                return {endpoint.Renderbuffer->GetWidth(), endpoint.Renderbuffer->GetHeight(), 1};
            }
            return GetCopyImageLevelSize(endpoint.Texture, uploadTarget, level);
        }
    } // namespace

    Bool ValidateCopyImageSubData_State(const MG_Backend::CopyImageEndpoint& src,
                                        GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY,
                                        const MG_Backend::CopyImageEndpoint& dst,
                                        GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY,
                                        GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        if (!ValidateCopyImageObjectExists(src, "source") ||
            !ValidateCopyImageObjectExists(dst, "destination")) {
            return false;
        }
        // GL_RENDERBUFFER has no TextureTarget to convert to, and it needs none: it is its own
        // whole-image target, and the endpoint that carries it was resolved from the renderbuffer
        // namespace, so it matches its object by construction.
        const auto srcTextureTarget =
            src.IsRenderbuffer() ? TextureTarget::Unknown : MG_Util::ConvertGLEnumToTextureTarget(srcTarget);
        const auto dstTextureTarget =
            dst.IsRenderbuffer() ? TextureTarget::Unknown : MG_Util::ConvertGLEnumToTextureTarget(dstTarget);
        if ((!src.IsRenderbuffer() && !TextureImpl::ValidateTextureTarget(srcTextureTarget)) ||
            (!dst.IsRenderbuffer() && !TextureImpl::ValidateTextureTarget(dstTextureTarget))) {
            return false;
        }
        // GL_TEXTURE_BUFFER and the cube FACE enums convert to a target this frontend knows, but
        // 18.3.2 does not accept them here - only the eleven whole-image targets do.
        if (!ValidateCopyImageTarget(srcTarget, "source") || !ValidateCopyImageTarget(dstTarget, "destination")) {
            return false;
        }
        if (!ValidateCopyImageTargetMatchesObject(src.Texture, srcTextureTarget, "source") ||
            !ValidateCopyImageTargetMatchesObject(dst.Texture, dstTextureTarget, "destination")) {
            return false;
        }
        if (!TextureImpl::ValidateTextureLevelNumber(srcLevel) ||
            !TextureImpl::ValidateTextureLevelNumber(dstLevel)) {
            return false;
        }
        // ValidateTextureLevelNumber only bounds the index by GL_MAX_TEXTURE_SIZE; it cannot
        // see that this particular texture stops at level 0. Both backends turn <level> into an
        // image subresource with no further checking (DirectVulkan builds a VkImageCopy from it,
        // DirectGLES forwards it to the ES copy), so a level the texture never had reached the
        // driver as an out-of-range mip index - on Adreno that is a SIGSEGV inside
        // vkCmdCopyImage, which is what KHR-GL43.copy_image.non_existent_mipmap used to do to
        // the whole glcts process. The answer the spec asks for is GL_INVALID_VALUE.
        if (!ValidateCopyImageEndpointLevelExists(src, srcLevel, __func__) ||
            !ValidateCopyImageEndpointLevelExists(dst, dstLevel, __func__)) {
            return false;
        }
        if (srcWidth < 0 || srcHeight < 0 || srcDepth < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Copy dimensions must be non-negative."));
            return false;
        }
        if (srcWidth == 0 || srcHeight == 0 || srcDepth == 0) {
            return false;
        }
        // A multisample image can only be copied to one with the same sample count, and a
        // single-sample image reports zero - so this one comparison is also what rejects
        // copying between a multisample target and a non-multisample one.
        const Int srcSamples = GetCopyImageEndpointSamples(src);
        const Int dstSamples = GetCopyImageEndpointSamples(dst);
        if (srcSamples != dstSamples) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    std::format("The two images have different sample counts ({} vs. {}).",
                                srcSamples, dstSamples)));
            return false;
        }
        // 18.3.2: both images must be complete. An incomplete one has no defined texels to copy
        // and no defined storage to copy into.
        const Bool srcComplete = IsCopyImageEndpointComplete(src);
        const Bool dstComplete = IsCopyImageEndpointComplete(dst);
        if (!srcComplete || !dstComplete) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    std::format("A copied image is incomplete (source complete: {}, destination complete: {}).",
                                srcComplete, dstComplete)));
            return false;
        }
        const auto srcUploadTarget = GetPrimaryUploadTarget(src.Texture);
        const auto dstUploadTarget = GetPrimaryUploadTarget(dst.Texture);
        const auto srcBlock = TextureImpl::ResolveCopyImageTexelBlock(
            GetCopyImageEndpointFormat(src), GetCopyImageEndpointCompressedFormat(src, srcUploadTarget, srcLevel));
        const auto dstBlock = TextureImpl::ResolveCopyImageTexelBlock(
            GetCopyImageEndpointFormat(dst), GetCopyImageEndpointCompressedFormat(dst, dstUploadTarget, dstLevel));
        if (!TextureImpl::ValidateCopyImageFormatCompatibility(srcBlock, dstBlock)) {
            return false;
        }
        const IntVec3 srcLevelSize = GetCopyImageEndpointLevelSize(src, srcUploadTarget, srcLevel);
        const IntVec3 dstLevelSize = GetCopyImageEndpointLevelSize(dst, dstUploadTarget, dstLevel);
        if (!TextureImpl::ValidateCopyImageBlockAlignment(srcBlock, srcX, srcY, srcWidth, srcHeight,
                                                          srcLevelSize.x(), srcLevelSize.y(), "source") ||
            !TextureImpl::ValidateCopyImageBlockAlignment(dstBlock, dstX, dstY, srcWidth, srcHeight,
                                                          dstLevelSize.x(), dstLevelSize.y(), "destination")) {
            return false;
        }
        return true;
    }

    void CopyTexSubImage1D_State(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width) {
        // TODO: implement
    }

    Bool CopyTexImage2D_State(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                              GLsizei height, GLint border) {
        auto internalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        const auto textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
        if (!ValidateTextureMutable(textureObject, __func__)) return false;

        const auto& currentReadFBO =
            MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
        if (!currentReadFBO) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyTexImage2D_State",
                                             "No framebuffer is currently bound to the GL_READ_FRAMEBUFFER target."));
            return false;
        }

        Bool isDepth = MG_Util::IsDepthFormatInternalFormat(internalFormat);
        Bool isStencil = MG_Util::IsStencilFormatInternalFormat(internalFormat);
        TextureInternalFormat srcInternalFormat = TextureInternalFormat::Unknown;
#define GET_SRC_INTERNAL_FORMAT(AttachmentType)                                                                        \
    const auto& srcAttachment = currentReadFBO->GetAttachment(AttachmentType);                                         \
    if (srcAttachment.IsTexture()) {                                                                                   \
        const auto& texObj = srcAttachment.GetTexture();                                                               \
        srcInternalFormat = texObj->GetFormat();                                                                       \
    } else if (srcAttachment.IsRenderbuffer()) {                                                                       \
        const auto& rboObj = srcAttachment.GetRenderbuffer();                                                          \
        srcInternalFormat = rboObj->GetInternalFormat();                                                               \
    } else {                                                                                                           \
        MG_State::pGLContext->RecordError(                                                                             \
            ErrorCode::InvalidOperation,                                                                               \
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyTexImage2D_State",                                     \
                                         "The attachment specified by the read buffer is incomplete."));               \
        return false;                                                                                                  \
    }
        if (isDepth && isStencil) {
            // A combined internalformat copies both halves, so the read framebuffer
            // must populate both attachment points.
            const auto& stencilAttachment = currentReadFBO->GetAttachment(FramebufferAttachmentType::Stencil);
            const auto& depthAttachment = currentReadFBO->GetAttachment(FramebufferAttachmentType::Depth);
            if (!depthAttachment.IsValid() || depthAttachment.IsEmpty() || !stencilAttachment.IsValid() ||
                stencilAttachment.IsEmpty()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", "CopyTexImage2D_State",
                        "DEPTH_STENCIL copy requires both depth and stencil attachments in the read framebuffer."));
                return false;
            }
            GET_SRC_INTERNAL_FORMAT(FramebufferAttachmentType::Depth);
        } else if (isDepth) {
            GET_SRC_INTERNAL_FORMAT(FramebufferAttachmentType::Depth);
        } else if (isStencil) {
            GET_SRC_INTERNAL_FORMAT(FramebufferAttachmentType::Stencil);
        } else {
            const auto& readBufferType = currentReadFBO->GetReadBuffer();
            GET_SRC_INTERNAL_FORMAT(readBufferType);
        }

        // The validator has already recorded GL_INVALID_OPERATION; just decline. Throwing
        // here unwound a C++ exception through the C GL ABI and killed the process (see the
        // same reasoning at :604-609).
        if (!TextureImpl::ValidateCopyTexImageBaseFormatSubset(internalFormat, srcInternalFormat)) return false;

        GLenum outInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(srcInternalFormat);
        GLenum realInternalFormat = GL_RGBA8;
        GLenum format = GL_DEPTH_COMPONENT;
        GLenum type = GL_UNSIGNED_INT;
        MG_Util::TextureFormatProcessor::NormalizePixelFormat(outInternalFormat, PixelFormatNormalizeOptionBit::None,
                                                              &realInternalFormat, &format, &type);
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        TexImage2D_State(target, level, (GLint)realInternalFormat, width, height, border, format, type, nullptr);
        MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).Bind(pixelUnpackBufferObject);
        return true;
    }

    void CopyTexImage2D_Backend(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                                GLsizei height, GLint border) {
        MG_Backend::gBackendFunctionsTable.GL.CopyTexImage2D(target, level, internalformat, x, y, width, height,
                                                             border);
    }

    void CopyTexImage1D_State(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                              GLint border) {
        // 1D textures are not implemented by this backend set. Record the error the way every
        // other unsupported entry point does - throwing unwinds through the C GL ABI and kills
        // the process, which is never an acceptable answer to an unsupported call.
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidOperation,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CopyTexImage1D",
                                         "1D textures are not supported by this implementation"));
    }

    void CompressedTexSubImage3D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                       GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize,
                                       const void* data) {
        // TODO: implement compressed upload - see CompressedTexImage2D_State.
        RecordUnsupportedCompressedFormat(__func__);
    }

    // Replaces a block-aligned rectangle of the compressed image glCompressedTexImage2D (or a
    // compressed glTexImage2D) shadowed for this level. Same deviation as the image call it
    // patches: the uncompressed texel shadow beside it is NOT touched, because there is no
    // BC/ETC codec here to decode the incoming blocks with - so what changes is the image
    // glGetCompressedTexImage hands back, not what the level samples as. Marking the texels
    // dirty would therefore only re-upload bytes that did not change.
    void CompressedTexSubImage2D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                       GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
        // ======================= Converting ================================
        const auto textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        // Zero block width doubles as "format is not a specific compressed format", the
        // INVALID_ENUM case - one lookup answers both questions.
        const auto compressedInfo = MG_Util::GetCompressedFormatInfo(format);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        // A proxy holds no image to modify; only the glTexImage*/glCompressedTexImage* pair
        // accepts one.
        if (TextureImpl::IsProxyTextureTarget(textureUploadTarget)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "A proxy target has no texture image to modify."));
            return;
        }
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;
        if (width < 0 || height < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "width and height must be non-negative."));
            return;
        }
        if (compressedInfo.blockWidth == 0) {
            RecordUnsupportedCompressedFormat(__func__);
            return;
        }

        auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        auto* textureMipmapObject = MG_State::GLState::AsMipmapTexture(textureObject.get());
        if (textureMipmapObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }
        // GL 4.6 core 8.7: INVALID_OPERATION unless the image being modified is stored in
        // exactly this compressed format. That is also what makes the block arithmetic below
        // sound - the level's grid is measured with THIS format's block size.
        const GLenum levelFormat =
            textureMipmapObject->GetMipmapCompressedFormat(textureUploadTarget, static_cast<Uint>(level));
        if (levelFormat != format) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "format does not match the internal format of the texture image."));
            return;
        }

        const IntVec3 levelSize = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, static_cast<Uint>(level));
        // Written as a subtraction rather than `xoffset + width > levelSize.x()`: both operands
        // are application-supplied GLints, so the sum is free to overflow, and a signed overflow
        // is undefined behaviour that a compiler may resolve by assuming the check passes.
        // levelSize is our own and non-negative, and the offsets are known non-negative by the
        // time the subtraction runs, so this form cannot wrap.
        if (xoffset < 0 || yoffset < 0 || width > levelSize.x() - xoffset || height > levelSize.y() - yoffset) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "The replaced region does not lie within the texture image."));
            return;
        }
        // GL 4.6 core 8.7 for block-based formats: the region must start on a block boundary
        // and must either be a whole number of blocks wide/high or run to the image's edge.
        const Int blockWidth = static_cast<Int>(compressedInfo.blockWidth);
        const Int blockHeight = static_cast<Int>(compressedInfo.blockHeight);
        const Bool alignedX = (xoffset % blockWidth == 0) &&
                              (width % blockWidth == 0 || xoffset + width == levelSize.x());
        const Bool alignedY = (yoffset % blockHeight == 0) &&
                              (height % blockHeight == 0 || yoffset + height == levelSize.y());
        if (!alignedX || !alignedY) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "The replaced region is not aligned to the format's compressed blocks."));
            return;
        }
        // Exactly the size the format and dimensions imply, which is also what keeps the copy
        // below in bounds.
        const SizeT expectedImageSize =
            MG_Util::CalculateCompressedTextureImageSize(compressedInfo, {width, height, 1});
        if (imageSize < 0 || static_cast<SizeT>(imageSize) != expectedImageSize) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "imageSize does not match the compressed image size."));
            return;
        }

        // ======================= Processing ================================
        if (!ValidateCompressedUnpackBufferSource(data, expectedImageSize, __func__)) return;
        const void* compressedBytes = CompressedUnpackSource(data);
        if (expectedImageSize == 0) return; // a zero-sized region is a legal no-op
        if (compressedBytes == nullptr) {
            // No unpack buffer and a null client pointer: there is nothing to read. GL leaves
            // this undefined rather than erroring, and dereferencing it is the one answer that
            // is never acceptable.
            MGLOG_D("%s: null data with no pixel unpack buffer bound, nothing to replace", __func__);
            return;
        }

        // Once per process: the call is about to succeed, and what it does is narrower than what
        // an application has every right to expect from it. Before this existed the call answered
        // GL_INVALID_ENUM, which was wrong but at least visible; a silent success that leaves the
        // sampled texels untouched is the kind of thing that costs a day to find from the other
        // end. MGLOG_W is the right level and now survives at INFO; it sat at MGLOG_I only
        // while the Log.h ordering compiled warnings out of the builds that ship.
        static std::atomic<Bool> announcedNoCodec{false};
        if (!announcedNoCodec.exchange(true)) {
            MGLOG_W("%s: the compressed blocks are stored verbatim and returned by "
                    "glGetCompressedTexImage, but there is no BC/ETC decoder here, so they do not "
                    "reach the texels this level SAMPLES as. Upload through glTexSubImage2D for "
                    "that.",
                    __func__);
        }

        // The level's compressed image is stored as one blob, so the rectangle is patched into
        // a copy of it and the whole thing handed back. Compressed sub-image uploads are not a
        // hot path, and this keeps the storage layer's compressed API to the two calls it has.
        const SizeT blobSize =
            textureMipmapObject->GetMipmapCompressedByteSize(textureUploadTarget, static_cast<Uint>(level));
        const void* existing =
            textureMipmapObject->MapMipmapCompressedImage(textureUploadTarget, static_cast<Uint>(level));
        if (blobSize == 0 || existing == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "The texture level holds no compressed image to modify."));
            return;
        }
        Vector<Uint8> blob(blobSize);
        Memcpy(blob.data(), existing, blobSize);

        const SizeT blockByteSize = compressedInfo.blockByteSize;
        const SizeT levelBlocksX = (static_cast<SizeT>(levelSize.x()) + compressedInfo.blockWidth - 1) /
                                   compressedInfo.blockWidth;
        const SizeT levelRowBytes = levelBlocksX * blockByteSize;
        const SizeT regionBlocksX = (static_cast<SizeT>(width) + compressedInfo.blockWidth - 1) /
                                    compressedInfo.blockWidth;
        const SizeT regionBlocksY = (static_cast<SizeT>(height) + compressedInfo.blockHeight - 1) /
                                    compressedInfo.blockHeight;
        const SizeT firstBlockX = static_cast<SizeT>(xoffset) / compressedInfo.blockWidth;
        const SizeT firstBlockY = static_cast<SizeT>(yoffset) / compressedInfo.blockHeight;
        const SizeT regionRowBytes = regionBlocksX * blockByteSize;
        const auto* source = static_cast<const Uint8*>(compressedBytes);
        for (SizeT row = 0; row < regionBlocksY; ++row) {
            const SizeT destOffset = (firstBlockY + row) * levelRowBytes + firstBlockX * blockByteSize;
            if (destOffset + regionRowBytes > blobSize) break; // a level whose blob predates its size
            Memcpy(blob.data() + destOffset, source + row * regionRowBytes, regionRowBytes);
        }
        textureMipmapObject->SetMipmapCompressedImage(textureUploadTarget, static_cast<Uint>(level), format,
                                                      blob.data(), blobSize);
    }

    void CompressedTexSubImage1D_State(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format,
                                       GLsizei imageSize, const void* data) {
        // TODO: implement compressed upload - see CompressedTexImage2D_State.
        RecordUnsupportedCompressedFormat(__func__);
    }

    void CompressedTexImage3D_State(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                                    GLsizei depth, GLint border, GLsizei imageSize, const void* data) {
        const auto textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
        if (!ValidateTextureMutable(textureObject, __func__)) return;

        // TODO: implement compressed upload - see CompressedTexImage2D_State.
        RecordUnsupportedCompressedFormat(__func__);
    }

    void CompressedTexImage2D_State(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                                    GLint border, GLsizei imageSize, const void* data) {
        // ======================= Converting ================================
        const auto textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        // Zero block width doubles as "internalformat is not a specific compressed format", which is
        // the INVALID_ENUM case - one lookup answers both questions.
        const auto compressedInfo = MG_Util::GetCompressedFormatInfo(internalformat);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeWithTextureUploadTarget(textureUploadTarget, width, height)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, 1)) return;
        if (!TextureImpl::ValidateTextureBorderNumber(border)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;
        if (compressedInfo.blockWidth == 0) {
            RecordUnsupportedCompressedFormat(__func__);
            return;
        }
        // GL 4.6 core 8.7: imageSize must be exactly the size the format and dimensions imply,
        // otherwise INVALID_VALUE. This is also the guard that keeps the copy below in bounds.
        const SizeT expectedImageSize =
            MG_Util::CalculateCompressedTextureImageSize(compressedInfo, {width, height, 1});
        if (imageSize < 0 || static_cast<SizeT>(imageSize) != expectedImageSize) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "imageSize does not match the compressed image size."));
            return;
        }

        // Object resolution copied from TexImage2D_State rather than routed through
        // GetTextureObjectByTarget: GL 4.6 core 8.7 lets a proxy target reach glCompressedTexImage2D,
        // and only CreateOrReplaceProxyTextureObject gives the proxy a fresh object to answer the
        // level queries from.
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        const Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!ValidateTextureMutable(textureObject, __func__)) return;

        // ======================= Processing ================================
        // Texel storage stays uncompressed, exactly the deviation the RGTC/BPTC/ETC2 arms of
        // ConvertGLEnumToTextureInternalFormat already document: neither backend has a BC/ETC codec
        // and TextureInternalFormat has no compressed enumerator, so the shadow keeps the "one
        // format, N bytes per texel" layout the backend upload sizing, glGenerateMipmap's
        // bytes-per-texel division and the pixel-store packer all rely on. The image therefore
        // samples as zeros. The application's bytes are kept beside it so glGetCompressedTexImage can
        // return the image *as stored*, which GL 4.6 core 8.11 requires and which no re-encode could
        // satisfy byte for byte.
        const TextureInternalFormat textureInternalFormat =
            MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        textureObject->SetInternalFormat(textureInternalFormat);

        // A proxy records the format and nothing else - it must never take storage, and it must never
        // be tagged compressed, or GL_TEXTURE_COMPRESSED_IMAGE_SIZE on a proxy would stop being
        // INVALID_OPERATION.
        if (isProxy) return;

        const SizeT internalBpp =
            MG_Util::GetInternalBytesPerPixel(textureInternalFormat, TexturePixelDataType::UnsignedByte);
        const SizeT internalBytes = static_cast<SizeT>(width) * static_cast<SizeT>(height) * internalBpp;

        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        DiscardMipmapChainOnBaseRespecification(textureMipmapObject, textureUploadTarget, level);
        // AllocateStorage clears any compressed image the level used to hold, so this must run before
        // SetMipmapCompressedImage re-arms it.
        textureMipmapObject->AllocateStorage(textureUploadTarget, level, {{width, height, 1}, internalBytes});

        if (!ValidateCompressedUnpackBufferSource(data, expectedImageSize, __func__)) return;
        const void* compressedBytes = CompressedUnpackSource(data);
        textureMipmapObject->SetMipmapCompressedImage(textureUploadTarget, level, internalformat, compressedBytes,
                                                      expectedImageSize);
        textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);
    }

    void CompressedTexImage1D_State(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border,
                                    GLsizei imageSize, const void* data) {
        const auto textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
        if (!ValidateTextureMutable(textureObject, __func__)) return;

        // TODO: implement compressed upload - see CompressedTexImage2D_State.
        RecordUnsupportedCompressedFormat(__func__);
    }

    void BindTexture_State(GLenum target, GLuint texture) {
        const Int activeUnit = MG_State::pGLContext->GetActiveTextureUnit();
        MGLOG_D("BindTexture_State called with target: 0x%X, texture: %u, unit: %d", target, texture, activeUnit);
        // ======================= Converting ================================
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);

        // ===================== Error Checking ==============================
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;

        // GL 3.3 core 3.8: name 0 is the target's default texture object - a real texture that
        // glTexImage*/glTexParameter*/glGetTex* must operate on - not "nothing bound". Binding it
        // restores the unit/target slot to its initial state.
        if (texture == 0) {
            auto& currentUnit = MG_State::pGLContext->GetTextureUnitObject(activeUnit);
            auto& bindingSlot = currentUnit.GetBindingSlot(textureTarget);
            const Bool changed = bindingSlot.Bind(MG_State::pGLContext->GetDefaultTextureObject(textureTarget));
            MG_State::pGLContext->NoteTextureUnitTouched(activeUnit, changed);
            return;
        }

        // Some desktop-side helper code saves GL_ACTIVE_TEXTURE and later feeds it back into glBindTexture
        // as if it were a texture name. Treating that as a no-op preserves the previous "invalid bind does not
        // change texture state" behavior, but avoids poisoning the error state every frame.
        if (!MG_State::pGLContext->ValidateTextureName(texture) && texture >= GL_TEXTURE0 && texture <= GL_TEXTURE31) {
            return;
        }

        // GL 3.3 core 3.8.1: a name that GenTextures never returned - or that has since been deleted -
        // is not a legal bind target in the core profile (no application-generated names), and the error
        // is INVALID_OPERATION, not INVALID_VALUE.
        if (!MG_State::pGLContext->ValidateTextureName(texture)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "BindTexture_State", "Invalid texture name"));
            return;
        }

        // ======================= Processing ================================
        Bool doesTextureExist = MG_State::pGLContext->ValidateTextureObject(texture);
        if (!doesTextureExist) {
            MG_State::pGLContext->CreateTextureObject(texture, textureTarget);
        }
        auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);

        // ===================== Error Checking ==============================
        if (doesTextureExist && !TextureImpl::ValidateTextureTargetUniformity(textureObject, textureTarget)) return;

        // ======================= Processing ================================
        auto& currentUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = currentUnit.GetBindingSlot(textureTarget);
        const Bool changed = bindingSlot.Bind(textureObject);
        MG_State::pGLContext->NoteTextureUnitTouched(MG_State::pGLContext->GetActiveTextureUnit(), changed);
    }

    void ActiveTexture_State(GLenum texture) {
        // ===================== Error Checking ==============================
        // GL 3.3 core 3.8: the valid range is [GL_TEXTURE0, GL_TEXTURE0 +
        // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS) - NOT a fixed 0..31 range. GL CTS's per-case
        // state reset iterates every advertised combined unit, so rejecting units the
        // implementation itself reports would leave a sticky GL_INVALID_ENUM behind and abort
        // whole test batches. The backend already clamps its advertised value to the state
        // layer's MAX_TEXTURE_IMAGE_UNITS capacity.
        Int maxCombinedUnits = static_cast<Int>(MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS);
        if (MG_Backend::pActiveBackendObject) {
            maxCombinedUnits = std::min(
                maxCombinedUnits, MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxCombinedTextureImageUnits);
        }
        if (texture < GL_TEXTURE0 || static_cast<Int>(texture - GL_TEXTURE0) >= maxCombinedUnits) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ActiveTexture_State",
                    std::format("Texture must be one of GL_TEXTUREi, where i is in the range 0 to {}, but got "
                                "invalid enum: 0x{:X}, which may stand for unit {}.",
                                maxCombinedUnits - 1, texture, texture - GL_TEXTURE0)));
            return;
        }

        // ======================= Processing ================================
        const Int unit = (Int)texture - GL_TEXTURE0;
        MGLOG_D("ActiveTexture_State: unit = %d", unit);
        MG_State::pGLContext->SetActiveTextureUnit(unit);
    }

    void GetTexImage_Backend(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
        MG_Backend::gBackendFunctionsTable.GL.GetTexImage(target, level, format, type, pixels);
    }

    // Add to GL_Texture.cpp
    // The half of the GetTexImage/GetTextureImage error set (GL 4.6 core 8.11) that depends on the
    // resolved texture object rather than on how it was named. Shared because the by-name entry
    // point does not route through GetTexImage_State and so used to enforce none of it.
    Bool ValidateTextureImageQuery(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, GLint level,
                                   TextureInputFormat textureInputFormat, TexturePixelDataType texturePixelDataType,
                                   GLsizei bufSize, const void* pixels, const char* caller) {
        if (!TextureImpl::ValidateTextureObject(textureObject)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "No valid texture bound to target"));
            return false;
        }

        // A multisample texture has per-sample data with no single image to return, and a buffer
        // texture's data lives in the buffer object - neither target is in the accepted list.
        const auto target = textureObject->GetTarget();
        if (target == TextureTarget::Texture2DMultisample || target == TextureTarget::Texture2DMultisampleArray ||
            target == TextureTarget::TextureBuffer) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Texture target has no image to read back."));
            return false;
        }

        // Level range. The by-target path would reach these again inside
        // CopyTextureImageToClientOrPBO_State, but the by-name path on a backend that answers
        // GetTextureImage itself never gets there.
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return false;
        if (target == TextureTarget::TextureRectangle && level != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Level must be zero for rectangle textures"));
            return false;
        }

        // GL 4.6 core 8.11.4 names cube completeness as the only completeness a readback requires,
        // and for a cube map that is exactly what IsComplete() answers (all six faces defined at
        // every level). It must not speak for any other target: on a mip chain it also rejects
        // "level N defined, the levels below it not", which is a perfectly readable texture at
        // level N - and the shape glClearTexImage's conformance cases build, since they define
        // only the level they clear. The requested level's own existence is checked below.
        if ((target == TextureTarget::TextureCubeMap || target == TextureTarget::TextureCubeMapArray) &&
            !textureObject->IsComplete()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture is incomplete"));
            return false;
        }

        // Check PBO state
        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();

        if (pixelPackBufferObject) {
            // Check if PBO is mapped
            if (pixelPackBufferObject->IsMapped()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Pixel pack buffer is currently mapped"));
                return false;
            }

            // Check alignment
            const SizeT typeSize = MG_Util::GetTexturePixelDataTypeSize(texturePixelDataType);
            if (typeSize != 0 && reinterpret_cast<uintptr_t>(pixels) % typeSize != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "Pixel data not aligned for pixel pack buffer"));
                return false;
            }
        }

        // Shared format/type/internal-format matrix (packed-type pairing, depth-vs-color mismatch,
        // integer-ness). Also rejects STENCIL_INDEX readback, which needs GL_ARB_texture_stencil8
        // (not advertised by MobileGL).
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(
                textureInputFormat, textureObject->GetFormat(), texturePixelDataType)) {
            return false;
        }

        // GetTexImage-specific: DEPTH_STENCIL readback needs a depth-stencil texture (a depth-only
        // texture has no stencil data to return).
        if (textureInputFormat == TextureInputFormat::DepthStencil &&
            textureObject->GetFormat() != TextureInternalFormat::DepthStencil &&
            textureObject->GetFormat() != TextureInternalFormat::Depth24Stencil8 &&
            textureObject->GetFormat() != TextureInternalFormat::Depth32FStencil8) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "DEPTH_STENCIL readback requires a depth-stencil texture"));
            return false;
        }

        // The destination has to be big enough. This has to happen here rather than after the read
        // has been packed: any of the reasons the read can bail out early - an unmapped level, a
        // pack step that declines the format - would otherwise swallow the error entirely.
        if (textureObject->GetStorageType() == TextureStorageType::Mipmap) {
            const auto* textureMipmapObject =
                static_cast<const MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
            const auto& uploadTargets = textureObject->GetUploadTargets();
            // The half of the completeness gate above that GL does keep: the REQUESTED level has
            // to hold an image. A name that was never given one carries no levels at all (which is
            // also what an Unknown internal format answers), and a chain grown to reach level N
            // leaves every level below it at {0, 0, 0}.
            if (uploadTargets.empty() || static_cast<Uint>(level) >= textureMipmapObject->GetMipmapLevelCount()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture level has no image to read back."));
                return false;
            }
            const auto texelSize = textureMipmapObject->GetMipmapTexelSize(uploadTargets[0], level);
            if (texelSize.x() <= 0 || texelSize.y() <= 0 || texelSize.z() <= 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture level has no image to read back."));
                return false;
            }

            // Tightly packed, and summed over every face because a cube map query returns all
            // six. Pack pixel-store state only ever grows this, so a request rejected here
            // could not have fit under any packing.
            const SizeT required = MG_Util::CalculateInputTextureImageSize(textureInputFormat,
                                                                           texturePixelDataType, texelSize) *
                                   uploadTargets.size();

            if (bufSize >= 0 && static_cast<SizeT>(bufSize) < required) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Destination buffer is too small."));
                return false;
            }

            if (pixelPackBufferObject) {
                const SizeT bufferSize = pixelPackBufferObject->GetSize();
                const SizeT offset = reinterpret_cast<SizeT>(pixels);
                if (offset > bufferSize || required > bufferSize - offset) {
                    MG_State::pGLContext->RecordError(
                        ErrorCode::InvalidOperation,
                        MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                     "Packing would write past the end of the pixel pack buffer."));
                    return false;
                }
            }
        }

        return true;
    }

    Bool GetTexImage_State(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
        // ======================= Converting ================================
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);

        // ===================== Error Checking ==============================
        // Validate target
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Invalid texture target"));
            return false;
        }

        // Validate level
        if (level < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Level must be non-negative"));
            return false;
        }

        // Validate format
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Invalid format"));
            return false;
        }

        // Validate type
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetTexImage_State", "Invalid pixel data type"));
            return false;
        }

        // Get texture object
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        Bool isProxy = TextureImpl::IsProxyTextureTarget(textureUploadTarget);
        auto& textureObject =
            isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                    : bindingSlot.GetBoundObject();

        // glGetTexImage has no bufSize argument: -1 stands for "no client-side limit".
        return ValidateTextureImageQuery(textureObject, level, textureInputFormat, texturePixelDataType, -1, pixels,
                                         "GetTexImage_State");
    }

    void CopyTextureImageToClientOrPBO_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                            TextureUploadTarget textureUploadTarget, GLint level, GLenum format,
                                            GLenum type, GLsizei bufSize, void* pixels, const char* caller) {
        if (!textureObject) return;

        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture storage is not mipmap-backed."));
            return;
        }

        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (static_cast<Uint>(level) >= textureMipmapObject->GetMipmapLevelCount()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture level is out of range."));
            return;
        }

        const auto texelSize = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level);
        const void* src = textureMipmapObject->MapMipmapData(textureUploadTarget, level);
        if (!src) return;

        SizeT packedSize = 0;
        void* packedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataPack(
            src, MG_State::pGLContext->GetPixelStoreParameters(false), textureObject->GetFormat(), texturePixelDataType,
            textureInputFormat, texturePixelDataType, texelSize, false, packedSize);
        if (!packedPixels || packedSize == 0) {
            if (packedPixels) free(packedPixels);
            return;
        }

        if (bufSize >= 0 && static_cast<SizeT>(bufSize) < packedSize) {
            free(packedPixels);
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Destination buffer is too small."));
            return;
        }

        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
        if (pixelPackBufferObject) {
            const SizeT offset = reinterpret_cast<SizeT>(pixels);
            if (offset + packedSize > pixelPackBufferObject->GetSize()) {
                free(packedPixels);
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Pixel pack buffer is too small."));
                return;
            }
            pixelPackBufferObject->UploadSubData({packedPixels, packedSize}, offset);
        } else if (pixels) {
            Memcpy(pixels, packedPixels, packedSize);
        }

        free(packedPixels);
    }

    /* @INSERTION_POINT:FUNCTION_IMPLEMENTATION@ */
    void CreateTextures(GLenum target, GLsizei n, GLuint* textures) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "n must be non-negative."));
            return;
        }
        if (n > 0 && !textures) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture output pointer cannot be null."));
            return;
        }

        switch (target) {
        case GL_TEXTURE_1D:
        case GL_TEXTURE_2D:
        case GL_TEXTURE_3D:
        case GL_TEXTURE_1D_ARRAY:
        case GL_TEXTURE_2D_ARRAY:
        case GL_TEXTURE_RECTANGLE:
        case GL_TEXTURE_CUBE_MAP:
        case GL_TEXTURE_CUBE_MAP_ARRAY:
        case GL_TEXTURE_BUFFER:
        case GL_TEXTURE_2D_MULTISAMPLE:
        case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Invalid texture target."));
            return;
        }

        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;

        Vector<Uint> textureNames;
        MG_State::pGLContext->GenTextureNames(n, textureNames);
        for (GLsizei i = 0; i < n; ++i) {
            textures[i] = textureNames[i];
            MG_State::pGLContext->CreateTextureObject(textureNames[i], textureTarget);
        }
    }

    // Shared front half of glTextureStorage1D/2D/3D. `dimension` selects which set of targets the
    // entry point accepts; `depth` is 1 for the lower-dimensional forms.
    static Bool ValidateTextureStorageShape(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                            int dimension, GLsizei levels, GLsizei width, GLsizei height,
                                            GLsizei depth, const char* caller) {
        if (levels < 1) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "levels must be positive."));
            return false;
        }
        // Immutable storage has to describe a real image, so unlike glTexImage*D a zero extent is
        // out of range rather than a legal empty level (GL 4.6 core 8.19).
        if (width < 1 || height < 1 || depth < 1) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "width, height and depth must be positive."));
            return false;
        }
        if (!IsTextureStorageTargetForDimension(textureObject->GetTarget(), dimension)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("The effective target {} is not accepted by this entry point.",
                                MG_Util::ConvertTextureTargetToString(textureObject->GetTarget()))));
            return false;
        }
        const Uint maxLevels = MaxTextureStorageLevels(textureObject->GetTarget(), width, height, depth);
        if (static_cast<Uint>(levels) > maxLevels) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("levels {} exceeds the {} the level-zero size admits.", levels, maxLevels)));
            return false;
        }
        return true;
    }

    void TextureStorage1D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        if (!ValidateTextureStorageInternalFormat(textureInternalFormat, __func__)) return;
        if (!ValidateTextureStorageShape(textureObject, 1, levels, width, 1, 1, __func__)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }
        if (!ValidateTextureMutable(textureObject, __func__)) return;

        const auto textureUploadTarget = GetPrimaryUploadTarget(textureObject);
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        textureObject->SetInternalFormat(textureInternalFormat);
        for (GLsizei level = 0; level < levels; ++level) {
            const GLsizei levelWidth = std::max<GLsizei>(1, width >> level);
            const SizeT byteSize = ComputeTextureStorageByteSize(textureInternalFormat, levelWidth, 1, 1);
            textureMipmapObject->AllocateStorage(textureUploadTarget, level, {{levelWidth, 1, 1}, byteSize});
            textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, false);
        }
        // Immutable storage defines exactly `levels` levels; AllocateStorage only grows, so a
        // longer pre-existing chain has to be dropped explicitly.
        textureMipmapObject->TruncateMipmapLevels(textureUploadTarget, static_cast<Uint>(levels));
        textureObject->SetImmutableLevels(static_cast<Uint>(levels));
    }

    void TextureStorage2D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        if (!ValidateTextureStorageInternalFormat(textureInternalFormat, __func__)) return;
        if (!ValidateTextureStorageShape(textureObject, 2, levels, width, height, 1, __func__)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }
        if (!ValidateTextureMutable(textureObject, __func__)) return;
        if (textureObject->GetTarget() == TextureTarget::TextureCubeMap && width != height) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Cube map immutable storage must be square."));
            return;
        }

        auto textureUploadTarget = GetPrimaryUploadTarget(textureObject);
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        GLenum realInternalFormat = internalformat;
        GLenum realFormat = GL_RGBA;
        GLenum realType = GL_UNSIGNED_BYTE;
        MG_Util::TextureFormatProcessor::NormalizePixelFormat(
            MG_Util::ConvertTextureInternalFormatToGLEnum(textureInternalFormat), PixelFormatNormalizeOptionBit::None,
            &realInternalFormat, &realFormat, &realType);
        const SizeT bytesPerPixel = MG_Util::GetInternalBytesPerPixel(
            textureInternalFormat, MG_Util::ConvertGLEnumToTexturePixelDataType(realType));

        textureObject->SetInternalFormat(textureInternalFormat);
        // A cube map has six upload targets and glTexStorage2D allocates all of them at once (GL 4.6
        // core 8.19). Allocating only the primary one left the object cube-incomplete, so every
        // framebuffer it was attached to reported GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT. Every other
        // 2D target has exactly one upload target, so this loop is a no-op change for them.
        // A specific compressed internalformat commits every level it allocates to that
        // format, the same way glTexImage2D does - and here it matters twice over, because
        // immutable storage plus glCompressedTexSubImage2D IS the modern way to upload a
        // compressed texture: without the tag that sub-image call finds an uncompressed
        // level and refuses it. Zero width means a generic (implementation's choice)
        // format, which MobileGL answers with uncompressed storage, so it is not tagged.
        const auto compressedInfo = MG_Util::GetCompressedFormatInfo(internalformat);
        for (const auto uploadTarget : textureObject->GetUploadTargets()) {
            for (GLsizei level = 0; level < levels; ++level) {
                const GLsizei levelWidth = std::max<GLsizei>(1, width >> level);
                const GLsizei levelHeight = std::max<GLsizei>(1, height >> level);
                const SizeT byteSize =
                    static_cast<SizeT>(levelWidth) * static_cast<SizeT>(levelHeight) * bytesPerPixel;
                textureMipmapObject->AllocateStorage(uploadTarget, level, {{levelWidth, levelHeight, 1}, byteSize});
                textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                if (compressedInfo.blockWidth != 0) {
                    // After AllocateStorage, which clears the tag.
                    textureMipmapObject->SetMipmapCompressedImage(
                        uploadTarget, static_cast<Uint>(level), internalformat, nullptr,
                        MG_Util::CalculateCompressedTextureImageSize(compressedInfo,
                                                                     {levelWidth, levelHeight, 1}));
                }
            }
            // See TextureStorage1D.
            textureMipmapObject->TruncateMipmapLevels(uploadTarget, static_cast<Uint>(levels));
        }
        textureObject->SetImmutableLevels(static_cast<Uint>(levels));
    }

    // No block-compressed format is defined for a three-dimensional image, so glTexStorage3D on
    // TEXTURE_3D must reject one - and with INVALID_OPERATION, not the INVALID_ENUM an unknown
    // sized format gets (GL 4.6 core 8.19 / Khronos bug 11239, KHR-GLxx.texture_storage
    // .compressed_data). Written against the enum ranges rather than a name list because the
    // families are contiguous and MobileGL's own internal-format enum drops the ones it cannot
    // carry, which would make this check silently narrower than the API surface.
    static Bool IsCompressedGLInternalFormat(GLenum internalformat) {
        switch (internalformat) {
        case 0x8225: // GL_COMPRESSED_RED
        case 0x8226: // GL_COMPRESSED_RG
        case 0x84ED: // GL_COMPRESSED_RGB
        case 0x84EE: // GL_COMPRESSED_RGBA
        case 0x8C48: // GL_COMPRESSED_SRGB
        case 0x8C49: // GL_COMPRESSED_SRGB_ALPHA
            return true;
        default:
            break;
        }
        return (internalformat >= 0x83F0 && internalformat <= 0x83F3) || // S3TC / DXT
               (internalformat >= 0x8DBB && internalformat <= 0x8DBE) || // RGTC
               (internalformat >= 0x8E8C && internalformat <= 0x8E8F) || // BPTC
               (internalformat >= 0x9270 && internalformat <= 0x9279) || // ETC2 / EAC
               (internalformat >= 0x93B0 && internalformat <= 0x93BD) || // ASTC LDR
               (internalformat >= 0x93D0 && internalformat <= 0x93DD);   // ASTC sRGB
    }

    void TextureStorage3D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height,
                          GLsizei depth) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        if (textureObject->GetTarget() == TextureTarget::Texture3D &&
            IsCompressedGLInternalFormat(internalformat)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    std::format("{} is a compressed internal format and cannot back GL_TEXTURE_3D storage.",
                                MG_Util::ConvertGLEnumToString(internalformat))));
            return;
        }
        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        if (!ValidateTextureStorageInternalFormat(textureInternalFormat, __func__)) return;
        if (!ValidateTextureStorageShape(textureObject, 3, levels, width, height, depth, __func__)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }
        if (!ValidateTextureMutable(textureObject, __func__)) return;
        if (textureObject->GetTarget() == TextureTarget::TextureCubeMapArray &&
            (width != height || depth % 6 != 0)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "Cube map array immutable storage must be square with depth multiple of 6."));
            return;
        }

        const auto textureUploadTarget = GetPrimaryUploadTarget(textureObject);
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        textureObject->SetInternalFormat(textureInternalFormat);
        // Array targets keep their layer count constant across levels; only true 3D
        // textures halve depth per level (GL 3.3 §3.9 glTexStorage3D).
        const Bool depthMips = DepthParticipatesInMipmapping(textureObject->GetTarget());
        // The same specific-compressed-format tag glTexStorage2D records, for the array targets a
        // compressed glTexStorage3D is legal on (GL_TEXTURE_3D was refused above). Zero width means
        // a generic format, which MobileGL answers with uncompressed storage, so it is not tagged.
        const auto compressedInfo = MG_Util::GetCompressedFormatInfo(internalformat);
        for (GLsizei level = 0; level < levels; ++level) {
            const GLsizei levelWidth = std::max<GLsizei>(1, width >> level);
            const GLsizei levelHeight = std::max<GLsizei>(1, height >> level);
            const GLsizei levelDepth = depthMips ? std::max<GLsizei>(1, depth >> level) : depth;
            const SizeT byteSize = ComputeTextureStorageByteSize(textureInternalFormat, levelWidth, levelHeight,
                                                                 levelDepth);
            textureMipmapObject->AllocateStorage(textureUploadTarget, level,
                                                 {{levelWidth, levelHeight, levelDepth}, byteSize});
            textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, false);
            if (compressedInfo.blockWidth != 0) {
                // After AllocateStorage, which clears the tag.
                textureMipmapObject->SetMipmapCompressedImage(
                    textureUploadTarget, static_cast<Uint>(level), internalformat, nullptr,
                    MG_Util::CalculateCompressedTextureImageSize(compressedInfo,
                                                                 {levelWidth, levelHeight, levelDepth}));
            }
        }
        // See TextureStorage1D.
        textureMipmapObject->TruncateMipmapLevels(textureUploadTarget, static_cast<Uint>(levels));
        textureObject->SetImmutableLevels(static_cast<Uint>(levels));
    }

    // Shared front half of glTextureStorage2DMultisample/3DMultisample. The target forms are reached
    // through a binding and get their target validated there; by name the object itself has to be
    // checked, and so do the extents, which the binding path never sees (GL 4.6 core 8.19).
    static Bool ValidateNamedMultisampleStorage(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                TextureTarget expectedTarget, GLsizei samples, GLsizei width,
                                                GLsizei height, GLsizei depth, const char* caller) {
        if (!textureObject) return false;
        if (textureObject->GetTarget() != expectedTarget) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("The effective target {} is not accepted by this entry point.",
                                MG_Util::ConvertTextureTargetToString(textureObject->GetTarget()))));
            return false;
        }
        if (width < 1 || height < 1 || depth < 1) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "width, height and depth must be positive."));
            return false;
        }
        const auto& limits = MG_Backend::pActiveBackendObject->GetDynamicParameters();
        if (width > limits.MaxTextureSize || height > limits.MaxTextureSize) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "width and height must not exceed GL_MAX_TEXTURE_SIZE."));
            return false;
        }
        if (depth > limits.MaxArrayTextureLayers) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "depth must not exceed GL_MAX_ARRAY_TEXTURE_LAYERS."));
            return false;
        }
        // More samples than the implementation offers is a request it cannot serve rather than a
        // malformed argument, so INVALID_OPERATION and not INVALID_VALUE. The limit comes from the
        // getter rather than the backend parameter it is derived from, because the frontend raises
        // that number - validating against the raw one would reject a count GL_MAX_SAMPLES
        // advertises.
        GLint maxSamples = 1;
        GetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        if (samples > maxSamples) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "samples exceeds GL_MAX_SAMPLES."));
            return false;
        }
        // Storage is defined once. The mipmap forms go through ValidateTextureMutable for this; the
        // multisample ones reached the backend without ever asking.
        if (!ValidateTextureMutable(textureObject, caller)) return false;
        return true;
    }

    void TextureStorage2DMultisample(GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLboolean fixedsamplelocations) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!ValidateNamedMultisampleStorage(textureObject, TextureTarget::Texture2DMultisample, samples, width, height,
                                             1, __func__))
            return;
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            TexStorage2DMultisample(target, samples, internalformat, width, height, fixedsamplelocations);
        });
    }

    void TextureStorage3DMultisample(GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!ValidateNamedMultisampleStorage(textureObject, TextureTarget::Texture2DMultisampleArray, samples, width,
                                             height, depth, __func__))
            return;
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            TexStorage3DMultisample(target, samples, internalformat, width, height, depth, fixedsamplelocations);
        });
    }

    void TexStorage1D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width) {
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;
        // Not ValidateTextureUploadTarget: GL_TEXTURE_CUBE_MAP is a legal glTexStorage2D target but
        // has no single upload target - it allocates all six faces - so validating one would reject
        // it. The accepted set for this entry point is the dimension's storage targets, and the
        // by-name form below does the per-face work.
        if (!IsTextureStorageTargetForDimension(textureTarget, 1)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::format("Target {} does not take 1D immutable storage.",
                                                         MG_Util::ConvertGLEnumToString(target))));
            return;
        }

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!TextureImpl::ValidateTextureNotDefault(textureObject, __func__)) return;

        TextureStorage1D(textureObject->GetExternalIndex(), levels, internalformat, width);
    }

    void TexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;
        // Not ValidateTextureUploadTarget: GL_TEXTURE_CUBE_MAP is a legal glTexStorage2D target but
        // has no single upload target - it allocates all six faces - so validating one would reject
        // it. The accepted set for this entry point is the dimension's storage targets, and the
        // by-name form below does the per-face work.
        if (!IsTextureStorageTargetForDimension(textureTarget, 2)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::format("Target {} does not take 2D immutable storage.",
                                                         MG_Util::ConvertGLEnumToString(target))));
            return;
        }

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!TextureImpl::ValidateTextureNotDefault(textureObject, __func__)) return;

        TextureStorage2D(textureObject->GetExternalIndex(), levels, internalformat, width, height);
    }

    void TexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height,
                      GLsizei depth) {
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;
        // Not ValidateTextureUploadTarget: GL_TEXTURE_CUBE_MAP is a legal glTexStorage2D target but
        // has no single upload target - it allocates all six faces - so validating one would reject
        // it. The accepted set for this entry point is the dimension's storage targets, and the
        // by-name form below does the per-face work.
        if (!IsTextureStorageTargetForDimension(textureTarget, 3)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::format("Target {} does not take 3D immutable storage.",
                                                         MG_Util::ConvertGLEnumToString(target))));
            return;
        }

        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& bindingSlot = activeUnit.GetBindingSlot(textureTarget);
        auto& textureObject = bindingSlot.GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!TextureImpl::ValidateTextureNotDefault(textureObject, __func__)) return;

        TextureStorage3D(textureObject->GetExternalIndex(), levels, internalformat, width, height, depth);
    }

    // Unlike glTexImage*Multisample, where a zero-sized image is a legal deallocation (see
    // AllocateMultisampleTextureStorage), the immutable forms take a strictly positive size: GL
    // 4.6 core 8.19 makes width, height or depth < 1 INVALID_VALUE. Without this the shared
    // _State helper would deallocate the image and TexStorageMultisample_State would then freeze
    // the now-imageless texture as immutable.
    static Bool ValidateTexStorageMultisampleSize(GLsizei width, GLsizei height, GLsizei depth, const char* caller) {
        if (width >= 1 && height >= 1 && depth >= 1) {
            return true;
        }
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidValue,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                         "Immutable multisample storage requires width, height and depth >= 1."));
        return false;
    }

    // The multisample storage forms allocate exactly what the glTexImage*Multisample ones do, and
    // then freeze it: TEXTURE_IMMUTABLE_FORMAT becomes TRUE and a second call is INVALID_OPERATION
    // (GL 4.6 core 8.19). Only the allocation was shared before, so a multisample texture stayed
    // mutable forever and could be respecified any number of times.
    static void TexStorageMultisample_State(GLenum target, Bool allocated, const char* caller) {
        static_cast<void>(caller);
        if (!allocated) return;
        const TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (TextureImpl::IsProxyTextureTarget(MG_Util::ConvertGLEnumToTextureUploadTarget(target))) return;
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& textureObject = activeUnit.GetBindingSlot(textureTarget).GetBoundObject();
        if (!textureObject) return;
        textureObject->SetImmutableLevels(1);
    }

    void TexStorage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLboolean fixedsamplelocations) {
        const TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        if (!ValidateTextureMutable(activeUnit.GetBindingSlot(textureTarget).GetBoundObject(), __func__)) return;
        if (!ValidateTexStorageMultisampleSize(width, height, 1, __func__)) return;
        TexStorageMultisample_State(
            target, TexImage2DMultisample_State(target, samples, internalformat, width, height, fixedsamplelocations),
            __func__);
    }

    void TexStorage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
        const TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        if (!ValidateTextureMutable(activeUnit.GetBindingSlot(textureTarget).GetBoundObject(), __func__)) return;
        if (!ValidateTexStorageMultisampleSize(width, height, depth, __func__)) return;
        TexStorageMultisample_State(target,
                                    TexImage3DMultisample_State(target, samples, internalformat, width, height, depth,
                                                                fixedsamplelocations),
                                    __func__);
    }

    void TextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                           const void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            TexSubImage1D_State(target, level, xoffset, width, format, type, pixels);
        });
    }

    void TextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                           GLenum format, GLenum type, const void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;

        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, 1)) return;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }

        auto textureUploadTarget = GetPrimaryUploadTarget(textureObject);
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(textureInputFormat, textureObject->GetFormat(),
                                                                           texturePixelDataType))
            return;

        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (static_cast<Uint>(level) >= textureMipmapObject->GetMipmapLevelCount()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture level is out of range."));
            return;
        }
        if (!TextureImpl::ValidateTextureSubImageOffsets(textureObject, xoffset, width, yoffset, height)) return;
        // This entry point does not go through TexSubImage2D_State, so it needs the unpack-buffer
        // rules of its own.
        if (!ValidatePixelUnpackBufferSource(pixels, textureInputFormat, texturePixelDataType, {width, height, 1},
                                             __func__))
            return;

        const void* originalPixels = pixels;
        const auto& pixelUnpackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (pixelUnpackBufferObject) {
            originalPixels = reinterpret_cast<const char*>(pixelUnpackBufferObject->MappedData()) +
                             reinterpret_cast<SizeT>(pixels);
        }
        if (!originalPixels) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "No data supplied from pixels parameter and no PBO bound."));
            return;
        }

        SizeT inputSize = 0;
        void* processedPixels = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            originalPixels, MG_State::pGLContext->GetPixelStoreParameters(true), textureObject->GetFormat(),
            textureInputFormat, texturePixelDataType, {width, height, 1}, false, inputSize);
        if (!processedPixels || inputSize == 0) {
            if (processedPixels) free(processedPixels);
            return;
        }

        const auto texelSize = textureMipmapObject->GetMipmapTexelSize(textureUploadTarget, level);
        const SizeT internalBpp = MG_Util::GetInternalBytesPerPixel(textureObject->GetFormat(), texturePixelDataType);
        const SizeT srcRowSize = static_cast<SizeT>(width) * internalBpp;
        const SizeT destRowSize = static_cast<SizeT>(texelSize.x()) * internalBpp;

        const auto* srcData = static_cast<const Uint8*>(processedPixels);
        Uint8* destData = static_cast<Uint8*>(textureMipmapObject->MapMipmapData(textureUploadTarget, level));
        if (destData) {
            for (GLsizei y = 0; y < height; ++y) {
                const SizeT destRowOffset = static_cast<SizeT>(yoffset + y) * destRowSize +
                                            static_cast<SizeT>(xoffset) * internalBpp;
                const SizeT srcRowOffset = static_cast<SizeT>(y) * srcRowSize;
                Memcpy(destData + destRowOffset, srcData + srcRowOffset, srcRowSize);
            }
            textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);
        }
        free(processedPixels);
    }

    void CompressedTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                     GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            CompressedTexSubImage2D_State(target, level, xoffset, yoffset, width, height, format, imageSize, data);
        });
    }

    void TextureSubImage3D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                           GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            TexSubImage3D_State(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
        });
    }

    void TextureParameteri(GLuint texture, GLenum pname, GLint param) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        TextureParameterObject_State(textureObject, pname, param, __func__);
    }

    void TextureParameterf(GLuint texture, GLenum pname, GLfloat param) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        TextureParameterObjectf_State(textureObject, pname, param, __func__);
    }

    void TextureParameterfv(GLuint texture, GLenum pname, const GLfloat* params) {
        if (!params) return;
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { TexParameterfv_State(target, pname, params); });
    }

    void TextureParameteriv(GLuint texture, GLenum pname, const GLint* params) {
        if (!params) return;
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { TexParameteriv_State(target, pname, params); });
    }

    void TextureParameterIiv(GLuint texture, GLenum pname, const GLint* params) {
        if (!params) return;
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            TexParameterIiv_State(target, pname, params);
        });
    }

    void TextureParameterIuiv(GLuint texture, GLenum pname, const GLuint* params) {
        if (!params) return;
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            TexParameterIuiv_State(target, pname, params);
        });
    }

    void BindTextureUnit(GLuint unit, GLuint texture) {
        if (unit >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture unit is out of range."));
            return;
        }

        auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(static_cast<Int>(unit));
        if (texture == 0) {
            // GL 4.5 8.1: texture zero unbinds every target of the unit, i.e. rebinds each
            // target's default texture object (the unit's initial state).
            Bool changed = false;
            for (auto& slot : textureUnit.GetAllBindingSlots()) {
                if (slot.Bind(MG_State::pGLContext->GetDefaultTextureObject(slot.GetTarget()))) changed = true;
            }
            MG_State::pGLContext->NoteTextureUnitTouched(static_cast<Int>(unit), changed);
            return;
        }

        auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture object does not exist."));
            return;
        }
        const Bool changed = textureUnit.GetBindingSlot(textureObject->GetTarget()).Bind(textureObject);
        MG_State::pGLContext->NoteTextureUnitTouched(static_cast<Int>(unit), changed);
    }

    GLint GetCombinedTextureImageUnitCount() {
        GLint maxTextureUnits = 0;
        GetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxTextureUnits);
        return std::min<GLint>(std::max(maxTextureUnits, 0), MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS);
    }

    namespace {
        // ARB_multi_bind checks the whole [first, first + count) range before binding anything and
        // reports an overrun as INVALID_OPERATION - not the INVALID_VALUE the single-bind entry
        // points report for an out-of-range unit, and not after binding the in-range prefix.
        Bool ValidateMultiBindUnitRange(GLuint first, GLsizei count, GLint unitCount, const char* funcName) {
            if (count < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName, "count must be non-negative."));
                return false;
            }
            if (static_cast<Uint64>(first) + static_cast<Uint64>(count) > static_cast<Uint64>(unitCount)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName,
                                                 std::format("first + count ({} + {}) exceeds the {} available units.",
                                                             first, count, unitCount)));
                return false;
            }
            return true;
        }

        // ARB_multi_bind states the equivalence to a loop of single binds "except that <textures>
        // will not be created if they do not exist": glBindTexture instantiates a name GenTextures
        // merely reserved, the multi-bind entry points must refuse it. The error class is
        // INVALID_OPERATION for both of them, where the scalar glBindImageTexture reports
        // INVALID_VALUE - hence the check here rather than inside BindImageTexture.
        //
        // Deliberately PER ELEMENT: the extension defines these calls as a loop, so a bad entry
        // costs its own unit and leaves the rest of the range bound.
        SharedPtr<MG_State::GLState::ITextureObject> ResolveMultiBindTexture(GLuint texture, GLsizei index,
                                                                            const char* funcName) {
            SharedPtr<MG_State::GLState::ITextureObject> textureObject =
                MG_State::pGLContext->GetTextureObject(texture);
            if (!textureObject) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", funcName,
                        std::format("textures[{}] ({}) is not the name of an existing texture object.", index,
                                    texture)));
            }
            return textureObject;
        }

        // ARB_multi_bind: an element naming texture zero unbinds EVERY target of its unit, i.e.
        // rebinds each target's default texture object - the unit's initial state. Same rule
        // glBindTextureUnit(unit, 0) follows.
        void UnbindAllTargetsOnUnit(Int unit) {
            auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
            Bool changed = false;
            for (auto& slot : textureUnit.GetAllBindingSlots()) {
                if (slot.Bind(MG_State::pGLContext->GetDefaultTextureObject(slot.GetTarget()))) changed = true;
            }
            MG_State::pGLContext->NoteTextureUnitTouched(unit, changed);
        }
    } // namespace

    // ARB_multi_bind: glBindTextures binds each texture to ITS OWN target on unit <first> + i, so
    // there is no target parameter and no way to express it through glBindTexture - the per-unit,
    // by-object form glBindTextureUnit uses is the one that matches. A NULL <textures> unbinds the
    // whole range.
    void BindTextures(GLuint first, GLsizei count, const GLuint* textures) {
        if (!ValidateMultiBindUnitRange(first, count, GetCombinedTextureImageUnitCount(), __func__)) return;

        for (GLsizei i = 0; i < count; ++i) {
            const GLuint texture = textures ? textures[i] : 0;
            const Int unit = static_cast<Int>(first) + i;
            if (texture == 0) {
                UnbindAllTargetsOnUnit(unit);
                continue;
            }
            const SharedPtr<MG_State::GLState::ITextureObject> textureObject =
                ResolveMultiBindTexture(texture, i, __func__);
            if (!textureObject) continue;

            auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
            const Bool changed = textureUnit.GetBindingSlot(textureObject->GetTarget()).Bind(textureObject);
            MG_State::pGLContext->NoteTextureUnitTouched(unit, changed);
        }
    }

    // ARB_multi_bind: glBindImageTextures is a loop of glBindImageTexture with every parameter but
    // the unit and the texture fixed by the spec - level 0, layered, layer 0, READ_WRITE, and the
    // texture's own internal format. An element that names texture zero resets the unit.
    void BindImageTextures(GLuint first, GLsizei count, const GLuint* textures) {
        if (!ValidateMultiBindUnitRange(first, count, static_cast<GLint>(GetAdvertisedImageUnitCount()), __func__)) {
            return;
        }

        for (GLsizei i = 0; i < count; ++i) {
            const GLuint texture = textures ? textures[i] : 0;
            const GLuint unit = first + static_cast<GLuint>(i);
            if (texture == 0) {
                BindImageTexture(unit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
                continue;
            }
            const SharedPtr<MG_State::GLState::ITextureObject> textureObject =
                ResolveMultiBindTexture(texture, i, __func__);
            if (!textureObject) continue;

            // "An INVALID_OPERATION error is generated if the internal format of any texture is not
            // supported for image textures" - a texture that has never been given storage has no
            // format at all and lands here too, rather than being reported as a bad enum by the
            // scalar path.
            const GLenum format = MG_Util::ConvertTextureInternalFormatToGLEnum(textureObject->GetFormat());
            if (!IsValidImageTextureFormat(format)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        std::format("textures[{}] ({}) has an internal format that is not supported for image "
                                    "textures.",
                                    i, texture)));
                continue;
            }
            BindImageTexture(unit, texture, 0, GL_TRUE, 0, GL_READ_WRITE, format);
        }
    }

    void GetTextureImage(GLuint texture, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        if (!ValidateTextureImageQuery(textureObject, level, MG_Util::ConvertGLEnumToTextureInputFormat(format),
                                       MG_Util::ConvertGLEnumToTexturePixelDataType(type), bufSize, pixels,
                                       __func__)) {
            return;
        }
        const auto uploadTarget = GetPrimaryUploadTarget(textureObject);
        if (MG_Backend::pActiveBackendObject != nullptr &&
            MG_Backend::pActiveBackendObject->GetBackendType() == BackendType::DirectVulkan &&
            MG_Backend::gBackendFunctionsTable.GL.GetTextureImage != nullptr) {
            MG_Backend::gBackendFunctionsTable.GL.GetTextureImage(textureObject, uploadTarget, level, format, type,
                                                                  bufSize, pixels);
            return;
        }
        CopyTextureImageToClientOrPBO_State(textureObject, uploadTarget, level, format, type, bufSize, pixels,
                                            __func__);
    }

    void GetCompressedTextureImage(GLuint texture, GLint level, GLsizei bufSize, void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        // Level first: GL 4.6 core 8.11 wants INVALID_VALUE for an out-of-range level even when the
        // texture would also fail the compressed check below.
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;

        // Unlike glGetTextureImage this never asks a backend: the compressed image only ever exists
        // in the CPU shadow (no backend was handed the compressed bytes at all), so the shadow is
        // authoritative rather than potentially stale.
        CopyCompressedTextureImageToClientOrPBO(textureObject, GetPrimaryUploadTarget(textureObject), level, bufSize,
                                                pixels, __func__);
    }

    void GetTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                            GLsizei height, GLsizei depth, GLenum format, GLenum type, GLsizei bufSize, void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        if (level < 0 || xoffset < 0 || yoffset < 0 || zoffset < 0 || width < 0 || height < 0 || depth < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture sub-image range is invalid."));
            return;
        }
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture storage is not mipmap-backed."));
            return;
        }

        const auto uploadTarget = GetPrimaryUploadTarget(textureObject);
        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (static_cast<Uint>(level) >= textureMipmapObject->GetMipmapLevelCount()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture level is out of range."));
            return;
        }

        const auto texelSize = textureMipmapObject->GetMipmapTexelSize(uploadTarget, static_cast<Uint>(level));
        const Bool isFullLevelRead = xoffset == 0 && yoffset == 0 && zoffset == 0 &&
                                     width == texelSize.x() && height == texelSize.y() &&
                                     depth == texelSize.z();
        if (!isFullLevelRead) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Partial texture sub-image readback is not implemented yet."));
            return;
        }

        GetTextureImage(texture, level, format, type, bufSize, pixels);
    }

    // A buffer texture carries none of the sampler or level state these queries report. Reached by
    // name there is no target token to blame, so the wrong object is INVALID_OPERATION rather than
    // the INVALID_ENUM the target forms report for an unaccepted target (GL 4.6 core 8.11).
    static Bool ValidateNamedTextureHasParameters(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                  const char* caller) {
        if (!textureObject) return false;
        if (textureObject->GetStorageType() == TextureStorageType::Buffer) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "The effective target of `texture` has no texture parameters."));
            return false;
        }
        return true;
    }

    void GetTextureParameteriv(GLuint texture, GLenum pname, GLint* params) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!ValidateNamedTextureHasParameters(textureObject, __func__)) return;
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { GetTexParameteriv_State(target, pname, params); });
    }

    void GetTextureParameterfv(GLuint texture, GLenum pname, GLfloat* params) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!ValidateNamedTextureHasParameters(textureObject, __func__)) return;
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { GetTexParameterfv_State(target, pname, params); });
    }

    void GetTextureParameterIiv(GLuint texture, GLenum pname, GLint* params) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!ValidateNamedTextureHasParameters(textureObject, __func__)) return;
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { GetTexParameterIiv_State(target, pname, params); });
    }

    void GetTextureParameterIuiv(GLuint texture, GLenum pname, GLuint* params) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!ValidateNamedTextureHasParameters(textureObject, __func__)) return;
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) { GetTexParameterIuiv_State(target, pname, params); });
    }

    void GetTextureLevelParameteriv(GLuint texture, GLint level, GLenum pname, GLint* params) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            GetTexLevelParameteriv_State(target, level, pname, params);
        });
    }

    void GetTextureLevelParameterfv(GLuint texture, GLint level, GLenum pname, GLfloat* params) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            GetTexLevelParameterfv_State(target, level, pname, params);
        });
    }

    void BindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                          GLenum format) {
        if (unit >= GetAdvertisedImageUnitCount()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Image texture unit is out of range."));
            return;
        }
        if (level < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture level must be non-negative."));
            return;
        }
        if (layer < 0 && layered == GL_FALSE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture layer must be non-negative."));
            return;
        }
        if (access != GL_READ_ONLY && access != GL_WRITE_ONLY && access != GL_READ_WRITE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Invalid image texture access."));
            return;
        }
        if (!IsValidImageTextureFormat(format)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Invalid image texture format."));
            return;
        }

        SharedPtr<MG_State::GLState::ITextureObject> textureObject;
        if (texture != 0) {
            if (!MG_State::pGLContext->ValidateTextureObject(texture)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture name is not a texture object."));
                return;
            }
            textureObject = MG_State::pGLContext->GetTextureObject(texture);
        }

        auto bindImageTexture = MG_Backend::gBackendFunctionsTable.GL.BindImageTexture;
        if (!bindImageTexture) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support image texture binding."));
            return;
        }

        MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(unit))
            .Bind(textureObject, level, layered, layer, access, format);
        MG_State::pGLContext->NoteTextureUnitTouched(static_cast<Int>(unit));
        bindImageTexture(unit, texture, level, layered, layer, access, format);
    }

    // GL 4.6 core 8.14.4: a cube map that is not cube complete has no consistent set of faces to
    // filter down, so generating its mipmaps is INVALID_OPERATION. Without this the incomplete
    // texture reached the backend, where DirectVulkan asserts on it and takes the process down.
    Bool ValidateGenerateMipmapTexture(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                       const char* caller) {
        if (!textureObject) return false;
        const auto target = textureObject->GetTarget();
        if ((target == TextureTarget::TextureCubeMap || target == TextureTarget::TextureCubeMapArray) &&
            !textureObject->IsComplete()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Mipmap generation requires a cube complete cube map texture."));
            return false;
        }
        return true;
    }

    void GenerateMipmap(GLenum target) {
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) {
            return;
        }
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        auto& textureObject = activeUnit.GetBindingSlot(textureTarget).GetBoundObject();
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "GenerateMipmap requires a bound texture."));
            return;
        }
        if (!ValidateGenerateMipmapTexture(textureObject, __func__)) return;

        auto* mipmapTexture = dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        MOBILEGL_ASSERT(mipmapTexture != nullptr, "GenerateMipmap requires mipmap texture storage.");
        EnsureGeneratedMipmapStorageAllocated(*mipmapTexture);
        GenerateMipmap_Backend(target);
    }

    void GenerateTextureMipmap(GLuint texture) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!ValidateGenerateMipmapTexture(textureObject, __func__)) return;
        auto* mipmapTexture = dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        MOBILEGL_ASSERT(mipmapTexture != nullptr, "GenerateTextureMipmap requires mipmap texture storage.");
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum target) {
            EnsureGeneratedMipmapStorageAllocated(*mipmapTexture);
            GenerateMipmap_Backend(target);
        });
    }

    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
        if (!GetTexImage_State(target, level, format, type, pixels)) return;
        if (MG_Backend::gBackendFunctionsTable.GL.GetTexImage != nullptr) {
            GetTexImage_Backend(target, level, format, type, pixels);
            return;
        }
        TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        const auto& textureObject = activeUnit.GetBindingSlot(textureTarget).GetBoundObject();
        CopyTextureImageToClientOrPBO_State(textureObject, textureUploadTarget, level, format, type, -1, pixels,
                                            __func__);
    }

    void GetInternalformativ(GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint* params) {
        if (!params || bufSize <= 0) return;

        const TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        const Bool isRenderbufferTarget = target == GL_RENDERBUFFER;
        if (!isRenderbufferTarget && !TextureImpl::ValidateTextureTarget(textureTarget)) return;

        TextureInternalFormat textureInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        textureInternalFormat = MG_Util::ConvertInternalFormatToSized(textureInternalFormat, TextureInputFormat::RGBA,
                                                                      TexturePixelDataType::UnsignedByte);
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;

        GLenum preferredInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(textureInternalFormat);
        GLenum imageFormat = GL_RGBA;
        GLenum imageType = GL_UNSIGNED_BYTE;
        MG_Util::TextureFormatProcessor::NormalizePixelFormat(preferredInternalFormat, PixelFormatNormalizeOptionBit::None,
                                                              &preferredInternalFormat, &imageFormat, &imageType);

        auto writeValues = [&](std::initializer_list<GLint> values) {
            GLsizei index = 0;
            for (GLint value : values) {
                if (index >= bufSize) break;
                params[index++] = value;
            }
            while (index < bufSize) {
                params[index++] = 0;
            }
        };

        const Bool isDepthFormat = MG_Util::IsDepthFormatInternalFormat(textureInternalFormat);
        const Bool isStencilFormat = MG_Util::IsStencilFormatInternalFormat(textureInternalFormat);
        const ComponentSizes componentSizes = MG_Util::GetComponentSizesForInternalFormat(textureInternalFormat);
        const Bool isIntegerFormat = imageFormat == GL_RED_INTEGER || imageFormat == GL_RG_INTEGER ||
                                     imageFormat == GL_RGB_INTEGER || imageFormat == GL_RGBA_INTEGER;

        SizeT targetIndex = isRenderbufferTarget ? MG_Backend::GetRenderbufferFormatCapabilityTargetIndex()
                                                 : MG_Backend::GetFormatCapabilityTargetIndex(textureTarget);
        if (targetIndex >= MG_Backend::kFormatCapabilityTargetCount) return;
        const SizeT formatIndex = static_cast<SizeT>(textureInternalFormat);

        MG_Backend::FormatCapabilityFlags fullCaps{};
        MG_Backend::FormatCapabilityFlags caveatCaps{};
        const Vector<Int>* sampleCounts = nullptr;
        if (MG_Backend::pActiveBackendObject) {
            const auto& cache = MG_Backend::pActiveBackendObject->GetFormatCapabilities();
            fullCaps = cache.FullCaps[targetIndex][formatIndex];
            caveatCaps = cache.CaveatCaps[targetIndex][formatIndex];
            sampleCounts = &cache.SampleCounts[targetIndex][formatIndex];
        }

        auto hasFull = [&](MG_Backend::FormatCapability capability) {
            return MG_Backend::HasFormatCapability(fullCaps, capability);
        };
        auto hasCaveat = [&](MG_Backend::FormatCapability capability) {
            return MG_Backend::HasFormatCapability(caveatCaps, capability);
        };
        auto supportFor = [&](MG_Backend::FormatCapability capability) -> GLint {
            if (hasFull(capability)) return GL_FULL_SUPPORT;
            if (hasCaveat(capability)) return GL_CAVEAT_SUPPORT;
            return GL_NONE;
        };
        auto supportForWithFallback = [&](MG_Backend::FormatCapability primary,
                                          MG_Backend::FormatCapability fallback) -> GLint {
            if (hasFull(primary)) return GL_FULL_SUPPORT;
            if (hasCaveat(primary) || hasFull(fallback) || hasCaveat(fallback)) return GL_CAVEAT_SUPPORT;
            return GL_NONE;
        };
        switch (pname) {
        case GL_INTERNALFORMAT_SUPPORTED:
            writeValues({(hasFull(MG_Backend::FormatCapability::Creatable) ||
                          hasCaveat(MG_Backend::FormatCapability::Creatable))
                             ? GL_TRUE
                             : GL_FALSE});
            return;
        case GL_INTERNALFORMAT_PREFERRED:
            writeValues({static_cast<GLint>(preferredInternalFormat)});
            return;
        case GL_INTERNALFORMAT_RED_SIZE:
            writeValues({componentSizes.Red});
            return;
        case GL_INTERNALFORMAT_GREEN_SIZE:
            writeValues({componentSizes.Green});
            return;
        case GL_INTERNALFORMAT_BLUE_SIZE:
            writeValues({componentSizes.Blue});
            return;
        case GL_INTERNALFORMAT_ALPHA_SIZE:
            writeValues({componentSizes.Alpha});
            return;
        case GL_INTERNALFORMAT_DEPTH_SIZE:
            writeValues({componentSizes.Depth});
            return;
        case GL_INTERNALFORMAT_STENCIL_SIZE:
            writeValues({componentSizes.Stencil});
            return;
        case GL_INTERNALFORMAT_SHARED_SIZE:
            writeValues({textureInternalFormat == TextureInternalFormat::RGB9E5 ? 5 : 0});
            return;
        case GL_INTERNALFORMAT_RED_TYPE:
            writeValues({GetTextureComponentType(textureInternalFormat, componentSizes.Red, false, false)});
            return;
        case GL_INTERNALFORMAT_GREEN_TYPE:
            writeValues({GetTextureComponentType(textureInternalFormat, componentSizes.Green, false, false)});
            return;
        case GL_INTERNALFORMAT_BLUE_TYPE:
            writeValues({GetTextureComponentType(textureInternalFormat, componentSizes.Blue, false, false)});
            return;
        case GL_INTERNALFORMAT_ALPHA_TYPE:
            writeValues({GetTextureComponentType(textureInternalFormat, componentSizes.Alpha, false, false)});
            return;
        case GL_INTERNALFORMAT_DEPTH_TYPE:
            writeValues({GetTextureComponentType(textureInternalFormat, componentSizes.Depth, true, false)});
            return;
        case GL_INTERNALFORMAT_STENCIL_TYPE:
            writeValues({GetTextureComponentType(textureInternalFormat, componentSizes.Stencil, false, true)});
            return;
        case GL_TEXTURE_IMAGE_FORMAT:
            writeValues({static_cast<GLint>(imageFormat)});
            return;
        case GL_TEXTURE_IMAGE_TYPE:
            writeValues({static_cast<GLint>(imageType)});
            return;
        case GL_TEXTURE_COMPRESSED:
        case GL_TEXTURE_COMPRESSED_BLOCK_WIDTH:
        case GL_TEXTURE_COMPRESSED_BLOCK_HEIGHT:
        case GL_TEXTURE_COMPRESSED_BLOCK_SIZE:
            writeValues({0});
            return;
        case GL_COLOR_COMPONENTS:
            writeValues({(!isDepthFormat && !isStencilFormat) ? GL_TRUE : GL_FALSE});
            return;
        case GL_DEPTH_COMPONENTS:
            writeValues({isDepthFormat ? GL_TRUE : GL_FALSE});
            return;
        case GL_STENCIL_COMPONENTS:
            writeValues({isStencilFormat ? GL_TRUE : GL_FALSE});
            return;
        case GL_FRAMEBUFFER_RENDERABLE:
            writeValues({supportFor(MG_Backend::FormatCapability::FramebufferRenderable)});
            return;
        case GL_FRAMEBUFFER_RENDERABLE_LAYERED:
            writeValues({supportFor(MG_Backend::FormatCapability::FramebufferLayered)});
            return;
        case GL_FILTER:
            writeValues({supportForWithFallback(MG_Backend::FormatCapability::LinearFilter,
                                                MG_Backend::FormatCapability::Sampled)});
            return;
        case GL_MIPMAP:
            writeValues({supportForWithFallback(MG_Backend::FormatCapability::GenerateMipmap,
                                                MG_Backend::FormatCapability::Sampled)});
            return;
        case GL_TEXTURE_GATHER:
        case GL_TEXTURE_GATHER_SHADOW:
            writeValues({supportFor(MG_Backend::FormatCapability::TextureGather)});
            return;
        case GL_TEXTURE_SHADOW:
            writeValues({supportFor(MG_Backend::FormatCapability::TextureShadow)});
            return;
        case GL_NUM_SAMPLE_COUNTS:
            writeValues({sampleCounts != nullptr ? static_cast<GLint>(sampleCounts->size()) : 0});
            return;
        case GL_SAMPLES:
            if (sampleCounts != nullptr) {
                GLsizei index = 0;
                for (Int sampleCount : *sampleCounts) {
                    if (index >= bufSize) break;
                    params[index++] = sampleCount;
                }
                while (index < bufSize) {
                    params[index++] = 0;
                }
                return;
            }
            writeValues({});
            return;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname is not supported by GetInternalformativ."));
            return;
        }
    }

    void GetMultisamplefv(GLenum pname, GLuint index, GLfloat* val) {
        if (val == nullptr) return;
        if (pname != GL_SAMPLE_POSITION) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Only GL_SAMPLE_POSITION is supported."));
            return;
        }

        const Int maxSamples = MG_Backend::pActiveBackendObject != nullptr
            ? std::max(MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxSamples, 1)
            : 1;
        if (static_cast<Int>(index) >= maxSamples) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Sample index is out of range."));
            return;
        }

        // Keep sample positions deterministic even before the backend exposes vendor-specific patterns.
        val[0] = 0.5f;
        val[1] = 0.5f;
    }

    void TexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                       GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels) {
        TexSubImage3D_State(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
    }

    void TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                       GLenum format, GLenum type, const void* pixels) {
        TexSubImage2D_State(target, level, xoffset, yoffset, width, height, format, type, pixels);
    }

    void TexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                       const GLvoid* pixels) {
        TexSubImage1D_State(target, level, xoffset, width, format, type, pixels);
    }

    void TexParameterf(GLenum target, GLenum pname, GLfloat param) {
        TexParameterf_State(target, pname, param);
    }

    void TexParameteri(GLenum target, GLenum pname, GLint param) {
        TexParameteri_State(target, pname, param);
    }

    void TexParameterfv(GLenum target, GLenum pname, const GLfloat* params) {
        TexParameterfv_State(target, pname, params);
    }

    void TexParameteriv(GLenum target, GLenum pname, const GLint* params) {
        TexParameteriv_State(target, pname, params);
    }

    void TexParameterIiv(GLenum target, GLenum pname, const GLint* params) {
        TexParameterIiv_State(target, pname, params);
    }

    void TexParameterIuiv(GLenum target, GLenum pname, const GLuint* params) {
        TexParameterIuiv_State(target, pname, params);
    }

    void TexImage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height,
                               GLsizei depth, GLboolean fixedsamplelocations) {
        TexImage3DMultisample_State(target, samples, internalformat, width, height, depth, fixedsamplelocations);
    }

    void TexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height,
                               GLboolean fixedsamplelocations) {
        TexImage2DMultisample_State(target, samples, internalformat, width, height, fixedsamplelocations);
    }

    void TexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth,
                    GLint border, GLenum format, GLenum type, const void* pixels) {
        TexImage3D_State(target, level, internalformat, width, height, depth, border, format, type, pixels);
    }

    void TexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                    GLenum format, GLenum type, const void* pixels) {
        TexImage2D_State(target, level, internalformat, width, height, border, format, type, pixels);
    }

    void TexImage1D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLint border, GLenum format,
                    GLenum type, const GLvoid* pixels) {
        TexImage1D_State(target, level, internalFormat, width, border, format, type, pixels);
    }

    void TexBuffer(GLenum target, GLenum internalformat, GLuint buffer) {
        TexBuffer_State(target, internalformat, buffer);
    }

    // The buffer texture bound to `target` on the active unit - what the non-DSA range form
    // operates on. Kept separate from TexBuffer_State because that one resolves the target
    // itself and attaches the whole buffer.
    static const SharedPtr<MG_State::GLState::ITextureObject>& GetBoundBufferTexture(GLenum target,
                                                                                     const char* caller) {
        TextureUploadTarget uploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        if (!TextureImpl::ValidateTextureUploadTarget(uploadTarget)) return nullTextureObject;
        (void)caller;
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        return activeUnit.GetBindingSlot(MG_Util::ConvertGLEnumToTextureTarget(target)).GetBoundObject();
    }

    void TexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size) {
        AttachBufferToTexture(GetBoundBufferTexture(target, __func__), internalformat, buffer, offset,
                              static_cast<SizeT>(size < 0 ? 0 : size), __func__);
    }

    void TextureBuffer(GLuint texture, GLenum internalformat, GLuint buffer) {
        AttachBufferToTexture(GetTextureObjectByName(texture, __func__), internalformat, buffer, 0,
                              MG_State::GLState::TextureObjectBuffer::kWholeBuffer, __func__);
    }

    void TextureBufferRange(GLuint texture, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size) {
        AttachBufferToTexture(GetTextureObjectByName(texture, __func__), internalformat, buffer, offset,
                              static_cast<SizeT>(size < 0 ? 0 : size), __func__);
    }

    GLboolean IsTexture(GLuint texture) {
        return IsTexture_State(texture);
    }

    void GetTexParameterIuiv(GLenum target, GLenum pname, GLuint* params) {
        GetTexParameterIuiv_State(target, pname, params);
    }

    void GetTexParameterIiv(GLenum target, GLenum pname, GLint* params) {
        GetTexParameterIiv_State(target, pname, params);
    }

    void GetTexParameteriv(GLenum target, GLenum pname, GLint* params) {
        GetTexParameteriv_State(target, pname, params);
    }

    void GetTexParameterfv(GLenum target, GLenum pname, GLfloat* params) {
        GetTexParameterfv_State(target, pname, params);
    }

    void GetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params) {
        GetTexLevelParameteriv_State(target, level, pname, params);
    }

    void GetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat* params) {
        GetTexLevelParameterfv_State(target, level, pname, params);
    }

    void GetCompressedTexImage(GLenum target, GLint level, void* img) {
        GetCompressedTexImage_State(target, level, img);
    }

    void GenTextures(GLsizei n, GLuint* textures) {
        GenTextures_State(n, textures);
    }

    void DeleteTextures(GLsizei n, const GLuint* textures) {
        DeleteTextures_State(n, textures);
    }

    void CopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y,
                           GLsizei width, GLsizei height) {
        CopyTexSubImage3D_State(target, level, xoffset, yoffset, zoffset, x, y, width, height);
    }

    void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width,
                           GLsizei height) {
        CopyTexSubImage2D_Backend(target, level, xoffset, yoffset, x, y, width, height);
    }

    void CopyTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                               GLsizei width, GLsizei height) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        // GL 4.6 sec. 8.8: the 2D form only accepts these effective targets; cube maps must
        // go through CopyTextureSubImage3D with the face as a layer.
        const auto target = textureObject->GetTarget();
        if (target != TextureTarget::Texture2D && target != TextureTarget::Texture1DArray &&
            target != TextureTarget::TextureRectangle) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "CopyTextureSubImage2D requires a 2D, 1D-array, or "
                                             "rectangle texture."));
            return;
        }
        if (!ValidateCopyTextureSubImage(textureObject, level, xoffset, yoffset, 0, width, height, 1, __func__)) {
            return;
        }
        WithTemporarilyBoundNamedTexture(textureObject, [&](GLenum glTarget) {
            CopyTexSubImage2D_Backend(glTarget, level, xoffset, yoffset, x, y, width, height);
        });
    }

    void CopyTextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        if (textureObject->GetTarget() != TextureTarget::Texture1D) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "CopyTextureSubImage1D requires a 1D texture."));
            return;
        }
        if (!ValidateCopyTextureSubImage(textureObject, level, xoffset, 0, 0, width, 1, 1, __func__)) return;
        CopyReadFramebufferIntoMipmapRegion(textureObject, GetPrimaryUploadTarget(textureObject), level, xoffset,
                                            /*yoffset=*/0, /*zoffset=*/0, x, y, width, /*height=*/1, __func__);
    }

    void CopyTextureSubImage3D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x,
                               GLint y, GLsizei width, GLsizei height) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        // GL 4.6 core 8.6: the 3D form takes the layered targets, a cube map included - the face
        // is selected by zoffset.
        const auto target = textureObject->GetTarget();
        if (target != TextureTarget::Texture3D && target != TextureTarget::Texture2DArray &&
            target != TextureTarget::TextureCubeMap && target != TextureTarget::TextureCubeMapArray) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "CopyTextureSubImage3D requires a 3D, 2D-array, cube map, or "
                                             "cube map array texture."));
            return;
        }
        if (!ValidateCopyTextureSubImage(textureObject, level, xoffset, yoffset, zoffset, width, height, 1, __func__)) {
            return;
        }
        // A cube map addresses its faces as separate upload targets, so zoffset selects the target
        // rather than a slice within one; every other layered target keeps zoffset as the slice.
        TextureUploadTarget uploadTarget = GetPrimaryUploadTarget(textureObject);
        GLint sliceOffset = zoffset;
        if (target == TextureTarget::TextureCubeMap) {
            uploadTarget = static_cast<TextureUploadTarget>(
                static_cast<SizeT>(TextureUploadTarget::CubeMapPositiveX) + static_cast<SizeT>(zoffset));
            sliceOffset = 0;
        }
        CopyReadFramebufferIntoMipmapRegion(textureObject, uploadTarget, level, xoffset, yoffset, sliceOffset, x, y,
                                            width, height, __func__);
    }

    void CopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width) {
        CopyTexSubImage1D_State(target, level, xoffset, x, y, width);
    }

    void CopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLsizei height, GLint border) {
        if (!CopyTexImage2D_State(target, level, internalformat, x, y, width, height, border)) return;
        CopyTexImage2D_Backend(target, level, internalformat, x, y, width, height, border);
    }

    void CopyTexImage1D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLint border) {
        CopyTexImage1D_State(target, level, internalformat, x, y, width, border);
    }

    void CopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                          GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        // A missing name is INVALID_VALUE here, where GetTextureObjectByName's own diagnostic is
        // INVALID_OPERATION - so resolve through the plain lookups, which answer a null
        // SharedPtr, and let the validator record the error this entry point owes.
        //
        // The TARGET picks the namespace: GL 4.6 core 18.3.2 accepts GL_RENDERBUFFER, and a
        // renderbuffer name has nothing to do with a texture name. Resolving both through
        // GetTextureObject made every renderbuffer endpoint INVALID_VALUE - or, when the number
        // happened to collide with a live texture, INVALID_ENUM from the target check.
        const auto resolveEndpoint = [](GLuint name, GLenum target) {
            MG_Backend::CopyImageEndpoint endpoint{};
            if (target == GL_RENDERBUFFER) {
                endpoint.Renderbuffer = MG_State::pGLContext->GetRenderbufferObject(name);
            } else {
                endpoint.Texture = MG_State::pGLContext->GetTextureObject(name);
            }
            return endpoint;
        };
        const MG_Backend::CopyImageEndpoint src = resolveEndpoint(srcName, srcTarget);
        const MG_Backend::CopyImageEndpoint dst = resolveEndpoint(dstName, dstTarget);
        if (!ValidateCopyImageSubData_State(src, srcTarget, srcLevel, srcX, srcY, dst, dstTarget,
                                            dstLevel, dstX, dstY, srcWidth, srcHeight, srcDepth)) {
            return;
        }
        CopyImageSubData_Backend(src, srcTarget, srcLevel, srcX, srcY, srcZ, dst, dstTarget, dstLevel,
                                 dstX, dstY, dstZ, srcWidth, srcHeight, srcDepth);
    }

    void CompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                                 GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data) {
        CompressedTexSubImage3D_State(target, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize,
                                      data);
    }

    void CompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                 GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
        CompressedTexSubImage2D_State(target, level, xoffset, yoffset, width, height, format, imageSize, data);
    }

    void CompressedTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format,
                                 GLsizei imageSize, const void* data) {
        CompressedTexSubImage1D_State(target, level, xoffset, width, format, imageSize, data);
    }

    void CompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                              GLsizei depth, GLint border, GLsizei imageSize, const void* data) {
        CompressedTexImage3D_State(target, level, internalformat, width, height, depth, border, imageSize, data);
    }

    void CompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                              GLint border, GLsizei imageSize, const void* data) {
        CompressedTexImage2D_State(target, level, internalformat, width, height, border, imageSize, data);
    }

    void CompressedTexImage1D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border,
                              GLsizei imageSize, const void* data) {
        CompressedTexImage1D_State(target, level, internalformat, width, border, imageSize, data);
    }

    void BindTexture(GLenum target, GLuint texture) {
        BindTexture_State(target, texture);
    }

    void ActiveTexture(GLenum texture) {
        ActiveTexture_State(texture);
    }

} // namespace MobileGL::MG_Impl::GLImpl
