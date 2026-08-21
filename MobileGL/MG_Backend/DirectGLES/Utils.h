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

        // The CHANNEL WIDENING an image-bindable texture's ES storage takes, so that a format
        // GLSL ES cannot spell as an image is carried by one it can.
        //
        // GL has forty image formats, GLSL ES core has thirteen, and no test device advertises
        // GL_NV_image_formats - so a shader declaring one of the other twenty-six has no legal
        // ESSL at all and glBindImageTexture rejects the narrow format outright for most of them
        // (GL_INVALID_VALUE for nineteen of twenty-six on Adreno, twenty-five on both Malis).
        // Seventeen have a core format of the SAME per-channel width and component type,
        // differing only in channel count, and in one of those the emulation is EXACT: GL already
        // defines an imageLoad from a narrower format as (r, 0, 0, 1) and an imageStore as
        // dropping the components the format does not have, so the carrier's surplus channels
        // hold values GL has already named. WidenImageFormatsPass pins them in the shader; this
        // is the storage half, and DirectGLES::TextureImpl::SyncImageTextureBinding the bind
        // half. All three ask WidenedCoreEsslImageFormat, so they cannot pick different carriers.
        //
        // Reports nothing (InternalFormat == GL_UNKNOWN_MGL) for a format that is core already,
        // for the nine with no exact carrier (r11f_g11f_b10f, rgb10_a2, rgb10_a2ui, rgba16, rg16,
        // r16, rgba16_snorm, rg16_snorm, r16_snorm - those keep the honest "no GLSL ES spelling"
        // diagnostic rather than a silent approximation), and on a driver that HAS
        // GL_NV_image_formats, where the shader keeps the declared format and no widening may
        // happen behind it.
        //
        // The widened triple REPLACES what GenerateTextureFormatInfo chose, including any
        // renderability substitution: an image that cannot be image-bound is useless whatever its
        // attachment behaviour, so the image constraint wins. In practice that only bites
        // RG8_SNORM/R8_SNORM on a driver without EXT_render_snorm, where the storage stays
        // signed-normalized instead of becoming the half float that fallback would have picked -
        // so an image-bound texture in one of those two formats is no longer attachable, and
        // glGetTexImage on it falls through to the CPU shadow, which a shader-side imageStore
        // does not update. Accepted deliberately: before the widening, an image binding in either
        // format was refused outright by every driver tested and the stage that declared it never
        // compiled at all, so nothing that works today is being given up.
        //
        // KNOWN GAP, for the same "all three layers move together" reason: a widened texture that
        // is ALSO an FBO colour attachment gains one to three writable channels, and a draw into
        // it can leave values in channels GL says are 0 and 1. Sampling and imageLoad are covered
        // (the swizzle composition in SyncTextureParamsToBackend and the shader-side mask), but a
        // glReadPixels/glGetTexImage that asks for more channels than the frontend format has
        // would see them. Closing it needs the per-draw-buffer colour mask the three-channel
        // widening already carries (FramebufferImpl::g_alphaWidenedDrawBufferMask) generalized
        // from "alpha" to a channel count, which is its own change.
        struct ImageBindableStorageWidening {
            GLenum InternalFormat = GL_UNKNOWN_MGL;
            GLenum Format = GL_UNKNOWN_MGL;
            GLenum Type = GL_UNKNOWN_MGL;
            // Channels the FRONTEND format has, i.e. how many of the carrier's four the client
            // data fills. The rest are uploaded as 0, and the fourth as the format's implied 1.
            Uint SourceChannels = 0;
            // Whether that implied 1 is the integer one or a saturated normalized field - the
            // transfer type cannot tell the two apart (GL_UNSIGNED_BYTE serves both RG8 and
            // RG8UI), so the carrier decides.
            Bool IntegerData = false;

            explicit operator Bool() const { return InternalFormat != GL_UNKNOWN_MGL; }
        };
        ImageBindableStorageWidening GetImageBindableStorageWidening(TextureInternalFormat internalFormat);
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
        // Prefix of the const element->unit-offset table RemapImageArrayElementUnits declares
        // next to a widened image array; the suffix is the array's own name.
        constexpr const char* IMAGE_UNIT_MAP_PREFIX = "mg_imageUnitMap_";
        // One image ARRAY whose elements the application pointed at units that are not
        // consecutive-from-element-zero.
        struct ImageArrayUnitPlan {
            String name;       // the array's name, exactly as the emitted ESSL declares it
            Vector<Int> units; // the frontend image unit element k has to reach
        };
        // Desktop GL lets an application give each element of an image array an ARBITRARY unit
        // (glUniform1i per element). ES has no such call at all - "ES image units come
        // exclusively from the layout(binding=N) qualifier" - and one declaration carries one
        // binding, so ESSL nails an array's elements to the CONSECUTIVE units N, N+1, N+2, ...
        // MobileGL used to stamp element [0]'s unit as the binding and let the rest fall where
        // they fell: KHR-GL4x.shader_image_load_store.advanced-sso-simple assigns 0,2,4,6 and
        // 1,3,5,7, so its two programs actually addressed 0,1,2,3 and 1,2,3,4 - one layer got the
        // wrong value and three were never written, with no GL error and no link log. The same
        // defect for SAMPLER arrays was fixed API-side (SubscriptUniformNameForElement); an image
        // array has no API side to fix.
        //
        // Repaired by WIDENING the array to cover every unit from the lowest it needs to the
        // highest, rebasing its binding on the lowest, and routing every subscript through a
        // `const highp int` table of per-element offsets. The elements in between are declared
        // and never accessed. A const array indexed by a constant expression IS one, so literal
        // subscripts stay literal; and a table lookup on a dynamically uniform index is itself
        // dynamically uniform, which is what GLSL ES 3.20 asks of an image array index - so this
        // works whether SPIRV-Cross unrolled the application's loop over the array or not. That
        // is the reason for the table rather than one scalar declaration per element: scalars
        // cannot be selected by a non-constant index at all.
        //
        // Declines - leaving the array exactly as it was, and naming it in `outDeclined` for the
        // caller to report - when the span will not fit in `stageImageUniformBudget`
        // (GL_MAX_<stage>_IMAGE_UNIFORMS; <= 0 means "cannot say", and then it is not enforced),
        // when the emitted extent disagrees with the reflection, or when the array is reached by
        // anything other than a subscript. Silence was the whole defect here, so a decline must
        // be audible.
        //
        // Must run AFTER RebindImageUniformsToFrontendUnits and BakeImageFormatQualifiers (both
        // key on the GL uniform name and on a binding already being stamped) and BEFORE
        // SplitReadWriteImageUniforms (so both halves of a split inherit the widened extent and
        // the rebased binding) and RemoveLayoutBinding (which is what preserves image bindings).
        // Like them, it is downstream of the L2 shader-translation memo, so the per-program units
        // it reads need no entry in BuildEsslTranslationKey.
        String RemapImageArrayElementUnits(const String& glslCode, const Vector<ImageArrayUnitPlan>& plans,
                                           Int stageImageUniformBudget, Vector<String>* outDeclined = nullptr);
        // The member list of a `gl_PerVertex { ... }` redeclaration in already-emitted ESSL -
        // the text between the braces, verbatim - or nullopt when the shader does not redeclare
        // the block in that direction. `input` selects the `in gl_PerVertex` form over the
        // `out` one.
        //
        // Exists so BuildPassthroughTessControlEssl can MIRROR the stages it has to sit between
        // rather than guess at them. Whether SPIRV-Cross redeclares the built-in block, and with
        // which members, depends on what the application's shader touched; a synthesized stage
        // that redeclares a different shape than its neighbours is an ES link error against a
        // program that has no other problem.
        std::optional<String> ExtractPerVertexBlockMembers(const String& essl, Bool input);
        // The pass-through tessellation control stage GL 4.6 core 11.2.2 describes: "the input
        // patch is passed through unmodified", the output patch has PATCH_VERTICES vertices, and
        // the levels come from the PATCH_DEFAULT_OUTER_LEVEL / PATCH_DEFAULT_INNER_LEVEL state.
        //
        // Desktop GL makes the control stage OPTIONAL. OpenGL ES 3.2 does not: it has no
        // PATCH_DEFAULT_*_LEVEL state at all (only glPatchParameteri, for PATCH_VERTICES) and
        // rejects a program that has an evaluation stage without a control stage - with an EMPTY
        // info log, verified on an Adreno 830 with no MobileGL in the process. MobileGL's own
        // frontend link succeeds, so the program reports GL_LINK_STATUS = TRUE, program 0 is
        // bound in its place, and every draw silently renders nothing.
        //
        // `inPerVertexMembers` / `outPerVertexMembers` are the member lists to redeclare gl_in
        // and gl_out with - normally taken from the neighbouring stages' own emitted ESSL via
        // ExtractPerVertexBlockMembers, and empty to leave the driver's built-in declaration
        // alone, which is what matching a neighbour that did not redeclare requires.
        //
        // All four outer levels and both inner levels are written unconditionally: writing a
        // level the evaluation stage's domain does not use is legal and ignored, and it saves
        // this from having to know the domain. They are literal 1.0 because that is the GL
        // default and glPatchParameterfv - their only setter - is a stub in this frontend
        // (MG_Impl/GLImpl/Exporting/Definitions.cpp). Implementing that entry point means making
        // the levels a parameter here AND part of what makes a built program stale, exactly as
        // PATCH_VERTICES already is; the two must move together, so they are named together.
        //
        // The same stage, for the same reason, that DirectVulkan synthesizes in
        // ProgramFactory::BuildPassthroughTessControlSource - Vulkan likewise requires both
        // tessellation stages. Kept as two generators rather than one because the two targets
        // disagree on everything but the algorithm: desktop GLSL 450 against ESSL, a fixed
        // gl_PerVertex shape that Vulkan matches structurally against a mirrored one, and a
        // VkShaderModule against a driver shader object.
        String BuildPassthroughTessControlEssl(Uint esslVersion, Uint patchVertices,
                                               const String& inPerVertexMembers,
                                               const String& outPerVertexMembers);
        // Prefix of the writeonly half a read+write image uniform is split into (see
        // SplitReadWriteImageUniforms); the suffix is the image's own (already stage-tagged) name.
        constexpr const char* IMAGE_WRITE_ALIAS_PREFIX = "mg_imageWrite_";
        // Stem of the per-stage name every image declaration SplitReadWriteImageUniforms rewrites
        // is renamed under; ImageStageAliasPrefix appends the stage tag and the separator.
        constexpr const char* IMAGE_STAGE_ALIAS_PREFIX = "mg_image";
        // "mg_imageVs_", "mg_imageFs_", "mg_imageCs_", ... - the prefix SplitReadWriteImageUniforms
        // renames a rewritten image declaration under, so that no two stages can end up declaring
        // the same image uniform name with different memory qualifiers. Exposed for the tests.
        String ImageStageAliasPrefix(GLenum shaderType);
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
        // Restores a legal declaration, and RENAMES it per stage while doing so:
        //  * loaded only            -> add `readonly`
        //  * stored only            -> add `writeonly`
        //  * both                   -> emit TWO declarations on the same binding and of the
        //                              same type, `coherent readonly <name>` and `coherent
        //                              writeonly <IMAGE_WRITE_ALIAS_PREFIX><name>`, point
        //                              every imageStore at the second one, and follow each of
        //                              those stores with `memoryBarrierImage();`. Several image
        //                              variables may share an image unit as long as they have
        //                              the same type and format, which is exactly what the pair
        //                              is.
        //
        // The rename (ImageStageAliasPrefix) is the other half of the repair and applies to all
        // three cases. The qualifier chosen above is a decision about ONE STAGE's accesses, and
        // GLSL requires a uniform declared in two stages to be declared identically - so a shader
        // that stores an image from the vertex stage and loads it from the fragment stage came out
        // of here `writeonly` in one and `readonly` in the other. Adreno merges the two same-named
        // declarations and silently drops the vertex-stage STORES: no GL error, no link log,
        // LINK_STATUS = 1, and the image still reads back its initial contents
        // (KHR-GL4x.shader_image_load_store.advanced-memory-dependentInvocation; a raw-ES probe
        // isolated the trigger to the same-name/mismatched-qualifier pair, and only when both
        // carry `coherent`). A per-stage name leaves no cross-stage variable to merge. Every
        // rewritten declaration is renamed, including the readonly half of a split pair; the
        // declarations this pass leaves untouched keep their names, and those are exactly the ones
        // that already agree across stages.
        //
        // The `coherent` on both halves of the pair is load-bearing, not decoration: GLSL only
        // guarantees a write through one image variable is visible to a read through a DIFFERENT
        // one when both are coherent, and the split is what makes a same-variable
        // read-after-write cross-variable. The single-declaration repairs above do not get it -
        // nothing aliases them.
        //
        // The barrier is the other half of the same problem, and coherent alone did not cover it:
        // visibility is not ORDER. Within one invocation the ES compiler sees a write to one
        // variable and a read of another it has no reason to believe alias, and is free to serve
        // the read from before the write - which is what advanced-memory-order's store/load/
        // compare loop measured on Adreno. memoryBarrierImage() orders exactly those two, is core
        // GLSL ES 3.10 in every stage, and is not an execution barrier, so it is legal in
        // non-uniform control flow. It costs something in a shader that stores to a read+write
        // image in a loop, which is why it is confined to the split pair.
        //
        // Budget note: the split DOUBLES the image-uniform count of the stage it fires in, so
        // a driver advertising a tight GL_MAX_{FRAGMENT,VERTEX,...}_IMAGE_UNIFORMS can turn a
        // shader that used to compile into a link failure. ES only guarantees 4 fragment image
        // uniforms, so a shader with more than half the limit in read+write images is the case
        // to watch.
        //
        // Runs on the transpiled ESSL, so it must see the bindings the frontend units were
        // already rewritten to and must run before those bindings are stripped - see the call
        // site in Managers.cpp. It is downstream of the L2 shader-translation memo (which stores
        // what SPIRV-Cross emitted, before any of these text passes), so `shaderType` steering the
        // names it mints needs no entry in BuildEsslTranslationKey.
        //
        // `outSplitCount`, when given, receives the number of declarations that were actually
        // doubled - i.e. exactly how many image uniforms this stage gained over what the
        // application declared. Zero for every shader but a handful, and the only number the
        // budget note above can be reported with.
        String SplitReadWriteImageUniforms(const String& glslCode, GLenum shaderType,
                                           Uint* outSplitCount = nullptr);
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
