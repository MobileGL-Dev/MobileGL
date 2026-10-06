# P13 W3b (ID-P13-3): one named-refusal control. Runs the integration binary with a subsystem mask
# that clears one of bits 0-13 and passes only if the run went red AND its log carries
# ConfigLoader's Fatal{PipeSubsystemsFixedOn} naming that mask. Invoked by
# mgl_itest_add_refused_mask_control (CMakeLists.txt) with -DEXE -DFILTER -DMASK -DLOG; the mask
# and the rest of the lane's environment reach the child through ctest's ENVIRONMENT property.
foreach(var EXE FILTER MASK LOG)
    if (NOT DEFINED ${var})
        message(FATAL_ERROR "RefusedMaskControl: -D${var}= is required")
    endif()
endforeach()
if (NOT "$ENV{MOBILEGL_PIPE_PUSH}" STREQUAL "${MASK}")
    message(FATAL_ERROR "RefusedMaskControl: the environment carries MOBILEGL_PIPE_PUSH="
                        "'$ENV{MOBILEGL_PIPE_PUSH}', not the control's ${MASK}; it would prove nothing")
endif()
# A split build writes the client role's log as <stem>.client.log (MG_Util/Debug/Log.cpp's
# RoleLogPath), so both spellings are matched by "<stem>." - and ONLY by that: a bare "<stem>*"
# also matched a sibling control whose name extends this one's (ObjectSubsystemControlRefused vs
# ...RefusedTexture), and under ctest -j this control deleted the other's log while it ran.
string(REGEX REPLACE "[.]log$" "" stem "${LOG}")
file(GLOB stale "${stem}.*")
if (stale)
    file(REMOVE ${stale})
endif()
execute_process(COMMAND "${EXE}" "--gtest_filter=${FILTER}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if (rc EQUAL 0)
    message(FATAL_ERROR "RefusedMaskControl: MOBILEGL_PIPE_PUSH=${MASK} ran GREEN (${FILTER}); a mask "
                        "that clears a fixed-on subsystem bit must be refused at startup.\n${out}")
endif()
file(GLOB logs "${stem}.*")
set(found FALSE)
foreach(log IN LISTS logs)
    file(READ "${log}" text)
    string(FIND "${text}" "Fatal{PipeSubsystemsFixedOn, \"MOBILEGL_PIPE_PUSH=${MASK} clears" at)
    if (NOT at EQUAL -1)
        set(found TRUE)
    endif()
endforeach()
if (NOT found)
    message(FATAL_ERROR "RefusedMaskControl: MOBILEGL_PIPE_PUSH=${MASK} went red (rc=${rc}) but no log "
                        "under ${stem}.* names Fatal{PipeSubsystemsFixedOn} for that mask - a red for "
                        "another reason is not this control's red.\n${out}\n${err}")
endif()
message(STATUS "MOBILEGL_PIPE_PUSH=${MASK} was refused by name at startup (rc=${rc})")
