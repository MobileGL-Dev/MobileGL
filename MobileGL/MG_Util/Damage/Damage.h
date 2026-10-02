// MobileGL - MobileGL/MG_Util/Damage/Damage.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// DAMAGE AND BUFFER AGE: the bookkeeping behind partial presents, kept free of any GL, EGL, Vulkan
// or wire type so a host test pins it.
//
// Coordinates. A Rect is in pixels of a surface. EGL's swap-with-damage rectangles have their
// origin at the BOTTOM-left (GL window coordinates); a wl_buffer, a shared image's memory rows and
// a Vulkan image are addressed from the TOP-left. FlipY converts between the two and is its own
// inverse.
//
// Damage. A Region is either the whole surface or a bounded list of rectangles that may overlap
// (a copy or a repaint of an overlap twice is harmless). Normalize clamps it to the surface and
// keeps it within kMaxRects, merging the pair that grows least, so it fits a fixed-size record.
//
// Rotating buffers. A buffer that is written only every Nth frame (one of a window's shared
// images) has missed the damage of the frames in between: BufferDamageTracker keeps, per buffer,
// what changed since that buffer was last written, which is what the next write must bring over.
//
// Buffer age (EGL_EXT_buffer_age). SwapchainAgeTracker answers, per image of a swapchain, how many
// presents ago the image's content was the frame then presented: 0 when unknown (never presented,
// its content not kept, or the chain was rebuilt).

#include <Includes.h>

#include <limits>

namespace MobileGL::MG_Util::Damage {
    struct Rect {
        Int32 X = 0;
        Int32 Y = 0;
        Int32 Width = 0;
        Int32 Height = 0;

        constexpr Bool Empty() const { return Width <= 0 || Height <= 0; }
        constexpr Int64 Area() const { return Empty() ? 0 : static_cast<Int64>(Width) * Height; }
        constexpr Bool operator==(const Rect&) const = default;
    };

    // The most rectangles a damage record carries.
    inline constexpr Uint32 kMaxRects = 16;

    // Bottom-left origin <-> top-left origin, on a surface `height` rows tall.
    constexpr Rect FlipY(const Rect& rect, Int32 height) {
        return {rect.X, height - rect.Y - rect.Height, rect.Width, rect.Height};
    }

    // The bounding box of two rectangles (an empty one adds nothing).
    constexpr Rect Bound(const Rect& a, const Rect& b) {
        if (a.Empty()) return b;
        if (b.Empty()) return a;
        const Int32 x0 = std::min(a.X, b.X);
        const Int32 y0 = std::min(a.Y, b.Y);
        const Int32 x1 = std::max(a.X + a.Width, b.X + b.Width);
        const Int32 y1 = std::max(a.Y + a.Height, b.Y + b.Height);
        return {x0, y0, x1 - x0, y1 - y0};
    }

    // `rect` clipped to the surface (0, 0, width, height); empty when it lies outside.
    constexpr Rect Clip(const Rect& rect, Int32 width, Int32 height) {
        const Int64 x0 = std::max<Int64>(rect.X, 0);
        const Int64 y0 = std::max<Int64>(rect.Y, 0);
        const Int64 x1 = std::min<Int64>(static_cast<Int64>(rect.X) + rect.Width, width);
        const Int64 y1 = std::min<Int64>(static_cast<Int64>(rect.Y) + rect.Height, height);
        if (x1 <= x0 || y1 <= y0) return {};
        return {static_cast<Int32>(x0), static_cast<Int32>(y0), static_cast<Int32>(x1 - x0),
                static_cast<Int32>(y1 - y0)};
    }

    // Whether `outer` contains every pixel of `inner`.
    constexpr Bool Contains(const Rect& outer, const Rect& inner) {
        return inner.X >= outer.X && inner.Y >= outer.Y && inner.X + inner.Width <= outer.X + outer.Width &&
               inner.Y + inner.Height <= outer.Y + outer.Height;
    }

    // `a` with `b` taken out: up to four rectangles (the bands above and below `b`, and the parts left
    // and right of it in between).
    inline Vector<Rect> Subtract(const Rect& a, const Rect& b) {
        const Int32 ax1 = a.X + a.Width, ay1 = a.Y + a.Height;
        const Int32 bx1 = b.X + b.Width, by1 = b.Y + b.Height;
        if (a.Empty()) return {};
        if (b.Empty() || b.X >= ax1 || bx1 <= a.X || b.Y >= ay1 || by1 <= a.Y) return {a};
        Vector<Rect> pieces;
        const Int32 midY0 = std::max(a.Y, b.Y), midY1 = std::min(ay1, by1);
        if (b.Y > a.Y) pieces.push_back({a.X, a.Y, a.Width, b.Y - a.Y});
        if (by1 < ay1) pieces.push_back({a.X, by1, a.Width, ay1 - by1});
        if (b.X > a.X) pieces.push_back({a.X, midY0, b.X - a.X, midY1 - midY0});
        if (bx1 < ax1) pieces.push_back({bx1, midY0, ax1 - bx1, midY1 - midY0});
        return pieces;
    }

    // The same pixels as `rects`, as rectangles no two of which overlap (a copy whose regions must
    // not overlap in memory takes these).
    inline Vector<Rect> Disjoint(const Vector<Rect>& rects) {
        Vector<Rect> out;
        for (const Rect& rect : rects) {
            Vector<Rect> pieces{rect};
            for (const Rect& taken : out) {
                Vector<Rect> next;
                for (const Rect& piece : pieces) {
                    for (const Rect& left : Subtract(piece, taken)) next.push_back(left);
                }
                pieces = std::move(next);
                if (pieces.empty()) break;
            }
            for (const Rect& piece : pieces) {
                if (!piece.Empty()) out.push_back(piece);
            }
        }
        return out;
    }

    class Region {
    public:
        static Region Full() {
            Region region;
            region.m_full = true;
            return region;
        }
        static Region FromRects(const Rect* rects, SizeT count) {
            Region region;
            for (SizeT i = 0; i < count; ++i) region.Add(rects[i]);
            return region;
        }
        // EGL_KHR/EXT_swap_buffers_with_damage's argument: `count` groups of x, y, width, height,
        // bottom-left origin. No rectangles means the whole surface.
        template <typename IntT>
        static Region FromEglRects(const IntT* rects, IntT count) {
            if (rects == nullptr || count <= 0) return Full();
            Region region;
            for (IntT i = 0; i < count; ++i) {
                region.Add({static_cast<Int32>(rects[i * 4 + 0]), static_cast<Int32>(rects[i * 4 + 1]),
                            static_cast<Int32>(rects[i * 4 + 2]), static_cast<Int32>(rects[i * 4 + 3])});
            }
            return region;
        }

        Bool IsFull() const { return m_full; }
        // Nothing damaged (and not the whole surface).
        Bool IsEmpty() const { return !m_full && m_rects.empty(); }
        const Vector<Rect>& Rects() const { return m_rects; }

        void Add(const Rect& rect) {
            if (m_full || rect.Empty()) return;
            m_rects.push_back(rect);
        }
        void Unite(const Region& other) {
            if (m_full) return;
            if (other.m_full) {
                SetFull();
                return;
            }
            for (const Rect& rect : other.m_rects) Add(rect);
        }
        void SetFull() {
            m_full = true;
            m_rects.clear();
        }
        void Clear() {
            m_full = false;
            m_rects.clear();
        }

        // Clipped to a `width` x `height` surface; a rectangle that covers it makes the region Full;
        // more than `maxRects` rectangles are merged, cheapest pair first, until they fit.
        void Normalize(Int32 width, Int32 height, Uint32 maxRects = kMaxRects) {
            if (m_full) return;
            Vector<Rect> kept;
            kept.reserve(m_rects.size());
            for (const Rect& rect : m_rects) {
                const Rect clipped = Clip(rect, width, height);
                if (clipped.Empty()) continue;
                if (clipped.X == 0 && clipped.Y == 0 && clipped.Width == width && clipped.Height == height) {
                    SetFull();
                    return;
                }
                kept.push_back(clipped);
            }
            // A rectangle inside another (a repeat, most often) adds nothing.
            Vector<Rect> distinct;
            distinct.reserve(kept.size());
            for (SizeT i = 0; i < kept.size(); ++i) {
                Bool covered = false;
                for (SizeT j = 0; j < kept.size() && !covered; ++j) {
                    if (i == j || !Contains(kept[j], kept[i])) continue;
                    // Of two equal rectangles the first stays.
                    covered = !(kept[j] == kept[i]) || j < i;
                }
                if (!covered) distinct.push_back(kept[i]);
            }
            const SizeT limit = std::max<Uint32>(maxRects, 1);
            // Far too many to pair up every frame: their bounding box.
            if (distinct.size() > 4 * kMaxRects && distinct.size() > limit) {
                Rect box;
                for (const Rect& rect : distinct) box = Bound(box, rect);
                distinct.assign(1, box);
            }
            while (distinct.size() > limit) {
                SizeT bestI = 0, bestJ = 1;
                Int64 bestGrowth = std::numeric_limits<Int64>::max();
                for (SizeT i = 0; i < distinct.size(); ++i) {
                    for (SizeT j = i + 1; j < distinct.size(); ++j) {
                        const Int64 growth =
                            Bound(distinct[i], distinct[j]).Area() - distinct[i].Area() - distinct[j].Area();
                        if (growth < bestGrowth) {
                            bestGrowth = growth;
                            bestI = i;
                            bestJ = j;
                        }
                    }
                }
                distinct[bestI] = Bound(distinct[bestI], distinct[bestJ]);
                distinct.erase(distinct.begin() + static_cast<std::ptrdiff_t>(bestJ));
            }
            if (distinct.size() == 1 && distinct[0] == Rect{0, 0, width, height}) {
                SetFull();
                return;
            }
            m_rects = std::move(distinct);
        }

        // The rectangles flipped between bottom-left and top-left origin (a Full region stays Full).
        Region FlippedY(Int32 height) const {
            Region region = *this;
            for (Rect& rect : region.m_rects) rect = FlipY(rect, height);
            return region;
        }

        Bool operator==(const Region&) const = default;

    private:
        Bool m_full = false;
        Vector<Rect> m_rects;
    };

    // A region as a record's rectangle list - x, y, width, height per rectangle into `out` (room for
    // `maxRects`) - and the count. 0 is the whole surface (what a record that carries no damage
    // says); an empty region is one empty rectangle, so it still reads as "nothing" at the other
    // end. A region with more rectangles than fit goes as their bounding box.
    inline Uint32 PackRects(const Region& region, Int32* out, Uint32 maxRects) {
        if (region.IsFull() || maxRects == 0) return 0;
        if (region.IsEmpty()) {
            out[0] = out[1] = out[2] = out[3] = 0;
            return 1;
        }
        const Vector<Rect>& rects = region.Rects();
        if (rects.size() > maxRects) {
            Rect box;
            for (const Rect& rect : rects) box = Bound(box, rect);
            out[0] = box.X, out[1] = box.Y, out[2] = box.Width, out[3] = box.Height;
            return 1;
        }
        for (SizeT i = 0; i < rects.size(); ++i) {
            out[i * 4 + 0] = rects[i].X;
            out[i * 4 + 1] = rects[i].Y;
            out[i * 4 + 2] = rects[i].Width;
            out[i * 4 + 3] = rects[i].Height;
        }
        return static_cast<Uint32>(rects.size());
    }

    // Per buffer of a rotating set: the damage each has missed since it was last written.
    class BufferDamageTracker {
    public:
        explicit BufferDamageTracker(SizeT buffers = 0) { Resize(buffers); }

        void Resize(SizeT buffers) {
            m_missed.assign(buffers, Region::Full());
        }
        SizeT Size() const { return m_missed.size(); }

        // The buffer's contents are unknown (new, reallocated, resized): its next write copies all.
        void Invalidate(SizeT index) {
            if (index < m_missed.size()) m_missed[index].SetFull();
        }
        void InvalidateAll() {
            for (Region& region : m_missed) region.SetFull();
        }

        // A frame whose damage is `frame` is written into buffer `index` of a `width` x `height`
        // surface: the answer is what to copy into it - the frame's damage and everything the
        // buffer missed - and every other buffer now misses the frame's damage too.
        Region TakeForWrite(SizeT index, const Region& frame, Int32 width, Int32 height) {
            Region copy = Region::Full();
            if (index < m_missed.size()) {
                copy = std::move(m_missed[index]);
                copy.Unite(frame);
                copy.Normalize(width, height);
                m_missed[index].Clear();
            }
            for (SizeT i = 0; i < m_missed.size(); ++i) {
                if (i == index) continue;
                m_missed[i].Unite(frame);
                m_missed[i].Normalize(width, height);
            }
            return copy;
        }

    private:
        Vector<Region> m_missed;
    };

    // EGL_EXT_buffer_age over the images of one swapchain.
    class SwapchainAgeTracker {
    public:
        // A new (or rebuilt) chain of `imageCount` images: every age is unknown.
        void Reset(SizeT imageCount) {
            m_presentedAt.assign(imageCount, 0);
            m_presents = 0;
        }
        // Image `index` was presented. `contentKept`: its content stays what was presented until
        // it is drawn into again (otherwise its next age is unknown).
        void OnPresented(SizeT index, Bool contentKept) {
            ++m_presents;
            if (index < m_presentedAt.size()) m_presentedAt[index] = contentKept ? m_presents : 0;
        }
        // A frame was swapped without reaching any image: the counting no longer matches the
        // application's, so every age is unknown.
        void OnFrameDropped() { std::fill(m_presentedAt.begin(), m_presentedAt.end(), Uint64{0}); }
        // The age of image `index` as the next frame's target: 0 = unknown.
        Int32 AgeOf(SizeT index) const {
            if (index >= m_presentedAt.size() || m_presentedAt[index] == 0) return 0;
            const Uint64 age = m_presents - m_presentedAt[index] + 1;
            return age > static_cast<Uint64>(std::numeric_limits<Int32>::max()) ? 0 : static_cast<Int32>(age);
        }

    private:
        Vector<Uint64> m_presentedAt; // the present count at which the image was last presented, 0 = none
        Uint64 m_presents = 0;
    };
} // namespace MobileGL::MG_Util::Damage
