#ifndef NOTAGREP_REGEX_EXEC_H
#define NOTAGREP_REGEX_EXEC_H

#include "nfa.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Match result
typedef struct {
    size_t start;   // Start offset of match
    size_t end;     // End offset of match (exclusive)
} Match;

// Execution context (reusable across searches)
typedef struct {
    // State sets for NFA simulation
    uint32_t *current;
    uint32_t *next;
    size_t capacity;

    // Generation-based seen tracking (avoids memset per byte)
    uint32_t *seen;         // Generation number when state was last seen
    size_t seen_size;       // Number of states (not bytes)
    uint32_t generation;    // Current generation counter
} ExecContext;

// Initialize execution context
// capacity is the max number of states (use nfa->count)
int exec_context_init(ExecContext *ctx, size_t capacity);

// Free execution context
void exec_context_free(ExecContext *ctx);

// Check if the NFA matches at a specific position in the input
// Returns true if match found, sets match->end to end position
bool nfa_match_at(const Nfa *nfa, ExecContext *ctx,
                  const uint8_t *input, size_t input_len,
                  size_t start_pos, Match *match);

// Find the first match in the input (unanchored search)
// Returns true if match found
bool nfa_find_first(const Nfa *nfa, ExecContext *ctx,
                    const uint8_t *input, size_t input_len,
                    Match *match);

// Find all matches in the input
// Calls callback for each match
// Returns number of matches found
typedef void (*match_callback)(const Match *match, void *user_data);
size_t nfa_find_all(const Nfa *nfa, ExecContext *ctx,
                    const uint8_t *input, size_t input_len,
                    match_callback cb, void *user_data);

// Check if any match exists (for -l/-q modes)
bool nfa_contains(const Nfa *nfa, ExecContext *ctx,
                  const uint8_t *input, size_t input_len);

#endif // NOTAGREP_REGEX_EXEC_H
