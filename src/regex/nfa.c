#include "nfa.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define NO_STATE UINT32_MAX

// =============================================================================
// NFA construction helpers
// =============================================================================

static Nfa *nfa_new(void) {
    Nfa *nfa = calloc(1, sizeof(Nfa));
    if (!nfa) return NULL;

    nfa->capacity = 64;
    nfa->states = calloc(nfa->capacity, sizeof(NfaState));
    if (!nfa->states) {
        free(nfa);
        return NULL;
    }

    return nfa;
}

void nfa_free(Nfa *nfa) {
    if (nfa) {
        free(nfa->states);
        free(nfa);
    }
}

static uint32_t nfa_add_state(Nfa *nfa, NfaStateType type) {
    if (nfa->count >= nfa->capacity) {
        size_t new_cap = nfa->capacity * 2;
        NfaState *new_states = realloc(nfa->states, new_cap * sizeof(NfaState));
        if (!new_states) return NO_STATE;
        nfa->states = new_states;
        nfa->capacity = new_cap;
    }

    uint32_t idx = (uint32_t)nfa->count++;
    memset(&nfa->states[idx], 0, sizeof(NfaState));
    nfa->states[idx].type = type;
    nfa->states[idx].out1 = NO_STATE;
    nfa->states[idx].out2 = NO_STATE;

    return idx;
}

// Fragment: a partial NFA with dangling out pointers
typedef struct {
    uint32_t start;
    // List of state indices with dangling out1 pointers
    uint32_t *out_list;
    size_t out_count;
    size_t out_capacity;
} Fragment;

static Fragment *frag_new(uint32_t start) {
    Fragment *f = calloc(1, sizeof(Fragment));
    if (!f) return NULL;
    f->start = start;
    f->out_capacity = 4;
    f->out_list = malloc(f->out_capacity * sizeof(uint32_t));
    if (!f->out_list) {
        free(f);
        return NULL;
    }
    return f;
}

static void frag_free(Fragment *f) {
    if (f) {
        free(f->out_list);
        free(f);
    }
}

static void frag_add_out(Fragment *f, uint32_t state_idx) {
    if (f->out_count >= f->out_capacity) {
        size_t new_cap = f->out_capacity * 2;
        uint32_t *new_list = realloc(f->out_list, new_cap * sizeof(uint32_t));
        if (!new_list) return;
        f->out_list = new_list;
        f->out_capacity = new_cap;
    }
    f->out_list[f->out_count++] = state_idx;
}

// Patch all dangling outputs to point to a target state
static void frag_patch(Nfa *nfa, Fragment *f, uint32_t target) {
    for (size_t i = 0; i < f->out_count; i++) {
        uint32_t idx = f->out_list[i];
        if (nfa->states[idx].out1 == NO_STATE) {
            nfa->states[idx].out1 = target;
        } else if (nfa->states[idx].out2 == NO_STATE) {
            nfa->states[idx].out2 = target;
        }
    }
}

// Merge output lists from f2 into f1
static void frag_merge_outs(Fragment *f1, Fragment *f2) {
    for (size_t i = 0; i < f2->out_count; i++) {
        frag_add_out(f1, f2->out_list[i]);
    }
}

// =============================================================================
// Thompson construction
// =============================================================================

static Fragment *compile_node(Nfa *nfa, const AstNode *node);

static Fragment *compile_literal(Nfa *nfa, uint8_t byte) {
    uint32_t s = nfa_add_state(nfa, NFA_BYTE);
    if (s == NO_STATE) return NULL;

    nfa->states[s].data.byte = byte;

    Fragment *f = frag_new(s);
    if (!f) return NULL;
    frag_add_out(f, s);

    return f;
}

static Fragment *compile_any(Nfa *nfa) {
    uint32_t s = nfa_add_state(nfa, NFA_ANY);
    if (s == NO_STATE) return NULL;

    Fragment *f = frag_new(s);
    if (!f) return NULL;
    frag_add_out(f, s);

    return f;
}

static Fragment *compile_class(Nfa *nfa, const CharClass *cc) {
    uint32_t s = nfa_add_state(nfa, NFA_CLASS);
    if (s == NO_STATE) return NULL;

    memcpy(nfa->states[s].data.char_class.bitmap, cc->bits, 32);
    nfa->states[s].data.char_class.negated = cc->negated;

    Fragment *f = frag_new(s);
    if (!f) return NULL;
    frag_add_out(f, s);

    return f;
}

static Fragment *compile_anchor_start(Nfa *nfa) {
    uint32_t s = nfa_add_state(nfa, NFA_ANCHOR_START);
    if (s == NO_STATE) return NULL;

    Fragment *f = frag_new(s);
    if (!f) return NULL;
    frag_add_out(f, s);

    return f;
}

static Fragment *compile_anchor_end(Nfa *nfa) {
    uint32_t s = nfa_add_state(nfa, NFA_ANCHOR_END);
    if (s == NO_STATE) return NULL;

    Fragment *f = frag_new(s);
    if (!f) return NULL;
    frag_add_out(f, s);

    return f;
}

static Fragment *compile_empty(Nfa *nfa) {
    // Empty string: just a split with both branches dangling
    uint32_t s = nfa_add_state(nfa, NFA_SPLIT);
    if (s == NO_STATE) return NULL;

    Fragment *f = frag_new(s);
    if (!f) return NULL;
    frag_add_out(f, s);

    return f;
}

static Fragment *compile_concat(Nfa *nfa, const AstNode *node) {
    if (node->data.list.count == 0) {
        return compile_empty(nfa);
    }

    Fragment *result = compile_node(nfa, node->data.list.children[0]);
    if (!result) return NULL;

    for (size_t i = 1; i < node->data.list.count; i++) {
        Fragment *next = compile_node(nfa, node->data.list.children[i]);
        if (!next) {
            frag_free(result);
            return NULL;
        }

        // Patch result's outputs to next's start
        frag_patch(nfa, result, next->start);

        // Result now has next's outputs
        result->out_count = 0;
        frag_merge_outs(result, next);
        frag_free(next);
    }

    return result;
}

static Fragment *compile_alt(Nfa *nfa, const AstNode *node) {
    if (node->data.list.count == 0) {
        return compile_empty(nfa);
    }

    if (node->data.list.count == 1) {
        return compile_node(nfa, node->data.list.children[0]);
    }

    // Create split state
    uint32_t split = nfa_add_state(nfa, NFA_SPLIT);
    if (split == NO_STATE) return NULL;

    Fragment *result = frag_new(split);
    if (!result) return NULL;

    // Compile first two branches
    Fragment *f1 = compile_node(nfa, node->data.list.children[0]);
    Fragment *f2 = compile_node(nfa, node->data.list.children[1]);
    if (!f1 || !f2) {
        frag_free(f1);
        frag_free(f2);
        frag_free(result);
        return NULL;
    }

    nfa->states[split].out1 = f1->start;
    nfa->states[split].out2 = f2->start;

    frag_merge_outs(result, f1);
    frag_merge_outs(result, f2);
    frag_free(f1);
    frag_free(f2);

    // Handle additional branches by chaining splits
    for (size_t i = 2; i < node->data.list.count; i++) {
        uint32_t new_split = nfa_add_state(nfa, NFA_SPLIT);
        if (new_split == NO_STATE) {
            frag_free(result);
            return NULL;
        }

        Fragment *fi = compile_node(nfa, node->data.list.children[i]);
        if (!fi) {
            frag_free(result);
            return NULL;
        }

        // Chain: new_split -> (previous split, new branch)
        nfa->states[new_split].out1 = result->start;
        nfa->states[new_split].out2 = fi->start;

        result->start = new_split;
        frag_merge_outs(result, fi);
        frag_free(fi);
    }

    return result;
}

static Fragment *compile_quest(Nfa *nfa, const AstNode *node) {
    Fragment *child = compile_node(nfa, node->data.child);
    if (!child) return NULL;

    // Split: try child or skip
    uint32_t split = nfa_add_state(nfa, NFA_SPLIT);
    if (split == NO_STATE) {
        frag_free(child);
        return NULL;
    }

    nfa->states[split].out1 = child->start;
    // out2 remains dangling (skip path)

    Fragment *result = frag_new(split);
    if (!result) {
        frag_free(child);
        return NULL;
    }

    frag_merge_outs(result, child);
    frag_add_out(result, split);  // The skip path

    frag_free(child);
    return result;
}

static Fragment *compile_star(Nfa *nfa, const AstNode *node) {
    Fragment *child = compile_node(nfa, node->data.child);
    if (!child) return NULL;

    // Split: try child or skip
    uint32_t split = nfa_add_state(nfa, NFA_SPLIT);
    if (split == NO_STATE) {
        frag_free(child);
        return NULL;
    }

    nfa->states[split].out1 = child->start;
    // out2 remains dangling (exit path)

    // Patch child's outputs back to split (loop)
    frag_patch(nfa, child, split);

    Fragment *result = frag_new(split);
    if (!result) {
        frag_free(child);
        return NULL;
    }

    frag_add_out(result, split);  // The exit path

    frag_free(child);
    return result;
}

static Fragment *compile_plus(Nfa *nfa, const AstNode *node) {
    Fragment *child = compile_node(nfa, node->data.child);
    if (!child) return NULL;

    // Split after child: loop back or exit
    uint32_t split = nfa_add_state(nfa, NFA_SPLIT);
    if (split == NO_STATE) {
        frag_free(child);
        return NULL;
    }

    nfa->states[split].out1 = child->start;  // Loop back
    // out2 remains dangling (exit path)

    // Patch child's outputs to split
    frag_patch(nfa, child, split);

    Fragment *result = frag_new(child->start);
    if (!result) {
        frag_free(child);
        return NULL;
    }

    frag_add_out(result, split);  // The exit path

    frag_free(child);
    return result;
}

static Fragment *compile_repeat(Nfa *nfa, const AstNode *node) {
    int min = node->data.repeat.min;
    int max = node->data.repeat.max;

    // Special cases
    if (max == 0) {
        return compile_empty(nfa);
    }

    Fragment *result = NULL;

    // Build min required copies
    for (int i = 0; i < min; i++) {
        Fragment *copy = compile_node(nfa, node->data.repeat.child);
        if (!copy) {
            frag_free(result);
            return NULL;
        }

        if (result) {
            frag_patch(nfa, result, copy->start);
            result->out_count = 0;
            frag_merge_outs(result, copy);
            frag_free(copy);
        } else {
            result = copy;
        }
    }

    // Build optional copies (up to max)
    if (max < 0) {
        // Unbounded: add a* after min copies
        Fragment *star_child = compile_node(nfa, node->data.repeat.child);
        if (!star_child) {
            frag_free(result);
            return NULL;
        }

        uint32_t split = nfa_add_state(nfa, NFA_SPLIT);
        if (split == NO_STATE) {
            frag_free(result);
            frag_free(star_child);
            return NULL;
        }

        nfa->states[split].out1 = star_child->start;
        frag_patch(nfa, star_child, split);

        if (result) {
            frag_patch(nfa, result, split);
            result->out_count = 0;
            frag_add_out(result, split);
        } else {
            result = frag_new(split);
            if (!result) {
                frag_free(star_child);
                return NULL;
            }
            frag_add_out(result, split);
        }

        frag_free(star_child);
    } else {
        // Bounded: add (max - min) optional copies
        for (int i = min; i < max; i++) {
            Fragment *copy = compile_node(nfa, node->data.repeat.child);
            if (!copy) {
                frag_free(result);
                return NULL;
            }

            uint32_t split = nfa_add_state(nfa, NFA_SPLIT);
            if (split == NO_STATE) {
                frag_free(result);
                frag_free(copy);
                return NULL;
            }

            nfa->states[split].out1 = copy->start;
            // out2 is skip path

            if (result) {
                frag_patch(nfa, result, split);
                result->out_count = 0;
                frag_merge_outs(result, copy);
                frag_add_out(result, split);
            } else {
                result = frag_new(split);
                if (!result) {
                    frag_free(copy);
                    return NULL;
                }
                frag_merge_outs(result, copy);
                frag_add_out(result, split);
            }

            frag_free(copy);
        }
    }

    if (!result) {
        result = compile_empty(nfa);
    }

    return result;
}

static Fragment *compile_node(Nfa *nfa, const AstNode *node) {
    if (!node) return compile_empty(nfa);

    switch (node->type) {
        case AST_LITERAL:
            return compile_literal(nfa, node->data.literal);

        case AST_DOT:
            return compile_any(nfa);

        case AST_CLASS:
            return compile_class(nfa, node->data.char_class);

        case AST_CONCAT:
            return compile_concat(nfa, node);

        case AST_ALT:
            return compile_alt(nfa, node);

        case AST_QUEST:
            return compile_quest(nfa, node);

        case AST_STAR:
            return compile_star(nfa, node);

        case AST_PLUS:
            return compile_plus(nfa, node);

        case AST_REPEAT:
            return compile_repeat(nfa, node);

        case AST_GROUP:
            return compile_node(nfa, node->data.child);

        case AST_ANCHOR_START:
            return compile_anchor_start(nfa);

        case AST_ANCHOR_END:
            return compile_anchor_end(nfa);

        case AST_EMPTY:
            return compile_empty(nfa);
    }

    return NULL;
}

// =============================================================================
// Public API
// =============================================================================

Nfa *nfa_compile(const AstNode *ast) {
    Nfa *nfa = nfa_new();
    if (!nfa) return NULL;

    Fragment *frag = compile_node(nfa, ast);
    if (!frag) {
        nfa_free(nfa);
        return NULL;
    }

    // Add match state
    uint32_t match = nfa_add_state(nfa, NFA_MATCH);
    if (match == NO_STATE) {
        frag_free(frag);
        nfa_free(nfa);
        return NULL;
    }

    // Patch fragment outputs to match state
    frag_patch(nfa, frag, match);

    nfa->start = frag->start;
    nfa->match = match;

    frag_free(frag);
    return nfa;
}

// =============================================================================
// Debug printing
// =============================================================================

void nfa_debug_print(const Nfa *nfa) {
    fprintf(stderr, "NFA (%zu states, start=%u, match=%u)\n",
            nfa->count, nfa->start, nfa->match);

    for (size_t i = 0; i < nfa->count; i++) {
        const NfaState *s = &nfa->states[i];
        fprintf(stderr, "  [%zu] ", i);

        switch (s->type) {
            case NFA_MATCH:
                fprintf(stderr, "MATCH\n");
                break;

            case NFA_SPLIT:
                fprintf(stderr, "SPLIT -> %u, %u\n", s->out1, s->out2);
                break;

            case NFA_BYTE:
                if (s->data.byte >= 32 && s->data.byte < 127) {
                    fprintf(stderr, "BYTE '%c' -> %u\n", s->data.byte, s->out1);
                } else {
                    fprintf(stderr, "BYTE 0x%02x -> %u\n", s->data.byte, s->out1);
                }
                break;

            case NFA_CLASS:
                fprintf(stderr, "CLASS%s -> %u\n",
                        s->data.char_class.negated ? " (negated)" : "", s->out1);
                break;

            case NFA_ANY:
                fprintf(stderr, "ANY -> %u\n", s->out1);
                break;

            case NFA_ANCHOR_START:
                fprintf(stderr, "ANCHOR_START -> %u\n", s->out1);
                break;

            case NFA_ANCHOR_END:
                fprintf(stderr, "ANCHOR_END -> %u\n", s->out1);
                break;
        }
    }
}
