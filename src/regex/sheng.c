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
// Sheng DFA Compilation (Hyperscan algorithm)
// =============================================================================
//
// Sheng DFA uses SIMD shuffle for O(1) state transitions.
// For each input byte b, there's a 16-byte shuffle mask where:
//   masks[b][state] = next_state for transition (state, b)
//
// Execution: next_state = shuffle(masks[byte], broadcast(state))[0]
//
// This is simple and correct - no nybble decomposition needed.
// Memory: 256 * 16 = 4KB per DFA

bool sheng_compile(ShengDfa *sheng, const Dfa *dfa) {
    if (!sheng || !dfa) return false;

    memset(sheng, 0, sizeof(*sheng));

    // Sheng only works for small DFAs (<=16 states)
    if (dfa->state_count > SHENG_MAX_STATES) {
        sheng->is_valid = false;
        return false;
    }

    sheng->state_count = (uint8_t)dfa->state_count;

    // Build the 256 shuffle masks (one per input byte)
    // masks[byte][state] = next_state for transition (state, byte)
    for (int byte = 0; byte < 256; byte++) {
        for (uint32_t state = 0; state < dfa->state_count; state++) {
            uint16_t next = dfa->states[state].transitions[byte];
            // Clamp to 0 (dead state) if next >= 16
            sheng->masks[byte][state] = (next < SHENG_MAX_STATES) ? (uint8_t)next : 0;
        }
        // Fill remaining slots with 0 (dead state) for states that don't exist
        for (uint32_t state = dfa->state_count; state < 16; state++) {
            sheng->masks[byte][state] = 0;
        }
    }

    // Build accept mask
    sheng->accept_mask = 0;
    for (uint32_t state = 0; state < dfa->state_count; state++) {
        if (dfa->states[state].flags & DFA_FLAG_MATCH) {
            sheng->accept_mask |= (1 << state);
        }
    }

    sheng->is_valid = true;
    return true;
}

// =============================================================================
// Sheng DFA Execution (ARM NEON)
// =============================================================================

#if defined(SHENG_ARM64)

bool sheng_contains(const ShengDfa *sheng, const uint8_t *input, size_t len) {
    if (!sheng || !sheng->is_valid || len == 0) return false;

    uint8_t state = DFA_START_STATE;

    // Check if start state is accepting
    if (sheng->accept_mask & (1 << state)) {
        return true;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = input[i];

        // Load the shuffle mask for this input byte
        uint8x16_t mask = vld1q_u8(sheng->masks[byte]);

        // Broadcast current state to all lanes
        uint8x16_t state_vec = vdupq_n_u8(state);

        // Shuffle: result[i] = mask[state_vec[i]] = mask[state] for all i
        uint8x16_t result = vqtbl1q_u8(mask, state_vec);

        // Extract next state from lane 0
        state = vgetq_lane_u8(result, 0);

        if (state == 0) {
            // Dead state - restart from start state for unanchored search
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

    // Check if start state is accepting
    if (sheng->accept_mask & (1 << state)) {
        matched = true;
        match_end = start;
    }

    for (size_t i = start; i < len; i++) {
        uint8_t byte = input[i];

        // Load the shuffle mask for this input byte
        uint8x16_t mask = vld1q_u8(sheng->masks[byte]);

        // Broadcast current state and shuffle
        uint8x16_t state_vec = vdupq_n_u8(state);
        uint8x16_t result = vqtbl1q_u8(mask, state_vec);

        // Extract next state
        state = vgetq_lane_u8(result, 0);

        if (state == 0) {
            break;  // Dead state - no more matches possible from this position
        }

        if (sheng->accept_mask & (1 << state)) {
            matched = true;
            match_end = i + 1;
            // Continue to find longest match
        }
    }

    if (matched && match) {
        match->start = start;
        match->end = match_end;
    }

    return matched;
}

// =============================================================================
// Sheng DFA Execution (x86-64 SSE/SSSE3)
// =============================================================================

#elif defined(SHENG_X86_64)

bool sheng_contains(const ShengDfa *sheng, const uint8_t *input, size_t len) {
    if (!sheng || !sheng->is_valid || len == 0) return false;

    uint8_t state = DFA_START_STATE;

    if (sheng->accept_mask & (1 << state)) {
        return true;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = input[i];

        // Load the shuffle mask for this input byte
        __m128i mask = _mm_loadu_si128((__m128i *)sheng->masks[byte]);

        // Broadcast current state to all lanes
        __m128i state_vec = _mm_set1_epi8(state);

        // Shuffle using PSHUFB (SSSE3)
        __m128i result = _mm_shuffle_epi8(mask, state_vec);

        // Extract next state from lane 0
        state = (uint8_t)_mm_extract_epi8(result, 0);

        if (state == 0) {
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
        uint8_t byte = input[i];

        __m128i mask = _mm_loadu_si128((__m128i *)sheng->masks[byte]);
        __m128i state_vec = _mm_set1_epi8(state);
        __m128i result = _mm_shuffle_epi8(mask, state_vec);

        state = (uint8_t)_mm_extract_epi8(result, 0);

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

// =============================================================================
// Sheng DFA Execution (Scalar fallback)
// =============================================================================

#else

bool sheng_contains(const ShengDfa *sheng, const uint8_t *input, size_t len) {
    if (!sheng || !sheng->is_valid || len == 0) return false;

    uint8_t state = DFA_START_STATE;

    if (sheng->accept_mask & (1 << state)) {
        return true;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = input[i];

        // Direct table lookup - masks[byte][state] gives next state
        state = sheng->masks[byte][state];

        if (state == 0) {
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
        uint8_t byte = input[i];

        // Direct table lookup
        state = sheng->masks[byte][state];

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
