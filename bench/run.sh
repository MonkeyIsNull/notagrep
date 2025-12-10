#!/bin/bash
# Benchmark notagrep against grep and ripgrep

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DATA_DIR="$SCRIPT_DIR/data"

GREP="/usr/bin/grep"
RG="/opt/homebrew/bin/rg"
GG="git"  # git grep --no-index
NOTAGREP="$PROJECT_DIR/notagrep"

RUNS=5  # Number of runs per benchmark

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# Check prerequisites
check_prereqs() {
    local missing=0

    if [ ! -x "$NOTAGREP" ]; then
        echo "Error: notagrep not found at $NOTAGREP"
        echo "Run 'make' first to build it."
        missing=1
    fi

    if [ ! -x "$RG" ]; then
        echo "Warning: ripgrep not found at $RG"
        RG=""
    fi

    if ! command -v $GG &> /dev/null; then
        echo "Warning: git not found, skipping git-grep benchmarks"
        GG=""
    fi

    if [ ! -f "$DATA_DIR/small.txt" ]; then
        echo "Error: Benchmark data not found."
        echo "Run 'make bench-data' or './bench/gen_data.sh' first."
        missing=1
    fi

    if [ $missing -eq 1 ]; then
        exit 1
    fi
}

# Run a command and return elapsed time in seconds
time_cmd() {
    local cmd="$1"

    # Use perl for high-resolution timing
    local start=$(perl -MTime::HiRes=time -e 'print time')
    eval "$cmd" > /dev/null 2>&1 || true
    local end=$(perl -MTime::HiRes=time -e 'print time')

    echo "$end - $start" | bc -l
}

# Run benchmark with multiple iterations
run_bench() {
    local name="$1"
    local cmd="$2"
    local total=0

    # Warm up (cache the file)
    eval "$cmd" > /dev/null 2>&1 || true

    for ((i=1; i<=RUNS; i++)); do
        local elapsed=$(time_cmd "$cmd")
        total=$(echo "$total + $elapsed" | bc -l)
    done

    local avg=$(echo "scale=4; $total / $RUNS" | bc -l)
    echo "$avg"
}

# Format time in milliseconds with consistent width
format_time() {
    local t="$1"
    local ms=$(echo "scale=1; $t * 1000" | bc -l)
    printf "%7.1fms" $ms
}

# Calculate and format speedup ratio
calc_speedup() {
    local base="$1"
    local compare="$2"

    if (( $(echo "$compare > 0" | bc -l) )); then
        echo "scale=2; $base / $compare" | bc -l
    else
        echo "0"
    fi
}

# Format speedup with color
format_speedup() {
    local ratio="$1"
    local label="$2"

    if (( $(echo "$ratio >= 1" | bc -l) )); then
        printf "${GREEN}%5.1fx${NC}" $ratio
    else
        local inv=$(echo "scale=1; 1 / $ratio" | bc -l)
        printf "${RED}%5.1fx slower${NC}" $inv
    fi
}

# Print table separator
sep() {
    echo "------------------------+----------+----------+----------+----------+------------+------------+------------"
}

# Print table header
header() {
    printf "${BOLD}%-24s${NC}" "Test"
    printf "|${BOLD}%9s ${NC}" "grep"
    printf "|${BOLD}%9s ${NC}" "git-grep"
    printf "|${BOLD}%9s ${NC}" "ripgrep"
    printf "|${BOLD}%9s ${NC}" "notagrep"
    printf "|${BOLD}%11s ${NC}" "vs grep"
    printf "|${BOLD}%11s ${NC}" "vs git-grep"
    printf "|${BOLD}%11s${NC}\n" "vs ripgrep"
}

# Print a result row
print_row() {
    local name="$1"
    local t_grep="$2"
    local t_gg="$3"
    local t_rg="$4"
    local t_ng="$5"

    printf "%-24s" "$name"
    printf "|%s " "$(format_time $t_grep)"

    if [ -n "$t_gg" ]; then
        printf "|%s " "$(format_time $t_gg)"
    else
        printf "|%9s " "N/A"
    fi

    if [ -n "$t_rg" ]; then
        printf "|%s " "$(format_time $t_rg)"
    else
        printf "|%9s " "N/A"
    fi

    printf "|%s " "$(format_time $t_ng)"

    # vs grep
    local vs_grep=$(calc_speedup $t_grep $t_ng)
    printf "|"
    format_speedup $vs_grep "grep"
    printf "     "

    # vs git-grep
    if [ -n "$t_gg" ]; then
        local vs_gg=$(calc_speedup $t_gg $t_ng)
        printf "|"
        format_speedup $vs_gg "gg"
        printf "     "
    else
        printf "|%11s " "N/A"
    fi

    # vs ripgrep
    if [ -n "$t_rg" ]; then
        local vs_rg=$(calc_speedup $t_rg $t_ng)
        printf "|"
        format_speedup $vs_rg "rg"
    else
        printf "|%11s" "N/A"
    fi

    echo
}

# ============================================================================
# Main
# ============================================================================

check_prereqs

echo
echo "${BOLD}notagrep benchmark suite${NC}"
echo "========================"
echo
echo "Tools:"
echo "  grep:     $GREP"
[ -n "$GG" ] && echo "  git-grep: $GG grep --no-index"
[ -n "$RG" ] && echo "  ripgrep:  $RG"
echo "  notagrep: $NOTAGREP"
echo
echo "Runs per test: $RUNS (average reported)"
echo

# Find system include directory
INCLUDE_DIR=""
if [ -d "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include" ]; then
    INCLUDE_DIR="/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include"
elif [ -d "/usr/include" ]; then
    INCLUDE_DIR="/usr/include"
fi

# Store all results for summary
declare -a ALL_GREP_TIMES
declare -a ALL_GG_TIMES
declare -a ALL_RG_TIMES
declare -a ALL_NG_TIMES
declare -a ALL_NAMES
idx=0

# ============================================================================
# SINGLE FILE BENCHMARKS
# ============================================================================

echo "${BOLD}Single File Tests${NC}"
echo "(100KB = small.txt, 10MB = medium.txt, 100MB = large.txt)"
echo
sep
header
sep

# --- Fixed String Search (-F) ---

t_grep=$(run_bench "grep" "$GREP -F 'function' '$DATA_DIR/small.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -F 'function' -- '$DATA_DIR/small.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG -F 'function' '$DATA_DIR/small.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP -F 'function' '$DATA_DIR/small.txt'")
print_row "literal 100KB" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="literal 100KB"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

t_grep=$(run_bench "grep" "$GREP -F 'function' '$DATA_DIR/medium.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -F 'function' -- '$DATA_DIR/medium.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG -F 'function' '$DATA_DIR/medium.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP -F 'function' '$DATA_DIR/medium.txt'")
print_row "literal 10MB" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="literal 10MB"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

t_grep=$(run_bench "grep" "$GREP -F 'function' '$DATA_DIR/large.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -F 'function' -- '$DATA_DIR/large.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG -F 'function' '$DATA_DIR/large.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP -F 'function' '$DATA_DIR/large.txt'")
print_row "literal 100MB" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="literal 100MB"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

# --- Case-insensitive search ---

t_grep=$(run_bench "grep" "$GREP -iF 'FUNCTION' '$DATA_DIR/medium.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -iF 'FUNCTION' -- '$DATA_DIR/medium.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG -iF 'FUNCTION' '$DATA_DIR/medium.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP -iF 'FUNCTION' '$DATA_DIR/medium.txt'")
print_row "literal -i 10MB" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="literal -i 10MB"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

sep

# --- Regex: Simple patterns ---

t_grep=$(run_bench "grep" "$GREP -E 'func' '$DATA_DIR/medium.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -E 'func' -- '$DATA_DIR/medium.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG 'func' '$DATA_DIR/medium.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP 'func' '$DATA_DIR/medium.txt'")
print_row "regex literal" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="regex literal"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

t_grep=$(run_bench "grep" "$GREP -E 'func.*return' '$DATA_DIR/medium.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -E 'func.*return' -- '$DATA_DIR/medium.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG 'func.*return' '$DATA_DIR/medium.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP 'func.*return' '$DATA_DIR/medium.txt'")
print_row "regex .*" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="regex .*"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

# --- Regex: Character classes ---

t_grep=$(run_bench "grep" "$GREP -E '[a-z]+_[0-9]+' '$DATA_DIR/medium.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -E '[a-z]+_[0-9]+' -- '$DATA_DIR/medium.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG '[a-z]+_[0-9]+' '$DATA_DIR/medium.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP '[a-z]+_[0-9]+' '$DATA_DIR/medium.txt'")
print_row "regex [a-z]+_[0-9]+" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="regex [a-z]+_[0-9]+"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

t_grep=$(run_bench "grep" "$GREP -E '[0-9]{1,3}\.[0-9]{1,3}' '$DATA_DIR/medium.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -E '[0-9]{1,3}\\.[0-9]{1,3}' -- '$DATA_DIR/medium.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG '[0-9]{1,3}\.[0-9]{1,3}' '$DATA_DIR/medium.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP '[0-9]{1,3}\.[0-9]{1,3}' '$DATA_DIR/medium.txt'")
print_row "regex IP-like" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="regex IP-like"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

# --- Regex: Alternation ---

t_grep=$(run_bench "grep" "$GREP -E '(error|warning|fatal)' '$DATA_DIR/medium.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -E '(error|warning|fatal)' -- '$DATA_DIR/medium.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG '(error|warning|fatal)' '$DATA_DIR/medium.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP '(error|warning|fatal)' '$DATA_DIR/medium.txt'")
print_row "regex alternation" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="regex alternation"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

t_grep=$(run_bench "grep" "$GREP -E '(int|char|void|long|short|float|double)' '$DATA_DIR/medium.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -E '(int|char|void|long|short|float|double)' -- '$DATA_DIR/medium.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG '(int|char|void|long|short|float|double)' '$DATA_DIR/medium.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP '(int|char|void|long|short|float|double)' '$DATA_DIR/medium.txt'")
print_row "regex 7-way alt" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="regex 7-way alt"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

# --- Regex: Anchored patterns ---

t_grep=$(run_bench "grep" "$GREP -E '^#include' '$DATA_DIR/medium.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -E '^#include' -- '$DATA_DIR/medium.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG '^#include' '$DATA_DIR/medium.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP '^#include' '$DATA_DIR/medium.txt'")
print_row "regex ^anchor" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="regex ^anchor"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

t_grep=$(run_bench "grep" "$GREP -E ';$' '$DATA_DIR/medium.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -E ';$' -- '$DATA_DIR/medium.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG ';$' '$DATA_DIR/medium.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP ';\$' '$DATA_DIR/medium.txt'")
print_row "regex anchor\$" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="regex anchor\$"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

# --- Regex: Complex patterns ---

t_grep=$(run_bench "grep" "$GREP -E '(struct|typedef).*\{' '$DATA_DIR/medium.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -E '(struct|typedef).*\\{' -- '$DATA_DIR/medium.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG '(struct|typedef).*\{' '$DATA_DIR/medium.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP '(struct|typedef).*\{' '$DATA_DIR/medium.txt'")
print_row "regex struct def" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="regex struct def"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

# --- Regex: Dot-star heavy (pathological for some engines) ---

t_grep=$(run_bench "grep" "$GREP -E '.*function.*' '$DATA_DIR/medium.txt'")
[ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -E '.*function.*' -- '$DATA_DIR/medium.txt'") || t_gg=""
[ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG '.*function.*' '$DATA_DIR/medium.txt'") || t_rg=""
t_ng=$(run_bench "notagrep" "$NOTAGREP '.*function.*' '$DATA_DIR/medium.txt'")
print_row "regex .*X.*" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
ALL_NAMES[$idx]="regex .*X.*"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

sep
echo

# ============================================================================
# DIRECTORY TRAVERSAL BENCHMARKS
# ============================================================================

if [ -n "$INCLUDE_DIR" ]; then
    echo "${BOLD}Directory Traversal Tests${NC}"
    echo "(searching: $INCLUDE_DIR)"
    echo
    sep
    header
    sep

    # Literal recursive
    t_grep=$(run_bench "grep" "$GREP -rF 'include' '$INCLUDE_DIR'")
    [ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -rF 'include' -- '$INCLUDE_DIR'") || t_gg=""
    [ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG -F 'include' '$INCLUDE_DIR'") || t_rg=""
    t_ng=$(run_bench "notagrep" "$NOTAGREP -F 'include' '$INCLUDE_DIR'")
    print_row "dir literal" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
    ALL_NAMES[$idx]="dir literal"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

    # Regex recursive
    t_grep=$(run_bench "grep" "$GREP -rE '[a-z]+_t' '$INCLUDE_DIR'")
    [ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -rE '[a-z]+_t' -- '$INCLUDE_DIR'") || t_gg=""
    [ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG '[a-z]+_t' '$INCLUDE_DIR'") || t_rg=""
    t_ng=$(run_bench "notagrep" "$NOTAGREP '[a-z]+_t' '$INCLUDE_DIR'")
    print_row "dir regex" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
    ALL_NAMES[$idx]="dir regex"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

    # Files-only mode (-l)
    t_grep=$(run_bench "grep" "$GREP -rlF 'stdio' '$INCLUDE_DIR'")
    [ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -rlF 'stdio' -- '$INCLUDE_DIR'") || t_gg=""
    [ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG -lF 'stdio' '$INCLUDE_DIR'") || t_rg=""
    t_ng=$(run_bench "notagrep" "$NOTAGREP -lF 'stdio' '$INCLUDE_DIR'")
    print_row "dir -l (files only)" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
    ALL_NAMES[$idx]="dir -l"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

    # Case-insensitive recursive
    t_grep=$(run_bench "grep" "$GREP -riF 'ERROR' '$INCLUDE_DIR'")
    [ -n "$GG" ] && t_gg=$(run_bench "git-grep" "$GG grep --no-index -riF 'ERROR' -- '$INCLUDE_DIR'") || t_gg=""
    [ -n "$RG" ] && t_rg=$(run_bench "rg" "$RG -iF 'ERROR' '$INCLUDE_DIR'") || t_rg=""
    t_ng=$(run_bench "notagrep" "$NOTAGREP -iF 'ERROR' '$INCLUDE_DIR'")
    print_row "dir -i literal" "$t_grep" "$t_gg" "$t_rg" "$t_ng"
    ALL_NAMES[$idx]="dir -i literal"; ALL_GREP_TIMES[$idx]=$t_grep; ALL_GG_TIMES[$idx]=$t_gg; ALL_RG_TIMES[$idx]=$t_rg; ALL_NG_TIMES[$idx]=$t_ng; ((idx++))

    sep
    echo
fi

# ============================================================================
# SUMMARY
# ============================================================================

echo "${BOLD}Summary${NC}"
echo "======="
echo

# Calculate stats
total_vs_grep=0
total_vs_gg=0
total_vs_rg=0
wins_vs_grep=0
wins_vs_gg=0
wins_vs_rg=0
count=0
gg_count=0

for ((i=0; i<idx; i++)); do
    if [ -n "${ALL_NG_TIMES[$i]}" ] && (( $(echo "${ALL_NG_TIMES[$i]} > 0" | bc -l) )); then
        vs_grep=$(calc_speedup ${ALL_GREP_TIMES[$i]} ${ALL_NG_TIMES[$i]})
        if [ "$vs_grep" != "0" ]; then
            total_vs_grep=$(echo "$total_vs_grep + $vs_grep" | bc -l)
            if (( $(echo "$vs_grep >= 1" | bc -l) )); then
                ((wins_vs_grep++))
            fi
        fi

        if [ -n "${ALL_GG_TIMES[$i]}" ] && [ "${ALL_GG_TIMES[$i]}" != "" ]; then
            vs_gg=$(calc_speedup ${ALL_GG_TIMES[$i]} ${ALL_NG_TIMES[$i]})
            if [ "$vs_gg" != "0" ]; then
                total_vs_gg=$(echo "$total_vs_gg + $vs_gg" | bc -l)
                if (( $(echo "$vs_gg >= 1" | bc -l) )); then
                    ((wins_vs_gg++))
                fi
                ((gg_count++))
            fi
        fi

        if [ -n "${ALL_RG_TIMES[$i]}" ]; then
            vs_rg=$(calc_speedup ${ALL_RG_TIMES[$i]} ${ALL_NG_TIMES[$i]})
            if [ "$vs_rg" != "0" ]; then
                total_vs_rg=$(echo "$total_vs_rg + $vs_rg" | bc -l)
                if (( $(echo "$vs_rg >= 1" | bc -l) )); then
                    ((wins_vs_rg++))
                fi
            fi
        fi
        ((count++))
    fi
done

avg_vs_grep=$(echo "scale=2; $total_vs_grep / $count" | bc -l)
[ $gg_count -gt 0 ] && avg_vs_gg=$(echo "scale=2; $total_vs_gg / $gg_count" | bc -l) || avg_vs_gg="0"
avg_vs_rg=$(echo "scale=2; $total_vs_rg / $count" | bc -l)

echo "Tests run: $count"
echo
echo "vs grep:"
echo "  Average speedup: ${avg_vs_grep}x"
echo "  Tests faster:    $wins_vs_grep / $count"
echo

if [ -n "$GG" ] && [ $gg_count -gt 0 ]; then
    echo "vs git-grep:"
    echo "  Average speedup: ${avg_vs_gg}x"
    echo "  Tests faster:    $wins_vs_gg / $gg_count"
    echo

    if (( $(echo "$avg_vs_gg >= 1" | bc -l) )); then
        echo -e "${GREEN}notagrep is ${avg_vs_gg}x faster than git-grep on average${NC}"
    else
        inv=$(echo "scale=2; 1 / $avg_vs_gg" | bc -l)
        echo -e "${YELLOW}notagrep is ${inv}x slower than git-grep on average${NC}"
    fi
    echo
fi

if [ -n "$RG" ]; then
    echo "vs ripgrep:"
    echo "  Average speedup: ${avg_vs_rg}x"
    echo "  Tests faster:    $wins_vs_rg / $count"
    echo

    if (( $(echo "$avg_vs_rg >= 1" | bc -l) )); then
        echo -e "${GREEN}notagrep is ${avg_vs_rg}x faster than ripgrep on average${NC}"
    else
        inv=$(echo "scale=2; 1 / $avg_vs_rg" | bc -l)
        echo -e "${YELLOW}notagrep is ${inv}x slower than ripgrep on average${NC}"
    fi
fi

echo
