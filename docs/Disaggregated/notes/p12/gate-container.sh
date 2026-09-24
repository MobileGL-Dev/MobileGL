#!/usr/bin/env bash
# gate.sh <tree> <tag>: the P7 v2 local gate (logs/p7-gate-template2.sh) for a P12 package tree, minus
# build-verify and the retrace leg (both run at integration). G1 compares against the p12 base
# (9f669e52: .text a52203, symbols in logs/p12/pull-syms-base.txt).
TREE=${1:?tree}; TAG=${2:?tag}
T=/tmp/p12gate-$TAG; mkdir -p "$T"
cd "$TREE" || exit 1
unset MOBILEGL_FLATC_EXECUTABLE
unset DISPLAY WAYLAND_DISPLAY
echo "=== START $(date) head=$(git rev-parse --short HEAD) dirty=$(git status --porcelain --untracked-files=no | wc -l) ==="
ninja -C build-split -j 5 2>&1 | tail -1; echo "=== split build rc=${PIPESTATUS[0]} ==="
echo "(pull build and G1 run separately)"; lrc=0
python3 scripts/ci/fatal_census.py 2>&1 | tail -1
python3 scripts/link_ratchet.py --build-dir build-split --baseline scripts/data/link_ratchet_baseline.txt --assert-monotone 2>&1 | tail -1
python3 scripts/ci/spawn_lane_parity.py build-split 2>&1 | grep -c -i error | sed 's/^/parity errors: /'
python3 scripts/ci/protocol_revision_pin.py 2>&1 | tail -1
python3 scripts/ci/wire_declines_audit.py . 2>&1 | tail -1

echo "=== HYGIENE ==="
hyg() { local name=$1; shift; if "$@" > "$T/hyg-$name.log" 2>&1; then echo "hygiene $name OK"; else echo "hygiene $name RED"; tail -5 "$T/hyg-$name.log" | sed 's/^/    /'; fi; }
hyg gen_pipe bash -c 'python3 scripts/gen_pipe.py && git diff --exit-code'
hyg gen_pipe_self python3 scripts/gen_pipe.py --self-test
hyg link_seam python3 scripts/ci/link_seam_purity.py --self-test
hyg memo_purity python3 scripts/ci/espryt_memo_purity.py --self-test
hyg symbol_report python3 scripts/symbol_report.py --self-test
hyg stdio bash -c "! grep -rnE 'fprintf[[:space:]]*\((stderr|stdout)|(^|[^[:alnum:]_>.])printf[[:space:]]*\(|(^|[^[:alnum:]_>.:])puts[[:space:]]*\(|std::(cout|cerr)' MobileGL/MG_Backend MobileGL/MG_State"
hyg dirty_surface bash -c 'python3 scripts/gen_pipe_dirty_surface.py --check && python3 scripts/gen_pipe_dirty_surface.py --self-test'
hyg field_ownership bash -c 'python3 scripts/gen_pipe_field_ownership.py --check && python3 scripts/gen_pipe_field_ownership.py --self-test'
hyg r16 bash -c 'bash scripts/ci/control_smoke_test.sh && python3 scripts/ci/test_trace_infrastructure.py && python3 scripts/ci/tcp_lane_tools_test.py && python3 tools/trace_replay/test_run_tcp_matrix.py && python3 tools/trace_replay/test_compare_actuals.py'
hyg g5_p3a bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD
hyg g5_p4a bash scripts/p4a_untouched_regions.sh 37da3c3a HEAD
hyg doc_citations python3 scripts/check_doc_citations.py --rev HEAD --strict docs/Disaggregated/*.md docs/Disaggregated/notes/p7/INTEGRATOR-DECISIONS-P7.md MobileGL/MG_Remote/CONTRACT-P7.md

cd build-split
for L in unit integration-split integration-spawn integration-tcp integration-magma-split integration-magma-spawn integration-magma-tcp integration-magma-full-split; do
  printf "lane %-28s " "$L"; ctest -L "^$L\$" -j 4 2>&1 | grep -E 'tests passed|tests failed|No tests' | head -1 | tee "$T/lane-$L.txt"
  if grep -qE "[1-9][0-9]* tests failed" "$T/lane-$L.txt" 2>/dev/null; then sed 's/^/    FAILED: /' Testing/Temporary/LastTestsFailed.log | head -8; fi
done
for M in monolith inproc; do
  printf "lane %-28s " "integration-gpu[$M]"
  MOBILEGL_TRANSPORT=$M MOBILEGL_IPC_ROLE_SPLIT_STATE=1 MOBILEGL_IPC_STRICT_ERRORS=1 MOBILEGL_IPC_RUN_AHEAD=1 \
  MOBILEGL_ITEST_REQUIRE_GPU=1 MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH=1 \
  MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS=1 MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER=1 \
    ctest -L integration-gpu -j 4 2>&1 | grep -E 'tests passed|tests failed|No tests' | head -1 | tee "$T/lane-gpu-$M.txt"
  if grep -qE "[1-9][0-9]* tests failed" "$T/lane-gpu-$M.txt" 2>/dev/null; then sed 's/^/    FAILED: /' Testing/Temporary/LastTestsFailed.log | grep -v Skipped | head -8; fi
done
echo "=== DONE $(date) ==="
