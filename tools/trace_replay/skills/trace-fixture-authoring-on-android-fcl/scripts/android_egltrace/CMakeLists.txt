cmake_minimum_required(VERSION 3.22.1)

project(mobilegl_android_egltrace)

set(APITRACE_ROOT "" CACHE PATH "Path to apitrace source tree")
set(PATCHED_EGLTRACE_CPP "" CACHE FILEPATH "Generated and patched egltrace.cpp")
set(PATCHED_GLPROC_EGL_CPP "" CACHE FILEPATH "Patched glproc_egl.cpp")
if(NOT EXISTS "${APITRACE_ROOT}/wrappers")
    message(FATAL_ERROR "APITRACE_ROOT must point to apitrace")
endif()
if(NOT EXISTS "${PATCHED_EGLTRACE_CPP}")
    message(FATAL_ERROR "PATCHED_EGLTRACE_CPP is required")
endif()
if(NOT EXISTS "${PATCHED_GLPROC_EGL_CPP}")
    message(FATAL_ERROR "PATCHED_GLPROC_EGL_CPP is required")
endif()

set(APITRACE_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/apitrace")
set(APITRACE_VERSION "mobilegl-capture")

find_package(Python3 REQUIRED)
find_package(Threads REQUIRED)

include("${APITRACE_ROOT}/cmake/ConvenienceLibrary.cmake")

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC_SNAPPY ON CACHE BOOL "" FORCE)
set(DOC_INSTALL_DIR "doc" CACHE PATH "" FORCE)
set(HAVE_X86 OFF CACHE BOOL "" FORCE)
set(ZLIB_FOUND OFF CACHE BOOL "" FORCE)
set(PNG_FOUND OFF CACHE BOOL "" FORCE)
set(Snappy_FOUND OFF CACHE BOOL "" FORCE)
set(BROTLIDEC_FOUND OFF CACHE BOOL "" FORCE)
set(BROTLIENC_FOUND OFF CACHE BOOL "" FORCE)
set(ZSTD_FOUND OFF CACHE BOOL "" FORCE)
set(CMAKE_EXECUTABLE_FORMAT "MobileGLAndroid" CACHE INTERNAL "" FORCE)

add_custom_target(check)
add_subdirectory("${APITRACE_ROOT}/thirdparty" "${APITRACE_BINARY_DIR}/thirdparty")

set(APITRACE_GENERATED_DIR "${APITRACE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${APITRACE_GENERATED_DIR}")
configure_file("${APITRACE_ROOT}/version.h.in" "${APITRACE_GENERATED_DIR}/version.h" @ONLY)

add_custom_command(
        OUTPUT
        "${APITRACE_GENERATED_DIR}/glproc.hpp"
        "${APITRACE_GENERATED_DIR}/glproc.cpp"
        COMMAND ${Python3_EXECUTABLE}
        "${APITRACE_ROOT}/dispatch/glproc.py"
        "${APITRACE_GENERATED_DIR}/glproc.hpp"
        "${APITRACE_GENERATED_DIR}/glproc.cpp"
        DEPENDS
        "${APITRACE_ROOT}/dispatch/glproc.py"
        "${APITRACE_ROOT}/dispatch/dispatch.py"
        "${APITRACE_ROOT}/specs/wglapi.py"
        "${APITRACE_ROOT}/specs/glxapi.py"
        "${APITRACE_ROOT}/specs/cglapi.py"
        "${APITRACE_ROOT}/specs/eglapi.py"
        "${APITRACE_ROOT}/specs/glapi.py"
        "${APITRACE_ROOT}/specs/gltypes.py"
        "${APITRACE_ROOT}/specs/stdapi.py")

add_library(apitrace_os STATIC
        "${APITRACE_ROOT}/lib/os/os_backtrace.cpp"
        "${APITRACE_ROOT}/lib/os/os_crtdbg.cpp"
        "${APITRACE_ROOT}/lib/os/os_posix.cpp")
target_include_directories(apitrace_os PUBLIC
        "${APITRACE_ROOT}/compat"
        "${APITRACE_ROOT}/thirdparty"
        "${APITRACE_ROOT}/lib/os"
        "${APITRACE_ROOT}/lib/trace")
target_link_libraries(apitrace_os PUBLIC Threads::Threads)

add_library(glproc STATIC
        "${APITRACE_GENERATED_DIR}/glproc.cpp"
        "${PATCHED_GLPROC_EGL_CPP}")
target_include_directories(glproc PUBLIC
        "${APITRACE_GENERATED_DIR}"
        "${APITRACE_ROOT}/wrappers"
        "${APITRACE_ROOT}/dispatch"
        "${APITRACE_ROOT}/lib/os"
        "${APITRACE_ROOT}/thirdparty/khronos")
target_link_libraries(glproc PUBLIC apitrace_os dl)

add_library(highlight STATIC "${APITRACE_ROOT}/lib/highlight/highlight.cpp")
target_include_directories(highlight PUBLIC "${APITRACE_ROOT}/lib/highlight")

add_library(guids STATIC "${APITRACE_ROOT}/lib/guids/guids.cpp")
target_include_directories(guids PUBLIC
        "${APITRACE_ROOT}/lib/guids"
        "${APITRACE_ROOT}/lib/os")

add_library(common STATIC
        "${APITRACE_ROOT}/lib/trace/trace_callset.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_dump.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_fast_callset.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_file.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_file_read.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_file_zlib.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_file_brotli.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_file_snappy.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_file_zstd.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_file_zstd_seekable.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_model.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_option.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_ostream_snappy.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_ostream_zlib.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_ostream_zstd.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_parser.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_parser_flags.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_parser_loop.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_profiler.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_writer.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_writer_local.cpp"
        "${APITRACE_ROOT}/lib/trace/trace_writer_model.cpp")
target_include_directories(common PUBLIC
        "${APITRACE_ROOT}/compat"
        "${APITRACE_ROOT}/thirdparty"
        "${APITRACE_ROOT}/lib/guids"
        "${APITRACE_ROOT}/lib/highlight"
        "${APITRACE_ROOT}/lib/os"
        "${APITRACE_ROOT}/lib/trace"
        "${APITRACE_ROOT}/lib/ubjson")
target_link_libraries(common PUBLIC
        guids
        highlight
        apitrace_os
        Snappy::snappy
        ZLIB::ZLIB
        PkgConfig::BROTLIDEC
        PkgConfig::ZSTD
        zstd_seekable)

add_convenience_library(trace
        "${APITRACE_ROOT}/wrappers/memtrace.hpp"
        "${APITRACE_ROOT}/wrappers/memtrace.cpp")
target_include_directories(trace PUBLIC
        "${APITRACE_ROOT}/thirdparty/crc32c")
target_link_libraries(trace
        common
        guids
        crc32c)

add_library(glhelpers STATIC
        "${APITRACE_ROOT}/helpers/glfeatures.cpp"
        "${APITRACE_ROOT}/helpers/eglsize.cpp")
target_include_directories(glhelpers PUBLIC
        "${APITRACE_GENERATED_DIR}"
        "${APITRACE_ROOT}/dispatch"
        "${APITRACE_ROOT}/helpers"
        "${APITRACE_ROOT}/lib/os"
        "${APITRACE_ROOT}/thirdparty/khronos")
target_link_libraries(glhelpers PUBLIC glproc apitrace_os)

add_convenience_library(gltrace_common
        "${APITRACE_ROOT}/wrappers/glcaps.cpp"
        "${APITRACE_ROOT}/wrappers/config.cpp"
        "${APITRACE_ROOT}/wrappers/gltrace_arrays.cpp"
        "${APITRACE_ROOT}/wrappers/gltrace_state.cpp"
        "${APITRACE_ROOT}/wrappers/glmemshadow.hpp"
        "${APITRACE_ROOT}/wrappers/glmemshadow.cpp"
        "${APITRACE_ROOT}/wrappers/gltrace_unpack_compressed.hpp"
        "${APITRACE_ROOT}/wrappers/gltrace_unpack_compressed.cpp")
add_dependencies(gltrace_common glproc)
target_include_directories(gltrace_common PUBLIC
        "${APITRACE_ROOT}/wrappers")
target_link_libraries(gltrace_common
        glhelpers
        trace)

add_library(egltrace SHARED
        "${PATCHED_EGLTRACE_CPP}"
        "${APITRACE_ROOT}/wrappers/dlsym.cpp"
        "${PATCHED_GLPROC_EGL_CPP}")
add_dependencies(egltrace glproc)
set_target_properties(egltrace PROPERTIES PREFIX "")
target_compile_definitions(egltrace PRIVATE -DEGLTRACE=1)
target_include_directories(egltrace PRIVATE
        "${APITRACE_ROOT}/wrappers"
        "${APITRACE_GENERATED_DIR}"
        "${APITRACE_ROOT}/helpers"
        "${APITRACE_ROOT}/dispatch"
        "${APITRACE_ROOT}/lib/os"
        "${APITRACE_ROOT}/lib/trace"
        "${APITRACE_ROOT}/thirdparty/khronos")
target_link_libraries(egltrace
        gltrace_common
        glproc
        Threads::Threads
        dl)
