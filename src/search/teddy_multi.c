#include "teddy_multi.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Platform detection
#if defined(__aarch64__) || defined(_M_ARM64)
    #define TEDDY_MULTI_ARM64 1
    #include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64)
    #define TEDDY_MULTI_X86_64 1
    #include <immintrin.h>
    #include <tmmintrin.h>
#endif

// Helper: convert to lowercase
static inline uint8_t to_lower(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

// Helper to set mask bits for a byte with pattern bit
static inline void set_mask_bits_multi(uint8_t *mask_lo, uint8_t *mask_hi,
                                       uint8_t byte, uint8_t pattern_bit,
                                       bool case_insensitive) {
    if (case_insensitive) {
        uint8_t lo = to_lower(byte);
        uint8_t hi_byte = (lo >= 'a' && lo <= 'z') ? lo - 32 : lo;

        mask_lo[lo & 0x0F] |= pattern_bit;
        mask_hi[lo >> 4] |= pattern_bit;

        if (lo >= 'a' && lo <= 'z') {
            mask_lo[hi_byte & 0x0F] |= pattern_bit;
            mask_hi[hi_byte >> 4] |= pattern_bit;
        }
    } else {
        mask_lo[byte & 0x0F] |= pattern_bit;
        mask_hi[byte >> 4] |= pattern_bit;
    }
}

int teddy_multi_init(TeddyMulti *t, const uint8_t **patterns, const size_t *lens,
                     size_t count, bool case_insensitive) {
    if (!t || !patterns || !lens || count == 0 || count > TEDDY_MULTI_MAX_PATTERNS) {
        return -1;
    }

    // Check all patterns are at least 2 bytes
    size_t min_len = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (!patterns[i] || lens[i] < 2) {
            return -1;
        }
        if (lens[i] < min_len) {
            min_len = lens[i];
        }
    }

    memset(t, 0, sizeof(*t));
    t->pattern_count = count;
    t->min_pattern_len = min_len;
    t->case_insensitive = case_insensitive;

    // Copy pattern data (we take ownership)
    for (size_t i = 0; i < count; i++) {
        t->patterns[i] = malloc(lens[i]);
        if (!t->patterns[i]) {
            // Cleanup on failure
            for (size_t j = 0; j < i; j++) {
                free(t->patterns[j]);
            }
            return -1;
        }
        memcpy(t->patterns[i], patterns[i], lens[i]);
        t->pattern_lens[i] = lens[i];
    }

    // Build masks - each pattern gets a bit (1 << i)
    for (size_t i = 0; i < count; i++) {
        uint8_t pattern_bit = (uint8_t)(1 << i);

        // Set bits for byte 0
        set_mask_bits_multi(t->mask0_lo, t->mask0_hi,
                           patterns[i][0], pattern_bit, case_insensitive);

        // Set bits for byte 1
        set_mask_bits_multi(t->mask1_lo, t->mask1_hi,
                           patterns[i][1], pattern_bit, case_insensitive);
    }

    return 0;
}

void teddy_multi_free(TeddyMulti *t) {
    if (!t) return;

    // Free owned pattern memory
    for (size_t i = 0; i < t->pattern_count; i++) {
        free(t->patterns[i]);
        t->patterns[i] = NULL;
    }
    t->pattern_count = 0;
}

bool teddy_multi_available(void) {
#if defined(TEDDY_MULTI_ARM64) || defined(TEDDY_MULTI_X86_64)
    return true;
#else
    return false;
#endif
}

// =============================================================================
// Verification helpers
// =============================================================================

static inline bool verify_pattern(const uint8_t *hay, const uint8_t *needle,
                                  size_t len, bool case_insensitive) {
    if (case_insensitive) {
        for (size_t i = 0; i < len; i++) {
            if (to_lower(hay[i]) != to_lower(needle[i])) {
                return false;
            }
        }
        return true;
    }

    // Case-sensitive: use 8-byte chunks for speed
    size_t i = 0;
    while (i + 8 <= len) {
        uint64_t h8, n8;
        memcpy(&h8, hay + i, 8);
        memcpy(&n8, needle + i, 8);
        if (h8 != n8) return false;
        i += 8;
    }
    while (i < len) {
        if (hay[i] != needle[i]) return false;
        i++;
    }
    return true;
}

// Try to verify a candidate position against all patterns in the bucket mask
// Returns the pattern index if matched, or -1 if no match
static inline int verify_candidate(const TeddyMulti *t, const uint8_t *hay,
                                   size_t haystack_len, size_t pos, uint8_t bucket_mask) {
    // Check each pattern that's in this bucket
    for (size_t p = 0; p < t->pattern_count; p++) {
        if (!(bucket_mask & (1 << p))) {
            continue;  // Pattern not in this bucket
        }

        size_t plen = t->pattern_lens[p];
        if (pos + plen > haystack_len) {
            continue;  // Would exceed haystack
        }

        if (verify_pattern(hay + pos, t->patterns[p], plen, t->case_insensitive)) {
            return (int)p;
        }
    }
    return -1;
}

// =============================================================================
// ARM64 NEON Implementation
// =============================================================================
#ifdef TEDDY_MULTI_ARM64

static int teddy_multi_search_neon(const TeddyMulti *t, const uint8_t *hay, size_t len,
                                   teddy_multi_match_cb cb, void *ctx) {
    if (len < t->min_pattern_len) {
        return 0;
    }

    int matches = 0;
    const size_t search_len = len - t->min_pattern_len + 1;

    // Load masks into NEON registers
    uint8x16_t mask0_lo = vld1q_u8(t->mask0_lo);
    uint8x16_t mask0_hi = vld1q_u8(t->mask0_hi);
    uint8x16_t mask1_lo = vld1q_u8(t->mask1_lo);
    uint8x16_t mask1_hi = vld1q_u8(t->mask1_hi);
    uint8x16_t nybble_mask = vdupq_n_u8(0x0F);

    size_t i = 0;

    // Process 16 bytes at a time
    while (i + 16 <= search_len) {
        // Load chunks for fingerprint bytes 0 and 1
        uint8x16_t chunk0 = vld1q_u8(hay + i);
        uint8x16_t chunk1 = vld1q_u8(hay + i + 1);

        // Split byte 0 into nybbles
        uint8x16_t lo0 = vandq_u8(chunk0, nybble_mask);
        uint8x16_t hi0 = vshrq_n_u8(chunk0, 4);

        // Split byte 1 into nybbles
        uint8x16_t lo1 = vandq_u8(chunk1, nybble_mask);
        uint8x16_t hi1 = vshrq_n_u8(chunk1, 4);

        // Parallel table lookup
        uint8x16_t res0_lo = vqtbl1q_u8(mask0_lo, lo0);
        uint8x16_t res0_hi = vqtbl1q_u8(mask0_hi, hi0);
        uint8x16_t res1_lo = vqtbl1q_u8(mask1_lo, lo1);
        uint8x16_t res1_hi = vqtbl1q_u8(mask1_hi, hi1);

        // Combine: byte 0 candidates AND byte 1 candidates
        uint8x16_t cand0 = vandq_u8(res0_lo, res0_hi);
        uint8x16_t cand1 = vandq_u8(res1_lo, res1_hi);
        uint8x16_t candidates = vandq_u8(cand0, cand1);

        // Extract candidate positions
        // Store to memory and iterate (ARM doesn't have great movemask)
        uint8_t cand_bytes[16];
        vst1q_u8(cand_bytes, candidates);

        for (int j = 0; j < 16; j++) {
            uint8_t bucket_mask = cand_bytes[j];
            if (bucket_mask == 0) continue;

            size_t pos = i + j;
            int pat_idx = verify_candidate(t, hay, len, pos, bucket_mask);
            if (pat_idx >= 0) {
                if (cb) {
                    TeddyMultiMatch m = {
                        .pos = pos,
                        .pattern_idx = (size_t)pat_idx,
                        .pattern_len = t->pattern_lens[pat_idx]
                    };
                    cb(&m, ctx);
                }
                matches++;
            }
        }

        i += 16;
    }

    // Scalar tail
    while (i < search_len) {
        // Check each pattern at this position
        for (size_t p = 0; p < t->pattern_count; p++) {
            size_t plen = t->pattern_lens[p];
            if (i + plen > len) continue;

            if (verify_pattern(hay + i, t->patterns[p], plen, t->case_insensitive)) {
                if (cb) {
                    TeddyMultiMatch m = {
                        .pos = i,
                        .pattern_idx = p,
                        .pattern_len = plen
                    };
                    cb(&m, ctx);
                }
                matches++;
                break;  // Only count first match at this position
            }
        }
        i++;
    }

    return matches;
}

static bool teddy_multi_find_first_neon(const TeddyMulti *t, const uint8_t *hay, size_t len,
                                        size_t *out_pos, size_t *out_pattern_idx) {
    if (len < t->min_pattern_len) {
        return false;
    }

    const size_t search_len = len - t->min_pattern_len + 1;

    uint8x16_t mask0_lo = vld1q_u8(t->mask0_lo);
    uint8x16_t mask0_hi = vld1q_u8(t->mask0_hi);
    uint8x16_t mask1_lo = vld1q_u8(t->mask1_lo);
    uint8x16_t mask1_hi = vld1q_u8(t->mask1_hi);
    uint8x16_t nybble_mask = vdupq_n_u8(0x0F);

    size_t i = 0;

    while (i + 16 <= search_len) {
        uint8x16_t chunk0 = vld1q_u8(hay + i);
        uint8x16_t chunk1 = vld1q_u8(hay + i + 1);

        uint8x16_t lo0 = vandq_u8(chunk0, nybble_mask);
        uint8x16_t hi0 = vshrq_n_u8(chunk0, 4);
        uint8x16_t lo1 = vandq_u8(chunk1, nybble_mask);
        uint8x16_t hi1 = vshrq_n_u8(chunk1, 4);

        uint8x16_t res0_lo = vqtbl1q_u8(mask0_lo, lo0);
        uint8x16_t res0_hi = vqtbl1q_u8(mask0_hi, hi0);
        uint8x16_t res1_lo = vqtbl1q_u8(mask1_lo, lo1);
        uint8x16_t res1_hi = vqtbl1q_u8(mask1_hi, hi1);

        uint8x16_t cand0 = vandq_u8(res0_lo, res0_hi);
        uint8x16_t cand1 = vandq_u8(res1_lo, res1_hi);
        uint8x16_t candidates = vandq_u8(cand0, cand1);

        // Quick check if any candidates
        uint64x2_t as_u64 = vreinterpretq_u64_u8(candidates);
        uint64_t lo64 = vgetq_lane_u64(as_u64, 0);
        uint64_t hi64 = vgetq_lane_u64(as_u64, 1);

        if (lo64 | hi64) {
            uint8_t cand_bytes[16];
            vst1q_u8(cand_bytes, candidates);

            for (int j = 0; j < 16; j++) {
                uint8_t bucket_mask = cand_bytes[j];
                if (bucket_mask == 0) continue;

                size_t pos = i + j;
                int pat_idx = verify_candidate(t, hay, len, pos, bucket_mask);
                if (pat_idx >= 0) {
                    if (out_pos) *out_pos = pos;
                    if (out_pattern_idx) *out_pattern_idx = (size_t)pat_idx;
                    return true;
                }
            }
        }

        i += 16;
    }

    // Scalar tail
    while (i < search_len) {
        for (size_t p = 0; p < t->pattern_count; p++) {
            size_t plen = t->pattern_lens[p];
            if (i + plen > len) continue;

            if (verify_pattern(hay + i, t->patterns[p], plen, t->case_insensitive)) {
                if (out_pos) *out_pos = i;
                if (out_pattern_idx) *out_pattern_idx = p;
                return true;
            }
        }
        i++;
    }

    return false;
}

static size_t teddy_multi_count_lines_neon(const TeddyMulti *t, const uint8_t *hay, size_t len) {
    if (len < t->min_pattern_len) {
        return 0;
    }

    size_t lines = 0;
    size_t last_line_end = SIZE_MAX;
    const size_t search_len = len - t->min_pattern_len + 1;

    uint8x16_t mask0_lo = vld1q_u8(t->mask0_lo);
    uint8x16_t mask0_hi = vld1q_u8(t->mask0_hi);
    uint8x16_t mask1_lo = vld1q_u8(t->mask1_lo);
    uint8x16_t mask1_hi = vld1q_u8(t->mask1_hi);
    uint8x16_t nybble_mask = vdupq_n_u8(0x0F);

    size_t i = 0;

    while (i + 16 <= search_len) {
        uint8x16_t chunk0 = vld1q_u8(hay + i);
        uint8x16_t chunk1 = vld1q_u8(hay + i + 1);

        uint8x16_t lo0 = vandq_u8(chunk0, nybble_mask);
        uint8x16_t hi0 = vshrq_n_u8(chunk0, 4);
        uint8x16_t lo1 = vandq_u8(chunk1, nybble_mask);
        uint8x16_t hi1 = vshrq_n_u8(chunk1, 4);

        uint8x16_t res0_lo = vqtbl1q_u8(mask0_lo, lo0);
        uint8x16_t res0_hi = vqtbl1q_u8(mask0_hi, hi0);
        uint8x16_t res1_lo = vqtbl1q_u8(mask1_lo, lo1);
        uint8x16_t res1_hi = vqtbl1q_u8(mask1_hi, hi1);

        uint8x16_t cand0 = vandq_u8(res0_lo, res0_hi);
        uint8x16_t cand1 = vandq_u8(res1_lo, res1_hi);
        uint8x16_t candidates = vandq_u8(cand0, cand1);

        // Quick check if any candidates
        uint64x2_t as_u64 = vreinterpretq_u64_u8(candidates);
        uint64_t lo64 = vgetq_lane_u64(as_u64, 0);
        uint64_t hi64 = vgetq_lane_u64(as_u64, 1);

        if (lo64 | hi64) {
            uint8_t cand_bytes[16];
            vst1q_u8(cand_bytes, candidates);

            for (int j = 0; j < 16; j++) {
                uint8_t bucket_mask = cand_bytes[j];
                if (bucket_mask == 0) continue;

                size_t pos = i + j;

                // Skip if already on a counted line
                if (last_line_end != SIZE_MAX && pos <= last_line_end) {
                    continue;
                }

                int pat_idx = verify_candidate(t, hay, len, pos, bucket_mask);
                if (pat_idx >= 0) {
                    lines++;
                    // Find end of this line
                    const uint8_t *nl = memchr(hay + pos, '\n', len - pos);
                    last_line_end = nl ? (size_t)(nl - hay) : len;
                }
            }
        }

        i += 16;
    }

    // Scalar tail
    while (i < search_len) {
        // Skip if already on a counted line
        if (last_line_end != SIZE_MAX && i <= last_line_end) {
            i++;
            continue;
        }

        for (size_t p = 0; p < t->pattern_count; p++) {
            size_t plen = t->pattern_lens[p];
            if (i + plen > len) continue;

            if (verify_pattern(hay + i, t->patterns[p], plen, t->case_insensitive)) {
                lines++;
                const uint8_t *nl = memchr(hay + i, '\n', len - i);
                last_line_end = nl ? (size_t)(nl - hay) : len;
                break;
            }
        }
        i++;
    }

    return lines;
}

#endif // TEDDY_MULTI_ARM64

// =============================================================================
// x86-64 SSSE3 Implementation
// =============================================================================
#ifdef TEDDY_MULTI_X86_64

static int teddy_multi_search_ssse3(const TeddyMulti *t, const uint8_t *hay, size_t len,
                                    teddy_multi_match_cb cb, void *ctx) {
    if (len < t->min_pattern_len) {
        return 0;
    }

    int matches = 0;
    const size_t search_len = len - t->min_pattern_len + 1;

    __m128i mask0_lo = _mm_load_si128((__m128i*)t->mask0_lo);
    __m128i mask0_hi = _mm_load_si128((__m128i*)t->mask0_hi);
    __m128i mask1_lo = _mm_load_si128((__m128i*)t->mask1_lo);
    __m128i mask1_hi = _mm_load_si128((__m128i*)t->mask1_hi);
    __m128i nybble_mask = _mm_set1_epi8(0x0F);

    size_t i = 0;

    while (i + 16 <= search_len) {
        __m128i chunk0 = _mm_loadu_si128((__m128i*)(hay + i));
        __m128i chunk1 = _mm_loadu_si128((__m128i*)(hay + i + 1));

        __m128i lo0 = _mm_and_si128(chunk0, nybble_mask);
        __m128i hi0 = _mm_and_si128(_mm_srli_epi16(chunk0, 4), nybble_mask);
        __m128i lo1 = _mm_and_si128(chunk1, nybble_mask);
        __m128i hi1 = _mm_and_si128(_mm_srli_epi16(chunk1, 4), nybble_mask);

        __m128i res0_lo = _mm_shuffle_epi8(mask0_lo, lo0);
        __m128i res0_hi = _mm_shuffle_epi8(mask0_hi, hi0);
        __m128i res1_lo = _mm_shuffle_epi8(mask1_lo, lo1);
        __m128i res1_hi = _mm_shuffle_epi8(mask1_hi, hi1);

        __m128i cand0 = _mm_and_si128(res0_lo, res0_hi);
        __m128i cand1 = _mm_and_si128(res1_lo, res1_hi);
        __m128i candidates = _mm_and_si128(cand0, cand1);

        // Store and iterate through candidates
        uint8_t cand_bytes[16];
        _mm_storeu_si128((__m128i*)cand_bytes, candidates);

        for (int j = 0; j < 16; j++) {
            uint8_t bucket_mask = cand_bytes[j];
            if (bucket_mask == 0) continue;

            size_t pos = i + j;
            int pat_idx = verify_candidate(t, hay, len, pos, bucket_mask);
            if (pat_idx >= 0) {
                if (cb) {
                    TeddyMultiMatch m = {
                        .pos = pos,
                        .pattern_idx = (size_t)pat_idx,
                        .pattern_len = t->pattern_lens[pat_idx]
                    };
                    cb(&m, ctx);
                }
                matches++;
            }
        }

        i += 16;
    }

    // Scalar tail
    while (i < search_len) {
        for (size_t p = 0; p < t->pattern_count; p++) {
            size_t plen = t->pattern_lens[p];
            if (i + plen > len) continue;

            if (verify_pattern(hay + i, t->patterns[p], plen, t->case_insensitive)) {
                if (cb) {
                    TeddyMultiMatch m = {
                        .pos = i,
                        .pattern_idx = p,
                        .pattern_len = plen
                    };
                    cb(&m, ctx);
                }
                matches++;
                break;
            }
        }
        i++;
    }

    return matches;
}

static bool teddy_multi_find_first_ssse3(const TeddyMulti *t, const uint8_t *hay, size_t len,
                                         size_t *out_pos, size_t *out_pattern_idx) {
    if (len < t->min_pattern_len) {
        return false;
    }

    const size_t search_len = len - t->min_pattern_len + 1;

    __m128i mask0_lo = _mm_load_si128((__m128i*)t->mask0_lo);
    __m128i mask0_hi = _mm_load_si128((__m128i*)t->mask0_hi);
    __m128i mask1_lo = _mm_load_si128((__m128i*)t->mask1_lo);
    __m128i mask1_hi = _mm_load_si128((__m128i*)t->mask1_hi);
    __m128i nybble_mask = _mm_set1_epi8(0x0F);

    size_t i = 0;

    while (i + 16 <= search_len) {
        __m128i chunk0 = _mm_loadu_si128((__m128i*)(hay + i));
        __m128i chunk1 = _mm_loadu_si128((__m128i*)(hay + i + 1));

        __m128i lo0 = _mm_and_si128(chunk0, nybble_mask);
        __m128i hi0 = _mm_and_si128(_mm_srli_epi16(chunk0, 4), nybble_mask);
        __m128i lo1 = _mm_and_si128(chunk1, nybble_mask);
        __m128i hi1 = _mm_and_si128(_mm_srli_epi16(chunk1, 4), nybble_mask);

        __m128i res0_lo = _mm_shuffle_epi8(mask0_lo, lo0);
        __m128i res0_hi = _mm_shuffle_epi8(mask0_hi, hi0);
        __m128i res1_lo = _mm_shuffle_epi8(mask1_lo, lo1);
        __m128i res1_hi = _mm_shuffle_epi8(mask1_hi, hi1);

        __m128i cand0 = _mm_and_si128(res0_lo, res0_hi);
        __m128i cand1 = _mm_and_si128(res1_lo, res1_hi);
        __m128i candidates = _mm_and_si128(cand0, cand1);

        // Quick check
        if (!_mm_testz_si128(candidates, candidates)) {
            uint8_t cand_bytes[16];
            _mm_storeu_si128((__m128i*)cand_bytes, candidates);

            for (int j = 0; j < 16; j++) {
                uint8_t bucket_mask = cand_bytes[j];
                if (bucket_mask == 0) continue;

                size_t pos = i + j;
                int pat_idx = verify_candidate(t, hay, len, pos, bucket_mask);
                if (pat_idx >= 0) {
                    if (out_pos) *out_pos = pos;
                    if (out_pattern_idx) *out_pattern_idx = (size_t)pat_idx;
                    return true;
                }
            }
        }

        i += 16;
    }

    // Scalar tail
    while (i < search_len) {
        for (size_t p = 0; p < t->pattern_count; p++) {
            size_t plen = t->pattern_lens[p];
            if (i + plen > len) continue;

            if (verify_pattern(hay + i, t->patterns[p], plen, t->case_insensitive)) {
                if (out_pos) *out_pos = i;
                if (out_pattern_idx) *out_pattern_idx = p;
                return true;
            }
        }
        i++;
    }

    return false;
}

static size_t teddy_multi_count_lines_ssse3(const TeddyMulti *t, const uint8_t *hay, size_t len) {
    if (len < t->min_pattern_len) {
        return 0;
    }

    size_t lines = 0;
    size_t last_line_end = SIZE_MAX;
    const size_t search_len = len - t->min_pattern_len + 1;

    __m128i mask0_lo = _mm_load_si128((__m128i*)t->mask0_lo);
    __m128i mask0_hi = _mm_load_si128((__m128i*)t->mask0_hi);
    __m128i mask1_lo = _mm_load_si128((__m128i*)t->mask1_lo);
    __m128i mask1_hi = _mm_load_si128((__m128i*)t->mask1_hi);
    __m128i nybble_mask = _mm_set1_epi8(0x0F);

    size_t i = 0;

    while (i + 16 <= search_len) {
        __m128i chunk0 = _mm_loadu_si128((__m128i*)(hay + i));
        __m128i chunk1 = _mm_loadu_si128((__m128i*)(hay + i + 1));

        __m128i lo0 = _mm_and_si128(chunk0, nybble_mask);
        __m128i hi0 = _mm_and_si128(_mm_srli_epi16(chunk0, 4), nybble_mask);
        __m128i lo1 = _mm_and_si128(chunk1, nybble_mask);
        __m128i hi1 = _mm_and_si128(_mm_srli_epi16(chunk1, 4), nybble_mask);

        __m128i res0_lo = _mm_shuffle_epi8(mask0_lo, lo0);
        __m128i res0_hi = _mm_shuffle_epi8(mask0_hi, hi0);
        __m128i res1_lo = _mm_shuffle_epi8(mask1_lo, lo1);
        __m128i res1_hi = _mm_shuffle_epi8(mask1_hi, hi1);

        __m128i cand0 = _mm_and_si128(res0_lo, res0_hi);
        __m128i cand1 = _mm_and_si128(res1_lo, res1_hi);
        __m128i candidates = _mm_and_si128(cand0, cand1);

        if (!_mm_testz_si128(candidates, candidates)) {
            uint8_t cand_bytes[16];
            _mm_storeu_si128((__m128i*)cand_bytes, candidates);

            for (int j = 0; j < 16; j++) {
                uint8_t bucket_mask = cand_bytes[j];
                if (bucket_mask == 0) continue;

                size_t pos = i + j;

                if (last_line_end != SIZE_MAX && pos <= last_line_end) {
                    continue;
                }

                int pat_idx = verify_candidate(t, hay, len, pos, bucket_mask);
                if (pat_idx >= 0) {
                    lines++;
                    const uint8_t *nl = memchr(hay + pos, '\n', len - pos);
                    last_line_end = nl ? (size_t)(nl - hay) : len;
                }
            }
        }

        i += 16;
    }

    // Scalar tail
    while (i < search_len) {
        if (last_line_end != SIZE_MAX && i <= last_line_end) {
            i++;
            continue;
        }

        for (size_t p = 0; p < t->pattern_count; p++) {
            size_t plen = t->pattern_lens[p];
            if (i + plen > len) continue;

            if (verify_pattern(hay + i, t->patterns[p], plen, t->case_insensitive)) {
                lines++;
                const uint8_t *nl = memchr(hay + i, '\n', len - i);
                last_line_end = nl ? (size_t)(nl - hay) : len;
                break;
            }
        }
        i++;
    }

    return lines;
}

#endif // TEDDY_MULTI_X86_64

// =============================================================================
// Scalar Fallback
// =============================================================================
#if !defined(TEDDY_MULTI_ARM64) && !defined(TEDDY_MULTI_X86_64)

static int teddy_multi_search_scalar(const TeddyMulti *t, const uint8_t *hay, size_t len,
                                     teddy_multi_match_cb cb, void *ctx) {
    if (len < t->min_pattern_len) {
        return 0;
    }

    int matches = 0;
    const size_t search_len = len - t->min_pattern_len + 1;

    for (size_t i = 0; i < search_len; i++) {
        for (size_t p = 0; p < t->pattern_count; p++) {
            size_t plen = t->pattern_lens[p];
            if (i + plen > len) continue;

            if (verify_pattern(hay + i, t->patterns[p], plen, t->case_insensitive)) {
                if (cb) {
                    TeddyMultiMatch m = {
                        .pos = i,
                        .pattern_idx = p,
                        .pattern_len = plen
                    };
                    cb(&m, ctx);
                }
                matches++;
                break;
            }
        }
    }

    return matches;
}

static bool teddy_multi_find_first_scalar(const TeddyMulti *t, const uint8_t *hay, size_t len,
                                          size_t *out_pos, size_t *out_pattern_idx) {
    if (len < t->min_pattern_len) {
        return false;
    }

    const size_t search_len = len - t->min_pattern_len + 1;

    for (size_t i = 0; i < search_len; i++) {
        for (size_t p = 0; p < t->pattern_count; p++) {
            size_t plen = t->pattern_lens[p];
            if (i + plen > len) continue;

            if (verify_pattern(hay + i, t->patterns[p], plen, t->case_insensitive)) {
                if (out_pos) *out_pos = i;
                if (out_pattern_idx) *out_pattern_idx = p;
                return true;
            }
        }
    }

    return false;
}

static size_t teddy_multi_count_lines_scalar(const TeddyMulti *t, const uint8_t *hay, size_t len) {
    if (len < t->min_pattern_len) {
        return 0;
    }

    size_t lines = 0;
    size_t last_line_end = SIZE_MAX;
    const size_t search_len = len - t->min_pattern_len + 1;

    for (size_t i = 0; i < search_len; i++) {
        if (last_line_end != SIZE_MAX && i <= last_line_end) {
            continue;
        }

        for (size_t p = 0; p < t->pattern_count; p++) {
            size_t plen = t->pattern_lens[p];
            if (i + plen > len) continue;

            if (verify_pattern(hay + i, t->patterns[p], plen, t->case_insensitive)) {
                lines++;
                const uint8_t *nl = memchr(hay + i, '\n', len - i);
                last_line_end = nl ? (size_t)(nl - hay) : len;
                break;
            }
        }
    }

    return lines;
}

#endif // Scalar fallback

// =============================================================================
// Public API
// =============================================================================

int teddy_multi_search(const TeddyMulti *t, const uint8_t *haystack, size_t haystack_len,
                       teddy_multi_match_cb cb, void *ctx) {
    if (!t || !haystack) {
        return -1;
    }

#ifdef TEDDY_MULTI_ARM64
    return teddy_multi_search_neon(t, haystack, haystack_len, cb, ctx);
#elif defined(TEDDY_MULTI_X86_64)
    return teddy_multi_search_ssse3(t, haystack, haystack_len, cb, ctx);
#else
    return teddy_multi_search_scalar(t, haystack, haystack_len, cb, ctx);
#endif
}

bool teddy_multi_find_first(const TeddyMulti *t, const uint8_t *haystack, size_t haystack_len,
                            size_t *pos, size_t *pattern_idx) {
    if (!t || !haystack) {
        return false;
    }

#ifdef TEDDY_MULTI_ARM64
    return teddy_multi_find_first_neon(t, haystack, haystack_len, pos, pattern_idx);
#elif defined(TEDDY_MULTI_X86_64)
    return teddy_multi_find_first_ssse3(t, haystack, haystack_len, pos, pattern_idx);
#else
    return teddy_multi_find_first_scalar(t, haystack, haystack_len, pos, pattern_idx);
#endif
}

bool teddy_multi_contains(const TeddyMulti *t, const uint8_t *haystack, size_t haystack_len) {
    return teddy_multi_find_first(t, haystack, haystack_len, NULL, NULL);
}

size_t teddy_multi_count_lines(const TeddyMulti *t, const uint8_t *haystack, size_t haystack_len) {
    if (!t || !haystack) {
        return 0;
    }

#ifdef TEDDY_MULTI_ARM64
    return teddy_multi_count_lines_neon(t, haystack, haystack_len);
#elif defined(TEDDY_MULTI_X86_64)
    return teddy_multi_count_lines_ssse3(t, haystack, haystack_len);
#else
    return teddy_multi_count_lines_scalar(t, haystack, haystack_len);
#endif
}
