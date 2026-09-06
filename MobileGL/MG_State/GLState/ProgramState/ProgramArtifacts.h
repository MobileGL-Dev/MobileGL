// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h> // String/Vector/Array/UnorderedMap/SharedPtr + GL enums. DEBT NOTE (MGPipeTypes.h:24-31 style):
                      // Includes.h:80-84 still pulls glslang and :59 spirv_cross_c.h; this header is glslang-free BY
                      // SYMBOL (what P7's `nm -D | grep glslang` measures), not by preprocessed text. A textually
                      // glslang-free closure needs MG_Util/Types.h split off Includes.h (Types.h:11 includes it
                      // back) - out of scope for P0.5.
#include <set>        // std::set<String> (LinkArtifacts); NOT provided by Includes.h on its own terms
// PURITY: no ShaderObject.h, no SpvcSession.h, nothing under MG_Util/ShaderTranspiler/, no Config.h,
// no MG_Backend/, no BufferState/. scripts/check_include_closure.py probe "artifacts-header" (ROADMAP P0.5;
// ARCHITECTURE.md:260) asserts that closure. the glslang scope token appears exactly twice below (B.0 D5: the two
// glslang-typed LinkArtifacts members moved verbatim) and the gate pins that count.

namespace MobileGL::MG_State::GLState {
    // Sentinel for a uniform location without global-UBO backing storage (should not
    // survive linking: GenerateBinary falls back to tail-allocated scratch storage).
    // Namespace scope so SpirvArtifacts::reservedNumSamplesOffset can default to it;
    // ProgramObject::kInvalidUniformOffset is defined from this one.
    inline constexpr Uint kInvalidUniformOffset = ~0u;

    // Everything the query surface ever asked a glslang TType, flattened. Twenty
    // predicates, no recursion: nothing post-link ever walks a struct, a type name or the
    // AST, so a POD covers the whole surface exactly.
    struct TypeFacts {
        Bool isArray = false;
        // A runtime-sized array (a storage block's unsized trailing member) is an array
        // that is NOT sized; GL_ARRAY_SIZE reports 0 for it.
        Bool isSizedArray = false;
        Bool isMatrix = false;
        Bool isVector = false;
        Bool isOpaque = false;
        Bool isTexture = false;
        Bool isImage = false;
        Bool isDouble = false;   // getBasicType() == EbtDouble
        Bool isVoid = false;     // getBasicType() == EbtVoid (hidden block members)
        Bool isBuffer = false;   // getQualifier().storage == EvqBuffer
        Bool isPatch = false;    // getQualifier().patch
        Bool hasIndex = false;   // getQualifier().hasIndex()
        Bool hasFormat = false;  // getQualifier().hasFormat()
        Int vectorSize = 0;
        Int matrixCols = 0;
        Int matrixRows = 0;
        Int layoutIndex = 0;     // getQualifier().layoutIndex
        Uint layoutFormat = 0;   // getQualifier().getFormat()
        // glslang TLayoutMatrix, widened. For a uniform this is already RESOLVED against
        // the owning block's qualifier, so the getUniformBlock() fallback the old
        // accessors carried is gone.
        Int layoutMatrix = 0;
        // glslang TBasicType, widened - ApplyUniformInitialValues and the typed
        // glGetUniform* paths compare against a handful of enumerators.
        Int basicType = 0;
    };

    // One glslang TObjectReflection, flattened. Used for uniforms, blocks, pipe inputs
    // and pipe outputs alike, because glslang reflects all four as TObjectReflection.
    struct ResourceReflection {
        String name;
        GLenum glDefineType = 0;
        Int offset = -1;
        // TObjectReflection::size, RAW. For a uniform prefer `arraySize` below, which is
        // the resolved GL_UNIFORM_SIZE answer.
        Int size = 0;
        // TObjectReflection::index - for a uniform, the TPROGRAM block index owning it
        // (-1 for a default-block one; translate with GlBlockIndexFromTProgram).
        Int index = -1;
        Int counterIndex = -1;
        Int arrayStride = 0;
        Int topLevelArraySize = 0;
        Int topLevelArrayStride = 0;
        Int binding = -1;
        Int location = -1; // layoutLocation()
        // EShLanguageMask of the stages that reference it; 0 means "declared but read by
        // nobody", which is what the dead-default-block-uniform filter tests.
        Uint32 stages = 0;
        // GL_UNIFORM_SIZE / GL_ARRAY_SIZE, already resolved through the
        // isSizedArray()/getOuterArraySize()/size fallback.
        GLint arraySize = 1;
        TypeFacts type;
    };

    using UniformReflection = ResourceReflection;
    using BlockReflection = ResourceReflection;
    using PipeInputReflection = ResourceReflection;
    using PipeOutputReflection = ResourceReflection;

    // Transform feedback (GL 3.0 core: glTransformFeedbackVaryings applies on
    // the NEXT link; the linked snapshot below is what draws and queries see).
    struct XfbVarying {
        String name;
        GLenum type = GL_FLOAT;
        GLint size = 1;           // array element count
        Uint32 bufferIndex = 0;   // capture buffer slot
        Uint32 offsetBytes = 0;   // offset within the capture buffer
        Uint32 byteSize = 0;      // bytes captured per vertex for this varying
        // Offset within the gap-free record a backend that cannot express the GL
        // layout captures into; see NeedsScatteredTransformFeedbackCapture.
        Uint32 packedOffsetBytes = 0;

        // GL 4.6 core 11.1.2.1 / 7.3.1.1: a member of an output interface block is
        // captured under "<block name>.<member>". `name` keeps that GL spelling (it is
        // what the interface queries and the ESSL backend's driver-side capture list
        // need, since SPIRV-Cross re-emits the block under its own type name), while
        // the three fields below carry what a SPIR-V backend needs instead: the
        // decoration target is the block's *instance* variable and the member index
        // inside it. blockMemberIndex < 0 means "not a block member".
        String blockInstanceName;
        String blockName;
        Int blockMemberIndex = -1;
        // Which element of an arrayed block member this capture names, -1 for "the
        // member as a whole". SPIR-V cannot decorate a single array element, so a
        // backend needs the element index to tell a full run from a partial one.
        Int blockMemberElement = -1;
    };

    // ---- P1: everything a link PRODUCES, in one movable block ----
    //
    // The membership rule is mechanical, not editorial: this is exactly the field list
    // ResetLinkArtifacts() clears (plus the four it forgot to - infoLog,
    // linkedFragDataLocation/Index and the geometry strip-capture pair - which are just
    // as much link output). Nothing else belongs here.
    //
    // Why a struct: once glLinkProgram runs on a worker (P1 stage 4) the worker writes
    // its OWN LinkArtifacts and the GL thread publishes it with a single move, instead
    // of thirty cross-thread field assignments. Until then this is a pure refactor.
    //
    // Access rule (invariant I5): the member below is private and reachable ONLY
    // through ProgramObject::Artifacts(), which calls EnsureLinkJoined() first. That is
    // what makes "every read of link output joins the pending link" a property the
    // compiler checks rather than a review item - a new reader cannot spell the field
    // without going through the gate. m_artifacts lives in ProgramObject and is private
    // there; the type being namespace-scope changes nothing about that gate.
    // ---- the owned mirror of glslang's reflection ----
    //
    // WHY THIS EXISTS. Every GL query about a linked program used to be answered by
    // asking the live glslang TProgram - program->getUniform(i).getType()->isMatrix()
    // and friends. That made the TProgram part of the program's PERMANENT state, which
    // in turn made the whole front end (parse + link) unskippable: the L1 shader
    // translation memo could hand back the SPIR-V but the reflection still had to be
    // rebuilt from a freshly parsed AST.
    //
    // These three tables are a snapshot of everything the query surface ever reads off
    // the TProgram, in PLAIN OWNED VALUES - no TType*, no TString, nothing pointing into
    // a glslang pool. Taken once at the tail of DoReflection (SnapshotGlslangReflection),
    // they are copyable, immutable after the link, and safe to memoize and share between
    // ProgramObjects and threads. Once they are filled, `program` is dead weight to
    // everything except DoReflection itself.
    //
    // INDEXED BY TPROGRAM INDEX, deliberately: that is the space uniformIndexInTProgram,
    // glUniformIndexToTProgram and tProgramUniformIndexToGl already speak, so every
    // accessor that used to call program->getUniform(i) indexes uniformReflection[i]
    // instead, unchanged in every other respect.

    struct LinkArtifacts {
        // Live only between LinkProgram() and the end of DoReflection. Everything after
        // that reads the owned mirror below; a link served from the L1 memo never
        // constructs one at all, so this is null for such a program and MUST NOT be
        // dereferenced outside DoReflection.
        SharedPtr<glslang::TProgram> program;

        // The owned reflection snapshot. Indexed by TProgram index; see the structs above.
        Vector<UniformReflection> uniformReflection;
        Vector<BlockReflection> blockReflection;
        Vector<PipeInputReflection> pipeInputReflection;
        Vector<PipeOutputReflection> pipeOutputReflection;
        // Program-level scalars glslang answers off the linked intermediates.
        // Whether the program's LAST stage is the fragment stage. A color number - and so a
        // color index - exists only there; a separable tess/geometry/vertex program's
        // outputs are varyings and must report -1 (KHR-GL43.program_interface_query.
        // separate-programs-tess-control).
        Bool lastStageIsFragment = false;
        Array<GLuint, 3> computeLocalSize{};
        // Replaces program->getUniformIndex(name). Maps the reflected name to its
        // TProgram uniform index.
        UnorderedMap<String, Int> uniformIndexByName;

        // Attributes (Vertex in)
        Vector<String> attribs;
        Vector<GLenum> attribTypes;

        // FragData (Frag out): the per-link snapshot of the explicit request maps.
        UnorderedMap<String, Uint> linkedFragDataLocation;
        UnorderedMap<String, Uint> linkedFragDataIndex;

        // GL-facing index spaces (see the translation helpers above): GL active-uniform
        // index <-> glslang TProgram uniform index, GL uniform-block index <-> TProgram
        // block index. -1 marks a TProgram entry GL does not expose (dead default-block
        // uniforms swept into MGL_GLOBAL_UBO by the relaxed parse, and that block itself).
        Vector<Int> glUniformIndexToTProgram;
        Vector<Int> tProgramUniformIndexToGl;
        Vector<Int> glBlockIndexToTProgram;
        Vector<Int> tProgramBlockIndexToGl;
        // GL_UNIFORM_BLOCK index space: ACTUAL uniform blocks only, a strict subsequence of
        // glBlockIndexToTProgram above.
        //
        // That list is the BLOCK space - everything the relaxed parse produced except
        // MGL_GLOBAL_UBO - and it is what the backends walk and what every block-keyed table
        // here (uniformBlockBinding, uniformBlockIndexByName, blockReflection ordering) is
        // indexed by. It is NOT the GL uniform-block list: MobileGL does not pass
        // EShReflectionSeparateBuffers to buildReflection, so glslang routes BUFFER blocks
        // through indexToUniformBlock too, and the list therefore also carries every shader
        // storage block and every synthesized gl_AtomicCounterBlock_N. GL 4.6 core 7.6 gives
        // those their own enumerations (GL_SHADER_STORAGE_BLOCK and
        // GL_ACTIVE_ATOMIC_COUNTER_BUFFERS respectively), and GL_ACTIVE_UNIFORM_BLOCKS /
        // glGetActiveUniformBlock*/glGetUniformBlockIndex must not see either.
        //
        // Kept as a SECOND space rather than filtering the first in place: DirectGLES assigns
        // one ESSL uniform-buffer binding point per entry of the block list as it walks it
        // (Managers.cpp CacheResourceLocations and the matching per-draw loop in
        // DirectGLES.cpp), so compacting that list would renumber every backend binding
        // point, and tProgramBlockIndexToGl[i] < 0 is what DoReflection and
        // BuildGlobalUboRouting read as "member of the synthesized global UBO".
        Vector<Int> glUniformBlockIndexToBlock; // GL uniform-block index -> block index
        Vector<Int> blockIndexToGlUniformBlock; // block index -> GL uniform-block index (-1)
        // Per-link merged snapshot of the layout(location = N) qualifiers the attached
        // shaders' default-block uniforms declared, as glslang recorded them at the point
        // its relaxed remap dropped them (the relaxed parse drops them from reflection; the
        // DoReflection assigner restores them from here).
        UnorderedMap<String, Int> linkedExplicitUniformLocations;
        // Per-link snapshot of the default-block uniform INITIALIZERS the attached shaders
        // declared ("uniform int i = 1;"). Desktop GLSL says that value is what the uniform
        // reads until the application overwrites it, and relinking restores it - but the
        // relaxed parse turns those uniforms into members of MGL_GLOBAL_UBO, where SPIR-V
        // cannot carry an initializer, so the value only survives as this side-channel.
        // Applied into the uniform shadow at the phase-B publish (ApplyUniformInitialValues).
        Vector<glslang::TIntermediate::TUniformInitializer> uniformInitialValues;
        UnorderedMap<String, Uint> uniformLocations;
        // ---- "written since link" (see MarkUniformWrittenAtLocation) ----
        // In LinkArtifacts deliberately: a link is exactly the event that retracts every
        // write (GL resets uniforms to their initial values), so living here means the set
        // is cleared by the same three paths that clear the rest of a link's output -
        // Link()'s whole-struct reset, ResetLinkArtifacts, and the publish's move - and no
        // fourth reset site can be forgotten. Empty (and never allocated) for a program
        // that never asked to be separable.
        Vector<Uint64> writtenUniformLocationBits;
        Vector<Uint64> writtenUniformIndexBits;
        Vector<Uint> writtenUniformIndices;
        // Ordered by location,
        // aka. uniformIndexInTProgram[loc] == "uniform index of TProgram at location `loc`"
        Vector<Int> uniformIndexInTProgram;
        // ditto. Will be set at glUniform1i
        Vector<Int> uniformSamplerOrImageUnitIndex;
        // Sampler/image layout(binding = N) initial texture/image units, captured by
        // TMglGlslIoResolver at mapIO's collect callback - the last point at which the
        // qualifier still says what the shader declared. An OUTPUT of the link, not an
        // input to it: nothing supplies this map, the resolver fills it.
        UnorderedMap<String, Uint> explicitOpaqueUniformBindings;

        // Ordered by uniform block index
        // index is DIFFERENT from binding!!!
        //
        // Let's define UniformBlockIndex == the order at glslang getUniformBlock()
        // aka `i = glGetUniformBlockIndex(prog, "BlockName")` implies:
        // `prog->getUniformBlock(i) == "BlockName"`
        // These stuff are present for GL semantics, not for backend inspection
        // These may change after-link (because GL spec decided to have `glUniformBlockBinding`)
        UnorderedMap<String, Uint> uniformBlockIndexByName;
        Vector<Int> uniformBlockBinding;
        // glShaderStorageBlockBinding overrides, keyed by GL block name. See
        // SetShaderStorageBlockBinding for why this one is by name and not by index.
        //
        // ALSO SEEDED AT LINK, by ProgramLinkTask::SeedDefaultStorageBlockBindings, with the
        // GL-mandated binding 0 for every storage block whose shader declared no
        // layout(binding = N). Those blocks have no other way to be told apart from a block
        // that declared one: glslang's IO mapper invents a binding and writes it into the
        // qualifier, so the reflection reports the invention. A seed is therefore "GL's
        // default binding for this block", and a later glShaderStorageBlockBinding simply
        // overwrites it - default and rebind travel one path.
        UnorderedMap<String, Int> shaderStorageBlockBinding;
        // Block type names of the storage blocks the program's shaders declared with NO
        // layout(binding = N). Input to the seeding above; filled during mapIO by
        // TMglGlslIoResolver, which is the last observer that can still tell a declared
        // binding from an invented one - and, unlike the per-shader lexer this replaced,
        // sees the declaration with its macros expanded.
        std::set<String> storageBlocksWithoutBinding;
        // The same list for UNIFORM blocks, and it is needed for the same reason: glslang's
        // auto-mapper assigns every uniform block a binding whether or not the shader asked
        // for one, so uniformBlockBinding below cannot tell "declared 1" from "invented 1".
        // GL 4.6 core 7.6.2 requires an unqualified block to report ZERO.
        std::set<String> uniformBlocksWithoutBinding;

        Uint activeUniformCount = 0;
        // This program's fragment stage read gl_NumSamples, so the source pipeline lowered it
        // onto the reserved default-block uniform (ShaderTranspiler::NUM_SAMPLES_UNIFORM_NAME)
        // and the draw path owes it the draw framebuffer's sample count before every draw.
        //
        // PHASE A on purpose, even though the byte offset it needs is phase-B output: the
        // gate has to be answerable without joining the SPIR-V job, or every draw of every
        // program would pay a join to discover it has nothing to write.
        Bool usesReservedNumSamples = false;
        Uint maxUniformLocation = 0;
        Int uniformNameMaxLength = 0;
        Int attribInNameMaxLength = 0;
        Int uniformBlockNameMaxLength = 0;

        String infoLog;
        Bool linkStatus = false;

        // Transform feedback: the linked snapshot (the request lives outside, on the
        // GL-thread-owned side).
        Vector<XfbVarying> xfbVaryings;
        // The glTransformFeedbackVaryings request list exactly as this link consumed it,
        // INCLUDING the gl_NextBuffer / gl_SkipComponentsN pseudo-varyings that
        // xfbVaryings deliberately drops (they steer the capture layout and must never
        // reach a backend's varying list). GL_TRANSFORM_FEEDBACK_VARYING enumerates the
        // full request, pseudo-varyings and all, so the interface query needs its own copy.
        Vector<String> xfbInterfaceNames;
        Vector<Uint32> xfbStrides;
        Vector<Uint32> gsStripTriangles;
        Bool gsStripCaptureFixup = false;
        GLenum gsInputPrimitive = GL_NONE;
        // GL_TESS_CONTROL_OUTPUT_VERTICES: the `layout(vertices = N) out` of the linked
        // tessellation control stage, or 0 when the program has none. Checked against
        // GL_MAX_PATCH_VERTICES at link (GL 4.6 core 11.2.1.1).
        Int tcsOutputVertices = 0;
        // The rest of the geometry stage's link properties, and the tessellation evaluation
        // stage's. Every one of these is a glGetProgramiv answer that had no source at all:
        // the query surface listed the geometry pnames only to fall through to
        // GL_INVALID_ENUM, and the GL_TESS_GEN_* pnames were not mentioned anywhere. They
        // come from the linked intermediates for the same reason gsInputPrimitive and
        // tcsOutputVertices do - glslang has already merged the compilation units' layout
        // qualifiers and diagnosed contradictions, so the linked program is the thing that
        // knows.
        GLenum gsOutputPrimitive = GL_NONE;
        Int gsMaxVertices = 0;
        Int gsInvocations = 0;
        // The tessellation evaluation stage's layout: GL_QUADS / GL_TRIANGLES / GL_ISOLINES,
        // GL_EQUAL / GL_FRACTIONAL_EVEN / GL_FRACTIONAL_ODD, GL_CW / GL_CCW, and point mode.
        GLenum tessGenMode = GL_NONE;
        GLenum tessGenSpacing = GL_NONE;
        GLenum tessGenVertexOrder = GL_NONE;
        Bool tessGenPointMode = false;
        GLenum xfbBufferMode = GL_INTERLEAVED_ATTRIBS;
        Int xfbVaryingNameMaxLength = 0;
        Bool xfbNeedsScatteredCapture = false;
        Uint32 xfbPackedStride = 0;
    };

    // ---- everything phase B of a link produces, in one movable block ----
    //
    // The membership rule is the same mechanical one LinkArtifacts uses: this is exactly
    // what ProgramSpirvTask writes, which is what makes moving it THE publish. It is
    // deliberately NOT part of LinkArtifacts, and that separation is what routes the five
    // readers of SPIR-V-derived data through their own join gate by compiler rather than
    // by review - m_spirv is private and Spirv() is the only spelling that reaches it.
    //
    // Why these three and nothing else: `generatedSpirv` has no GL-thread reader at all
    // (every consumer is a backend draw/prepare path), and `uniformOffsets` +
    // `globalUboScratch` are the ONLY things glUniform*/glGetUniform* need that are
    // derived from the OPTIMIZED SPIR-V rather than from glslang reflection - spirv-opt
    // runs in place and can delete a uniform, or the whole global UBO, so the offsets
    // cannot be lifted out of glslang's reflection instead.
    struct SpirvArtifacts {
        Vector<Vector<unsigned>> generatedSpirv;
        Bool enableSpirvValidation = false;
        // Byte offset of each uniform location inside globalUboScratch, or
        // kInvalidUniformOffset. Sized maxUniformLocation + 1 by the routing pass.
        Vector<Uint> uniformOffsets;
        Vector<Uint8> globalUboScratch;
        // Byte offset of the reserved gl_NumSamples stand-in inside globalUboScratch, or
        // kInvalidUniformOffset. Taken by NAME from the SPIR-V metadata rather than through
        // uniformOffsets, because the member has no GL location at all: the link task keeps
        // it out of the GL-visible uniform index space so no application can see or write it.
        Uint reservedNumSamplesOffset = kInvalidUniformOffset;
        // False for a program whose SPIR-V was never produced (phase B cancelled at
        // teardown or by a relink) or whose optimizer run failed. GL has no way to
        // retract a LINK_STATUS it already reported true, so such a program stays
        // "linked" and every reflection answer it has given stays correct - it is simply
        // not drawable, which the backends already express through their link-status
        // gates.
        Bool spirvStatus = false;
        // Whether these modules KEPT their 64-bit floats instead of being narrowed to 32
        // (ShaderTranspiler::DemoteFloat64Pass). Decided per PROGRAM, never per module - the
        // global UBO is one buffer all stages read, so two stages disagreeing about whether a
        // `uniform double` occupies 4 or 8 bytes would put every uniform after it at a
        // different offset in each. Recorded here rather than re-derived from the backend
        // because it is the layout THESE modules were built with: it is what the routing
        // table's offsets mean, and glUniform*d / glGetUniform*v have to write and read the
        // width the shader actually declares.
        Bool nativeFloat64 = false;
        // Whether gl_PointSize was demoted out of THESE modules' tessellation/geometry
        // stages into an ordinary varying (ShaderCompiler::
        // DemoteTessellationGeometryPointSizeForProgram) because the backend cannot host
        // the built-in there. Per PROGRAM by construction - a consumer whose producer
        // kept the built-in would read garbage - and recorded here rather than
        // re-derived because it cannot be: the rewrite's whole point is that the final
        // bytes no longer declare the capability that armed it. The backends read it to
        // respell a "gl_PointSize" transform-feedback capture as the carrier
        // (ShaderCompiler::POINT_SIZE_CAPTURE_CARRIER_NAME). The GL reflection surface
        // deliberately keeps answering "gl_PointSize": demotion happens after phase A,
        // so every query keeps the truthful GL spelling.
        Bool pointSizeDemoted = false;
    };

    // ---- the archive field tables (ARCHITECTURE.md:259): ONE table per type, serving both directions ----
    //
    // Visitor contract: v(const char* name, Field&) - Field is const when Self is const, so a
    // single table serves the serializer (const) and the deserializer (non-const); `Self`
    // deduces either. A visitor recurses into TypeFacts / ResourceReflection / XfbVarying by
    // calling VisitFields on the element it was handed; the tables never recurse themselves.
    //
    // Free constrained templates rather than members so the struct bodies above stay a verbatim
    // move. The sizeof trip wires below are what keep these tables honest: a member added to a
    // struct changes its size, trips the assertion, and the message sends the author here.
    template <class Self, class V>
        requires std::same_as<std::remove_const_t<Self>, TypeFacts>
    void VisitFields(Self& a, V&& v) {
        v("isArray", a.isArray);
        v("isSizedArray", a.isSizedArray);
        v("isMatrix", a.isMatrix);
        v("isVector", a.isVector);
        v("isOpaque", a.isOpaque);
        v("isTexture", a.isTexture);
        v("isImage", a.isImage);
        v("isDouble", a.isDouble);
        v("isVoid", a.isVoid);
        v("isBuffer", a.isBuffer);
        v("isPatch", a.isPatch);
        v("hasIndex", a.hasIndex);
        v("hasFormat", a.hasFormat);
        v("vectorSize", a.vectorSize);
        v("matrixCols", a.matrixCols);
        v("matrixRows", a.matrixRows);
        v("layoutIndex", a.layoutIndex);
        v("layoutFormat", a.layoutFormat);
        v("layoutMatrix", a.layoutMatrix);
        v("basicType", a.basicType);
    } // 20 fields

    template <class Self, class V>
        requires std::same_as<std::remove_const_t<Self>, ResourceReflection>
    void VisitFields(Self& a, V&& v) {
        v("name", a.name);
        v("glDefineType", a.glDefineType);
        v("offset", a.offset);
        v("size", a.size);
        v("index", a.index);
        v("counterIndex", a.counterIndex);
        v("arrayStride", a.arrayStride);
        v("topLevelArraySize", a.topLevelArraySize);
        v("topLevelArrayStride", a.topLevelArrayStride);
        v("binding", a.binding);
        v("location", a.location);
        v("stages", a.stages);
        v("arraySize", a.arraySize);
        v("type", a.type); // visited as a value; the visitor recurses with VisitFields(a.type, v) if it wants to
    } // 14 fields

    template <class Self, class V>
        requires std::same_as<std::remove_const_t<Self>, XfbVarying>
    void VisitFields(Self& a, V&& v) {
        v("name", a.name);
        v("type", a.type);
        v("size", a.size);
        v("bufferIndex", a.bufferIndex);
        v("offsetBytes", a.offsetBytes);
        v("byteSize", a.byteSize);
        v("packedOffsetBytes", a.packedOffsetBytes);
        v("blockInstanceName", a.blockInstanceName);
        v("blockName", a.blockName);
        v("blockMemberIndex", a.blockMemberIndex);
        v("blockMemberElement", a.blockMemberElement);
    } // 11 fields

    // Every member EXCEPT `program`: it is null for every archived instance by construction
    // (ProgramTranslationCache.h asserts that at insert) and must never be serialized - it is
    // the live glslang TProgram that only DoReflection may touch. 57 of the 58 members.
    template <class Self, class V>
        requires std::same_as<std::remove_const_t<Self>, LinkArtifacts>
    void VisitFields(Self& a, V&& v) {
        v("uniformReflection", a.uniformReflection);
        v("blockReflection", a.blockReflection);
        v("pipeInputReflection", a.pipeInputReflection);
        v("pipeOutputReflection", a.pipeOutputReflection);
        v("lastStageIsFragment", a.lastStageIsFragment);
        v("computeLocalSize", a.computeLocalSize);
        v("uniformIndexByName", a.uniformIndexByName);
        v("attribs", a.attribs);
        v("attribTypes", a.attribTypes);
        v("linkedFragDataLocation", a.linkedFragDataLocation);
        v("linkedFragDataIndex", a.linkedFragDataIndex);
        v("glUniformIndexToTProgram", a.glUniformIndexToTProgram);
        v("tProgramUniformIndexToGl", a.tProgramUniformIndexToGl);
        v("glBlockIndexToTProgram", a.glBlockIndexToTProgram);
        v("tProgramBlockIndexToGl", a.tProgramBlockIndexToGl);
        v("glUniformBlockIndexToBlock", a.glUniformBlockIndexToBlock);
        v("blockIndexToGlUniformBlock", a.blockIndexToGlUniformBlock);
        v("linkedExplicitUniformLocations", a.linkedExplicitUniformLocations);
        v("uniformInitialValues", a.uniformInitialValues);
        v("uniformLocations", a.uniformLocations);
        v("writtenUniformLocationBits", a.writtenUniformLocationBits);
        v("writtenUniformIndexBits", a.writtenUniformIndexBits);
        v("writtenUniformIndices", a.writtenUniformIndices);
        v("uniformIndexInTProgram", a.uniformIndexInTProgram);
        v("uniformSamplerOrImageUnitIndex", a.uniformSamplerOrImageUnitIndex);
        v("explicitOpaqueUniformBindings", a.explicitOpaqueUniformBindings);
        v("uniformBlockIndexByName", a.uniformBlockIndexByName);
        v("uniformBlockBinding", a.uniformBlockBinding);
        v("shaderStorageBlockBinding", a.shaderStorageBlockBinding);
        v("storageBlocksWithoutBinding", a.storageBlocksWithoutBinding);
        v("uniformBlocksWithoutBinding", a.uniformBlocksWithoutBinding);
        v("activeUniformCount", a.activeUniformCount);
        v("usesReservedNumSamples", a.usesReservedNumSamples);
        v("maxUniformLocation", a.maxUniformLocation);
        v("uniformNameMaxLength", a.uniformNameMaxLength);
        v("attribInNameMaxLength", a.attribInNameMaxLength);
        v("uniformBlockNameMaxLength", a.uniformBlockNameMaxLength);
        v("infoLog", a.infoLog);
        v("linkStatus", a.linkStatus);
        v("xfbVaryings", a.xfbVaryings);
        v("xfbInterfaceNames", a.xfbInterfaceNames);
        v("xfbStrides", a.xfbStrides);
        v("gsStripTriangles", a.gsStripTriangles);
        v("gsStripCaptureFixup", a.gsStripCaptureFixup);
        v("gsInputPrimitive", a.gsInputPrimitive);
        v("tcsOutputVertices", a.tcsOutputVertices);
        v("gsOutputPrimitive", a.gsOutputPrimitive);
        v("gsMaxVertices", a.gsMaxVertices);
        v("gsInvocations", a.gsInvocations);
        v("tessGenMode", a.tessGenMode);
        v("tessGenSpacing", a.tessGenSpacing);
        v("tessGenVertexOrder", a.tessGenVertexOrder);
        v("tessGenPointMode", a.tessGenPointMode);
        v("xfbBufferMode", a.xfbBufferMode);
        v("xfbVaryingNameMaxLength", a.xfbVaryingNameMaxLength);
        v("xfbNeedsScatteredCapture", a.xfbNeedsScatteredCapture);
        v("xfbPackedStride", a.xfbPackedStride);
    } // 57 fields (58 members minus `program`)

    template <class Self, class V>
        requires std::same_as<std::remove_const_t<Self>, SpirvArtifacts>
    void VisitFields(Self& a, V&& v) {
        v("generatedSpirv", a.generatedSpirv);
        v("enableSpirvValidation", a.enableSpirvValidation);
        v("uniformOffsets", a.uniformOffsets);
        v("globalUboScratch", a.globalUboScratch);
        v("reservedNumSamplesOffset", a.reservedNumSamplesOffset);
        v("spirvStatus", a.spirvStatus);
        v("nativeFloat64", a.nativeFloat64);
        v("pointSizeDemoted", a.pointSizeDemoted);
    } // 8 fields

    // ---- trip wires ----
    // TypeFacts is a POD on every ABI: 13 Bool + 3 bytes of padding + 7 x 4-byte scalars.
    static_assert(std::is_trivially_copyable_v<TypeFacts> && sizeof(TypeFacts) == 44,
                  "TypeFacts changed: add the field to VisitFields(TypeFacts) (and its serializer when one exists), then update this number");
    // The container-bearing structs have one size per standard library (std::string and
    // std::set differ between libstdc++ and libc++), so their numbers are pinned PER STL:
    // libstdc++ (the Linux CI toolchain) here, libc++ (the NDK) by the integrator, MSVC
    // unasserted. ProgramArtifactsTest records every sizeof as a ctest property on every
    // platform, which is where a new toolchain's numbers are read from.
#if defined(__GLIBCXX__) && !defined(_GLIBCXX_DEBUG) && (SIZE_MAX == UINT64_MAX)
#define MGL_RESOURCEREFLECTION_SIZE 128
#define MGL_XFBVARYING_SIZE 128
#define MGL_LINKARTIFACTS_SIZE 1056
#define MGL_SPIRVARTIFACTS_SIZE 88
#elif defined(_LIBCPP_VERSION) && (SIZE_MAX == UINT64_MAX) && defined(MGL_ARTIFACT_SIZES_LIBCXX_PINNED)
    // The integrator pins these from the NDK build (brief C.4); until then this branch is inert.
#endif
#ifdef MGL_LINKARTIFACTS_SIZE
    static_assert(sizeof(ResourceReflection) == MGL_RESOURCEREFLECTION_SIZE,
                  "ResourceReflection changed size: add the field to VisitFields(ResourceReflection) (and its serializer when one exists), then update this number");
    static_assert(sizeof(XfbVarying) == MGL_XFBVARYING_SIZE,
                  "XfbVarying changed size: add the field to VisitFields(XfbVarying) (and its serializer when one exists), then update this number");
    static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE,
                  "LinkArtifacts changed size: add the field to VisitFields(LinkArtifacts) (and its serializer when one exists), then update this number");
    static_assert(sizeof(SpirvArtifacts) == MGL_SPIRVARTIFACTS_SIZE,
                  "SpirvArtifacts changed size: add the field to VisitFields(SpirvArtifacts) (and its serializer when one exists), then update this number");
#endif
} // namespace MobileGL::MG_State::GLState
