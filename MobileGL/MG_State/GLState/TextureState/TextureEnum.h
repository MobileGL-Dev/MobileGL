// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureEnum.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

namespace MobileGL {
    enum class TextureTarget {
        Texture1D = 0,
        Texture2D,
        Texture3D,
        TextureCubeMap,
        TextureRectangle,
        Texture2DMultisample,
        TextureBuffer,
        Texture1DArray,
        Texture2DArray,
        TextureCubeMapArray,
        Texture2DMultisampleArray,
        TextureTargetCount,
        Unknown = -1
    };

    // Don't tinker with order in this enum (especially CubeMap faces),
    // it is used in MipmapUploadTargetArray
    enum class TextureUploadTarget {
        Texture1D = 0,
        Texture2D,
        Texture3D,
        ProxyTexture1D,
        ProxyTexture2D,
        ProxyTexture3D,
        Texture1DArray,
        Texture2DArray,
        ProxyTexture1DArray,
        ProxyTexture2DArray,
        TextureRectangle,
        ProxyTextureRectangle,
        CubeMapPositiveX,
        CubeMapNegativeX,
        CubeMapPositiveY,
        CubeMapNegativeY,
        CubeMapPositiveZ,
        CubeMapNegativeZ,
        TextureBuffer,
        ProxyCubeMap,
        CubeMapArray,
        ProxyCubeMapArray,
        Texture2DMultisample,
        Texture2DMultisampleArray,
        ProxyTexture2DMultisample,
        ProxyTexture2DMultisampleArray,
        TextureUploadTargetCount,
        Unknown = -1
    };

    enum class TextureStorageType {
        Mipmap,
        Buffer
    };

    enum class TextureInputFormat {
        Red,
        RG,
        RGB,
        BGR,
        RGBA,
        BGRA,
        RInteger,
        RGInteger,
        RGBInteger,
        BGRInteger,
        RGBAInteger,
        BGRAInteger,
        StencilIndex,
        DepthComponent,
        DepthStencil,

        FormatCount,
        Unknown = -1
    };

    enum class TextureInternalFormat {
        R8,
        R8Snorm,
        R16,
        R16Snorm,
        RG8,
        RG8Snorm,
        RG16,
        RG16Snorm,
        R3G3B2,
        RGB4,
        RGB5,
        RGB8,
        RGB8Snorm,
        RGB10,
        RGB12,
        RGB16,
        RGB16Snorm,
        RGBA2,
        RGBA4,
        RGB5A1,
        RGBA8,
        RGBA8Snorm,
        RGB10A2,
        RGB10A2UI,
        RGBA12,
        RGBA16,
        RGBA16Snorm,
        SRGB8,
        SRGB8Alpha8,
        R16F,
        RG16F,
        RGB16F,
        RGBA16F,
        R32F,
        RG32F,
        RGB32F,
        RGBA32F,
        R11FG11FB10F,
        RGB9E5,
        R8I,
        R8UI,
        R16I,
        R16UI,
        R32I,
        R32UI,
        RG8I,
        RG8UI,
        RG16I,
        RG16UI,
        RG32I,
        RG32UI,
        RGB8I,
        RGB8UI,
        RGB16I,
        RGB16UI,
        RGB32I,
        RGB32UI,
        RGBA8I,
        RGBA8UI,
        RGBA16I,
        RGBA16UI,
        RGBA32I,
        RGBA32UI,
        DepthComponent16,
        DepthComponent24,
        DepthComponent32, // not a standard format in OpenGL core profile
        DepthComponent32F,
        Depth24Stencil8,
        Depth32FStencil8,

        DepthComponent,
        DepthStencil,
        Red,
        RG,
        RGB,
        RGBA,

        TextureInternalFormatCount,
        Unknown = -1
    };

    enum class TexturePixelDataType {
        UnsignedByte,
        Byte,
        UnsignedShort,
        Short,
        UnsignedInt,
        Int,
        Float,
        HalfFloat,
        UnsignedInt248,
        UnsignedByte332,
        UnsignedByte233Rev,
        UnsignedShort565,
        UnsignedShort565Rev,
        UnsignedShort4444,
        UnsignedShort4444Rev,
        UnsignedShort5551,
        UnsignedShort1555Rev,
        UnsignedInt8888,
        UnsignedInt8888Rev,
        UnsignedInt1010102,
        UnsignedInt2101010Rev,
        UnsignedInt101111Rev,
        UnsignedInt5999Rev,
        Float32UnsignedInt248Rev,

        TypeCount,
        Unknown = -1
    };

    enum class TextureSwizzleParam {
        Red,
        Green,
        Blue,
        Alpha,
        Zero,
        One,

        SwizzleParamCount,
        Unknown = -1
    };
} // namespace MobileGL
