cmake_minimum_required(VERSION 3.22.1)

project(trace_replay_runner)

set(APITRACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../third_party/apitrace")
set(APITRACE_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/apitrace")
set(APITRACE_VERSION "mobilegl-trace")

find_package(Python3 REQUIRED)
find_package(Threads REQUIRED)

include("${APITRACE_ROOT}/cmake/ConvenienceLibrary.cmake")

function(target_optimize TARGET)
    target_compile_definitions(${TARGET} PRIVATE -DNDEBUG)
    target_compile_options(${TARGET} PRIVATE -O3)
endfunction()

function(include_with_scope)
    include(${ARGV})
endfunction()

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

add_library(glproc STATIC
        "${APITRACE_GENERATED_DIR}/glproc.cpp"
        apitrace_glproc_android.cpp)
target_include_directories(glproc PUBLIC
        "${APITRACE_GENERATED_DIR}"
        "${APITRACE_ROOT}/dispatch"
        "${APITRACE_ROOT}/lib/os"
        "${APITRACE_ROOT}/thirdparty/khronos")
target_link_libraries(glproc PUBLIC apitrace_os dl)

execute_process(
        COMMAND ${Python3_EXECUTABLE} "${APITRACE_ROOT}/retrace/glretrace.py"
        OUTPUT_FILE "${APITRACE_GENERATED_DIR}/glretrace_gl.cpp"
        COMMAND_ERROR_IS_FATAL ANY)

execute_process(
        COMMAND ${Python3_EXECUTABLE} "${APITRACE_ROOT}/retrace/glstate_params.py"
        OUTPUT_FILE "${APITRACE_GENERATED_DIR}/glstate_params.cpp"
        COMMAND_ERROR_IS_FATAL ANY)

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

add_library(highlight STATIC "${APITRACE_ROOT}/lib/highlight/highlight.cpp")
target_include_directories(highlight PUBLIC "${APITRACE_ROOT}/lib/highlight")

add_library(guids STATIC "${APITRACE_ROOT}/lib/guids/guids.cpp")
target_include_directories(guids PUBLIC
        "${APITRACE_ROOT}/lib/guids"
        "${APITRACE_ROOT}/lib/os")

add_library(image STATIC
        "${APITRACE_ROOT}/lib/image/image_bmp.cpp"
        "${APITRACE_ROOT}/lib/image/image_png.cpp"
        "${APITRACE_ROOT}/lib/image/image_pnm.cpp"
        "${APITRACE_ROOT}/lib/image/image_raw.cpp"
        "${APITRACE_ROOT}/lib/image/image_md5.cpp")
target_include_directories(image PUBLIC "${APITRACE_ROOT}/lib/image")
target_link_libraries(image PUBLIC md5 PNG::PNG)

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

add_library(retrace_common STATIC
        "${APITRACE_ROOT}/retrace/json.cpp"
        "${APITRACE_ROOT}/retrace/process_name.cpp"
        "${APITRACE_ROOT}/retrace/retrace.cpp"
        "${APITRACE_ROOT}/retrace/retrace_main.cpp"
        "${APITRACE_ROOT}/retrace/retrace_stdc.cpp"
        "${APITRACE_ROOT}/retrace/retrace_swizzle.cpp"
        "${APITRACE_ROOT}/retrace/state_writer.cpp"
        "${APITRACE_ROOT}/retrace/state_writer_json.cpp"
        "${APITRACE_ROOT}/retrace/state_writer_ubjson.cpp"
        "${APITRACE_ROOT}/retrace/ws.cpp")
target_compile_definitions(retrace_common PUBLIC RETRACE)
target_compile_definitions(retrace_common PRIVATE exit=mobilegl_apitrace_exit)
target_compile_options(retrace_common PRIVATE "-include${CMAKE_CURRENT_LIST_DIR}/apitrace_exit.hpp")
target_include_directories(retrace_common PUBLIC
        "${APITRACE_GENERATED_DIR}"
        "${APITRACE_ROOT}/compat"
        "${APITRACE_ROOT}/dispatch"
        "${APITRACE_ROOT}/helpers"
        "${APITRACE_ROOT}/lib/highlight"
        "${APITRACE_ROOT}/lib/image"
        "${APITRACE_ROOT}/lib/os"
        "${APITRACE_ROOT}/lib/trace"
        "${APITRACE_ROOT}/lib/ubjson"
        "${APITRACE_ROOT}/retrace"
        "${APITRACE_ROOT}/thirdparty"
        "${APITRACE_ROOT}/thirdparty/khronos")
target_link_libraries(retrace_common PUBLIC image common)

add_library(glretrace_common STATIC
        "${APITRACE_GENERATED_DIR}/glretrace_gl.cpp"
        "${APITRACE_GENERATED_DIR}/glstate_params.cpp"
        "${APITRACE_ROOT}/retrace/glretrace_cgl.cpp"
        "${APITRACE_ROOT}/retrace/glretrace_egl.cpp"
        # GLX here is only the apitrace trace-call frontend. Android execution
        # still goes through apitrace_glws_android.cpp and MobileGL's EGL entrypoints.
        "${APITRACE_ROOT}/retrace/glretrace_glx.cpp"
        "${APITRACE_ROOT}/retrace/glretrace_main.cpp"
        "${APITRACE_ROOT}/retrace/glretrace_wgl.cpp"
        "${APITRACE_ROOT}/retrace/glretrace_wgl_font_bitmaps.cpp"
        "${APITRACE_ROOT}/retrace/glretrace_wgl_font_outlines.cpp"
        "${APITRACE_ROOT}/retrace/glretrace_ws.cpp"
        "${APITRACE_ROOT}/retrace/glstate.cpp"
        "${APITRACE_ROOT}/retrace/glstate_formats.cpp"
        "${APITRACE_ROOT}/retrace/glstate_images.cpp"
        "${APITRACE_ROOT}/retrace/glstate_shaders.cpp"
        "${APITRACE_ROOT}/retrace/glws.cpp"
        "${APITRACE_ROOT}/retrace/metric_backend_amd_perfmon.cpp"
        "${APITRACE_ROOT}/retrace/metric_backend_intel_perfquery.cpp"
        "${APITRACE_ROOT}/retrace/metric_backend_opengl.cpp"
        "${APITRACE_ROOT}/retrace/metric_helper.cpp"
        "${APITRACE_ROOT}/retrace/metric_writer.cpp"
        apitrace_glws_android.cpp)
target_include_directories(glretrace_common PUBLIC
        "${APITRACE_GENERATED_DIR}"
        "${APITRACE_ROOT}/dispatch"
        "${APITRACE_ROOT}/helpers"
        "${APITRACE_ROOT}/retrace")
target_compile_definitions(glretrace_common PRIVATE exit=mobilegl_apitrace_exit)
target_compile_options(glretrace_common PRIVATE "-include${CMAKE_CURRENT_LIST_DIR}/apitrace_exit.hpp")
target_link_libraries(glretrace_common PUBLIC retrace_common glhelpers glproc)

add_library(trace_replay_runner SHARED
        trace_replay_core.cpp
        trace_replay_jni.cpp)

target_compile_features(trace_replay_runner PRIVATE cxx_std_17)

target_include_directories(trace_replay_runner PRIVATE
        "${APITRACE_ROOT}/lib/image")

target_link_libraries(trace_replay_runner
        glretrace_common
        retrace_common
        image
        android
        dl)
