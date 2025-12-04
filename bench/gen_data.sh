#!/bin/bash
# Generate synthetic test data for benchmarks

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_DIR="$SCRIPT_DIR/data"

mkdir -p "$DATA_DIR"

# Word list for generating realistic text
WORDS=(
    "the" "be" "to" "of" "and" "a" "in" "that" "have" "I"
    "it" "for" "not" "on" "with" "he" "as" "you" "do" "at"
    "this" "but" "his" "by" "from" "they" "we" "say" "her" "she"
    "or" "an" "will" "my" "one" "all" "would" "there" "their" "what"
    "so" "up" "out" "if" "about" "who" "get" "which" "go" "me"
    "function" "return" "static" "const" "struct" "void" "int" "char"
    "include" "define" "ifdef" "endif" "typedef" "enum" "union"
    "error" "warning" "fatal" "debug" "info" "trace" "log"
    "file" "buffer" "size" "count" "index" "data" "value" "result"
)

# Generate a random line with optional special patterns
gen_line() {
    local len=$((RANDOM % 15 + 5))  # 5-20 words
    local line=""

    for ((i=0; i<len; i++)); do
        local word="${WORDS[$((RANDOM % ${#WORDS[@]}))]}"

        # Occasionally add patterns that match our test regexes
        if ((RANDOM % 50 == 0)); then
            word="func_$((RANDOM % 1000))"
        elif ((RANDOM % 100 == 0)); then
            word="error"
        elif ((RANDOM % 100 == 0)); then
            word="warning"
        elif ((RANDOM % 200 == 0)); then
            word="fatal"
        fi

        if [ -n "$line" ]; then
            line="$line $word"
        else
            line="$word"
        fi
    done

    echo "$line"
}

# Generate file of approximate size
gen_file() {
    local target_size=$1
    local output=$2
    local current_size=0

    echo "Generating $output (target: $((target_size / 1024))KB)..."

    > "$output"  # Clear file

    while [ $current_size -lt $target_size ]; do
        # Generate a batch of lines for efficiency
        for ((j=0; j<100; j++)); do
            gen_line >> "$output"
        done
        current_size=$(wc -c < "$output")
    done

    actual_size=$(wc -c < "$output")
    echo "  Created: $((actual_size / 1024))KB"
}

echo "Generating benchmark data..."
echo

# Small: 100KB
gen_file 102400 "$DATA_DIR/small.txt"

# Medium: 10MB
gen_file 10485760 "$DATA_DIR/medium.txt"

# Large: 100MB
gen_file 104857600 "$DATA_DIR/large.txt"

echo
echo "Done! Files created in $DATA_DIR"
ls -lh "$DATA_DIR"
