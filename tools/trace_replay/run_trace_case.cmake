foreach(required TRACE_REPLAY_EXE MOBILEGL_LIBRARY TRACE_ARCHIVE TRACE_FILE TRACE_GOLDEN TRACE_OUTPUT_DIR TRACE_BACKEND TRACE_CASE_NAME TRACE_TARGET_CALL TRACE_WIDTH TRACE_HEIGHT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

if(NOT DEFINED TRACE_SSIM_THRESHOLD OR "${TRACE_SSIM_THRESHOLD}" STREQUAL "")
    set(TRACE_SSIM_THRESHOLD 0.99)
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
set(alternate_golden_args)
if(DEFINED TRACE_ALTERNATE_GOLDEN AND NOT "${TRACE_ALTERNATE_GOLDEN}" STREQUAL "")
    list(APPEND alternate_golden_args --alternate-golden "${TRACE_ALTERNATE_GOLDEN}")
endif()
set(coherent_as_flush_args)
if(TRACE_COHERENT_AS_FLUSH)
    list(APPEND coherent_as_flush_args --coherent-as-flush)
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
        ${alternate_golden_args}
        --diff "${TRACE_OUTPUT_DIR}/output/${TRACE_CASE_NAME}-diff.png"
        --output "${TRACE_OUTPUT_DIR}/output"
        --backend "${TRACE_BACKEND}"
        --mobilegl-library "${MOBILEGL_LIBRARY}"
        --target-call "${TRACE_TARGET_CALL}"
        --width "${TRACE_WIDTH}"
        --height "${TRACE_HEIGHT}"
        --ssim-threshold "${TRACE_SSIM_THRESHOLD}"
        --crop-x "${TRACE_CROP_X}"
        --crop-y "${TRACE_CROP_Y}"
        --crop-width "${TRACE_CROP_WIDTH}"
        --crop-height "${TRACE_CROP_HEIGHT}"
        ${coherent_as_flush_args}
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
    if(EXISTS "${TRACE_GOLDEN}")
        file(COPY_FILE "${TRACE_GOLDEN}" "${TRACE_ARTIFACT_DIR}/${TRACE_CASE_NAME}-${TRACE_BACKEND}-golden.png")
    endif()
    if(DEFINED TRACE_ALTERNATE_GOLDEN AND NOT "${TRACE_ALTERNATE_GOLDEN}" STREQUAL "" AND EXISTS "${TRACE_ALTERNATE_GOLDEN}")
        file(COPY_FILE "${TRACE_ALTERNATE_GOLDEN}" "${TRACE_ARTIFACT_DIR}/${TRACE_CASE_NAME}-${TRACE_BACKEND}-alternate-golden.png")
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

# --- MOBILEGL_PIPE_VERIFY: the third CI mode's own assertions (gates G3 and G8) --------------
#
# A retrace that exported MOBILEGL_PIPE_VERIFY=1 at a library which was never configured with
# -DMOBILEGL_PIPE_VERIFY=ON is a no-op that looks exactly like a clean pass: the variable steers
# nothing, the frames still match their goldens, and the case reports green having verified
# nothing at all. The mode therefore has to prove it ran, and the only channel a `cmake -P` script
# has for that is the library's own log.
#
# Three demands, all of them silent when MOBILEGL_PIPE_VERIFY is unset or "0", so an ordinary
# retrace is untouched:
#   * mobilegl.log exists - the replay wrote one, so the library was loaded and logging;
#   * it carries "MGPipe: verify armed" - the comparator armed in THIS process;
#   * it carries neither Fatal{PipeVerifyDiffer (a push/pull divergence, the thing the mode
#     exists to find) nor Fatal{UnmigratedPipeInput (a backend read of a field the verb's fill
#     table does not list - fixed by adding the row to MG_Pipe/FillPoints.def, never by marking
#     the field sticky).
# The Fatal check is not redundant with the replay's exit status: MOBILEGL_PIPE_VERIFY_FATAL=0 is
# the supported triage configuration, and there the divergence is logged and counted rather than
# aborted, so the run would otherwise finish 0 with its own report in the log.
if(DEFINED ENV{MOBILEGL_PIPE_VERIFY} AND NOT "$ENV{MOBILEGL_PIPE_VERIFY}" STREQUAL "")
    set(pipe_verify_case "${TRACE_CASE_NAME} ${TRACE_BACKEND}")
    if("$ENV{MOBILEGL_PIPE_VERIFY}" STREQUAL "0" OR "$ENV{MOBILEGL_PIPE_VERIFY}" STREQUAL "false")
        message(STATUS "MGPipe verify: MOBILEGL_PIPE_VERIFY=$ENV{MOBILEGL_PIPE_VERIFY}, no verify assertions for ${pipe_verify_case}")
    elseif(NOT EXISTS "${mobilegl_log}")
        message(FATAL_ERROR
                "MOBILEGL_PIPE_VERIFY is set for ${pipe_verify_case} but the run wrote no ${mobilegl_log}, "
                "so there is no evidence the comparator ever armed. A verify retrace with no library log "
                "cannot be counted as a verify retrace.")
    else()
        file(READ "${mobilegl_log}" pipe_verify_log)
        string(FIND "${pipe_verify_log}" "MGPipe: verify armed" pipe_verify_armed_at)
        if(pipe_verify_armed_at EQUAL -1)
            message(FATAL_ERROR
                    "MOBILEGL_PIPE_VERIFY is set for ${pipe_verify_case} and the library never reported "
                    "\"MGPipe: verify armed\". Either this libMobileGL.so was not built with "
                    "-DMOBILEGL_PIPE_VERIFY=ON - in which case the whole verify retrace lane is comparing "
                    "nothing - or the runtime knob never reached the process. Check that the VERIFY "
                    "runtime artifact is the one unpacked at ${MOBILEGL_LIBRARY}.")
        endif()
        file(STRINGS "${mobilegl_log}" pipe_verify_fatals
                REGEX "Fatal\\{(PipeVerifyDiffer|UnmigratedPipeInput)")
        if(pipe_verify_fatals)
            foreach(line IN LISTS pipe_verify_fatals)
                message(STATUS "${line}")
            endforeach()
            list(LENGTH pipe_verify_fatals pipe_verify_fatal_count)
            message(FATAL_ERROR
                    "${pipe_verify_case}: ${pipe_verify_fatal_count} MGPipe Fatal(s) under "
                    "MOBILEGL_PIPE_VERIFY. Fatal{PipeVerifyDiffer, \"<Field>@<Verb>\"} is a real push/pull "
                    "divergence and is recorded, not silenced; Fatal{UnmigratedPipeInput, \"<Field>@<Verb>\"} "
                    "is a missing row in MG_Pipe/FillPoints.def's class table - add it, regenerate, rerun "
                    "(never mark the field sticky).")
        endif()
        message(STATUS "MGPipe verify: ${pipe_verify_case} armed, zero divergences, zero unmigrated reads")
    endif()
endif()
