# notagrep

![notagrep cat](notagrep_cat.jpg)

> "Никакого кофе в мире не хватит, чтобы ускорить grep"

A high-performance grep implementation written in C, designed to compete with tools like ripgrep.

## Warning
This is alpha freaking ware and not ready for real usage.... yet

## Algorithm References

Clean-room implementations based on these academic papers:

- **Aho-Corasick**: "Efficient String Matching: An Aid to Bibliographic Search" (1975) by Alfred Aho and Margaret Corasick
- **Boyer-Moore**: "A Fast String Searching Algorithm" (1977) by Robert Boyer and J Strother Moore
- **Commentz-Walter hybrid concept**: combines Aho-Corasick trie with Boyer-Moore skipping

## Key Optimizations

- SIMD vectorization (NEON on ARM64, AVX2 on x86-64)
- Aho-Corasick automaton for multi-pattern matching
- Boyer-Moore-Horspool with unrolled skip loop
- Inner literal extraction for regex prefiltering
- Rare byte frequency optimization
- Parallel file processing with thread pool

## Building

```bash
make
```

## Usage

```bash
./notagrep [OPTIONS] PATTERN [PATH...]
```

## Implementation Notes

Key insights from git-grep's kwset.c implementation:

- Git-grep achieves speed through algorithmic efficiency, not SIMD vectorization
- The unrolled skip loop performs 10 delta lookups but only checks for matches 3 times, reducing branch misprediction
- The guard character check examines the second-to-last byte before full verification, rejecting most false positives with a single comparison
- The md2 (minimum delta 2) value enables smart skipping after partial match failures instead of advancing by just one byte
- For single-byte patterns, memchr is used directly as it is highly SIMD-optimized in libc

These techniques can be faster than explicit SIMD for string searching because Boyer-Moore's skip table provides large jumps that outweigh the benefits of vectorized byte-by-byte comparison.

## License

See the source files for licensing information.
