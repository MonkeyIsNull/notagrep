#ifndef NOTAGREP_REGEX_NFA_H
#define NOTAGREP_REGEX_NFA_H

#include "ast.h"
#include <stdint.h>
#include <stdbool.h>

// NFA state types
typedef enum {
    NFA_MATCH,      // Match state (accept)
    NFA_SPLIT,      // Split (epsilon transitions to two states)
    NFA_BYTE,       // Match single byte
    NFA_CLASS,      // Match character class
    NFA_ANY,        // Match any byte (for . in bytes mode)
    NFA_ANCHOR_START, // ^ anchor (match at line start)
    NFA_ANCHOR_END,   // $ anchor (match at line end)
} NfaStateType;

// NFA state
typedef struct {
    NfaStateType type;

    union {
        // NFA_BYTE
        uint8_t byte;

        // NFA_CLASS
        struct {
            uint8_t bitmap[32];  // 256-bit bitmap
            bool negated;
        } char_class;
    } data;

    // Transitions (state indices, UINT32_MAX = no transition)
    uint32_t out1;
    uint32_t out2;  // Only used for NFA_SPLIT
} NfaState;

// Complete NFA
typedef struct {
    NfaState *states;
    size_t count;
    size_t capacity;
    uint32_t start;   // Start state index
    uint32_t match;   // Match state index
} Nfa;

// Build an NFA from an AST
// Returns NULL on error
Nfa *nfa_compile(const AstNode *ast);

// Free an NFA
void nfa_free(Nfa *nfa);

// Debug: print NFA to stderr
void nfa_debug_print(const Nfa *nfa);

#endif // NOTAGREP_REGEX_NFA_H
