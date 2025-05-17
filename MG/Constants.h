//
// Created by BZLZHH on 2025/3/15.
//

#ifndef MOBILEGL_CONSTANTS_H
#define MOBILEGL_CONSTANTS_H

#include <array>
#include <unordered_map>
#include <unordered_set>
#include <GL/gl.h>
#include <ankerl/unordered_dense.h>

#ifdef __ANDROID__
#define __ANDROID_API__ 26
#endif

#ifdef _WIN32
#define MG_EXPORT extern "C" __declspec(dllexport)
#else
#define MG_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace ankerl {
    template <typename K, typename V>
    using unordered_map = ankerl::unordered_dense::map<K, V>;
    
    template <typename T>
    using unordered_set = ankerl::unordered_dense::set<T>;
}

namespace MG_Constants {
    namespace Common {
        inline const int LOG_LEVEL_DEBUG = 0x0010;
        inline const int LOG_LEVEL_WARN = 0x0011;
        inline const int LOG_LEVEL_ERROR = 0x0012;
        inline const int LOG_LEVEL_INFO = 0x0013;
        inline const int LOG_LEVEL_FATAL = 0x0014;
    }
    
    namespace Blend {
        static const ankerl::unordered_set<GLenum> VALID_FACTORS = {
                GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
                GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_SRC_ALPHA_SATURATE
        }; // OpenGL 3
    }
    
    namespace Depth {
        static const ankerl::unordered_set<GLenum> VALID_FUNCS = {
                GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS
        }; // OpenGL 3
    }
    
    namespace CommonState {
        static const ankerl::unordered_set<GLenum> VALID_CAPS = {
                GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_CULL_FACE,
                GL_POLYGON_OFFSET_FILL, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE
        }; // OpenGL 3
    }
    
    namespace Framebuffer {
        static const ankerl::unordered_set<GLenum> VALID_ATTACHMENTS = {
                GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT,
                GL_STENCIL_ATTACHMENT, GL_DEPTH_STENCIL_ATTACHMENT
        }; // OpenGL 3
        
        static const ankerl::unordered_set<GLenum> VALID_TARGETS = {
                GL_FRAMEBUFFER, GL_DRAW_FRAMEBUFFER, GL_READ_FRAMEBUFFER
        }; // OpenGL 3
        
        static const ankerl::unordered_set<GLenum> VALID_ACCESS = {
                GL_READ_ONLY, GL_WRITE_ONLY, GL_READ_WRITE
        }; // OpenGL 3
    }
    
    namespace Buffer {
        static const ankerl::unordered_set<GLenum> VALID_PARAM_NAMES = {
                GL_BUFFER_ACCESS, GL_BUFFER_MAPPED, GL_BUFFER_SIZE, GL_BUFFER_USAGE
        }; // OpenGL 3
        
        static const ankerl::unordered_set<GLenum> VALID_TARGETS = { 
                GL_ARRAY_BUFFER, GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, GL_ELEMENT_ARRAY_BUFFER,
                GL_PIXEL_PACK_BUFFER, GL_PIXEL_UNPACK_BUFFER, GL_TEXTURE_BUFFER,
                GL_TRANSFORM_FEEDBACK_BUFFER,  GL_UNIFORM_BUFFER
        }; // OpenGL 3
        
        static const ankerl::unordered_set<GLenum> VALID_ACCESS = {
                GL_READ_ONLY, GL_WRITE_ONLY, GL_READ_WRITE
        }; // OpenGL 3
    }

    namespace PixelStore {
        static const ankerl::unordered_map<GLenum, GLint> DEFAULT_VALUES_MAP = {
                {GL_PACK_SWAP_BYTES,   GL_FALSE},
                {GL_PACK_LSB_FIRST,    GL_FALSE},
                {GL_PACK_ROW_LENGTH,   0},
                {GL_PACK_IMAGE_HEIGHT, 0},
                {GL_PACK_SKIP_ROWS,    0},
                {GL_PACK_SKIP_PIXELS,  0},
                {GL_PACK_SKIP_IMAGES,  0},
                {GL_PACK_ALIGNMENT,    4},
                {GL_UNPACK_SWAP_BYTES, GL_FALSE},
                {GL_UNPACK_LSB_FIRST,  GL_FALSE},
                {GL_UNPACK_ROW_LENGTH, 0},
                {GL_UNPACK_IMAGE_HEIGHT, 0},
                {GL_UNPACK_SKIP_ROWS,  0},
                {GL_UNPACK_SKIP_PIXELS,0},
                {GL_UNPACK_SKIP_IMAGES,0},
                {GL_UNPACK_ALIGNMENT,  4}
        }; // OpenGL 3
        
        static const ankerl::unordered_set<GLenum> VALID_PARAM_NAMES = {
                GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST, GL_PACK_ROW_LENGTH, GL_PACK_IMAGE_HEIGHT,
                GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_IMAGES, GL_PACK_ALIGNMENT,
                GL_UNPACK_SWAP_BYTES, GL_UNPACK_LSB_FIRST, GL_UNPACK_ROW_LENGTH,
                GL_UNPACK_IMAGE_HEIGHT,
                GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_IMAGES,
                GL_UNPACK_ALIGNMENT
        }; // OpenGL 3
    }
    
    namespace Texture {
        inline const GLuint MAX_TEXTURE_UNITS = 80;
        const GLsizei MAX_TEXTURE_SIZE = 32768;
        const GLsizei MAX_ARRAY_LAYERS = 256;
        inline const ankerl::unordered_set<GLenum> VALID_TARGETS = {
                GL_TEXTURE_2D, GL_PROXY_TEXTURE_2D, GL_TEXTURE_1D_ARRAY, GL_PROXY_TEXTURE_1D_ARRAY,
                GL_TEXTURE_RECTANGLE, GL_PROXY_TEXTURE_RECTANGLE, GL_TEXTURE_CUBE_MAP_POSITIVE_X,
                GL_TEXTURE_CUBE_MAP_NEGATIVE_X, GL_TEXTURE_CUBE_MAP_POSITIVE_Y, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, 
                GL_TEXTURE_CUBE_MAP_POSITIVE_Z, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, GL_PROXY_TEXTURE_CUBE_MAP
        }; // OpenGL 3

        inline const ankerl::unordered_set<GLenum> VALID_FORMATS = {
                GL_RED, GL_RG, GL_RGB, GL_BGR, GL_RGBA, GL_BGRA, GL_DEPTH_COMPONENT, GL_DEPTH_STENCIL
        }; // OpenGL 3

        inline const ankerl::unordered_set<GLenum> VALID_INTERNAL_FORMATS = {
                GL_RED, GL_RG, GL_RGB, GL_BGR, GL_RGBA, GL_BGRA, GL_DEPTH_COMPONENT, GL_DEPTH_STENCIL,
                GL_RGBA32F, GL_RGBA32I, GL_RGBA32UI, GL_RGBA16, GL_RGBA16F, GL_RGBA16I, 
                GL_RGBA16UI, GL_RGBA8, GL_RGBA8UI, GL_SRGB8_ALPHA8, GL_RGB10_A2, GL_RGB10_A2UI, 
                GL_R11F_G11F_B10F, GL_RG32F, GL_RG32I, GL_RG32UI, GL_RG16, GL_RG16F, GL_RGB16I,
                GL_RGB16UI, GL_RG8, GL_RG8I, GL_RG8UI, GL_R32F, GL_R32I, GL_R32UI, GL_R16F, 
                GL_R16I, GL_R16UI, GL_R8, GL_R8I, GL_R8UI, GL_RGBA16_SNORM, GL_RGBA8_SNORM, 
                GL_RGB32F, GL_RGB32I, GL_RGB32UI, GL_RGB16_SNORM, GL_RGB16F, GL_RGB16I, GL_RGB16UI,
                GL_RGB16, GL_RGB8_SNORM, GL_RGB8, GL_RGB8I, GL_RGB8UI, GL_SRGB8, GL_RGB9_E5, 
                GL_RG16_SNORM, GL_RG8_SNORM, GL_COMPRESSED_RG_RGTC2, GL_COMPRESSED_SIGNED_RG_RGTC2,
                GL_R16_SNORM, GL_R8_SNORM, GL_COMPRESSED_RED_RGTC1, GL_COMPRESSED_SIGNED_RED_RGTC1,
                GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT32, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT16,
                GL_DEPTH32F_STENCIL8, GL_DEPTH24_STENCIL8
        }; // OpenGL 3

        inline const ankerl::unordered_set<GLenum> VALID_TYPES = {
                GL_UNSIGNED_BYTE, GL_BYTE, GL_UNSIGNED_SHORT, GL_SHORT, GL_UNSIGNED_INT, GL_INT, 
                GL_FLOAT, GL_UNSIGNED_BYTE_3_3_2, GL_UNSIGNED_BYTE_2_3_3_REV, GL_UNSIGNED_SHORT_5_6_5, 
                GL_UNSIGNED_SHORT_5_6_5_REV, GL_UNSIGNED_SHORT_4_4_4_4, GL_UNSIGNED_SHORT_4_4_4_4_REV,
                GL_UNSIGNED_SHORT_5_5_5_1, GL_UNSIGNED_SHORT_1_5_5_5_REV, GL_UNSIGNED_INT_8_8_8_8,
                GL_UNSIGNED_INT_8_8_8_8_REV, GL_UNSIGNED_INT_10_10_10_2, GL_UNSIGNED_INT_2_10_10_10_REV
        }; // OpenGL 3

        inline const ankerl::unordered_set<GLenum> VALID_TEXTURE_PARAM_NAMES = {
                GL_TEXTURE_BASE_LEVEL, GL_TEXTURE_COMPARE_FUNC, GL_TEXTURE_COMPARE_MODE, GL_TEXTURE_MIN_FILTER, 
                GL_TEXTURE_MAG_FILTER, GL_TEXTURE_MAX_LEVEL, GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G,
                GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A, GL_TEXTURE_SWIZZLE_RGBA, GL_TEXTURE_WRAP_S,
                GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R, GL_TEXTURE_LOD_BIAS, GL_TEXTURE_MIN_LOD, GL_TEXTURE_MAX_LOD, GL_TEXTURE_BORDER_COLOR
        }; // OpenGL 3
        
        inline const ankerl::unordered_set<GLenum> VALID_QUERY_LEVEL_PROPERTY_PARAM_NAMES = {
                GL_TEXTURE_WIDTH, GL_TEXTURE_HEIGHT, GL_TEXTURE_DEPTH, GL_TEXTURE_INTERNAL_FORMAT,
                GL_TEXTURE_RED_SIZE, GL_TEXTURE_GREEN_SIZE, GL_TEXTURE_BLUE_SIZE, GL_TEXTURE_ALPHA_SIZE,
                GL_TEXTURE_DEPTH_SIZE, GL_TEXTURE_COMPRESSED, GL_TEXTURE_COMPRESSED_IMAGE_SIZE
        }; // OpenGL 3
    }

    namespace Backend {
#define BACKEND_VULKAN 0x0015
#define BACKEND_METAL 0x0016
#define BACKEND_GLES 0x0017
    }
    
    inline const char* RenderName = "MobileGL";
}

#endif //MOBILEGL_CONSTANTS_H
