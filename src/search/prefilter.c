#include "prefilter.h"
#include "ahocorasick.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Architecture detection
#if defined(__x86_64__) || defined(_M_X64)
    #define ARCH_X86_64
    #include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define ARCH_ARM64
    #include <arm_neon.h>
#endif

// SIMD vector size
#if defined(ARCH_X86_64)
    #define VECTOR_SIZE 16  // SSE2: 128-bit
#elif defined(ARCH_ARM64)
    #define VECTOR_SIZE 16  // NEON: 128-bit
#else
    #define VECTOR_SIZE 1   // No SIMD
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

    // Set skip distances for characters in the needle (except last)
    for (size_t i = 0; i < pf->needle_len - 1; i++) {
        size_t skip = pf->needle_len - 1 - i;
        if (pf->case_insensitive) {
            pf->skip_table[to_lower(pf->needle[i])] = skip;
            pf->skip_table[to_upper(pf->needle[i])] = skip;
        } else {
            pf->skip_table[pf->needle[i]] = skip;
        }
    }
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

    // Enable SIMD for sufficiently long needles
#if defined(ARCH_X86_64) || defined(ARCH_ARM64)
    pf->use_simd = (len >= SIMD_MIN_NEEDLE_LEN);
#else
    pf->use_simd = false;
#endif

    return 0;
}

void prefilter_free(Prefilter *pf) {
    if (pf->needle) {
        free(pf->needle);
        pf->needle = NULL;
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
static inline bool verify_match(const Prefilter *pf, const uint8_t *pos) {
    if (pf->case_insensitive) {
        return memcmp_ci(pos, pf->needle, pf->needle_len) == 0;
    } else {
        return memcmp(pos, pf->needle, pf->needle_len) == 0;
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

    int count = 0;
    const size_t *delta = pf->skip_table;
    const size_t len = pf->needle_len;
    const uint8_t *needle = pf->needle;

    // Guard character: second-to-last byte for quick rejection
    const uint8_t gc = pf->case_insensitive ? to_lower(needle[len - 2]) : needle[len - 2];

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
            // Not a full match, skip by 1 (or could use md2 like git-grep)
            tp++;
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
            d = 1;
        }
        tp += d;
    }

    return count;
}

// =============================================================================
// SIMD implementations
// =============================================================================

#if defined(ARCH_X86_64)

static int search_simd_x86(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len,
                           prefilter_match_cb cb, void *ctx) {
    if (haystack_len < pf->needle_len) {
        return 0;
    }

    int count = 0;
    size_t end_pos = haystack_len - pf->needle_len;

    // Broadcast first and last bytes
    __m128i first_vec, first_vec2;
    __m128i last_vec, last_vec2;

    if (pf->case_insensitive) {
        first_vec = _mm_set1_epi8(pf->first_byte_lower);
        first_vec2 = _mm_set1_epi8(pf->first_byte_upper);
        last_vec = _mm_set1_epi8(to_lower(pf->last_byte));
        last_vec2 = _mm_set1_epi8(to_upper(pf->last_byte));
    } else {
        first_vec = _mm_set1_epi8(pf->first_byte);
        last_vec = _mm_set1_epi8(pf->last_byte);
    }

    size_t i = 0;
    size_t last_offset = pf->needle_len - 1;

    // Process 16 bytes at a time
    while (i + VECTOR_SIZE <= end_pos + 1) {
        // Load 16 bytes at position i (first byte candidates)
        __m128i chunk_first = _mm_loadu_si128((const __m128i *)(haystack + i));
        // Load 16 bytes at position i + needle_len - 1 (last byte candidates)
        __m128i chunk_last = _mm_loadu_si128((const __m128i *)(haystack + i + last_offset));

        // Compare for first byte matches
        __m128i eq_first = _mm_cmpeq_epi8(chunk_first, first_vec);
        if (pf->case_insensitive) {
            __m128i eq_first2 = _mm_cmpeq_epi8(chunk_first, first_vec2);
            eq_first = _mm_or_si128(eq_first, eq_first2);
        }

        // Compare for last byte matches
        __m128i eq_last = _mm_cmpeq_epi8(chunk_last, last_vec);
        if (pf->case_insensitive) {
            __m128i eq_last2 = _mm_cmpeq_epi8(chunk_last, last_vec2);
            eq_last = _mm_or_si128(eq_last, eq_last2);
        }

        // AND the results: positions where both first and last match
        __m128i candidates = _mm_and_si128(eq_first, eq_last);
        int mask = _mm_movemask_epi8(candidates);

        // Process each candidate
        while (mask) {
            int bit = __builtin_ctz(mask);
            size_t pos = i + bit;

            if (pos <= end_pos && verify_match(pf, haystack + pos)) {
                count++;
                if (cb) cb(pos, ctx);
            }

            mask &= mask - 1;  // Clear lowest bit
        }

        i += VECTOR_SIZE;
    }

    // Handle remaining bytes with scalar search
    while (i <= end_pos) {
        bool first_match;
        if (pf->case_insensitive) {
            uint8_t c = to_lower(haystack[i]);
            first_match = (c == pf->first_byte_lower);
        } else {
            first_match = (haystack[i] == pf->first_byte);
        }

        if (first_match && verify_match(pf, haystack + i)) {
            count++;
            if (cb) cb(i, ctx);
        }
        i++;
    }

    return count;
}

#elif defined(ARCH_ARM64)

static int search_simd_arm(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len,
                           prefilter_match_cb cb, void *ctx) {
    if (haystack_len < pf->needle_len) {
        return 0;
    }

    int count = 0;
    size_t end_pos = haystack_len - pf->needle_len;

    // Broadcast first and last bytes
    uint8x16_t first_vec, first_vec2;
    uint8x16_t last_vec, last_vec2;

    if (pf->case_insensitive) {
        first_vec = vdupq_n_u8(pf->first_byte_lower);
        first_vec2 = vdupq_n_u8(pf->first_byte_upper);
        last_vec = vdupq_n_u8(to_lower(pf->last_byte));
        last_vec2 = vdupq_n_u8(to_upper(pf->last_byte));
    } else {
        first_vec = vdupq_n_u8(pf->first_byte);
        last_vec = vdupq_n_u8(pf->last_byte);
    }

    size_t i = 0;
    size_t last_offset = pf->needle_len - 1;

    // Process 16 bytes at a time
    while (i + VECTOR_SIZE <= end_pos + 1) {
        // Load 16 bytes
        uint8x16_t chunk_first = vld1q_u8(haystack + i);
        uint8x16_t chunk_last = vld1q_u8(haystack + i + last_offset);

        // Compare for first byte matches
        uint8x16_t eq_first = vceqq_u8(chunk_first, first_vec);
        if (pf->case_insensitive) {
            uint8x16_t eq_first2 = vceqq_u8(chunk_first, first_vec2);
            eq_first = vorrq_u8(eq_first, eq_first2);
        }

        // Compare for last byte matches
        uint8x16_t eq_last = vceqq_u8(chunk_last, last_vec);
        if (pf->case_insensitive) {
            uint8x16_t eq_last2 = vceqq_u8(chunk_last, last_vec2);
            eq_last = vorrq_u8(eq_last, eq_last2);
        }

        // AND the results
        uint8x16_t candidates = vandq_u8(eq_first, eq_last);

        // Check if any candidates exist (fast path)
        uint64_t low = vgetq_lane_u64(vreinterpretq_u64_u8(candidates), 0);
        uint64_t high = vgetq_lane_u64(vreinterpretq_u64_u8(candidates), 1);

        if (low | high) {
            // Extract candidate positions
            uint8_t mask_bytes[16];
            vst1q_u8(mask_bytes, candidates);

            for (int j = 0; j < 16; j++) {
                if (mask_bytes[j] && i + j <= end_pos) {
                    if (verify_match(pf, haystack + i + j)) {
                        count++;
                        if (cb) cb(i + j, ctx);
                    }
                }
            }
        }

        i += VECTOR_SIZE;
    }

    // Handle remaining bytes with scalar search
    while (i <= end_pos) {
        bool first_match;
        if (pf->case_insensitive) {
            uint8_t c = to_lower(haystack[i]);
            first_match = (c == pf->first_byte_lower);
        } else {
            first_match = (haystack[i] == pf->first_byte);
        }

        if (first_match && verify_match(pf, haystack + i)) {
            count++;
            if (cb) cb(i, ctx);
        }
        i++;
    }

    return count;
}

#endif

// =============================================================================
// Public API
// =============================================================================

int prefilter_search(const Prefilter *pf, const uint8_t *haystack, size_t haystack_len,
                     prefilter_match_cb cb, void *ctx) {
    if (!pf || !pf->needle || !haystack) {
        return -1;
    }

#if defined(ARCH_X86_64)
    if (pf->use_simd) {
        return search_simd_x86(pf, haystack, haystack_len, cb, ctx);
    }
#elif defined(ARCH_ARM64)
    if (pf->use_simd) {
        return search_simd_arm(pf, haystack, haystack_len, cb, ctx);
    }
#endif

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
