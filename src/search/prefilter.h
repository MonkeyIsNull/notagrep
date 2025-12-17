#ifndef NOTAGREP_PREFILTER_H
#define NOTAGREP_PREFILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "teddy.h"
#include "teddy_multi.h"
#include "packed_pair.h"

// Minimum needle length for SIMD to be beneficial
#define SIMD_MIN_NEEDLE_LEN 2  // Teddy works with 2+ bytes

typedef struct {
    uint8_t *needle;
    size_t needle_len;

    // Boyer-Moore-Horspool bad character table
    size_t skip_table[256];

    // Non-matching byte set: bytes that never appear in the needle
    // Used for fast SIMD skip - if all bytes in a chunk are non-matching,
    // we can skip the entire chunk without checking individual positions
    uint8_t non_matching[256];  // 1 = byte never in needle, 0 = byte in needle
    size_t non_matching_count;  // Number of non-matching bytes (0-256)

    // Rare byte optimization: two rarest bytes in the needle for packed pair search
    // We search for both bytes simultaneously with SIMD, then verify full pattern
    uint8_t rare1;              // Rarest byte value
    size_t rare1_offset;        // Offset of rarest byte in needle
    uint8_t rare2;              // Second rarest byte value
    size_t rare2_offset;        // Offset of second rarest byte in needle

    // Legacy single rare byte (for rare-byte-first search fallback)
    uint8_t rare_byte;          // The rare byte value
    size_t rare_byte_offset;    // Offset of rare byte in needle
    uint8_t rare_byte_score;    // Rarity score (higher = rarer, 0-255)

    // Git-grep style optimization: md2 (minimum delta 2)
    // After a partial match failure, skip by this distance instead of 1
    // This is the minimum distance to the next occurrence of the guard char
    size_t md2;
    uint8_t guard_char;         // Second-to-last byte (for quick rejection)

    // SIMD optimization state
    bool use_simd;
    uint8_t first_byte;    // First byte of needle
    uint8_t last_byte;     // Last byte of needle

    // Case-insensitive variants (for -i flag)
    uint8_t first_byte_upper;
    uint8_t first_byte_lower;
    bool case_insensitive;

    // Teddy SIMD searcher (embedded to avoid allocation overhead)
    Teddy teddy;
    bool teddy_valid;      // True if teddy was successfully initialized

    // Packed Pair SIMD searcher (faster than Teddy on ARM64)
    PackedPair packed_pair;
    bool packed_pair_valid;  // True if packed_pair was successfully initialized
} Prefilter;

// Initialize prefilter with a literal needle
// Takes ownership of needle memory (will be freed on prefilter_free)
// Returns 0 on success, -1 on error
int prefilter_init(Prefilter *pf, const uint8_t *needle, size_t len, bool case_insensitive);

// Free prefilter resources
void prefilter_free(Prefilter *pf);

// Find all occurrences of needle in haystack
// Calls callback for each match position
// Returns number of matches found, or -1 on error
typedef void (*prefilter_match_cb)(size_t pos, void *ctx);
int prefilter_search(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len,
                     prefilter_match_cb cb, void *ctx);

// Find first occurrence of needle in haystack
// Returns position of match, or -1 if not found
ssize_t prefilter_find_first(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len);

// Check if needle exists in haystack (for -l/-q modes)
// Returns true if at least one match exists
bool prefilter_contains(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len);

// Count number of unique lines containing the needle (for -c mode)
// This is more efficient than using prefilter_search with a callback
size_t prefilter_count_lines(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len);

// =============================================================================
// Multi-literal prefilter (for alternation patterns)
// Uses TeddyMulti SIMD for small pattern sets (2-8 patterns)
// Falls back to Aho-Corasick for larger sets or when SIMD unavailable
// =============================================================================

#define MULTI_PREFILTER_MAX 16

// Forward declaration for Aho-Corasick
struct AhoCorasick;

typedef struct {
    // Primary: TeddyMulti SIMD for 2-8 patterns (fastest)
    TeddyMulti teddy_multi;
    bool use_teddy_multi;          // True if using TeddyMulti SIMD

    // Fallback: Aho-Corasick for >8 patterns or case-insensitive
    struct AhoCorasick *ac;        // Aho-Corasick automaton
    bool use_ac;                   // True if using Aho-Corasick

    // Single pattern fallback
    Prefilter filters[MULTI_PREFILTER_MAX];

    // Pattern metadata
    size_t *pattern_lens;          // Pattern lengths for match reporting
    size_t count;
    bool case_insensitive;
} MultiPrefilter;

// Initialize multi-prefilter with multiple literals
// Returns 0 on success, -1 on error
int multi_prefilter_init(MultiPrefilter *mpf,
                         uint8_t **needles, size_t *lens, size_t count,
                         bool case_insensitive);

// Free multi-prefilter resources
void multi_prefilter_free(MultiPrefilter *mpf);

// Match result for multi-prefilter
typedef struct {
    size_t pos;        // Position of match
    size_t len;        // Length of matched literal
    size_t which;      // Index of which literal matched
} MultiMatch;

// Find all occurrences of any needle in haystack
typedef void (*multi_match_cb)(const MultiMatch *match, void *ctx);
int multi_prefilter_search(const MultiPrefilter *mpf,
                           const uint8_t *haystack, size_t haystack_len,
                           multi_match_cb cb, void *ctx);

// Check if any needle exists in haystack
bool multi_prefilter_contains(const MultiPrefilter *mpf,
                              const uint8_t *haystack, size_t haystack_len);

// Count unique lines containing any needle (for -c mode)
size_t multi_prefilter_count_lines(const MultiPrefilter *mpf,
                                    const uint8_t *haystack, size_t haystack_len);

// =============================================================================
// Single-byte scanner (for required byte fallback)
// =============================================================================

// Find all occurrences of a single byte (very fast using memchr)
typedef void (*byte_match_cb)(size_t pos, void *ctx);
void byte_scan_all(uint8_t byte, const uint8_t *haystack, size_t len,
                   byte_match_cb cb, void *ctx);

// Find first occurrence of a byte
ssize_t byte_scan_first(uint8_t byte, const uint8_t *haystack, size_t len);

#endif // NOTAGREP_PREFILTER_H
