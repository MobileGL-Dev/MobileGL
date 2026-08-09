// MobileGL - MobileGL/MG_Backend/DirectGLES/Utils.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/Texture/TextureFormatProcessor.h>

namespace MobileGL::MG_Backend::DirectGLES {
    namespace DebugImpl {
        class ErrorLopper {
        public:
            static void Loop(const std::function<void(GLenum)>&);
            static void Clear();
            ErrorLopper();
            ~ErrorLopper();
        };

        class OpenGLScopeMarker {
        public:
            explicit OpenGLScopeMarker(const String& scopeName);
            ~OpenGLScopeMarker();
        };
    } // namespace DebugImpl

    namespace BufferImpl {} // namespace BufferImpl

    namespace VertexArrayImpl {
        GLenum GetBindingQuery(GLenum target, bool isTexture);
    } // namespace VertexArrayImpl

    namespace TextureImpl {
        // Whether images on this format-capability target can back a colour attachment, and so
        // need a colour-renderable storage format even when the frontend asked for a
        // three-channel one ES never renders to. Shared by the capability probe (which passes the
        // capabilities it has just queried, before the globals are published) and by the
        // allocation path (which reads the active backend's), so the format the cache was probed
        // with is always the format the image is created with.
        Bool TargetRequiresRenderableFormat(SizeT targetIndex);
        Flags<PixelFormatNormalizeOptionBit> GetRenderTargetNormalizeOptions(
            const MG_External::GLESCapabilities& capabilities, SizeT targetIndex);

        void GenerateTextureFormatInfo(TextureInternalFormat internalFormat, GLenum* outInternalFormat,
                                       GLenum* outFormat, GLenum* outType,
                                       TextureTarget target = TextureTarget::Unknown);
        void GenerateRenderbufferFormatInfo(TextureInternalFormat internalFormat, GLenum* outInternalFormat,
                                            GLenum* outFormat, GLenum* outType);
        Bool ShouldUseCaveatTextureFormat(TextureInternalFormat internalFormat, TextureTarget target);

        // True when the format the image is actually created with has an alpha channel the
        // frontend format does not (the three-channel colour-renderable widening). GL reads such
        // a channel back as 1.0, so any swizzle source of ALPHA has to be answered with ONE and
        // any readback of the image has to overwrite the alpha the draw happened to leave there.
        Bool BackendTextureFormatAddsAlpha(TextureInternalFormat internalFormat, TextureTarget target);
        Bool BackendRenderbufferFormatAddsAlpha(TextureInternalFormat internalFormat);
        Bool ShouldUseCaveatRenderbufferFormat(TextureInternalFormat internalFormat);
    } // namespace TextureImpl

    namespace FramebufferImpl {} // namespace FramebufferImpl

    // Pure CPU helpers of the client-format readback conversion (ReadPixels/GetTexImage repack a wide
    // RGBA(_INTEGER) read into the caller's (format, type) layout). Kept context-free so unit tests can
    // exercise the exact packing the GL CTS packed_pixels oracle compares against.
    namespace ReadbackImpl {
        struct ReadbackChannelMapping {
            Int sourceChannel[4]; // RGBA source channel feeding each destination component
            Int channelCount;     // destination component count
            Bool isInteger;
        };
        Bool GetReadbackChannelMapping(GLenum format, ReadbackChannelMapping& outMapping);

        // Byte size of one destination component of `type`; packed types report the packed word size.
        // 0 = type not supported by the conversion path.
        SizeT GetReadbackComponentSize(GLenum type);

        // Bit-field layout of a GL packed pixel type. width/shift are indexed in the client format's
        // component order (matching ReadbackChannelMapping); shift is the LSB position of the field in
        // the packed word: non-REV types pack the first component from the MSB, *_REV types from the
        // LSB (GL 3.3 table 3.6; field positions mirror the GL CTS glcPackedPixelsTests pack_* oracle).
        struct PackedReadbackLayout {
            Int fieldCount;     // format components stored in the packed word
            Int width[4];       // bit width of each component's field
            Int shift[4];       // LSB bit position of each component's field
            SizeT byteSize;     // packed word size in bytes (1, 2 or 4)
            Bool isFloatPacked; // 10F_11F_11F_REV / 5_9_9_9_REV: fields hold unsigned small floats
        };
        Bool GetPackedReadbackLayout(GLenum type, PackedReadbackLayout& out);

        // Unsigned small-float encoders (EXT_packed_float / EXT_texture_shared_exponent semantics).
        Uint32 EncodeFloatToUnsignedF11(Float value);
        Uint32 EncodeFloatToUnsignedF10(Float value);
        Uint32 EncodeSharedExponentRGB9E5(const Float rgb[3]);

        // Destination bytes per pixel for a (format mapping, type) readback pair; 0 when the pair is
        // not convertible (unknown type, packed field count != format component count, floating-point
        // or packed-float type with an integer format).
        SizeT GetReadbackDstPixelSize(const ReadbackChannelMapping& mapping, GLenum type);

        // Repacks one row of wide RGBA(_INTEGER) texels (4 components of wideType each) into the
        // client's (format, type) layout. src holds width * 4 * GetReadbackComponentSize(wideType)
        // bytes, dst receives width * GetReadbackDstPixelSize(mapping, type) bytes.
        void ConvertWideReadbackRow(const Uint8* src, Uint8* dst, SizeT width, GLenum wideType,
                                    const ReadbackChannelMapping& mapping, GLenum type);

        // Stores wide RGBA(_INTEGER) rows into the client pointer or the bound PACK pixel buffer,
        // honoring the client-side PACK pixel-store parameters (row length, alignment, skips,
        // swap-bytes, and - when applyPackImageParams - image height/skip images). Shared by the
        // DirectGLES and DirectVulkan readback conversion paths.
        Bool StoreWideRowsToClient(const Uint8* wide, GLenum wideType, GLsizei width, GLsizei sliceHeight,
                                   GLsizei sliceCount, const ReadbackChannelMapping& mapping, GLenum type,
                                   void* pixels, Bool applyPackImageParams);
    } // namespace ReadbackImpl

    namespace PrgramImpl {
        String ProcessOutColorLocations(const String& glslCode);
        String ForceSupporterOutput(const String& glslCode);
        String ClampNormFallbackOutputs(String glslCode, GLenum shaderType, Uint32 snormOutputMask,
                                        Uint32 unormOutputMask);
        String ForceFlatIntegerVaryings(const String& glslCode, GLenum shaderType);
        // Legacy GLSL's gl_FragColor is broadcast to every enabled draw buffer (GL 4.6
        // 15.2.3), but ShaderSourceProcessor lowers it to the single output mg_FragColor,
        // which only ever reaches draw buffer 0. Replicates it across `drawBufferCount`
        // outputs and copies the value into them at the end of main. A no-op for
        // drawBufferCount <= 1, i.e. for everything but a framebuffer that actually
        // enables several draw buffers, so the ordinary single-target shader is untouched.
        String BroadcastLegacyFragColor(String glslCode, GLenum shaderType, Uint drawBufferCount);
        String RemoveLayoutBinding(const String& glslCode);
        // Prefix of the per-sampler float uniform that carries GL_TEXTURE_LOD_BIAS into
        // the shader (see EmulateTextureLodBias); the suffix is the sampler's own name.
        constexpr const char* LOD_BIAS_UNIFORM_PREFIX = "mg_lodBias_";
        // ES has no per-texture/sampler LOD bias at all (GL_TEXTURE_LOD_BIAS is desktop
        // only; Vulkan spells it VkSamplerCreateInfo::mipLodBias), so it has to reach the
        // shader as a uniform and be folded into every lookup's level of detail. Declares
        // one `uniform highp float mg_lodBias_<sampler>;` per mip-capable sampler and adds
        // it to the bias / explicit-LOD argument of every lookup that takes one. Draws push
        // the bound texture's (or sampler object's) value into it; a shader whose samplers
        // all have a zero bias is therefore unaffected. Returns the source unchanged when
        // there is nothing to rewrite.
        String EmulateTextureLodBias(const String& glslCode);
    } // namespace PrgramImpl

    namespace Utils {
        void CheckGLESError();
        GLenum GetBindingQuery(GLenum target, bool isTexture);
    } // namespace Utils
} // namespace MobileGL::MG_Backend::DirectGLES
