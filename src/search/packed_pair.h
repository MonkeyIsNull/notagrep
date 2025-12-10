#ifndef NOTAGREP_PACKED_PAIR_H
#define NOTAGREP_PACKED_PAIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Packed Pair: SIMD-accelerated literal string search using rare byte pairs
//
// Algorithm overview (based on memchr crate's approach):
// 1. Select two statistically rare bytes from the pattern using frequency heuristics
// 2. Use SIMD to scan for the first rare byte (processes 64 bytes per iteration)
// 3. When found, check if the second rare byte is at the expected offset
// 4. If both match, verify full pattern with memcmp
//
// This is faster than Teddy on ARM64 because:
// - Uses simple vceqq_u8 (byte compare) instead of vqtbl1q_u8 (table lookup)
// - Single aligned load per 16 bytes instead of overlapping loads
// - vmaxvq_u8 for quick zero-check avoids expensive movemask extraction

typedef struct {
    // Two rare bytes selected from pattern
    uint8_t rare1;
    uint8_t rare2;

    // Positions of rare bytes in pattern
    size_t offset1;
    size_t offset2;

    // Pattern data
    const uint8_t *pattern;
    size_t pattern_len;

    // Case insensitivity
    bool case_insensitive;
} PackedPair;

// Initialize PackedPair with a pattern
// Selects two rare bytes from the pattern for efficient filtering
// Returns 0 on success, -1 on error
int packed_pair_init(PackedPair *pp, const uint8_t *pattern, size_t len, bool case_insensitive);

// Free PackedPair resources (currently no-op since pattern is borrowed)
void packed_pair_free(PackedPair *pp);

// Check if PackedPair SIMD is available on this platform
bool packed_pair_available(void);

// Match callback type
typedef void (*packed_pair_match_cb)(size_t pos, void *ctx);

// Search for pattern in haystack using Packed Pair SIMD
// Calls callback for each match position
// Returns number of matches found, or -1 on error
int packed_pair_search(const PackedPair *pp, const uint8_t *haystack, size_t haystack_len,
                       packed_pair_match_cb cb, void *ctx);

// Find first occurrence of pattern
// Returns position of match, or -1 if not found
ssize_t packed_pair_find_first(const PackedPair *pp, const uint8_t *haystack, size_t haystack_len);

// Check if pattern exists in haystack
bool packed_pair_contains(const PackedPair *pp, const uint8_t *haystack, size_t haystack_len);

#endif // NOTAGREP_PACKED_PAIR_H
