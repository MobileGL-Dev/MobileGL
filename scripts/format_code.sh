#!/usr/bin/env bash
set -uo pipefail
if [ -t 1 ]; then
    CYAN=$'\033[36m'
    GREEN=$'\033[32m'
    YELLOW=$'\033[33m'
    RED=$'\033[31m'
    BOLD=$'\033[1m'
    RESET=$'\033[0m'
else
    CYAN='' ; GREEN='' ; YELLOW='' ; RED='' ; BOLD='' ; RESET=''
fi

OK="✔"; SKIP="⤼"; FAIL="✖"

cd "$(dirname "$0")" || { printf "%s\n" "Cannot cd to script dir"; exit 1; }

DIR="../MobileGL"
CLANG_FORMAT_CONFIG="../.clang-format"

CLANG_FORMAT_PATH="$(command -v clang-format-23 2>/dev/null || true)"
if [ -z "$CLANG_FORMAT_PATH" ]; then
    CLANG_FORMAT_PATH="$(command -v clang-format 2>/dev/null || true)"
fi
if [ -z "$CLANG_FORMAT_PATH" ]; then
    printf '%s\n' "${RED}Error:${RESET} no clang-format found in PATH (tried clang-format-21 and clang-format)."
    exit 1
fi

if [ ! -f "$CLANG_FORMAT_CONFIG" ]; then
    printf '%s\n' "${RED}Error:${RESET} clang-format config not found at ${CLANG_FORMAT_CONFIG}"
    exit 1
fi

printf '%s %s\n' "${CYAN}${BOLD}Using clang-format:${RESET}" "${CLANG_FORMAT_PATH}"
printf '%s %s\n' "${CYAN}${BOLD}Config file:${RESET}" "${CLANG_FORMAT_CONFIG}"
DIR_ABS="(missing)"
if [ -d "$DIR" ]; then
    DIR_ABS="$(cd "$DIR" && pwd)"
fi
printf '%s %s -> %s\n\n' "${CYAN}${BOLD}Target dir:${RESET}" "${DIR}" "${DIR_ABS}"

if [ "$DIR_ABS" = "(missing)" ]; then
    printf '%s\n' "${YELLOW}Target directory '${DIR}' does not exist relative to script dir '$(pwd)'.${RESET}"
    printf '%s\n' "ls -la of current dir:"
    ls -la
    exit 1
fi

candidates=()
while IFS= read -r -d '' f; do
    candidates+=("$f")
done < <(
    find "$DIR" \
        \( -type d \( -name out -o -name build \) -prune \) -o \
        -type f \( -name "*.h" -o -name "*.hpp" -o -name "*.cpp" -o -name "*.cc" -o -name "*.c" \) -print0 2>/dev/null
)

printf '%s\n' "${YELLOW}find results:${RESET} ${#candidates[@]} candidate(s) found."
if [ "${#candidates[@]}" -eq 0 ]; then
    printf '%s\n' "${YELLOW}No files matched.${RESET}"
    ls -la "$DIR" | sed -n '1,40p'
    exit 0
fi

todo=()
skipped_def=0
skipped_big=0
skipped_err=0
i=0
total_candidates=${#candidates[@]}
for f in "${candidates[@]}"; do
    i=$((i+1))
    if (( i % 50 == 0 )); then
        printf '%s\n' "${CYAN}Filtering progress:${RESET} ${i}/${total_candidates}"
    fi

    base=$(basename "$f" 2>/dev/null) || base=""
    if [ "$base" = "Definitions.cpp" ]; then
        skipped_def=$((skipped_def+1))
        continue
    fi

    line_count=$(awk 'END{print NR+0}' "$f" 2>/dev/null) || { line_count=0; skipped_err=$((skipped_err+1)); printf '%s\n' "${YELLOW}Warn:${RESET} awk failed on '$f' (skipping)"; continue; }
    if ! [[ "$line_count" =~ ^[0-9]+$ ]]; then
        skipped_err=$((skipped_err+1))
        printf '%s\n' "${YELLOW}Warn:${RESET} got non-numeric line count for '$f' -> '${line_count}', skipping"
        continue
    fi
    if [ "$line_count" -gt 10000 ]; then
        skipped_big=$((skipped_big+1))
        continue
    fi
    todo+=("$f")
done

printf '\n%s\n\n' "${YELLOW}Filtering result:${RESET} total candidates=${total_candidates}, to-format=${#todo[@]}, skipped Definitions.cpp=${skipped_def}, skipped >10000=${skipped_big}, awk-errors=${skipped_err}"

total=${#todo[@]}
if [ "$total" -eq 0 ]; then
    printf '%s\n' "${YELLOW}Nothing to format.${RESET}"
    exit 0
fi

OK="✔"; SKIP="⤼"; FAIL="✖"
ARROW="╰─>"; SIMPLE_ARROW="─>"

draw_progress() {
    local current=$1 total=$2 width=36

    local percent=0
    if [ "$total" -gt 0 ]; then
        percent=$(( (current * 100 + total/2) / total ))
        if [ "$percent" -gt 100 ]; then percent=100; fi
    fi

    local filled=0
    if [ "$total" -gt 0 ]; then
        filled=$(( (current * width + total - 1) / total ))
    fi
    if [ "$filled" -gt "$width" ]; then filled=$width; fi
    if [ "$filled" -lt 0 ]; then filled=0; fi

    local empty=$(( width - filled ))

    local bar_filled=""
    local bar_empty=""
    if [ "$filled" -gt 0 ]; then
        bar_filled="$(printf '%0.s█' $(seq 1 "$filled") 2>/dev/null || true)"
    fi
    if [ "$empty" -gt 0 ]; then
        bar_empty="$(printf '%0.s░' $(seq 1 "$empty") 2>/dev/null || true)"
    fi

    printf "\r%s [%3d/%3d] %3d%% |%s%s| " "${CYAN}${BOLD}Progress${RESET}" "$current" "$total" "$percent" "$bar_filled" "$bar_empty"
}

processed=0
trap 'printf "\nInterrupted\n"; exit' INT TERM

for f in "${todo[@]}"; do
    processed=$((processed+1))
    draw_progress "$processed" "$total"

    printf '\n%s %s %s\n' "${CYAN}Formatting${RESET}" "$SIMPLE_ARROW" "$f"
    if "$CLANG_FORMAT_PATH" -i -style="file:${CLANG_FORMAT_CONFIG}" "$f"; then
        printf '   %s %s %s\n' "$ARROW" "${GREEN}${OK}${RESET}" "done"
    else
        printf '   %s %s %s\n' "$ARROW" "${RED}${FAIL}${RESET}" "failed"
    fi
    draw_progress "$processed" "$total"
done

printf '\n\n%s %s\n' "${GREEN}All done!${RESET}" "Formatted: ${processed}, Skipped: $((skipped_def + skipped_big + skipped_err))"
