# notagrep

<img src="cat.jpg" alt="log" width="80%" />

> "Никакого кофе в мире не хватит, чтобы ускорить grep"

A fast grep alternative written in C, optimized for speed through algorithmic efficiency and SIMD vectorization.

## Warning
This is alpha freaking ware and not ready for real usage.... yet

## Installation

```bash
make
```

## Quickstart

```bash
# Basic literal string search
notagrep "hello" file.txt

# Fixed string search (no regex)
notagrep -F "hello world" file.txt

# Regex search (default)
notagrep "error|warning" file.txt

# Case insensitive search
notagrep -i "TODO" .

# Count matches
notagrep -c "function" src/

# List files with matches
notagrep -l "import" .
```

## Regex Support

### Supported Features
- **Alternation**: `a|b|c`
- **Character classes**: `[a-z]`, `[^0-9]`
- **Quantifiers**: `*`, `+`, `?`
- **Anchors**: `^` (start of line), `$` (end of line)
- **Dot**: `.` (any character)
- **Grouping**: `(...)`

### NOT Supported
- Backreferences (`\1`, `\2`, etc.)
- Lookahead/lookbehind (`(?=...)`, `(?<=...)`)
- Named groups (`(?P<name>...)`)
- Unicode character classes (`\p{...}`)

## Key Optimizations

- SIMD vectorization (NEON on ARM64, AVX2 on x86-64)
- Aho-Corasick automaton for multi-pattern matching
- Boyer-Moore-Horspool with unrolled skip loop
- Inner literal extraction for regex prefiltering
- Rare byte frequency optimization
- Parallel file processing with thread pool

## Algorithm References

Clean-room implementations based on:

- **Aho-Corasick**: "Efficient String Matching: An Aid to Bibliographic Search" (1975) by Alfred Aho and Margaret Corasick
- **Boyer-Moore**: "A Fast String Searching Algorithm" (1977) by Robert Boyer and J Strother Moore
- **Commentz-Walter hybrid**: combines Aho-Corasick trie with Boyer-Moore skipping

## License

See the source files for licensing information.
