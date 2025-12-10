#include "packed_pair.h"
#include <string.h>
#include <ctype.h>

// Platform detection
#if defined(__aarch64__) || defined(_M_ARM64)
    #define PACKED_PAIR_ARM64 1
    #include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64)
    #define PACKED_PAIR_X86_64 1
    #include <immintrin.h>
#endif

// Byte frequency table: higher value = rarer byte (better for prefilter)
// Based on English text frequency analysis
static const uint8_t BYTE_FREQ[256] = {
    // 0x00-0x0F: Control characters (very rare in text)
    255, 255, 255, 255, 255, 255, 255, 255, 255, 100, 50, 255, 255, 100, 255, 255,
    // 0x10-0x1F: More control characters
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    // 0x20-0x2F: Space and punctuation
    10,   // space - very common
    200,  // !
    150,  // "
    230,  // #
    230,  // $
    230,  // %
    180,  // &
    150,  // '
    120,  // (
    120,  // )
    180,  // *
    180,  // +
    100,  // ,
    120,  // -
    80,   // .
    150,  // /
    // 0x30-0x39: Digits
    90, 90, 90, 90, 90, 90, 90, 90, 90, 90,
    // 0x3A-0x40: Punctuation
    100,  // :
    100,  // ;
    150,  // <
    80,   // =
    150,  // >
    200,  // ?
    230,  // @
    // 0x41-0x5A: Uppercase letters (less common than lowercase)
    60, 70, 70, 70, 30, 80, 80, 70, 50, 200, 200, 70, 70,
    70, 50, 70, 230, 60, 50, 40, 80, 180, 150, 200, 150, 230,
    // 0x5B-0x60: Punctuation
    150, 150, 150, 230, 120, 200,
    // 0x61-0x7A: Lowercase letters (common)
    20, 80, 60, 60, 10, 80, 70, 50, 30, 200, 180, 50, 60,
    40, 30, 60, 230, 40, 30, 20, 60, 150, 120, 180, 120, 220,
    // 0x7B-0x7F: Punctuation and DEL
    150, 180, 150, 230, 255,
    // 0x80-0xFF: High bytes (rare in ASCII text)
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

// Helper: convert to lowercase
static inline uint8_t to_lower(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

// Select the two rarest bytes from the pattern
// For case-insensitive, we use the less-rare of upper/lower variants
static void select_rare_bytes(PackedPair *pp, const uint8_t *pattern, size_t len, bool case_insensitive) {
    size_t best1_idx = 0;
    size_t best2_idx = len > 1 ? 1 : 0;
    uint8_t best1_score = 0;
    uint8_t best2_score = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = pattern[i];
        uint8_t score;

        if (case_insensitive && ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z'))) {
            // For case-insensitive, use the less rare score
            uint8_t lo = to_lower(b);
            uint8_t hi = lo - 32;
            uint8_t lo_score = BYTE_FREQ[lo];
            uint8_t hi_score = BYTE_FREQ[hi];
            score = (lo_score < hi_score) ? lo_score : hi_score;
        } else {
            score = BYTE_FREQ[b];
        }

        // Track top two rarest bytes
        if (score > best1_score) {
            best2_score = best1_score;
            best2_idx = best1_idx;
            best1_score = score;
            best1_idx = i;
        } else if (score > best2_score) {
            best2_score = score;
            best2_idx = i;
        }
    }

    // Ensure offset1 < offset2 for consistent verification
    if (best1_idx < best2_idx) {
        pp->offset1 = best1_idx;
        pp->offset2 = best2_idx;
    } else {
        pp->offset1 = best2_idx;
        pp->offset2 = best1_idx;
    }

    pp->rare1 = pattern[pp->offset1];
    pp->rare2 = pattern[pp->offset2];

    if (case_insensitive) {
        pp->rare1 = to_lower(pp->rare1);
        pp->rare2 = to_lower(pp->rare2);
    }
}

int packed_pair_init(PackedPair *pp, const uint8_t *pattern, size_t len, bool case_insensitive) {
    if (!pp || !pattern || len < 2) {
        return -1;
    }

    memset(pp, 0, sizeof(*pp));
    pp->pattern = pattern;
    pp->pattern_len = len;
    pp->case_insensitive = case_insensitive;

    select_rare_bytes(pp, pattern, len, case_insensitive);

    return 0;
}

void packed_pair_free(PackedPair *pp) {
    (void)pp;
}

bool packed_pair_available(void) {
#if defined(PACKED_PAIR_ARM64) || defined(PACKED_PAIR_X86_64)
    return true;
#else
    return false;
#endif
}

// =============================================================================
// ARM64 NEON Implementation
// =============================================================================
#ifdef PACKED_PAIR_ARM64

// Verify a match at the given position
static inline bool verify_match(const uint8_t *hay, const uint8_t *needle,
                                size_t len, bool case_insensitive) {
    if (case_insensitive) {
        for (size_t i = 0; i < len; i++) {
            if (to_lower(hay[i]) != to_lower(needle[i])) {
                return false;
            }
        }
        return true;
    }
    return memcmp(hay, needle, len) == 0;
}

int packed_pair_search(const PackedPair *pp, const uint8_t *haystack, size_t haystack_len,
                       packed_pair_match_cb cb, void *ctx) {
    if (!pp || !haystack || haystack_len < pp->pattern_len) {
        return 0;
    }

    int matches = 0;

    if (!pp->case_insensitive) {
        // Case-sensitive: use memmem (very fast, uses Two-Way algorithm)
        const uint8_t *p = haystack;
        const uint8_t *end = haystack + haystack_len;

        while (p < end - pp->pattern_len + 1) {
            const uint8_t *found = memmem(p, end - p, pp->pattern, pp->pattern_len);
            if (!found) break;

            if (cb) cb(found - haystack, ctx);
            matches++;
            p = found + 1;
        }
    } else {
        // Case-insensitive: use memchr + verify (memmem doesn't support case-insensitive)
        const uint8_t *p = haystack + pp->offset1;
        const uint8_t *end = haystack + haystack_len - pp->pattern_len + pp->offset1 + 1;

        while (p < end) {
            const uint8_t *found = memchr(p, pp->rare1, end - p);

            // Also check for uppercase variant if rare1 is lowercase letter
            if (pp->rare1 >= 'a' && pp->rare1 <= 'z') {
                uint8_t upper = pp->rare1 - 32;
                const uint8_t *found_upper = memchr(p, upper, end - p);
                if (found_upper && (!found || found_upper < found)) {
                    found = found_upper;
                }
            }

            if (!found) break;

            size_t pattern_start = (found - haystack) - pp->offset1;

            // Check rare2 (case-insensitive)
            uint8_t hay_rare2 = to_lower(haystack[pattern_start + pp->offset2]);
            if (hay_rare2 == pp->rare2) {
                if (verify_match(haystack + pattern_start, pp->pattern,
                                pp->pattern_len, true)) {
                    if (cb) cb(pattern_start, ctx);
                    matches++;
                }
            }

            p = found + 1;
        }
    }

    return matches;
}

ssize_t packed_pair_find_first(const PackedPair *pp, const uint8_t *haystack, size_t haystack_len) {
    if (!pp || !haystack || haystack_len < pp->pattern_len) {
        return -1;
    }

    if (!pp->case_insensitive) {
        // Case-sensitive: use memmem directly
        const uint8_t *found = memmem(haystack, haystack_len, pp->pattern, pp->pattern_len);
        return found ? (ssize_t)(found - haystack) : -1;
    } else {
        // Case-insensitive: use memchr + verify
        const uint8_t *p = haystack + pp->offset1;
        const uint8_t *end = haystack + haystack_len - pp->pattern_len + pp->offset1 + 1;

        while (p < end) {
            const uint8_t *found = memchr(p, pp->rare1, end - p);

            if (pp->rare1 >= 'a' && pp->rare1 <= 'z') {
                uint8_t upper = pp->rare1 - 32;
                const uint8_t *found_upper = memchr(p, upper, end - p);
                if (found_upper && (!found || found_upper < found)) {
                    found = found_upper;
                }
            }

            if (!found) break;

            size_t pattern_start = (found - haystack) - pp->offset1;

            uint8_t hay_rare2 = to_lower(haystack[pattern_start + pp->offset2]);
            if (hay_rare2 == pp->rare2) {
                if (verify_match(haystack + pattern_start, pp->pattern,
                                pp->pattern_len, true)) {
                    return (ssize_t)pattern_start;
                }
            }

            p = found + 1;
        }
    }

    return -1;
}

#endif // PACKED_PAIR_ARM64

// =============================================================================
// x86-64 Implementation (placeholder - use Teddy instead)
// =============================================================================
#ifdef PACKED_PAIR_X86_64

// On x86-64, Teddy with pshufb is faster, so we provide a simple fallback
static inline bool verify_match(const uint8_t *hay, const uint8_t *needle,
                                size_t len, bool case_insensitive) {
    if (case_insensitive) {
        for (size_t i = 0; i < len; i++) {
            if (to_lower(hay[i]) != to_lower(needle[i])) {
                return false;
            }
        }
        return true;
    }
    return memcmp(hay, needle, len) == 0;
}

int packed_pair_search(const PackedPair *pp, const uint8_t *haystack, size_t haystack_len,
                       packed_pair_match_cb cb, void *ctx) {
    if (!pp || !haystack || haystack_len < pp->pattern_len) {
        return 0;
    }

    int matches = 0;
    size_t search_end = haystack_len - pp->pattern_len + 1;

    for (size_t i = 0; i < search_end; i++) {
        if (verify_match(haystack + i, pp->pattern, pp->pattern_len, pp->case_insensitive)) {
            if (cb) cb(i, ctx);
            matches++;
        }
    }

    return matches;
}

ssize_t packed_pair_find_first(const PackedPair *pp, const uint8_t *haystack, size_t haystack_len) {
    if (!pp || !haystack || haystack_len < pp->pattern_len) {
        return -1;
    }

    size_t search_end = haystack_len - pp->pattern_len + 1;

    for (size_t i = 0; i < search_end; i++) {
        if (verify_match(haystack + i, pp->pattern, pp->pattern_len, pp->case_insensitive)) {
            return (ssize_t)i;
        }
    }

    return -1;
}

#endif // PACKED_PAIR_X86_64

// =============================================================================
// Scalar Fallback Implementation
// =============================================================================
#if !defined(PACKED_PAIR_ARM64) && !defined(PACKED_PAIR_X86_64)

static inline bool verify_match(const uint8_t *hay, const uint8_t *needle,
                                size_t len, bool case_insensitive) {
    if (case_insensitive) {
        for (size_t i = 0; i < len; i++) {
            if (to_lower(hay[i]) != to_lower(needle[i])) {
                return false;
            }
        }
        return true;
    }
    return memcmp(hay, needle, len) == 0;
}

int packed_pair_search(const PackedPair *pp, const uint8_t *haystack, size_t haystack_len,
                       packed_pair_match_cb cb, void *ctx) {
    if (!pp || !haystack || haystack_len < pp->pattern_len) {
        return 0;
    }

    int matches = 0;
    size_t search_end = haystack_len - pp->pattern_len + 1;

    for (size_t i = 0; i < search_end; i++) {
        if (verify_match(haystack + i, pp->pattern, pp->pattern_len, pp->case_insensitive)) {
            if (cb) cb(i, ctx);
            matches++;
        }
    }

    return matches;
}

ssize_t packed_pair_find_first(const PackedPair *pp, const uint8_t *haystack, size_t haystack_len) {
    if (!pp || !haystack || haystack_len < pp->pattern_len) {
        return -1;
    }

    size_t search_end = haystack_len - pp->pattern_len + 1;

    for (size_t i = 0; i < search_end; i++) {
        if (verify_match(haystack + i, pp->pattern, pp->pattern_len, pp->case_insensitive)) {
            return (ssize_t)i;
        }
    }

    return -1;
}

#endif // !PACKED_PAIR_ARM64 && !PACKED_PAIR_X86_64

// =============================================================================
// Public API
// =============================================================================

bool packed_pair_contains(const PackedPair *pp, const uint8_t *haystack, size_t haystack_len) {
    return packed_pair_find_first(pp, haystack, haystack_len) >= 0;
}
