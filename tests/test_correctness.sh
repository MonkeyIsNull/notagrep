#!/bin/bash
# Correctness test suite for notagrep
# Compares output against grep to ensure matching behavior

NOTAGREP="${NOTAGREP:-./notagrep}"
TESTDIR="/tmp/notagrep-tests"
PASSED=0
FAILED=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

cleanup() {
    rm -rf "$TESTDIR"
}

setup() {
    cleanup
    mkdir -p "$TESTDIR"

    # Create test files
    cat > "$TESTDIR/simple.txt" << 'EOF'
hello world
this is a test
hello again
testing 123
HELLO uppercase
EOF

    cat > "$TESTDIR/special.txt" << 'EOF'
coralogix test line
another line
cora in the middle
final cora
EOF

    cat > "$TESTDIR/patterns.txt" << 'EOF'
error: something went wrong
warning: be careful
fatal error occurred
info: all good
debug message here
error and warning on same line
EOF

    cat > "$TESTDIR/code.txt" << 'EOF'
int main() {
    char *str = "hello";
    void func();
    long value = 42;
    short x;
    float y;
    double z;
    return 0;
}
EOF

    cat > "$TESTDIR/anchors.txt" << 'EOF'
#include <stdio.h>
#include <stdlib.h>
int main() {
    printf("hello");
    return 0;
}
EOF
}

run_test() {
    local name="$1"
    local pattern="$2"
    local file="$3"
    local flags="${4:-}"

    # grep without -H doesn't show filename for single file
    # notagrep with -H always shows filename, so strip it for comparison
    local grep_out=$(grep $flags -E "$pattern" "$file" 2>/dev/null | sort || true)
    # Strip "filename:" prefix from notagrep output
    local ng_out=$($NOTAGREP $flags "$pattern" "$file" 2>/dev/null | sed 's/^[^:]*://' | sort || true)

    if [ "$grep_out" = "$ng_out" ]; then
        echo -e "${GREEN}PASS${NC}: $name"
        ((PASSED++))
    else
        echo -e "${RED}FAIL${NC}: $name"
        echo "  Pattern: $pattern"
        echo "  File: $file"
        echo "  Flags: $flags"
        echo "  grep output:"
        echo "$grep_out" | head -5 | sed 's/^/    /'
        echo "  notagrep output:"
        echo "$ng_out" | head -5 | sed 's/^/    /'
        ((FAILED++))
    fi
}

run_count_test() {
    local name="$1"
    local pattern="$2"
    local file="$3"
    local flags="${4:-}"

    # For count test, grep -c counts matching lines, notagrep should match
    local grep_count=$(grep $flags -c -E "$pattern" "$file" 2>/dev/null || echo "0")
    local ng_count=$($NOTAGREP $flags -c "$pattern" "$file" 2>/dev/null | cut -d: -f2 || echo "0")

    if [ "$grep_count" = "$ng_count" ]; then
        echo -e "${GREEN}PASS${NC}: $name (count=$grep_count)"
        ((PASSED++))
    else
        echo -e "${RED}FAIL${NC}: $name"
        echo "  Pattern: $pattern"
        echo "  grep count: $grep_count"
        echo "  notagrep count: $ng_count"
        ((FAILED++))
    fi
}

echo "notagrep correctness test suite"
echo "================================"
echo ""

setup

echo "Basic literal tests:"
run_test "simple literal" "hello" "$TESTDIR/simple.txt"
run_test "4-char literal" "cora" "$TESTDIR/special.txt"
run_test "literal not found" "xyz123" "$TESTDIR/simple.txt"
run_test "literal at start" "hello" "$TESTDIR/simple.txt"
run_test "literal at end" "123" "$TESTDIR/simple.txt"

echo ""
echo "Case insensitive tests:"
run_test "case insensitive" "hello" "$TESTDIR/simple.txt" "-i"
run_test "case insensitive HELLO" "HELLO" "$TESTDIR/simple.txt" "-i"

echo ""
echo "Regex pattern tests:"
run_test "alternation 3-way" "error|warning|fatal" "$TESTDIR/patterns.txt"
run_test "alternation 7-way" "int|char|void|long|short|float|double" "$TESTDIR/code.txt"
run_test "character class" "[a-z]+" "$TESTDIR/simple.txt"
run_test "dot star" ".*test.*" "$TESTDIR/simple.txt"
run_test "anchored start" "^#include" "$TESTDIR/anchors.txt"
run_test "anchored end" "0;$" "$TESTDIR/anchors.txt"

echo ""
echo "Count tests (-c):"
run_count_test "count simple" "hello" "$TESTDIR/simple.txt"
run_count_test "count 4-char" "cora" "$TESTDIR/special.txt"
run_count_test "count alternation" "error|warning" "$TESTDIR/patterns.txt"

echo ""
echo "================================"
echo "Results: $PASSED passed, $FAILED failed"

cleanup

if [ $FAILED -gt 0 ]; then
    exit 1
fi
exit 0
