foreach(required TRACE_REPLAY_EXE MOBILEGL_LIBRARY OPENRA_TRACE_ARCHIVE OPENRA_GOLDEN OPENRA_OUTPUT_DIR)
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
        --backend DirectGLES
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

if(NOT replay_result EQUAL 0)
    message(FATAL_ERROR "OpenRA trace replay failed with status ${replay_result}")
endif()
