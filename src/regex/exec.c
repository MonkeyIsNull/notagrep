#include "exec.h"
#include <stdlib.h>
#include <string.h>

#define NO_STATE UINT32_MAX

// =============================================================================
// Execution context
// =============================================================================

int exec_context_init(ExecContext *ctx, size_t capacity) {
    memset(ctx, 0, sizeof(*ctx));

    ctx->current = malloc(capacity * sizeof(uint32_t));
    ctx->next = malloc(capacity * sizeof(uint32_t));
    ctx->capacity = capacity;

    // Generation-based seen tracking (one uint32_t per state)
    ctx->seen_size = capacity;
    ctx->seen = calloc(capacity, sizeof(uint32_t));
    ctx->generation = 1;  // Start at 1 so 0 means "never seen"

    if (!ctx->current || !ctx->next || !ctx->seen) {
        exec_context_free(ctx);
        return -1;
    }

    return 0;
}

void exec_context_free(ExecContext *ctx) {
    free(ctx->current);
    free(ctx->next);
    free(ctx->seen);
    memset(ctx, 0, sizeof(*ctx));
}

// =============================================================================
// NFA simulation helpers
// =============================================================================

// Generation-based seen tracking - O(1) "clear" by incrementing generation
static inline bool seen_test(ExecContext *ctx, uint32_t state) {
    return ctx->seen[state] == ctx->generation;
}

static inline void seen_set(ExecContext *ctx, uint32_t state) {
    ctx->seen[state] = ctx->generation;
}

static inline void seen_clear(ExecContext *ctx) {
    ctx->generation++;
    // Handle overflow by resetting all
    if (ctx->generation == 0) {
        memset(ctx->seen, 0, ctx->seen_size * sizeof(uint32_t));
        ctx->generation = 1;
    }
}

// Add a state to the set, following epsilon transitions
static void add_state(const Nfa *nfa, ExecContext *ctx,
                      uint32_t *set, size_t *count, uint32_t state,
                      const uint8_t *input, size_t input_len, size_t pos) {
    if (state == NO_STATE) return;
    if (seen_test(ctx, state)) return;

    seen_set(ctx, state);
    const NfaState *s = &nfa->states[state];

    switch (s->type) {
        case NFA_SPLIT:
            // Epsilon transitions: follow both branches
            add_state(nfa, ctx, set, count, s->out1, input, input_len, pos);
            add_state(nfa, ctx, set, count, s->out2, input, input_len, pos);
            break;

        case NFA_ANCHOR_START:
            // Match at start of input or after newline
            if (pos == 0 || (pos > 0 && input[pos - 1] == '\n')) {
                add_state(nfa, ctx, set, count, s->out1, input, input_len, pos);
            }
            break;

        case NFA_ANCHOR_END:
            // Match at end of input or before newline
            if (pos == input_len || (pos < input_len && input[pos] == '\n')) {
                add_state(nfa, ctx, set, count, s->out1, input, input_len, pos);
            }
            break;

        default:
            // Non-epsilon state: add to set
            set[(*count)++] = state;
            break;
    }
}

// Test if a byte matches a character class
static inline bool class_matches(const NfaState *s, uint8_t byte) {
    bool in_set = (s->data.char_class.bitmap[byte / 8] & (1 << (byte % 8))) != 0;
    return s->data.char_class.negated ? !in_set : in_set;
}

// =============================================================================
// NFA simulation
// =============================================================================

bool nfa_match_at(const Nfa *nfa, ExecContext *ctx,
                  const uint8_t *input, size_t input_len,
                  size_t start_pos, Match *match) {
    size_t current_count = 0;
    size_t next_count = 0;

    // Clear seen (O(1) with generation counter) and add start state
    seen_clear(ctx);
    add_state(nfa, ctx, ctx->current, &current_count, nfa->start, input, input_len, start_pos);

    // Track if we've reached a match state and where
    bool matched = false;
    size_t match_end = 0;

    // Check for immediate match (empty pattern)
    for (size_t i = 0; i < current_count; i++) {
        if (nfa->states[ctx->current[i]].type == NFA_MATCH) {
            matched = true;
            match_end = start_pos;
            break;
        }
    }

    // Process each byte
    for (size_t pos = start_pos; pos < input_len && current_count > 0; pos++) {
        uint8_t byte = input[pos];
        next_count = 0;

        // Clear seen (O(1) with generation counter)
        seen_clear(ctx);

        // Process each current state
        for (size_t i = 0; i < current_count; i++) {
            uint32_t state_idx = ctx->current[i];
            const NfaState *s = &nfa->states[state_idx];

            bool advances = false;

            switch (s->type) {
                case NFA_BYTE:
                    advances = (byte == s->data.byte);
                    break;

                case NFA_CLASS:
                    advances = class_matches(s, byte);
                    break;

                case NFA_ANY:
                    advances = true;
                    break;

                default:
                    break;
            }

            if (advances) {
                add_state(nfa, ctx, ctx->next, &next_count, s->out1,
                          input, input_len, pos + 1);
            }
        }

        // Swap current and next
        uint32_t *tmp = ctx->current;
        ctx->current = ctx->next;
        ctx->next = tmp;
        current_count = next_count;

        // Check for match state
        for (size_t i = 0; i < current_count; i++) {
            if (nfa->states[ctx->current[i]].type == NFA_MATCH) {
                matched = true;
                match_end = pos + 1;
                // Don't break - continue to find longest match
            }
        }
    }

    if (matched && match) {
        match->start = start_pos;
        match->end = match_end;
    }

    return matched;
}

bool nfa_find_first(const Nfa *nfa, ExecContext *ctx,
                    const uint8_t *input, size_t input_len,
                    Match *match) {
    // Try matching at each position
    for (size_t pos = 0; pos <= input_len; pos++) {
        if (nfa_match_at(nfa, ctx, input, input_len, pos, match)) {
            return true;
        }
    }
    return false;
}

size_t nfa_find_all(const Nfa *nfa, ExecContext *ctx,
                    const uint8_t *input, size_t input_len,
                    match_callback cb, void *user_data) {
    size_t count = 0;
    size_t pos = 0;

    while (pos <= input_len) {
        Match m;
        if (nfa_match_at(nfa, ctx, input, input_len, pos, &m)) {
            count++;
            if (cb) cb(&m, user_data);

            // Advance past this match (at least one byte to avoid infinite loop)
            if (m.end > pos) {
                pos = m.end;
            } else {
                pos++;
            }
        } else {
            pos++;
        }
    }

    return count;
}

bool nfa_contains(const Nfa *nfa, ExecContext *ctx,
                  const uint8_t *input, size_t input_len) {
    return nfa_find_first(nfa, ctx, input, input_len, NULL);
}
