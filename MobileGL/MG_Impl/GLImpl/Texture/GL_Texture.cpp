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
#include "MipmapGenerationPlan.h"

#include <MG_State/GLState/Core.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_Util/Metrics/TextureMetrics.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Util/Texture/PixelStoreProcessor.h>
#include <MG_Util/Texture/TextureFormatProcessor.h>
#include <MG_Util/Texture/EtcDecoder.h>
#include <MG_Util/Classifiers/TextureEnumClassifier.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_Impl/GLImpl/Framebuffer/Validators.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/Sampler/Validators.h>
#include <MG_Util/Math/FixedPointConversion.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_Impl/Pipe/PipeFill.h>
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/EGLImpl/EGLImpl.h>
#include <algorithm>
#include <initializer_list>
// CONTRACT-P5.md §7 / ID-14: a null check on a GLFunctionsTable slot may not survive into the
// client under split - it becomes a caps-mirror read. SlotCaps.h carries the rule and the test
// that decides which of its two spellings a site takes; in a pull build both expand to exactly
// the check they replaced.
#include <MG_Remote/Client/SlotCaps.h>
// P4a, ID-18 M2. The ONE door MG_State and MG_Impl have into the client's emitters; the three
// call sites below are declarations only, exactly as the frontend's mutators are.
#include <MG_Pipe/PipeMutation.h>

// P9 (W1 §1): AN UNPACK SOURCE IS READ FROM THE BUFFER'S CLIENT SHADOW,
// and under a transport a GPU write into that buffer - a read_pixels_to_buffer, which is how a
// glReadPixels into a pack buffer now crosses, or a shader store - reaches the shadow only through
// SyncGpuWrites, exactly as glMapBufferRange and glGetBufferSubData already do. Monolith keeps its
// read as it was: its ReadPixels maps the driver PBO back into the shadow inside the call, and the
// pull build compiles this to nothing (G1).
#define MGL_SYNC_PIXEL_UNPACK_SOURCE(buffer)                                                                           \
    do {                                                                                                               \
        (buffer)->SyncGpuWrites();                                                                                     \
    } while (0)

namespace MobileGL::MG_Impl::GLImpl {
    static SharedPtr<MG_State::GLState::ITextureObject> nullTextureObject;

    namespace {
        void SetTextureBorderColorFromFloats(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                             const GLfloat* params) {
            textureObject->SetBorderColor(FloatVec4(params[0], params[1], params[2], params[3]));
        }

        // glTexParameteriv(GL_TEXTURE_BORDER_COLOR): GL 4.6 core 8.10 sends the components through
        // equation 2.2 into the floating-point border colour. glGetTexParameteriv reverses it with
        // equation 2.3; the two live in one header so they cannot drift apart.
        void SetTextureBorderColorFromInts(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                           const GLint* params) {
            textureObject->SetBorderColor(FloatVec4(
                MG_Util::SignedNormalizedInt32ToFloat(params[0]), MG_Util::SignedNormalizedInt32ToFloat(params[1]),
                MG_Util::SignedNormalizedInt32ToFloat(params[2]), MG_Util::SignedNormalizedInt32ToFloat(params[3])));
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

        // Which spelling of a call is running. The bound-target and by-name forms share every core
        // below and differ in how they report a target the call does not accept: INVALID_ENUM for
        // a target token, INVALID_OPERATION for a named texture's effective target (GL 4.6 core
        // 8.6, 8.10, 8.11, 8.14.4).
        enum class TextureCallForm {
            Bound,
            Named
        };

        ErrorCode TargetErrorFor(TextureCallForm form) {
            return form == TextureCallForm::Named ? ErrorCode::InvalidOperation : ErrorCode::InvalidEnum;
        }

        void RecordTargetError(TextureCallForm form, const char* caller, const char* message) {
            MG_State::pGLContext->RecordError(TargetErrorFor(form),
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, message));
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

        // How many LAYERS a texture of this target has, given its base level's state-side extent.
        // GL keeps a 1D array's layer count in the height and every other layered target's in the
        // depth; a cube map has exactly six and a 3D texture has one (its depth is spatial).
        Uint LayerCountOfImmutableTexture(TextureTarget target, const IntVec3& baseSize) {
            switch (target) {
            case TextureTarget::Texture1DArray:
                return static_cast<Uint>(std::max(baseSize.y(), 1));
            case TextureTarget::Texture2DArray:
            case TextureTarget::TextureCubeMapArray:
            case TextureTarget::Texture2DMultisampleArray:
                return static_cast<Uint>(std::max(baseSize.z(), 1));
            case TextureTarget::TextureCubeMap:
                return 6;
            default:
                return 1;
            }
        }

        // GL 4.6 core 8.19: TexStorage* leaves the texture describing itself as a full-extent view
        // of its own storage - TEXTURE_VIEW_MIN_LEVEL 0, TEXTURE_VIEW_NUM_LEVELS <levels>,
        // TEXTURE_VIEW_MIN_LAYER 0, TEXTURE_VIEW_NUM_LAYERS the layer count. That is not just a
        // query detail: glTextureView COMPOSES onto these ("<numlevels> and the value of
        // TEXTURE_VIEW_NUM_LEVELS from the original texture minus <minlevel>", 8.18), so leaving
        // them at the mutable-texture default of 0 would clamp every view to zero levels.
        void SeedImmutableViewState(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, Uint levels) {
            if (!textureObject) return;
            textureObject->SetViewLevelLayerRange(
                0, levels, 0, LayerCountOfImmutableTexture(textureObject->GetTarget(), textureObject->GetBaseSize()));
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
            case GL_TEXTURE_SHARED_SIZE:
                // GL 4.6 core table 8.24: the size in bits of the SHARED EXPONENT, which only the
                // one shared-exponent format has. Everything else answers zero, and the
                // conformance suite compares "at least", not "equal".
                return textureInternalFormat == TextureInternalFormat::RGB9E5 ? 5 : 0;
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
            return static_cast<GLuint>(
                std::min<GLint>(MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxImageUnits,
                                MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS));
        }

        // How many components of a GL-space texel size actually halve down the mip chain.
        //
        // An array texture's LAYER COUNT is not a dimension of the image (GL 4.6 core 8.14.3): it
        // stays put all the way down, and it is stored in whichever component sits after the
        // image's own dimensions - z for a 2D array or a cube array, and HEIGHT for a 1D array,
        // whose level is recorded as {width, layers, 1}.
        //
        // THE one statement of that rule on the frontend side, because three readers have to agree
        // on it or a chain is allocated under one and judged under another: this allocator,
        // ComputeMipmapCompleteForFilter (MG_State/GLState/TextureState/TextureObject.cpp, which
        // uses the identical 1/2/3 split) and DirectVulkan's MipShrinkingComponentCount. It used to
        // be a two-way `depthMips` flag, which had no way to say "height is not a dimension" - so
        // glGenerateMipmap on a GL_TEXTURE_1D_ARRAY allocated a chain whose LAYER COUNT halved,
        // and the completeness rule then rejected the texture the generate was supposed to make
        // complete. The backend allocator could not repair it either: it only ever GROWS a chain,
        // and the frontend's (wrong) count is always the longer of the two.
        Int MipShrinkingAxisCount(TextureTarget target) {
            switch (target) {
            case TextureTarget::Texture1D:
                // {width, 1, 1} - the other two are already 1, but say so rather than rely on it.
                return 1;
            case TextureTarget::Texture1DArray:
                // {width, layers, 1}: height IS the layer count.
                return 1;
            case TextureTarget::Texture2DArray:
            case TextureTarget::TextureCubeMapArray:
                // {width, height, layers}: depth IS the layer count.
                return 2;
            case TextureTarget::Texture3D:
                return 3;
            default:
                // 2D, cube faces, rectangle, multisample: a plain two-dimensional image.
                return 2;
            }
        }

        // Only true 3D textures halve their depth per level; every array target keeps its layer
        // count. Expressed through the rule above so the two cannot drift.
        Bool DepthParticipatesInMipmapping(TextureTarget target) {
            return MipShrinkingAxisCount(target) == 3;
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

        // The longest mip chain the level-0 size admits, over the axes that actually reduce.
        Uint ComputeFullMipmapLevelCount(const IntVec3& baseTexelSize, Int shrinkingAxes);

        Uint MaxTextureStorageLevels(TextureTarget target, GLsizei width, GLsizei height, GLsizei depth) {
            return ComputeFullMipmapLevelCount(
                {std::max<Int>(width, 1), std::max<Int>(height, 1), std::max<Int>(depth, 1)},
                MipShrinkingAxisCount(target));
        }

        Uint ComputeFullMipmapLevelCount(const IntVec3& baseTexelSize, Int shrinkingAxes) {
            Int maxDimension = 1;
            for (Int axis = 0; axis < shrinkingAxes && axis < 3; ++axis) {
                maxDimension = std::max<Int>(maxDimension, baseTexelSize[axis]);
            }
            Uint mipLevelCount = 1;
            while (maxDimension > 1) {
                maxDimension = std::max<Int>(maxDimension / 2, 1);
                ++mipLevelCount;
            }
            return mipLevelCount;
        }

        IntVec3 ComputeMipmapTexelSize(const IntVec3& baseTexelSize, Uint relativeLevel, Int shrinkingAxes) {
            IntVec3 size = {std::max<Int>(baseTexelSize.x(), 1), std::max<Int>(baseTexelSize.y(), 1),
                            std::max<Int>(baseTexelSize.z(), 1)};
            for (Int axis = 0; axis < shrinkingAxes && axis < 3; ++axis) {
                size[axis] = std::max<Int>(size[axis] >> static_cast<Int>(relativeLevel), 1);
            }
            return size;
        }

        // glGenerateMipmap defines levels BASE_LEVEL+1 up to the level the base image's extent and
        // MAX_LEVEL admit, and leaves every other level exactly as it was (GL 4.6 core 8.17). Both
        // transports take this one path: the window, and the refusal to redefine the levels around
        // it, are properties of the GL call rather than of how the backend is reached.
        Bool EnsureGeneratedMipmapStorageAllocated(MG_State::GLState::TextureObjectMipmap& texture,
                                                   TextureUploadTarget uploadTarget) {
            const auto plan = ComputeMipmapGenerationRange(texture, uploadTarget);
            if (plan.End <= plan.Base) return false;
            // Immutable textures and views already own every level generation can write.
            // Allocating through a view would redefine its owner's levels at the VIEW's base
            // extent - one slice of the owner's layers - destroying the storage of every layer and
            // level outside the view window.
            if (texture.IsImmutable()) return true;
            const IntVec3 baseSize = texture.GetMipmapTexelSize(uploadTarget, plan.Base);
            const SizeT baseBytes = texture.GetMipmapByteSize(uploadTarget, plan.Base);
            const SizeT texels = static_cast<SizeT>(baseSize.x()) * baseSize.y() * baseSize.z();
            if (!baseBytes || !texels || baseBytes % texels) return false;
            const SizeT pixelBytes = baseBytes / texels;
            const Int axes = MipShrinkingAxisCount(texture.GetTarget());
            for (Uint level = plan.Base + 1; level < plan.End; ++level) {
                const auto size = ComputeMipmapTexelSize(baseSize, level - plan.Base, axes);
                const SizeT bytes = pixelBytes * static_cast<SizeT>(size.x()) * size.y() * size.z();
                texture.AllocateStorage(uploadTarget, level, {size, bytes});
                texture.MarkStorageDirty(uploadTarget, level, false);
            }
            // Levels before BASE_LEVEL and after the generated end remain valid images and are left
            // alone: neither MAX_LEVEL nor a generate truncates the chain. AllocateStorage only
            // grows, so a longer chain a level-0 respecification left behind is dropped there
            // (DiscardMipmapChainOnBaseRespecification) rather than here.
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
            return target == TextureTarget::Texture2DMultisample || target == TextureTarget::Texture2DMultisampleArray;
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

        // The ceiling the frontend enforces, which is EXACTLY the one MobileGL advertises for
        // this format's category - GL_MAX_DEPTH_TEXTURE_SAMPLES, GL_MAX_INTEGER_SAMPLES or
        // GL_MAX_COLOR_TEXTURE_SAMPLES, all three of which have a GL 4.6 minimum of one and are
        // reported as probed. It used to floor all three at GL_MAX_SAMPLES (4) on the reasoning
        // that an application reads GL_MAX_SAMPLES once and hands that count to every
        // glTexStorage*Multisample. That reasoning had it backwards: on Adreno and on Mali an
        // integer multisample texture is backed by ONE sample, so accepting four here did not
        // make four samples exist - ClampSamplesToBackendSupport quietly allocated one and the
        // application wrote per-sample data it could never read back. Raising INVALID_OPERATION
        // is what a real driver does, and it is what makes that silent squeeze unreachable for
        // application-visible storage.
        Int GetMaxSupportedTextureSamples(TextureTarget textureTarget, TextureInternalFormat textureInternalFormat) {
            if (MG_Backend::pActiveBackendObject == nullptr) {
                return std::numeric_limits<Int>::max();
            }

            const Bool isDepthOrStencil = MG_Util::IsDepthFormatInternalFormat(textureInternalFormat) ||
                                          MG_Util::IsStencilFormatInternalFormat(textureInternalFormat);
            Bool isIntegerFormat = false;
            if (!isDepthOrStencil) {
                GLenum normalizedInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(textureInternalFormat);
                GLenum normalizedFormat = GL_RGBA;
                GLenum normalizedType = GL_UNSIGNED_BYTE;
                MG_Util::TextureFormatProcessor::NormalizePixelFormat(
                    normalizedInternalFormat, PixelFormatNormalizeOptionBit::None, &normalizedInternalFormat,
                    &normalizedFormat, &normalizedType);
                isIntegerFormat = normalizedFormat == GL_RED_INTEGER || normalizedFormat == GL_RG_INTEGER ||
                                  normalizedFormat == GL_RGB_INTEGER || normalizedFormat == GL_RGBA_INTEGER;
            }
            const Int categoryMaxSamples = isDepthOrStencil  ? GetAdvertisedDepthTextureMaxSamples()
                                           : isIntegerFormat ? GetAdvertisedIntegerMaxSamples()
                                                             : GetAdvertisedColorTextureMaxSamples();

            // glGetInternalformativ(GL_SAMPLES) is answered from this very list
            // (GetInternalformativ below), and GL 4.6 core 8.8 makes that query the definition of
            // the per-format maximum - so when the probe has an answer it IS the ceiling, and the
            // category limit only stands in where nothing was probed.
            //
            // This used to be max(probed, category), which made the probe dead: the walk starts
            // AT the category limit (BackendObject_DirectGLES's ProbeTextureSampleCounts) so its
            // head can never exceed it, and max() therefore always collapsed to the category
            // value. A format whose 4- and 2-sample probes fail inside a 4-sample category - a
            // float colour format under EXT_color_buffer_float is the natural instance - was
            // still accepted at 4, silently squeezed to 1 by ClampSamplesToBackendSupport, and
            // then reported as 4 by GL_TEXTURE_SAMPLES while glGetInternalformativ said 1.
            const Int probedMaxSamples = GetProbedMaxTextureSamples(textureTarget, textureInternalFormat);
            return probedMaxSamples > 0 ? probedMaxSamples : categoryMaxSamples;
        }

        Bool ValidateTextureMultisampleStorage(TextureTarget textureTarget, GLsizei samples, GLsizei width,
                                               GLsizei height, GLsizei depth,
                                               TextureInternalFormat textureInternalFormat, const char* caller) {
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
                    ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
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
            if ((width <= 0 || height <= 0 || depth <= 0) && !TextureImpl::IsProxyTextureTarget(textureUploadTarget)) {
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
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                                     "Compressed texture formats are not supported."));
        }

        // The ETC2/EAC arm of the compressed uploads: the blocks are decoded into the level's
        // uncompressed texel shadow (the storage the format resolves to), so the level samples and
        // reads back as the application's image on every backend. `blocks` covers the region
        // (offset, size) of the level. False - nothing written - for any other compressed format,
        // which still has no decoder here, or for storage not shaped as the decoder writes it.
        Bool DecodeEtcBlocksIntoTexelShadow(MG_State::GLState::TextureObjectMipmap* texture,
                                            TextureUploadTarget uploadTarget, Uint level, GLenum format,
                                            const void* blocks, IntVec3 offset, IntVec3 size) {
            if (blocks == nullptr || !MG_Util::EtcDecoder::IsEtcFormat(format)) return false;
            const SizeT texelBytes = MG_Util::EtcDecoder::DecodedTexelBytes(format);
            if (MG_Util::GetInternalBytesPerPixel(texture->GetFormat(), TexturePixelDataType::UnsignedByte) !=
                texelBytes) {
                return false;
            }
            const IntVec3 levelSize = texture->GetMipmapTexelSize(uploadTarget, level);
            const Int levelDepth = std::max(levelSize.z(), 1);
            const SizeT levelBytes = static_cast<SizeT>(levelSize.x()) * static_cast<SizeT>(levelSize.y()) *
                                     static_cast<SizeT>(levelDepth) * texelBytes;
            if (levelBytes == 0 || texture->GetMipmapByteSize(uploadTarget, level) < levelBytes) return false;
            auto* shadow = static_cast<Uint8*>(texture->MapMipmapData(uploadTarget, level));
            if (shadow == nullptr) return false;
            MG_Util::EtcDecoder::DecodeRegion(format, static_cast<const Uint8*>(blocks), size.x(), size.y(), size.z(),
                                              shadow, levelSize.x(), levelSize.y(), levelDepth, offset.x(), offset.y(),
                                              offset.z());
            texture->MarkStorageDirtyRegion(uploadTarget, level, offset, size);
            return true;
        }

        // GL_TEXTURE_WIDTH of a buffer texture: how many texels of the texture's internal format fit
        // in the buffer range it addresses, CLAMPED to GL_MAX_TEXTURE_BUFFER_SIZE. Attaching a larger
        // buffer is legal (GL 4.6 core 8.9) - the texture simply addresses the first
        // MAX_TEXTURE_BUFFER_SIZE texels of it, and that clamped count is what WIDTH reports.
        //
        // GL_TEXTURE_BUFFER_SIZE is deliberately NOT clamped the same way: it reports the range in
        // basic machine units exactly as glTexBuffer/glTexBufferRange were given it. Swapping the two
        // fails KHR-GL43.texture_buffer.texture_buffer_max_size in the opposite direction.
        GLint GetBufferTextureTexelWidth(const MG_State::GLState::ITextureObject* textureObject) {
            const SizeT texelByteSize = MG_Util::GetSizedInternalFormatSizeInBytes(textureObject->GetFormat());
            // A format with no known footprint has no texel count to report; answering 0 beats
            // dividing by it.
            if (texelByteSize == 0) return 0;
            const auto* bufferTextureObject = static_cast<const MG_State::GLState::TextureObjectBuffer*>(textureObject);
            const SizeT texelCount = bufferTextureObject->GetBufferRangeSizeInBytes() / texelByteSize;
            const SizeT maxTexelCount = static_cast<SizeT>(
                std::max(0, MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxTextureBufferSize));
            return static_cast<GLint>(std::min(texelCount, maxTexelCount));
        }

        // GL_TEXTURE_BUFFER_DATA_STORE_BINDING: the name of the buffer a buffer texture is attached
        // to, or 0 when none is - and 0, the state's initial value, for any other storage.
        GLuint GetBufferTextureDataStoreBinding(MG_State::GLState::ITextureObject* textureObject) {
            if (textureObject->GetStorageType() != TextureStorageType::Buffer) return 0u;
            auto* bufferTextureObject = static_cast<MG_State::GLState::TextureObjectBuffer*>(textureObject);
            const auto& buffer = bufferTextureObject->GetBufferBindingSlot().GetBoundObject();
            return buffer != nullptr ? buffer->GetExternalIndex() : 0u;
        }

        // glGetTexLevelParameter{i,f}v answers WIDTH/HEIGHT/DEPTH out of the mipmap chain, and (since
        // the buffer-texture arms above) out of the attached buffer range for GL_TEXTURE_BUFFER. This
        // is what is left: a storage class with no level geometry at all. Report it instead of
        // throwing - THROW_UNIMPL_EXCEPTION unwinds a C++ exception through the C GL ABI and takes the
        // process down, which is never an acceptable answer to a query - see the same reasoning above
        // for the compressed-format path.
        void RecordUnsupportedLevelQueryStorage(const char* caller, GLenum pname) {
            MGLOG_W_ONCE("%s: glGetTexLevelParameter(pname=%s) is not implemented for this texture's "
                         "storage class; recording GL_INVALID_OPERATION instead of terminating",
                         caller, MG_Util::ConvertGLEnumToString(pname).c_str());
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Level queries are not supported for this texture's storage class."));
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

    // Whether a raw internalformat enum names a compressed format - the question GL asks whenever an
    // entry point is forbidden on a compressed image: glTexStorage3D on TEXTURE_3D (no
    // block-compressed format is defined for a three-dimensional image, so it is INVALID_OPERATION
    // rather than the INVALID_ENUM an unknown sized format gets - GL 4.6 core 8.19 / Khronos bug
    // 11239, KHR-GLxx.texture_storage.compressed_data) and the clear-texture pair (8.19 again).
    // Written against the enum ranges rather than a name list because the families are contiguous
    // and MobileGL's own internal-format enum drops the ones it cannot carry, which would make this
    // check silently narrower than the API surface.
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

    namespace {
        void RecordClearTextureError(const char* caller, ErrorCode code, const String& message) {
            MG_State::pGLContext->RecordError(code, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, message));
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
            // GL 4.6 core 8.19: a compressed internal format is INVALID_OPERATION for both clear
            // entry points. Two tags to ask, because they answer different questions: the stored
            // one covers a level glCompressedTexImage* or a SPECIFIC compressed internalformat
            // defined, the requested one covers the six generic GL_COMPRESSED_* enums that MobileGL
            // deliberately backs with uncompressed storage (see MipmapStorage) and that would
            // otherwise look like an ordinary RGBA8 image by the time the clear runs.
            const auto& uploadTargets = mipmapTexture->GetUploadTargets();
            if (!uploadTargets.empty() &&
                (mipmapTexture->GetMipmapCompressedFormat(uploadTargets[0], static_cast<Uint>(level)) != GL_NONE ||
                 mipmapTexture->GetMipmapRequestedCompressedFormat(uploadTargets[0], static_cast<Uint>(level)) !=
                     GL_NONE)) {
                RecordClearTextureError(caller, ErrorCode::InvalidOperation, "Compressed textures cannot be cleared.");
                return nullptr;
            }
            return mipmapTexture;
        }

        Bool BuildClearPixel(const SharedPtr<MG_State::GLState::TextureObjectMipmap>& textureObject, GLenum format,
                             GLenum type, const void* data, Vector<Uint8>& clearPixel) {
            const TextureInputFormat inputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
            const TexturePixelDataType inputType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
            if (!TextureImpl::ValidateTextureInputFormat(inputFormat) ||
                !TextureImpl::ValidateTexturePixelDataType(inputType) ||
                !TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(inputFormat, textureObject->GetFormat(),
                                                                               inputType)) {
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
                data, clearPixelStore, textureObject->GetFormat(), inputFormat, inputType, {1, 1, 1}, false,
                clearPixelSize);
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
                               TextureUploadTarget uploadTarget, GLint level, GLint xoffset, GLint yoffset,
                               GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                               const Vector<Uint8>& clearPixel, const char* caller) {
            const IntVec3 texelSize = textureObject->GetMipmapTexelSize(uploadTarget, static_cast<Uint>(level));
            if (texelSize.x() <= 0 || texelSize.y() <= 0 || texelSize.z() <= 0) {
                RecordClearTextureError(caller, ErrorCode::InvalidOperation,
                                        "The requested texture level has no storage.");
                return false;
            }
            if (xoffset < 0 || yoffset < 0 || zoffset < 0 || width < 0 || height < 0 || depth < 0 ||
                width > texelSize.x() - xoffset || height > texelSize.y() - yoffset ||
                depth > texelSize.z() - zoffset) {
                RecordClearTextureError(caller, ErrorCode::InvalidValue,
                                        "The clear region lies outside the requested texture level.");
                return false;
            }
            if (width == 0 || height == 0 || depth == 0) return true;

            const SizeT texelCount = static_cast<SizeT>(texelSize.x()) * static_cast<SizeT>(texelSize.y()) *
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

            auto* destination =
                static_cast<Uint8*>(textureObject->MapMipmapData(uploadTarget, static_cast<Uint>(level)));
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
                    Uint8* row = destination + static_cast<SizeT>(zoffset + z) * fullSliceBytes +
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
                PixelStoreParam::PackAlignment, PixelStoreParam::PackRowLength,  PixelStoreParam::PackImageHeight,
                PixelStoreParam::PackSkipRows,  PixelStoreParam::PackSkipPixels, PixelStoreParam::PackSkipImages,
                PixelStoreParam::PackSwapBytes, PixelStoreParam::PackLSBFirst};
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
            const SizeT readBytesPerTexel = MG_Util::GetInputBytesPerPixel(
                MG_Util::ConvertGLEnumToTextureInputFormat(format), MG_Util::ConvertGLEnumToTexturePixelDataType(type));
            if (readBytesPerTexel != bytesPerTexel) {
                MGLOG_W_ONCE("%s: cannot copy into a %zu-byte texel from a %zu-byte readback layout", caller,
                             bytesPerTexel, readBytesPerTexel);
                return false;
            }

            Vector<Uint8> scratch(static_cast<SizeT>(width) * static_cast<SizeT>(height) * bytesPerTexel);
            {
                ScopedNeutralPackState neutralPack;
                MGP_FILL(ReadPixels);
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
            if (!ClearMipmapRegion(textureObject, uploadTarget, level, 0, 0, 0, size.x(), size.y(), size.z(),
                                   clearPixel, __func__)) {
                return;
            }
        }
    }

    void ClearTexSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                          GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* data) {
        auto textureObject = GetClearTextureObject(texture, level, __func__);
        if (!textureObject) return;

        Vector<Uint8> clearPixel;
        if (!BuildClearPixel(textureObject, format, type, data, clearPixel)) return;

        const auto& uploadTargets = textureObject->GetUploadTargets();
        if (textureObject->GetTarget() == TextureTarget::TextureCubeMap) {
            if (zoffset < 0 || depth < 0 || static_cast<SizeT>(zoffset) > uploadTargets.size() ||
                static_cast<SizeT>(depth) > uploadTargets.size() - static_cast<SizeT>(zoffset)) {
                RecordClearTextureError(__func__, ErrorCode::InvalidValue,
                                        "The cube-map clear region selects invalid faces.");
                return;
            }
            for (GLsizei face = 0; face < depth; ++face) {
                if (!ClearMipmapRegion(textureObject, uploadTargets[static_cast<SizeT>(zoffset + face)], level, xoffset,
                                       yoffset, 0, width, height, 1, clearPixel, __func__)) {
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
        ClearMipmapRegion(textureObject, uploadTargets.front(), level, xoffset, yoffset, zoffset, width, height, depth,
                          clearPixel, __func__);
    }

    static Bool ValidateTextureParameterForTarget(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                  GLenum pname, GLint param, TextureCallForm form, const char* caller) {
        const auto target = textureObject->GetTarget();
        if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT && !ValidateMaxAnisotropy(param, caller)) {
            return false;
        }
        if ((pname == GL_TEXTURE_BASE_LEVEL || pname == GL_TEXTURE_MAX_LEVEL) && param < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                                      "Texture level parameter must be non-negative."));
            return false;
        }

        if ((target == TextureTarget::Texture2DMultisample || target == TextureTarget::Texture2DMultisampleArray) &&
            pname == GL_TEXTURE_BASE_LEVEL && param != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Multisample texture base level must be zero."));
            return false;
        }

        // GL_OES_EGL_image_external: base level 0, CLAMP_TO_EDGE only, NEAREST or LINEAR only.
        if (target == TextureTarget::External) {
            if (pname == GL_TEXTURE_BASE_LEVEL && param != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "External texture base level must be zero."));
                return false;
            }
            if (((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == GL_TEXTURE_WRAP_R) &&
                 param != GL_CLAMP_TO_EDGE) ||
                (pname == GL_TEXTURE_MIN_FILTER && param != GL_NEAREST && param != GL_LINEAR)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "External textures clamp to edge and filter without mipmaps."));
                return false;
            }
        }

        if (target == TextureTarget::TextureRectangle && pname == GL_TEXTURE_BASE_LEVEL && param != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Rectangle texture base level must be zero."));
            return false;
        }

        if ((target == TextureTarget::Texture2DMultisample || target == TextureTarget::Texture2DMultisampleArray) &&
            (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == GL_TEXTURE_WRAP_R ||
             pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER || pname == GL_TEXTURE_MIN_LOD ||
             pname == GL_TEXTURE_MAX_LOD || pname == GL_TEXTURE_LOD_BIAS || pname == GL_TEXTURE_COMPARE_MODE ||
             pname == GL_TEXTURE_COMPARE_FUNC || pname == GL_TEXTURE_BORDER_COLOR ||
             pname == GL_TEXTURE_MAX_ANISOTROPY_EXT)) {
            // GL 4.6 core 8.10: through glTexParameter* a multisample target simply does not ACCEPT
            // these pnames (INVALID_ENUM); by name the texture is what is wrong (INVALID_OPERATION).
            RecordTargetError(form, caller, "Multisample textures do not accept sampler-state pnames.");
            return false;
        }

        // The six pnames a texture object shares with a sampler object carry an enum VALUE, and an
        // unrecognised one is INVALID_ENUM. The texture path used to hand the value straight to
        // ConvertGLEnumToSamplerWrapMode / ...FilterMode and throw the Unknown away, so
        // glTexParameteri(GL_TEXTURE_WRAP_S, GL_RED) was silently accepted. Sampler objects have had
        // exactly this validator all along; calling it here rather than writing a second one is also
        // what keeps the two spellings of the same state from drifting.
        //
        // Called selectively: ValidateSamplerParam's default arm reports InvalidEnum for anything it
        // does not know, and the texture-only pnames (BASE_LEVEL, SWIZZLE_*, ...) are not in its list.
        switch (pname) {
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
        case GL_TEXTURE_WRAP_R:
        case GL_TEXTURE_MIN_FILTER:
        case GL_TEXTURE_MAG_FILTER:
        case GL_TEXTURE_COMPARE_MODE:
        case GL_TEXTURE_COMPARE_FUNC:
            if (!SamplerImpl::ValidateSamplerParam(pname, static_cast<GLenum>(param))) return false;
            break;
        default:
            break;
        }

        if (target == TextureTarget::TextureRectangle) {
            if ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) &&
                (param == GL_MIRROR_CLAMP_TO_EDGE || param == GL_MIRRORED_REPEAT || param == GL_REPEAT)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Invalid wrap mode for rectangle texture."));
                return false;
            }
            if (pname == GL_TEXTURE_MIN_FILTER && param != GL_NEAREST && param != GL_LINEAR) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
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

    // glTexParameteri/glTextureParameteri and every vector form's scalar pnames, once the texture is
    // resolved.
    static void TextureParameterObject_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                             GLenum pname, GLint param, TextureCallForm form, const char* caller) {
        if (!textureObject) return;
        if (!ValidateTextureParameterForTarget(textureObject, pname, param, form, caller)) return;

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
            textureObject->SetAutoGenerateMipmap(param != GL_FALSE);
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
        // P4a, ID-18 M2 - THE THIRTEENTH MGP_NOTE_AGGREGATE(TextureParams) SITE, and the one
        // no publisher reached. Nine of the thirteen are TextureObject.cpp's own mutators and
        // publish through TextureObjectBase::PipePublishParams; the tenth is
        // SetDepthStencilTextureMode; two more move fields MGPTextureParams does not carry. The
        // last is SamplerObject::BumpVersion, whose own comment calls it "the one choke point
        // every setter reaches" - and MGPTextureParams takes MinLod, MaxLod and LodBias off that
        // object, so every glTexParameter that writes GL_TEXTURE_MIN_LOD / MAX_LOD / LOD_BIAS
        // landed on state nothing watched and the applier's record kept saying MinLod = 0.
        // Wrong pixels, not a lost optimisation.
        //
        // THE HOOK IS HERE RATHER THAN ON BumpVersion because MG_State/GLState/SamplerState is
        // package C's after the tag; C.7 grants this file for exactly this class of path ("the
        // grant is one call site per path"), and this switch IS the path - every arm of it
        // either writes the built-in SamplerObject or writes a texture field that publishes for
        // itself. Placed after the switch, so the error arms above return without emitting.
        //
        // IT IS ALSO ID-14's RE-EMIT HOOK. C's sampler CSO cache is content-addressed, so the
        // handle MGPTextureParams::BuiltinSampler names MOVES WITH THE CONTENT; the emitter
        // re-Acquires from the cache and releases the previous handle here. An over-call is
        // free: the emitter's version-first skip reads GetTextureParamsVersion() AND
        // SamplerObject::GetVersion() and returns without hashing anything when neither moved.
        MobileGL::MG_Pipe::MGPipeEmitTextureParams(*textureObject);
    }

    // The float twin of TextureParameterObject_State.
    static void TextureParameterObjectf_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                              GLenum pname, GLfloat param, TextureCallForm form, const char* caller) {
        if (!textureObject) return;
        if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT && !ValidateMaxAnisotropy(param, caller)) return;
        const GLint validationParam = pname == GL_TEXTURE_MAX_ANISOTROPY_EXT ? 1 : static_cast<GLint>(param);
        if (!ValidateTextureParameterForTarget(textureObject, pname, validationParam, form, caller)) return;

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
            const auto swizzleValue = MG_Util::ConvertGLEnumToTextureSwizzleParam((GLenum)param);
            if (swizzleValue == TextureSwizzleParam::Unknown) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Invalid texture swizzle value."));
                return;
            }
            textureObject->SetSwizzleParam(MG_Util::ConvertGLEnumPnameToTextureSwizzleParam(pname), swizzleValue);
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
            textureObject->GetSamplerObject()->SetMaxAnisotropy(param);
            break;
        case GL_GENERATE_MIPMAP:
            textureObject->SetAutoGenerateMipmap(param != 0.0f);
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
        // P4a, ID-18 M2 - THE THIRTEENTH MGP_NOTE_AGGREGATE(TextureParams) SITE, and the one
        // no publisher reached. Nine of the thirteen are TextureObject.cpp's own mutators and
        // publish through TextureObjectBase::PipePublishParams; the tenth is
        // SetDepthStencilTextureMode; two more move fields MGPTextureParams does not carry. The
        // last is SamplerObject::BumpVersion, whose own comment calls it "the one choke point
        // every setter reaches" - and MGPTextureParams takes MinLod, MaxLod and LodBias off that
        // object, so every glTexParameter that writes GL_TEXTURE_MIN_LOD / MAX_LOD / LOD_BIAS
        // landed on state nothing watched and the applier's record kept saying MinLod = 0.
        // Wrong pixels, not a lost optimisation.
        //
        // THE HOOK IS HERE RATHER THAN ON BumpVersion because MG_State/GLState/SamplerState is
        // package C's after the tag; C.7 grants this file for exactly this class of path ("the
        // grant is one call site per path"), and this switch IS the path - every arm of it
        // either writes the built-in SamplerObject or writes a texture field that publishes for
        // itself. Placed after the switch, so the error arms above return without emitting.
        //
        // IT IS ALSO ID-14's RE-EMIT HOOK. C's sampler CSO cache is content-addressed, so the
        // handle MGPTextureParams::BuiltinSampler names MOVES WITH THE CONTENT; the emitter
        // re-Acquires from the cache and releases the previous handle here. An over-call is
        // free: the emitter's version-first skip reads GetTextureParamsVersion() AND
        // SamplerObject::GetVersion() and returns without hashing anything when neither moved.
        MobileGL::MG_Pipe::MGPipeEmitTextureParams(*textureObject);
    }

    const SharedPtr<MG_State::GLState::ITextureObject>& GetTextureObjectByTarget(
        TextureUploadTarget textureUploadTarget, TextureTarget textureTarget) {
        if (TextureImpl::IsProxyTextureTarget(textureUploadTarget)) {
            auto& textureObject = TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget);
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

    // The targets glTexParameter* / glGetTexParameter* accept (GL 4.6 core 8.10 and 8.11). This is a
    // SHORTER list than the one ConvertGLEnumToTextureTarget knows, and deliberately so: that
    // converter folds the six cube-map FACE targets onto TextureCubeMap because glTexImage2D and
    // glCopyTexImage2D need exactly that folding, and it maps GL_TEXTURE_BUFFER to a real target
    // because glTexBuffer needs it. Neither is a legal parameter target, so without a separate
    // predicate glTexParameteri(GL_TEXTURE_CUBE_MAP_POSITIVE_X, ...) quietly applied the parameter
    // to the bound cube map and glGetTexParameterIiv(GL_TEXTURE_BUFFER, ...) quietly answered from
    // the default texture - both GL_NO_ERROR where the spec says GL_INVALID_ENUM.
    //
    // An enum the converter does not know at all was equally silent: it produced TextureTarget::
    // Unknown, GetTextureObjectByTargetForParameter handed back the null object and every caller
    // returned without recording anything. Rejecting here closes that too, at the entry point rather
    // than at the lookup, so exactly one error is recorded.
    //
    // EXACTLY the ten targets 8.10 and 8.11 enumerate - no proxies. The spec's own asymmetry is the
    // proof: GetTexLevelParameter needs an explicit clause extending its list with PROXY_TEXTURE_1D,
    // PROXY_TEXTURE_2D and the rest, and neither TexParameter nor GetTexParameter carries one. That
    // clause is why GetTexLevelParameteriv_State/GetTexLevelParameterfv_State are deliberately NOT
    // gated by this predicate.
    //
    // Routing was not a reason to accept them: GetTextureObjectByTargetForParameter resolves a proxy
    // object only after a proxy glTexImage has run, so before that the parameter call was a silent
    // no-op and after it the parameter was applied for real - both GL_NO_ERROR, and both the same
    // silent-acceptance shape this predicate exists to close for cube faces and GL_TEXTURE_BUFFER.
    static Bool IsLegalTextureParameterTarget(GLenum target) {
        switch (target) {
        case GL_TEXTURE_1D:
        case GL_TEXTURE_2D:
        case GL_TEXTURE_3D:
        case GL_TEXTURE_1D_ARRAY:
        case GL_TEXTURE_2D_ARRAY:
        case GL_TEXTURE_RECTANGLE:
        case GL_TEXTURE_CUBE_MAP:
        case GL_TEXTURE_CUBE_MAP_ARRAY:
        case GL_TEXTURE_2D_MULTISAMPLE:
        case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
        case GL_TEXTURE_EXTERNAL_OES:
            return true;
        default:
            return false;
        }
    }

    // The by-name spelling of the same rule: glTextureParameter* and glGetTextureParameter* have no
    // target token, so GL 4.6 core 8.10 and 8.11 apply the list to the texture's effective target,
    // and a texture outside it (a buffer texture) is INVALID_OPERATION. Null after recording why.
    static const SharedPtr<MG_State::GLState::ITextureObject>& NamedParameterTexture(GLuint texture,
                                                                                     const char* caller) {
        const auto& textureObject = GetTextureObjectByName(texture, caller);
        if (!textureObject) return nullTextureObject;
        const GLenum effectiveTarget = MG_Util::ConvertTextureTargetToGLEnum(textureObject->GetTarget());
        if (IsLegalTextureParameterTarget(effectiveTarget)) return textureObject;
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidOperation,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                         std::format("The effective target {} has no texture parameters.",
                                                     MG_Util::ConvertGLEnumToString(effectiveTarget))));
        return nullTextureObject;
    }

    static Bool ValidateTextureParameterTarget(GLenum target, const char* caller) {
        if (IsLegalTextureParameterTarget(target)) return true;
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidEnum,
            MakeUnique<GenericErrorInfo>(
                "MG_Impl/GLImpl", caller,
                std::format("target {} does not accept texture parameters.", MG_Util::ConvertGLEnumToString(target))));
        return false;
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

    // The texture a bound-target parameter call (set or get) addresses. Null after recording why.
    static const SharedPtr<MG_State::GLState::ITextureObject>& BoundParameterTexture(GLenum target,
                                                                                     const char* caller) {
        if (!ValidateTextureParameterTarget(target, caller)) return nullTextureObject;
        return GetTextureObjectByTargetForParameter(MG_Util::ConvertGLEnumToTextureUploadTarget(target),
                                                    MG_Util::ConvertGLEnumToTextureTarget(target));
    }

    // The backend half of mipmap generation for a validated texture whose chain is allocated. Both
    // forms name the texture itself, so no binding is read on the way to the backend.
    static void GenerateMipmap_Backend(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject) {
        MGP_FILL(GenerateTextureMipmap);
        MG_Backend::gBackendFunctionsTable.GL.GenerateTextureMipmap(
            textureObject, MG_Util::ConvertTextureTargetToGLEnum(textureObject->GetTarget()));
    }

    void MaybeAutoGenerateMipmap(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, Bool isProxy,
                                 GLint level) {
        if (isProxy || level != 0 || !textureObject || !textureObject->GetAutoGenerateMipmap()) {
            return;
        }
        GenerateMipmap_Backend(textureObject);
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
        MGL_SYNC_PIXEL_UNPACK_SOURCE(unpackBuffer);
        return reinterpret_cast<const char*>(unpackBuffer->MappedData()) + reinterpret_cast<SizeT>(data);
    }

    // The box a sub-image upload or copy writes has to lie inside the image it WRITES, which is the
    // requested (uploadTarget, level) pair's - not level 0's.
    //
    // This exists because the general-purpose ValidateTextureSubImageOffsets bounds everything by
    // ITextureObject::GetBaseSize(), which is hardcoded to level 0 (TextureObject::GetBaseSize ->
    // GetTexelSize(0, 0)). CopyReadFramebufferIntoMipmapRegion, meanwhile, sizes its rows and
    // slices from GetMipmapTexelSize(uploadTarget, level) and memcpys into the exact-sized
    // std::vector MipmapStorage allocated for that level, with no clamp of its own. A box that is
    // legal at level 0 and out of range at level N therefore passed validation and wrote past the
    // end of the heap allocation - e.g. a 4x4 copy at offset (4,4) into level 2 of an 8x8x4
    // GL_RGBA8 array texture ran 24 bytes past a 64-byte buffer. Every level > 0 of every
    // mipmapped texture was reachable that way, and both entry points had been no-ops before, so
    // the whole exposure arrived with their implementation.
    static Bool ValidateSubImageRegionAtLevel(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                              TextureUploadTarget uploadTarget, GLint level, GLint xoffset,
                                              GLint yoffset, GLint zoffset, GLsizei width, GLsizei height,
                                              GLsizei depth, const char* caller) {
        const auto* mipmapTexture = MG_State::GLState::AsMipmapTexture(textureObject.get());
        if (mipmapTexture == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "The destination texture has no mipmap storage."));
            return false;
        }
        const IntVec3 levelSize = mipmapTexture->GetMipmapTexelSize(uploadTarget, static_cast<Uint>(level));
        // A level that was never defined reports a degenerate extent. GL 4.6 core 8.6 makes
        // copying into an undefined texture image INVALID_OPERATION, and it is also what keeps the
        // writer below from indexing an empty allocation.
        if (levelSize.x() <= 0 || levelSize.y() <= 0 || levelSize.z() <= 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "The requested texture level has no storage."));
            return false;
        }
        // Signed 64-bit sums: xoffset and width are both GLint and an application may pass values
        // whose sum overflows a GLint, which would otherwise compare as negative and pass.
        const Int64 lastX = static_cast<Int64>(xoffset) + static_cast<Int64>(width);
        const Int64 lastY = static_cast<Int64>(yoffset) + static_cast<Int64>(height);
        const Int64 lastZ = static_cast<Int64>(zoffset) + static_cast<Int64>(depth);
        if (xoffset < 0 || yoffset < 0 || zoffset < 0 || lastX > levelSize.x() || lastY > levelSize.y() ||
            lastZ > levelSize.z()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("The destination region does not lie inside level {} ({}x{}x{}).", level, levelSize.x(),
                                levelSize.y(), levelSize.z())));
            return false;
        }
        return true;
    }

    // The image a cube map's layer `layer` names: faces are layers 0..5 in table 9.3 order.
    static TextureUploadTarget CubeFaceUploadTarget(GLint layer) {
        return static_cast<TextureUploadTarget>(static_cast<SizeT>(TextureUploadTarget::CubeMapPositiveX) +
                                                static_cast<SizeT>(layer));
    }

    // Whether `target` is in `accepted`; otherwise the form's error for an unaccepted target.
    static Bool ValidateCommandTarget(TextureCallForm form, TextureTarget target,
                                      std::initializer_list<TextureTarget> accepted, const char* caller) {
        if (std::find(accepted.begin(), accepted.end(), target) != accepted.end()) return true;
        RecordTargetError(form, caller, "The texture target is not accepted by this command.");
        return false;
    }

    // Every TexSubImage* and TextureSubImage* form once its texture and image are resolved: replace
    // the box `offset`/`size` of `level` with unpacked pixels. With `cubeFaces` the image is a cube
    // map's and z addresses faces (GL 4.6 core 8.6, TextureSubImage3D only): source slice k lands in
    // face offset.z + k.
    static void TexSubImageObject(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                  TextureUploadTarget uploadTarget, Bool cubeFaces, GLint level, IntVec3 offset,
                                  IntVec3 size, GLenum format, GLenum type, const void* pixels, const char* caller) {
        const TextureInputFormat inputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        const TexturePixelDataType dataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
        if (!TextureImpl::ValidateTexturePixelDataType(dataType)) return;
        if (!TextureImpl::ValidateTextureInputFormat(inputFormat)) return;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureSizeRange(size.x(), size.y(), size.z())) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(uploadTarget, level)) return;
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!TextureImpl::ValidateTextureInternalFormatCompatibleWithInput(inputFormat, textureObject->GetFormat(),
                                                                           dataType))
            return;
        if (!ValidatePixelUnpackBufferSource(pixels, inputFormat, dataType, size, caller)) return;

        auto* mipmapObject = MG_State::GLState::AsMipmapTexture(textureObject.get());
        if (mipmapObject == nullptr || static_cast<Uint>(level) >= mipmapObject->GetMipmapLevelCount()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture level is out of range."));
            return;
        }
        if (cubeFaces) {
            if (offset.z() < 0 || size.z() > 6 - offset.z()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                                          "zoffset and depth must select cube faces."));
                return;
            }
            for (GLint face = 0; face < size.z(); ++face) {
                if (!ValidateSubImageRegionAtLevel(textureObject, CubeFaceUploadTarget(offset.z() + face), level,
                                                   offset.x(), offset.y(), 0, size.x(), size.y(), 1, caller))
                    return;
            }
        } else if (!ValidateSubImageRegionAtLevel(textureObject, uploadTarget, level, offset.x(), offset.y(),
                                                  offset.z(), size.x(), size.y(), size.z(), caller)) {
            return;
        }

        const void* source = pixels;
        const auto& unpackBuffer =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelUnpack).GetBoundObject();
        if (unpackBuffer) {
            MGL_SYNC_PIXEL_UNPACK_SOURCE(unpackBuffer);
            source = reinterpret_cast<const char*>(unpackBuffer->MappedData()) + reinterpret_cast<SizeT>(pixels);
        }
        if (!source) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "No data supplied from pixels parameter and no PBO bound."));
            return;
        }

        SizeT unpackedSize = 0;
        void* unpacked = MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(
            source, MG_State::pGLContext->GetPixelStoreParameters(true), textureObject->GetFormat(), inputFormat,
            dataType, size, false, unpackedSize);
        if (!unpacked || unpackedSize == 0) {
            free(unpacked);
            return;
        }

        const SizeT bytesPerTexel = MG_Util::GetInternalBytesPerPixel(textureObject->GetFormat(), dataType);
        const SizeT srcRowBytes = static_cast<SizeT>(size.x()) * bytesPerTexel;
        const SizeT srcSliceBytes = static_cast<SizeT>(size.y()) * srcRowBytes;
        const auto* src = static_cast<const Uint8*>(unpacked);
        for (GLint slice = 0; slice < size.z(); ++slice) {
            const TextureUploadTarget image = cubeFaces ? CubeFaceUploadTarget(offset.z() + slice) : uploadTarget;
            const GLint z = cubeFaces ? 0 : offset.z() + slice;
            const IntVec3 imageSize = mipmapObject->GetMipmapTexelSize(image, level);
            const SizeT dstRowBytes = static_cast<SizeT>(imageSize.x()) * bytesPerTexel;
            const SizeT dstSliceBytes = static_cast<SizeT>(imageSize.y()) * dstRowBytes;
            auto* dst = static_cast<Uint8*>(mipmapObject->MapMipmapData(image, level));
            if (!dst) continue;
            for (GLint row = 0; row < size.y(); ++row) {
                Memcpy(dst + static_cast<SizeT>(z) * dstSliceBytes +
                           static_cast<SizeT>(offset.y() + row) * dstRowBytes +
                           static_cast<SizeT>(offset.x()) * bytesPerTexel,
                       src + static_cast<SizeT>(slice) * srcSliceBytes + static_cast<SizeT>(row) * srcRowBytes,
                       srcRowBytes);
            }
            if (cubeFaces)
                mipmapObject->MarkStorageDirtyRegion(image, level, {offset.x(), offset.y(), 0},
                                                     {size.x(), size.y(), 1});
        }
        free(unpacked);
        if (!cubeFaces) mipmapObject->MarkStorageDirtyRegion(uploadTarget, level, offset, size);
        MaybeAutoGenerateMipmap(textureObject, false, level);
    }

    // The bound-target forms: GL 4.6 core table 8.15 names the targets each accepts.
    static void TexSubImage_State(GLenum target, std::initializer_list<TextureTarget> accepted, GLint level,
                                  IntVec3 offset, IntVec3 size, GLenum format, GLenum type, const void* pixels,
                                  const char* caller) {
        const TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        const TextureUploadTarget uploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        if (TextureImpl::IsProxyTextureTarget(uploadTarget)) {
            RecordTargetError(TextureCallForm::Bound, caller, "A proxy target has no texture image to modify.");
            return;
        }
        if (!ValidateCommandTarget(TextureCallForm::Bound, textureTarget, accepted, caller)) return;
        const auto& textureObject =
            MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit())
                .GetBindingSlot(textureTarget)
                .GetBoundObject();
        TexSubImageObject(textureObject, uploadTarget, false, level, offset, size, format, type, pixels, caller);
    }

    void TexSubImage3D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                             GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels) {
        TexSubImage_State(target,
                          {TextureTarget::Texture3D, TextureTarget::Texture2DArray, TextureTarget::TextureCubeMapArray},
                          level, {xoffset, yoffset, zoffset}, {width, height, depth}, format, type, pixels, __func__);
    }

    // The 2D form takes a cube map through its six face targets, which the converter folds onto
    // TextureCubeMap; TEXTURE_CUBE_MAP itself has no upload target and is refused by that check.
    void TexSubImage2D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                             GLenum format, GLenum type, const void* pixels) {
        if (MG_Util::ConvertGLEnumToTextureUploadTarget(target) == TextureUploadTarget::Unknown) {
            RecordTargetError(TextureCallForm::Bound, __func__,
                              "The texture target is not accepted by this sub-image command.");
            return;
        }
        TexSubImage_State(target,
                          {TextureTarget::Texture2D, TextureTarget::Texture1DArray, TextureTarget::TextureRectangle,
                           TextureTarget::TextureCubeMap},
                          level, {xoffset, yoffset, 0}, {width, height, 1}, format, type, pixels, __func__);
    }

    void TexSubImage1D_State(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                             const GLvoid* pixels) {
        TexSubImage_State(target, {TextureTarget::Texture1D}, level, {xoffset, 0, 0}, {width, 1, 1}, format, type,
                          pixels, __func__);
    }

    // The vector setters' object cores. BORDER_COLOR and SWIZZLE_RGBA are the two vector pnames;
    // every other pname goes to the scalar core with the first element.
    static void TextureParameterObjectfv_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                               GLenum pname, const GLfloat* params, TextureCallForm form,
                                               const char* caller) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR:
            if (!ValidateTextureParameterForTarget(textureObject, pname, 0, form, caller)) return;
            SetTextureBorderColorFromFloats(textureObject, params);
            break;
        case GL_TEXTURE_SWIZZLE_RGBA: {
            const GLint swizzle[4] = {static_cast<GLint>(params[0]), static_cast<GLint>(params[1]),
                                      static_cast<GLint>(params[2]), static_cast<GLint>(params[3])};
            SetTextureSwizzleParamsFromInts(textureObject, swizzle, caller);
            break;
        }
        default:
            TextureParameterObjectf_State(textureObject, pname, *params, form, caller);
            break;
        }
    }

    static void TextureParameterObjectiv_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                               GLenum pname, const GLint* params, TextureCallForm form,
                                               const char* caller) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR:
            if (!ValidateTextureParameterForTarget(textureObject, pname, 0, form, caller)) return;
            SetTextureBorderColorFromInts(textureObject, params);
            break;
        case GL_TEXTURE_SWIZZLE_RGBA:
            SetTextureSwizzleParamsFromInts(textureObject, params, caller);
            break;
        default:
            TextureParameterObject_State(textureObject, pname, *params, form, caller);
            break;
        }
    }

    static void TextureParameterObjectIiv_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                GLenum pname, const GLint* params, TextureCallForm form,
                                                const char* caller) {
        if (pname != GL_TEXTURE_BORDER_COLOR) {
            TextureParameterObjectiv_State(textureObject, pname, params, form, caller);
            return;
        }
        if (!ValidateTextureParameterForTarget(textureObject, pname, 0, form, caller)) return;
        SetTextureBorderColorFromIntegerInts(textureObject, params);
    }

    static void TextureParameterObjectIuiv_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                 GLenum pname, const GLuint* params, TextureCallForm form,
                                                 const char* caller) {
        switch (pname) {
        case GL_TEXTURE_BORDER_COLOR:
            if (!ValidateTextureParameterForTarget(textureObject, pname, 0, form, caller)) return;
            SetTextureBorderColorFromUnsignedInts(textureObject, params);
            break;
        case GL_TEXTURE_SWIZZLE_RGBA: {
            const GLint swizzle[4] = {static_cast<GLint>(params[0]), static_cast<GLint>(params[1]),
                                      static_cast<GLint>(params[2]), static_cast<GLint>(params[3])};
            SetTextureSwizzleParamsFromInts(textureObject, swizzle, caller);
            break;
        }
        default:
            TextureParameterObject_State(textureObject, pname, static_cast<GLint>(*params), form, caller);
            break;
        }
    }

    void TexParameterf_State(GLenum target, GLenum pname, GLfloat param) {
        const auto& textureObject = BoundParameterTexture(target, __func__);
        TextureParameterObjectf_State(textureObject, pname, param, TextureCallForm::Bound, __func__);
    }

    void TexParameteri_State(GLenum target, GLenum pname, GLint param) {
        const auto& textureObject = BoundParameterTexture(target, __func__);
        TextureParameterObject_State(textureObject, pname, param, TextureCallForm::Bound, __func__);
    }

    void TexParameterfv_State(GLenum target, GLenum pname, const GLfloat* params) {
        const auto& textureObject = BoundParameterTexture(target, __func__);
        if (!textureObject || !params) return;
        TextureParameterObjectfv_State(textureObject, pname, params, TextureCallForm::Bound, __func__);
    }

    void TexParameteriv_State(GLenum target, GLenum pname, const GLint* params) {
        const auto& textureObject = BoundParameterTexture(target, __func__);
        if (!textureObject || !params) return;
        TextureParameterObjectiv_State(textureObject, pname, params, TextureCallForm::Bound, __func__);
    }

    void TexParameterIiv_State(GLenum target, GLenum pname, const GLint* params) {
        const auto& textureObject = BoundParameterTexture(target, __func__);
        if (!textureObject || !params) return;
        TextureParameterObjectIiv_State(textureObject, pname, params, TextureCallForm::Bound, __func__);
    }

    void TexParameterIuiv_State(GLenum target, GLenum pname, const GLuint* params) {
        const auto& textureObject = BoundParameterTexture(target, __func__);
        if (!textureObject || !params) return;
        TextureParameterObjectIuiv_State(textureObject, pname, params, TextureCallForm::Bound, __func__);
    }

    // The format a multisample image is stored in, once format, sample count and extent are
    // validated for `textureTarget`; Unknown after recording why.
    static TextureInternalFormat ValidateMultisampleImage(TextureTarget textureTarget, GLsizei samples,
                                                          GLenum internalformat, GLsizei width, GLsizei height,
                                                          GLsizei depth, const char* caller) {
        const TextureInternalFormat format =
            MG_Util::ConvertInternalFormatToSized(MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat),
                                                  TextureInputFormat::RGBA, TexturePixelDataType::UnsignedByte);
        if (!TextureImpl::ValidateTextureInternalFormat(format)) return TextureInternalFormat::Unknown;
        if (!ValidateTextureMultisampleStorage(textureTarget, samples, width, height, depth, format, caller)) {
            return TextureInternalFormat::Unknown;
        }
        return format;
    }

    // (Re)allocates the one image of a resolved multisample texture.
    static Bool AllocateMultisampleImage(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                         TextureUploadTarget uploadTarget, TextureInternalFormat format,
                                         GLsizei samples, GLsizei width, GLsizei height, GLsizei depth,
                                         GLboolean fixedsamplelocations, const char* caller) {
        if (!TextureImpl::ValidateTextureObject(textureObject)) return false;
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture storage is not mipmap-backed."));
            return false;
        }
        AllocateMultisampleTextureStorage(textureObject, uploadTarget, format, samples, width, height, depth,
                                          fixedsamplelocations);
        return true;
    }

    // glTexImage2DMultisample/3DMultisample: the texture bound to `target`, or the target's proxy.
    // Name 0 resolves to the target's default texture object, a real texture this call respecifies.
    static Bool TexImageMultisample_State(GLenum target, TextureTarget acceptedTarget, GLsizei samples,
                                          GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth,
                                          GLboolean fixedsamplelocations, const char* caller) {
        const TextureUploadTarget uploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        const TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return false;
        if (!TextureImpl::ValidateTextureUploadTarget(uploadTarget)) return false;
        if (textureTarget != acceptedTarget) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             acceptedTarget == TextureTarget::Texture2DMultisample
                                                 ? "Target must be GL_TEXTURE_2D_MULTISAMPLE or its proxy."
                                                 : "Target must be GL_TEXTURE_2D_MULTISAMPLE_ARRAY or its proxy."));
            return false;
        }
        const TextureInternalFormat format =
            ValidateMultisampleImage(textureTarget, samples, internalformat, width, height, depth, caller);
        if (format == TextureInternalFormat::Unknown) return false;
        const auto& textureObject =
            TextureImpl::IsProxyTextureTarget(uploadTarget)
                ? TextureImpl::pProxyTextureManager->CreateOrReplaceProxyTextureObject(uploadTarget)
                : MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit())
                      .GetBindingSlot(textureTarget)
                      .GetBoundObject();
        return AllocateMultisampleImage(textureObject, uploadTarget, format, samples, width, height, depth,
                                        fixedsamplelocations, caller);
    }

    Bool TexImage3DMultisample_State(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
        return TexImageMultisample_State(target, TextureTarget::Texture2DMultisampleArray, samples, internalformat,
                                         width, height, depth, fixedsamplelocations, __func__);
    }

    Bool TexImage2DMultisample_State(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLboolean fixedsamplelocations) {
        return TexImageMultisample_State(target, TextureTarget::Texture2DMultisample, samples, internalformat, width,
                                         height, 1, fixedsamplelocations, __func__);
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
        if (!TextureImpl::ValidateCubeMapArrayShape(textureUploadTarget, width, height, depth, __func__)) return;
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
            MGL_SYNC_PIXEL_UNPACK_SOURCE(pixelUnpackBufferObject);
            originalPixels =
                reinterpret_cast<const char*>(pixelUnpackBufferObject->MappedData()) + reinterpret_cast<SizeT>(pixels);
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
            // Also after AllocateStorage, which clears it. Records the generic GL_COMPRESSED_*
            // enums too, which the tag above deliberately skips - glClearTexImage has to refuse
            // them all (GL 4.6 core 8.19).
            if (IsCompressedGLInternalFormat(static_cast<GLenum>(internalformat))) {
                textureMipmapObject->SetMipmapRequestedCompressedFormat(textureUploadTarget, level,
                                                                        static_cast<GLenum>(internalformat));
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
        MaybeAutoGenerateMipmap(textureObject, isProxy, level);
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
            MGL_SYNC_PIXEL_UNPACK_SOURCE(pixelUnpackBufferObject);
            originalPixels =
                reinterpret_cast<const char*>(pixelUnpackBufferObject->MappedData()) + reinterpret_cast<SizeT>(pixels);
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
            textureMipmapObject->AllocateStorage(textureUploadTarget, level, {{width, height, 1}, internalBytes});
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
            // Also after AllocateStorage, which clears it. Records the generic GL_COMPRESSED_*
            // enums too, which the tag above deliberately skips - glClearTexImage has to refuse
            // them all (GL 4.6 core 8.19).
            if (IsCompressedGLInternalFormat(static_cast<GLenum>(internalformat))) {
                textureMipmapObject->SetMipmapRequestedCompressedFormat(textureUploadTarget, level,
                                                                        static_cast<GLenum>(internalformat));
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
        MaybeAutoGenerateMipmap(textureObject, isProxy, level);
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
            MGL_SYNC_PIXEL_UNPACK_SOURCE(pixelUnpackBufferObject);
            originalPixels =
                reinterpret_cast<const char*>(pixelUnpackBufferObject->MappedData()) + reinterpret_cast<SizeT>(pixels);
        }

        MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get()),
                        "Texture object here should always be an object with mipmap");
        auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (!isProxy) {
            DiscardMipmapChainOnBaseRespecification(textureMipmapObject, textureUploadTarget, level);
            textureMipmapObject->AllocateStorage(textureUploadTarget, level, {{width, 1, 1}, internalBytes});
            // After AllocateStorage, which clears the tag. No block-compressed format has a 1D
            // layout, so only the specific-format tag the 2D/3D paths record is skipped here - the
            // request itself still has to be remembered for glClearTexImage (GL 4.6 core 8.19).
            if (IsCompressedGLInternalFormat(static_cast<GLenum>(internalFormat))) {
                textureMipmapObject->SetMipmapRequestedCompressedFormat(textureUploadTarget, level,
                                                                        static_cast<GLenum>(internalFormat));
            }
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
        MaybeAutoGenerateMipmap(textureObject, isProxy, level);
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

    // GL 4.6 core 8.9 / GL_EXT_texture_buffer: the two TARGET-taking forms (glTexBuffer,
    // glTexBufferRange) accept exactly GL_TEXTURE_BUFFER, and anything else is GL_INVALID_ENUM.
    // Checked up front rather than left to fall out of "the bound object is not a buffer texture"
    // deeper in, because that path's error code depends on which entry point took it - the
    // name-taking DSA forms owe GL_INVALID_OPERATION for the same shape - and because for some
    // targets it did not reach that check at all. esextcTextureBufferErrors walks every other
    // texture target through both entry points and reads the code back each time.
    static Bool ValidateBufferTextureTarget(GLenum target, const char* caller) {
        if (target == GL_TEXTURE_BUFFER) return true;
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidEnum,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                         std::format("target 0x{:X} is not GL_TEXTURE_BUFFER.", target)));
        return false;
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
            const Int alignment =
                std::max(1, MG_Backend::pActiveBackendObject->GetDynamicParameters().TextureBufferOffsetAlignment);
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
        if (!ValidateBufferTextureTarget(target, __func__)) return;
        if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        if (!TextureImpl::ValidateTextureInternalFormat(textureInternalFormat)) return;
        // The sized-format table a buffer texture accepts (GL 4.6 core table 8.15). The DSA and
        // range forms have always run this through AttachBufferToTexture; this one carried a TODO
        // instead, so glTexBuffer(GL_TEXTURE_BUFFER, GL_DEPTH_COMPONENT32F, ...) succeeded.
        if (!IsBufferTextureInternalFormat(internalformat)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    std::format("internalformat 0x{:X} is not one of the sized formats a buffer texture accepts.",
                                internalformat)));
            return;
        }
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
            // Defensive: the target gate above already rejected every target but GL_TEXTURE_BUFFER,
            // whose binding slot only ever holds buffer textures.
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

    // Every texture parameter but the border colour, for one resolved texture: up to four values,
    // as doubles because a double holds each enum, level and float this state stores exactly. False
    // after recording an error. The border colour converts differently for each query type (GL 4.6
    // core 8.11.2), so each form answers it itself.
    static Bool QueryTextureParameter(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, GLenum pname,
                                      Double (&values)[4], Uint& count, const char* caller) {
        const auto& sampler = textureObject->GetSamplerObject();
        count = 1;
        switch (pname) {
        // GL_OES_EGL_image_external: one unit per external texture (the image arrives as RGBA).
        case GL_REQUIRED_TEXTURE_IMAGE_UNITS_OES:
            if (textureObject->GetTarget() != TextureTarget::External) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "GL_REQUIRED_TEXTURE_IMAGE_UNITS_OES is an external texture's."));
                return false;
            }
            values[0] = 1;
            return true;
        case GL_TEXTURE_MAG_FILTER:
            values[0] = MG_Util::ConvertSamplerFilterModeToGLEnum(sampler->GetMagFilter(), SamplerMipmapMode::None);
            return true;
        case GL_TEXTURE_MIN_FILTER:
            values[0] = MG_Util::ConvertSamplerFilterModeToGLEnum(sampler->GetMinFilter(), sampler->GetMipmapMode());
            return true;
        case GL_TEXTURE_MIN_LOD:
            values[0] = sampler->GetMinLod();
            return true;
        case GL_TEXTURE_MAX_LOD:
            values[0] = sampler->GetMaxLod();
            return true;
        case GL_TEXTURE_LOD_BIAS:
            values[0] = sampler->GetLodBias();
            return true;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            values[0] = sampler->GetMaxAnisotropy();
            return true;
        case GL_TEXTURE_WRAP_S:
            values[0] = MG_Util::ConvertSamplerWrapModeToGLEnum(sampler->GetWrapS());
            return true;
        case GL_TEXTURE_WRAP_T:
            values[0] = MG_Util::ConvertSamplerWrapModeToGLEnum(sampler->GetWrapT());
            return true;
        case GL_TEXTURE_WRAP_R:
            values[0] = MG_Util::ConvertSamplerWrapModeToGLEnum(sampler->GetWrapR());
            return true;
        case GL_TEXTURE_COMPARE_MODE:
            values[0] = MG_Util::ConvertSamplerCompareModeToGLEnum(sampler->GetCompareMode());
            return true;
        case GL_TEXTURE_COMPARE_FUNC:
            values[0] = MG_Util::ConvertSamplerCompareFuncToGLEnum(sampler->GetSamplerCompareFunc());
            return true;
        case GL_TEXTURE_BASE_LEVEL:
            values[0] = textureObject->GetSpecifiedLevelRange().x();
            return true;
        case GL_TEXTURE_MAX_LEVEL:
            values[0] = textureObject->GetSpecifiedLevelRange().y();
            return true;
        case GL_TEXTURE_IMMUTABLE_FORMAT:
            values[0] = textureObject->IsImmutable() ? GL_TRUE : GL_FALSE;
            return true;
        case GL_TEXTURE_IMMUTABLE_LEVELS:
            values[0] = textureObject->GetImmutableLevels();
            return true;
        // GL 4.6 core table 23.17. Zero on a mutable texture; TexStorage* seeds the full extent
        // and glTextureView composes onto it (SeedImmutableViewState / TextureView).
        case GL_TEXTURE_VIEW_MIN_LEVEL:
            values[0] = textureObject->GetViewMinLevel();
            return true;
        case GL_TEXTURE_VIEW_NUM_LEVELS:
            values[0] = textureObject->GetViewNumLevels();
            return true;
        case GL_TEXTURE_VIEW_MIN_LAYER:
            values[0] = textureObject->GetViewMinLayer();
            return true;
        case GL_TEXTURE_VIEW_NUM_LAYERS:
            values[0] = textureObject->GetViewNumLayers();
            return true;
        case GL_TEXTURE_SWIZZLE_R:
        case GL_TEXTURE_SWIZZLE_G:
        case GL_TEXTURE_SWIZZLE_B:
        case GL_TEXTURE_SWIZZLE_A:
            values[0] = MG_Util::ConvertTextureSwizzleParamToGLEnum(
                textureObject->GetAllSwizzleParams()[static_cast<SizeT>(pname - GL_TEXTURE_SWIZZLE_R)]);
            return true;
        case GL_TEXTURE_SWIZZLE_RGBA: {
            const auto& swizzle = textureObject->GetAllSwizzleParams();
            for (SizeT i = 0; i < 4; ++i) {
                values[i] = MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzle[i]);
            }
            count = 4;
            return true;
        }
        case GL_DEPTH_STENCIL_TEXTURE_MODE:
            values[0] = textureObject->GetDepthStencilTextureMode();
            return true;
        case GL_IMAGE_FORMAT_COMPATIBILITY_TYPE:
            values[0] = GL_IMAGE_FORMAT_COMPATIBILITY_BY_SIZE;
            return true;
        // GL 4.6 core 8.11.2: the target the texture was bound to first, which for the target form
        // is the target parameter.
        case GL_TEXTURE_TARGET:
            values[0] = MG_Util::ConvertTextureTargetToGLEnum(textureObject->GetTarget());
            return true;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("pname {} is not a valid texture parameter.", MG_Util::ConvertGLEnumToString(pname))));
            return false;
        }
    }

    static void GetTextureParameterObjectiv_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                  GLenum pname, GLint* params, const char* caller) {
        if (pname == GL_TEXTURE_BORDER_COLOR) {
            // The inverse of glTexParameteriv: GL 4.6 core equation 2.3 against equation 2.2 on the
            // write side (SetTextureBorderColorFromInts). The raw integer colour is the Iiv query's.
            const auto& borderColor = textureObject->GetBorderColor();
            for (int i = 0; i < 4; ++i)
                params[i] = MG_Util::FloatToSignedNormalizedInt32(borderColor[i]);
            return;
        }
        Double values[4] = {};
        Uint count = 0;
        if (!QueryTextureParameter(textureObject, pname, values, count, caller)) return;
        for (Uint i = 0; i < count; ++i)
            params[i] = static_cast<GLint>(values[i]);
    }

    static void GetTextureParameterObjectfv_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                  GLenum pname, GLfloat* params, const char* caller) {
        if (pname == GL_TEXTURE_BORDER_COLOR) {
            const auto& borderColor = textureObject->GetBorderColor();
            for (int i = 0; i < 4; ++i)
                params[i] = borderColor[i];
            return;
        }
        Double values[4] = {};
        Uint count = 0;
        if (!QueryTextureParameter(textureObject, pname, values, count, caller)) return;
        for (Uint i = 0; i < count; ++i)
            params[i] = static_cast<GLfloat>(values[i]);
    }

    static void GetTextureParameterObjectIiv_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                   GLenum pname, GLint* params, const char* caller) {
        if (pname != GL_TEXTURE_BORDER_COLOR) {
            GetTextureParameterObjectiv_State(textureObject, pname, params, caller);
            return;
        }
        const auto& borderColor = textureObject->GetBorderColorI();
        for (int i = 0; i < 4; ++i)
            params[i] = borderColor[i];
    }

    static void GetTextureParameterObjectIuiv_State(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                                    GLenum pname, GLuint* params, const char* caller) {
        if (pname == GL_TEXTURE_BORDER_COLOR) {
            const auto& borderColor = textureObject->GetBorderColorUI();
            for (int i = 0; i < 4; ++i)
                params[i] = borderColor[i];
            return;
        }
        Double values[4] = {};
        Uint count = 0;
        if (!QueryTextureParameter(textureObject, pname, values, count, caller)) return;
        for (Uint i = 0; i < count; ++i)
            params[i] = static_cast<GLuint>(static_cast<GLint>(values[i]));
    }

    void GetTexParameterIuiv_State(GLenum target, GLenum pname, GLuint* params) {
        const auto& textureObject = BoundParameterTexture(target, __func__);
        if (textureObject && params) GetTextureParameterObjectIuiv_State(textureObject, pname, params, __func__);
    }

    void GetTexParameterIiv_State(GLenum target, GLenum pname, GLint* params) {
        const auto& textureObject = BoundParameterTexture(target, __func__);
        if (textureObject && params) GetTextureParameterObjectIiv_State(textureObject, pname, params, __func__);
    }

    void GetTexParameteriv_State(GLenum target, GLenum pname, GLint* params) {
        const auto& textureObject = BoundParameterTexture(target, __func__);
        if (textureObject && params) GetTextureParameterObjectiv_State(textureObject, pname, params, __func__);
    }

    void GetTexParameterfv_State(GLenum target, GLenum pname, GLfloat* params) {
        const auto& textureObject = BoundParameterTexture(target, __func__);
        if (textureObject && params) GetTextureParameterObjectfv_State(textureObject, pname, params, __func__);
    }

    // One level parameter of one resolved texture image. False after recording an error; every
    // level pname is a scalar.
    static Bool QueryTextureLevelParameter(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                           TextureUploadTarget uploadTarget, Bool isProxy, GLint level, GLenum pname,
                                           Int64& value, const char* caller) {
        const auto storageType = textureObject->GetStorageType();
        const auto* mipmapObject = MG_State::GLState::AsMipmapTexture(textureObject.get());
        switch (pname) {
        case GL_TEXTURE_WIDTH:
        case GL_TEXTURE_HEIGHT:
        case GL_TEXTURE_DEPTH: {
            const SizeT axis = pname == GL_TEXTURE_WIDTH ? 0 : (pname == GL_TEXTURE_HEIGHT ? 1 : 2);
            if (storageType == TextureStorageType::Mipmap && mipmapObject != nullptr) {
                value = mipmapObject->GetMipmapTexelSize(uploadTarget, level)[axis];
            } else if (storageType == TextureStorageType::Buffer) {
                // A buffer texture is one-dimensional.
                value = axis == 0 ? GetBufferTextureTexelWidth(textureObject.get()) : 1;
            } else {
                RecordUnsupportedLevelQueryStorage(caller, pname);
                return false;
            }
            return true;
        }
        case GL_TEXTURE_INTERNAL_FORMAT: {
            // A level stored compressed reports the token it was given, not the uncompressed format
            // backing it (GL 4.6 core 8.11). glCompressedTexImage2D sets that tag, and so does a
            // glTexImage2D given a SPECIFIC compressed internalformat.
            const GLenum compressedFormat = GetCompressedLevelFormat(textureObject, uploadTarget, level);
            value = compressedFormat != GL_NONE
                        ? compressedFormat
                        : MG_Util::ConvertTextureInternalFormatToGLEnum(textureObject->GetFormat());
            return true;
        }
        case GL_TEXTURE_SAMPLES:
            value = textureObject->GetSamples();
            return true;
        case GL_TEXTURE_FIXED_SAMPLE_LOCATIONS:
            value = textureObject->HasFixedSampleLocations() ? GL_TRUE : GL_FALSE;
            return true;
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
        case GL_TEXTURE_SHARED_SIZE:
            value = GetTextureLevelComponentParameter(textureObject->GetFormat(), pname);
            return true;
        case GL_TEXTURE_COMPRESSED:
            value = GetCompressedLevelFormat(textureObject, uploadTarget, level) != GL_NONE ? GL_TRUE : GL_FALSE;
            return true;
        case GL_TEXTURE_COMPRESSED_IMAGE_SIZE:
            // GL 4.6 core 8.11: an uncompressed image or a proxy has no compressed size to report.
            if (isProxy || GetCompressedLevelFormat(textureObject, uploadTarget, level) == GL_NONE) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", caller,
                        "GL_TEXTURE_COMPRESSED_IMAGE_SIZE needs a compressed, non-proxy texture image."));
                return false;
            }
            value =
                static_cast<Int64>(mipmapObject->GetMipmapCompressedByteSize(uploadTarget, static_cast<Uint>(level)));
            return true;
        case GL_TEXTURE_BUFFER_DATA_STORE_BINDING:
            value = GetBufferTextureDataStoreBinding(textureObject.get());
            return true;
        case GL_TEXTURE_BUFFER_SIZE:
        case GL_TEXTURE_BUFFER_OFFSET: {
            // GL 4.6 core 8.9: the window of the attached buffer a GL_TEXTURE_BUFFER texture
            // addresses; any other storage has nothing to report.
            if (storageType != TextureStorageType::Buffer) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "GL_TEXTURE_BUFFER_SIZE / GL_TEXTURE_BUFFER_OFFSET need a buffer "
                                                 "texture."));
                return false;
            }
            // Basic machine units, and UNCLAMPED - see GetBufferTextureTexelWidth for why this
            // half does not take the GL_MAX_TEXTURE_BUFFER_SIZE clamp that WIDTH does.
            const auto* bufferTextureObject = static_cast<MG_State::GLState::TextureObjectBuffer*>(textureObject.get());
            value =
                static_cast<Int64>(pname == GL_TEXTURE_BUFFER_SIZE ? bufferTextureObject->GetBufferRangeSizeInBytes()
                                                                   : bufferTextureObject->GetBufferRangeOffset());
            return true;
        }
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                                     "pname is not a valid texture level parameter."));
            return false;
        }
    }

    // The texture image a bound-target level query addresses: a proxy's or the bound texture's.
    static const SharedPtr<MG_State::GLState::ITextureObject>& BoundLevelQueryTexture(GLenum target, GLint level) {
        const TextureUploadTarget uploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        if (!TextureImpl::ValidateTextureUploadTarget(uploadTarget)) return nullTextureObject;
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return nullTextureObject;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(uploadTarget, level)) return nullTextureObject;
        const auto& textureObject =
            TextureImpl::IsProxyTextureTarget(uploadTarget)
                ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(uploadTarget)
                : MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit())
                      .GetBindingSlot(MG_Util::ConvertGLEnumToTextureTarget(target))
                      .GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return nullTextureObject;
        return textureObject;
    }

    void GetTexLevelParameteriv_State(GLenum target, GLint level, GLenum pname, GLint* params) {
        const auto& textureObject = BoundLevelQueryTexture(target, level);
        if (!textureObject) return;
        const TextureUploadTarget uploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        Int64 value = 0;
        if (!QueryTextureLevelParameter(textureObject, uploadTarget, TextureImpl::IsProxyTextureTarget(uploadTarget),
                                        level, pname, value, __func__))
            return;
        if (params) *params = static_cast<GLint>(value);
    }

    void GetTexLevelParameterfv_State(GLenum target, GLint level, GLenum pname, GLfloat* params) {
        const auto& textureObject = BoundLevelQueryTexture(target, level);
        if (!textureObject) return;
        const TextureUploadTarget uploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        Int64 value = 0;
        if (!QueryTextureLevelParameter(textureObject, uploadTarget, TextureImpl::IsProxyTextureTarget(uploadTarget),
                                        level, pname, value, __func__))
            return;
        if (params) *params = static_cast<GLfloat>(value);
    }

    // Where every compressed readback lands its bytes: the destination-size check, then either the
    // bound pixel pack buffer (pixels is an offset into it) or client memory. No pixel-store packing
    // here on purpose: GL 4.6 core 8.11 says the pixel storage modes are ignored for a compressed
    // image, which is also the only way the round trip stays byte-exact.
    static void WriteCompressedImageToClientOrPBO(const void* src, SizeT imageSize, GLsizei bufSize, void* pixels,
                                                  const char* caller) {
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

        if (pixels) Memcpy(pixels, src, imageSize);
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

        WriteCompressedImageToClientOrPBO(src, imageSize, bufSize, pixels, caller);
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

    Bool ValidateCopyTextureSubImage(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, GLint level,
                                     GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height,
                                     GLsizei depth, const char* caller);

    // The shared body of glCopyTexSubImage3D and glCopyTextureSubImage3D once the caller has
    // resolved the destination texture. `allowCubeFaceFromZOffset` is the ONE difference between
    // the two forms: the DSA form takes a cube map and selects the face with zoffset (GL 4.6 core
    // 8.6), while the target-taking form cannot even name a cube map here - GL_TEXTURE_CUBE_MAP is
    // not in glCopyTexSubImage3D's accepted-target list, its faces go through
    // glCopyTexSubImage2D - so for it zoffset is always a layer index.
    static void CopyTextureSubImage3DResolved(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                              GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x,
                                              GLint y, GLsizei width, GLsizei height, Bool allowCubeFaceFromZOffset,
                                              const char* caller) {
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (width < 0 || height < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Copy dimensions must be non-negative."));
            return;
        }

        // THE FACE MAPPING HAS TO HAPPEN BEFORE THE BOUNDS CHECK, not after it. A cube map stores
        // its six faces as six upload targets of ONE z-slice each, so its GetBaseSize().z() is 1 -
        // and the generic offset validator, whose z bound always comes from that, rejected every
        // zoffset in 1..5 with GL_INVALID_VALUE before the mapping below could run. Five of six
        // faces were unreachable through glCopyTextureSubImage3D even though the entry point
        // documents zoffset as the face selector (GL 4.6 core 8.6). The cube bound is the FACE
        // COUNT, which the generic validator has no way to express because its `depth` parameter
        // is the copy extent; glClearTexSubImage already special-cases the same shape.
        TextureUploadTarget uploadTarget = GetPrimaryUploadTarget(textureObject);
        GLint sliceOffset = zoffset;
        if (allowCubeFaceFromZOffset && textureObject->GetTarget() == TextureTarget::TextureCubeMap) {
            const SizeT faceCount = textureObject->GetUploadTargets().size();
            if (zoffset < 0 || static_cast<SizeT>(zoffset) >= faceCount) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "zoffset selects the cube map face and must be in [0, " +
                                                     std::to_string(faceCount) + ")."));
                return;
            }
            uploadTarget = static_cast<TextureUploadTarget>(static_cast<SizeT>(TextureUploadTarget::CubeMapPositiveX) +
                                                            static_cast<SizeT>(zoffset));
            sliceOffset = 0;
        }

        if (!ValidateSubImageRegionAtLevel(textureObject, uploadTarget, level, xoffset, yoffset, sliceOffset, width,
                                           height, /*depth=*/1, caller)) {
            return;
        }
        if (!FramebufferImpl::ValidateReadFramebufferForCopy(caller)) return;
        CopyReadFramebufferIntoMipmapRegion(textureObject, uploadTarget, level, xoffset, yoffset, sliceOffset, x, y,
                                            width, height, caller);
    }

    // The same for the one-dimensional pair. A 1D level is {width, 1, 1}, so the y and z arms of
    // the check above are trivially satisfied and the x arm is the whole rule - which is exactly
    // the one that overflowed: level 2 of an 8-texel GL_RGBA8 1D texture is 8 bytes, and a 4-texel
    // copy at xoffset 4 wrote 16 bytes starting 16 bytes in, entirely outside the allocation.
    static void CopyTextureSubImage1DResolved(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                              GLint level, GLint xoffset, GLint x, GLint y, GLsizei width,
                                              const char* caller) {
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (width < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Copy dimensions must be non-negative."));
            return;
        }
        const TextureUploadTarget uploadTarget = GetPrimaryUploadTarget(textureObject);
        if (!ValidateSubImageRegionAtLevel(textureObject, uploadTarget, level, xoffset, /*yoffset=*/0,
                                           /*zoffset=*/0, width, /*height=*/1, /*depth=*/1, caller)) {
            return;
        }
        if (!FramebufferImpl::ValidateReadFramebufferForCopy(caller)) return;
        CopyReadFramebufferIntoMipmapRegion(textureObject, uploadTarget, level, xoffset, /*yoffset=*/0,
                                            /*zoffset=*/0, x, y, width, /*height=*/1, caller);
    }

    void CopyTexSubImage3D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x,
                                 GLint y, GLsizei width, GLsizei height) {
        // GL 4.6 core 8.6 table: the three-dimensional form of the bound-texture copy accepts
        // exactly TEXTURE_3D, TEXTURE_2D_ARRAY and TEXTURE_CUBE_MAP_ARRAY. A cube map's faces are
        // two-dimensional targets of their own and go through glCopyTexSubImage2D.
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (textureTarget != TextureTarget::Texture3D && textureTarget != TextureTarget::Texture2DArray &&
            textureTarget != TextureTarget::TextureCubeMapArray) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "glCopyTexSubImage3D requires GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY or "
                                             "GL_TEXTURE_CUBE_MAP_ARRAY."));
            return;
        }
        const auto textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
        if (!textureObject) return;
        CopyTextureSubImage3DResolved(textureObject, level, xoffset, yoffset, zoffset, x, y, width, height,
                                      /*allowCubeFaceFromZOffset=*/false, __func__);
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
        MGP_FILL(CopyTexSubImage2D);
        MG_Backend::gBackendFunctionsTable.GL.CopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
    }

    static void CopyTextureSubImage2D_Backend(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                              GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                                              GLsizei width, GLsizei height) {
        MGP_FILL(CopyTextureSubImage2D);
        MG_Backend::gBackendFunctionsTable.GL.CopyTextureSubImage2D(
            textureObject, MG_Util::ConvertTextureTargetToGLEnum(textureObject->GetTarget()), level, xoffset, yoffset,
            x, y, width, height);
    }

    void CopyImageSubData_Backend(const MG_Backend::CopyImageEndpoint& src, GLenum srcTarget, GLint srcLevel,
                                  GLint srcX, GLint srcY, GLint srcZ, const MG_Backend::CopyImageEndpoint& dst,
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
        MGP_FILL(CopyImageSubData);
        copyImageSubData(src, srcTarget, srcLevel, srcX, srcY, srcZ, dst, dstTarget, dstLevel, dstX, dstY, dstZ,
                         srcWidth, srcHeight, srcDepth);
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
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateCopyImageSubData_State",
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
        Bool ValidateCopyImageObjectExists(const MG_Backend::CopyImageEndpoint& endpoint, const char* endpointName) {
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

        // The per-axis extent of one endpoint's image AS THIS ENTRY POINT ADDRESSES IT, which is
        // not always the level extent this frontend stores.
        //
        // GL 4.6 core 18.3.2 treats EVERY array texture as a stack of slices addressed by z, and
        // gives a 1D array an image height of 1. This frontend stores a 1D array the way
        // glTexImage2D(GL_TEXTURE_1D_ARRAY, w, layers) writes it instead - layers on y - so the
        // two views have to be told apart here. Measuring y against the LAYER count is what let
        // srcY = 14 on a 16-wide, 16-layer 1D array come back GL_NO_ERROR
        // (KHR-GL43.copy_image.exceeding_boundaries, the src_test_case y variants); the CTS is
        // unambiguous about the convention, forcing height = 1 for 1D and 1D_ARRAY and listing
        // 1D_ARRAY as multilayer.
        //
        // A CUBE MAP is the other target whose z bound is not the level extent: this frontend
        // keeps its six faces as six separate one-slice upload targets, so the level says 1 and
        // the real bound is 6. A cube-map ARRAY is one upload target whose depth already counts
        // layer-faces, and every remaining target is answered by the level extent verbatim.
        IntVec3 GetCopyImageEndpointRegionBounds(const MG_Backend::CopyImageEndpoint& endpoint,
                                                 const IntVec3& levelSize) {
            const TextureTarget target = (!endpoint.IsRenderbuffer() && endpoint.Texture)
                                             ? endpoint.Texture->GetTarget()
                                             : TextureTarget::Unknown;
            if (target == TextureTarget::TextureCubeMap) {
                return {levelSize.x(), levelSize.y(), 6};
            }
            if (target == TextureTarget::Texture1DArray) {
                return {levelSize.x(), 1, std::max(levelSize.y(), 1)};
            }
            return {levelSize.x(), levelSize.y(), std::max(levelSize.z(), 1)};
        }

        // GL 4.6 core 18.3.2 requires INVALID_VALUE when the region exceeds either image's
        // boundaries. The only bounds-shaped call this validator used to make was
        // ValidateCopyImageBlockAlignment, whose first line returns true for every UNCOMPRESSED
        // format - so no uncompressed copy was bounded at all, and the z extent could not be
        // bounded even in principle because srcZ/dstZ never reached the validator. Texture
        // endpoints were covered only by accident, through the ES driver's own error, which the
        // DirectGLES backend logs and swallows rather than reporting; a GL_RENDERBUFFER endpoint
        // got neither (KHR-GL43.copy_image.exceeding_boundaries).
        Bool ValidateCopyImageRegionBounds(const MG_Backend::CopyImageEndpoint& endpoint, const IntVec3& levelSize,
                                           GLint x, GLint y, GLint z, GLsizei width, GLsizei height, GLsizei depth,
                                           const char* endpointName) {
            // An extent this frontend does not know cannot bound anything, and guessing would
            // reject a copy GL allows. Every caller has already established that the level
            // exists and that the image is complete, so this is a belt-and-braces guard.
            if (levelSize.x() <= 0 || levelSize.y() <= 0) return true;
            const IntVec3 bounds = GetCopyImageEndpointRegionBounds(endpoint, levelSize);
            if (x >= 0 && y >= 0 && z >= 0 && static_cast<Int64>(x) + width <= bounds.x() &&
                static_cast<Int64>(y) + height <= bounds.y() && static_cast<Int64>(z) + depth <= bounds.z()) {
                return true;
            }
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateCopyImageSubData_State",
                    std::format("The {} region [{}, {}, {}] + [{} x {} x {}] does not fit inside the {} x {} x {} "
                                "image.",
                                endpointName, x, y, z, width, height, depth, bounds.x(), bounds.y(), bounds.z())));
            return false;
        }
    } // namespace

    Bool ValidateCopyImageSubData_State(const MG_Backend::CopyImageEndpoint& src, GLenum srcTarget, GLint srcLevel,
                                        GLint srcX, GLint srcY, GLint srcZ, const MG_Backend::CopyImageEndpoint& dst,
                                        GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                        GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        if (!ValidateCopyImageObjectExists(src, "source") || !ValidateCopyImageObjectExists(dst, "destination")) {
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
        if (!TextureImpl::ValidateTextureLevelNumber(srcLevel) || !TextureImpl::ValidateTextureLevelNumber(dstLevel)) {
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
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Copy dimensions must be non-negative."));
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
                    std::format("The two images have different sample counts ({} vs. {}).", srcSamples, dstSamples)));
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
        if (!TextureImpl::ValidateCopyImageBlockAlignment(srcBlock, srcX, srcY, srcWidth, srcHeight, srcLevelSize.x(),
                                                          srcLevelSize.y(), "source") ||
            !TextureImpl::ValidateCopyImageBlockAlignment(dstBlock, dstX, dstY, srcWidth, srcHeight, dstLevelSize.x(),
                                                          dstLevelSize.y(), "destination")) {
            return false;
        }
        // One region extent, measured against both images: GL 4.6 core 18.3.2 gives the copy a
        // single width/height/depth and requires it to fit in the source AND the destination.
        if (!ValidateCopyImageRegionBounds(src, srcLevelSize, srcX, srcY, srcZ, srcWidth, srcHeight, srcDepth,
                                           "source") ||
            !ValidateCopyImageRegionBounds(dst, dstLevelSize, dstX, dstY, dstZ, srcWidth, srcHeight, srcDepth,
                                           "destination")) {
            return false;
        }
        return true;
    }

    void CopyTexSubImage1D_State(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width) {
        // The bound-texture form of glCopyTextureSubImage1D. GL 4.6 core 8.6 accepts only
        // GL_TEXTURE_1D here.
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (textureTarget != TextureTarget::Texture1D) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                                     "glCopyTexSubImage1D requires GL_TEXTURE_1D."));
            return;
        }
        const auto textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        auto& textureObject = GetTextureObjectByTarget(textureUploadTarget, textureTarget);
        if (!textureObject) return;
        CopyTextureSubImage1DResolved(textureObject, level, xoffset, x, y, width, __func__);
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
        MGP_FILL(CopyTexImage2D);
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

    // Every CompressedTex*SubImage2D/3D form once its texture and image are resolved: a block-aligned
    // box of each slice's compressed image is replaced. With `cubeFaces` the image is a cube map's and
    // z addresses faces, as for TextureSubImage3D (GL 4.6 core 8.7 lets the by-name 3D form take one).
    //
    // A deviation from GL: the uncompressed texel shadow beside the compressed image is touched only
    // for ETC2/EAC, the one family with a decoder here. For the others what changes is the image
    // glGetCompressedTexImage hands back, not what the level samples as.
    static void CompressedTexSubImageObject(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                            TextureUploadTarget uploadTarget, Bool cubeFaces, GLint level,
                                            IntVec3 offset, IntVec3 size, GLenum format, GLsizei imageSize,
                                            const void* data, const char* caller) {
        // Zero block width doubles as "format is not a specific compressed format", the INVALID_ENUM
        // case - one lookup answers both questions.
        const auto compressedInfo = MG_Util::GetCompressedFormatInfo(format);
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(uploadTarget, level)) return;
        if (size.x() < 0 || size.y() < 0 || size.z() < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                                      "width, height and depth must be non-negative."));
            return;
        }
        if (compressedInfo.blockWidth == 0) {
            RecordUnsupportedCompressedFormat(caller);
            return;
        }
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        auto* mipmapObject = MG_State::GLState::AsMipmapTexture(textureObject.get());
        if (mipmapObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture storage is not mipmap-backed."));
            return;
        }
        if (cubeFaces && (offset.z() < 0 || size.z() > 6 - offset.z())) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "zoffset and depth must select cube faces."));
            return;
        }

        // Each image written: one image holding every slice, or one cube face per slice.
        const GLint imageCount = cubeFaces ? size.z() : 1;
        const auto imageAt = [&](GLint i) { return cubeFaces ? CubeFaceUploadTarget(offset.z() + i) : uploadTarget; };
        const Int blockWidth = static_cast<Int>(compressedInfo.blockWidth);
        const Int blockHeight = static_cast<Int>(compressedInfo.blockHeight);
        for (GLint i = 0; i < imageCount; ++i) {
            // GL 4.6 core 8.7: INVALID_OPERATION unless the image is stored in exactly this compressed
            // format, which is also what makes the block arithmetic below sound.
            if (mipmapObject->GetMipmapCompressedFormat(imageAt(i), static_cast<Uint>(level)) != format) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "format does not match the internal format of the texture image."));
                return;
            }
            const IntVec3 levelSize = mipmapObject->GetMipmapTexelSize(imageAt(i), static_cast<Uint>(level));
            const Int levelDepth = cubeFaces ? 1 : std::max(levelSize.z(), 1);
            const Int z = cubeFaces ? 0 : offset.z();
            const Int depth = cubeFaces ? 1 : size.z();
            // Subtractions rather than sums: offset + extent are application-supplied GLints, and a
            // signed overflow is undefined behaviour a compiler may resolve by assuming the check passes.
            if (offset.x() < 0 || offset.y() < 0 || z < 0 || size.x() > levelSize.x() - offset.x() ||
                size.y() > levelSize.y() - offset.y() || depth > levelDepth - z) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "The replaced region does not lie within the texture image."));
                return;
            }
            // GL 4.6 core 8.7 for block formats: the region starts on a block boundary and is a whole
            // number of blocks or runs to the image's edge. Every format here is 4x4x1, so each slice is
            // its own block grid and depth carries no alignment rule.
            const Bool alignedX = (offset.x() % blockWidth == 0) &&
                                  (size.x() % blockWidth == 0 || offset.x() + size.x() == levelSize.x());
            const Bool alignedY = (offset.y() % blockHeight == 0) &&
                                  (size.y() % blockHeight == 0 || offset.y() + size.y() == levelSize.y());
            if (!alignedX || !alignedY) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", caller,
                        "The replaced region is not aligned to the format's compressed blocks."));
                return;
            }
        }
        // Exactly the size the format and dimensions imply, which also keeps the copy below in bounds.
        const SizeT expectedImageSize = MG_Util::CalculateCompressedTextureImageSize(compressedInfo, size);
        if (imageSize < 0 || static_cast<SizeT>(imageSize) != expectedImageSize) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "imageSize does not match the compressed image size."));
            return;
        }

        if (!ValidateCompressedUnpackBufferSource(data, expectedImageSize, caller)) return;
        const void* compressedBytes = CompressedUnpackSource(data);
        if (expectedImageSize == 0) return; // a zero-sized region is a legal no-op
        if (compressedBytes == nullptr) {
            // No unpack buffer and a null client pointer: nothing to read. GL leaves this undefined
            // rather than erroring, and dereferencing it is never acceptable.
            MGLOG_D("%s: null data with no pixel unpack buffer bound, nothing to replace", caller);
            return;
        }

        // Once per process: the call is about to succeed, and what it does is narrower than what an
        // application may expect from it.
        static std::atomic<Bool> announcedNoCodec{false};
        if (!MG_Util::EtcDecoder::IsEtcFormat(format) && !announcedNoCodec.exchange(true)) {
            MGLOG_W("%s: the compressed blocks are stored verbatim and returned by glGetCompressedTexImage, but "
                    "there is no BC/ETC decoder here, so they do not reach the texels the level SAMPLES as. Upload "
                    "through glTexSubImage for that.",
                    caller);
        }

        const SizeT blockBytes = compressedInfo.blockByteSize;
        const SizeT regionBlocksX =
            (static_cast<SizeT>(size.x()) + compressedInfo.blockWidth - 1) / compressedInfo.blockWidth;
        const SizeT regionBlocksY =
            (static_cast<SizeT>(size.y()) + compressedInfo.blockHeight - 1) / compressedInfo.blockHeight;
        const SizeT regionRowBytes = regionBlocksX * blockBytes;
        const SizeT regionSliceBytes = regionRowBytes * regionBlocksY;
        const SizeT firstBlockX = static_cast<SizeT>(offset.x()) / compressedInfo.blockWidth;
        const SizeT firstBlockY = static_cast<SizeT>(offset.y()) / compressedInfo.blockHeight;
        const auto* source = static_cast<const Uint8*>(compressedBytes);
        for (GLint i = 0; i < imageCount; ++i) {
            const TextureUploadTarget image = imageAt(i);
            // The level's compressed image is one blob, so the box is patched into a copy of it and the
            // whole blob handed back. Not a hot path, and it keeps the storage API to two calls.
            const SizeT blobSize = mipmapObject->GetMipmapCompressedByteSize(image, static_cast<Uint>(level));
            const void* existing = mipmapObject->MapMipmapCompressedImage(image, static_cast<Uint>(level));
            if (blobSize == 0 || existing == nullptr) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "The texture level holds no compressed image to modify."));
                return;
            }
            Vector<Uint8> blob(blobSize);
            Memcpy(blob.data(), existing, blobSize);
            const IntVec3 levelSize = mipmapObject->GetMipmapTexelSize(image, static_cast<Uint>(level));
            const SizeT levelBlocksX =
                (static_cast<SizeT>(levelSize.x()) + compressedInfo.blockWidth - 1) / compressedInfo.blockWidth;
            const SizeT levelBlocksY =
                (static_cast<SizeT>(levelSize.y()) + compressedInfo.blockHeight - 1) / compressedInfo.blockHeight;
            const SizeT levelRowBytes = levelBlocksX * blockBytes;
            const SizeT levelSliceBytes = levelRowBytes * levelBlocksY;
            const GLint firstSlice = cubeFaces ? 0 : offset.z();
            const GLint sliceCount = cubeFaces ? 1 : size.z();
            const Uint8* imageSource = source + static_cast<SizeT>(i) * regionSliceBytes;
            for (GLint slice = 0; slice < sliceCount; ++slice) {
                const SizeT destSliceBase = static_cast<SizeT>(firstSlice + slice) * levelSliceBytes;
                for (SizeT row = 0; row < regionBlocksY; ++row) {
                    const SizeT destOffset =
                        destSliceBase + (firstBlockY + row) * levelRowBytes + firstBlockX * blockBytes;
                    if (destOffset + regionRowBytes > blobSize) break; // a level whose blob predates its size
                    Memcpy(blob.data() + destOffset,
                           imageSource + static_cast<SizeT>(slice) * regionSliceBytes + row * regionRowBytes,
                           regionRowBytes);
                }
            }
            mipmapObject->SetMipmapCompressedImage(image, static_cast<Uint>(level), format, blob.data(), blobSize);
            DecodeEtcBlocksIntoTexelShadow(mipmapObject, image, static_cast<Uint>(level), format, imageSource,
                                           {offset.x(), offset.y(), firstSlice}, {size.x(), size.y(), sliceCount});
        }
    }

    // The bound-target forms: a target outside the command's list, a proxy among them, is INVALID_ENUM.
    static void CompressedTexSubImage_State(GLenum target, std::initializer_list<TextureTarget> accepted, GLint level,
                                            IntVec3 offset, IntVec3 size, GLenum format, GLsizei imageSize,
                                            const void* data, const char* caller) {
        const TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        const TextureUploadTarget uploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
        if (uploadTarget == TextureUploadTarget::Unknown || TextureImpl::IsProxyTextureTarget(uploadTarget)) {
            RecordTargetError(TextureCallForm::Bound, caller,
                              "A proxy or unknown target has no texture image to modify.");
            return;
        }
        if (!ValidateCommandTarget(TextureCallForm::Bound, textureTarget, accepted, caller)) return;
        const auto& textureObject = GetTextureObjectByTarget(uploadTarget, textureTarget);
        CompressedTexSubImageObject(textureObject, uploadTarget, false, level, offset, size, format, imageSize, data,
                                    caller);
    }

    void CompressedTexSubImage3D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                       GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize,
                                       const void* data) {
        CompressedTexSubImage_State(
            target, {TextureTarget::Texture2DArray, TextureTarget::Texture3D, TextureTarget::TextureCubeMapArray},
            level, {xoffset, yoffset, zoffset}, {width, height, depth}, format, imageSize, data, __func__);
    }

    void CompressedTexSubImage2D_State(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                       GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
        CompressedTexSubImage_State(
            target, {TextureTarget::Texture2D, TextureTarget::Texture1DArray, TextureTarget::TextureCubeMap}, level,
            {xoffset, yoffset, 0}, {width, height, 1}, format, imageSize, data, __func__);
    }

    void CompressedTexSubImage1D_State(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format,
                                       GLsizei imageSize, const void* data) {
        // TODO: implement compressed upload - see CompressedTexImage2D_State.
        RecordUnsupportedCompressedFormat(__func__);
    }

    // The three-dimensional twin of CompressedTexImage2D_State, and the same deviation applies: the
    // blocks are shadowed verbatim for glGetCompressedTexImage while the texels this level SAMPLES
    // as stay zero - except ETC2/EAC, which is decoded into them. A 3D compressed image is a stack of
    // `depth` two-dimensional block grids - every format that reaches here has a 4x4x1 block - so
    // the blob layout is slice-major and CalculateCompressedTextureImageSize already multiplies by
    // depth.
    void CompressedTexImage3D_State(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                                    GLsizei depth, GLint border, GLsizei imageSize, const void* data) {
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
        if (!TextureImpl::ValidateCubeMapArrayShape(textureUploadTarget, width, height, depth, __func__)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, depth)) return;
        if (!TextureImpl::ValidateTextureBorderNumber(border)) return;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(textureUploadTarget, level)) return;
        if (compressedInfo.blockWidth == 0) {
            RecordUnsupportedCompressedFormat(__func__);
            return;
        }
        // GL 4.6 core 8.7: imageSize must be exactly the size the format and dimensions imply,
        // otherwise INVALID_VALUE. This is also the guard that keeps the copy below in bounds.
        const SizeT expectedImageSize =
            MG_Util::CalculateCompressedTextureImageSize(compressedInfo, {width, height, depth});
        if (imageSize < 0 || static_cast<SizeT>(imageSize) != expectedImageSize) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "imageSize does not match the compressed image size."));
            return;
        }

        // Object resolution copied from TexImage3D_State rather than routed through
        // GetTextureObjectByTarget, for the reason CompressedTexImage2D_State gives: a proxy target
        // is legal here and only CreateOrReplaceProxyTextureObject gives it an object to answer the
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
        const TextureInternalFormat textureInternalFormat =
            MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        textureObject->SetInternalFormat(textureInternalFormat);

        // A proxy records the format and nothing else - it must never take storage, and it must never
        // be tagged compressed, or GL_TEXTURE_COMPRESSED_IMAGE_SIZE on a proxy would stop being
        // INVALID_OPERATION.
        if (isProxy) return;

        const SizeT internalBpp =
            MG_Util::GetInternalBytesPerPixel(textureInternalFormat, TexturePixelDataType::UnsignedByte);
        const SizeT internalBytes =
            static_cast<SizeT>(width) * static_cast<SizeT>(height) * static_cast<SizeT>(depth) * internalBpp;

        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        DiscardMipmapChainOnBaseRespecification(textureMipmapObject, textureUploadTarget, level);
        // AllocateStorage clears any compressed image the level used to hold, so this must run before
        // SetMipmapCompressedImage re-arms it.
        textureMipmapObject->AllocateStorage(textureUploadTarget, level, {{width, height, depth}, internalBytes});

        if (!ValidateCompressedUnpackBufferSource(data, expectedImageSize, __func__)) return;
        const void* compressedBytes = CompressedUnpackSource(data);
        textureMipmapObject->SetMipmapCompressedImage(textureUploadTarget, level, internalformat, compressedBytes,
                                                      expectedImageSize);
        DecodeEtcBlocksIntoTexelShadow(textureMipmapObject, textureUploadTarget, static_cast<Uint>(level),
                                       internalformat, compressedBytes, {0, 0, 0}, {width, height, depth});
        textureMipmapObject->MarkStorageDirty(textureUploadTarget, level, true);
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
        // bytes-per-texel division and the pixel-store packer all rely on. ETC2/EAC blocks are
        // decoded into that storage (MG_Util::EtcDecoder); any other family samples as zeros, having
        // no decoder here. The application's bytes are kept beside it so glGetCompressedTexImage can
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
        DecodeEtcBlocksIntoTexelShadow(textureMipmapObject, textureUploadTarget, static_cast<Uint>(level),
                                       internalformat, compressedBytes, {0, 0, 0}, {width, height, 1});
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
            maxCombinedUnits =
                std::min(maxCombinedUnits,
                         MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxCombinedTextureImageUnits);
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
        MGP_FILL(GetTexImage);
        MG_Backend::gBackendFunctionsTable.GL.GetTexImage(target, level, format, type, pixels);
    }

    // Add to GL_Texture.cpp
    // The half of the GetTexImage/GetTextureImage error set (GL 4.6 core 8.11) that depends on the
    // resolved texture object rather than on how it was named. Shared because the by-name entry
    // point does not route through GetTexImage_State and so used to enforce none of it.
    // A cube map's six faces are six independent images, and both readback spellings name one of
    // them: glGetTexImage through the TARGET token, glGetTextureSubImage through zoffset. Both then
    // have to tell the size checks below that ONE image is coming back, not six.
    static Bool IsCubeMapFaceUploadTarget(TextureUploadTarget target) {
        return target >= TextureUploadTarget::CubeMapPositiveX && target <= TextureUploadTarget::CubeMapNegativeZ;
    }

    // `imagesQueried` is how many of the texture's upload-target images the query hands back, and
    // exists for the destination-size check at the bottom. Zero means "all of them", which is what
    // the whole-level forms return - every face of a cube map. glGetTextureSubImage naming ONE cube
    // face passes 1: sizing that request against six faces' worth would reject the only buffer a
    // single-face read has any reason to pass.
    Bool ValidateTextureImageQuery(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, GLint level,
                                   TextureInputFormat textureInputFormat, TexturePixelDataType texturePixelDataType,
                                   GLsizei bufSize, const void* pixels, const char* caller, SizeT imagesQueried = 0) {
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
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Texture target has no image to read back."));
            return false;
        }

        // Level range. The by-target path would reach these again inside
        // CopyTextureImageToClientOrPBO_State, but the by-name path on a backend that answers
        // GetTextureImage itself never gets there.
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return false;
        if (target == TextureTarget::TextureRectangle && level != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Level must be zero for rectangle textures"));
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
        // integer-ness). Also rejects a STENCIL_INDEX readback of anything but stencil-only
        // storage, which is the only pairing GL 4.4 / ARB_texture_stencil8 ever made legal.
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

            // Tightly packed, and summed over every face because a whole-level cube map query
            // returns all six - unless the caller named a single face, which is what a non-zero
            // imagesQueried says. Pack pixel-store state only ever grows this, so a request
            // rejected here could not have fit under any packing.
            const SizeT imageCount = imagesQueried != 0 ? imagesQueried : uploadTargets.size();
            const SizeT required =
                MG_Util::CalculateInputTextureImageSize(textureInputFormat, texturePixelDataType, texelSize) *
                imageCount;

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
        auto& textureObject = isProxy ? TextureImpl::pProxyTextureManager->GetProxyTextureObject(textureUploadTarget)
                                      : bindingSlot.GetBoundObject();

        // glGetTexImage has no bufSize argument: -1 stands for "no client-side limit". That skips
        // the destination-size branch but NOT the pixel-pack-buffer one, which measures the same
        // `required` against the bound PBO's real size - so a cube FACE query has to say it returns
        // one image here too, or a PBO sized for the one face this call packs is refused as too
        // small while the copy that follows writes exactly that much into it.
        return ValidateTextureImageQuery(textureObject, level, textureInputFormat, texturePixelDataType, -1, pixels,
                                         "GetTexImage_State", IsCubeMapFaceUploadTarget(textureUploadTarget) ? 1u : 0u);
    }

    // What this helper can and cannot answer.
    //
    // ProcessTexturePixelsDataPack performs NO format or type conversion: it sizes every texel with
    // GetInternalBytesPerPixel(the TEXTURE's internal format) and memcpys the shadow rows verbatim,
    // and it carries a standing TODO for the pixel-store parameters, so it honours only SwapBytes and
    // the bitmap LSBFirst path. Both facts are invisible from the outside, and both are dangerous:
    //
    //   * a (format, type) narrower than the shadow's own texel makes the copy write MORE bytes than
    //     the caller's buffer holds. glGetTexImage passes bufSize = -1 (it has no bufSize argument),
    //     so the size guard below is skipped and the Memcpy runs off the end of the application's
    //     allocation - reading an 8x8 GL_RGBA8 level as (GL_RED, GL_UNSIGNED_BYTE) writes 256 bytes
    //     into the 64 that GL 4.6 core 8.11 says are required. A wider (format, type) is not an
    //     overflow but is still wrong data.
    //   * a pack state that puts padding, a row-length override or a skip offset between rows is
    //     ignored outright, so the rows land at the wrong destination strides - while the GPU
    //     readback path (DirectGLES StoreClientRows, and DirectVulkan through it) honours all of it.
    //     Same glGetTexImage call, two different destination layouts, decided by whether the texture
    //     happens to have a GPU image.
    //
    // So the copy is only correct when the client layout IS the shadow layout and the destination
    // walk is tight. That is checked here rather than assumed, and a request outside it is refused
    // with an error instead of being answered wrongly. Refusing is a real narrowing of what GL
    // promises - the spec wants the conversion performed - but the alternative on this path is a
    // heap overflow, and the conversion belongs in the pack processor rather than in another
    // open-coded copy here.
    static Bool ValidateShadowReadbackLayout(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                             TextureInputFormat textureInputFormat,
                                             TexturePixelDataType texturePixelDataType, GLsizei width,
                                             const char* caller) {
        const SizeT shadowTexelSize =
            MG_Util::GetInternalBytesPerPixel(textureObject->GetFormat(), texturePixelDataType);
        const SizeT clientTexelSize = MG_Util::GetInputBytesPerPixel(textureInputFormat, texturePixelDataType);
        if (shadowTexelSize == 0 || clientTexelSize == 0 || shadowTexelSize != clientTexelSize) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("Reading this texture back needs a format/type conversion that the CPU-shadow "
                                "path cannot perform: the shadow texel is {} bytes and the requested one is {}.",
                                shadowTexelSize, clientTexelSize)));
            return false;
        }

        // A tight destination walk is the only one the pack processor produces. GL_PACK_ALIGNMENT
        // defaults to 4, so a row whose byte count is not already a multiple of it needs padding that
        // would never be written - no glPixelStorei call from the application is required to reach
        // this.
        const auto packParams = MG_State::pGLContext->GetPixelStoreParameters(false);
        const SizeT alignment = packParams.Alignment > 0 ? static_cast<SizeT>(packParams.Alignment) : 1;
        const SizeT rowBytes = static_cast<SizeT>(std::max<GLsizei>(width, 0)) * clientTexelSize;
        const Bool tightRows = (rowBytes % alignment) == 0;
        const Bool noOverrides = packParams.RowLength == 0 && packParams.ImageHeight == 0 &&
                                 packParams.SkipPixels == 0 && packParams.SkipRows == 0 && packParams.SkipImages == 0;
        if (!tightRows || !noOverrides) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    "The CPU-shadow readback path packs rows tightly and cannot honour a pixel-store state that "
                    "adds row padding, a row-length override or a skip offset."));
            return false;
        }
        return true;
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
        if (!ValidateShadowReadbackLayout(textureObject, textureInputFormat, texturePixelDataType, texelSize.x(),
                                          caller)) {
            return;
        }

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
                                            int dimension, GLsizei levels, GLsizei width, GLsizei height, GLsizei depth,
                                            const char* caller) {
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
            if (IsCompressedGLInternalFormat(internalformat)) {
                // After AllocateStorage, which clears the tag. See TexImage1D_State: no compressed
                // format has a 1D block layout, but glClearTexImage still has to refuse the request.
                textureMipmapObject->SetMipmapRequestedCompressedFormat(textureUploadTarget, static_cast<Uint>(level),
                                                                        internalformat);
            }
        }
        // Immutable storage defines exactly `levels` levels; AllocateStorage only grows, so a
        // longer pre-existing chain has to be dropped explicitly.
        textureMipmapObject->TruncateMipmapLevels(textureUploadTarget, static_cast<Uint>(levels));
        textureObject->SetImmutableLevels(static_cast<Uint>(levels));
        SeedImmutableViewState(textureObject, static_cast<Uint>(levels));
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
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Cube map immutable storage must be square."));
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
                // GL 4.6 core 8.19: for GL_TEXTURE_1D_ARRAY the state-side HEIGHT is the LAYER
                // COUNT, and layers do not halve down the mip chain - level i is
                // (max(1, width >> i), height). Shrinking it made every mipmapped 1D array
                // level report fewer layers than it has.
                const Bool heightIsLayerCount = textureObject->GetTarget() == TextureTarget::Texture1DArray;
                const GLsizei levelHeight = heightIsLayerCount ? height : std::max<GLsizei>(1, height >> level);
                const SizeT byteSize = static_cast<SizeT>(levelWidth) * static_cast<SizeT>(levelHeight) * bytesPerPixel;
                textureMipmapObject->AllocateStorage(uploadTarget, level, {{levelWidth, levelHeight, 1}, byteSize});
                textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                if (compressedInfo.blockWidth != 0) {
                    // After AllocateStorage, which clears the tag.
                    textureMipmapObject->SetMipmapCompressedImage(
                        uploadTarget, static_cast<Uint>(level), internalformat, nullptr,
                        MG_Util::CalculateCompressedTextureImageSize(compressedInfo, {levelWidth, levelHeight, 1}));
                }
                if (IsCompressedGLInternalFormat(internalformat)) {
                    // Also after AllocateStorage. The generic enums land here and nowhere above,
                    // and glClearTexImage has to refuse them too (GL 4.6 core 8.19).
                    textureMipmapObject->SetMipmapRequestedCompressedFormat(uploadTarget, static_cast<Uint>(level),
                                                                            internalformat);
                }
            }
            // See TextureStorage1D.
            textureMipmapObject->TruncateMipmapLevels(uploadTarget, static_cast<Uint>(levels));
        }
        textureObject->SetImmutableLevels(static_cast<Uint>(levels));
        SeedImmutableViewState(textureObject, static_cast<Uint>(levels));
    }

    void TextureStorage3D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height,
                          GLsizei depth) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        if (textureObject->GetTarget() == TextureTarget::Texture3D && IsCompressedGLInternalFormat(internalformat)) {
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
        const auto textureUploadTarget = GetPrimaryUploadTarget(textureObject);
        // The cube-array shape rules, shared with glTexImage3D / glCompressedTexImage3D so the
        // three cannot drift (they had: this check used to exist here and nowhere else).
        if (!TextureImpl::ValidateCubeMapArrayShape(textureUploadTarget, width, height, depth, __func__)) return;

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
            const SizeT byteSize =
                ComputeTextureStorageByteSize(textureInternalFormat, levelWidth, levelHeight, levelDepth);
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
            if (IsCompressedGLInternalFormat(internalformat)) {
                // Also after AllocateStorage. The generic enums land here and nowhere above,
                // and glClearTexImage has to refuse them too (GL 4.6 core 8.19).
                textureMipmapObject->SetMipmapRequestedCompressedFormat(textureUploadTarget, static_cast<Uint>(level),
                                                                        internalformat);
            }
        }
        // See TextureStorage1D.
        textureMipmapObject->TruncateMipmapLevels(textureUploadTarget, static_cast<Uint>(levels));
        textureObject->SetImmutableLevels(static_cast<Uint>(levels));
        SeedImmutableViewState(textureObject, static_cast<Uint>(levels));
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

    // glTextureStorage2DMultisample/3DMultisample: the named texture's one image, frozen the way
    // glTexStorage*Multisample freezes it (GL 4.6 core 8.19).
    static void TextureStorageMultisample(GLuint texture, TextureTarget acceptedTarget, GLsizei samples,
                                          GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth,
                                          GLboolean fixedsamplelocations, const char* caller) {
        const auto& textureObject = GetTextureObjectByName(texture, caller);
        if (!ValidateNamedMultisampleStorage(textureObject, acceptedTarget, samples, width, height, depth, caller))
            return;
        const TextureInternalFormat format =
            ValidateMultisampleImage(acceptedTarget, samples, internalformat, width, height, depth, caller);
        if (format == TextureInternalFormat::Unknown) return;
        if (!AllocateMultisampleImage(textureObject, GetPrimaryUploadTarget(textureObject), format, samples, width,
                                      height, depth, fixedsamplelocations, caller))
            return;
        textureObject->SetImmutableLevels(1);
        SeedImmutableViewState(textureObject, 1);
    }

    void TextureStorage2DMultisample(GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLboolean fixedsamplelocations) {
        TextureStorageMultisample(texture, TextureTarget::Texture2DMultisample, samples, internalformat, width, height,
                                  1, fixedsamplelocations, __func__);
    }

    void TextureStorage3DMultisample(GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
        TextureStorageMultisample(texture, TextureTarget::Texture2DMultisampleArray, samples, internalformat, width,
                                  height, depth, fixedsamplelocations, __func__);
    }

    namespace {
        void RecordTextureViewError(ErrorCode code, const String& message) {
            MG_State::pGLContext->RecordError(code,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "TextureView", message));
        }

        // The internalformat the view-compatibility rule has to compare against, which is NOT
        // always ConvertTextureInternalFormatToGLEnum(GetFormat()): MobileGL answers every
        // compressed request with uncompressed storage and only remembers the requested enum on
        // the side, so a BPTC parent would otherwise present itself as RGBA8 and admit an RGBA8
        // view that table 8.21 forbids.
        GLenum ResolveTextureViewSourceFormat(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject) {
            const auto* mipmapTexture = MG_State::GLState::AsMipmapTexture(textureObject.get());
            if (mipmapTexture != nullptr && !textureObject->GetUploadTargets().empty()) {
                const TextureUploadTarget uploadTarget = textureObject->GetUploadTargets()[0];
                const GLenum stored = mipmapTexture->GetMipmapCompressedFormat(uploadTarget, 0);
                if (stored != GL_NONE) return stored;
                const GLenum requested = mipmapTexture->GetMipmapRequestedCompressedFormat(uploadTarget, 0);
                if (requested != GL_NONE) return requested;
            }
            return MG_Util::ConvertTextureInternalFormatToGLEnum(textureObject->GetFormat());
        }

        Bool BackendSupportsTextureViews() {
            const auto& activeBackendObject = MG_Backend::pActiveBackendObject;
            if (!activeBackendObject) return false;
            // Deliberately the ADVERTISED extension list rather than a separate capability bit:
            // it makes "MobileGL claims GL_ARB_texture_view" and "glTextureView actually works"
            // the same fact by construction. DirectVulkan always advertises it; DirectGLES only
            // does when the driver has EXT/OES_texture_view, because ES cannot otherwise give two
            // texture names one storage (see the no-EXT discussion in BackendObject_DirectGLES).
            const auto& extensions = activeBackendObject->GetRendererInfo().RendererGLInfo.Extensions;
            return std::find(extensions.begin(), extensions.end(), E_GL_ARB_texture_view) != extensions.end();
        }
    } // namespace

    // glTextureView - ARB_texture_view, core since GL 4.3 (GL 4.6 core 8.18).
    //
    // Creates a texture whose STORAGE is another texture's, optionally reinterpreting the format
    // and narrowing the level/layer range. The error list below is the spec's, in the order the
    // conformance suite (KHR-GL43.texture_view.errors, cases a..s) walks it.
    void TextureView(GLuint texture, GLenum target, GLuint origtexture, GLenum internalformat, GLuint minlevel,
                     GLuint numlevels, GLuint minlayer, GLuint numlayers) {
        if (!BackendSupportsTextureViews()) {
            // The honest answer when the backend cannot share one storage between two texture
            // names. Raising an error - and withholding the GL_ARB_texture_view string - is the
            // only alternative to a silent no-op that leaves the view with no storage at all,
            // which is indistinguishable from success at the call site and renders garbage.
            MGLOG_W_ONCE("glTextureView: the active backend has no texture-view support "
                         "(GL_EXT_texture_view / GL_OES_texture_view absent); raising GL_INVALID_OPERATION");
            RecordTextureViewError(ErrorCode::InvalidOperation, "The active backend does not support texture views.");
            return;
        }

        const TextureTarget viewTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (!TextureImpl::ValidateTextureTarget(viewTarget)) return;

        // a) <texture> is 0.
        if (texture == 0) {
            RecordTextureViewError(ErrorCode::InvalidValue, "texture must not be zero.");
            return;
        }
        // b) <texture> is not a name returned by glGenTextures.
        if (!MG_State::pGLContext->ValidateTextureName(texture)) {
            RecordTextureViewError(ErrorCode::InvalidOperation,
                                   std::format("texture {} is not a name returned by glGenTextures.", texture));
            return;
        }
        // c) <texture> has already been bound and given a target. A name that any bind (or
        // glCreateTextures, or an earlier glTextureView) has instantiated owns a texture object;
        // only a still-uninstantiated reservation may become a view.
        if (MG_State::pGLContext->ValidateTextureObject(texture)) {
            RecordTextureViewError(ErrorCode::InvalidOperation,
                                   std::format("texture {} has already been bound and given a target.", texture));
            return;
        }
        // d) <origtexture> is not the name of a texture object. Note the error code differs from
        // (b): INVALID_VALUE here, INVALID_OPERATION there.
        auto origTextureObject = MG_State::pGLContext->GetTextureObject(origtexture);
        if (origtexture == 0 || !origTextureObject) {
            RecordTextureViewError(ErrorCode::InvalidValue,
                                   std::format("origtexture {} is not the name of a texture object.", origtexture));
            return;
        }
        // e) <origtexture> is a mutable texture object. A view aliases storage that can never be
        // respecified underneath it, so only immutable storage qualifies.
        if (!origTextureObject->IsImmutable()) {
            RecordTextureViewError(ErrorCode::InvalidOperation,
                                   std::format("origtexture {} does not have immutable storage.", origtexture));
            return;
        }
        // f) target is incompatible with origtexture's target (table 8.20).
        const TextureTarget origTarget = origTextureObject->GetTarget();
        if (!TextureImpl::IsLegalTextureViewTargetPair(origTarget, viewTarget)) {
            RecordTextureViewError(
                ErrorCode::InvalidOperation,
                std::format("target {} is not a legal texture-view target for an origtexture whose target is {}.",
                            MG_Util::ConvertGLEnumToString(target),
                            MG_Util::ConvertGLEnumToString(MG_Util::ConvertTextureTargetToGLEnum(origTarget))));
            return;
        }
        // k)..q) the per-target <numlayers> constraints, all INVALID_VALUE.
        const Uint requiredLayers = TextureImpl::RequiredTextureViewLayerCount(viewTarget);
        if (requiredLayers != 0 && numlayers != requiredLayers) {
            RecordTextureViewError(ErrorCode::InvalidValue,
                                   std::format("target {} requires numlayers to be {}, but it is {}.",
                                               MG_Util::ConvertGLEnumToString(target), requiredLayers, numlayers));
            return;
        }
        if (viewTarget == TextureTarget::TextureCubeMapArray && (numlayers == 0 || numlayers % 6 != 0)) {
            RecordTextureViewError(
                ErrorCode::InvalidValue,
                std::format("GL_TEXTURE_CUBE_MAP_ARRAY requires numlayers to be a multiple of 6, but it is {}.",
                            numlayers));
            return;
        }
        // g)/h) the format-compatibility rule (table 8.21). A format WITH a view class may be
        // reinterpreted as any other format in the same class; a format with NO entry in the
        // table - every depth, stencil and depth/stencil format among them - may only ever be
        // viewed as itself, which is why the Better Clouds D24S8 view must name
        // GL_DEPTH24_STENCIL8 exactly.
        const GLenum origFormat = ResolveTextureViewSourceFormat(origTextureObject);
        const auto origViewClass = TextureImpl::GetTextureViewClass(origFormat);
        if (origViewClass == TextureImpl::TextureViewClass::None) {
            if (internalformat != origFormat) {
                RecordTextureViewError(
                    ErrorCode::InvalidOperation,
                    std::format("origtexture's internal format {} has no view class, so internalformat must be "
                                "identical to it, but it is {}.",
                                MG_Util::ConvertGLEnumToString(origFormat),
                                MG_Util::ConvertGLEnumToString(internalformat)));
                return;
            }
        } else if (TextureImpl::GetTextureViewClass(internalformat) != origViewClass) {
            RecordTextureViewError(
                ErrorCode::InvalidOperation,
                std::format("internalformat {} is not in the same view class as origtexture's internal format {}.",
                            MG_Util::ConvertGLEnumToString(internalformat),
                            MG_Util::ConvertGLEnumToString(origFormat)));
            return;
        }
        const TextureInternalFormat viewInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        if (!TextureImpl::ValidateTextureInternalFormat(viewInternalFormat)) return;

        // i)/j) the range checks, both against the ORIGINAL's view state rather than its raw
        // level/layer counts. On a plain immutable texture TexStorage* seeded those with the full
        // extent, so the two agree; on a view-of-a-view they are what bounds the child to the
        // parent's already-narrowed window.
        const Uint origNumLevels = origTextureObject->GetViewNumLevels();
        const Uint origNumLayers = origTextureObject->GetViewNumLayers();
        if (minlevel >= origNumLevels) {
            RecordTextureViewError(ErrorCode::InvalidValue,
                                   std::format("minlevel {} is larger than origtexture's greatest level {}.", minlevel,
                                               origNumLevels == 0 ? 0 : origNumLevels - 1));
            return;
        }
        if (minlayer >= origNumLayers) {
            RecordTextureViewError(ErrorCode::InvalidValue,
                                   std::format("minlayer {} is larger than origtexture's greatest layer {}.", minlayer,
                                               origNumLayers == 0 ? 0 : origNumLayers - 1));
            return;
        }
        // r)/s) a cube-map or cube-map-array view demands square levels, because its faces are
        // square by definition and the storage it borrows is not reshaped.
        if (viewTarget == TextureTarget::TextureCubeMap || viewTarget == TextureTarget::TextureCubeMapArray) {
            const IntVec3 baseSize = origTextureObject->GetBaseSize();
            if (baseSize.x() != baseSize.y()) {
                RecordTextureViewError(
                    ErrorCode::InvalidOperation,
                    std::format("a cube-map texture view requires origtexture's width and height to match, but "
                                "they are {}x{}.",
                                baseSize.x(), baseSize.y()));
                return;
            }
        }

        // GL 4.6 core 8.18, verbatim:
        //   TEXTURE_VIEW_MIN_LEVEL  = <minlevel> + origtexture's TEXTURE_VIEW_MIN_LEVEL
        //   TEXTURE_VIEW_NUM_LEVELS = min(<numlevels>, origtexture's TEXTURE_VIEW_NUM_LEVELS - <minlevel>)
        //   TEXTURE_VIEW_MIN_LAYER  = <minlayer> + origtexture's TEXTURE_VIEW_MIN_LAYER
        //   TEXTURE_VIEW_NUM_LAYERS = min(<numlayers>, origtexture's TEXTURE_VIEW_NUM_LAYERS - <minlayer>)
        // Because the offsets ADD all the way down, the composed values are already expressed in
        // the ROOT's coordinates - which is exactly what lets the view point straight at the root
        // and skip the chain.
        const auto& storageOwner =
            origTextureObject->IsTextureView() ? origTextureObject->GetViewStorageOwner() : origTextureObject;
        const Uint composedMinLevel = minlevel + origTextureObject->GetViewMinLevel();
        const Uint composedNumLevels = std::min(numlevels, origNumLevels - minlevel);
        const Uint composedMinLayer = minlayer + origTextureObject->GetViewMinLayer();
        const Uint composedNumLayers = std::min(numlayers, origNumLayers - minlayer);

        const auto& viewObject =
            MG_State::pGLContext->CreateTextureViewObject(texture, viewTarget, storageOwner, composedMinLevel,
                                                          composedNumLevels, composedMinLayer, composedNumLayers);
        if (!viewObject) {
            RecordTextureViewError(ErrorCode::InvalidOperation, "Failed to create the texture view object.");
            return;
        }
        viewObject->SetInternalFormat(viewInternalFormat);
        viewObject->SetSamples(storageOwner->GetSamples());
        viewObject->SetFixedSampleLocations(storageOwner->HasFixedSampleLocations());
        MG_Pipe::MGPipeEmitSamplerViewCreate(*viewObject);
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
        SeedImmutableViewState(textureObject, 1);
    }

    void TexStorage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height,
                                 GLboolean fixedsamplelocations) {
        const TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        if (!ValidateTextureMutable(activeUnit.GetBindingSlot(textureTarget).GetBoundObject(), __func__)) return;
        if (!ValidateTexStorageMultisampleSize(width, height, 1, __func__)) return;
        TexStorageMultisample_State(
            target, TexImage2DMultisample_State(target, samples, internalformat, width, height, fixedsamplelocations),
            __func__);
    }

    void TexStorage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height,
                                 GLsizei depth, GLboolean fixedsamplelocations) {
        const TextureTarget textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        if (!ValidateTextureMutable(activeUnit.GetBindingSlot(textureTarget).GetBoundObject(), __func__)) return;
        if (!ValidateTexStorageMultisampleSize(width, height, depth, __func__)) return;
        TexStorageMultisample_State(
            target,
            TexImage3DMultisample_State(target, samples, internalformat, width, height, depth, fixedsamplelocations),
            __func__);
    }

    // The by-name sub-image forms check the effective target against GL 4.6 core table 8.15.
    void TextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                           const void* pixels) {
        const auto& textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject || !ValidateCommandTarget(TextureCallForm::Named, textureObject->GetTarget(),
                                                     {TextureTarget::Texture1D}, __func__))
            return;
        TexSubImageObject(textureObject, GetPrimaryUploadTarget(textureObject), false, level, {xoffset, 0, 0},
                          {width, 1, 1}, format, type, pixels, __func__);
    }

    void TextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                           GLenum format, GLenum type, const void* pixels) {
        const auto& textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject ||
            !ValidateCommandTarget(
                TextureCallForm::Named, textureObject->GetTarget(),
                {TextureTarget::Texture2D, TextureTarget::Texture1DArray, TextureTarget::TextureRectangle}, __func__))
            return;
        TexSubImageObject(textureObject, GetPrimaryUploadTarget(textureObject), false, level, {xoffset, yoffset, 0},
                          {width, height, 1}, format, type, pixels, __func__);
    }

    // GL 4.6 core 8.6: only the by-name 3D form takes a cube map, as six layers; it must be cube
    // complete. A cube map array's completeness is not checked here, as before.
    void TextureSubImage3D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                           GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels) {
        const auto& textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        const TextureTarget target = textureObject->GetTarget();
        if (!ValidateCommandTarget(TextureCallForm::Named, target,
                                   {TextureTarget::Texture3D, TextureTarget::Texture2DArray,
                                    TextureTarget::TextureCubeMapArray, TextureTarget::TextureCubeMap},
                                   __func__))
            return;
        const Bool cubeFaces = target == TextureTarget::TextureCubeMap;
        if (cubeFaces && !textureObject->IsComplete()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "The cube map texture is not cube complete."));
            return;
        }
        TexSubImageObject(
            textureObject, cubeFaces ? TextureUploadTarget::CubeMapPositiveX : GetPrimaryUploadTarget(textureObject),
            cubeFaces, level, {xoffset, yoffset, zoffset}, {width, height, depth}, format, type, pixels, __func__);
    }

    // No compressed format here has a 1D form, so the call can only be refused.
    void CompressedTextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format,
                                     GLsizei imageSize, const void* data) {
        const auto& textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject || !ValidateCommandTarget(TextureCallForm::Named, textureObject->GetTarget(),
                                                     {TextureTarget::Texture1D}, __func__))
            return;
        RecordUnsupportedCompressedFormat(__func__);
    }

    void CompressedTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                     GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
        const auto& textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject ||
            !ValidateCommandTarget(TextureCallForm::Named, textureObject->GetTarget(),
                                   {TextureTarget::Texture2D, TextureTarget::Texture1DArray}, __func__))
            return;
        CompressedTexSubImageObject(textureObject, GetPrimaryUploadTarget(textureObject), false, level,
                                    {xoffset, yoffset, 0}, {width, height, 1}, format, imageSize, data, __func__);
    }

    void CompressedTextureSubImage3D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                     GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize,
                                     const void* data) {
        const auto& textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        const TextureTarget target = textureObject->GetTarget();
        if (!ValidateCommandTarget(TextureCallForm::Named, target,
                                   {TextureTarget::Texture2DArray, TextureTarget::Texture3D,
                                    TextureTarget::TextureCubeMapArray, TextureTarget::TextureCubeMap},
                                   __func__))
            return;
        const Bool cubeFaces = target == TextureTarget::TextureCubeMap;
        CompressedTexSubImageObject(
            textureObject, cubeFaces ? TextureUploadTarget::CubeMapPositiveX : GetPrimaryUploadTarget(textureObject),
            cubeFaces, level, {xoffset, yoffset, zoffset}, {width, height, depth}, format, imageSize, data, __func__);
    }

    void TextureParameteri(GLuint texture, GLenum pname, GLint param) {
        TextureParameterObject_State(NamedParameterTexture(texture, __func__), pname, param, TextureCallForm::Named,
                                     __func__);
    }

    void TextureParameterf(GLuint texture, GLenum pname, GLfloat param) {
        TextureParameterObjectf_State(NamedParameterTexture(texture, __func__), pname, param, TextureCallForm::Named,
                                      __func__);
    }

    void TextureParameterfv(GLuint texture, GLenum pname, const GLfloat* params) {
        const auto& textureObject = NamedParameterTexture(texture, __func__);
        if (textureObject && params)
            TextureParameterObjectfv_State(textureObject, pname, params, TextureCallForm::Named, __func__);
    }

    void TextureParameteriv(GLuint texture, GLenum pname, const GLint* params) {
        const auto& textureObject = NamedParameterTexture(texture, __func__);
        if (textureObject && params)
            TextureParameterObjectiv_State(textureObject, pname, params, TextureCallForm::Named, __func__);
    }

    void TextureParameterIiv(GLuint texture, GLenum pname, const GLint* params) {
        const auto& textureObject = NamedParameterTexture(texture, __func__);
        if (textureObject && params)
            TextureParameterObjectIiv_State(textureObject, pname, params, TextureCallForm::Named, __func__);
    }

    void TextureParameterIuiv(GLuint texture, GLenum pname, const GLuint* params) {
        const auto& textureObject = NamedParameterTexture(texture, __func__);
        if (textureObject && params)
            TextureParameterObjectIuiv_State(textureObject, pname, params, TextureCallForm::Named, __func__);
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

    // The half glGetTextureImage and glGetTextureSubImage share: which of the two readbacks answers,
    // for ONE named upload target. Factored out so the sub-image form can name a cube FACE - the
    // by-name spelling of the face token glGetTexImage takes - instead of re-deriving the target and
    // silently landing on the +X face the way the delegation it replaces did.
    static void GetTextureImageForUploadTarget(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                               TextureUploadTarget uploadTarget, GLint level, GLenum format,
                                               GLenum type, GLsizei bufSize, void* pixels, const char* caller) {
        MGP_FILL(GetTextureImage);
        MG_Backend::gBackendFunctionsTable.GL.GetTextureImage(textureObject, uploadTarget, level, format, type, bufSize,
                                                              pixels);
        return;
    }

    void GetTextureImage(GLuint texture, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        if (!ValidateTextureImageQuery(textureObject, level, MG_Util::ConvertGLEnumToTextureInputFormat(format),
                                       MG_Util::ConvertGLEnumToTexturePixelDataType(type), bufSize, pixels, __func__)) {
            return;
        }
        auto uploadTarget = GetPrimaryUploadTarget(textureObject);
        // Unlike glGetTexImage's face target, this DSA call reads all six faces.
        if (textureObject->GetTarget() == TextureTarget::TextureCubeMap) uploadTarget = TextureUploadTarget::Unknown;
        GetTextureImageForUploadTarget(textureObject, uploadTarget, level, format, type, bufSize, pixels, __func__);
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

    // GL 4.6 core 8.11.4: the region checks glGetTextureSubImage and glGetCompressedTextureSubImage
    // share, ahead of anything about the destination. On success `levelExtent` is the level's
    // (width, height, depth) in the sub-image's own coordinates - a 1D array's layers are its
    // height, a 2D array's or cube map array's layer(-face)s its depth, and a cube map's six faces
    // its depth too.
    static Bool ValidateTextureSubImageRegion(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                              GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                                              GLsizei height, GLsizei depth, IntVec3& levelExtent, const char* caller) {
        const auto recordError = [caller](ErrorCode code, const char* message) {
            MG_State::pGLContext->RecordError(code, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, message));
            return false;
        };
        const auto target = textureObject->GetTarget();
        if (textureObject->GetStorageType() != TextureStorageType::Mipmap ||
            target == TextureTarget::Texture2DMultisample || target == TextureTarget::Texture2DMultisampleArray) {
            return recordError(ErrorCode::InvalidOperation, "The effective target is a buffer or multisample texture.");
        }
        if (level < 0 || xoffset < 0 || yoffset < 0 || zoffset < 0 || width < 0 || height < 0 || depth < 0) {
            return recordError(ErrorCode::InvalidValue, "Texture sub-image range is invalid.");
        }
        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        if (static_cast<Uint>(level) >= textureMipmapObject->GetMipmapLevelCount()) {
            return recordError(ErrorCode::InvalidValue, "Texture level is out of range.");
        }
        if (target == TextureTarget::Texture1D && (yoffset != 0 || height != 1)) {
            return recordError(ErrorCode::InvalidValue, "A 1D texture needs yoffset 0 and height 1.");
        }
        if ((target == TextureTarget::Texture1D || target == TextureTarget::Texture1DArray ||
             target == TextureTarget::Texture2D || target == TextureTarget::TextureRectangle) &&
            (zoffset != 0 || depth != 1)) {
            return recordError(ErrorCode::InvalidValue, "This texture target needs zoffset 0 and depth 1.");
        }

        levelExtent =
            textureMipmapObject->GetMipmapTexelSize(GetPrimaryUploadTarget(textureObject), static_cast<Uint>(level));
        // A cube map's level is stored per face, so its own extent reads z = 1; the six faces are
        // what zoffset/depth address.
        if (target == TextureTarget::TextureCubeMap) levelExtent.z() = 6;
        if (static_cast<Int64>(xoffset) + width > levelExtent.x() ||
            static_cast<Int64>(yoffset) + height > levelExtent.y() ||
            static_cast<Int64>(zoffset) + depth > levelExtent.z()) {
            return recordError(ErrorCode::InvalidValue, "Texture sub-image region exceeds the texture level.");
        }
        return true;
    }

    void GetTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                            GLsizei height, GLsizei depth, GLenum format, GLenum type, GLsizei bufSize, void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        IntVec3 levelExtent;
        if (!ValidateTextureSubImageRegion(textureObject, level, xoffset, yoffset, zoffset, width, height, depth,
                                           levelExtent, __func__)) {
            return;
        }

        // A cube map answers one face (zoffset names it, the by-name spelling of glGetTexImage's
        // face token) or all six; any other run of faces would have to concatenate faces into the
        // destination, which is the same unimplemented multi-image packing the check below refuses.
        const Bool isCubeMap = textureObject->GetTarget() == TextureTarget::TextureCubeMap;
        const Bool isSingleCubeFaceRead = isCubeMap && depth == 1;
        const Bool isFullLevelRead = xoffset == 0 && yoffset == 0 && width == levelExtent.x() &&
                                     height == levelExtent.y() &&
                                     (isSingleCubeFaceRead || (zoffset == 0 && depth == levelExtent.z()));
        if (!isFullLevelRead) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Partial texture sub-image readback is not implemented yet."));
            return;
        }

        TextureUploadTarget readUploadTarget = GetPrimaryUploadTarget(textureObject);
        if (isSingleCubeFaceRead) {
            readUploadTarget =
                static_cast<TextureUploadTarget>(static_cast<Int>(TextureUploadTarget::CubeMapPositiveX) + zoffset);
        } else if (isCubeMap) {
            readUploadTarget = TextureUploadTarget::Unknown; // all six faces, as glGetTextureImage reads them
        }
        if (!ValidateTextureImageQuery(textureObject, level, MG_Util::ConvertGLEnumToTextureInputFormat(format),
                                       MG_Util::ConvertGLEnumToTexturePixelDataType(type), bufSize, pixels, __func__,
                                       isSingleCubeFaceRead ? 1u : 0u)) {
            return;
        }
        GetTextureImageForUploadTarget(textureObject, readUploadTarget, level, format, type, bufSize, pixels, __func__);
    }

    // GL 4.6 core 8.11.4. Like glGetCompressedTextureImage this answers out of the CPU shadow of the
    // compressed blocks (no backend was handed them), cropped to whole blocks: the region has to
    // start on a block boundary and cover whole blocks except where it reaches the level's edge.
    void GetCompressedTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                      GLsizei width, GLsizei height, GLsizei depth, GLsizei bufSize, void* pixels) {
        auto textureObject = GetTextureObjectByName(texture, __func__);
        if (!textureObject) return;
        IntVec3 levelExtent;
        if (!ValidateTextureSubImageRegion(textureObject, level, xoffset, yoffset, zoffset, width, height, depth,
                                           levelExtent, __func__)) {
            return;
        }

        // A cube map's faces are separate images; every other target keeps its layers in one.
        const Bool isCubeMap = textureObject->GetTarget() == TextureTarget::TextureCubeMap;
        const auto imageTarget = [&](GLint z) {
            return isCubeMap
                       ? static_cast<TextureUploadTarget>(static_cast<Int>(TextureUploadTarget::CubeMapPositiveX) + z)
                       : GetPrimaryUploadTarget(textureObject);
        };
        const GLenum compressedFormat = GetCompressedLevelFormat(textureObject, imageTarget(zoffset), level);
        if (compressedFormat == GL_NONE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Texture level is not stored in a compressed format."));
            return;
        }
        const auto blockInfo = MG_Util::GetCompressedFormatInfo(compressedFormat);
        const Int blockWidth = static_cast<Int>(blockInfo.blockWidth);
        const Int blockHeight = static_cast<Int>(blockInfo.blockHeight);
        if (blockWidth == 0 || blockHeight == 0) return;
        if (xoffset % blockWidth != 0 || yoffset % blockHeight != 0 ||
            (width % blockWidth != 0 && xoffset + width != levelExtent.x()) ||
            (height % blockHeight != 0 && yoffset + height != levelExtent.y())) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "The region is not aligned to the compressed block size."));
            return;
        }

        const auto blocksFor = [](Int extent, Int block) { return static_cast<SizeT>((extent + block - 1) / block); };
        const SizeT levelRowBlocks = blocksFor(levelExtent.x(), blockWidth);
        const SizeT levelImageBlocks = levelRowBlocks * blocksFor(levelExtent.y(), blockHeight);
        const SizeT rowBlocks = blocksFor(width, blockWidth);
        const SizeT rows = blocksFor(height, blockHeight);
        const SizeT rowBytes = rowBlocks * blockInfo.blockByteSize;
        Vector<Uint8> region(rowBytes * rows * static_cast<SizeT>(depth));

        const auto* textureMipmapObject = MG_State::GLState::AsMipmapTexture(textureObject.get());
        for (GLsizei z = 0; z < depth && rows > 0 && rowBytes > 0; ++z) {
            const auto target = imageTarget(zoffset + z);
            const auto* image = static_cast<const Uint8*>(
                textureMipmapObject->MapMipmapCompressedImage(target, static_cast<Uint>(level)));
            const SizeT imageSize = textureMipmapObject->GetMipmapCompressedByteSize(target, static_cast<Uint>(level));
            // Layers of one image are stored one after another, each a full level's worth of blocks.
            const SizeT layer = isCubeMap ? 0 : static_cast<SizeT>(zoffset + z);
            const SizeT firstBlock = layer * levelImageBlocks +
                                     static_cast<SizeT>(yoffset / blockHeight) * levelRowBlocks +
                                     static_cast<SizeT>(xoffset / blockWidth);
            const SizeT endByte = (firstBlock + (rows - 1) * levelRowBlocks) * blockInfo.blockByteSize + rowBytes;
            if (!image || endByte > imageSize) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "Texture image is not stored in a compressed format."));
                return;
            }
            for (SizeT row = 0; row < rows; ++row) {
                Memcpy(region.data() + (static_cast<SizeT>(z) * rows + row) * rowBytes,
                       image + (firstBlock + row * levelRowBlocks) * blockInfo.blockByteSize, rowBytes);
            }
        }
        WriteCompressedImageToClientOrPBO(region.data(), region.size(), bufSize, pixels, __func__);
    }

    void GetTextureParameteriv(GLuint texture, GLenum pname, GLint* params) {
        const auto& textureObject = NamedParameterTexture(texture, __func__);
        if (textureObject && params) GetTextureParameterObjectiv_State(textureObject, pname, params, __func__);
    }

    void GetTextureParameterfv(GLuint texture, GLenum pname, GLfloat* params) {
        const auto& textureObject = NamedParameterTexture(texture, __func__);
        if (textureObject && params) GetTextureParameterObjectfv_State(textureObject, pname, params, __func__);
    }

    void GetTextureParameterIiv(GLuint texture, GLenum pname, GLint* params) {
        const auto& textureObject = NamedParameterTexture(texture, __func__);
        if (textureObject && params) GetTextureParameterObjectIiv_State(textureObject, pname, params, __func__);
    }

    void GetTextureParameterIuiv(GLuint texture, GLenum pname, GLuint* params) {
        const auto& textureObject = NamedParameterTexture(texture, __func__);
        if (textureObject && params) GetTextureParameterObjectIuiv_State(textureObject, pname, params, __func__);
    }

    // By name there is no face to name, so a cube map answers for face zero, POSITIVE_X (GL 4.6 core
    // 8.11.3) - the first of its upload targets.
    static Bool QueryNamedTextureLevelParameter(GLuint texture, GLint level, GLenum pname, Int64& value,
                                                const char* caller) {
        const auto& textureObject = GetTextureObjectByName(texture, caller);
        if (!textureObject) return false;
        const TextureUploadTarget uploadTarget = GetPrimaryUploadTarget(textureObject);
        if (uploadTarget == TextureUploadTarget::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "The texture has no level parameters."));
            return false;
        }
        if (!TextureImpl::ValidateTextureLevelNumber(level)) return false;
        if (!TextureImpl::ValidateTextureLevelWithUploadTarget(uploadTarget, level)) return false;
        return QueryTextureLevelParameter(textureObject, uploadTarget, false, level, pname, value, caller);
    }

    void GetTextureLevelParameteriv(GLuint texture, GLint level, GLenum pname, GLint* params) {
        Int64 value = 0;
        if (QueryNamedTextureLevelParameter(texture, level, pname, value, __func__) && params)
            *params = static_cast<GLint>(value);
    }

    void GetTextureLevelParameterfv(GLuint texture, GLint level, GLenum pname, GLfloat* params) {
        Int64 value = 0;
        if (QueryNamedTextureLevelParameter(texture, level, pname, value, __func__) && params)
            *params = static_cast<GLfloat>(value);
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
        // AND THE IMAGE-UNIT MARK BESIDE IT. The line above moves the TEXTURE-unit high-water
        // mark, which is a different array: a reader that needs "the highest image unit ever
        // bound" cannot take it from there without either over-walking (a texture bind at unit
        // 31 with no image bound anywhere) or, worse, under-walking if that line ever moves. The
        // split client's per-draw writable-image sweep is that reader
        // (MG_Remote/Client/GpuWritePending.cpp). Push builds only, so the pull build's bytes do
        // not move (G1).
        MG_State::pGLContext->NoteImageUnitTouched(static_cast<Int>(unit));
        MGP_FILL(BindImageTexture);
        bindImageTexture(unit, texture, level, layered, layer, access, format);
    }

    // Both mipmap forms once the texture is resolved (GL 4.6 core 8.14.4). A cube map that is not cube
    // complete has no consistent set of faces to filter down, so generating its mipmaps is
    // INVALID_OPERATION; without that check the incomplete texture reached the backend, where
    // DirectVulkan asserts on it and takes the process down.
    static void GenerateMipmapObject(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                     TextureCallForm form, const char* caller) {
        const TextureTarget target = textureObject->GetTarget();
        if (!ValidateCommandTarget(form, target,
                                   {TextureTarget::Texture1D, TextureTarget::Texture2D, TextureTarget::Texture3D,
                                    TextureTarget::Texture1DArray, TextureTarget::Texture2DArray,
                                    TextureTarget::TextureCubeMap, TextureTarget::TextureCubeMapArray},
                                   caller))
            return;
        if ((target == TextureTarget::TextureCubeMap || target == TextureTarget::TextureCubeMapArray) &&
            !textureObject->IsComplete()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Mipmap generation requires a cube complete cube map texture."));
            return;
        }
        auto* mipmapTexture = dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        MOBILEGL_ASSERT(mipmapTexture != nullptr, "Mipmap generation requires mipmap texture storage.");
        EnsureGeneratedMipmapStorageAllocated(*mipmapTexture);
        GenerateMipmap_Backend(textureObject);
    }

    void GenerateMipmap(GLenum target) {
        const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (!TextureImpl::ValidateTextureTarget(textureTarget)) return;
        const auto& textureObject =
            MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit())
                .GetBindingSlot(textureTarget)
                .GetBoundObject();
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "GenerateMipmap requires a bound texture."));
            return;
        }
        GenerateMipmapObject(textureObject, TextureCallForm::Bound, __func__);
    }

    void GenerateTextureMipmap(GLuint texture) {
        const auto& textureObject = GetTextureObjectByName(texture, __func__);
        if (textureObject) GenerateMipmapObject(textureObject, TextureCallForm::Named, __func__);
    }

    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
        if (!GetTexImage_State(target, level, format, type, pixels)) return;
        GetTexImage_Backend(target, level, format, type, pixels);
        return;
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
        MG_Util::TextureFormatProcessor::NormalizePixelFormat(preferredInternalFormat,
                                                              PixelFormatNormalizeOptionBit::None,
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
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
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

        // GL 4.6 core 14.3.1.1: a sample of the DRAW framebuffer, which has GL_SAMPLES of them - so
        // a single-sample framebuffer has none to ask about.
        const GLint samples = ResolveDrawFramebufferSampleCount();
        if (static_cast<Int64>(index) >= samples) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Sample index is not below the draw framebuffer's GL_SAMPLES."));
            return;
        }

        // The backend publishes where its device really places the samples of each count (a
        // shader's gl_SamplePosition reads the same places). A count it could not determine keeps
        // answering the pixel centre.
        Float position[2] = {0.5f, 0.5f};
        if (MG_Backend::pActiveBackendObject != nullptr) {
            MG_Backend::pActiveBackendObject->GetDynamicParameters().GetSamplePosition(samples, static_cast<Int>(index),
                                                                                       position);
        }
        val[0] = position[0];
        val[1] = position[1];
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

    // GL_OES_EGL_image. The only EGLImages with storage behind them are the server's shared images
    // (EGL_LINUX_DMA_BUF_EXT): level 0 of the bound texture is defined at the image's size through
    // glTexImage2D's own path - so the respecification is recorded and announced as for any other
    // - and the backend then swaps that storage for the image.
    //
    // GL_OES_EGL_image_external's target takes it the same way: an external texture is a 2D one
    // below the frontend. A YUV image (NV12/P010) is taken by either target; the backend converts it
    // into the texture's RGBA level at the first use in each frame (SharedImageYuv.h).
    void EGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image) {
        if (target != GL_TEXTURE_2D && target != GL_TEXTURE_EXTERNAL_OES) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "target must be GL_TEXTURE_2D or GL_TEXTURE_EXTERNAL_OES."));
            return;
        }
        const Bool external = target == GL_TEXTURE_EXTERNAL_OES;
        Uint64 sharedImageId = 0;
        EGLint width = 0;
        EGLint height = 0;
        if (!MG_Impl::EGLImpl::LookupSharedImage(static_cast<EGLImage>(image), &sharedImageId, &width, &height)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "image is not a valid EGLImage."));
            return;
        }
        auto& activeUnit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
        const SharedPtr<MG_State::GLState::ITextureObject> textureObject =
            activeUnit.GetBindingSlot(external ? TextureTarget::External : TextureTarget::Texture2D).GetBoundObject();
        if (!TextureImpl::ValidateTextureObject(textureObject)) return;
        if (!ValidateTextureMutable(textureObject, __func__)) return;
        if (!TextureImpl::ValidateTextureSizeRange(width, height, 1)) return;

        if (external) {
            // glTexImage2D does not take the external target, so its level 0 is defined here the way
            // that call defines a 2D one without data: an RGBA8 base, any older chain dropped.
            textureObject->SetInternalFormat(TextureInternalFormat::RGBA8);
            auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
            DiscardMipmapChainOnBaseRespecification(mipmapObject, TextureUploadTarget::Texture2D, 0);
            mipmapObject->AllocateStorage(TextureUploadTarget::Texture2D, 0,
                                          {{width, height, 1}, static_cast<SizeT>(width) * height * 4});
        } else {
            // NULL is an offset into a bound unpack buffer; the definition must read nothing.
            GLint unpackBuffer = 0;
            GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
            if (unpackBuffer != 0) BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            TexImage2D_State(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            if (unpackBuffer != 0) BindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpackBuffer));
        }

        auto* client = MG_Backend::ClientHooksOf(MG_Backend::pActiveBackendObject.get());
        if (client == nullptr || !client->AttachSharedImageToTexture(textureObject->GetLifetimeId(), sharedImageId)) {
            MGLOG_E_ONCE("glEGLImageTargetTexture2DOES: shared image %llu could not be bound to texture %u",
                         static_cast<unsigned long long>(sharedImageId), textureObject->GetExternalIndex());
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "the image could not be bound."));
            return;
        }
        MG_Impl::EGLImpl::NoteSharedImageBoundToTexture();
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
        // The TARGET-taking form owes GL_INVALID_ENUM for a target that is not GL_TEXTURE_BUFFER,
        // where the name-taking DSA forms below owe GL_INVALID_OPERATION for the corresponding
        // "that texture is not a buffer texture". Same shared body, different gate.
        if (!ValidateBufferTextureTarget(target, __func__)) return;
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
        CopyTextureSubImage2D_Backend(textureObject, level, xoffset, yoffset, x, y, width, height);
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
        CopyTextureSubImage1DResolved(textureObject, level, xoffset, x, y, width, __func__);
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
        // A cube map addresses its faces as separate upload targets, so zoffset selects the target
        // rather than a slice within one; every other layered target keeps zoffset as the slice.
        CopyTextureSubImage3DResolved(textureObject, level, xoffset, yoffset, zoffset, x, y, width, height,
                                      /*allowCubeFaceFromZOffset=*/true, __func__);
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
        if (!ValidateCopyImageSubData_State(src, srcTarget, srcLevel, srcX, srcY, srcZ, dst, dstTarget, dstLevel, dstX,
                                            dstY, dstZ, srcWidth, srcHeight, srcDepth)) {
            return;
        }
        CopyImageSubData_Backend(src, srcTarget, srcLevel, srcX, srcY, srcZ, dst, dstTarget, dstLevel, dstX, dstY, dstZ,
                                 srcWidth, srcHeight, srcDepth);
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
