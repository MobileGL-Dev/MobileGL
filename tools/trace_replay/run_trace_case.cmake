foreach(required TRACE_REPLAY_EXE MOBILEGL_LIBRARY TRACE_ARCHIVE TRACE_FILE TRACE_GOLDEN TRACE_OUTPUT_DIR TRACE_BACKEND TRACE_CASE_NAME TRACE_TARGET_CALL TRACE_WIDTH TRACE_HEIGHT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

if(NOT DEFINED TRACE_TOLERANCE OR "${TRACE_TOLERANCE}" STREQUAL "")
    set(TRACE_TOLERANCE 0)
endif()
if(NOT DEFINED TRACE_CROP_X OR "${TRACE_CROP_X}" STREQUAL "")
    set(TRACE_CROP_X 0)
endif()
if(NOT DEFINED TRACE_CROP_Y OR "${TRACE_CROP_Y}" STREQUAL "")
    set(TRACE_CROP_Y 0)
endif()
if(NOT DEFINED TRACE_CROP_WIDTH OR "${TRACE_CROP_WIDTH}" STREQUAL "")
    set(TRACE_CROP_WIDTH 0)
endif()
if(NOT DEFINED TRACE_CROP_HEIGHT OR "${TRACE_CROP_HEIGHT}" STREQUAL "")
    set(TRACE_CROP_HEIGHT 0)
endif()
if(NOT DEFINED TRACE_FUZZ_PERCENT OR "${TRACE_FUZZ_PERCENT}" STREQUAL "")
    set(TRACE_FUZZ_PERCENT 20)
endif()

if(EXISTS "${TRACE_OUTPUT_DIR}")
    file(REMOVE_RECURSE "${TRACE_OUTPUT_DIR}")
endif()
file(MAKE_DIRECTORY "${TRACE_OUTPUT_DIR}/input")
file(MAKE_DIRECTORY "${TRACE_OUTPUT_DIR}/output")

execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xzf "${TRACE_ARCHIVE}"
        WORKING_DIRECTORY "${TRACE_OUTPUT_DIR}/input"
        RESULT_VARIABLE extract_result
        OUTPUT_VARIABLE extract_stdout
        ERROR_VARIABLE extract_stderr)
if(NOT extract_result EQUAL 0)
    message(STATUS "${extract_stdout}")
    message(STATUS "${extract_stderr}")
    message(FATAL_ERROR "failed to extract ${TRACE_ARCHIVE}")
endif()

set(trace_path "${TRACE_OUTPUT_DIR}/input/${TRACE_FILE}")
if(NOT EXISTS "${trace_path}")
    message(FATAL_ERROR "extracted trace was not found at ${trace_path}")
endif()

execute_process(
        COMMAND "${TRACE_REPLAY_EXE}"
        --trace "${trace_path}"
        --golden "${TRACE_GOLDEN}"
        --diff "${TRACE_OUTPUT_DIR}/output/${TRACE_CASE_NAME}-diff.png"
        --output "${TRACE_OUTPUT_DIR}/output"
        --backend "${TRACE_BACKEND}"
        --mobilegl-library "${MOBILEGL_LIBRARY}"
        --target-call "${TRACE_TARGET_CALL}"
        --width "${TRACE_WIDTH}"
        --height "${TRACE_HEIGHT}"
        --tolerance "${TRACE_TOLERANCE}"
        --crop-x "${TRACE_CROP_X}"
        --crop-y "${TRACE_CROP_Y}"
        --crop-width "${TRACE_CROP_WIDTH}"
        --crop-height "${TRACE_CROP_HEIGHT}"
        --fuzz-percent "${TRACE_FUZZ_PERCENT}"
        RESULT_VARIABLE replay_result
        OUTPUT_VARIABLE replay_stdout
        ERROR_VARIABLE replay_stderr)

message(STATUS "${replay_stdout}")
message(STATUS "${replay_stderr}")

set(retrace_log "${TRACE_OUTPUT_DIR}/output/retrace.log")
set(mobilegl_log "${TRACE_OUTPUT_DIR}/output/mobilegl.log")
if(EXISTS "${retrace_log}")
    file(STRINGS "${retrace_log}" gl_identity_lines REGEX "MOBILEGL_TRACE_GL_")
    foreach(line IN LISTS gl_identity_lines)
        message(STATUS "${line}")
    endforeach()
endif()

set(result_json "${TRACE_OUTPUT_DIR}/output/result.json")
if(DEFINED TRACE_ARTIFACT_DIR AND NOT "${TRACE_ARTIFACT_DIR}" STREQUAL "")
    file(MAKE_DIRECTORY "${TRACE_ARTIFACT_DIR}")
    set(actual_png "${TRACE_OUTPUT_DIR}/output/actual.png")
    if(EXISTS "${actual_png}")
        file(COPY_FILE "${actual_png}" "${TRACE_ARTIFACT_DIR}/${TRACE_CASE_NAME}-${TRACE_BACKEND}-actual.png")
    endif()
    if(EXISTS "${result_json}")
        file(COPY_FILE "${result_json}" "${TRACE_ARTIFACT_DIR}/${TRACE_CASE_NAME}-${TRACE_BACKEND}-result.json")
    endif()
    if(EXISTS "${retrace_log}")
        file(COPY_FILE "${retrace_log}" "${TRACE_ARTIFACT_DIR}/${TRACE_CASE_NAME}-${TRACE_BACKEND}-retrace.log")
    endif()
    if(EXISTS "${mobilegl_log}")
        file(COPY_FILE "${mobilegl_log}" "${TRACE_ARTIFACT_DIR}/${TRACE_CASE_NAME}-${TRACE_BACKEND}-mobilegl.log")
    endif()
endif()

if(EXISTS "${result_json}")
    file(READ "${result_json}" result_contents)
    message(STATUS "${result_contents}")
else()
    if(EXISTS "${retrace_log}")
        file(READ "${retrace_log}" retrace_log_contents)
        message(STATUS "${retrace_log_contents}")
    endif()
    if(EXISTS "${mobilegl_log}")
        file(READ "${mobilegl_log}" mobilegl_log_contents)
        message(STATUS "${mobilegl_log_contents}")
    endif()
    message(FATAL_ERROR "trace replay did not write ${result_json}")
endif()

if(NOT replay_result EQUAL 0)
    message(FATAL_ERROR "${TRACE_CASE_NAME} ${TRACE_BACKEND} trace replay failed with status ${replay_result}")
endif()
