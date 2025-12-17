#include "dfa.h"
#include <string.h>

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

    // Unanchored: try each position
    // This is the common case for patterns like "foo" or "a.*b"
    for (size_t start = 0; start <= len; start++) {
        uint16_t state = DFA_START_STATE;
        const uint8_t *p = input + start;
        const uint8_t *end = input + len;

        // Check for immediate match
        if (states[state].flags & DFA_FLAG_MATCH) {
            if (match) {
                match->start = start;
                match->end = start;
            }
            return true;
        }

        // Try to match from this position
        bool matched = false;
        size_t match_end = start;

        while (p < end) {
            state = states[state].transitions[*p++];

            if (state == DFA_DEAD_STATE) {
                break;
            }

            if (states[state].flags & DFA_FLAG_MATCH) {
                matched = true;
                match_end = p - input;
                // Continue for longest match
            }
        }

        if (matched) {
            if (match) {
                match->start = start;
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

    // For start-anchored patterns
    if (dfa->has_start_anchor) {
        // Try at start of input
        uint16_t state = DFA_START_STATE;

        if (states[state].flags & DFA_FLAG_MATCH) {
            return true;
        }

        for (size_t i = 0; i < len; i++) {
            state = states[state].transitions[input[i]];
            if (state == DFA_DEAD_STATE) {
                // Reset at newlines for ^ anchor
                if (input[i] == '\n') {
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

    // Unanchored search - optimized single pass
    // We track if we're "in a potential match" and reset on dead state
    for (size_t start = 0; start <= len; start++) {
        uint16_t state = DFA_START_STATE;

        // Check for immediate match
        if (states[state].flags & DFA_FLAG_MATCH) {
            return true;
        }

        const uint8_t *p = input + start;
        const uint8_t *end = input + len;

        while (p < end) {
            state = states[state].transitions[*p++];

            if (state == DFA_DEAD_STATE) {
                break;
            }

            if (states[state].flags & DFA_FLAG_MATCH) {
                return true;
            }
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
