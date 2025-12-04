// Clean-room Aho-Corasick implementation
// Based on the algorithm from "Efficient String Matching: An Aid to
// Bibliographic Search" by Aho and Corasick (1975)

#include "ahocorasick.h"
#include <stdlib.h>
#include <string.h>

// Trie node structure
typedef struct ACNode {
    struct ACNode *children[256];  // Child nodes for each byte
    struct ACNode *fail;           // Failure link (Aho-Corasick)
    int *outputs;                  // Array of pattern IDs that end here
    int output_count;              // Number of outputs
    int output_capacity;           // Allocated capacity
    int depth;                     // Depth from root
} ACNode;

// Main automaton structure
struct AhoCorasick {
    ACNode *root;                  // Root of the trie
    ACNode **all_nodes;            // Array of all allocated nodes (for cleanup)
    size_t node_count;             // Number of nodes
    size_t node_capacity;          // Allocated capacity

    uint8_t *patterns;             // Storage for pattern bytes
    size_t *pattern_offsets;       // Offset of each pattern in storage
    size_t *pattern_lens;          // Length of each pattern
    int *pattern_ids;              // User-provided pattern IDs
    size_t pattern_count;          // Number of patterns
    size_t patterns_size;          // Total bytes used in patterns storage

    bool case_insensitive;         // Case-insensitive matching
    bool compiled;                 // Has ac_compile() been called?
};

// Case folding lookup table
static const uint8_t to_lower_tbl[256] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
     ' ',  '!',  '"',  '#',  '$',  '%',  '&', 0x27,
     '(',  ')',  '*',  '+',  ',',  '-',  '.',  '/',
     '0',  '1',  '2',  '3',  '4',  '5',  '6',  '7',
     '8',  '9',  ':',  ';',  '<',  '=',  '>',  '?',
     '@',  'a',  'b',  'c',  'd',  'e',  'f',  'g',
     'h',  'i',  'j',  'k',  'l',  'm',  'n',  'o',
     'p',  'q',  'r',  's',  't',  'u',  'v',  'w',
     'x',  'y',  'z',  '[', 0x5c,  ']',  '^',  '_',
     '`',  'a',  'b',  'c',  'd',  'e',  'f',  'g',
     'h',  'i',  'j',  'k',  'l',  'm',  'n',  'o',
     'p',  'q',  'r',  's',  't',  'u',  'v',  'w',
     'x',  'y',  'z',  '{',  '|',  '}',  '~', 0x7f,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

static inline uint8_t fold_case(uint8_t c, bool case_insensitive) {
    return case_insensitive ? to_lower_tbl[c] : c;
}

// Create a new trie node
static ACNode *node_create(void) {
    ACNode *node = calloc(1, sizeof(ACNode));
    return node;
}

// Free a trie node
static void node_free(ACNode *node) {
    if (node) {
        free(node->outputs);
        free(node);
    }
}

// Add an output pattern to a node
static int node_add_output(ACNode *node, int pattern_id) {
    if (node->output_count >= node->output_capacity) {
        int new_cap = node->output_capacity == 0 ? 4 : node->output_capacity * 2;
        int *new_outputs = realloc(node->outputs, new_cap * sizeof(int));
        if (!new_outputs) return -1;
        node->outputs = new_outputs;
        node->output_capacity = new_cap;
    }
    node->outputs[node->output_count++] = pattern_id;
    return 0;
}

// Track a node for cleanup
static int ac_track_node(AhoCorasick *ac, ACNode *node) {
    if (ac->node_count >= ac->node_capacity) {
        size_t new_cap = ac->node_capacity == 0 ? 64 : ac->node_capacity * 2;
        ACNode **new_nodes = realloc(ac->all_nodes, new_cap * sizeof(ACNode *));
        if (!new_nodes) return -1;
        ac->all_nodes = new_nodes;
        ac->node_capacity = new_cap;
    }
    ac->all_nodes[ac->node_count++] = node;
    return 0;
}

AhoCorasick *ac_create(bool case_insensitive) {
    AhoCorasick *ac = calloc(1, sizeof(AhoCorasick));
    if (!ac) return NULL;

    ac->case_insensitive = case_insensitive;

    // Allocate root node
    ac->root = node_create();
    if (!ac->root) {
        free(ac);
        return NULL;
    }

    if (ac_track_node(ac, ac->root) < 0) {
        node_free(ac->root);
        free(ac);
        return NULL;
    }

    // Allocate pattern storage
    ac->patterns = malloc(AC_MAX_TOTAL_LEN);
    ac->pattern_offsets = malloc(AC_MAX_PATTERNS * sizeof(size_t));
    ac->pattern_lens = malloc(AC_MAX_PATTERNS * sizeof(size_t));
    ac->pattern_ids = malloc(AC_MAX_PATTERNS * sizeof(int));

    if (!ac->patterns || !ac->pattern_offsets || !ac->pattern_lens || !ac->pattern_ids) {
        ac_free(ac);
        return NULL;
    }

    return ac;
}

int ac_add_pattern(AhoCorasick *ac, const uint8_t *pattern, size_t len, int id) {
    if (!ac || !pattern || len == 0) return -1;
    if (ac->compiled) return -1;  // Can't add after compile
    if (ac->pattern_count >= AC_MAX_PATTERNS) return -1;
    if (ac->patterns_size + len > AC_MAX_TOTAL_LEN) return -1;

    // Store pattern
    size_t offset = ac->patterns_size;
    memcpy(ac->patterns + offset, pattern, len);
    ac->pattern_offsets[ac->pattern_count] = offset;
    ac->pattern_lens[ac->pattern_count] = len;
    ac->pattern_ids[ac->pattern_count] = id;
    ac->patterns_size += len;

    // Build trie path for this pattern
    ACNode *node = ac->root;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = fold_case(pattern[i], ac->case_insensitive);

        if (!node->children[c]) {
            ACNode *child = node_create();
            if (!child) return -1;
            child->depth = node->depth + 1;
            if (ac_track_node(ac, child) < 0) {
                node_free(child);
                return -1;
            }
            node->children[c] = child;
        }
        node = node->children[c];
    }

    // Mark this node as accepting for this pattern
    if (node_add_output(node, (int)ac->pattern_count) < 0) {
        return -1;
    }

    ac->pattern_count++;
    return 0;
}

int ac_compile(AhoCorasick *ac) {
    if (!ac || ac->compiled) return -1;
    if (ac->pattern_count == 0) return -1;

    // Build failure links using BFS
    // Allocate queue for BFS traversal
    ACNode **queue = malloc(ac->node_count * sizeof(ACNode *));
    if (!queue) return -1;

    size_t head = 0, tail = 0;

    // Initialize: depth-1 nodes have fail link to root
    for (int c = 0; c < 256; c++) {
        ACNode *child = ac->root->children[c];
        if (child) {
            child->fail = ac->root;
            queue[tail++] = child;
        }
    }

    // BFS to build failure links for remaining nodes
    while (head < tail) {
        ACNode *node = queue[head++];

        for (int c = 0; c < 256; c++) {
            ACNode *child = node->children[c];
            if (!child) continue;

            // Find failure link for child
            ACNode *fail = node->fail;
            while (fail && !fail->children[c]) {
                fail = fail->fail;
            }
            child->fail = fail ? fail->children[c] : ac->root;

            // Merge outputs from failure link (suffix outputs)
            if (child->fail->output_count > 0) {
                for (int i = 0; i < child->fail->output_count; i++) {
                    if (node_add_output(child, child->fail->outputs[i]) < 0) {
                        free(queue);
                        return -1;
                    }
                }
            }

            queue[tail++] = child;
        }
    }

    free(queue);
    ac->compiled = true;
    return 0;
}

size_t ac_search(const AhoCorasick *ac, const uint8_t *text, size_t len,
                 ac_match_cb callback, void *ctx) {
    if (!ac || !ac->compiled || !text) return 0;

    size_t matches = 0;
    ACNode *node = ac->root;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = fold_case(text[i], ac->case_insensitive);

        // Follow failure links until we find a match or reach root
        while (node != ac->root && !node->children[c]) {
            node = node->fail;
        }

        node = node->children[c];
        if (!node) {
            node = ac->root;
            continue;
        }

        // Report all matches at this position
        if (node->output_count > 0) {
            for (int j = 0; j < node->output_count; j++) {
                int pat_idx = node->outputs[j];
                size_t pat_len = ac->pattern_lens[pat_idx];
                size_t match_start = i + 1 - pat_len;

                matches++;
                if (callback) {
                    callback(match_start, pat_len, ac->pattern_ids[pat_idx], ctx);
                }
            }
        }
    }

    return matches;
}

bool ac_contains(const AhoCorasick *ac, const uint8_t *text, size_t len) {
    if (!ac || !ac->compiled || !text) return false;

    ACNode *node = ac->root;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = fold_case(text[i], ac->case_insensitive);

        while (node != ac->root && !node->children[c]) {
            node = node->fail;
        }

        node = node->children[c];
        if (!node) {
            node = ac->root;
            continue;
        }

        // Found a match!
        if (node->output_count > 0) {
            return true;
        }
    }

    return false;
}

void ac_free(AhoCorasick *ac) {
    if (!ac) return;

    // Free all nodes
    for (size_t i = 0; i < ac->node_count; i++) {
        node_free(ac->all_nodes[i]);
    }
    free(ac->all_nodes);

    // Free pattern storage
    free(ac->patterns);
    free(ac->pattern_offsets);
    free(ac->pattern_lens);
    free(ac->pattern_ids);

    free(ac);
}

size_t ac_pattern_count(const AhoCorasick *ac) {
    return ac ? ac->pattern_count : 0;
}
