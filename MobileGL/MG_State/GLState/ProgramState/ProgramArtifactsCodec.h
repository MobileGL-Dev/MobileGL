// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramArtifactsCodec.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "ProgramArtifacts.h"

// The reflection ARCHIVE's serializer (P4a, D-H2): create_shader_state's payload is per-stage
// SPIR-V plus LinkArtifacts + SpirvArtifacts, whole structs, and until now nothing could turn
// those into bytes. Every VisitFields comment in ProgramArtifacts.h said "(and its serializer
// when one exists)"; this is it.
//
// IT LIVES BESIDE THE HEADER RATHER THAN INSIDE IT, deliberately: ProgramArtifacts.h carries
// the check_include_closure.py "artifacts-header" probe, which pins that the header is
// glslang-free by symbol and reaches no ShaderObject, no SpvcSession, no Config and no
// MG_Backend. A codec inside it would have to be inspected against that probe on every edit;
// a codec beside it leaves the probe untouched, and this file is compiled only in a push
// build (the root CMakeLists.txt appends it inside `if (MOBILEGL_PIPE_PUSH)`).
//
// WHEN IT ACTUALLY RUNS, and the answer is "not on the monolith hot path at all". In monolith
// the archive does not travel: MGPProgramDesc's seven blob refs are declared with Size 0 -
// "this record does not declare its blob" - and MGPipeApplyCreateShaderState takes the two
// structs by pointer beside the record, so the applier reads the frontend's own archive and
// this codec is never called. The VERIFY build is where it is exercised, and it is exercised
// as LIVE CODE WITH A GATE rather than as dead code with a unit test: the applier serialises,
// deserialises and field-compares before storing, and a mismatch is
// Fatal{PipeVerifyDiffer, "program-archive"}. Under split, P5 is what makes it the transport's
// path.
//
// THE FORMAT, and every part of it is a refusal rather than a guess:
//   * a VERSION word first, and a wire-schema fingerprint second (v2), so member order,
//     type or count-layout drift is a MISMATCH before decoding. The local-only v1 form
//     retains its legacy native-size echo;
//   * length-prefixed everything - strings, vectors, maps, sets - with the count checked
//     against the bytes that remain before a single element is allocated, so a corrupt count
//     cannot turn into a four-billion-element resize;
//   * little-endian, which is asserted rather than assumed;
//   * and `LinkArtifacts::program` is NEVER visited. It is the live glslang TProgram, it is
//     null for every archived instance by construction, and VisitFields deliberately omits it
//     (57 of the 58 members). Decode leaves it null.
//
// P5e (pg), CONTRACT-P5E §5.5 "codec completeness": THE SKIPPED MEMBER IS NAMED, AND IT IS
// `LinkArtifacts::program`. The contract asks this package either to carry it or to prove the
// twin never reads it, and the proof is a closed grep rather than an argument: every read of
// that member in the tree is inside `ProgramLinkTask.cpp` (`DoReflection`, which is the only
// code that may dereference it at all) and the three sites that `reset()` it -
// `ProgramLinkTask.cpp:678,764,877` and `ProgramObject::ResetLinkArtifacts`. `MG_Backend/`
// contains NO read of it, in either backend; `ProgramTranslationCache` asserts it is null at
// insert. So the archive answers every reflection question the program twin asks, and the one
// member it does not carry is one no twin has ever asked for. It could not be carried in any
// case: it points into a glslang arena that no archived instance owns.
//
// P6.5: native STL object sizes never describe this field-wise byte stream. Disaggregated
// peers use v2's recursive wire-schema fingerprint; monolith verification retains v1.
namespace MobileGL::MG_State::GLState {

    // Bumped whenever the byte format changes in a way a previous reader would misread. A
    // reader that sees a different word REFUSES; it never tries to guess a layout.
#if MOBILEGL_BUILD_DISAGGREGATED
    inline constexpr Uint32 kProgramArtifactsCodecVersion = 2;
    // Derived from the actual VisitFields order/names, container element schemas and scalar
    // representations. No native container size/offset is included. Also checked in Hello.
    Uint64 ProgramArtifactsSchemaFingerprint();
#else
    inline constexpr Uint32 kProgramArtifactsCodecVersion = 1;
#endif

    // Appends the archive to `out` (which is not cleared, so a caller may frame it). Never
    // fails: everything it walks is owned plain data.
    void EncodeProgramArtifacts(const LinkArtifacts& link, const SpirvArtifacts& spirv,
                                Vector<Uint8>& out);

    // Replaces `link` and `spirv` with what `bytes` describes. Returns false - with both
    // outputs left in a defined, default state - for a truncated stream, a version mismatch, a
    // wire-schema mismatch (native-size echo for local v1), or trailing bytes. `link.program`
    // is always null on return.
    Bool DecodeProgramArtifacts(const Uint8* bytes, SizeT size, LinkArtifacts& link,
                                SpirvArtifacts& spirv);

    // ---- P5e (pg): the archive as the SERVER OWNS it -------------------------------------
    //
    // WHAT THE TWO STRUCTS ABOVE DO NOT CARRY, and why a package that makes the server answer
    // every reflection question out of them had to notice: THE STAGE OF EACH MODULE.
    // `SpirvArtifacts::generatedSpirv` is one module per SHADER OBJECT the link consumed, in
    // snapshot order, and the stage of each lives in `ProgramObject::m_linkedShaderSnapshot` -
    // GL-thread state, not an artifact. `MGPProgramDesc::StageMask` cannot stand in for it: it
    // is a bit SET, and GL lets two shader objects of the same stage be attached to one
    // program, so a list rebuilt from the mask would be shorter than `generatedSpirv` and the
    // backend's "linked stages and modules must agree" refusal would fire on a perfectly good
    // program.
    //
    // So the frame is [stage count][stage words][the codec's own bytes], written and read by
    // this package on both sides, and `ProgramArchive` is what a decode produces: the record's
    // OWN copy, with the lifetime of the record rather than of the staged run that carried it
    // (rule C). Stages travel as Uint32 rather than as `ShaderStage` so this header keeps its
    // include closure - `ProgramArtifacts.h` beside it may not reach `ShaderStage.h`.
    struct ProgramArchive {
        LinkArtifacts Link;
        SpirvArtifacts Spirv;
        // ShaderStage, one per Spirv.generatedSpirv entry, at the same index.
        Vector<Uint32> LinkedStages;
    };

    // The bound a decode refuses past: six is what MGPProgramDesc::Spirv[] can name and what
    // ProgramEmit.h's own truncation counter already watches, doubled so that a program this
    // stack can build is never refused here for a reason the emitter did not already count.
    inline constexpr SizeT kProgramArchiveMaxStages = 12;

    // Appends the framed archive to `out` (not cleared, so a caller may frame it further).
    // `linkedStages` must be index-aligned with `spirv.generatedSpirv`.
    void EncodeProgramArchive(const LinkArtifacts& link, const SpirvArtifacts& spirv,
                              const Vector<Uint32>& linkedStages, Vector<Uint8>& out);

    // Replaces `out` with what `bytes` describes. Same refusal terms as
    // DecodeProgramArtifacts, plus: a stage count past kProgramArchiveMaxStages, and a stage
    // count that does not match the decoded module count.
    Bool DecodeProgramArchive(const Uint8* bytes, SizeT size, ProgramArchive& out);

    // How many fields a type's VisitFields table actually visits. The codec walks exactly that
    // table, so this is what pins "the codec did not quietly grow an arm of its own" - most of
    // all for LinkArtifacts, whose 58th member is the live TProgram the table omits. It is a
    // runtime count rather than a static_assert because VisitFields needs an INSTANCE and
    // these structs carry strings, vectors and maps: none of them is a constant expression.
    // ProgramArtifactsCodecTest is where it is asserted.
    template <class T>
    inline SizeT ProgramArtifactsVisitedFieldCount() {
        T probe{};
        SizeT count = 0;
        VisitFields(probe, [&count](const char*, auto&) { ++count; });
        return count;
    }
} // namespace MobileGL::MG_State::GLState
