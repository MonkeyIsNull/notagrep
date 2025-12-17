#include "dfa.h"
#include <string.h>

// Platform-specific memchr variants
#if defined(__APPLE__) || defined(__linux__)
#include <string.h>
#endif

// memchr for 1, 2, or 3 bytes - find the first occurrence of any of the bytes
// Returns pointer to match or NULL if not found
static inline const uint8_t *memchr_accel(const uint8_t *haystack, size_t len,
                                          const DfaStateAccel *accel) {
    if (accel->count == 1) {
        return memchr(haystack, accel->bytes[0], len);
    }

    // For 2-3 bytes, scan byte by byte (can be optimized with SIMD later)
    // This is still faster than DFA transitions when most bytes loop back
    const uint8_t b0 = accel->bytes[0];
    const uint8_t b1 = accel->bytes[1];
    const uint8_t b2 = (accel->count >= 3) ? accel->bytes[2] : 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = haystack[i];
        if (c == b0 || c == b1 || (accel->count >= 3 && c == b2)) {
            return haystack + i;
        }
    }
    return NULL;
}

// =============================================================================
// DFA Execution - Hot Path
// =============================================================================

// Match at a specific position (anchored match)
// Returns true if match found, sets match->start and match->end
bool dfa_match_at(const Dfa *dfa, const uint8_t *input, size_t len,
                  size_t start, Match *match) {
    if (!dfa || start > len) return false;

    const DfaState *states = dfa->states;
    uint16_t state = DFA_START_STATE;
    const uint8_t *p = input + start;
    const uint8_t *end = input + len;

    // Track match positions (for leftmost-longest semantics)
    bool matched = false;
    size_t match_end = start;

    // Check for immediate match (empty pattern or start anchor satisfied)
    if (states[state].flags & DFA_FLAG_MATCH) {
        matched = true;
        match_end = start;
    }

    // Main loop - this is the hot path
    while (p < end) {
        state = states[state].transitions[*p++];

        if (state == DFA_DEAD_STATE) {
            break;
        }

        if (states[state].flags & DFA_FLAG_MATCH) {
            matched = true;
            match_end = p - input;
            // Continue to find longest match
        }
    }

    // Handle end anchor if present
    // For end anchor, match must be at end of input or before newline
    if (dfa->has_end_anchor && matched) {
        // Verify match position is at end of line or input
        if (match_end < len && input[match_end] != '\n') {
            // Not at end of line/input - match is invalid
            matched = false;
        }
    }

    if (matched && match) {
        match->start = start;
        match->end = match_end;
    }

    return matched;
}

// Find first match anywhere in input (unanchored)
// Returns true if match found
bool dfa_find_first(const Dfa *dfa, const uint8_t *input, size_t len,
                    Match *match) {
    if (!dfa) return false;

    const DfaState *states = dfa->states;

    // For start-anchored patterns, only try at position 0 or after newlines
    if (dfa->has_start_anchor) {
        // Try at start of input
        if (dfa_match_at(dfa, input, len, 0, match)) {
            return true;
        }

        // Try after each newline
        for (size_t i = 0; i < len; i++) {
            if (input[i] == '\n' && i + 1 <= len) {
                if (dfa_match_at(dfa, input, len, i + 1, match)) {
                    return true;
                }
            }
        }

        return false;
    }

    // Unanchored: single pass with state reset on dead state
    // This avoids O(n*m) by not restarting from every position
    const DfaStateAccel *accel = dfa->accel;
    uint16_t state = DFA_START_STATE;
    const uint8_t *p = input;
    const uint8_t *end = input + len;
    size_t match_start = 0;

    // Check for immediate match (empty pattern)
    if (states[state].flags & DFA_FLAG_MATCH) {
        if (match) {
            match->start = 0;
            match->end = 0;
        }
        return true;
    }

    while (p < end) {
        // Try to accelerate if this state is acceleratable
        if (accel && accel[state].count > 0) {
            const uint8_t *skip = memchr_accel(p, end - p, &accel[state]);
            if (!skip) {
                // No interesting bytes - pattern cannot match from here
                return false;
            }
            p = skip;
        }

        uint8_t byte = *p++;
        state = states[state].transitions[byte];

        if (state == DFA_DEAD_STATE) {
            // Reset to start state, record potential match start
            state = DFA_START_STATE;
            match_start = p - input;
            // Re-process this byte from start state
            state = states[state].transitions[byte];
            if (state == DFA_DEAD_STATE) {
                state = DFA_START_STATE;
                match_start = p - input;
            }
            continue;
        }

        if (states[state].flags & DFA_FLAG_MATCH) {
            // Found a match - now find longest match
            size_t match_end = p - input;
            while (p < end) {
                state = states[state].transitions[*p];
                if (state == DFA_DEAD_STATE) {
                    break;
                }
                p++;
                if (states[state].flags & DFA_FLAG_MATCH) {
                    match_end = p - input;
                }
            }
            if (match) {
                match->start = match_start;
                match->end = match_end;
            }
            return true;
        }
    }

    return false;
}

// Check if any match exists (fast path for -l/-q/-c modes)
// This is the most optimized path - we just need to know if there's a match
bool dfa_contains(const Dfa *dfa, const uint8_t *input, size_t len) {
    if (!dfa) return false;

    const DfaState *states = dfa->states;
    const DfaStateAccel *accel = dfa->accel;  // May be NULL

    // For start-anchored patterns
    if (dfa->has_start_anchor) {
        uint16_t state = DFA_START_STATE;

        if (states[state].flags & DFA_FLAG_MATCH) {
            return true;
        }

        const uint8_t *p = input;
        const uint8_t *end = input + len;

        while (p < end) {
            // Try to accelerate if this state is acceleratable
            if (accel && accel[state].count > 0) {
                const uint8_t *skip = memchr_accel(p, end - p, &accel[state]);
                if (!skip) {
                    // No interesting bytes found - stay in this state
                    // For anchored patterns, check if we hit any newlines
                    const uint8_t *nl = memchr(p, '\n', end - p);
                    if (nl) {
                        p = nl + 1;
                        state = DFA_START_STATE;
                        if (states[state].flags & DFA_FLAG_MATCH) {
                            return true;
                        }
                        continue;
                    }
                    return false;
                }
                p = skip;
            }

            state = states[state].transitions[*p++];

            if (state == DFA_DEAD_STATE) {
                // Reset at newlines for ^ anchor
                if (p[-1] == '\n') {
                    state = DFA_START_STATE;
                    if (states[state].flags & DFA_FLAG_MATCH) {
                        return true;
                    }
                }
                continue;
            }

            if (states[state].flags & DFA_FLAG_MATCH) {
                return true;
            }
        }

        return false;
    }

    // Unanchored search - single pass with state acceleration
    // Key insight: we don't need to restart from every position
    // We run the DFA continuously, resetting to start state on dead state
    uint16_t state = DFA_START_STATE;
    const uint8_t *p = input;
    const uint8_t *end = input + len;

    // Check for immediate match (empty pattern)
    if (states[state].flags & DFA_FLAG_MATCH) {
        return true;
    }

    while (p < end) {
        // Try to accelerate if this state is acceleratable
        if (accel && accel[state].count > 0) {
            const uint8_t *skip = memchr_accel(p, end - p, &accel[state]);
            if (!skip) {
                // No interesting bytes found - no match possible from this state
                // But we might be able to restart from a later position
                // For now, just return false (most patterns this is correct)
                return false;
            }
            p = skip;
        }

        state = states[state].transitions[*p++];

        if (state == DFA_DEAD_STATE) {
            // Reset to start state for unanchored search
            state = DFA_START_STATE;
            if (states[state].flags & DFA_FLAG_MATCH) {
                return true;
            }
            continue;
        }

        if (states[state].flags & DFA_FLAG_MATCH) {
            return true;
        }
    }

    return false;
}

// Count lines with matches (for -c mode)
// Returns count of distinct lines that have at least one match
size_t dfa_count_lines(const Dfa *dfa, const uint8_t *input, size_t len) {
    if (!dfa || len == 0) return 0;

    const DfaState *states = dfa->states;
    size_t count = 0;
    size_t line_start = 0;

    while (line_start < len) {
        // Find end of current line
        const uint8_t *line_end_ptr = memchr(input + line_start, '\n', len - line_start);
        size_t line_end = line_end_ptr ? (size_t)(line_end_ptr - input) : len;

        // Check if this line has a match
        bool line_matched = false;

        if (dfa->has_start_anchor) {
            // Only check at start of line
            uint16_t state = DFA_START_STATE;

            if (states[state].flags & DFA_FLAG_MATCH) {
                line_matched = true;
            } else {
                for (size_t i = line_start; i < line_end && !line_matched; i++) {
                    state = states[state].transitions[input[i]];
                    if (state == DFA_DEAD_STATE) break;
                    if (states[state].flags & DFA_FLAG_MATCH) {
                        line_matched = true;
                    }
                }
            }
        } else {
            // Try each position in the line
            for (size_t start = line_start; start <= line_end && !line_matched; start++) {
                uint16_t state = DFA_START_STATE;

                if (states[state].flags & DFA_FLAG_MATCH) {
                    line_matched = true;
                    break;
                }

                for (size_t i = start; i < line_end; i++) {
                    state = states[state].transitions[input[i]];
                    if (state == DFA_DEAD_STATE) break;
                    if (states[state].flags & DFA_FLAG_MATCH) {
                        line_matched = true;
                        break;
                    }
                }
            }
        }

        if (line_matched) {
            count++;
        }

        // Move to next line
        line_start = line_end + 1;
    }

    return count;
}
