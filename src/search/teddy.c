#include "teddy.h"
#include <string.h>
#include <ctype.h>

// Platform detection
#if defined(__aarch64__) || defined(_M_ARM64)
    #define TEDDY_ARM64 1
    #include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64)
    #define TEDDY_X86_64 1
    #include <immintrin.h>
    #include <tmmintrin.h>  // SSSE3 for pshufb
#endif

// Helper: convert to lowercase
static inline uint8_t to_lower(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}



// Helper to set mask bits for a byte (handles case-insensitivity)
static inline void set_mask_bits(uint8_t *mask_lo, uint8_t *mask_hi,
                                  uint8_t byte, bool case_insensitive) {
    if (case_insensitive) {
        uint8_t lo = to_lower(byte);
        uint8_t hi = lo - 32;  // uppercase if alphabetic

        mask_lo[lo & 0x0F] |= 0x01;
        mask_hi[lo >> 4] |= 0x01;

        if (lo >= 'a' && lo <= 'z') {
            mask_lo[hi & 0x0F] |= 0x01;
            mask_hi[hi >> 4] |= 0x01;
        }
    } else {
        mask_lo[byte & 0x0F] = 0x01;
        mask_hi[byte >> 4] = 0x01;
    }
}

int teddy_init(Teddy *t, const uint8_t *pattern, size_t len, bool case_insensitive) {
    if (!t || !pattern || len < 2) {
        return -1;  // Teddy requires at least 2 bytes
    }

    memset(t, 0, sizeof(*t));
    t->pattern = pattern;
    t->pattern_len = len;
    t->case_insensitive = case_insensitive;

    // Build masks for bytes 0 and 1 (always used)
    set_mask_bits(t->mask0_lo, t->mask0_hi, pattern[0], case_insensitive);
    set_mask_bits(t->mask1_lo, t->mask1_hi, pattern[1], case_insensitive);

    // For patterns >= 3 bytes, use 3-byte fingerprint for better filtering
    if (len >= 3) {
        set_mask_bits(t->mask2_lo, t->mask2_hi, pattern[2], case_insensitive);
        t->fingerprint_len = 3;
    } else {
        t->fingerprint_len = 2;
    }

    return 0;
}

void teddy_free(Teddy *t) {
    // Pattern is borrowed, nothing to free
    (void)t;
}

bool teddy_available(void) {
#if defined(TEDDY_ARM64) || defined(TEDDY_X86_64)
    return true;
#else
    return false;
#endif
}

// =============================================================================
// ARM64 NEON Implementation
// =============================================================================
#ifdef TEDDY_ARM64

// Fast verification: check 8 bytes at once, then remainder
static inline bool verify_match_fast(const uint8_t *hay, const uint8_t *needle,
                                     size_t len, bool case_insensitive) {
    if (case_insensitive) {
        // Case-insensitive: still need byte-by-byte
        for (size_t i = 0; i < len; i++) {
            if (to_lower(hay[i]) != to_lower(needle[i])) {
                return false;
            }
        }
        return true;
    }

    // Case-sensitive: use 8-byte chunks
    size_t i = 0;

    // Compare 8 bytes at a time
    while (i + 8 <= len) {
        uint64_t h8, n8;
        memcpy(&h8, hay + i, 8);
        memcpy(&n8, needle + i, 8);
        if (h8 != n8) return false;
        i += 8;
    }

    // Compare remaining bytes
    while (i < len) {
        if (hay[i] != needle[i]) return false;
        i++;
    }

    return true;
}

static int teddy_search_neon(const Teddy *t, const uint8_t *hay, size_t len,
                             teddy_match_cb cb, void *ctx) {
    if (len < t->pattern_len) {
        return 0;
    }

    int matches = 0;
    const size_t search_len = len - t->pattern_len + 1;
    const bool use_3byte = (t->fingerprint_len == 3);

    // Load masks into NEON registers
    uint8x16_t mask0_lo = vld1q_u8(t->mask0_lo);
    uint8x16_t mask0_hi = vld1q_u8(t->mask0_hi);
    uint8x16_t mask1_lo = vld1q_u8(t->mask1_lo);
    uint8x16_t mask1_hi = vld1q_u8(t->mask1_hi);
    uint8x16_t mask2_lo, mask2_hi;
    if (use_3byte) {
        mask2_lo = vld1q_u8(t->mask2_lo);
        mask2_hi = vld1q_u8(t->mask2_hi);
    }
    uint8x16_t nybble_mask = vdupq_n_u8(0x0F);

    size_t i = 0;

    // Process 32 bytes at a time (2x unrolled for better throughput)
    while (i + 32 <= search_len) {
        // First 16 bytes
        uint8x16_t chunk0_a = vld1q_u8(hay + i);
        uint8x16_t chunk1_a = vld1q_u8(hay + i + 1);

        // Second 16 bytes
        uint8x16_t chunk0_b = vld1q_u8(hay + i + 16);
        uint8x16_t chunk1_b = vld1q_u8(hay + i + 17);

        // Process first chunk
        uint8x16_t lo0_a = vandq_u8(chunk0_a, nybble_mask);
        uint8x16_t hi0_a = vshrq_n_u8(chunk0_a, 4);
        uint8x16_t lo1_a = vandq_u8(chunk1_a, nybble_mask);
        uint8x16_t hi1_a = vshrq_n_u8(chunk1_a, 4);

        uint8x16_t res0_lo_a = vqtbl1q_u8(mask0_lo, lo0_a);
        uint8x16_t res0_hi_a = vqtbl1q_u8(mask0_hi, hi0_a);
        uint8x16_t res1_lo_a = vqtbl1q_u8(mask1_lo, lo1_a);
        uint8x16_t res1_hi_a = vqtbl1q_u8(mask1_hi, hi1_a);

        uint8x16_t cand0_a = vandq_u8(res0_lo_a, res0_hi_a);
        uint8x16_t cand1_a = vandq_u8(res1_lo_a, res1_hi_a);
        uint8x16_t candidates_a = vandq_u8(cand0_a, cand1_a);

        // Process second chunk
        uint8x16_t lo0_b = vandq_u8(chunk0_b, nybble_mask);
        uint8x16_t hi0_b = vshrq_n_u8(chunk0_b, 4);
        uint8x16_t lo1_b = vandq_u8(chunk1_b, nybble_mask);
        uint8x16_t hi1_b = vshrq_n_u8(chunk1_b, 4);

        uint8x16_t res0_lo_b = vqtbl1q_u8(mask0_lo, lo0_b);
        uint8x16_t res0_hi_b = vqtbl1q_u8(mask0_hi, hi0_b);
        uint8x16_t res1_lo_b = vqtbl1q_u8(mask1_lo, lo1_b);
        uint8x16_t res1_hi_b = vqtbl1q_u8(mask1_hi, hi1_b);

        uint8x16_t cand0_b = vandq_u8(res0_lo_b, res0_hi_b);
        uint8x16_t cand1_b = vandq_u8(res1_lo_b, res1_hi_b);
        uint8x16_t candidates_b = vandq_u8(cand0_b, cand1_b);

        // For 3-byte fingerprint, also check byte 2
        if (use_3byte) {
            uint8x16_t chunk2_a = vld1q_u8(hay + i + 2);
            uint8x16_t lo2_a = vandq_u8(chunk2_a, nybble_mask);
            uint8x16_t hi2_a = vshrq_n_u8(chunk2_a, 4);
            uint8x16_t res2_lo_a = vqtbl1q_u8(mask2_lo, lo2_a);
            uint8x16_t res2_hi_a = vqtbl1q_u8(mask2_hi, hi2_a);
            uint8x16_t cand2_a = vandq_u8(res2_lo_a, res2_hi_a);
            candidates_a = vandq_u8(candidates_a, cand2_a);

            uint8x16_t chunk2_b = vld1q_u8(hay + i + 18);
            uint8x16_t lo2_b = vandq_u8(chunk2_b, nybble_mask);
            uint8x16_t hi2_b = vshrq_n_u8(chunk2_b, 4);
            uint8x16_t res2_lo_b = vqtbl1q_u8(mask2_lo, lo2_b);
            uint8x16_t res2_hi_b = vqtbl1q_u8(mask2_hi, hi2_b);
            uint8x16_t cand2_b = vandq_u8(res2_lo_b, res2_hi_b);
            candidates_b = vandq_u8(candidates_b, cand2_b);
        }

        // Process first 16 bytes
        uint64x2_t as_u64_a = vreinterpretq_u64_u8(candidates_a);
        uint64_t bits_lo = vgetq_lane_u64(as_u64_a, 0);
        uint64_t bits_hi = vgetq_lane_u64(as_u64_a, 1);

        while (bits_lo) {
            int byte_idx = __builtin_ctzll(bits_lo) >> 3;
            size_t pos = i + byte_idx;
            if (pos + t->pattern_len <= len) {
                if (verify_match_fast(hay + pos, t->pattern, t->pattern_len,
                                     t->case_insensitive)) {
                    if (cb) cb(pos, ctx);
                    matches++;
                }
            }
            bits_lo &= ~(0xFFULL << (byte_idx * 8));
        }

        while (bits_hi) {
            int byte_idx = __builtin_ctzll(bits_hi) >> 3;
            size_t pos = i + 8 + byte_idx;
            if (pos + t->pattern_len <= len) {
                if (verify_match_fast(hay + pos, t->pattern, t->pattern_len,
                                     t->case_insensitive)) {
                    if (cb) cb(pos, ctx);
                    matches++;
                }
            }
            bits_hi &= ~(0xFFULL << (byte_idx * 8));
        }

        // Process second 16 bytes
        uint64x2_t as_u64_b = vreinterpretq_u64_u8(candidates_b);
        bits_lo = vgetq_lane_u64(as_u64_b, 0);
        bits_hi = vgetq_lane_u64(as_u64_b, 1);

        while (bits_lo) {
            int byte_idx = __builtin_ctzll(bits_lo) >> 3;
            size_t pos = i + 16 + byte_idx;
            if (pos + t->pattern_len <= len) {
                if (verify_match_fast(hay + pos, t->pattern, t->pattern_len,
                                     t->case_insensitive)) {
                    if (cb) cb(pos, ctx);
                    matches++;
                }
            }
            bits_lo &= ~(0xFFULL << (byte_idx * 8));
        }

        while (bits_hi) {
            int byte_idx = __builtin_ctzll(bits_hi) >> 3;
            size_t pos = i + 24 + byte_idx;
            if (pos + t->pattern_len <= len) {
                if (verify_match_fast(hay + pos, t->pattern, t->pattern_len,
                                     t->case_insensitive)) {
                    if (cb) cb(pos, ctx);
                    matches++;
                }
            }
            bits_hi &= ~(0xFFULL << (byte_idx * 8));
        }

        i += 32;
    }

    // Process remaining 16 bytes if any
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

        if (use_3byte) {
            uint8x16_t chunk2 = vld1q_u8(hay + i + 2);
            uint8x16_t lo2 = vandq_u8(chunk2, nybble_mask);
            uint8x16_t hi2 = vshrq_n_u8(chunk2, 4);
            uint8x16_t res2_lo = vqtbl1q_u8(mask2_lo, lo2);
            uint8x16_t res2_hi = vqtbl1q_u8(mask2_hi, hi2);
            uint8x16_t cand2 = vandq_u8(res2_lo, res2_hi);
            candidates = vandq_u8(candidates, cand2);
        }

        uint64x2_t as_u64 = vreinterpretq_u64_u8(candidates);
        uint64_t bits_lo = vgetq_lane_u64(as_u64, 0);
        uint64_t bits_hi = vgetq_lane_u64(as_u64, 1);

        while (bits_lo) {
            int byte_idx = __builtin_ctzll(bits_lo) >> 3;
            size_t pos = i + byte_idx;
            if (pos + t->pattern_len <= len) {
                if (verify_match_fast(hay + pos, t->pattern, t->pattern_len,
                                     t->case_insensitive)) {
                    if (cb) cb(pos, ctx);
                    matches++;
                }
            }
            bits_lo &= ~(0xFFULL << (byte_idx * 8));
        }

        while (bits_hi) {
            int byte_idx = __builtin_ctzll(bits_hi) >> 3;
            size_t pos = i + 8 + byte_idx;
            if (pos + t->pattern_len <= len) {
                if (verify_match_fast(hay + pos, t->pattern, t->pattern_len,
                                     t->case_insensitive)) {
                    if (cb) cb(pos, ctx);
                    matches++;
                }
            }
            bits_hi &= ~(0xFFULL << (byte_idx * 8));
        }

        i += 16;
    }

    // Handle remaining bytes with scalar fallback
    while (i < search_len) {
        if (verify_match_fast(hay + i, t->pattern, t->pattern_len, t->case_insensitive)) {
            if (cb) cb(i, ctx);
            matches++;
        }
        i++;
    }

    return matches;
}

#endif // TEDDY_ARM64

// =============================================================================
// x86-64 SSSE3 Implementation
// =============================================================================
#ifdef TEDDY_X86_64

// Fast verification for x86-64: check 8 bytes at once
static inline bool verify_match_fast_x86(const uint8_t *hay, const uint8_t *needle,
                                         size_t len, bool case_insensitive) {
    if (case_insensitive) {
        for (size_t i = 0; i < len; i++) {
            if (to_lower(hay[i]) != to_lower(needle[i])) {
                return false;
            }
        }
        return true;
    }

    // Case-sensitive: use 8-byte chunks
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

static int teddy_search_ssse3(const Teddy *t, const uint8_t *hay, size_t len,
                              teddy_match_cb cb, void *ctx) {
    if (len < t->pattern_len) {
        return 0;
    }

    int matches = 0;
    const size_t search_len = len - t->pattern_len + 1;
    const bool use_3byte = (t->fingerprint_len == 3);

    // Load masks into SSE registers
    __m128i mask0_lo = _mm_load_si128((__m128i*)t->mask0_lo);
    __m128i mask0_hi = _mm_load_si128((__m128i*)t->mask0_hi);
    __m128i mask1_lo = _mm_load_si128((__m128i*)t->mask1_lo);
    __m128i mask1_hi = _mm_load_si128((__m128i*)t->mask1_hi);
    __m128i mask2_lo, mask2_hi;
    if (use_3byte) {
        mask2_lo = _mm_load_si128((__m128i*)t->mask2_lo);
        mask2_hi = _mm_load_si128((__m128i*)t->mask2_hi);
    }
    __m128i nybble_mask = _mm_set1_epi8(0x0F);

    size_t i = 0;

    // Process 16 bytes at a time
    while (i + 16 <= search_len) {
        // Load chunks for each fingerprint byte position
        __m128i chunk0 = _mm_loadu_si128((__m128i*)(hay + i));
        __m128i chunk1 = _mm_loadu_si128((__m128i*)(hay + i + 1));

        // Split byte 0 into nybbles
        __m128i lo0 = _mm_and_si128(chunk0, nybble_mask);
        __m128i hi0 = _mm_and_si128(_mm_srli_epi16(chunk0, 4), nybble_mask);

        // Split byte 1 into nybbles
        __m128i lo1 = _mm_and_si128(chunk1, nybble_mask);
        __m128i hi1 = _mm_and_si128(_mm_srli_epi16(chunk1, 4), nybble_mask);

        // Parallel lookup using pshufb
        __m128i res0_lo = _mm_shuffle_epi8(mask0_lo, lo0);
        __m128i res0_hi = _mm_shuffle_epi8(mask0_hi, hi0);
        __m128i res1_lo = _mm_shuffle_epi8(mask1_lo, lo1);
        __m128i res1_hi = _mm_shuffle_epi8(mask1_hi, hi1);

        // AND results for bytes 0 and 1
        __m128i cand0 = _mm_and_si128(res0_lo, res0_hi);
        __m128i cand1 = _mm_and_si128(res1_lo, res1_hi);
        __m128i candidates = _mm_and_si128(cand0, cand1);

        // For 3-byte fingerprint, also check byte 2
        if (use_3byte) {
            __m128i chunk2 = _mm_loadu_si128((__m128i*)(hay + i + 2));
            __m128i lo2 = _mm_and_si128(chunk2, nybble_mask);
            __m128i hi2 = _mm_and_si128(_mm_srli_epi16(chunk2, 4), nybble_mask);
            __m128i res2_lo = _mm_shuffle_epi8(mask2_lo, lo2);
            __m128i res2_hi = _mm_shuffle_epi8(mask2_hi, hi2);
            __m128i cand2 = _mm_and_si128(res2_lo, res2_hi);
            candidates = _mm_and_si128(candidates, cand2);
        }

        // Extract candidate positions using movemask
        int mask = _mm_movemask_epi8(candidates);

        // Verify each candidate
        while (mask) {
            int bit = __builtin_ctz(mask);
            size_t pos = i + bit;
            if (pos + t->pattern_len <= len) {
                if (verify_match_fast_x86(hay + pos, t->pattern, t->pattern_len,
                                          t->case_insensitive)) {
                    if (cb) cb(pos, ctx);
                    matches++;
                }
            }
            mask &= mask - 1;  // Clear lowest set bit
        }

        i += 16;
    }

    // Handle remaining bytes with scalar fallback
    while (i < search_len) {
        if (verify_match_fast_x86(hay + i, t->pattern, t->pattern_len, t->case_insensitive)) {
            if (cb) cb(i, ctx);
            matches++;
        }
        i++;
    }

    return matches;
}

#endif // TEDDY_X86_64

// =============================================================================
// Scalar Fallback Implementation
// =============================================================================
#if !defined(TEDDY_ARM64) && !defined(TEDDY_X86_64)
static int teddy_search_scalar(const Teddy *t, const uint8_t *hay, size_t len,
                               teddy_match_cb cb, void *ctx) {
    if (len < t->pattern_len) {
        return 0;
    }

    int matches = 0;
    const size_t search_len = len - t->pattern_len + 1;

    // Simple two-byte fingerprint check + verify
    uint8_t b0 = t->pattern[0];
    uint8_t b1 = t->pattern[1];

    for (size_t i = 0; i < search_len; i++) {
        // Check 2-byte fingerprint
        bool match0, match1;
        if (t->case_insensitive) {
            match0 = to_lower(hay[i]) == to_lower(b0);
            match1 = to_lower(hay[i + 1]) == to_lower(b1);
        } else {
            match0 = hay[i] == b0;
            match1 = hay[i + 1] == b1;
        }

        if (match0 && match1) {
            // Verify full pattern
            if (verify_match(hay + i, t->pattern, t->pattern_len, t->case_insensitive)) {
                if (cb) cb(i, ctx);
                matches++;
            }
        }
    }

    return matches;
}
#endif // !TEDDY_ARM64 && !TEDDY_X86_64

// =============================================================================
// Public API
// =============================================================================

int teddy_search(const Teddy *t, const uint8_t *haystack, size_t haystack_len,
                 teddy_match_cb cb, void *ctx) {
    if (!t || !haystack) {
        return -1;
    }

#ifdef TEDDY_ARM64
    return teddy_search_neon(t, haystack, haystack_len, cb, ctx);
#elif defined(TEDDY_X86_64)
    return teddy_search_ssse3(t, haystack, haystack_len, cb, ctx);
#else
    return teddy_search_scalar(t, haystack, haystack_len, cb, ctx);
#endif
}

ssize_t teddy_find_first(const Teddy *t, const uint8_t *haystack, size_t haystack_len) {
    if (!t || !haystack || haystack_len < t->pattern_len) {
        return -1;
    }

#ifdef TEDDY_ARM64
    const size_t search_len = haystack_len - t->pattern_len + 1;
    const uint8_t *hay = haystack;
    const bool use_3byte = (t->fingerprint_len == 3);

    uint8x16_t mask0_lo = vld1q_u8(t->mask0_lo);
    uint8x16_t mask0_hi = vld1q_u8(t->mask0_hi);
    uint8x16_t mask1_lo = vld1q_u8(t->mask1_lo);
    uint8x16_t mask1_hi = vld1q_u8(t->mask1_hi);
    uint8x16_t mask2_lo, mask2_hi;
    if (use_3byte) {
        mask2_lo = vld1q_u8(t->mask2_lo);
        mask2_hi = vld1q_u8(t->mask2_hi);
    }
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

        // For 3-byte fingerprint, also check byte 2
        if (use_3byte) {
            uint8x16_t chunk2 = vld1q_u8(hay + i + 2);
            uint8x16_t lo2 = vandq_u8(chunk2, nybble_mask);
            uint8x16_t hi2 = vshrq_n_u8(chunk2, 4);
            uint8x16_t res2_lo = vqtbl1q_u8(mask2_lo, lo2);
            uint8x16_t res2_hi = vqtbl1q_u8(mask2_hi, hi2);
            uint8x16_t cand2 = vandq_u8(res2_lo, res2_hi);
            candidates = vandq_u8(candidates, cand2);
        }

        uint64x2_t as_u64 = vreinterpretq_u64_u8(candidates);
        uint64_t bits_lo = vgetq_lane_u64(as_u64, 0);
        uint64_t bits_hi = vgetq_lane_u64(as_u64, 1);

        // Process low 8 bytes using bit manipulation
        while (bits_lo) {
            int byte_idx = __builtin_ctzll(bits_lo) >> 3;
            size_t pos = i + byte_idx;
            if (pos + t->pattern_len <= haystack_len) {
                if (verify_match_fast(hay + pos, t->pattern, t->pattern_len,
                                     t->case_insensitive)) {
                    return (ssize_t)pos;
                }
            }
            bits_lo &= ~(0xFFULL << (byte_idx * 8));
        }

        // Process high 8 bytes
        while (bits_hi) {
            int byte_idx = __builtin_ctzll(bits_hi) >> 3;
            size_t pos = i + 8 + byte_idx;
            if (pos + t->pattern_len <= haystack_len) {
                if (verify_match_fast(hay + pos, t->pattern, t->pattern_len,
                                     t->case_insensitive)) {
                    return (ssize_t)pos;
                }
            }
            bits_hi &= ~(0xFFULL << (byte_idx * 8));
        }
        i += 16;
    }

    // Scalar fallback for remaining
    while (i < search_len) {
        if (verify_match_fast(hay + i, t->pattern, t->pattern_len, t->case_insensitive)) {
            return (ssize_t)i;
        }
        i++;
    }
    return -1;

#elif defined(TEDDY_X86_64)
    const size_t search_len = haystack_len - t->pattern_len + 1;
    const uint8_t *hay = haystack;
    const bool use_3byte = (t->fingerprint_len == 3);

    __m128i mask0_lo = _mm_load_si128((__m128i*)t->mask0_lo);
    __m128i mask0_hi = _mm_load_si128((__m128i*)t->mask0_hi);
    __m128i mask1_lo = _mm_load_si128((__m128i*)t->mask1_lo);
    __m128i mask1_hi = _mm_load_si128((__m128i*)t->mask1_hi);
    __m128i mask2_lo, mask2_hi;
    if (use_3byte) {
        mask2_lo = _mm_load_si128((__m128i*)t->mask2_lo);
        mask2_hi = _mm_load_si128((__m128i*)t->mask2_hi);
    }
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

        // For 3-byte fingerprint, also check byte 2
        if (use_3byte) {
            __m128i chunk2 = _mm_loadu_si128((__m128i*)(hay + i + 2));
            __m128i lo2 = _mm_and_si128(chunk2, nybble_mask);
            __m128i hi2 = _mm_and_si128(_mm_srli_epi16(chunk2, 4), nybble_mask);
            __m128i res2_lo = _mm_shuffle_epi8(mask2_lo, lo2);
            __m128i res2_hi = _mm_shuffle_epi8(mask2_hi, hi2);
            __m128i cand2 = _mm_and_si128(res2_lo, res2_hi);
            candidates = _mm_and_si128(candidates, cand2);
        }

        int mask = _mm_movemask_epi8(candidates);
        while (mask) {
            int bit = __builtin_ctz(mask);
            size_t pos = i + bit;
            if (pos + t->pattern_len <= haystack_len) {
                if (verify_match_fast_x86(hay + pos, t->pattern, t->pattern_len,
                                          t->case_insensitive)) {
                    return (ssize_t)pos;
                }
            }
            mask &= mask - 1;
        }
        i += 16;
    }

    // Scalar fallback
    while (i < search_len) {
        if (verify_match_fast_x86(hay + i, t->pattern, t->pattern_len, t->case_insensitive)) {
            return (ssize_t)i;
        }
        i++;
    }
    return -1;

#else
    // Scalar fallback
    const size_t search_len = haystack_len - t->pattern_len + 1;
    for (size_t i = 0; i < search_len; i++) {
        if (verify_match(haystack + i, t->pattern, t->pattern_len, t->case_insensitive)) {
            return (ssize_t)i;
        }
    }
    return -1;
#endif
}

bool teddy_contains(const Teddy *t, const uint8_t *haystack, size_t haystack_len) {
    return teddy_find_first(t, haystack, haystack_len) >= 0;
}
