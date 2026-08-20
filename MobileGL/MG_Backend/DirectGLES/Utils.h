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

        // Stores packed 32-bit source words verbatim, with the same destination addressing, PACK
        // parameters and pixel-pack-buffer handling as StoreWideRowsToClient. For the sources whose
        // storage word already IS the client word (MG_Util::IsRawPackedPixelTransfer): routing those
        // through the wide float intermediate re-encodes them, and the RGB9_E5 encoder canonicalizes
        // the shared exponent, so glGetTexImage would answer with different bits than were stored.
        // `srcWords` holds sliceHeight * sliceCount tightly stacked rows of `width` 32-bit words.
        // False when `type` is not a 4-byte packed type.
        Bool StorePackedWordsToClient(const Uint8* srcWords, GLsizei width, GLsizei sliceHeight, GLsizei sliceCount,
                                      GLenum type, void* pixels, Bool applyPackImageParams);
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
        // SPIRV-Cross emits `#extension GL_EXT_texture_buffer : require` for every buffer-texture
        // sampler when it targets ESSL below 320, and offers no way to ask for the OES spelling.
        // On a driver that advertises only GL_OES_texture_buffer that directive is a compile
        // error, so the name is retargeted in the emitted source. A no-op on every other tier:
        // ES 3.2 needs no directive at all and an EXT driver already has the right one.
        String RetargetTextureBufferExtension(String glslCode,
                                              MG_External::GLESCapabilities::TextureBufferTier tier);
        // Adds `#extension GL_NV_image_formats : require` when the shader carries an image
        // format qualifier GLSL ES has no core spelling for. SPIRV-Cross prints the format and
        // asks for nothing, so the request has to be made here. `needed` is the caller's answer,
        // because only it knows which formats are in play AND whether the driver advertises the
        // extension - requesting an unadvertised extension is itself a compile error, so this is
        // never emitted speculatively. A no-op when not needed or already present.
        String RequestExtendedImageFormats(String glslCode, Bool needed);
        // Adds `#extension GL_OES_viewport_array : require` when the emitted ESSL names
        // gl_ViewportIndex. SPIRV-Cross prints that identifier and asks for nothing (unlike
        // gl_Layer, which it backs with GL_NV_viewport_array2 on ES) and ESSL has no core
        // spelling for it at any version, so the request has to be made here or the stage does
        // not compile - which loses the whole program, not just the multi-viewport routing.
        // `needed` is the caller's answer for the same reason as above: only it knows whether the
        // driver advertises the extension, and requesting an unadvertised one is itself a compile
        // error, so this is never emitted speculatively. A no-op when not needed or already
        // present.
        String RequestViewportArrayExtension(String glslCode, Bool needed);
        // Writes a format layout qualifier into the image declarations named in
        // `esslFormatByUniformName` that still have none. The completion half of the image-format
        // bake, and ONLY that: the SPIR-V pass (BakeImageFormatsPass) is what normally puts the
        // format in, but SPIRV-Cross throws rather than printing the formats it calls
        // desktop-only when it targets ESSL - r8ui among them, which is what the stencil half of
        // KHR-GL4x.packed_depth_stencil.stencil_texturing binds - and a throw loses the whole
        // stage. So those formats stay out of the module and are spelled here instead, on the
        // emitted text, where nothing can refuse them.
        //
        // Declarations that already carry a format are left exactly as they are, whoever wrote
        // it. Must run before RemoveLayoutBinding, which is where an image's layout qualifier
        // stops being safe to edit by hand.
        String BakeImageFormatQualifiers(String glslCode, const UnorderedMap<String, String>& esslFormatByUniformName);
        String RemoveLayoutBinding(const String& glslCode);
        // Prefix of the writeonly half a read+write image uniform is split into (see
        // SplitReadWriteImageUniforms); the suffix is the image's own name.
        constexpr const char* IMAGE_WRITE_ALIAS_PREFIX = "mg_imageWrite_";
        // ESSL refuses an image variable that carries a format qualifier other than r32f /
        // r32i / r32ui unless it also carries `readonly` or `writeonly` (GLSL ES 3.10 4.9 /
        // 3.20 4.10; glslang enforces it verbatim in ParseHelper.cpp's layoutObjectCheck).
        // SPIRV-Cross emits NEITHER for an image the shader both reads and writes: it
        // speculatively decorates every storage image NonWritable+NonReadable
        // (fixup_image_load_store_access), then OpImageRead clears NonReadable and
        // OpImageWrite clears NonWritable, and to_qualifiers_glsl only prints `readonly`
        // from NonWritable and `writeonly` from NonReadable. Desktop GLSL is happy with the
        // bare declaration, so the frontend raises no error and the illegal ESSL only shows
        // up as a device compile failure - and then as a silently no-op draw.
        //
        // Restores a legal declaration:
        //  * loaded only            -> add `readonly`
        //  * stored only            -> add `writeonly`
        //  * both                   -> emit TWO declarations on the same binding and of the
        //                              same type, `coherent readonly <name>` and `coherent
        //                              writeonly <IMAGE_WRITE_ALIAS_PREFIX><name>`, and point
        //                              every imageStore at the second one. Several image
        //                              variables may share an image unit as long as they have
        //                              the same type and format, which is exactly what the pair
        //                              is.
        //
        // The `coherent` on both halves of the pair is load-bearing, not decoration: GLSL only
        // guarantees a write through one image variable is visible to a read through a DIFFERENT
        // one when both are coherent, and the split is what makes a same-variable
        // read-after-write cross-variable. The single-declaration repairs above do not get it -
        // nothing aliases them.
        //
        // Budget note: the split DOUBLES the image-uniform count of the stage it fires in, so
        // a driver advertising a tight GL_MAX_{FRAGMENT,VERTEX,...}_IMAGE_UNIFORMS can turn a
        // shader that used to compile into a link failure. ES only guarantees 4 fragment image
        // uniforms, so a shader with more than half the limit in read+write images is the case
        // to watch.
        //
        // Runs on the transpiled ESSL, so it must see the bindings the frontend units were
        // already rewritten to and must run before those bindings are stripped - see the call
        // site in Managers.cpp.
        String SplitReadWriteImageUniforms(const String& glslCode);
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
        //
        // avoidExplicitLodBias leaves lookups that already carry an explicit LOD untouched,
        // so their constant level stays constant; only the implicit-LOD forms take the bias.
        // Off by default and only ever set on ANGLE + llvmpipe, where injecting the uniform
        // into a constant LOD crashes the driver (MOBILEGL_AVOID_EXPLICIT_LOD_BIAS).
        String EmulateTextureLodBias(const String& glslCode, Bool avoidExplicitLodBias = false);
    } // namespace PrgramImpl

    namespace Utils {
        void CheckGLESError();
        GLenum GetBindingQuery(GLenum target, bool isTexture);
    } // namespace Utils
} // namespace MobileGL::MG_Backend::DirectGLES
