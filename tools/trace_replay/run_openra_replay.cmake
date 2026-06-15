foreach(required TRACE_REPLAY_EXE MOBILEGL_LIBRARY OPENRA_TRACE_ARCHIVE OPENRA_GOLDEN OPENRA_OUTPUT_DIR OPENRA_BACKEND)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

if(EXISTS "${OPENRA_OUTPUT_DIR}")
    file(REMOVE_RECURSE "${OPENRA_OUTPUT_DIR}")
endif()
file(MAKE_DIRECTORY "${OPENRA_OUTPUT_DIR}/input")
file(MAKE_DIRECTORY "${OPENRA_OUTPUT_DIR}/output")

execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xzf "${OPENRA_TRACE_ARCHIVE}"
        WORKING_DIRECTORY "${OPENRA_OUTPUT_DIR}/input"
        RESULT_VARIABLE extract_result
        OUTPUT_VARIABLE extract_stdout
        ERROR_VARIABLE extract_stderr)
if(NOT extract_result EQUAL 0)
    message(STATUS "${extract_stdout}")
    message(STATUS "${extract_stderr}")
    message(FATAL_ERROR "failed to extract ${OPENRA_TRACE_ARCHIVE}")
endif()

set(openra_trace "${OPENRA_OUTPUT_DIR}/input/openra.trace")
if(NOT EXISTS "${openra_trace}")
    message(FATAL_ERROR "extracted OpenRA trace was not found at ${openra_trace}")
endif()

execute_process(
        COMMAND "${TRACE_REPLAY_EXE}"
        --trace "${openra_trace}"
        --golden "${OPENRA_GOLDEN}"
        --output "${OPENRA_OUTPUT_DIR}/output"
        --backend "${OPENRA_BACKEND}"
        --mobilegl-library "${MOBILEGL_LIBRARY}"
        --target-call 31249
        --width 640
        --height 480
        --tolerance 20
        --crop-x 1
        --crop-y 1
        --crop-width 638
        --crop-height 478
        --fuzz-percent 20
        RESULT_VARIABLE replay_result
        OUTPUT_VARIABLE replay_stdout
        ERROR_VARIABLE replay_stderr)

message(STATUS "${replay_stdout}")
message(STATUS "${replay_stderr}")

set(result_json "${OPENRA_OUTPUT_DIR}/output/result.json")
if(EXISTS "${result_json}")
    file(READ "${result_json}" result_contents)
    message(STATUS "${result_contents}")
else()
    message(FATAL_ERROR "trace replay did not write ${result_json}")
endif()

set(retrace_log "${OPENRA_OUTPUT_DIR}/output/retrace.log")
if(EXISTS "${retrace_log}")
    file(STRINGS "${retrace_log}" gl_identity_lines REGEX "MOBILEGL_TRACE_GL_")
    foreach(line IN LISTS gl_identity_lines)
        message(STATUS "${line}")
    endforeach()
endif()

if(DEFINED OPENRA_ARTIFACT_DIR AND NOT "${OPENRA_ARTIFACT_DIR}" STREQUAL "")
    file(MAKE_DIRECTORY "${OPENRA_ARTIFACT_DIR}")
    set(actual_png "${OPENRA_OUTPUT_DIR}/output/actual.png")
    if(EXISTS "${actual_png}")
        file(COPY_FILE "${actual_png}" "${OPENRA_ARTIFACT_DIR}/openra-${OPENRA_BACKEND}-actual.png")
    endif()
    if(EXISTS "${result_json}")
        file(COPY_FILE "${result_json}" "${OPENRA_ARTIFACT_DIR}/openra-${OPENRA_BACKEND}-result.json")
    endif()
    if(EXISTS "${retrace_log}")
        file(COPY_FILE "${retrace_log}" "${OPENRA_ARTIFACT_DIR}/openra-${OPENRA_BACKEND}-retrace.log")
    endif()
endif()

if(NOT replay_result EQUAL 0)
    message(FATAL_ERROR "OpenRA ${OPENRA_BACKEND} trace replay failed with status ${replay_result}")
endif()
