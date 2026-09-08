// MobileGL - MobileGL/MG_IntegrationTest/Harness/PipeSlotPeek.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The CLIENT slot allocator's occupancy, read from a scenario.
//
// It exists for one assertion, P3a's C-1: a frontend object that dies must return its
// MGPipeHandle slot WHATEVER BACKEND IS RUNNING. That question has no answer in the GL API -
// the leak it rules out is entirely inside the library, and it is invisible in pixels, in GL
// names and in `glGetError` - so the only honest observable is the allocator's own live count
// and high-water mark. Reading them is what makes the case fail on the backend it actually
// failed on (DirectVulkan, which installs no StateObjectDeathOps) rather than only on the one
// where a backend-owned free happened to exist.
//
// A separate translation unit for BackendCapsPeek.h's reason, verbatim: the scenario sources
// include the GL headers with prototypes and MobileGL's umbrella header is not meant to meet
// them in one file.

#pragma once

namespace MGITest {

    // Which client-side object kind to ask about. Mirrors MG_Pipe::MGPipeKind for exactly the
    // kinds a scenario has a reason to count, so that the enum does not travel through this
    // header and the GL headers together.
    enum class PipeSlotKind {
        Buffer,
        VertexElementsCso,
        // P4a's six (G8b). Every one of them is a kind the CLIENT mints and the client alone
        // frees (BRIEF-P4A.md D-I1: one death helper per kind, called from the frontend
        // object's own destructor, whatever backend is running), so every one of them can leak
        // the P3a C-1 way - and the leak is invisible in pixels, in GL names and in
        // glGetError, exactly as the VertexElementsCso one was.
        Texture,
        Renderbuffer,
        // Framebuffer has a HANDLE but no wire lifetime (D-I2): no create_*, no destroy row in
        // the catalogue, and its death helper does the notice and the free and emits nothing.
        // That makes the allocator the ONLY observable of its lifetime, so this row matters
        // more here than the others rather than less.
        Framebuffer,
        SamplerCso,
        SamplerViewCso,
        // ShaderCso covers BOTH the ordinary program slots and the program-pipeline COMPOSITES
        // minted out of the reserved high band (MGPipeHandles.h:86-107, D-H7). One kind, because
        // that is what the allocator has: the band is a second dense table inside the same kind
        // and LiveCount counts both.
        //
        // THE TWO SPACES' HIGH-WATER MARKS ARE NOT ONE NUMBER, and the correction matters here
        // more than anywhere else. c0b split them (contract-v2.md 4.3): HighWater(ShaderCso) is
        // now the ORDINARY space only and the band's own mark is CompositeHighWater(), because
        // a merged mark is pinned at ~983k from the first composite mint onward and every "the
        // high-water mark did not move over N churn rounds" assertion about ordinary programs
        // would be vacuously true for the rest of the process. The composite's leak case is a
        // separate CASE and reads the BAND'S OWN counters below (PeekPipeCompositeSlot*) - a
        // composite's slot has TWO independent release paths (the pipeline cache's LRU eviction
        // and the composite ProgramObject's destructor), and a slot that never comes back to
        // the band moves neither of the ordinary numbers.
        ShaderCso,
    };

    // Live slots of this kind right now, and one past the highest slot ever handed out.
    // Both return false, touching nothing, where the allocator is out of reach: in a PULL
    // build there is no allocator at all (it is `#if MOBILEGL_PIPE_PUSH`), and on Android this
    // module links the shipping libMobileGL.so built -fvisibility=hidden, so no internal symbol
    // resolves. A caller that gets false must SKIP rather than pass - "could not look" is not
    // "did not leak".
    bool PeekPipeSlotLiveCount(PipeSlotKind kind, unsigned* outLive);
    bool PeekPipeSlotHighWater(PipeSlotKind kind, unsigned* outHighWater);

    // The ShaderCso COMPOSITE BAND's own three numbers, the seventh..ninth members
    // contract-v2.md 4.3 asks this header for. There is no `kind` argument because the band is
    // ShaderCso's alone - AllocateComposite is the one door into it and no other kind has one.
    // All three return false on the same terms as the two above, and a caller that gets false
    // must SKIP.
    //
    //   PeekPipeCompositeSlotLiveCount   = MGPipeSlotAllocator::CompositeLiveCount(), the band's
    //                                      share of LiveCount(ShaderCso).
    //   PeekPipeCompositeSlotHighWater   = CompositeHighWater() VERBATIM, i.e. one past the
    //                                      highest band slot ever handed out. It is an ABSOLUTE
    //                                      slot number and therefore starts at the band's base,
    //                                      not at zero - "no composite was ever minted" reads as
    //                                      `high water == band base`, which is what the third
    //                                      member is for. It is not returned base-relative
    //                                      because a peek whose name says HighWater and whose
    //                                      value is a delta is exactly the kind of quietly
    //                                      redefined counter this member exists to correct.
    //   PeekPipeCompositeSlotBandBase    = kMGPipeShaderCsoCompositeSlotBase, the floor the
    //                                      other two are read against. A constant, but it
    //                                      reaches a scenario only through this header: the
    //                                      MG_Pipe headers and the GL headers are not meant to
    //                                      meet in one translation unit, which is why this
    //                                      harness exists at all.
    bool PeekPipeCompositeSlotLiveCount(unsigned* outLive);
    bool PeekPipeCompositeSlotHighWater(unsigned* outHighWater);
    bool PeekPipeCompositeSlotBandBase(unsigned* outBandBase);

} // namespace MGITest
