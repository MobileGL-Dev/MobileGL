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

# --- P5: the transport, threaded as an ENVIRONMENT rather than as a replay CLI flag ----------
#
# MOBILEGL_TRANSPORT reaches the library through the environment, so a `cmake -P` script can set
# it and the child inherits it - exactly how the MOBILEGL_PIPE_VERIFY block at the bottom of this
# file already works, and with no C++ change anywhere. The desktop CLI has no --env option
# (trace_replay_cli.cpp:116-179 is the whole option list) and the Android path already has one
# (trace-replay-ci.sh --env -> trace_replay_core.cpp's setenv block), so an environment read
# covers both directions and a new flag would buy nothing.
#
# TWO WAYS IN, ONE ASSERTION. `-DTRACE_TRANSPORT=` is what the SPLIT ctest variant passes, so
# that entry is self-describing and needs no ritual around it; exporting MOBILEGL_TRANSPORT in
# the calling process is what ~/w7/retrace_gate.py and CI's retrace-split job do over the
# UNCHANGED case names, because the gate parses a FOREIGN reference CTestTestfile.cmake whose
# name regex cannot see a variant suffix. Setting the variable from the -D FIRST means the
# assertions below read one value however it arrived.
if(DEFINED TRACE_TRANSPORT AND NOT "${TRACE_TRANSPORT}" STREQUAL "")
    set(ENV{MOBILEGL_TRANSPORT} "${TRACE_TRANSPORT}")
endif()
if(DEFINED TRACE_IPC_SERVER_PATH AND NOT "${TRACE_IPC_SERVER_PATH}" STREQUAL "")
    # ARCHITECTURE.md:543. P6 consumes it; P5 carries it so that "unparsed" and
    # "parsed and ignored" stop being the same observation.
    set(ENV{MOBILEGL_IPC_SERVER_PATH} "${TRACE_IPC_SERVER_PATH}")
endif()
if(DEFINED TRACE_IPC_CONTROL AND NOT "${TRACE_IPC_CONTROL}" STREQUAL "")
    set(ENV{MOBILEGL_IPC_CONTROL} "${TRACE_IPC_CONTROL}")
    set(ENV{MOBILEGL_IPC_DATA} stream)
    set(ENV{MOBILEGL_IPC_TOKEN} "${TRACE_IPC_TOKEN}")
    set(ENV{MOBILEGL_IPC_REQUIRE_SAME_BUILD} 1)
    set(ENV{MOBILEGL_IPC_LOG_FORWARD} 1)
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
# BOTH ROLES ARE SUFFIXED, and the client's rename is deliberate. Leaving it at the old
# `mobilegl.log` would have been the compatible choice and the wrong one: a reader that was not
# updated would go on finding a file, go on passing, and go on seeing only half the session.
# With both names moved, an un-updated reader gets "file not found" and says so.
set(mobilegl_log "${TRACE_OUTPUT_DIR}/output/mobilegl.client.log")
# P6: THE SERVER ROLE HAS ITS OWN LOG, and the census below must read BOTH or a server-side
# Fatal{ is invisible to it. Not hypothetical: under spawn the server is where every applier
# refusal and Fatal{UnmigratedSurface} is raised, which are exactly the lines this lane counts.
# The client file keeps the unsuffixed name, so every other reader here - the transport markers,
# the pipe-verify block, the artifact copy - is unchanged.
set(mobilegl_server_log "${TRACE_OUTPUT_DIR}/output/mobilegl.server.log")
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
    if(EXISTS "${mobilegl_server_log}")
        file(COPY_FILE "${mobilegl_server_log}"
             "${TRACE_ARTIFACT_DIR}/${TRACE_CASE_NAME}-${TRACE_BACKEND}-mobilegl.server.log")
    endif()
    if(EXISTS "${mobilegl_log}")
        file(COPY_FILE "${mobilegl_log}" "${TRACE_ARTIFACT_DIR}/${TRACE_CASE_NAME}-${TRACE_BACKEND}-mobilegl.client.log")
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
#   * it carries none of the three MGPipe Fatals: Fatal{PipeVerifyDiffer (a push/pull divergence,
#     the thing the mode exists to find), Fatal{UnmigratedPipeInput (a backend read of a field the
#     verb's fill table does not list - fixed by adding the row to MG_Pipe/FillPoints.def, never by
#     marking the field sticky), and Fatal{PipeVerifyBadKnob (a misspelt MOBILEGL_PIPE_VERIFY_CORRUPT
#     or MOBILEGL_PIPE_POISON_OMIT). The third is in the regex on purpose even though D2 makes it
#     abort the process: with MOBILEGL_PIPE_VERIFY_FATAL=0 the abort is exactly what does not
#     happen, and a typo'd knob would otherwise leave the negative-control lane looking healthy.
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
                REGEX "Fatal\\{(PipeVerifyDiffer|UnmigratedPipeInput|PipeVerifyBadKnob)")
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
                    "(never mark the field sticky); Fatal{PipeVerifyBadKnob, ...} is a misspelt "
                    "MOBILEGL_PIPE_VERIFY_CORRUPT / MOBILEGL_PIPE_POISON_OMIT - fix the spelling, the "
                    "vocabularies are kMGPipeInputFieldNames[] and kMGPipeVerbNames[].")
        endif()
        message(STATUS "MGPipe verify: ${pipe_verify_case} armed, zero divergences, zero unmigrated reads")
    endif()
endif()

# --- P5: MOBILEGL_TRANSPORT, the split arm's own assertions ----------------------------------
#
# The same problem the verify block above solves, with the same answer and one extra reason to
# need it. A retrace that exported MOBILEGL_TRANSPORT=inproc at a library configured WITHOUT
# -DMOBILEGL_BUILD_DISAGGREGATED=ON is not merely a no-op: the variable's PARSER does not exist
# in that build at all (CONTRACT-P5 5 - putting a complaint in the unconditional part of
# ConfigLoader would move a pull-build symbol and break gate G1), so the value is accepted by the
# environment and silently ignored, every frame still matches its golden, and the case reports a
# clean pass having run monolith end to end.
#
# THE LIBRARY'S OWN LOG IS THE ONLY CHANNEL a `cmake -P` script has for the difference, and it is
# also THE ONLY VALID REFUSAL CENSUS. A `ctest -V` transcript is a FALSE ZERO for Fatal{...}
# lines: the console sink is compiled out of the configurations these lanes run, so the aborts
# reach output/mobilegl.log and nowhere else. That is why TRACE_OUTPUT_DIR and TRACE_ARTIFACT_DIR
# carry the variant - without it both arms write one output/mobilegl.log, the second run wipes
# the first, and the census silently becomes a census of one arm. P4a shipped that defect once
# already; this is the same defect in a new place, pre-empted.
#
# Three demands, all silent when MOBILEGL_TRANSPORT is unset or "monolith":
#   * mobilegl.log exists - the replay wrote one, so the library was loaded and logging;
#   * it carries ConfigLoader's inproc line, which is emitted ONLY by a build that compiled the
#     parser AND resolved the value to InProcess. This is the falsifiable half;
#   * it carries no Fatal{ at all. Under split the emit table raises
#     Fatal{UnmigratedVerb, "<slot>"} for the 64 slots P5 does not implement, so a clean run of a
#     reduced-path target is a run that touched none of them - and any other Fatal{ (ProtocolCorruption,
#     UnmigratedPipeInput, AbiMismatch) is a real defect. The count and the distinct names are
#     printed either way, because the census is the deliverable even when the run passes.
if(DEFINED ENV{MOBILEGL_TRANSPORT} AND NOT "$ENV{MOBILEGL_TRANSPORT}" STREQUAL "")
    set(split_case "${TRACE_CASE_NAME} ${TRACE_BACKEND}")
    if("$ENV{MOBILEGL_TRANSPORT}" STREQUAL "monolith")
        message(STATUS "MGPipe split: MOBILEGL_TRANSPORT=monolith, no split assertions for ${split_case}")
    elseif(NOT EXISTS "${mobilegl_log}")
        message(FATAL_ERROR
                "MOBILEGL_TRANSPORT=$ENV{MOBILEGL_TRANSPORT} is set for ${split_case} but the run wrote "
                "no ${mobilegl_log}, so there is no evidence the transport ever resolved. A split "
                "retrace with no library log cannot be counted as a split retrace.")
    else()
        file(READ "${mobilegl_log}" split_log)
        # THE DISTINCTIVE PART OF ConfigLoader's INFO LINE, not the bare KEY=VALUE - review finding
        # M-5. ConfigLoader.cpp:71 logs `Config: Accepted env variable: %s=%s` for EVERY MOBILEGL_*
        # variable, unconditionally, in every build including the pull one. That line is MGLOG_D,
        # so at the INFO level CI and the gate use it is compiled out - but at
        # MOBILEGL_LOG_ACTIVE_LEVEL=..._DEBUG it reads `Config: Accepted env variable:
        # MOBILEGL_TRANSPORT=inproc` and satisfied the old substring search. Observed GREEN on a
        # crafted pull-build log. The person most likely to hit that is the one who rebuilds at
        # DEBUG to debug a split failure. The sentence below exists only in
        # ConfigLoader::InitTransport's InProcess arm, which exists only under
        # MOBILEGL_BUILD_DISAGGREGATED.
        #
        # ONE ROW PER TRANSPORT THIS RUNNER ADMITS. A transport with no row is a
        # FATAL below rather than a fall-through, because the fall-through is the
        # exact accident this whole block exists to prevent: an arm that named a
        # transport nobody asserted on and went green having proved nothing.
        #
        # THE SPAWN ROW CARRIES A SECOND SENTENCE, and it is the stronger half.
        # The ConfigLoader marker proves the VALUE RESOLVED - it is a statement
        # about a parser. `spawn ARMED - the server role runs in pid N` is
        # written by ClientSession::StartSpawned only after the connect and the
        # handshake succeeded, and it carries a pid that is not this process's.
        # inproc resolves its own marker while running the server role on a
        # thread HERE; nothing on that path can write this one.
        set(split_expected_second "")
        if("$ENV{MOBILEGL_TRANSPORT}" STREQUAL "inproc")
            set(split_expected_marker "MOBILEGL_TRANSPORT=inproc - the MGPipe record stream")
        elseif("$ENV{MOBILEGL_TRANSPORT}" STREQUAL "spawn")
            set(split_expected_marker "MOBILEGL_TRANSPORT=spawn - the MGPipe record stream")
            if("$ENV{MOBILEGL_IPC_CONTROL}" MATCHES "^tcp://")
                set(split_expected_second "control=tcp data=stream server=")
                if(NOT split_log MATCHES "control=tcp data=stream server=[^ \r\n]+ pid=[1-9][0-9]*")
                    message(FATAL_ERROR "${split_case}: TCP session has no endpoint and server pid arm proof")
                endif()
            else()
                set(split_expected_second "spawn ARMED - the server role runs in pid ")
            endif()
        else()
            message(FATAL_ERROR
                    "MOBILEGL_TRANSPORT=$ENV{MOBILEGL_TRANSPORT} is set for ${split_case} and this "
                    "runner has no marker for it, so the run could not be checked. ConfigLoader "
                    "recognises monolith|inproc|spawn|unix:<path>|pipe:<name> and REFUSES the last "
                    "two BY NAME, staying on monolith - a run that asked for one of those would "
                    "have replayed monolith. Add a row above when the transport lands; do not let "
                    "an unasserted transport reach a green.")
        endif()
        string(FIND "${split_log}" "${split_expected_marker}" split_armed_at)
        if(split_armed_at EQUAL -1)
            # Say which transport was actually asked for. ConfigLoader REFUSES spawn / unix: /
            # pipe: BY NAME and stays on monolith (they are P6's), so a run that set one of those
            # has a different diagnosis from one that set inproc against a monolith library, and
            # the old message named `inproc` either way.
            set(split_refusal "")
            if("$ENV{MOBILEGL_TRANSPORT}" STREQUAL "spawn")
                set(split_refusal
                        " NOTE: this run asked for 'spawn'. The server is a SEPARATE PROCESS here, so "
                        "the two ways to get this far with no marker are a library that has no spawn "
                        "arm (pre-P6) and a launcher that never resolved the transport at all. "
                        "MOBILEGL_IPC_SERVER_PATH must point at a libMobileGLServer.so built from this "
                        "same tree; LaunchServer REFUSES an unresolvable image BY NAME and never falls "
                        "back to running the server role locally.")
            endif()
            message(FATAL_ERROR
                    "MOBILEGL_TRANSPORT=$ENV{MOBILEGL_TRANSPORT} is set for ${split_case} and the library "
                    "never reported resolving it: ${mobilegl_log} carries no "
                    "\"${split_expected_marker}\". ConfigLoader::InitTransport logs that line at INFO "
                    "when it selects InProcess, and it exists only in a build configured with "
                    "-DMOBILEGL_BUILD_DISAGGREGATED=ON - in a build without it the whole parser is "
                    "compiled out and the variable is accepted and ignored, which is exactly the 'the "
                    "split lane ran monolith and went green' failure. Check that the SPLIT runtime "
                    "artifact is the one at ${MOBILEGL_LIBRARY}, and that MOBILEGL_LOG_ACTIVE_LEVEL "
                    "admits INFO (at WARN or above the line is compiled out and this reds for no "
                    "defect).${split_refusal}")
        endif()
        # ... and the half that a same-process run cannot satisfy. Checked
        # SEPARATELY from the marker above so the two failures read differently:
        # no marker means the transport never resolved, while marker-but-no-pid
        # means it resolved and the session never came up across the boundary.
        if(NOT "${split_expected_second}" STREQUAL "")
            string(FIND "${split_log}" "${split_expected_second}" split_second_at)
            if(split_second_at EQUAL -1)
                message(FATAL_ERROR
                        "${split_case}: ${mobilegl_log} reports MOBILEGL_TRANSPORT="
                        "$ENV{MOBILEGL_TRANSPORT} resolved, but carries no "
                        "\"${split_expected_second}\". That sentence is written by "
                        "ClientSession::StartSpawned AFTER the launch, the connect and the handshake "
                        "all succeeded, and the pid in it is the SERVER's - it is the only evidence "
                        "in this log that the server role left this process. Without it the run "
                        "resolved a transport and then replayed something else.")
            endif()
        endif()
        # THE SERVER ROLE'S LOG MUST EXIST, FAIL-CLOSED LIKE THE CLIENT'S ABOVE, and for EVERY
        # transport this runner admits. The lines the two censuses below count out of it - every
        # applier Fatal{, and the MGWIRE-FLOOR line, which only the server's
        # VulkanRenderer::OnSubmitsCompletedUpTo emits - exist nowhere else, and "no file" read
        # as "0 lines" is a green census of the half of the session that was never seen.
        #   * spawn: the server PROCESS writes it (over tcp, the fixture's forwarded lines land
        #     in the same file). A server that died before its first line leaves none.
        #   * inproc: the server role is the mgl-srv-apply THREAD of this process, and Log.h
        #     routes every line by thread role to the same `.server.log` sink, opened lazily on
        #     the first server-role line. At the INFO level this lane demands (see the marker
        #     message above), ServerLoop writes one line when that thread starts, so a clean
        #     inproc run always has the file too, and an inproc run without one never started
        #     its server role.
        # CHECKED AFTER THE MARKER AND THE SECOND SENTENCE, NOT BEFORE. A launch or handshake
        # failure also leaves no server log; checked first, this would report "the server left
        # no file" for a run whose more specific diagnosis is "resolved, but carries no spawn
        # ARMED", and send the reader to the wrong process.
        if(NOT EXISTS "${mobilegl_server_log}")
            message(FATAL_ERROR
                    "${split_case}: MOBILEGL_TRANSPORT=$ENV{MOBILEGL_TRANSPORT} but the run wrote no "
                    "${mobilegl_server_log}, so the server role's Fatal{ and MGWIRE-FLOOR lines "
                    "cannot be counted. A split retrace with no server log cannot be counted as a "
                    "clean one.")
        endif()
        # The refusal census. Recorded on every split run, pass or fail.
        #
        # BOTH FILES, and the server's is the one that matters most: under spawn every
        # applier refusal is raised over there, so a census reading only the client's would
        # report a confident zero for the half of the session it cannot see.
        file(STRINGS "${mobilegl_log}" split_fatals REGEX "Fatal\\{")
        file(STRINGS "${mobilegl_server_log}" split_server_fatals REGEX "Fatal\\{")
        list(APPEND split_fatals ${split_server_fatals})
        list(LENGTH split_fatals split_fatal_count)
        message(STATUS "MGPipe split: ${split_case} transport=$ENV{MOBILEGL_TRANSPORT}, "
                       "Fatal{ lines across ${mobilegl_log} and ${mobilegl_server_log}: "
                       "${split_fatal_count}")
        if(split_fatals)
            foreach(line IN LISTS split_fatals)
                message(STATUS "${line}")
            endforeach()
            message(FATAL_ERROR
                    "${split_case}: ${split_fatal_count} MGPipe Fatal(s) under MOBILEGL_TRANSPORT="
                    "$ENV{MOBILEGL_TRANSPORT}. Fatal{UnmigratedVerb, \"<slot>\"} is one of the 64 emit-table "
                    "slots P5 leaves unimplemented (R-4) - if the reduced path reached it, either the verb "
                    "census is wrong or this case is not on the reduced path; Fatal{ProtocolCorruption, ...} "
                    "is a record that crossed the wire without declaring its bytes (CONTRACT-P5 rule A); "
                    "Fatal{UnmigratedPipeInput, ...} is a missing row in the field-ownership table. None of "
                    "them is silenced here: this log is the only place they appear, because the console sink "
                    "is compiled out of the configurations this lane runs.")
        endif()
        # P7 B3 (review ID-P7-34): THE COMPLETED-FRAME-SERIAL FLOOR'S LANE, read exactly like the
        # Fatal census above - both roles' logs, every split run, counted even when zero.
        #
        # VulkanRenderer::OnSubmitsCompletedUpTo logs one "MGWIRE-FLOOR unsound-serial-complete"
        # line (MGLOG_W, unconditional, disaggregated builds only) each time it would raise the
        # floor to a frame serial that a submission still in flight carries - the defect behind
        # the OpenRA device divergence, where a mid-frame pooled-fence submission retiring
        # declared its whole serial complete while the Present submission of the same serial was
        # still copying. The clamp there makes the line unreachable, and without this red the
        # clamp had no lane: deleting it left every lane green and the picture on lavapipe gold,
        # because the host's barriers happen to hide the tear. The count was 26 per OpenRA replay
        # before the fix and 0 after; any nonzero count is the floor asserting a completion it
        # never waited for, whether or not this driver's timing turned it into pixels.
        file(STRINGS "${mobilegl_log}" split_floor REGEX "MGWIRE-FLOOR unsound-serial-complete")
        file(STRINGS "${mobilegl_server_log}" split_server_floor REGEX "MGWIRE-FLOOR unsound-serial-complete")
        list(APPEND split_floor ${split_server_floor})
        list(LENGTH split_floor split_floor_count)
        message(STATUS "MGPipe split: ${split_case} transport=$ENV{MOBILEGL_TRANSPORT}, "
                       "MGWIRE-FLOOR unsound-serial-complete lines: ${split_floor_count}")
        if(split_floor)
            list(GET split_floor 0 split_floor_first)
            message(STATUS "${split_floor_first}")
            message(FATAL_ERROR
                    "${split_case}: ${split_floor_count} MGWIRE-FLOOR unsound-serial-complete line(s) under "
                    "MOBILEGL_TRANSPORT=$ENV{MOBILEGL_TRANSPORT}. The completed-frame-serial floor was "
                    "raised to a serial an in-flight submission still carries, so a streamed buffer write "
                    "can take the unsynchronised host path over bytes that submission is still copying "
                    "(magma-b3.md §2). The clamp in VulkanRenderer::OnSubmitsCompletedUpTo is what makes "
                    "this unreachable; a line here means it was removed or bypassed.")
        endif()
    endif()
endif()
