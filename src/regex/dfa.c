#include "dfa.h"
#include <stdlib.h>
#include <string.h>

#define NO_STATE UINT32_MAX

// =============================================================================
// NFA State Set (for subset construction)
// =============================================================================

// A set of NFA states represented as a sorted array
typedef struct {
    uint32_t *states;
    size_t count;
    size_t capacity;
} NfaStateSet;

static NfaStateSet *stateset_create(size_t capacity) {
    NfaStateSet *set = malloc(sizeof(NfaStateSet));
    if (!set) return NULL;

    set->states = malloc(capacity * sizeof(uint32_t));
    if (!set->states) {
        free(set);
        return NULL;
    }

    set->count = 0;
    set->capacity = capacity;
    return set;
}

static void stateset_free(NfaStateSet *set) {
    if (set) {
        free(set->states);
        free(set);
    }
}

static void stateset_clear(NfaStateSet *set) {
    set->count = 0;
}

// Add state to set, maintaining sorted order
static bool stateset_add(NfaStateSet *set, uint32_t state) {
    // Binary search for insertion point
    size_t lo = 0, hi = set->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (set->states[mid] < state) {
            lo = mid + 1;
        } else if (set->states[mid] > state) {
            hi = mid;
        } else {
            return false;  // Already present
        }
    }

    // Grow if needed
    if (set->count >= set->capacity) {
        size_t new_cap = set->capacity * 2;
        uint32_t *new_states = realloc(set->states, new_cap * sizeof(uint32_t));
        if (!new_states) return false;
        set->states = new_states;
        set->capacity = new_cap;
    }

    // Insert at position lo
    memmove(&set->states[lo + 1], &set->states[lo],
            (set->count - lo) * sizeof(uint32_t));
    set->states[lo] = state;
    set->count++;
    return true;
}

// Check if sets are equal (they're sorted, so simple compare)
static bool stateset_equal(const NfaStateSet *a, const NfaStateSet *b) {
    if (a->count != b->count) return false;
    return memcmp(a->states, b->states, a->count * sizeof(uint32_t)) == 0;
}

// Hash a state set for lookup
static uint32_t stateset_hash(const NfaStateSet *set) {
    uint32_t hash = 5381;
    for (size_t i = 0; i < set->count; i++) {
        hash = ((hash << 5) + hash) ^ set->states[i];
    }
    return hash;
}

// =============================================================================
// DFA State Map (maps NFA state sets to DFA state indices)
// =============================================================================

typedef struct DfaStateEntry {
    NfaStateSet *nfa_set;       // The NFA state set
    uint32_t dfa_state;         // Corresponding DFA state index
    struct DfaStateEntry *next; // Hash chain
} DfaStateEntry;

typedef struct {
    DfaStateEntry **buckets;
    size_t bucket_count;
    size_t entry_count;
} DfaStateMap;

static DfaStateMap *statemap_create(size_t bucket_count) {
    DfaStateMap *map = malloc(sizeof(DfaStateMap));
    if (!map) return NULL;

    map->buckets = calloc(bucket_count, sizeof(DfaStateEntry *));
    if (!map->buckets) {
        free(map);
        return NULL;
    }

    map->bucket_count = bucket_count;
    map->entry_count = 0;
    return map;
}

static void statemap_free(DfaStateMap *map) {
    if (!map) return;

    for (size_t i = 0; i < map->bucket_count; i++) {
        DfaStateEntry *entry = map->buckets[i];
        while (entry) {
            DfaStateEntry *next = entry->next;
            stateset_free(entry->nfa_set);
            free(entry);
            entry = next;
        }
    }

    free(map->buckets);
    free(map);
}

// Look up an NFA state set, return DFA state index or UINT32_MAX if not found
static uint32_t statemap_lookup(DfaStateMap *map, const NfaStateSet *set) {
    uint32_t hash = stateset_hash(set) % map->bucket_count;

    for (DfaStateEntry *e = map->buckets[hash]; e; e = e->next) {
        if (stateset_equal(e->nfa_set, set)) {
            return e->dfa_state;
        }
    }

    return UINT32_MAX;
}

// Insert a new mapping (copies the state set)
static bool statemap_insert(DfaStateMap *map, const NfaStateSet *set, uint32_t dfa_state) {
    // Create a copy of the state set
    NfaStateSet *copy = stateset_create(set->count > 0 ? set->count : 1);
    if (!copy) return false;

    memcpy(copy->states, set->states, set->count * sizeof(uint32_t));
    copy->count = set->count;

    // Create entry
    DfaStateEntry *entry = malloc(sizeof(DfaStateEntry));
    if (!entry) {
        stateset_free(copy);
        return false;
    }

    entry->nfa_set = copy;
    entry->dfa_state = dfa_state;

    // Insert into bucket
    uint32_t hash = stateset_hash(set) % map->bucket_count;
    entry->next = map->buckets[hash];
    map->buckets[hash] = entry;
    map->entry_count++;

    return true;
}

// =============================================================================
// Epsilon Closure
// =============================================================================

// Temporary seen array for epsilon closure (reused)
typedef struct {
    uint8_t *seen;
    size_t capacity;
} EpsilonContext;

static bool epsilon_ctx_init(EpsilonContext *ctx, size_t nfa_state_count) {
    ctx->seen = calloc(nfa_state_count, sizeof(uint8_t));
    ctx->capacity = nfa_state_count;
    return ctx->seen != NULL;
}

static void epsilon_ctx_free(EpsilonContext *ctx) {
    free(ctx->seen);
    ctx->seen = NULL;
}

static void epsilon_ctx_clear(EpsilonContext *ctx) {
    memset(ctx->seen, 0, ctx->capacity);
}

// Add state and follow epsilon transitions
// For DFA construction, we don't have actual input position, so anchors are handled specially
static void epsilon_closure_add(const Nfa *nfa, EpsilonContext *ctx,
                                NfaStateSet *set, uint32_t state,
                                bool at_start, bool *has_start_anchor, bool *has_end_anchor) {
    if (state == NO_STATE) return;
    if (state >= ctx->capacity) return;
    if (ctx->seen[state]) return;

    ctx->seen[state] = 1;
    const NfaState *s = &nfa->states[state];

    switch (s->type) {
        case NFA_SPLIT:
            // Follow both epsilon transitions
            epsilon_closure_add(nfa, ctx, set, s->out1, at_start, has_start_anchor, has_end_anchor);
            epsilon_closure_add(nfa, ctx, set, s->out2, at_start, has_start_anchor, has_end_anchor);
            break;

        case NFA_ANCHOR_START:
            // Record that we have a start anchor
            if (has_start_anchor) *has_start_anchor = true;
            // Follow the transition (anchor is a zero-width assertion)
            epsilon_closure_add(nfa, ctx, set, s->out1, at_start, has_start_anchor, has_end_anchor);
            break;

        case NFA_ANCHOR_END:
            // Record that we have an end anchor
            if (has_end_anchor) *has_end_anchor = true;
            // Follow the transition
            epsilon_closure_add(nfa, ctx, set, s->out1, at_start, has_start_anchor, has_end_anchor);
            break;

        default:
            // Non-epsilon state: add to set
            stateset_add(set, state);
            break;
    }
}

// Compute epsilon closure with anchor tracking
static void epsilon_closure_with_anchors(const Nfa *nfa, EpsilonContext *ctx, NfaStateSet *set,
                                          bool *has_start_anchor, bool *has_end_anchor) {
    epsilon_ctx_clear(ctx);

    size_t original_count = set->count;
    uint32_t *original = malloc(original_count * sizeof(uint32_t));
    if (!original) return;

    memcpy(original, set->states, original_count * sizeof(uint32_t));
    stateset_clear(set);

    for (size_t i = 0; i < original_count; i++) {
        epsilon_closure_add(nfa, ctx, set, original[i], false, has_start_anchor, has_end_anchor);
    }

    free(original);
}

// =============================================================================
// DFA Compilation
// =============================================================================

// Work queue entry
typedef struct {
    uint32_t dfa_state;
    NfaStateSet *nfa_set;  // Borrowed, owned by state map
} WorkItem;

typedef struct {
    WorkItem *items;
    size_t head;
    size_t tail;
    size_t capacity;
} WorkQueue;

static bool workqueue_init(WorkQueue *q, size_t capacity) {
    q->items = malloc(capacity * sizeof(WorkItem));
    q->head = 0;
    q->tail = 0;
    q->capacity = capacity;
    return q->items != NULL;
}

static void workqueue_free(WorkQueue *q) {
    free(q->items);
}

static bool workqueue_empty(WorkQueue *q) {
    return q->head == q->tail;
}

static bool workqueue_push(WorkQueue *q, uint32_t dfa_state, NfaStateSet *nfa_set) {
    size_t next = (q->tail + 1) % q->capacity;
    if (next == q->head) {
        // Queue full, grow it
        size_t new_cap = q->capacity * 2;
        WorkItem *new_items = malloc(new_cap * sizeof(WorkItem));
        if (!new_items) return false;

        // Copy items in order
        size_t count = 0;
        for (size_t i = q->head; i != q->tail; i = (i + 1) % q->capacity) {
            new_items[count++] = q->items[i];
        }

        free(q->items);
        q->items = new_items;
        q->head = 0;
        q->tail = count;
        q->capacity = new_cap;
        next = q->tail + 1;
    }

    q->items[q->tail].dfa_state = dfa_state;
    q->items[q->tail].nfa_set = nfa_set;
    q->tail = next;
    return true;
}

static WorkItem workqueue_pop(WorkQueue *q) {
    WorkItem item = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    return item;
}

// Test if a byte matches an NFA state
static inline bool nfa_state_matches(const NfaState *s, uint8_t byte) {
    switch (s->type) {
        case NFA_BYTE:
            return byte == s->data.byte;

        case NFA_CLASS: {
            bool in_set = (s->data.char_class.bitmap[byte / 8] & (1 << (byte % 8))) != 0;
            return s->data.char_class.negated ? !in_set : in_set;
        }

        case NFA_ANY:
            return true;

        default:
            return false;
    }
}

// Check if a state set contains the NFA match state
static bool stateset_is_match(const Nfa *nfa, const NfaStateSet *set) {
    for (size_t i = 0; i < set->count; i++) {
        if (nfa->states[set->states[i]].type == NFA_MATCH) {
            return true;
        }
    }
    return false;
}

// Allocate a new DFA state
static uint32_t dfa_alloc_state(Dfa *dfa) {
    if (dfa->state_count >= dfa->state_capacity) {
        size_t new_cap = dfa->state_capacity * 2;
        if (new_cap > dfa->max_states) {
            new_cap = dfa->max_states;
        }
        if (dfa->state_count >= new_cap) {
            return UINT32_MAX;  // State limit reached
        }

        DfaState *new_states = realloc(dfa->states, new_cap * sizeof(DfaState));
        if (!new_states) return UINT32_MAX;

        dfa->states = new_states;
        dfa->state_capacity = new_cap;
    }

    uint32_t idx = dfa->state_count++;
    memset(&dfa->states[idx], 0, sizeof(DfaState));
    return idx;
}

Dfa *dfa_compile(const Nfa *nfa, uint32_t max_states) {
    if (!nfa || nfa->count == 0) return NULL;

    // Allocate DFA
    Dfa *dfa = calloc(1, sizeof(Dfa));
    if (!dfa) return NULL;

    dfa->max_states = max_states > 0 ? max_states : DFA_MAX_STATES_DEFAULT;
    if (dfa->max_states > DFA_MAX_STATES_LIMIT) {
        dfa->max_states = DFA_MAX_STATES_LIMIT;
    }

    // Start with initial capacity
    dfa->state_capacity = 64;
    if (dfa->state_capacity > dfa->max_states) {
        dfa->state_capacity = dfa->max_states;
    }

    dfa->states = malloc(dfa->state_capacity * sizeof(DfaState));
    if (!dfa->states) {
        free(dfa);
        return NULL;
    }

    dfa->nfa = nfa;

    // Create helper structures
    EpsilonContext eps_ctx;
    if (!epsilon_ctx_init(&eps_ctx, nfa->count)) {
        free(dfa->states);
        free(dfa);
        return NULL;
    }

    DfaStateMap *state_map = statemap_create(256);
    if (!state_map) {
        epsilon_ctx_free(&eps_ctx);
        free(dfa->states);
        free(dfa);
        return NULL;
    }

    WorkQueue queue;
    if (!workqueue_init(&queue, 64)) {
        statemap_free(state_map);
        epsilon_ctx_free(&eps_ctx);
        free(dfa->states);
        free(dfa);
        return NULL;
    }

    NfaStateSet *current_set = stateset_create(nfa->count);
    NfaStateSet *next_set = stateset_create(nfa->count);
    if (!current_set || !next_set) {
        stateset_free(current_set);
        stateset_free(next_set);
        workqueue_free(&queue);
        statemap_free(state_map);
        epsilon_ctx_free(&eps_ctx);
        free(dfa->states);
        free(dfa);
        return NULL;
    }

    // State 0 is always the dead state
    uint32_t dead_state = dfa_alloc_state(dfa);
    dfa->states[dead_state].flags = DFA_FLAG_DEAD;
    for (int i = 0; i < 256; i++) {
        dfa->states[dead_state].transitions[i] = DFA_DEAD_STATE;
    }

    // Compute initial state (epsilon closure of NFA start)
    epsilon_ctx_clear(&eps_ctx);
    stateset_clear(current_set);
    epsilon_closure_add(nfa, &eps_ctx, current_set, nfa->start, true,
                        &dfa->has_start_anchor, &dfa->has_end_anchor);

    // Create start state
    uint32_t start_state = dfa_alloc_state(dfa);
    if (start_state == UINT32_MAX) {
        dfa->build_failed = true;
        goto cleanup;
    }

    if (stateset_is_match(nfa, current_set)) {
        dfa->states[start_state].flags |= DFA_FLAG_MATCH;
    }

    statemap_insert(state_map, current_set, start_state);
    workqueue_push(&queue, start_state, NULL);

    // Store initial set for first iteration
    NfaStateSet *start_set = stateset_create(current_set->count);
    if (!start_set) {
        dfa->build_failed = true;
        goto cleanup;
    }
    memcpy(start_set->states, current_set->states, current_set->count * sizeof(uint32_t));
    start_set->count = current_set->count;

    // Main subset construction loop
    bool first = true;
    while (!workqueue_empty(&queue)) {
        WorkItem item = workqueue_pop(&queue);

        // Get the NFA state set for this DFA state
        NfaStateSet *src_set;
        if (first) {
            src_set = start_set;
            first = false;
        } else {
            // Look up in map - we need to find the entry
            for (size_t bucket = 0; bucket < state_map->bucket_count; bucket++) {
                for (DfaStateEntry *e = state_map->buckets[bucket]; e; e = e->next) {
                    if (e->dfa_state == item.dfa_state) {
                        src_set = e->nfa_set;
                        goto found;
                    }
                }
            }
            continue;  // Shouldn't happen
            found:;
        }

        // Compute transitions for each possible byte
        for (int byte = 0; byte < 256; byte++) {
            stateset_clear(next_set);

            // For each NFA state in the current set
            for (size_t i = 0; i < src_set->count; i++) {
                uint32_t nfa_state = src_set->states[i];
                const NfaState *s = &nfa->states[nfa_state];

                // If this state matches the byte, add the target to next_set
                if (nfa_state_matches(s, (uint8_t)byte)) {
                    stateset_add(next_set, s->out1);
                }
            }

            // Compute epsilon closure of next_set (tracking anchors)
            bool next_has_end_anchor = false;
            epsilon_closure_with_anchors(nfa, &eps_ctx, next_set, NULL, &next_has_end_anchor);
            if (next_has_end_anchor) {
                dfa->has_end_anchor = true;
            }

            // Look up or create the DFA state for this set
            uint32_t target_state;
            if (next_set->count == 0) {
                target_state = DFA_DEAD_STATE;
            } else {
                target_state = statemap_lookup(state_map, next_set);
                if (target_state == UINT32_MAX) {
                    // New state needed
                    target_state = dfa_alloc_state(dfa);
                    if (target_state == UINT32_MAX) {
                        dfa->build_failed = true;
                        stateset_free(start_set);
                        goto cleanup;
                    }

                    if (stateset_is_match(nfa, next_set)) {
                        dfa->states[target_state].flags |= DFA_FLAG_MATCH;
                    }

                    statemap_insert(state_map, next_set, target_state);
                    workqueue_push(&queue, target_state, NULL);
                }
            }

            dfa->states[item.dfa_state].transitions[byte] = (uint16_t)target_state;
        }
    }

    stateset_free(start_set);
    dfa->is_complete = true;

cleanup:
    stateset_free(current_set);
    stateset_free(next_set);
    workqueue_free(&queue);
    statemap_free(state_map);
    epsilon_ctx_free(&eps_ctx);

    if (dfa->build_failed) {
        dfa_free(dfa);
        return NULL;
    }

    return dfa;
}

void dfa_free(Dfa *dfa) {
    if (!dfa) return;
    free(dfa->states);
    free(dfa);
}

// =============================================================================
// State Explosion Detection
// =============================================================================

bool dfa_will_explode(const Nfa *nfa) {
    if (!nfa) return true;

    // Heuristic: if NFA has too many states, DFA likely to explode
    if (nfa->count > 100) return true;

    // Count number of NFA_ANY states (dots) - can cause explosion
    size_t any_count = 0;
    size_t split_count = 0;

    for (size_t i = 0; i < nfa->count; i++) {
        switch (nfa->states[i].type) {
            case NFA_ANY:
                any_count++;
                break;
            case NFA_SPLIT:
                split_count++;
                break;
            default:
                break;
        }
    }

    // Many dots with splits (like .*.*.*) will explode
    if (any_count > 3 && split_count > 3) return true;

    // Very many splits (nested alternations/quantifiers)
    if (split_count > 20) return true;

    return false;
}

// =============================================================================
// Debug
// =============================================================================

#include <stdio.h>

void dfa_debug_print(const Dfa *dfa) {
    if (!dfa) {
        fprintf(stderr, "DFA: NULL\n");
        return;
    }

    fprintf(stderr, "DFA: %u states (capacity %u, max %u)\n",
            dfa->state_count, dfa->state_capacity, dfa->max_states);
    fprintf(stderr, "  complete: %s, failed: %s\n",
            dfa->is_complete ? "yes" : "no",
            dfa->build_failed ? "yes" : "no");
    fprintf(stderr, "  anchors: start=%s, end=%s\n",
            dfa->has_start_anchor ? "yes" : "no",
            dfa->has_end_anchor ? "yes" : "no");

    // Print transition counts per state
    for (uint32_t i = 0; i < dfa->state_count && i < 10; i++) {
        const DfaState *s = &dfa->states[i];
        int non_dead = 0;
        for (int j = 0; j < 256; j++) {
            if (s->transitions[j] != DFA_DEAD_STATE) non_dead++;
        }
        fprintf(stderr, "  state %u: flags=%02x, %d non-dead transitions\n",
                i, s->flags, non_dead);
    }
    if (dfa->state_count > 10) {
        fprintf(stderr, "  ... and %u more states\n", dfa->state_count - 10);
    }
}
