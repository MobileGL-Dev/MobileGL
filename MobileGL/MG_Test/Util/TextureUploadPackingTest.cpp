// MobileGL - MobileGL/MG_Test/Util/TextureUploadPackingTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The sparse texture upload helpers: the packed staging page and the two-box split.
#include <gtest/gtest.h>
#include <MG_Util/Texture/PackedTextureUpload.h>
#include <MG_Util/Texture/TextureUploadBoxes.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace {
    namespace Packed = MobileGL::MG_Util::PackedTextureUpload;
    namespace Boxes = MobileGL::MG_Util::TextureUploadBoxes;

    // A level shadow whose every texel names its own coordinates, so a copied row shows where
    // it came from.
    struct Level {
        int width, height;
        std::vector<std::uint8_t> texels;

        Level(int w, int h) : width(w), height(h), texels(static_cast<std::size_t>(w) * h * Packed::BytesPerTexel) {
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    std::uint8_t* texel = Texel(x, y);
                    texel[0] = static_cast<std::uint8_t>(x);
                    texel[1] = static_cast<std::uint8_t>(y);
                    texel[2] = static_cast<std::uint8_t>(x >> 8);
                    texel[3] = static_cast<std::uint8_t>(y >> 8);
                }
            }
        }

        std::uint8_t* Texel(int x, int y) {
            return texels.data() + (static_cast<std::size_t>(y) * width + x) * Packed::BytesPerTexel;
        }

        Packed::Region Region(int level, int x, int y, int w, int h) {
            return {Texel(x, y), static_cast<std::size_t>(width) * Packed::BytesPerTexel, level, x, y, w, h};
        }
    };

    void AddRegion(Packed::Batch& batch, const Packed::Region& region) {
        batch.regions[batch.count++] = region;
    }

    bool Overlap(const Packed::Point& a, const Packed::Region& ra, const Packed::Point& b, const Packed::Region& rb) {
        return a.x < b.x + rb.width && b.x < a.x + ra.width && a.y < b.y + rb.height && b.y < a.y + ra.height;
    }

    void ExpectValidLayout(const Packed::Batch& batch) {
        ASSERT_GT(batch.width, 0);
        ASSERT_GT(batch.height, 0);
        EXPECT_LE(batch.width, Packed::PageSide);
        EXPECT_LE(batch.height, Packed::PageSide);
        for (std::size_t i = 0; i < batch.count; ++i) {
            const auto& region = batch.regions[i];
            const auto& offset = batch.offsets[i];
            EXPECT_GE(offset.x, 0);
            EXPECT_GE(offset.y, 0);
            EXPECT_LE(offset.x + region.width, batch.width) << "region " << i;
            EXPECT_LE(offset.y + region.height, batch.height) << "region " << i;
            for (std::size_t j = i + 1; j < batch.count; ++j) {
                EXPECT_FALSE(Overlap(offset, region, batch.offsets[j], batch.regions[j]))
                    << "regions " << i << " and " << j;
            }
        }
    }

    TEST(PackedTextureUpload, PacksScatteredSpritesWithoutOverlap) {
        Level mip0(512, 512);
        Level mip1(256, 256);
        Packed::Batch batch;
        AddRegion(batch, mip0.Region(0, 0, 0, 16, 16));
        AddRegion(batch, mip0.Region(0, 480, 32, 32, 16));
        AddRegion(batch, mip0.Region(0, 200, 400, 16, 32));
        AddRegion(batch, mip0.Region(0, 300, 300, 64, 64));
        AddRegion(batch, mip1.Region(1, 8, 8, 8, 8));
        AddRegion(batch, mip1.Region(1, 240, 240, 16, 16));

        ASSERT_TRUE(Packed::Pack(batch, 512ull * 512 + 256ull * 256));
        ExpectValidLayout(batch);
        // Largest first, so the 64x64 sprite takes the page origin.
        EXPECT_EQ(batch.regions[0].width * batch.regions[0].height, 64 * 64);
        EXPECT_EQ(batch.offsets[0].x, 0);
        EXPECT_EQ(batch.offsets[0].y, 0);
    }

    TEST(PackedTextureUpload, WritePageCopiesEachRegionToItsOffset) {
        Level mip0(512, 512);
        Packed::Batch batch;
        AddRegion(batch, mip0.Region(0, 3, 5, 7, 9));
        AddRegion(batch, mip0.Region(0, 400, 300, 20, 11));
        AddRegion(batch, mip0.Region(0, 260, 17, 13, 13));
        ASSERT_TRUE(Packed::Pack(batch, 512ull * 512));
        ExpectValidLayout(batch);

        std::vector<std::uint8_t> page(batch.ByteSize(), 0xCD);
        Packed::WritePage(batch, page.data());
        const std::size_t pitch = static_cast<std::size_t>(batch.width) * Packed::BytesPerTexel;
        for (std::size_t i = 0; i < batch.count; ++i) {
            const auto& region = batch.regions[i];
            const auto& offset = batch.offsets[i];
            for (int y = 0; y < region.height; ++y) {
                for (int x = 0; x < region.width; ++x) {
                    const std::uint8_t* texel =
                        page.data() + (offset.y + y) * pitch + (offset.x + x) * Packed::BytesPerTexel;
                    ASSERT_EQ(texel[0], static_cast<std::uint8_t>(region.x + x)) << "region " << i;
                    ASSERT_EQ(texel[1], static_cast<std::uint8_t>(region.y + y)) << "region " << i;
                }
            }
        }
    }

    TEST(PackedTextureUpload, DeclinesBatchesThatSaveTooLittle) {
        Level mip0(512, 512);
        Packed::Batch single;
        AddRegion(single, mip0.Region(0, 0, 0, 16, 16));
        EXPECT_FALSE(Packed::Pack(single, 512ull * 512)) << "one region already uploads exactly";

        Packed::Batch dense;
        AddRegion(dense, mip0.Region(0, 0, 0, 16, 16));
        AddRegion(dense, mip0.Region(0, 16, 0, 16, 16));
        EXPECT_FALSE(Packed::Pack(dense, 32ull * 16)) << "the union box is already small";
    }

    TEST(PackedTextureUpload, DeclinesRegionsThatCannotFitThePage) {
        Level mip0(512, 512);
        Packed::Batch wide;
        AddRegion(wide, mip0.Region(0, 0, 0, Packed::PageSide + 1, 4));
        AddRegion(wide, mip0.Region(0, 300, 300, 4, 4));
        EXPECT_FALSE(Packed::Pack(wide, 512ull * 512));

        Packed::Batch full;
        AddRegion(full, mip0.Region(0, 0, 0, Packed::PageSide, Packed::PageSide));
        AddRegion(full, mip0.Region(0, 256, 256, Packed::PageSide, Packed::PageSide));
        EXPECT_FALSE(Packed::Pack(full, 512ull * 512)) << "more dirty texels than one page holds";
    }

    TEST(PackedTextureUpload, PacksTheMaximumRegionCount) {
        Level mip0(512, 512);
        Packed::Batch batch;
        for (std::size_t i = 0; i < Packed::MaxRegions; ++i) {
            const int size = 8 + static_cast<int>(i % 4) * 4;
            AddRegion(batch, mip0.Region(0, static_cast<int>(i) * 15, static_cast<int>(i) * 15, size, size));
        }
        ASSERT_TRUE(Packed::Pack(batch, 512ull * 512));
        ExpectValidLayout(batch);
    }

    std::array<Boxes::Box, Boxes::MaxRegions> Regions(std::initializer_list<Boxes::Box> boxes) {
        std::array<Boxes::Box, Boxes::MaxRegions> regions{};
        std::size_t i = 0;
        for (const auto& box : boxes)
            regions[i++] = box;
        return regions;
    }

    bool Contains(const Boxes::Box& outer, const Boxes::Box& inner) {
        return outer.left <= inner.left && outer.top <= inner.top && outer.right >= inner.right &&
               outer.bottom >= inner.bottom;
    }

    TEST(TextureUploadBoxes, SplitsTwoSeparatedClusters) {
        const Boxes::Box a{0, 0, 16, 16}, b{20, 4, 36, 20}, c{400, 300, 416, 316}, d{420, 330, 436, 346};
        auto regions = Regions({c, a, d, b});
        const Boxes::Box bounds{0, 0, 436, 346};
        const auto split = Boxes::Split(bounds, regions, 4);
        ASSERT_TRUE(split.has_value());
        const auto& first = (*split)[0];
        const auto& second = (*split)[1];
        const bool firstHoldsLeft = Contains(first, a);
        const auto& left = firstHoldsLeft ? first : second;
        const auto& right = firstHoldsLeft ? second : first;
        EXPECT_TRUE(Contains(left, a) && Contains(left, b));
        EXPECT_TRUE(Contains(right, c) && Contains(right, d));
        EXPECT_LE(first.Texels() + second.Texels(), bounds.Texels() / 2);
        const bool disjoint = left.right <= right.left || left.bottom <= right.top;
        EXPECT_TRUE(disjoint);
    }

    TEST(TextureUploadBoxes, KeepsOneBoxWhenNoCutSeparatesTheRegions) {
        // Overlapping regions along the diagonal: no cut on either axis separates two groups.
        auto regions = Regions({{0, 0, 100, 100}, {90, 90, 190, 190}, {180, 180, 280, 280}});
        EXPECT_FALSE(Boxes::Split({0, 0, 280, 280}, regions, 3).has_value());
    }

    TEST(TextureUploadBoxes, SkipsSmallOrSingleRegionLevels) {
        EXPECT_FALSE(Boxes::WorthSearching({0, 0, 32, 32}, 4)) << "a small level keeps one job";
        EXPECT_FALSE(Boxes::WorthSearching({0, 0, 512, 512}, 1));
        EXPECT_FALSE(Boxes::WorthSearching({0, 0, 512, 512}, Boxes::MaxRegions + 1));
        EXPECT_TRUE(Boxes::WorthSearching({0, 0, 512, 512}, 2));
    }
} // namespace
