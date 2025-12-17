#ifndef NOTAGREP_TEDDY_MULTI_H
#define NOTAGREP_TEDDY_MULTI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// TeddyMulti: SIMD-accelerated multi-literal string search
//
// This is an extension of single-pattern Teddy that can search for up to 8
// patterns simultaneously using a bucket-based approach:
//
// 1. Each pattern is assigned to one of 8 buckets (using bit 0-7 in mask tables)
// 2. Patterns are grouped by their 2-byte fingerprint (first 2 bytes)
// 3. SIMD scans for any fingerprint match, then verifies against bucket patterns
//
// For small pattern sets (2-8 patterns), this is much faster than Aho-Corasick
// because there's no pointer chasing - just parallel SIMD lookups and bit ops.

#define TEDDY_MULTI_MAX_PATTERNS 8

typedef struct {
    // Nybble lookup masks for byte 0 (16 bytes each, aligned for SIMD)
    // Each entry is a bitset: bit N set means pattern N has this nybble at position 0
    uint8_t mask0_lo[16] __attribute__((aligned(16)));
    uint8_t mask0_hi[16] __attribute__((aligned(16)));

    // Nybble lookup masks for byte 1
    uint8_t mask1_lo[16] __attribute__((aligned(16)));
    uint8_t mask1_hi[16] __attribute__((aligned(16)));

    // Pattern data - up to 8 patterns (OWNED - must free on cleanup)
    uint8_t *patterns[TEDDY_MULTI_MAX_PATTERNS];
    size_t pattern_lens[TEDDY_MULTI_MAX_PATTERNS];
    size_t pattern_count;

    // Minimum pattern length (for determining search window)
    size_t min_pattern_len;

    // Case insensitivity
    bool case_insensitive;
} TeddyMulti;

// Match result
typedef struct {
    size_t pos;          // Position of match in haystack
    size_t pattern_idx;  // Index of matched pattern (0 to pattern_count-1)
    size_t pattern_len;  // Length of matched pattern
} TeddyMultiMatch;

// Match callback type
typedef void (*teddy_multi_match_cb)(const TeddyMultiMatch *match, void *ctx);

// Initialize TeddyMulti with multiple patterns
// patterns: array of pattern pointers
// lens: array of pattern lengths
// count: number of patterns (1 to TEDDY_MULTI_MAX_PATTERNS)
// Returns 0 on success, -1 on error
int teddy_multi_init(TeddyMulti *t, const uint8_t **patterns, const size_t *lens,
                     size_t count, bool case_insensitive);

// Free TeddyMulti resources
void teddy_multi_free(TeddyMulti *t);

// Check if TeddyMulti SIMD is available on this platform
bool teddy_multi_available(void);

// Search for any pattern in haystack using TeddyMulti SIMD
// Calls callback for each match
// Returns number of matches found, or -1 on error
int teddy_multi_search(const TeddyMulti *t, const uint8_t *haystack, size_t haystack_len,
                       teddy_multi_match_cb cb, void *ctx);

// Find first occurrence of any pattern
// Returns position of match in *pos, pattern index in *pattern_idx
// Returns true if match found, false otherwise
bool teddy_multi_find_first(const TeddyMulti *t, const uint8_t *haystack, size_t haystack_len,
                            size_t *pos, size_t *pattern_idx);

// Check if any pattern exists in haystack
bool teddy_multi_contains(const TeddyMulti *t, const uint8_t *haystack, size_t haystack_len);

// Count lines containing any pattern (for -c mode)
// Deduplicates multiple matches on the same line
size_t teddy_multi_count_lines(const TeddyMulti *t, const uint8_t *haystack, size_t haystack_len);

#endif // NOTAGREP_TEDDY_MULTI_H
