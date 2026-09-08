#!/usr/bin/env bash
# pin_device.sh <serial> pin|unpin|check
#
# Frequency pinning for the two disaggregation-campaign devices, standalone: pure adb + su, no
# sourcing of a device profile, no dependency on bench.sh. It exists because bench.sh's
# pin_freqs()/unpin_freqs() are MediaTek-legacy-only (/proc/ppm + /proc/gpufreq) and NEITHER of
# these two devices has those paths - on both of them bench.sh's pin writes into nothing and
# exits 0, which is precisely the silent "the run looks pinned but is not" failure the
# PROFILE_VERIFIED guard was built to catch. So: pin here, run bench.sh with --no-pin, and use
# `check` in place of the pin-integrity fields bench.sh would otherwise have sampled.
#
#   ./pin_device.sh 35d0befa pin
#   ./bench.sh --device devices/xiaomi-adreno830.env --backend magma --no-pin
#   ./pin_device.sh 35d0befa check   || echo "PIN DRIFTED - discard this run"
#   ./pin_device.sh 35d0befa unpin
#
# All three actions print the live big/little/GPU frequencies, the governors and the gate
# temperature, so the output of `pin` and of `unpin` is itself the before/after evidence.
#
# check exit codes (deliberately three-valued: "no pin at all" and "a pin that slipped" are
# different facts, and collapsing them to one non-zero would hide which one happened):
#   0  PINNED  - every pinned node is at its pin AND every live frequency equals it
#   1  DRIFT   - the device is partly pinned, or a live frequency has left its pin. The
#                dangerous state: a run overlapping this is not comparable. Discard it.
#   2  UNPINNED- no node is at a pin this script set; the vendor governors have their range
#                back. That is the correct state to leave a device in, and it is still non-zero
#                so that `pin_device.sh X check && measure` cannot silently measure unpinned.
#                Note this is asserted against OUR pins, not against the stock range: ColorOS
#                moves policy4's max on its own within seconds of a release, so an exact-stock
#                comparison reported DRIFT on a correctly unpinned device.
#
# Values were read off each device on 2026-09-07 and confirmed against a pinned window; see
# ../REPORT.md and the matching *.env profiles. Stock values are hardcoded rather than sampled
# at pin time on purpose - a restore that reads "stock" from an already-pinned device would
# make the pin permanent, which is how a device silently stays clamped across a reboot-less
# week of runs.

set -u -o pipefail
# Git Bash: stop MSYS rewriting /sys/... and /proc/... arguments into C:/Program Files/...
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

SERIAL=${1:-}
ACTION=${2:-}
case "$ACTION" in
  pin|unpin|check) ;;
  *) echo "usage: $0 <serial> pin|unpin|check" >&2; exit 64 ;;
esac

CPUFREQ=/sys/devices/system/cpu/cpufreq

# ---------------------------------------------------------------------------------------------
# Device table. Everything device-specific lives here; the actions below are generic.
#   POLICIES        - "label:policy:pinned_khz:stock_min:stock_max" per cpufreq policy to pin.
#                     Every policy that can run a hot thread must be listed, not just the two
#                     the protocol reports: on MT6993 an unpinned policy7 at 4.21 GHz defeats
#                     the entire pin the moment the scheduler lands a thread on it.
#   THERMAL_TYPE    - matched against /sys/class/thermal/thermal_zone*/type by name, never by
#                     zone number: numbering is not stable across boots.
# ---------------------------------------------------------------------------------------------
case "$SERIAL" in
  35d0befa)          # Xiaomi 24129PN74C, Snapdragon 8 Elite (SM8750), Adreno 830v2
    DEV_NAME="Xiaomi 24129PN74C / SM8750 / Adreno 830v2"
    GPU_STYLE=kgsl
    POLICIES="big:policy6:1958400:1017600:2841600 little:policy0:1555200:556800:2745600"
    THERMAL_TYPE=cpuss-0-0
    KGSL=/sys/class/kgsl/kgsl-3d0
    GPU_PIN_LEVEL=0            # pwrlevel 0 = 1100 MHz, above the stock devfreq ceiling of 1050
    GPU_STOCK_MIN_LEVEL=12
    GPU_STOCK_MAX_LEVEL=0
    GPU_PINNED_FREQ=1100000000 # gpuclk is in Hz on kgsl, NOT kHz
    GPU_FREQ_UNIT=Hz
    ;;
  3B159D009VZ00000)  # Oppo PLG110 / ColorOS, MediaTek MT6993 (Dimensity 9500), Mali
    DEV_NAME="Oppo PLG110 / MT6993 / Mali (gpufreqv2)"
    GPU_STYLE=gpufreqv2
    # 2000000 / 1600000 are the nearest available OPPs to the protocol's 1958400 / 1555200
    # (+2.1% / +2.9%); neither target is an exact step on this part.
    POLICIES="big:policy4:2000000:300000:3500000 little:policy0:1600000:300000:2100000 extra:policy7:2000000:300000:3200000"
    THERMAL_TYPE=soc_max
    GPU_FIX_NODE=/proc/gpufreqv2/fix_target_opp_index
    GPU_PIN_LEVEL=0            # OPP index 0 = 1716000 kHz, the top working OPP
    GPU_PINNED_FREQ=1716000
    GPU_FREQ_UNIT=kHz
    ;;
  *)
    echo "$0: unknown serial '$SERIAL'." >&2
    echo "Known: 35d0befa (Xiaomi/Adreno830), 3B159D009VZ00000 (Oppo/Mali)." >&2
    echo "Refusing to guess: the pin path differs per SoC and a wrong one fails silently." >&2
    exit 64 ;;
esac

A="adb -s $SERIAL"
# Quote the whole su invocation for the DEVICE shell, or the redirect runs unprivileged.
su_() { $A shell "su -c '$*'" 2>&1 | tr -d '\r'; }

$A get-state >/dev/null 2>&1 || { echo "$0: device $SERIAL is not connected" >&2; exit 65; }
[ "$(su_ 'id -u')" = "0" ] || { echo "$0: no root on $SERIAL (su failed)" >&2; exit 65; }

# --- read live state in ONE device round trip -------------------------------------------------
# Every read goes through su: on the Oppo the cpufreq nodes are 0660 system:system and a plain
# `adb shell cat scaling_governor` answers "Permission denied", which a careless parser reads
# as an empty governor rather than as a failure.
read_state() {
  local script="" spec label pol
  for spec in $POLICIES; do
    IFS=: read -r label pol _ _ _ <<<"$spec"
    script="$script echo \"${label}_gov=\$(cat $CPUFREQ/$pol/scaling_governor)\";"
    script="$script echo \"${label}_min=\$(cat $CPUFREQ/$pol/scaling_min_freq)\";"
    script="$script echo \"${label}_max=\$(cat $CPUFREQ/$pol/scaling_max_freq)\";"
    script="$script echo \"${label}_cur=\$(cat $CPUFREQ/$pol/scaling_cur_freq)\";"
  done
  script="$script for tz in /sys/class/thermal/thermal_zone*; do"
  script="$script if [ \"\$(cat \$tz/type 2>/dev/null)\" = \"$THERMAL_TYPE\" ]; then"
  script="$script echo \"temp_mc=\$(cat \$tz/temp)\"; echo \"temp_zone=\$tz\"; break; fi; done;"
  # Emit the GPU nodes RAW and pick them apart locally. The device-side script is delivered as
  # `su -c '<script>'`, so a single quote anywhere inside it (an awk program, a sed expression)
  # closes that quoting and the whole read silently returns nothing - which `check` then reports
  # as every node being "neither pin nor stock", i.e. a false DRIFT on a perfectly pinned device.
  # Parsing on this side keeps the device-side text quote-free.
  if [ "$GPU_STYLE" = kgsl ]; then
    script="$script echo \"gpu_minlvl=\$(cat $KGSL/min_pwrlevel)\";"
    script="$script echo \"gpu_maxlvl=\$(cat $KGSL/max_pwrlevel)\";"
    script="$script echo \"gpu_freqraw=\$(cat $KGSL/gpuclk)\";"
    script="$script echo \"gpu_busyraw=\$(cat $KGSL/gpu_busy_percentage)\";"
  else
    script="$script echo \"gpu_fixraw=\$(cat $GPU_FIX_NODE | head -1)\";"
    script="$script echo \"gpu_freqraw=\$(cat /sys/kernel/ged/hal/current_freqency | head -1)\";"
    script="$script echo \"gpu_busyraw=\$(cat /sys/kernel/ged/hal/gpu_utilization | head -1)\";"
  fi
  su_ "$script"
}

# gpuclk        -> "222000000"                                  ($NF)
# current_freqency -> "<opp_index> <freq_khz>"                   ($NF)
# gpu_busy_percentage -> "26 %"  /  gpu_utilization -> "7 0 100" ($1)
# fix_target_opp_index -> "[GPUFREQ-DEBUG] fix GPU/STACK OPP index: 0/0"   when pinned
#                      -> "[GPUFREQ-DEBUG] fix GPU/STACK OPP index is disabled" when not
derive_gpu() {
  ST[gpu_freq]=$(echo "${ST[gpu_freqraw]:-}" | awk '{print $NF}')
  ST[gpu_busy]=$(echo "${ST[gpu_busyraw]:-}" | awk '{print $1}')
  [ "$GPU_STYLE" = kgsl ] && return 0
  case "${ST[gpu_fixraw]:-}" in
    *"is disabled"*) ST[gpu_fix]=off ;;
    *"index: "*)     ST[gpu_fix]=$(echo "${ST[gpu_fixraw]}" | sed -e 's/.*index: //' -e 's|/.*||') ;;
    *)               ST[gpu_fix]="<unread>" ;;
  esac
}

declare -A ST
load_state() {
  local line k v
  ST=()
  while IFS= read -r line; do
    k=${line%%=*}; v=${line#*=}
    case "$line" in *=*) ST[$k]=$v ;; esac
  done < <(read_state)
  # A read that came back empty is a BROKEN READ, not an unpinned device and not a drifted one.
  # Without this the classifier sees every node as "neither pin nor stock" and prints DRIFT,
  # which reads as "your pin slipped" when the truth is "this script could not see the device".
  # Bail loudly instead: a pin/unpin whose verification cannot run must not look like a verdict.
  if [ "${#ST[@]}" -eq 0 ]; then
    echo "$0: could not read any state from $SERIAL (su read returned nothing)." >&2
    echo "  The device-side read is delivered as su -c '<script>'; check that nothing in it" >&2
    echo "  contains a single quote, and that su still works: adb -s $SERIAL shell su -c id" >&2
    exit 66
  fi
  derive_gpu
}

field() { echo "${ST[$1]:-<unread>}"; }

print_state() {
  local spec label pol pin smin smax
  echo "  device : $DEV_NAME ($SERIAL)"
  for spec in $POLICIES; do
    IFS=: read -r label pol pin smin smax <<<"$spec"
    printf "  %-7s %-8s gov=%-10s cur=%-9s min=%-9s max=%-9s   (pin %s / stock %s-%s)\n" \
      "$label" "$pol" "$(field ${label}_gov)" "$(field ${label}_cur)" \
      "$(field ${label}_min)" "$(field ${label}_max)" "$pin" "$smin" "$smax"
  done
  if [ "$GPU_STYLE" = kgsl ]; then
    printf "  %-7s %-8s pwrlevel=%s..%s  freq=%s %s  busy=%s%%   (pin lvl %s = %s %s / stock lvl %s..%s)\n" \
      gpu kgsl-3d0 "$(field gpu_maxlvl)" "$(field gpu_minlvl)" "$(field gpu_freq)" "$GPU_FREQ_UNIT" \
      "$(field gpu_busy)" "$GPU_PIN_LEVEL" "$GPU_PINNED_FREQ" "$GPU_FREQ_UNIT" \
      "$GPU_STOCK_MAX_LEVEL" "$GPU_STOCK_MIN_LEVEL"
  else
    printf "  %-7s %-8s fix_opp=%-4s freq=%s %s  busy=%s%%   (pin idx %s = %s %s / stock off)\n" \
      gpu gpufreqv2 "$(field gpu_fix)" "$(field gpu_freq)" "$GPU_FREQ_UNIT" "$(field gpu_busy)" \
      "$GPU_PIN_LEVEL" "$GPU_PINNED_FREQ" "$GPU_FREQ_UNIT"
  fi
  printf "  %-7s %-8s %s mC = %s C   (%s)\n" thermal "$THERMAL_TYPE" \
    "$(field temp_mc)" "$(awk -v t="$(field temp_mc)" 'BEGIN{if(t+0==0){print "?"}else{printf "%.1f", t/1000}}')" \
    "$(field temp_zone)"
}

# --- actions ----------------------------------------------------------------------------------
# Write order is min -> floor, then max -> target, then min -> target. Setting min above the
# current max (or max below the current min) is clamped by cpufreq, so a naive two-write pin
# succeeds on one device and silently half-applies on another depending on where stock sits
# relative to the target. Dropping min to the policy floor first makes the order stock-agnostic.
do_pin() {
  local spec label pol pin smin smax script=""
  for spec in $POLICIES; do
    IFS=: read -r label pol pin smin smax <<<"$spec"
    script="$script echo \$(cat $CPUFREQ/$pol/cpuinfo_min_freq) > $CPUFREQ/$pol/scaling_min_freq;"
    script="$script echo $pin > $CPUFREQ/$pol/scaling_max_freq;"
    script="$script echo $pin > $CPUFREQ/$pol/scaling_min_freq;"
  done
  su_ "$script" >/dev/null
  if [ "$GPU_STYLE" = kgsl ]; then
    # Collapse the pwrlevel range onto level 0. This also unlocks the top step: the devfreq
    # governor's own max_freq sits one step below it, so a devfreq min_freq/max_freq pin cannot
    # reach 1100 MHz at all.
    su_ "echo $GPU_PIN_LEVEL > $KGSL/min_pwrlevel; echo $GPU_PIN_LEVEL > $KGSL/max_pwrlevel" >/dev/null
  else
    su_ "echo $GPU_PIN_LEVEL > $GPU_FIX_NODE" >/dev/null
  fi
}

do_unpin() {
  local spec label pol pin smin smax script=""
  for spec in $POLICIES; do
    IFS=: read -r label pol pin smin smax <<<"$spec"
    script="$script echo \$(cat $CPUFREQ/$pol/cpuinfo_min_freq) > $CPUFREQ/$pol/scaling_min_freq;"
    script="$script echo $smax > $CPUFREQ/$pol/scaling_max_freq;"
    script="$script echo $smin > $CPUFREQ/$pol/scaling_min_freq;"
  done
  su_ "$script" >/dev/null
  if [ "$GPU_STYLE" = kgsl ]; then
    su_ "echo $GPU_STOCK_MIN_LEVEL > $KGSL/min_pwrlevel; echo $GPU_STOCK_MAX_LEVEL > $KGSL/max_pwrlevel" >/dev/null
  else
    su_ "echo -1 > $GPU_FIX_NODE" >/dev/null
  fi
}

# Classify every node this script writes as at-pin or not-at-pin.
#
# NOT "at-pin / at-stock / neither". An earlier version compared against the hardcoded stock
# range and called anything else DRIFT, and ColorOS broke it within seconds: after an unpin
# restored policy4 to 300000-3500000, the Oppo performance daemon lowered the max to 3200000 on
# its own, and the next check reported DRIFT on a correctly released device. Stock maxima are
# daemon-managed and are NOT constants. So the only thing asserted here is our own pin - which
# is the question that actually matters ("is this run pinned?"). The hardcoded stock values
# still drive the restore path, where handing the range back to the governor is all they have
# to do.
do_check() {
  local spec label pol pin smin smax n_pin=0 n_stock=0 n_other=0 drift="" notes=""
  for spec in $POLICIES; do
    IFS=: read -r label pol pin smin smax <<<"$spec"
    local mn mx cu
    mn=$(field ${label}_min); mx=$(field ${label}_max); cu=$(field ${label}_cur)
    if [ "$mn" = "$pin" ] && [ "$mx" = "$pin" ]; then
      n_pin=$((n_pin+1))
      # min==max leaves the governor no room, so a cur that is not the pin means something
      # outside cpufreq (thermal engine, vendor limiter) is overriding it.
      [ "$cu" = "$pin" ] || drift="$drift ${label}(${pol}) pinned to $pin but scaling_cur_freq=$cu;"
    elif [ "$mn" = "$mx" ]; then
      # Clamped, but not by us. Someone else (game mode, thermal engine) is holding this policy
      # at a fixed frequency, which is just as fatal to comparability as a missing pin.
      n_other=$((n_other+1))
      drift="$drift ${label}(${pol}) is clamped at $mn by something other than this script (our pin is $pin);"
    else
      n_stock=$((n_stock+1))
      [ "$mn" = "$smin" ] && [ "$mx" = "$smax" ] || \
        notes="$notes ${label}(${pol}) unpinned, range $mn-$mx (recorded stock $smin-$smax, which vendor daemons move);"
    fi
  done
  if [ "$GPU_STYLE" = kgsl ]; then
    if [ "$(field gpu_minlvl)" = "$GPU_PIN_LEVEL" ] && [ "$(field gpu_maxlvl)" = "$GPU_PIN_LEVEL" ]; then
      n_pin=$((n_pin+1))
      [ "$(field gpu_freq)" = "$GPU_PINNED_FREQ" ] || \
        drift="$drift gpu pinned to pwrlevel $GPU_PIN_LEVEL but gpuclk=$(field gpu_freq) (expected $GPU_PINNED_FREQ);"
    elif [ "$(field gpu_minlvl)" = "$GPU_STOCK_MIN_LEVEL" ] && [ "$(field gpu_maxlvl)" = "$GPU_STOCK_MAX_LEVEL" ]; then
      n_stock=$((n_stock+1))
    else
      n_other=$((n_other+1))
      drift="$drift gpu pwrlevel range $(field gpu_maxlvl)..$(field gpu_minlvl) is neither pin nor stock;"
    fi
  else
    if [ "$(field gpu_fix)" = "$GPU_PIN_LEVEL" ]; then
      n_pin=$((n_pin+1))
      # The GPU parks its rail when idle and then reports a fallback frequency; only treat a
      # mismatch as drift while the GPU is actually doing something.
      if [ "$(field gpu_freq)" != "$GPU_PINNED_FREQ" ]; then
        if [ "$(field gpu_busy)" = "0" ]; then
          notes="$notes gpu fixed at OPP $GPU_PIN_LEVEL, freq reads $(field gpu_freq) with busy=0 - rail parked, not drift;"
        else
          drift="$drift gpu fixed at OPP $GPU_PIN_LEVEL but current_freqency=$(field gpu_freq) (expected $GPU_PINNED_FREQ) at busy=$(field gpu_busy)%;"
        fi
      fi
    elif [ "$(field gpu_fix)" = "off" ]; then
      n_stock=$((n_stock+1))
    else
      n_other=$((n_other+1))
      drift="$drift gpu fix_target_opp_index='$(field gpu_fix)' is neither $GPU_PIN_LEVEL nor off;"
    fi
  fi

  # Notes are commentary (a parked GPU rail is not a slipped pin) and are collected in their own
  # variable rather than tagged inside $drift and filtered back out: the glob that would strip
  # them is greedy, so it could swallow a real drift message that happened to follow one.
  show() { [ -n "${1// /}" ] && echo "$1" | tr ';' '\n' | sed -e "s|^ *|    $2|" -e "/^ *$2 *\$/d"; return 0; }
  echo
  if [ -n "${drift// /}" ]; then
    echo "  VERDICT: DRIFT"
    show "$drift" "- "; show "$notes" "note: "
    return 1
  fi
  if [ "$n_other" -gt 0 ] || { [ "$n_pin" -gt 0 ] && [ "$n_stock" -gt 0 ]; }; then
    echo "  VERDICT: DRIFT (partially pinned: $n_pin at pin, $n_stock at stock, $n_other neither)"
    show "$notes" "note: "
    return 1
  fi
  if [ "$n_pin" -gt 0 ] && [ "$n_stock" -eq 0 ]; then
    echo "  VERDICT: PINNED - all $n_pin pinned nodes at their pins, live frequencies match."
    show "$notes" "note: "
    return 0
  fi
  echo "  VERDICT: UNPINNED - none of the $n_stock nodes is at a pin this script set; the vendor"
  echo "           governors have their range back and nothing this script writes is in effect."
  show "$notes" "note: "
  return 2
}

# --- main -------------------------------------------------------------------------------------
case "$ACTION" in
  check)
    load_state
    echo "== check =="
    print_state
    do_check
    exit $?
    ;;
  pin)
    load_state
    echo "== before pin =="
    print_state
    do_pin
    sleep 1
    load_state
    echo
    echo "== after pin =="
    print_state
    do_check
    rc=$?
    [ "$rc" = 0 ] || echo "  (pin did not take - do not measure against this)" >&2
    exit $rc
    ;;
  unpin)
    load_state
    echo "== before unpin =="
    print_state
    do_unpin
    sleep 1
    load_state
    echo
    echo "== after unpin =="
    print_state
    do_check
    rc=$?
    # 2 (UNPINNED) is success for this action.
    [ "$rc" = 2 ] && exit 0
    echo "  (unpin did not fully restore - the device is still clamped)" >&2
    exit 1
    ;;
esac
