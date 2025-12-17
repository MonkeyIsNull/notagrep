# DFA Optimization Research

This document contains research notes and links for DFA optimization techniques used in notagrep.

## Sheng DFA (SIMD State Transitions)

The Sheng algorithm uses SIMD shuffle instructions for O(1) state transitions. Originally from Intel Hyperscan.

### Source References

- **Hyperscan sheng_impl.h**: https://github.com/intel/hyperscan/blob/master/src/nfa/sheng_impl.h
- **Hyperscan sheng.c**: https://github.com/intel/hyperscan/blob/master/src/nfa/sheng.c

### Algorithm

Sheng DFA uses 256 shuffle masks (one per input byte):
```c
masks[byte][state] = next_state
```

For each input byte, a SIMD shuffle (pshufb on x86, tbl on ARM) performs the state transition in O(1).

Memory: 256 * 16 = 4KB per DFA (limited to 16 states).

### Implementation Notes

- Implemented in `src/regex/sheng.c`
- Currently disabled due to poor cache performance on grep workloads
- Sheng is ~24% slower than regular DFA due to 4KB cache footprint
- May be useful for patterns with very small DFAs (<8 states) where masks fit in L1 cache

## State Acceleration (memchr Skip)

Ripgrep's approach to accelerating DFA execution for patterns with self-looping states (like `.*`).

### Source References

- **regex-automata accel.rs**: https://github.com/BurntSushi/regex-automata/blob/master/src/dfa/accel.rs
- **Regex crate internals blog**: https://blog.burntsushi.net/regex-internals/

### Algorithm

For states where most transitions loop back to the same state (e.g., the `.*` state in `func.*return`):

1. At DFA construction, identify states with <= 3 "escape bytes" (bytes that transition to a different state)
2. During execution, use memchr to skip ahead to the next escape byte
3. This turns O(n) byte-by-byte scanning into O(n/k) where k is the density of escape bytes

### Implementation Notes

- Acceleration info stored in `DfaStateAccel` struct per state
- Used in `dfa_contains` and `dfa_find_first` hot loops
- Works well for patterns like `.*X` where X is rare

## Single-Pass DFA Execution

Traditional grep tries to match from every position (O(n*m)). A single-pass approach resets to the start state on dead state, achieving O(n).

### Implementation

In `dfa_contains` and `dfa_find_first`:
1. Run DFA from start state
2. On dead state, reset to start state and continue
3. On match state, record match and continue for longest match

This is correct for leftmost-first matching semantics used in grep.

## Future Optimizations

### Reverse DFA for Start Position

Ripgrep uses a reverse DFA to efficiently find match start positions for unanchored patterns. Not yet implemented.

### Lazy DFA

Build DFA states on-demand during execution to avoid state explosion. Useful for complex patterns that would explode a full DFA.

### Teddy-style Prefilter Integration

Use SIMD prefilter to find candidate positions, then verify with DFA. Already implemented for literal patterns.
