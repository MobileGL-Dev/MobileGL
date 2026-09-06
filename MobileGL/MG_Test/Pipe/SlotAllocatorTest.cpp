// MobileGL - MobileGL/MG_Test/Pipe/SlotAllocatorTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The client slot allocator's IDENTITY CONTRACT (P2 brief C.0 c3). Everything Track H keys
// off - Espryt's six slot tables and Magma's VaoDrawMemo - is only as sound as these five
// statements, and each of them is the answer to a bug the {slot, gen} pair exists to close:
//
//   GenMovesOnlyOnSlotReuse            - a respecify must NOT move the generation, or every
//                                        glBufferData would invalidate every memo; a REUSE
//                                        must, or a stale handle would address its successor.
//   FreedSlotComesBackBeforeHighWater  - slots stay DENSE, which is what lets the server's
//                                        object table be an array rather than a hash map.
//   SlotZeroIsNeverHandedOut           - {0, 0} is the null handle for every kind and {0, 1}
//                                        is the default framebuffer.
//   LifetimeIdSurvivesARecycledAddress - the frontend key is the lifetime id, never a heap
//                                        address and never a GL name, so an ABA on either
//                                        cannot reproduce a handle. This is the ABA
//                                        HandleRecycleScenario reproduces end to end.
//   CompositeShaderBandIsNeverHandedOut- the top 1/16 of the ShaderCso slot space belongs to
//                                        the program-pipeline composite resolver.
//
// Needs the push sources (SlotAllocator.cpp is compiled only under MOBILEGL_PIPE_PUSH), so
// every case is a visible SKIP in a pull build rather than a vanishing test, and the five
// names are the same five in every build.
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>

#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;

namespace {
    // Slot 0 is reserved for every kind - null, and the default framebuffer for kind
    // Framebuffer - so the first allocatable slot is 1 in every build, pull included.
    TEST(SlotAllocator, ReservedHandlesAreWhatMGPipeHandlesSaysTheyAre) {
        EXPECT_EQ(kMGPipeFirstAllocatableSlot, 1u);
        EXPECT_TRUE(MGPipeHandleIsNull(kMGPipeNullHandle));
        EXPECT_FALSE(MGPipeHandleIsNull(kMGPipeDefaultFramebuffer));
        EXPECT_EQ(kMGPipeDefaultFramebuffer.Slot, 0u);
    }

    TEST(SlotAllocator, GenMovesOnlyOnSlotReuse) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
#else
        MGPipeSlotAllocator allocator;

        const MGPipeHandle first = allocator.Allocate(MGPipeKind::Buffer);
        const MGPipeHandle second = allocator.Allocate(MGPipeKind::Buffer);
        EXPECT_EQ(first.Gen, 0u) << "a slot's FIRST handout is generation 0";
        EXPECT_EQ(second.Gen, 0u);
        EXPECT_NE(first.Slot, second.Slot);
        EXPECT_TRUE(allocator.IsLive(MGPipeKind::Buffer, first));

        // A respecify is not an event here at all: nothing in the allocator's interface can
        // move a live handle's generation, which is the contract "gen increments only when a
        // slot is REUSED, never on a respecify" stated as an absence.
        EXPECT_EQ(allocator.GenOfSlot(MGPipeKind::Buffer, first.Slot), first.Gen);

        allocator.Free(MGPipeKind::Buffer, first);
        EXPECT_FALSE(allocator.IsLive(MGPipeKind::Buffer, first));
        // The bump happens on the NEXT handout, not on the free, so a slot that is freed and
        // never reused keeps its generation - and a double free cannot skip one.
        EXPECT_EQ(allocator.GenOfSlot(MGPipeKind::Buffer, first.Slot), first.Gen);
        allocator.Free(MGPipeKind::Buffer, first);
        EXPECT_EQ(allocator.GenOfSlot(MGPipeKind::Buffer, first.Slot), first.Gen);

        const MGPipeHandle reused = allocator.Allocate(MGPipeKind::Buffer);
        EXPECT_EQ(reused.Slot, first.Slot) << "the free list did not hand the slot back";
        EXPECT_NE(reused.Gen, first.Gen) << "a REUSED slot must carry a new generation";
        EXPECT_EQ(reused.Gen, first.Gen + 1);

        // THE POINT OF THE GENERATION: the dead handle is not the live one, it does not
        // validate, and it cannot free the slot its successor now owns.
        EXPECT_FALSE(first == reused);
        EXPECT_FALSE(allocator.IsLive(MGPipeKind::Buffer, first));
        EXPECT_TRUE(allocator.IsLive(MGPipeKind::Buffer, reused));
        allocator.Free(MGPipeKind::Buffer, first);
        EXPECT_TRUE(allocator.IsLive(MGPipeKind::Buffer, reused)) << "a stale handle freed a live slot";

        // Kinds are independent slot spaces: a Buffer slot 1 and a Texture slot 1 are
        // different objects, and freeing one must not touch the other.
        const MGPipeHandle texture = allocator.Allocate(MGPipeKind::Texture);
        EXPECT_EQ(texture.Slot, kMGPipeFirstAllocatableSlot);
        EXPECT_EQ(texture.Gen, 0u);
        EXPECT_TRUE(allocator.IsLive(MGPipeKind::Texture, texture));
        EXPECT_TRUE(allocator.IsLive(MGPipeKind::Buffer, reused));
#endif
    }

    TEST(SlotAllocator, FreedSlotComesBackBeforeHighWaterGrows) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
#else
        MGPipeSlotAllocator allocator;

        MGPipeHandle handles[8];
        for (MGPipeHandle& handle : handles) handle = allocator.Allocate(MGPipeKind::Framebuffer);
        const Uint32 highWater = allocator.HighWater(MGPipeKind::Framebuffer);
        EXPECT_EQ(allocator.LiveCount(MGPipeKind::Framebuffer), 8u);
        EXPECT_EQ(allocator.FreeCount(MGPipeKind::Framebuffer), 0u);

        allocator.Free(MGPipeKind::Framebuffer, handles[2]);
        allocator.Free(MGPipeKind::Framebuffer, handles[5]);
        EXPECT_EQ(allocator.LiveCount(MGPipeKind::Framebuffer), 6u);
        EXPECT_EQ(allocator.FreeCount(MGPipeKind::Framebuffer), 2u);

        // Density is the whole reason the server's object table can be an array: the two
        // freed slots have to come back before a ninth is minted.
        const MGPipeHandle a = allocator.Allocate(MGPipeKind::Framebuffer);
        const MGPipeHandle b = allocator.Allocate(MGPipeKind::Framebuffer);
        EXPECT_EQ(allocator.HighWater(MGPipeKind::Framebuffer), highWater) << "the high-water mark grew with two "
                                                                             "slots waiting on the free list";
        EXPECT_TRUE((a.Slot == handles[2].Slot && b.Slot == handles[5].Slot) ||
                    (a.Slot == handles[5].Slot && b.Slot == handles[2].Slot))
            << "the reused slots are not the two that were freed";

        // Only now does the mark move.
        const MGPipeHandle fresh = allocator.Allocate(MGPipeKind::Framebuffer);
        EXPECT_GT(allocator.HighWater(MGPipeKind::Framebuffer), highWater);
        EXPECT_EQ(fresh.Gen, 0u) << "a slot handed out for the FIRST time is generation 0";

        allocator.Reset();
        EXPECT_EQ(allocator.HighWater(MGPipeKind::Framebuffer), 0u);
        EXPECT_EQ(allocator.LiveCount(MGPipeKind::Framebuffer), 0u);
        EXPECT_EQ(allocator.FreeCount(MGPipeKind::Framebuffer), 0u);
#endif
    }

    TEST(SlotAllocator, SlotZeroIsNeverHandedOut) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
#else
        MGPipeSlotAllocator allocator;
        for (SizeT kindIndex = 1; kindIndex < MGPipeSlotAllocator::kKindCount; ++kindIndex) {
            const MGPipeKind kind = static_cast<MGPipeKind>(kindIndex);
            for (int i = 0; i < 4; ++i) {
                const MGPipeHandle handle = allocator.Allocate(kind);
                EXPECT_GE(handle.Slot, kMGPipeFirstAllocatableSlot)
                    << "kind " << kindIndex << " handed out the reserved slot";
                EXPECT_FALSE(MGPipeHandleIsNull(handle));
                // {0, 1} is the default framebuffer and must never be minted either.
                EXPECT_FALSE(handle == kMGPipeDefaultFramebuffer);
                allocator.Free(kind, handle);
            }
        }
        // Freeing a slot never puts 0 on the free list, so a churned kind still starts at 1.
        const MGPipeHandle again = allocator.Allocate(MGPipeKind::Buffer);
        EXPECT_EQ(again.Slot, kMGPipeFirstAllocatableSlot);
#endif
    }

    TEST(SlotAllocator, LifetimeIdSurvivesARecycledAddress) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
#else
        using MG_State::GLState::VertexArrayObject;

        // The allocation must actually happen: C++ permits eliding a new/delete pair, and an
        // elided one would let two objects share an address for reasons that have nothing to
        // do with the allocator. Publishing every pointer through a volatile sink keeps the
        // pairs (MG_Test/State/ObjectLifetimeIdTest.cpp's trick, and the same one
        // HandleRecycleScenario uses to reproduce the ABA through public GL).
        static void* volatile addressSink = nullptr;

        MGPipeSlotAllocator allocator;
        std::unordered_map<std::uintptr_t, MGPipeHandle> handleAtAddress;
        int reuseCount = 0;

        for (int attempt = 0; attempt < 64; ++attempt) {
            auto object = std::make_unique<VertexArrayObject>(0u);
            addressSink = object.get();
            const auto address = reinterpret_cast<std::uintptr_t>(object.get());
            const Uint64 lifetimeId = object->GetLifetimeId();

            // Acquire is the ordinary client path: find by lifetime id, allocate on a miss.
            const MGPipeHandle handle = allocator.Acquire(MGPipeKind::VertexElementsCso, lifetimeId);
            EXPECT_FALSE(MGPipeHandleIsNull(handle));
            // Asking again with the same live object must answer the SAME handle - that is
            // what makes the map an identity rather than a counter.
            EXPECT_TRUE(allocator.Acquire(MGPipeKind::VertexElementsCso, lifetimeId) == handle);
            EXPECT_EQ(allocator.LifetimeIdOfSlot(MGPipeKind::VertexElementsCso, handle.Slot), lifetimeId);

            const auto previous = handleAtAddress.find(address);
            if (previous != handleAtAddress.end()) {
                ++reuseCount;
                // THE ABA. The heap handed the same address back, and the handle must still
                // be a different one - either a different slot, or the same slot with a new
                // generation. If this ever held, an address-keyed memo would serve the dead
                // object's entry to the live one, which is the bug Track H removes.
                EXPECT_FALSE(previous->second == handle)
                    << "a recycled heap address reproduced handle {slot=" << handle.Slot << ", gen=" << handle.Gen
                    << "}";
            }
            handleAtAddress[address] = handle;

            // The object dies; the client's death notification frees the slot.
            allocator.Free(MGPipeKind::VertexElementsCso, handle);
            EXPECT_FALSE(allocator.IsLive(MGPipeKind::VertexElementsCso, handle));
            // And the lifetime id stops resolving, so a late lookup cannot resurrect it.
            EXPECT_TRUE(MGPipeHandleIsNull(allocator.FindByLifetimeId(MGPipeKind::VertexElementsCso, lifetimeId)));
        }

        if (reuseCount == 0) {
            GTEST_SKIP() << "inconclusive, not proven: this allocator never handed the same address back across "
                            "64 construct/destroy rounds, so the recycled-address case was never exercised";
        }
        RecordProperty("address_reuses_observed", reuseCount);
#endif
    }

    TEST(SlotAllocator, CompositeShaderBandIsNeverHandedOut) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
#elif MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
        // Exhausting the ShaderCso slot space below the composite band is what proves the
        // band is held back, and reaching the band's edge trips the allocator's
        // "slot space is exhausted" MOBILEGL_ASSERT - which is live, and correctly so, in a
        // DEBUG build. The claim is checked in the INFO builds the gates run.
        GTEST_SKIP() << "asserts are live in a DEBUG build and the exhaustion arm trips one on purpose";
#else
        MGPipeSlotAllocator allocator;
        // Ordinary programs walk the low slots and never enter the band.
        for (int i = 0; i < 8; ++i) {
            const MGPipeHandle handle = allocator.Allocate(MGPipeKind::ShaderCso);
            EXPECT_FALSE(MGPipeIsCompositeShaderSlot(handle.Slot));
        }

        // Walk the whole space up to the band. The last handout below the base must be the
        // slot immediately under it, and the next call must refuse rather than step in - a
        // composite handle minted by the ordinary allocator would collide with one the
        // program-pipeline resolver mints for a different object entirely.
        MGPipeHandle last = kMGPipeNullHandle;
        while (allocator.HighWater(MGPipeKind::ShaderCso) < kMGPipeShaderCsoCompositeSlotBase) {
            last = allocator.Allocate(MGPipeKind::ShaderCso);
            ASSERT_FALSE(MGPipeIsCompositeShaderSlot(last.Slot))
                << "the ordinary allocator entered the composite band at slot " << last.Slot;
        }
        EXPECT_EQ(last.Slot, kMGPipeShaderCsoCompositeSlotBase - 1);
        EXPECT_TRUE(MGPipeHandleIsNull(allocator.Allocate(MGPipeKind::ShaderCso)))
            << "the allocator handed out a composite-band slot instead of refusing";

        // Every other kind is unaffected: the band is a ShaderCso rule, not a global one.
        MGPipeSlotAllocator plain;
        for (Uint32 i = 0; i < 4; ++i) {
            const MGPipeHandle handle = plain.Allocate(MGPipeKind::Buffer);
            EXPECT_EQ(handle.Slot, kMGPipeFirstAllocatableSlot + i);
        }
#endif
    }
} // namespace
