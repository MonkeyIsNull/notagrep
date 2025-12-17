#ifndef NOTAGREP_REGEX_DFA_H
#define NOTAGREP_REGEX_DFA_H

#include "nfa.h"
#include "exec.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// DFA state IDs
#define DFA_DEAD_STATE      0       // Dead state - no match possible
#define DFA_START_STATE     1       // Initial state

// Default limits
#define DFA_MAX_STATES_DEFAULT  1024    // 512KB memory
#define DFA_MAX_STATES_LIMIT    8192    // 4MB max

// DFA state flags
#define DFA_FLAG_MATCH      0x01    // Accepting state
#define DFA_FLAG_DEAD       0x02    // Dead end (all transitions to DEAD)

// Single DFA state with dense 256-entry transition table
// Size: 256 * 2 + padding = 514 bytes per state
typedef struct {
    uint16_t transitions[256];  // Next state for each input byte
    uint8_t flags;              // DFA_FLAG_* bits
    uint8_t _padding;
} DfaState;

// Complete DFA structure
typedef struct {
    DfaState *states;           // Array of states (index 0 = dead, 1 = start)
    uint32_t state_count;       // Number of allocated states
    uint32_t state_capacity;    // Capacity of states array
    uint32_t max_states;        // Limit before giving up

    // Anchor handling
    bool has_start_anchor;      // Pattern starts with ^
    bool has_end_anchor;        // Pattern ends with $

    // Build status
    bool is_complete;           // True if fully built (not truncated)
    bool build_failed;          // True if state explosion detected

    // Reference to original NFA (not owned)
    const Nfa *nfa;
} Dfa;

// =============================================================================
// DFA Compilation
// =============================================================================

// Compile a DFA from an NFA using subset construction
// Returns NULL if:
//   - State limit exceeded (use NFA fallback)
//   - Memory allocation failed
// The returned DFA must be freed with dfa_free()
Dfa *dfa_compile(const Nfa *nfa, uint32_t max_states);

// Free a DFA
void dfa_free(Dfa *dfa);

// Check if a pattern is likely to cause DFA state explosion
// This is a heuristic - returns true if DFA compilation should be skipped
bool dfa_will_explode(const Nfa *nfa);

// =============================================================================
// Sheng DFA (SIMD-accelerated for small DFAs)
// =============================================================================

// Maximum states for Sheng DFA (limited by NEON register width)
#define SHENG_MAX_STATES 16

// Sheng DFA uses nybble-based shuffle masks for O(1) state transitions
// Each state has two 16-byte masks: one for low nybble, one for high nybble
// The transition is: next_state = lo_mask[state][byte & 0xF] & hi_mask[state][byte >> 4]
typedef struct {
    uint8_t lo_masks[SHENG_MAX_STATES][16];  // Low nybble lookup per state
    uint8_t hi_masks[SHENG_MAX_STATES][16];  // High nybble lookup per state
    uint16_t accept_mask;                     // Bit i = state i is accepting
    uint8_t state_count;                      // Number of states (including dead)
    bool is_valid;                            // True if Sheng compilation succeeded
} ShengDfa;

// Compile a Sheng DFA from a regular DFA (only for DFAs with <= 16 states)
// Returns true if successful, false if DFA too large
bool sheng_compile(ShengDfa *sheng, const Dfa *dfa);

// Sheng execution functions (SIMD-accelerated)
bool sheng_match_at(const ShengDfa *sheng, const uint8_t *input, size_t len,
                    size_t start, Match *match);
bool sheng_contains(const ShengDfa *sheng, const uint8_t *input, size_t len);

// =============================================================================
// DFA Execution
// =============================================================================

// Match at a specific position (anchored)
// Returns true if match found, sets match->start and match->end
bool dfa_match_at(const Dfa *dfa, const uint8_t *input, size_t len,
                  size_t start, Match *match);

// Find first match anywhere in input (unanchored)
// Returns true if match found
bool dfa_find_first(const Dfa *dfa, const uint8_t *input, size_t len,
                    Match *match);

// Check if any match exists (fast path for -l/-q modes)
bool dfa_contains(const Dfa *dfa, const uint8_t *input, size_t len);

// Count lines with matches (for -c mode)
size_t dfa_count_lines(const Dfa *dfa, const uint8_t *input, size_t len);

// =============================================================================
// Debug
// =============================================================================

// Print DFA info to stderr
void dfa_debug_print(const Dfa *dfa);

#endif // NOTAGREP_REGEX_DFA_H
