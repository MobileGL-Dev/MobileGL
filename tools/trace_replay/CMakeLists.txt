cmake_minimum_required(VERSION 3.22.1)

if(NOT UNIX OR APPLE OR ANDROID)
    message(STATUS "MobileGL trace replay runner is only enabled on Linux")
    return()
endif()

set(MOBILEGL_TRACE_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(MOBILEGL_TRACE_SHARED_CPP_DIR "${CMAKE_SOURCE_DIR}/android-plugin/app/src/trace/cpp")
set(APITRACE_ROOT "${CMAKE_SOURCE_DIR}/android-plugin/third_party/apitrace")
set(APITRACE_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/apitrace")
set(APITRACE_VERSION "mobilegl-trace")

find_package(Python3 REQUIRED)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

if(NOT TARGET check)
    add_custom_target(check)
endif()
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

add_library(mobilegl_trace_apitrace_os STATIC
        "${APITRACE_ROOT}/lib/os/os_backtrace.cpp"
        "${APITRACE_ROOT}/lib/os/os_crtdbg.cpp"
        "${APITRACE_ROOT}/lib/os/os_posix.cpp")
target_include_directories(mobilegl_trace_apitrace_os PUBLIC
        "${APITRACE_ROOT}/compat"
        "${APITRACE_ROOT}/thirdparty"
        "${APITRACE_ROOT}/lib/os"
        "${APITRACE_ROOT}/lib/trace")
target_link_libraries(mobilegl_trace_apitrace_os PUBLIC Threads::Threads)

add_library(mobilegl_trace_highlight STATIC "${APITRACE_ROOT}/lib/highlight/highlight.cpp")
target_include_directories(mobilegl_trace_highlight PUBLIC "${APITRACE_ROOT}/lib/highlight")

add_library(mobilegl_trace_guids STATIC "${APITRACE_ROOT}/lib/guids/guids.cpp")
target_include_directories(mobilegl_trace_guids PUBLIC
        "${APITRACE_ROOT}/lib/guids"
        "${APITRACE_ROOT}/lib/os")

add_library(mobilegl_trace_image STATIC
        "${APITRACE_ROOT}/lib/image/image_bmp.cpp"
        "${APITRACE_ROOT}/lib/image/image_png.cpp"
        "${APITRACE_ROOT}/lib/image/image_pnm.cpp"
        "${APITRACE_ROOT}/lib/image/image_raw.cpp"
        "${APITRACE_ROOT}/lib/image/image_md5.cpp")
target_include_directories(mobilegl_trace_image PUBLIC "${APITRACE_ROOT}/lib/image")
target_link_libraries(mobilegl_trace_image PUBLIC md5 PNG::PNG)

add_library(mobilegl_trace_common STATIC
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
target_include_directories(mobilegl_trace_common PUBLIC
        "${APITRACE_ROOT}/compat"
        "${APITRACE_ROOT}/thirdparty"
        "${APITRACE_ROOT}/lib/guids"
        "${APITRACE_ROOT}/lib/highlight"
        "${APITRACE_ROOT}/lib/os"
        "${APITRACE_ROOT}/lib/trace"
        "${APITRACE_ROOT}/lib/ubjson")
target_link_libraries(mobilegl_trace_common PUBLIC
        mobilegl_trace_guids
        mobilegl_trace_highlight
        mobilegl_trace_apitrace_os
        Snappy::snappy
        ZLIB::ZLIB
        PkgConfig::BROTLIDEC
        PkgConfig::ZSTD
        zstd_seekable)

add_library(mobilegl_trace_glproc STATIC
        "${APITRACE_GENERATED_DIR}/glproc.cpp"
        "${MOBILEGL_TRACE_ROOT}/apitrace_glproc_mobilegl.cpp")
target_include_directories(mobilegl_trace_glproc PUBLIC
        "${APITRACE_GENERATED_DIR}"
        "${APITRACE_ROOT}/dispatch"
        "${APITRACE_ROOT}/lib/os"
        "${APITRACE_ROOT}/thirdparty/khronos")
target_link_libraries(mobilegl_trace_glproc PUBLIC mobilegl_trace_apitrace_os dl)

execute_process(
        COMMAND ${Python3_EXECUTABLE} "${APITRACE_ROOT}/retrace/glretrace.py"
        OUTPUT_FILE "${APITRACE_GENERATED_DIR}/glretrace_gl.cpp"
        COMMAND_ERROR_IS_FATAL ANY)

execute_process(
        COMMAND ${Python3_EXECUTABLE} "${APITRACE_ROOT}/retrace/glstate_params.py"
        OUTPUT_FILE "${APITRACE_GENERATED_DIR}/glstate_params.cpp"
        COMMAND_ERROR_IS_FATAL ANY)

add_library(mobilegl_trace_glhelpers STATIC
        "${APITRACE_ROOT}/helpers/glfeatures.cpp"
        "${APITRACE_ROOT}/helpers/eglsize.cpp")
target_include_directories(mobilegl_trace_glhelpers PUBLIC
        "${APITRACE_GENERATED_DIR}"
        "${APITRACE_ROOT}/dispatch"
        "${APITRACE_ROOT}/helpers"
        "${APITRACE_ROOT}/lib/os"
        "${APITRACE_ROOT}/thirdparty/khronos")
target_link_libraries(mobilegl_trace_glhelpers PUBLIC mobilegl_trace_glproc mobilegl_trace_apitrace_os)

add_library(mobilegl_trace_retrace_common STATIC
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
target_compile_definitions(mobilegl_trace_retrace_common PUBLIC RETRACE)
target_compile_definitions(mobilegl_trace_retrace_common PRIVATE
        exit=mobilegl_apitrace_exit
        main=mobilegl_apitrace_main)
target_compile_options(mobilegl_trace_retrace_common PRIVATE "-include${MOBILEGL_TRACE_SHARED_CPP_DIR}/apitrace_exit.hpp")
target_include_directories(mobilegl_trace_retrace_common PUBLIC
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
target_link_libraries(mobilegl_trace_retrace_common PUBLIC mobilegl_trace_image mobilegl_trace_common)

add_library(mobilegl_trace_glretrace_common STATIC
        "${APITRACE_GENERATED_DIR}/glretrace_gl.cpp"
        "${APITRACE_GENERATED_DIR}/glstate_params.cpp"
        "${APITRACE_ROOT}/retrace/glretrace_cgl.cpp"
        "${APITRACE_ROOT}/retrace/glretrace_egl.cpp"
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
        "${MOBILEGL_TRACE_ROOT}/apitrace_glws_egl.cpp")
target_include_directories(mobilegl_trace_glretrace_common PUBLIC
        "${APITRACE_GENERATED_DIR}"
        "${APITRACE_ROOT}/dispatch"
        "${APITRACE_ROOT}/helpers"
        "${APITRACE_ROOT}/retrace")
target_compile_definitions(mobilegl_trace_glretrace_common PRIVATE
        exit=mobilegl_apitrace_exit
        main=mobilegl_apitrace_main)
target_compile_options(mobilegl_trace_glretrace_common PRIVATE "-include${MOBILEGL_TRACE_SHARED_CPP_DIR}/apitrace_exit.hpp")
target_link_libraries(mobilegl_trace_glretrace_common PUBLIC
        mobilegl_trace_retrace_common
        mobilegl_trace_glhelpers
        mobilegl_trace_glproc)

add_executable(mobilegl_trace_replay
        "${MOBILEGL_TRACE_SHARED_CPP_DIR}/trace_replay_core.cpp"
        "${MOBILEGL_TRACE_ROOT}/trace_replay_cli.cpp")
target_compile_features(mobilegl_trace_replay PRIVATE cxx_std_17)
target_compile_definitions(mobilegl_trace_replay PRIVATE
        MOBILEGL_APITRACE_RETRACE_MAIN=mobilegl_apitrace_main)
target_include_directories(mobilegl_trace_replay PRIVATE
        "${MOBILEGL_TRACE_SHARED_CPP_DIR}"
        "${APITRACE_ROOT}/lib/image")
target_link_libraries(mobilegl_trace_replay PRIVATE
        "-Wl,--start-group"
        mobilegl_trace_glretrace_common
        mobilegl_trace_retrace_common
        mobilegl_trace_image
        mobilegl_trace_common
        mobilegl_trace_glhelpers
        mobilegl_trace_glproc
        mobilegl_trace_apitrace_os
        "-Wl,--end-group"
        dl)
add_dependencies(mobilegl_trace_replay MobileGL)

enable_testing()
function(add_openra_trace_replay_test BACKEND)
    add_test(
            NAME MobileGLTraceReplay.OpenRA.${BACKEND}
            COMMAND "${CMAKE_COMMAND}"
            -DTRACE_REPLAY_EXE=$<TARGET_FILE:mobilegl_trace_replay>
            -DMOBILEGL_LIBRARY=$<TARGET_FILE:MobileGL>
            -DOPENRA_TRACE_ARCHIVE=${MOBILEGL_TRACE_ROOT}/fixtures/openra.tgz
            -DOPENRA_GOLDEN=${MOBILEGL_TRACE_ROOT}/fixtures/openra.0000031249.png
            -DOPENRA_BACKEND=${BACKEND}
            -DOPENRA_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/openra/${BACKEND}
            -DOPENRA_ARTIFACT_DIR=${CMAKE_CURRENT_BINARY_DIR}/openra/actual-images
            -P ${MOBILEGL_TRACE_ROOT}/run_openra_replay.cmake)
    set_tests_properties(MobileGLTraceReplay.OpenRA.${BACKEND} PROPERTIES
            ENVIRONMENT "EGL_PLATFORM=surfaceless;LIBGL_ALWAYS_SOFTWARE=1;MESA_GL_VERSION_OVERRIDE=3.3;MESA_GLSL_VERSION_OVERRIDE=330")
endfunction()

add_openra_trace_replay_test(DirectGLES)
add_openra_trace_replay_test(DirectVulkan)
