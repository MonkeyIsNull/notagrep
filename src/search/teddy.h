#ifndef NOTAGREP_TEDDY_H
#define NOTAGREP_TEDDY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Teddy: SIMD-accelerated literal string search using nybble-based fingerprinting
//
// Algorithm overview:
// 1. Split each byte into low/high nybbles (4-bit halves)
// 2. Build lookup tables mapping nybble values to pattern bitsets
// 3. Use pshufb (x86) or vtbl (ARM) for 16 parallel lookups
// 4. AND results from low and high nybbles to find candidates
// 5. Verify candidates with full pattern comparison
//
// For single-pattern search, we use a 2-3 byte fingerprint from the first
// bytes of the pattern. This reduces false positives:
// - 2-byte: ~1/65536
// - 3-byte: ~1/16777216 (used for patterns >= 3 bytes)

typedef struct {
    // Nybble lookup masks for byte 0 (16 bytes each, aligned for SIMD)
    uint8_t mask0_lo[16] __attribute__((aligned(16)));
    uint8_t mask0_hi[16] __attribute__((aligned(16)));

    // Nybble lookup masks for byte 1 (16 bytes each, aligned for SIMD)
    uint8_t mask1_lo[16] __attribute__((aligned(16)));
    uint8_t mask1_hi[16] __attribute__((aligned(16)));

    // Nybble lookup masks for byte 2 (16 bytes each, aligned for SIMD)
    // Used for patterns >= 3 bytes for better filtering
    uint8_t mask2_lo[16] __attribute__((aligned(16)));
    uint8_t mask2_hi[16] __attribute__((aligned(16)));

    // Pattern data
    const uint8_t *pattern;
    size_t pattern_len;

    // Number of fingerprint bytes (2 or 3)
    int fingerprint_len;

    // Case insensitivity
    bool case_insensitive;
} Teddy;

// Initialize Teddy with a pattern
// Returns 0 on success, -1 on error
int teddy_init(Teddy *t, const uint8_t *pattern, size_t len, bool case_insensitive);

// Free Teddy resources (currently no-op since pattern is borrowed)
void teddy_free(Teddy *t);

// Check if Teddy SIMD is available on this platform
bool teddy_available(void);

// Match callback type
typedef void (*teddy_match_cb)(size_t pos, void *ctx);

// Search for pattern in haystack using Teddy SIMD
// Calls callback for each match position
// Returns number of matches found, or -1 on error
int teddy_search(const Teddy *t, const uint8_t *haystack, size_t haystack_len,
                 teddy_match_cb cb, void *ctx);

// Find first occurrence of pattern
// Returns position of match, or -1 if not found
ssize_t teddy_find_first(const Teddy *t, const uint8_t *haystack, size_t haystack_len);

// Check if pattern exists in haystack
bool teddy_contains(const Teddy *t, const uint8_t *haystack, size_t haystack_len);

#endif // NOTAGREP_TEDDY_H
