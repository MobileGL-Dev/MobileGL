cmake_minimum_required(VERSION 3.22.1)

if(NOT UNIX OR APPLE OR ANDROID)
    message(STATUS "MobileGL trace replay runner is only enabled on Linux")
    return()
endif()

set(MOBILEGL_TRACE_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(MOBILEGL_TRACE_SHARED_CPP_DIR "${CMAKE_SOURCE_DIR}/android-plugin/app/src/trace/cpp")
set(APITRACE_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/apitrace")
set(APITRACE_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/apitrace")
set(APITRACE_VERSION "mobilegl-trace")
set(MOBILEGL_TRACE_REPLAY_MOBILEGL_LIBRARY "" CACHE FILEPATH "Existing libMobileGL.so to use for trace replay tests")

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

if(MOBILEGL_TRACE_REPLAY_MOBILEGL_LIBRARY)
    set(mobilegl_trace_replay_mobilegl_library "${MOBILEGL_TRACE_REPLAY_MOBILEGL_LIBRARY}")
else()
    set(mobilegl_trace_replay_mobilegl_library "$<TARGET_FILE:MobileGL>")
    add_dependencies(mobilegl_trace_replay MobileGL)
endif()

enable_testing()
function(add_trace_replay_test CASE_NAME BACKEND)
    set(oneValueArgs
            TRACE_ARCHIVE
            TRACE_FILE
            GOLDEN
            ALTERNATE_GOLDEN
            TARGET_CALL
            WIDTH
            HEIGHT
            SSIM_THRESHOLD
            CROP_X
            CROP_Y
            CROP_WIDTH
            CROP_HEIGHT)
    cmake_parse_arguments(TRACE_CASE "" "${oneValueArgs}" "" ${ARGN})
    foreach(required TRACE_ARCHIVE GOLDEN TARGET_CALL WIDTH HEIGHT)
        if(NOT TRACE_CASE_${required})
            message(FATAL_ERROR "${required} is required for ${CASE_NAME}")
        endif()
    endforeach()
    if(NOT TRACE_CASE_TRACE_FILE)
        set(TRACE_CASE_TRACE_FILE trace.trace)
    endif()
    if(NOT TRACE_CASE_SSIM_THRESHOLD)
        set(TRACE_CASE_SSIM_THRESHOLD 0.99)
    endif()
    if(NOT TRACE_CASE_CROP_X)
        set(TRACE_CASE_CROP_X 0)
    endif()
    if(NOT TRACE_CASE_CROP_Y)
        set(TRACE_CASE_CROP_Y 0)
    endif()
    if(NOT TRACE_CASE_CROP_WIDTH)
        set(TRACE_CASE_CROP_WIDTH 0)
    endif()
    if(NOT TRACE_CASE_CROP_HEIGHT)
        set(TRACE_CASE_CROP_HEIGHT 0)
    endif()
    add_test(
            NAME MobileGLTraceReplay.${CASE_NAME}.${BACKEND}
            COMMAND "${CMAKE_COMMAND}"
            -DTRACE_REPLAY_EXE=$<TARGET_FILE:mobilegl_trace_replay>
            -DMOBILEGL_LIBRARY=${mobilegl_trace_replay_mobilegl_library}
            -DTRACE_CASE_NAME=${CASE_NAME}
            -DTRACE_ARCHIVE=${TRACE_CASE_TRACE_ARCHIVE}
            -DTRACE_FILE=${TRACE_CASE_TRACE_FILE}
            -DTRACE_GOLDEN=${TRACE_CASE_GOLDEN}
            -DTRACE_ALTERNATE_GOLDEN=${TRACE_CASE_ALTERNATE_GOLDEN}
            -DTRACE_BACKEND=${BACKEND}
            -DTRACE_TARGET_CALL=${TRACE_CASE_TARGET_CALL}
            -DTRACE_WIDTH=${TRACE_CASE_WIDTH}
            -DTRACE_HEIGHT=${TRACE_CASE_HEIGHT}
            -DTRACE_SSIM_THRESHOLD=${TRACE_CASE_SSIM_THRESHOLD}
            -DTRACE_CROP_X=${TRACE_CASE_CROP_X}
            -DTRACE_CROP_Y=${TRACE_CASE_CROP_Y}
            -DTRACE_CROP_WIDTH=${TRACE_CASE_CROP_WIDTH}
            -DTRACE_CROP_HEIGHT=${TRACE_CASE_CROP_HEIGHT}
            -DTRACE_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${CASE_NAME}/${BACKEND}
            -DTRACE_ARTIFACT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${CASE_NAME}/actual-images
            -P ${MOBILEGL_TRACE_ROOT}/run_trace_case.cmake)
    if(BACKEND STREQUAL "DirectGLES")
        set_tests_properties(MobileGLTraceReplay.${CASE_NAME}.${BACKEND} PROPERTIES
                ENVIRONMENT "EGL_PLATFORM=surfaceless;LIBGL_ALWAYS_SOFTWARE=1;MESA_GL_VERSION_OVERRIDE=3.3;MESA_GLSL_VERSION_OVERRIDE=330")
    else()
        set_tests_properties(MobileGLTraceReplay.${CASE_NAME}.${BACKEND} PROPERTIES
                ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;MESA_GL_VERSION_OVERRIDE=3.3;MESA_GLSL_VERSION_OVERRIDE=330")
    endif()
endfunction()

function(add_trace_replay_test_for_backends CASE_NAME)
    add_trace_replay_test(${CASE_NAME} DirectGLES ${ARGN})
    add_trace_replay_test(${CASE_NAME} DirectVulkan ${ARGN})
endfunction()

add_trace_replay_test_for_backends(OpenRA
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/openra.tgz
        TRACE_FILE openra.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/openra.0000031249.png
        TARGET_CALL 31249
        WIDTH 640
        HEIGHT 480
        CROP_X 1
        CROP_Y 1
        CROP_WIDTH 638
        CROP_HEIGHT 478
)

add_trace_replay_test_for_backends(minecraft-1.21.4-startup
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-startup.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-startup.0000092195.png
        TARGET_CALL 92195
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-main-menu
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-main-menu.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-main-menu.0000481787.png
        ALTERNATE_GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-main-menu.0000481787-mali.png
        TARGET_CALL 481787
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-in-world.0000280000.png
        TARGET_CALL 280000
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-sodium-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-sodium-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-sodium-in-world.0000923340.png
        TARGET_CALL 923340
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test(minecraft-1.21.4-fabric-common-mods-in-world DirectVulkan
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-common-mods-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-common-mods-in-world.0000522084.png
        TARGET_CALL 522084
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test(minecraft-1.21.4-fabric-common-mods-inventory DirectVulkan
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-common-mods-inventory.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-common-mods-inventory.0000728558.png
        TARGET_CALL 728558
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-bsl-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-bsl-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-bsl-in-world.0000110725.png
        TARGET_CALL 110725
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world.0000095322.png
        TARGET_CALL 95322
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world.0000141559.png
        TARGET_CALL 141559
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-sundial-lite-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-sundial-lite-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-sundial-lite-in-world.0000150023.png
        TARGET_CALL 150023
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world.0000151297.png
        TARGET_CALL 151297
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-complementary-unbound-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-complementary-unbound-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-complementary-unbound-in-world.0000146559.png
        TARGET_CALL 146559
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-mellow-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-mellow-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-mellow-in-world.0000096143.png
        TARGET_CALL 96143
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-nostalgia-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-nostalgia-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-nostalgia-in-world.0000153808-linux-mesa.png
        ALTERNATE_GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-nostalgia-in-world.0000153808.png
        TARGET_CALL 153808
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-bliss-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-bliss-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-bliss-in-world.0000113511.png
        TARGET_CALL 113511
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world.0000125124-linux-mesa.png
        TARGET_CALL 125124
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-iterationt-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-iterationt-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-iterationt-in-world.0000110538.png
        TARGET_CALL 110538
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world.0000115019.png
        TARGET_CALL 115019
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-photon-v1.1-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-photon-v1.1-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-photon-v1.1-in-world.0000159866.png
        TARGET_CALL 159866
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world.0000172128.png
        TARGET_CALL 172128
        WIDTH 854
        HEIGHT 480
)

add_trace_replay_test_for_backends(minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world
        TRACE_ARCHIVE ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world.tgz
        TRACE_FILE trace.trace
        GOLDEN ${MOBILEGL_TRACE_ROOT}/fixtures/minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world.0000145353.png
        TARGET_CALL 145353
        WIDTH 854
        HEIGHT 480
)
