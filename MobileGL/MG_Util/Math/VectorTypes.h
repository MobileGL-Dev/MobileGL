// MobileGL - MobileGL/MG_Util/Math/VectorTypes.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <Includes.h>

#include <cstring>

namespace MobileGL {
    template <typename Derived, typename T, SizeT N>
    struct VecBase {
        Array<T, N> data;

        VecBase() { std::fill(data.begin(), data.end(), T(0)); }

        VecBase(std::initializer_list<T> init) {
            std::copy_n(init.begin(), std::min(N, init.size()), data.begin());
            if (init.size() < N) {
                std::fill(data.begin() + init.size(), data.end(), T(0));
            }
        }

        explicit VecBase(T scalar) { std::fill(data.begin(), data.end(), scalar); }

        T& operator[](SizeT index) {
            if (index >= N) throw std::out_of_range("Vector index out of range");
            return data[index];
        }

        const T& operator[](SizeT index) const {
            if (index >= N) throw std::out_of_range("Vector index out of range");
            return data[index];
        }

        Bool operator==(const VecBase& rhs) const { return data == rhs.data; }
        Bool operator!=(const VecBase& rhs) const { return !(*this == rhs); }

        Derived operator+(const VecBase& rhs) const {
            Derived result;
            for (SizeT i = 0; i < N; ++i)
                result[i] = data[i] + rhs[i];
            return result;
        }

        Derived operator-(const VecBase& rhs) const {
            Derived result;
            for (SizeT i = 0; i < N; ++i)
                result[i] = data[i] - rhs[i];
            return result;
        }

        Derived operator*(T scalar) const {
            Derived result;
            for (SizeT i = 0; i < N; ++i)
                result[i] = data[i] * scalar;
            return result;
        }

        Derived operator/(T scalar) const {
            if (scalar == 0) throw std::domain_error("Division by zero");
            T inv = T(1) / scalar;
            return *this * inv;
        }

        T Dot(const VecBase& rhs) const {
            T result = T(0);
            for (SizeT i = 0; i < N; ++i)
                result += data[i] * rhs[i];
            return result;
        }

        T Length() const { return std::sqrt(Dot(*this)); }

        Derived Normalized() const {
            T len = Length();
            if (len > 0) {
                return *this * (T(1) / len);
            }
            return static_cast<const Derived&>(*this);
        }
    };

    // Bit-pattern equality, for a vector used as a cache or staleness KEY rather than as a
    // number. IEEE `==` - which operator== above is - says a NaN never equals itself, so a single
    // NaN component makes every comparison answer "changed" and whatever the key guards is
    // rebuilt on every use, forever. Two zeros of opposite sign compare unequal here, which only
    // ever costs one extra rebuild.
    template <typename Derived, typename T, SizeT N>
    Bool BitwiseEqual(const VecBase<Derived, T, N>& a, const VecBase<Derived, T, N>& b) {
        return std::memcmp(a.data.data(), b.data.data(), sizeof(T) * N) == 0;
    }

    template <typename T>
    struct Vec2 : public VecBase<Vec2<T>, T, 2> {
        using Base = VecBase<Vec2<T>, T, 2>;
        using Base::Base;

        Vec2(T x, T y) : Base{x, y} {}

        T& x() { return (*this)[0]; }
        T& y() { return (*this)[1]; }
        const T& x() const { return (*this)[0]; }
        const T& y() const { return (*this)[1]; }

        T& r() { return (*this)[0]; }
        T& g() { return (*this)[1]; }
        const T& r() const { return (*this)[0]; }
        const T& g() const { return (*this)[1]; }

        T Cross(const Vec2& rhs) const { return x() * rhs.y() - y() * rhs.x(); }

        Vec2 Rotated(T angle) const {
            T cosA = std::cos(angle);
            T sinA = std::sin(angle);
            return {x() * cosA - y() * sinA, x() * sinA + y() * cosA};
        }
    };

    template <typename T>
    struct Vec3 : public VecBase<Vec3<T>, T, 3> {
        using Base = VecBase<Vec3<T>, T, 3>;
        using Base::Base;

        Vec3(T x, T y, T z) : Base{x, y, z} {}
        explicit Vec3(const Vec2<T>& v2, T z = T(0)) : Base{v2.x(), v2.y(), z} {}

        T& x() { return (*this)[0]; }
        T& y() { return (*this)[1]; }
        T& z() { return (*this)[2]; }
        const T& x() const { return (*this)[0]; }
        const T& y() const { return (*this)[1]; }
        const T& z() const { return (*this)[2]; }

        T& r() { return (*this)[0]; }
        T& g() { return (*this)[1]; }
        T& b() { return (*this)[2]; }
        const T& r() const { return (*this)[0]; }
        const T& g() const { return (*this)[1]; }
        const T& b() const { return (*this)[2]; }

        Vec2<T> xy() const { return {x(), y()}; }
        Vec2<T> xz() const { return {x(), z()}; }
        Vec2<T> yz() const { return {y(), z()}; }

        Vec2<T> rg() const { return {x(), y()}; }
        Vec2<T> rb() const { return {x(), z()}; }
        Vec2<T> gb() const { return {y(), z()}; }

        Vec3 Cross(const Vec3& rhs) const {
            return {y() * rhs.z() - z() * rhs.y(), z() * rhs.x() - x() * rhs.z(), x() * rhs.y() - y() * rhs.x()};
        }
    };

    template <typename T>
    struct Vec4 : public VecBase<Vec4<T>, T, 4> {
        using Base = VecBase<Vec4<T>, T, 4>;
        using Base::Base;

        Vec4(T x, T y, T z, T w) : Base{x, y, z, w} {}
        explicit Vec4(const Vec3<T>& v3, T w = T(1)) : Base{v3.x(), v3.y(), v3.z(), w} {}

        T& x() { return (*this)[0]; }
        T& y() { return (*this)[1]; }
        T& z() { return (*this)[2]; }
        T& w() { return (*this)[3]; }

        Vec2<T> xy() const { return {x(), y()}; }
        Vec2<T> xz() const { return {x(), z()}; }
        Vec2<T> xw() const { return {x(), w()}; }
        Vec2<T> yz() const { return {y(), z()}; }
        Vec2<T> yw() const { return {y(), w()}; }
        Vec2<T> zw() const { return {z(), w()}; }
        Vec3<T> xyz() const { return {x(), y(), z()}; }
        Vec3<T> xyw() const { return {x(), y(), w()}; }
        Vec3<T> xzw() const { return {x(), z(), w()}; }
        Vec3<T> yzw() const { return {y(), z(), w()}; }

        T& r() { return (*this)[0]; }
        T& g() { return (*this)[1]; }
        T& b() { return (*this)[2]; }
        T& a() { return (*this)[3]; }

        Vec2<T> rg() const { return {x(), y()}; }
        Vec2<T> rb() const { return {x(), z()}; }
        Vec2<T> ra() const { return {x(), w()}; }
        Vec2<T> gb() const { return {y(), z()}; }
        Vec2<T> ga() const { return {y(), w()}; }
        Vec2<T> ba() const { return {z(), w()}; }
        Vec3<T> rgb() const { return {x(), y(), z()}; }
        Vec3<T> rga() const { return {x(), y(), w()}; }
        Vec3<T> rba() const { return {x(), z(), w()}; }
        Vec3<T> gba() const { return {y(), z(), w()}; }

        const T& x() const { return (*this)[0]; }
        const T& y() const { return (*this)[1]; }
        const T& z() const { return (*this)[2]; }
        const T& w() const { return (*this)[3]; }

        const T& r() const { return (*this)[0]; }
        const T& g() const { return (*this)[1]; }
        const T& b() const { return (*this)[2]; }
        const T& a() const { return (*this)[3]; }

        Vec3<T> Homogenized() const { return (w() != 0) ? xyz() / w() : xyz(); }
    };

    using IntVec2 = Vec2<Int32>;
    using IntVec3 = Vec3<Int32>;
    using IntVec4 = Vec4<Int32>;
    using UintVec2 = Vec2<Uint32>;
    using UintVec3 = Vec3<Uint32>;
    using UintVec4 = Vec4<Uint32>;
    using FloatVec2 = Vec2<Float>;
    using FloatVec3 = Vec3<Float>;
    using FloatVec4 = Vec4<Float>;
    using BoolVec2 = Vec2<Bool>;
    using BoolVec3 = Vec3<Bool>;
    using BoolVec4 = Vec4<Bool>;

    using Size2D = IntVec2;
    using Size3D = IntVec3;
    using TextureSize = IntVec3;

    using ColorRGB = FloatVec3;
    using ColorRGBA = FloatVec4;
    using ColorRGB8 = UintVec3;
    using ColorRGBA8 = UintVec4;

    namespace MG_Util {
        template <typename Derived, typename T, SizeT N>
        Derived Lerp(const VecBase<Derived, T, N>& a, const VecBase<Derived, T, N>& b, Float t) {
            Derived result;
            for (SizeT i = 0; i < N; ++i)
                result[i] = a[i] + (b[i] - a[i]) * t;
            return result;
        }

        template <typename Derived, typename T, SizeT N>
        Derived MultiplyComponents(const VecBase<Derived, T, N>& a, const VecBase<Derived, T, N>& b) {
            Derived result;
            for (SizeT i = 0; i < N; ++i)
                result[i] = a[i] * b[i];
            return result;
        }

        template <typename Derived, typename T, SizeT N>
        T Distance(const VecBase<Derived, T, N>& a, const VecBase<Derived, T, N>& b) {
            return (a - b).Length();
        }

        inline FloatVec3 Reflect(const FloatVec3& incident, const FloatVec3& normal) {
            return incident - normal * (2.0f * incident.Dot(normal));
        }
    } // namespace MG_Util

    class VecRange1D : public Vector<Range1D> {
    public:
        void Add(const Range1D& newRange, Double ratio = 0.07, SizeT* outMinStart = nullptr,
                 SizeT* outMaxEnd = nullptr);
        SizeT GetOverallMaxEnd() const;
        SizeT GetOverallMinStart() const;

    private:
        SizeT m_overallMaxEnd;
    };
} // namespace MobileGL