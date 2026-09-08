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
//   * a VERSION word first, and a MGL_LINKARTIFACTS_SIZE echo second, so a struct that gained
//     a field and a codec that did not is a MISMATCH AT READ TIME rather than a silent
//     truncation that deserialises garbage into the tail of a reflection table;
//   * length-prefixed everything - strings, vectors, maps, sets - with the count checked
//     against the bytes that remain before a single element is allocated, so a corrupt count
//     cannot turn into a four-billion-element resize;
//   * little-endian, which is asserted rather than assumed;
//   * and `LinkArtifacts::program` is NEVER visited. It is the live glslang TProgram, it is
//     null for every archived instance by construction, and VisitFields deliberately omits it
//     (57 of the 58 members). Decode leaves it null.
namespace MobileGL::MG_State::GLState {

    // Bumped whenever the byte format changes in a way a previous reader would misread. A
    // reader that sees a different word REFUSES; it never tries to guess a layout.
    inline constexpr Uint32 kProgramArtifactsCodecVersion = 1;

    // Appends the archive to `out` (which is not cleared, so a caller may frame it). Never
    // fails: everything it walks is owned plain data.
    void EncodeProgramArtifacts(const LinkArtifacts& link, const SpirvArtifacts& spirv,
                                Vector<Uint8>& out);

    // Replaces `link` and `spirv` with what `bytes` describes. Returns false - with both
    // outputs left in a defined, default state - for a truncated stream, a version mismatch, a
    // struct-size mismatch, or trailing bytes the format does not account for. `link.program`
    // is always null on return.
    Bool DecodeProgramArtifacts(const Uint8* bytes, SizeT size, LinkArtifacts& link,
                                SpirvArtifacts& spirv);

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
