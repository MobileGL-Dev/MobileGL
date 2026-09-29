// MobileGL - MobileGL/MG_Test/State/MipmapFootprintTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// MipmapStorage::GetDirtyFootprint against a reference bitmap of the writes.
//
// A backend whose copy of a level may hold GPU-written texels uploads exactly the footprint, so
// the footprint has to be the written texels and nothing else: every texel covered, no texel
// outside the writes, no texel twice. The rect list beside it only has to COVER the writes, and
// its merges, fold and 3/4 cut-off are what the footprint must not inherit - so the patterns
// below are the ones that trip them: touching boxes whose bounding box has gaps, more disjoint
// boxes than the list keeps, and random mixes of both, in 2D and 3D. The cover property of the
// list and the union box's exactness are checked alongside, since the footprint work sits inside
// the same insert.

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "Includes.h"

#include <MG_State/GLState/TextureState/MipmapStorage.h>

using namespace MobileGL;
using MG_State::GLState::MipmapDirtyRegion;
using MG_State::GLState::MipmapStorage;

namespace {
    struct Level {
        MipmapStorage storage;
        IntVec3 size;
        std::vector<bool> written;

        explicit Level(IntVec3 levelSize) : size(levelSize) {
            storage.AllocateLevel(0, {levelSize, Texels()});
            written.assign(Texels(), false);
        }

        SizeT Texels() const {
            return static_cast<SizeT>(size.x()) * static_cast<SizeT>(size.y()) * static_cast<SizeT>(size.z());
        }

        SizeT Index(Int x, Int y, Int z) const {
            return (static_cast<SizeT>(z) * static_cast<SizeT>(size.y()) + static_cast<SizeT>(y)) *
                       static_cast<SizeT>(size.x()) +
                   static_cast<SizeT>(x);
        }

        void Write(IntVec3 offset, IntVec3 extent) {
            storage.MarkDirtyRegion(0, offset, extent);
            for (Int z = offset.z(); z < offset.z() + extent.z(); ++z)
                for (Int y = offset.y(); y < offset.y() + extent.y(); ++y)
                    for (Int x = offset.x(); x < offset.x() + extent.x(); ++x) written[Index(x, y, z)] = true;
        }

        void Expect(const char* what) const {
            SCOPED_TRACE(what);
            ASSERT_TRUE(storage.IsDirty(0));
            std::vector<MipmapDirtyRegion> footprint;
            storage.GetDirtyFootprint(0, footprint);
            std::vector<int> covered(Texels(), 0);
            for (const auto& box : footprint) {
                ASSERT_FALSE(box.Empty());
                ASSERT_GE(box.lo.x(), 0);
                ASSERT_GE(box.lo.y(), 0);
                ASSERT_GE(box.lo.z(), 0);
                ASSERT_LE(box.hi.x(), size.x());
                ASSERT_LE(box.hi.y(), size.y());
                ASSERT_LE(box.hi.z(), size.z());
                for (Int z = box.lo.z(); z < box.hi.z(); ++z)
                    for (Int y = box.lo.y(); y < box.hi.y(); ++y)
                        for (Int x = box.lo.x(); x < box.hi.x(); ++x) ++covered[Index(x, y, z)];
            }
            SizeT missing = 0, extra = 0, twice = 0;
            MipmapDirtyRegion bounds{size, IntVec3{0, 0, 0}};
            for (Int z = 0; z < size.z(); ++z)
                for (Int y = 0; y < size.y(); ++y)
                    for (Int x = 0; x < size.x(); ++x) {
                        const SizeT i = Index(x, y, z);
                        missing += written[i] && covered[i] == 0;
                        extra += !written[i] && covered[i] > 0;
                        twice += covered[i] > 1;
                        if (written[i]) {
                            bounds.lo = {std::min(bounds.lo.x(), x), std::min(bounds.lo.y(), y),
                                         std::min(bounds.lo.z(), z)};
                            bounds.hi = {std::max(bounds.hi.x(), x + 1), std::max(bounds.hi.y(), y + 1),
                                         std::max(bounds.hi.z(), z + 1)};
                        }
                    }
            EXPECT_EQ(missing, 0u) << "written texels outside the footprint";
            EXPECT_EQ(extra, 0u) << "unwritten texels inside the footprint";
            EXPECT_EQ(twice, 0u) << "texels in more than one footprint box";

            const MipmapDirtyRegion region = storage.GetDirtyRegion(0);
            EXPECT_TRUE(region.lo == bounds.lo && region.hi == bounds.hi)
                << "the union box is not the writes' bounding box";

            MipmapDirtyRegion rects[MipmapStorage::kMaxDirtyRects];
            const SizeT rectCount = storage.GetDirtyRects(0, rects, MipmapStorage::kMaxDirtyRects);
            SizeT uncovered = 0;
            for (Int z = 0; z < size.z(); ++z)
                for (Int y = 0; y < size.y(); ++y)
                    for (Int x = 0; x < size.x(); ++x) {
                        if (!written[Index(x, y, z)] || rectCount == 0) continue;
                        bool inRect = false;
                        for (SizeT r = 0; r < rectCount && !inRect; ++r)
                            inRect = x >= rects[r].lo.x() && x < rects[r].hi.x() && y >= rects[r].lo.y() &&
                                     y < rects[r].hi.y() && z >= rects[r].lo.z() && z < rects[r].hi.z();
                        uncovered += !inRect;
                    }
            EXPECT_EQ(uncovered, 0u) << "the rect list no longer covers the writes";
        }
    };

    TEST(MipmapFootprintTest, TwoDistantBoxesAreTwoBoxes) {
        Level level({8, 8, 1});
        level.Write({0, 0, 0}, {2, 2, 1});
        level.Write({5, 5, 0}, {2, 2, 1});
        level.Expect("two distant 2x2");
        std::vector<MipmapDirtyRegion> footprint;
        level.storage.GetDirtyFootprint(0, footprint);
        EXPECT_EQ(footprint.size(), 2u);
    }

    // Touching boxes whose bounding box has a gap: the rect list merges them anyway.
    TEST(MipmapFootprintTest, ThreeQuadrantsLeaveTheFourthOut) {
        Level level({8, 8, 1});
        level.Write({0, 0, 0}, {4, 4, 1});
        level.Write({4, 0, 0}, {4, 4, 1});
        level.Write({0, 4, 0}, {4, 4, 1});
        level.Expect("three quadrants");
    }

    TEST(MipmapFootprintTest, OverlappingStaggeredBoxes) {
        Level level({16, 16, 1});
        level.Write({0, 0, 0}, {6, 6, 1});
        level.Write({3, 3, 0}, {6, 6, 1});
        level.Write({6, 6, 0}, {6, 6, 1});
        level.Expect("staggered overlaps");
    }

    // More disjoint boxes than the list keeps: it folds, the footprint must not.
    TEST(MipmapFootprintTest, MoreBoxesThanTheListKeeps) {
        Level level({40, 40, 1});
        for (int j = 0; j < 10; ++j)
            for (int i = 0; i < 10; ++i) level.Write({4 * i + 1, 4 * j + 1, 0}, {2, 2, 1});
        level.Expect("a hundred 2x2");
        std::vector<MipmapDirtyRegion> footprint;
        level.storage.GetDirtyFootprint(0, footprint);
        EXPECT_EQ(footprint.size(), 100u);
    }

    // Scanlines of one width merge exactly and stay one box without ever starting the log.
    TEST(MipmapFootprintTest, AbuttingScanlinesStayOneBox) {
        Level level({32, 32, 1});
        for (int y = 4; y < 20; ++y) level.Write({3, y, 0}, {10, 1, 1});
        level.Expect("scanlines");
        std::vector<MipmapDirtyRegion> footprint;
        level.storage.GetDirtyFootprint(0, footprint);
        EXPECT_EQ(footprint.size(), 1u);
    }

    // A whole-level dirty is a whole-level footprint, and a write on top of it changes nothing.
    TEST(MipmapFootprintTest, WholeLevelDirtyIsTheWholeLevel) {
        Level level({8, 8, 2});
        level.storage.MarkDirty(0, true);
        level.written.assign(level.Texels(), true);
        level.Write({1, 1, 0}, {2, 2, 1});
        level.Expect("whole level");
    }

    // Clean resets the footprint, log included.
    TEST(MipmapFootprintTest, CleanStartsOver) {
        Level level({8, 8, 1});
        level.Write({0, 0, 0}, {4, 4, 1});
        level.Write({4, 0, 0}, {4, 4, 1});
        level.Write({0, 4, 0}, {4, 4, 1});
        level.storage.MarkDirty(0, false);
        std::vector<MipmapDirtyRegion> footprint;
        level.storage.GetDirtyFootprint(0, footprint);
        EXPECT_TRUE(footprint.empty());
        level.written.assign(level.Texels(), false);
        level.Write({5, 5, 0}, {2, 2, 1});
        level.Expect("after clean");
    }

    TEST(MipmapFootprintTest, RandomWrites) {
        std::mt19937 rng(0x5eed1234u);
        const IntVec3 sizes[] = {{8, 8, 1}, {33, 17, 1}, {64, 64, 1}, {12, 9, 5}, {7, 1, 1}};
        for (int round = 0; round < 400; ++round) {
            const IntVec3 size = sizes[round % 5];
            Level level(size);
            const int writes = 1 + static_cast<int>(rng() % (round % 3 == 0 ? 160u : 12u));
            for (int w = 0; w < writes; ++w) {
                const Int maxEdge = round % 2 == 0 ? 3 : std::max(size.x(), size.y());
                const Int ex = 1 + static_cast<Int>(rng() % static_cast<unsigned>(std::min(maxEdge, size.x())));
                const Int ey = 1 + static_cast<Int>(rng() % static_cast<unsigned>(std::min(maxEdge, size.y())));
                const Int ez = 1 + static_cast<Int>(rng() % static_cast<unsigned>(size.z()));
                const Int ox = static_cast<Int>(rng() % static_cast<unsigned>(size.x() - ex + 1));
                const Int oy = static_cast<Int>(rng() % static_cast<unsigned>(size.y() - ey + 1));
                const Int oz = static_cast<Int>(rng() % static_cast<unsigned>(size.z() - ez + 1));
                level.Write({ox, oy, oz}, {ex, ey, ez});
            }
            level.Expect(("round " + std::to_string(round)).c_str());
            if (HasFatalFailure() || HasNonfatalFailure()) return;
        }
    }
} // namespace
