#include "dfa.h"
#include <string.h>

// Platform detection
#if defined(__aarch64__) || defined(_M_ARM64)
    #define SHENG_ARM64 1
    #include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64)
    #define SHENG_X86_64 1
    #include <immintrin.h>
    #include <tmmintrin.h>
#endif

// =============================================================================
// Sheng DFA Compilation
// =============================================================================
//
// Sheng DFA encodes transitions such that for each byte b, we can compute:
//   next_state = trans[current_state][b]
//
// Since we have at most 16 states (fitting in 4 bits), we can use SIMD shuffle
// instructions for O(1) lookup. For each input byte b:
//   1. Use lo_masks[state] to look up via (b & 0xF) -> gets "lo contribution"
//   2. Use hi_masks[state] to look up via (b >> 4) -> gets "hi contribution"
//   3. Combine: next_state = (lo_contribution >> (hi * 4)) & 0xF
//
// However, this decomposition only works for specific transition patterns.
// Instead, we use a simpler approach: for each state, build a 256-byte table
// compressed into 16 shuffle masks (one per high nybble). The low nybble
// selects which byte in the shuffle result to use.
//
// Actually, for correctness, we use a direct transition table approach:
// For each state s and byte b, we store trans[s][b] directly in a format
// amenable to SIMD. Since state IDs fit in 4 bits, we pack two per byte.

bool sheng_compile(ShengDfa *sheng, const Dfa *dfa) {
    if (!sheng || !dfa) return false;

    memset(sheng, 0, sizeof(*sheng));

    // Sheng only works for small DFAs (<=16 states)
    if (dfa->state_count > SHENG_MAX_STATES) {
        sheng->is_valid = false;
        return false;
    }

    sheng->state_count = (uint8_t)dfa->state_count;

    // Build direct transition tables
    // For each state, we have 16 "shuffle masks" - one for each high nybble
    // Each mask contains 16 entries (one for each low nybble)
    // The entry contains the next state (0-15, or 0 for dead state)

    for (uint32_t state = 0; state < dfa->state_count; state++) {
        const DfaState *s = &dfa->states[state];

        // lo_masks[state] = transitions for bytes 0x00-0x0F
        // This is indexed by the low nybble when high nybble matches state's index
        // Actually, we need to restructure: lo_masks[state][lo] should give
        // the next state for byte with low nybble = lo, across all high nybbles

        // For proper Sheng, we store: for state s and byte b (0x00-0xFF):
        //   trans[s][b] = next_state
        // We need two lookups to reconstruct this.

        // Correct approach:
        // masks[state][hi_nybble][lo_nybble] = next_state for byte (hi<<4 | lo)
        // But we only have lo_masks and hi_masks (16 bytes each per state).
        //
        // The trick is: for byte b, we want trans[state][b].
        // We can use: next = shuffle(masks[state], b & 0xF) - but this only
        // works if the transition is the same for all bytes with same low nybble.
        //
        // Since that's rarely true, Sheng actually uses a different trick:
        // It stores state in the high bits of a register and uses the byte value
        // to index into a larger table. For 16 states, this becomes tractable.
        //
        // For simplicity, let's use a 256-byte transition table per state
        // and do a straightforward scalar lookup for now, but structured
        // for potential SIMD optimization later.

        for (int byte = 0; byte < 256; byte++) {
            int lo = byte & 0xF;
            int hi = byte >> 4;
            uint16_t next = s->transitions[byte];

            // Store the next state (clamp to 0 if dead/out of range)
            uint8_t next_state = (next < SHENG_MAX_STATES) ? (uint8_t)next : 0;

            // Store in both masks so we can look up either way
            // lo_masks[state][lo] will have the state for hi=0
            // hi_masks[state][hi] will have the state for lo=0
            // This is for the scalar fallback
            if (hi == 0) {
                sheng->lo_masks[state][lo] = next_state;
            }
            if (lo == 0) {
                sheng->hi_masks[state][hi] = next_state;
            }
        }

        // Build accept mask
        if (s->flags & DFA_FLAG_MATCH) {
            sheng->accept_mask |= (1 << state);
        }
    }

    // Mark as invalid - the encoding above is still not correct for SIMD
    // We need to fall back to DFA for now
    sheng->is_valid = false;
    return false;
}

// =============================================================================
// Sheng DFA Execution (Scalar fallback)
// =============================================================================

// Scalar version for reference and non-SIMD platforms
static inline uint8_t sheng_transition_scalar(const ShengDfa *sheng,
                                               uint8_t state, uint8_t byte) {
    uint8_t lo_nyb = byte & 0x0F;
    uint8_t hi_nyb = byte >> 4;

    uint8_t lo_bits = sheng->lo_masks[state][lo_nyb];
    uint8_t hi_bits = sheng->hi_masks[state][hi_nyb];
    uint8_t possible = lo_bits & hi_bits;

    // Find the single set bit (should be exactly one for valid DFA)
    // Using CTZ (count trailing zeros) or bit scan
    if (possible == 0) return 0;  // Dead state

    // Fast path for small state numbers
    for (uint8_t i = 0; i < 16; i++) {
        if (possible & (1 << i)) return i;
    }
    return 0;
}

// =============================================================================
// Sheng DFA Execution (SIMD)
// =============================================================================

#if defined(SHENG_ARM64)

// ARM NEON implementation
bool sheng_contains(const ShengDfa *sheng, const uint8_t *input, size_t len) {
    if (!sheng || !sheng->is_valid || len == 0) return false;

    uint8_t state = DFA_START_STATE;

    // Check immediate match
    if (sheng->accept_mask & (1 << state)) {
        return true;
    }

    // Process bytes
    const uint8_t *p = input;
    const uint8_t *end = input + len;

    // Main loop using NEON table lookups
    while (p < end) {
        uint8_t byte = *p++;
        uint8_t lo_nyb = byte & 0x0F;
        uint8_t hi_nyb = byte >> 4;

        // Load the masks for current state
        uint8x16_t lo_mask = vld1q_u8(sheng->lo_masks[state]);
        uint8x16_t hi_mask = vld1q_u8(sheng->hi_masks[state]);

        // Use table lookup to get the bits for this nybble
        uint8x16_t lo_idx = vdupq_n_u8(lo_nyb);
        uint8x16_t hi_idx = vdupq_n_u8(hi_nyb);

        uint8x16_t lo_result = vqtbl1q_u8(lo_mask, lo_idx);
        uint8x16_t hi_result = vqtbl1q_u8(hi_mask, hi_idx);

        // AND to get possible next states
        uint8x16_t possible = vandq_u8(lo_result, hi_result);
        uint8_t possible_byte = vgetq_lane_u8(possible, 0);

        // Find next state (lowest set bit)
        if (possible_byte == 0) {
            // Dead state - restart from next position would be needed for unanchored
            // For contains, we can try from the next position
            state = DFA_START_STATE;

            // Check if start state is accepting
            if (sheng->accept_mask & (1 << state)) {
                return true;
            }
            continue;
        }

        // Get next state via CTZ
        state = __builtin_ctz(possible_byte);

        // Check for match
        if (sheng->accept_mask & (1 << state)) {
            return true;
        }
    }

    return false;
}

bool sheng_match_at(const ShengDfa *sheng, const uint8_t *input, size_t len,
                    size_t start, Match *match) {
    if (!sheng || !sheng->is_valid || start > len) return false;

    uint8_t state = DFA_START_STATE;
    bool matched = false;
    size_t match_end = start;

    // Check immediate match
    if (sheng->accept_mask & (1 << state)) {
        matched = true;
        match_end = start;
    }

    const uint8_t *p = input + start;
    const uint8_t *end = input + len;

    while (p < end) {
        uint8_t byte = *p++;
        uint8_t lo_nyb = byte & 0x0F;
        uint8_t hi_nyb = byte >> 4;

        // Load masks and do lookup
        uint8x16_t lo_mask = vld1q_u8(sheng->lo_masks[state]);
        uint8x16_t hi_mask = vld1q_u8(sheng->hi_masks[state]);

        uint8x16_t lo_idx = vdupq_n_u8(lo_nyb);
        uint8x16_t hi_idx = vdupq_n_u8(hi_nyb);

        uint8x16_t lo_result = vqtbl1q_u8(lo_mask, lo_idx);
        uint8x16_t hi_result = vqtbl1q_u8(hi_mask, hi_idx);

        uint8x16_t possible = vandq_u8(lo_result, hi_result);
        uint8_t possible_byte = vgetq_lane_u8(possible, 0);

        if (possible_byte == 0) {
            break;  // Dead state
        }

        state = __builtin_ctz(possible_byte);

        if (sheng->accept_mask & (1 << state)) {
            matched = true;
            match_end = p - input;
        }
    }

    if (matched && match) {
        match->start = start;
        match->end = match_end;
    }

    return matched;
}

#elif defined(SHENG_X86_64)

// x86-64 SSE implementation
bool sheng_contains(const ShengDfa *sheng, const uint8_t *input, size_t len) {
    if (!sheng || !sheng->is_valid || len == 0) return false;

    uint8_t state = DFA_START_STATE;

    if (sheng->accept_mask & (1 << state)) {
        return true;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = input[i];
        uint8_t lo_nyb = byte & 0x0F;
        uint8_t hi_nyb = byte >> 4;

        __m128i lo_mask = _mm_loadu_si128((__m128i *)sheng->lo_masks[state]);
        __m128i hi_mask = _mm_loadu_si128((__m128i *)sheng->hi_masks[state]);

        __m128i lo_idx = _mm_set1_epi8(lo_nyb);
        __m128i hi_idx = _mm_set1_epi8(hi_nyb);

        __m128i lo_result = _mm_shuffle_epi8(lo_mask, lo_idx);
        __m128i hi_result = _mm_shuffle_epi8(hi_mask, hi_idx);

        __m128i possible = _mm_and_si128(lo_result, hi_result);
        uint8_t possible_byte = _mm_extract_epi8(possible, 0);

        if (possible_byte == 0) {
            state = DFA_START_STATE;
            if (sheng->accept_mask & (1 << state)) {
                return true;
            }
            continue;
        }

        state = __builtin_ctz(possible_byte);

        if (sheng->accept_mask & (1 << state)) {
            return true;
        }
    }

    return false;
}

bool sheng_match_at(const ShengDfa *sheng, const uint8_t *input, size_t len,
                    size_t start, Match *match) {
    if (!sheng || !sheng->is_valid || start > len) return false;

    uint8_t state = DFA_START_STATE;
    bool matched = false;
    size_t match_end = start;

    if (sheng->accept_mask & (1 << state)) {
        matched = true;
        match_end = start;
    }

    for (size_t i = start; i < len; i++) {
        uint8_t byte = input[i];
        uint8_t lo_nyb = byte & 0x0F;
        uint8_t hi_nyb = byte >> 4;

        __m128i lo_mask = _mm_loadu_si128((__m128i *)sheng->lo_masks[state]);
        __m128i hi_mask = _mm_loadu_si128((__m128i *)sheng->hi_masks[state]);

        __m128i lo_idx = _mm_set1_epi8(lo_nyb);
        __m128i hi_idx = _mm_set1_epi8(hi_nyb);

        __m128i lo_result = _mm_shuffle_epi8(lo_mask, lo_idx);
        __m128i hi_result = _mm_shuffle_epi8(hi_mask, hi_idx);

        __m128i possible = _mm_and_si128(lo_result, hi_result);
        uint8_t possible_byte = _mm_extract_epi8(possible, 0);

        if (possible_byte == 0) {
            break;
        }

        state = __builtin_ctz(possible_byte);

        if (sheng->accept_mask & (1 << state)) {
            matched = true;
            match_end = i + 1;
        }
    }

    if (matched && match) {
        match->start = start;
        match->end = match_end;
    }

    return matched;
}

#else

// Scalar fallback for other platforms
bool sheng_contains(const ShengDfa *sheng, const uint8_t *input, size_t len) {
    if (!sheng || !sheng->is_valid || len == 0) return false;

    uint8_t state = DFA_START_STATE;

    if (sheng->accept_mask & (1 << state)) {
        return true;
    }

    for (size_t i = 0; i < len; i++) {
        state = sheng_transition_scalar(sheng, state, input[i]);

        if (state == 0) {
            // Dead state - restart
            state = DFA_START_STATE;
            if (sheng->accept_mask & (1 << state)) {
                return true;
            }
            continue;
        }

        if (sheng->accept_mask & (1 << state)) {
            return true;
        }
    }

    return false;
}

bool sheng_match_at(const ShengDfa *sheng, const uint8_t *input, size_t len,
                    size_t start, Match *match) {
    if (!sheng || !sheng->is_valid || start > len) return false;

    uint8_t state = DFA_START_STATE;
    bool matched = false;
    size_t match_end = start;

    if (sheng->accept_mask & (1 << state)) {
        matched = true;
        match_end = start;
    }

    for (size_t i = start; i < len; i++) {
        state = sheng_transition_scalar(sheng, state, input[i]);

        if (state == 0) {
            break;
        }

        if (sheng->accept_mask & (1 << state)) {
            matched = true;
            match_end = i + 1;
        }
    }

    if (matched && match) {
        match->start = start;
        match->end = match_end;
    }

    return matched;
}

#endif
