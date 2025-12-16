#include "prefilter.h"
#include "ahocorasick.h"
#include "teddy.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif


static inline uint8_t to_lower(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static inline uint8_t to_upper(uint8_t c) {
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static void build_skip_table(Prefilter *pf) {
    // Initialize all entries to needle length
    for (int i = 0; i < 256; i++) {
        pf->skip_table[i] = pf->needle_len;
    }

    // Set skip distances for characters in the needle
    // For BMH, we process ALL characters including the last one
    // Last character gets skip=0 to trigger match verification
    for (size_t i = 0; i < pf->needle_len; i++) {
        size_t skip = pf->needle_len - 1 - i;
        if (pf->case_insensitive) {
            pf->skip_table[to_lower(pf->needle[i])] = skip;
            pf->skip_table[to_upper(pf->needle[i])] = skip;
        } else {
            pf->skip_table[pf->needle[i]] = skip;
        }
    }
}

// Byte frequency table for English text (lower = rarer)
// Based on typical frequency analysis, normalized to 0-255 range
static const uint8_t byte_frequency[256] = {
    // Control chars (0-31): rare
    255, 255, 255, 255, 255, 255, 255, 255, 255, 100, 50, 255, 255, 100, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    // Space and punctuation (32-47)
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
    // Digits (48-57): moderately common in code
    90, 90, 90, 90, 90, 90, 90, 90, 90, 90,
    // Punctuation (58-64)
    100,  // :
    100,  // ;
    150,  // <
    80,   // =
    150,  // >
    200,  // ?
    230,  // @
    // Uppercase (65-90): less common than lowercase
    60, 70, 70, 70, 30, 80, 80, 70, 50, 200, 200, 70, 70,
    70, 50, 70, 230, 60, 50, 40, 80, 180, 150, 200, 150, 230,
    // Punctuation (91-96)
    150, 150, 150, 230, 120, 200,
    // Lowercase (97-122): common
    20, 80, 60, 60, 10, 80, 70, 50, 30, 200, 180, 50, 60,
    40, 30, 60, 230, 40, 30, 20, 60, 150, 120, 180, 120, 220,
    // Punctuation (123-127)
    150, 180, 150, 230, 255,
    // High bytes (128-255): rare in ASCII text
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

// Build set of bytes that never appear in the needle
// Also find the rarest byte in the needle for optimized search
static void build_non_matching_set(Prefilter *pf) {
    // Start with all bytes as non-matching
    memset(pf->non_matching, 1, 256);

    // Mark bytes that appear in the needle as matching
    for (size_t i = 0; i < pf->needle_len; i++) {
        if (pf->case_insensitive) {
            pf->non_matching[to_lower(pf->needle[i])] = 0;
            pf->non_matching[to_upper(pf->needle[i])] = 0;
        } else {
            pf->non_matching[pf->needle[i]] = 0;
        }
    }

    // Count non-matching bytes
    pf->non_matching_count = 0;
    for (int i = 0; i < 256; i++) {
        if (pf->non_matching[i]) {
            pf->non_matching_count++;
        }
    }

    // Find the rarest byte in the needle
    // This byte will be searched for first using memchr, then we verify around it
    pf->rare_byte = pf->needle[0];
    pf->rare_byte_offset = 0;
    pf->rare_byte_score = 0;
    uint8_t max_rarity = 0;  // Higher = rarer

    for (size_t i = 0; i < pf->needle_len; i++) {
        uint8_t b = pf->needle[i];
        uint8_t rarity;

        if (pf->case_insensitive) {
            // For case-insensitive, take the less rare of the two cases
            uint8_t lower_rarity = byte_frequency[to_lower(b)];
            uint8_t upper_rarity = byte_frequency[to_upper(b)];
            rarity = (lower_rarity < upper_rarity) ? lower_rarity : upper_rarity;
        } else {
            rarity = byte_frequency[b];
        }

        if (rarity > max_rarity) {
            max_rarity = rarity;
            pf->rare_byte = b;
            pf->rare_byte_offset = i;
        }
    }
    pf->rare_byte_score = max_rarity;
}

int prefilter_init(Prefilter *pf, const uint8_t *needle, size_t len, bool case_insensitive) {
    memset(pf, 0, sizeof(*pf));

    if (len == 0) {
        return -1;  // Empty needle not supported
    }

    // Copy needle
    pf->needle = malloc(len);
    if (!pf->needle) {
        return -1;
    }
    memcpy(pf->needle, needle, len);
    pf->needle_len = len;
    pf->case_insensitive = case_insensitive;

    // Store first and last bytes
    pf->first_byte = needle[0];
    pf->last_byte = needle[len - 1];

    if (case_insensitive) {
        pf->first_byte_lower = to_lower(pf->first_byte);
        pf->first_byte_upper = to_upper(pf->first_byte);
    }

    // Build BMH skip table
    build_skip_table(pf);

    // Build non-matching byte set for fast SIMD skip
    build_non_matching_set(pf);

    // Compute guard character and md2 (git-grep optimization)
    // Guard char is second-to-last byte - checked before full verification
    // md2 is minimum skip after partial match failure
    if (len >= 2) {
        pf->guard_char = case_insensitive ? to_lower(needle[len - 2]) : needle[len - 2];

        // md2 = minimum distance from second-to-last position to any occurrence
        // of the guard character elsewhere in the pattern (closer to the end)
        pf->md2 = len;
        for (size_t i = 0; i < len - 2; i++) {
            uint8_t c = case_insensitive ? to_lower(needle[i]) : needle[i];
            if (c == pf->guard_char) {
                size_t dist = len - 2 - i;
                if (dist < pf->md2) {
                    pf->md2 = dist;
                }
            }
        }
        if (pf->md2 == 0) pf->md2 = 1;  // Minimum skip of 1
    } else {
        pf->guard_char = needle[0];
        pf->md2 = 1;
    }

    // Initialize SIMD searchers (patterns >= 2 bytes)
    // Use pf->needle (copied data) not needle (input parameter) since
    // the input may be freed after this function returns
    pf->teddy_valid = false;
    pf->packed_pair_valid = false;

#if defined(__aarch64__) || defined(_M_ARM64)
    // On ARM64, use Packed Pair (faster than Teddy due to simpler SIMD ops)
    if (len >= SIMD_MIN_NEEDLE_LEN && packed_pair_available()) {
        if (packed_pair_init(&pf->packed_pair, pf->needle, len, case_insensitive) == 0) {
            pf->packed_pair_valid = true;
        }
    }
    pf->use_simd = pf->packed_pair_valid;
#else
    // On x86-64, use Teddy (pshufb is very fast)
    if (len >= SIMD_MIN_NEEDLE_LEN && teddy_available()) {
        if (teddy_init(&pf->teddy, pf->needle, len, case_insensitive) == 0) {
            pf->teddy_valid = true;
        }
    }
    pf->use_simd = pf->teddy_valid;
#endif

    return 0;
}

void prefilter_free(Prefilter *pf) {
    if (pf->needle) {
        free(pf->needle);
        pf->needle = NULL;
    }
    if (pf->teddy_valid) {
        teddy_free(&pf->teddy);
        pf->teddy_valid = false;
    }
    if (pf->packed_pair_valid) {
        packed_pair_free(&pf->packed_pair);
        pf->packed_pair_valid = false;
    }
}

// Case-insensitive memcmp
static inline int memcmp_ci(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t ca = to_lower(a[i]);
        uint8_t cb = to_lower(b[i]);
        if (ca != cb) return ca - cb;
    }
    return 0;
}

// Verify match at position (case-sensitive or insensitive)
// Uses Raita algorithm order: last, first, middle, then full comparison
// This statistically rejects mismatches faster than sequential comparison
static inline bool verify_match(const Prefilter *pf, const uint8_t *pos) {
    const size_t len = pf->needle_len;

    if (pf->case_insensitive) {
        // Raita order for case-insensitive
        if (to_lower(pos[len - 1]) != to_lower(pf->needle[len - 1])) return false;
        if (to_lower(pos[0]) != to_lower(pf->needle[0])) return false;
        if (len > 2) {
            size_t mid = len / 2;
            if (to_lower(pos[mid]) != to_lower(pf->needle[mid])) return false;
        }
        return memcmp_ci(pos, pf->needle, len) == 0;
    } else {
        // Raita order for case-sensitive: last, first, middle, then full
        if (pos[len - 1] != pf->needle[len - 1]) return false;
        if (pos[0] != pf->needle[0]) return false;
        if (len > 2) {
            size_t mid = len / 2;
            if (pos[mid] != pf->needle[mid]) return false;
        }
        return memcmp(pos, pf->needle, len) == 0;
    }
}

// =============================================================================
// Scalar (fallback) implementation - Unrolled Boyer-Moore-Horspool
// Based on git-grep's kwset.c bmexec() for maximum performance
// =============================================================================

// Fast path for single-byte patterns using libc memchr
static int search_single_byte(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len,
                              prefilter_match_cb cb, void *ctx) {
    int count = 0;
    const uint8_t *p = haystack;
    const uint8_t *end = haystack + haystack_len;
    uint8_t target = pf->needle[0];

    if (pf->case_insensitive) {
        // Case-insensitive: need to check both cases
        uint8_t lower = to_lower(target);
        uint8_t upper = to_upper(target);
        while (p < end) {
            // Search for lower case first
            const uint8_t *found = memchr(p, lower, end - p);
            const uint8_t *found_upper = memchr(p, upper, end - p);

            // Take the earlier match
            if (found_upper && (!found || found_upper < found)) {
                found = found_upper;
            }

            if (!found) break;

            count++;
            if (cb) cb(found - haystack, ctx);
            p = found + 1;
        }
    } else {
        // Case-sensitive: use memchr directly
        while (p < end) {
            const uint8_t *found = memchr(p, target, end - p);
            if (!found) break;

            count++;
            if (cb) cb(found - haystack, ctx);
            p = found + 1;
        }
    }

    return count;
}

// Rare-byte-first search: use memchr to find the rarest byte, then verify around it
// This is faster than BMH when the rare byte is truly rare in the haystack
static int search_rare_byte_first(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len,
                                   prefilter_match_cb cb, void *ctx) {
    int count = 0;
    const uint8_t *p = haystack + pf->rare_byte_offset;
    const uint8_t *end = haystack + haystack_len - (pf->needle_len - pf->rare_byte_offset - 1);

    while (p < end) {
        // Find next occurrence of the rare byte
        const uint8_t *found = memchr(p, pf->rare_byte, end - p);
        if (!found) break;

        // Calculate where the needle would start
        const uint8_t *needle_start = found - pf->rare_byte_offset;
        if (needle_start >= haystack && needle_start + pf->needle_len <= haystack + haystack_len) {
            if (verify_match(pf, needle_start)) {
                count++;
                if (cb) cb(needle_start - haystack, ctx);
                p = found + pf->needle_len - pf->rare_byte_offset;
                continue;
            }
        }
        p = found + 1;
    }

    return count;
}

// =============================================================================
// SIMD 4-byte prefix search (ARM64 NEON)
// Checks first 4 bytes of pattern across 16 positions in parallel
// =============================================================================
#if defined(__aarch64__) || defined(_M_ARM64)

// Convert NEON comparison result to bitmask using vshrn_n_u16
// Each match byte (0xFF) becomes a set bit in the result
static inline uint64_t neon_movemask(uint8x16_t v) {
    // Reinterpret as 16-bit elements and shift right to extract high bits
    uint16x8_t v16 = vreinterpretq_u16_u8(v);
    uint8x8_t narrowed = vshrn_n_u16(v16, 4);
    // Extract as 64-bit value - matches are spaced 4 bits apart
    return vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
}

// 4-byte prefix SIMD search
static int search_simd_prefix(const Prefilter *pf, const uint8_t *haystack,
                              size_t haystack_len, prefilter_match_cb cb, void *ctx) {
    if (pf->needle_len < 4) {
        return search_rare_byte_first(pf, haystack, haystack_len, cb, ctx);
    }

    int count = 0;
    const uint8_t *p = haystack;
    const uint8_t *end = haystack + haystack_len - pf->needle_len + 1;

    const uint8x16_t b0 = vdupq_n_u8(pf->needle[0]);
    const uint8x16_t b1 = vdupq_n_u8(pf->needle[1]);
    const uint8x16_t b2 = vdupq_n_u8(pf->needle[2]);
    const uint8x16_t b3 = vdupq_n_u8(pf->needle[3]);

    // Main SIMD loop
    while (p + 16 + 3 <= end) {
        uint8x16_t hay0 = vld1q_u8(p);
        uint8x16_t hay1 = vld1q_u8(p + 1);
        uint8x16_t hay2 = vld1q_u8(p + 2);
        uint8x16_t hay3 = vld1q_u8(p + 3);

        uint8x16_t match = vandq_u8(
            vandq_u8(vceqq_u8(hay0, b0), vceqq_u8(hay1, b1)),
            vandq_u8(vceqq_u8(hay2, b2), vceqq_u8(hay3, b3)));

        // Quick check if any prefix matches before extracting mask
        if (vmaxvq_u8(match) != 0xFF) {
            p += 16;
            continue;
        }

        uint64_t mask = neon_movemask(match);
        while (mask) {
            int idx = __builtin_ctzll(mask) >> 2;
            if (p + idx < end && memcmp(p + idx, pf->needle, pf->needle_len) == 0) {
                count++;
                if (cb) cb((p + idx) - haystack, ctx);
            }
            mask &= ~(0xFULL << (idx << 2));
        }
        p += 16;
    }

    // Scalar tail
    uint32_t prefix;
    memcpy(&prefix, pf->needle, 4);
    while (p < end) {
        uint32_t hay_prefix;
        memcpy(&hay_prefix, p, 4);
        if (hay_prefix == prefix && memcmp(p, pf->needle, pf->needle_len) == 0) {
            count++;
            if (cb) cb(p - haystack, ctx);
        }
        p++;
    }

    return count;
}

#endif // __aarch64__

// Unrolled Boyer-Moore-Horspool with 10-iteration skip loop
// This eliminates loop overhead and maximizes CPU pipeline efficiency
static int search_scalar_bmh(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len,
                             prefilter_match_cb cb, void *ctx) {
    if (haystack_len < pf->needle_len) {
        return 0;
    }

    // Special case: single-byte patterns use memchr
    if (pf->needle_len == 1) {
        return search_single_byte(pf, haystack, haystack_len, cb, ctx);
    }

    // Use SIMD 4-byte prefix search for large files on ARM64
    // This is faster than rare-byte-first for patterns >= 4 bytes on large data
#if defined(__aarch64__) || defined(_M_ARM64)
    if (pf->needle_len >= 4 && !pf->case_insensitive && haystack_len >= 64 * 1024) {
        return search_simd_prefix(pf, haystack, haystack_len, cb, ctx);
    }
#endif

    // Use rare-byte-first search for patterns >= 3 chars (case-sensitive only)
    // This uses memchr to find the rarest byte in the pattern, then verifies
    if (pf->needle_len >= 3 && !pf->case_insensitive) {
        return search_rare_byte_first(pf, haystack, haystack_len, cb, ctx);
    }

    int count = 0;
    const size_t *delta = pf->skip_table;
    const size_t len = pf->needle_len;

    // Use precomputed guard character and md2 from git-grep optimization
    const uint8_t gc = pf->guard_char;
    const size_t md2 = pf->md2;

    // tp points to where the last byte of needle would be
    const uint8_t *tp = haystack + len - 1;
    const uint8_t *ep = haystack + haystack_len;

    // Unrolled skip loop - process 10 positions at a time when we have enough room
    // This is the key optimization from git-grep's kwset.c
    if (haystack_len > 12 * len) {
        const uint8_t *fast_end = ep - 11 * len;

        while (tp <= fast_end) {
            size_t d;

            // 10 unrolled iterations with 3 early-exit checks
            d = delta[(size_t)tp[0]]; tp += d;
            d = delta[(size_t)tp[0]]; tp += d;
            if (d == 0) goto found;
            d = delta[(size_t)tp[0]]; tp += d;
            d = delta[(size_t)tp[0]]; tp += d;
            d = delta[(size_t)tp[0]]; tp += d;
            if (d == 0) goto found;
            d = delta[(size_t)tp[0]]; tp += d;
            d = delta[(size_t)tp[0]]; tp += d;
            d = delta[(size_t)tp[0]]; tp += d;
            if (d == 0) goto found;
            d = delta[(size_t)tp[0]]; tp += d;
            d = delta[(size_t)tp[0]]; tp += d;
            continue;

        found:
            // Last byte matched - check guard character (second-to-last)
            {
                uint8_t c2 = pf->case_insensitive ? to_lower(tp[-1]) : tp[-1];
                if (c2 == gc) {
                    // Both last two bytes match - verify full pattern
                    const uint8_t *start = tp - (len - 1);
                    if (verify_match(pf, start)) {
                        count++;
                        if (cb) cb(start - haystack, ctx);
                        tp += len;  // Skip past match
                        continue;
                    }
                }
            }
            // Not a full match - use md2 for smart skip (git-grep style)
            tp += md2;
        }
    }

    // Handle remaining bytes with careful bounds checking
    while (tp < ep) {
        size_t d = delta[(size_t)*tp];
        if (d == 0) {
            // Last byte matched - check guard and verify
            uint8_t c2 = pf->case_insensitive ? to_lower(tp[-1]) : tp[-1];
            if (c2 == gc) {
                const uint8_t *start = tp - (len - 1);
                if (start >= haystack && verify_match(pf, start)) {
                    count++;
                    if (cb) cb(start - haystack, ctx);
                    tp += len;
                    continue;
                }
            }
            d = md2;  // Use md2 instead of 1
        }
        tp += d;
    }

    return count;
}

// =============================================================================
// Public API
// Note: SIMD search uses Packed Pair on ARM64, Teddy on x86-64
// =============================================================================

int prefilter_search(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len,
                     prefilter_match_cb cb, void *ctx) {
    if (!pf || !pf->needle || !haystack) {
        return -1;
    }

    // Single-byte patterns: use memchr (highly SIMD-optimized in libc)
    if (pf->needle_len == 1) {
        return search_single_byte(pf, haystack, haystack_len, cb, ctx);
    }

    // Use Teddy on x86-64 (pshufb is fast there)
    // On ARM64, fall through to scalar BMH which uses rare-byte-first search
#if !defined(__aarch64__) && !defined(_M_ARM64)
    if (pf->teddy_valid) {
        return teddy_search(&pf->teddy, haystack, haystack_len, cb, ctx);
    }
#endif

    // Fallback to BMH (handles case-insensitive short patterns, etc.)
    return search_scalar_bmh(pf, haystack, haystack_len, cb, ctx);
}

ssize_t prefilter_find_first(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len) {
    if (!pf || !pf->needle || !haystack || haystack_len < pf->needle_len) {
        return -1;
    }

    // Special case: single-byte patterns use memchr
    if (pf->needle_len == 1) {
        if (pf->case_insensitive) {
            uint8_t lower = to_lower(pf->needle[0]);
            uint8_t upper = to_upper(pf->needle[0]);
            const uint8_t *found_l = memchr(haystack, lower, haystack_len);
            const uint8_t *found_u = memchr(haystack, upper, haystack_len);
            if (found_u && (!found_l || found_u < found_l)) found_l = found_u;
            return found_l ? (ssize_t)(found_l - haystack) : -1;
        } else {
            const uint8_t *found = memchr(haystack, pf->needle[0], haystack_len);
            return found ? (ssize_t)(found - haystack) : -1;
        }
    }

    // Use platform-specific SIMD search (patterns >= 2 bytes)
#if defined(__aarch64__) || defined(_M_ARM64)
    if (pf->packed_pair_valid) {
        return packed_pair_find_first(&pf->packed_pair, haystack, haystack_len);
    }
#else
    if (pf->teddy_valid) {
        return teddy_find_first(&pf->teddy, haystack, haystack_len);
    }
#endif

    // Fallback: Use rare-byte-first for case-sensitive patterns >= 3 chars
    if (pf->needle_len >= 3 && !pf->case_insensitive) {
        const uint8_t *p = haystack + pf->rare_byte_offset;
        const uint8_t *end = haystack + haystack_len - (pf->needle_len - pf->rare_byte_offset - 1);

        while (p < end) {
            const uint8_t *found = memchr(p, pf->rare_byte, end - p);
            if (!found) break;

            const uint8_t *needle_start = found - pf->rare_byte_offset;
            if (needle_start >= haystack && needle_start + pf->needle_len <= haystack + haystack_len) {
                if (verify_match(pf, needle_start)) {
                    return (ssize_t)(needle_start - haystack);
                }
            }
            p = found + 1;
        }
        return -1;
    }

    const size_t *delta = pf->skip_table;
    const size_t len = pf->needle_len;
    const uint8_t *needle = pf->needle;

    // Guard character: second-to-last byte
    const uint8_t gc = pf->case_insensitive ? to_lower(needle[len - 2]) : needle[len - 2];

    const uint8_t *tp = haystack + len - 1;
    const uint8_t *ep = haystack + haystack_len;

    // Unrolled skip loop for larger haystacks
    if (haystack_len > 12 * len) {
        const uint8_t *fast_end = ep - 11 * len;

        while (tp <= fast_end) {
            size_t d;

            d = delta[(size_t)tp[0]]; tp += d;
            d = delta[(size_t)tp[0]]; tp += d;
            if (d == 0) goto check_match;
            d = delta[(size_t)tp[0]]; tp += d;
            d = delta[(size_t)tp[0]]; tp += d;
            d = delta[(size_t)tp[0]]; tp += d;
            if (d == 0) goto check_match;
            d = delta[(size_t)tp[0]]; tp += d;
            d = delta[(size_t)tp[0]]; tp += d;
            d = delta[(size_t)tp[0]]; tp += d;
            if (d == 0) goto check_match;
            d = delta[(size_t)tp[0]]; tp += d;
            d = delta[(size_t)tp[0]]; tp += d;
            continue;

        check_match:
            {
                uint8_t c2 = pf->case_insensitive ? to_lower(tp[-1]) : tp[-1];
                if (c2 == gc) {
                    const uint8_t *start = tp - (len - 1);
                    if (verify_match(pf, start)) {
                        return (ssize_t)(start - haystack);
                    }
                }
            }
            tp++;
        }
    }

    // Handle remaining bytes
    while (tp < ep) {
        size_t d = delta[(size_t)*tp];
        if (d == 0) {
            uint8_t c2 = pf->case_insensitive ? to_lower(tp[-1]) : tp[-1];
            if (c2 == gc) {
                const uint8_t *start = tp - (len - 1);
                if (start >= haystack && verify_match(pf, start)) {
                    return (ssize_t)(start - haystack);
                }
            }
            d = 1;
        }
        tp += d;
    }

    return -1;
}

bool prefilter_contains(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len) {
    return prefilter_find_first(pf, haystack, haystack_len) >= 0;
}

// =============================================================================
// Multi-literal prefilter
// =============================================================================

int multi_prefilter_init(MultiPrefilter *mpf,
                         uint8_t **needles, size_t *lens, size_t count,
                         bool case_insensitive) {
    memset(mpf, 0, sizeof(*mpf));

    if (count == 0 || count > MULTI_PREFILTER_MAX) {
        return -1;
    }

    mpf->case_insensitive = case_insensitive;
    mpf->count = count;
    mpf->use_ac = false;
    mpf->ac = NULL;
    mpf->pattern_lens = NULL;

    // For single pattern, just use the regular prefilter (BMH is faster)
    if (count == 1) {
        if (prefilter_init(&mpf->filters[0], needles[0], lens[0], case_insensitive) < 0) {
            return -1;
        }
        return 0;
    }

    // For multiple patterns, use Aho-Corasick for O(n) matching
    mpf->ac = ac_create(case_insensitive);
    if (!mpf->ac) {
        return -1;
    }

    // Store pattern lengths for match reporting
    mpf->pattern_lens = malloc(count * sizeof(size_t));
    if (!mpf->pattern_lens) {
        ac_free(mpf->ac);
        mpf->ac = NULL;
        return -1;
    }

    // Add all patterns to the automaton
    for (size_t i = 0; i < count; i++) {
        mpf->pattern_lens[i] = lens[i];
        if (ac_add_pattern(mpf->ac, needles[i], lens[i], (int)i) < 0) {
            ac_free(mpf->ac);
            free(mpf->pattern_lens);
            mpf->ac = NULL;
            mpf->pattern_lens = NULL;
            return -1;
        }
    }

    // Compile the automaton (build failure links)
    if (ac_compile(mpf->ac) < 0) {
        ac_free(mpf->ac);
        free(mpf->pattern_lens);
        mpf->ac = NULL;
        mpf->pattern_lens = NULL;
        return -1;
    }

    mpf->use_ac = true;
    return 0;
}

void multi_prefilter_free(MultiPrefilter *mpf) {
    if (mpf->use_ac) {
        // Free Aho-Corasick automaton
        if (mpf->ac) {
            ac_free(mpf->ac);
            mpf->ac = NULL;
        }
        if (mpf->pattern_lens) {
            free(mpf->pattern_lens);
            mpf->pattern_lens = NULL;
        }
    } else {
        // Free individual prefilters (single pattern case)
        for (size_t i = 0; i < mpf->count; i++) {
            prefilter_free(&mpf->filters[i]);
        }
    }
    mpf->count = 0;
    mpf->use_ac = false;
}

// Context for collecting matches from Aho-Corasick
typedef struct {
    multi_match_cb user_cb;
    void *user_data;
    int count;
} ACSearchCtx;

// Callback adapter from AC format to MultiMatch format
static void ac_match_adapter(size_t pos, size_t len, int pattern_id, void *ctx) {
    ACSearchCtx *actx = (ACSearchCtx *)ctx;
    MultiMatch m = {
        .pos = pos,
        .len = len,
        .which = (size_t)pattern_id
    };
    if (actx->user_cb) {
        actx->user_cb(&m, actx->user_data);
    }
    actx->count++;
}

// Context for single-pattern fallback
typedef struct {
    multi_match_cb user_cb;
    void *user_data;
    size_t which;
    size_t needle_len;
    int count;
} SingleSearchCtx;

static void single_match_wrapper(size_t pos, void *ctx) {
    SingleSearchCtx *sctx = (SingleSearchCtx *)ctx;
    MultiMatch m = {
        .pos = pos,
        .len = sctx->needle_len,
        .which = sctx->which
    };
    if (sctx->user_cb) {
        sctx->user_cb(&m, sctx->user_data);
    }
    sctx->count++;
}

int multi_prefilter_search(const MultiPrefilter *mpf,
                           const uint8_t *haystack, size_t haystack_len,
                           multi_match_cb cb, void *ctx) {
    if (!mpf || mpf->count == 0) {
        return -1;
    }

    if (mpf->use_ac) {
        // Use Aho-Corasick for O(n) multi-pattern matching
        ACSearchCtx actx = {
            .user_cb = cb,
            .user_data = ctx,
            .count = 0
        };
        ac_search(mpf->ac, haystack, haystack_len, ac_match_adapter, &actx);
        return actx.count;
    } else {
        // Single pattern: use regular prefilter (faster for single pattern)
        SingleSearchCtx sctx = {
            .user_cb = cb,
            .user_data = ctx,
            .which = 0,
            .needle_len = mpf->filters[0].needle_len,
            .count = 0
        };
        prefilter_search(&mpf->filters[0], haystack, haystack_len,
                        single_match_wrapper, &sctx);
        return sctx.count;
    }
}

bool multi_prefilter_contains(const MultiPrefilter *mpf,
                              const uint8_t *haystack, size_t haystack_len) {
    if (!mpf || mpf->count == 0) {
        return false;
    }

    if (mpf->use_ac) {
        // Use Aho-Corasick for O(n) check
        return ac_contains(mpf->ac, haystack, haystack_len);
    } else {
        // Single pattern: use regular prefilter
        return prefilter_contains(&mpf->filters[0], haystack, haystack_len);
    }
}

// =============================================================================
// Single-byte scanner
// =============================================================================

void byte_scan_all(uint8_t byte, const uint8_t *haystack, size_t len,
                   byte_match_cb cb, void *ctx) {
    const uint8_t *p = haystack;
    const uint8_t *end = haystack + len;

    while (p < end) {
        const uint8_t *found = memchr(p, byte, end - p);
        if (!found) break;

        cb(found - haystack, ctx);
        p = found + 1;
    }
}

ssize_t byte_scan_first(uint8_t byte, const uint8_t *haystack, size_t len) {
    const uint8_t *found = memchr(haystack, byte, len);
    if (found) {
        return found - haystack;
    }
    return -1;
}
