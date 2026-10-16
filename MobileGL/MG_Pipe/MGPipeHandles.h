// MobileGL - MobileGL/MG_Pipe/MGPipeHandles.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// MGPipe object identity (plan B section 4.2).
//
// A handle is a {slot, gen} pair minted by the CLIENT and never by the server: no create_*
// call in the catalogue returns a server-cast handle, which is the deliberate deviation
// from driver-minted handles (D1) that lets the whole catalogue be remoted with ZERO creation round
// trips.
//
// Slots are dense and allocated PER KIND, so the server's object table is an array rather
// than a hash map. The allocator is a free list plus a high-water mark and has nothing to
// do with MG_State's IndexGenerator - that container's LIFO name reuse is the very problem
// {slot, gen} exists to close.
namespace MobileGL::MG_Pipe {
    enum class MGPipeKind : Uint8 {
        None = 0,
        Buffer = 1,
        Texture,
        Renderbuffer,
        Framebuffer,
        Xfb,
        RenderStateCso,
        VertexElementsCso,
        SamplerCso,
        SamplerViewCso,
        ShaderCso,
        Fence,
        Query,
        Context,
        KindCount,
    };

    // 8 bytes, POD, passed by value in a register pair.
    //
    // Gen increments only when a SLOT IS REUSED - never on a respecify - so {slot, gen} is
    // unique until the same slot has been recycled 2^32 times. That bound is documented
    // rather than defended at runtime in release builds: at one recycle per frame at
    // 1000 fps a single slot would take ~50 days of continuous churn to wrap, and the
    // debug allocator asserts on the wrap.
    //
    // Two generations exist in this design and they are strictly separate (section 4.2.2):
    // this one is the CLIENT's answer to "is this still the same GL object", while MGGen is
    // the SERVER's own epoch for "did I recast my driver object". Interface rule: no MGPipe
    // call may require the client to supply or know MGGen.
    struct MGPipeHandle {
        Uint32 Slot;
        Uint32 Gen;

        friend constexpr Bool operator==(const MGPipeHandle& a, const MGPipeHandle& b) {
            return a.Slot == b.Slot && a.Gen == b.Gen;
        }
    };

    static_assert(sizeof(MGPipeHandle) == 8, "MGPipeHandle is the 8-byte {slot, gen} pair");
    static_assert(alignof(MGPipeHandle) == 4, "MGPipeHandle must not gain padding on the wire");
    static_assert(std::is_trivially_copyable_v<MGPipeHandle>);

    // Reserved handles (section 4.2.1).
    //   {0, 0} is null for every kind.
    //   {0, 1} of kind Framebuffer is the DEFAULT framebuffer. It exists so the four
    //   pDefaultFramebufferInfo->defaultFBO identity comparisons in DirectGLES retire into
    //   an ordinary handle compare.
    inline constexpr MGPipeHandle kMGPipeNullHandle{0, 0};
    inline constexpr MGPipeHandle kMGPipeDefaultFramebuffer{0, 1};

    inline constexpr Bool MGPipeHandleIsNull(const MGPipeHandle& handle) {
        return handle.Slot == 0 && handle.Gen == 0;
    }

    // Slot 0 of every kind is reserved (null, and the default framebuffer for kind
    // Framebuffer), so a real allocation starts at 1.
    inline constexpr Uint32 kMGPipeFirstAllocatableSlot = 1;

    // ShaderCso slot space. The top 1/16 is reserved for program pipeline composites
    // (ARCHITECTURE.md §5.6): a composite is minted client-side from a pipeline object's stage
    // programs and is just another ShaderCso to the server. A band rather than a flag keeps the
    // composite resolver's lifetime bookkeeping out of the ordinary program slot allocator. The
    // only way in is MGPipeSlotAllocator::AllocateComposite (MG_Impl/Pipe/SlotAllocator.h);
    // Allocate refuses the band for ShaderCso, and the band has its own exhaustion check. A
    // composite slot has two release paths (pipeline cache LRU eviction, the composite
    // ProgramObject's destructor), both through MGPipeEmitShaderCsoDestroyAndFree (idempotent).
    inline constexpr Uint32 kMGPipeShaderCsoSlotLimit = 1u << 20;
    inline constexpr Uint32 kMGPipeShaderCsoCompositeSlotBase =
        kMGPipeShaderCsoSlotLimit - (kMGPipeShaderCsoSlotLimit >> 4);

    inline constexpr Bool MGPipeIsCompositeShaderSlot(Uint32 slot) {
        return slot >= kMGPipeShaderCsoCompositeSlotBase && slot < kMGPipeShaderCsoSlotLimit;
    }

    static_assert(kMGPipeShaderCsoCompositeSlotBase > kMGPipeFirstAllocatableSlot,
                  "the composite band must not swallow the ordinary program slots");
} // namespace MobileGL::MG_Pipe
