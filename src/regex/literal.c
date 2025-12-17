#include "literal.h"
#include <stdlib.h>
#include <string.h>

// Minimum useful literal length for prefiltering
#define MIN_LITERAL_LEN 2

// =============================================================================
// Literal extraction
// =============================================================================

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t capacity;
    bool failed;  // Set if we hit something that can't be a literal prefix
} ExtractState;

static void state_init(ExtractState *s) {
    s->buf = malloc(64);
    s->len = 0;
    s->capacity = 64;
    s->failed = false;
}

static void state_free(ExtractState *s) {
    free(s->buf);
}

static void state_append(ExtractState *s, uint8_t byte) {
    if (s->failed) return;

    if (s->len >= s->capacity) {
        size_t new_cap = s->capacity * 2;
        uint8_t *new_buf = realloc(s->buf, new_cap);
        if (!new_buf) {
            s->failed = true;
            return;
        }
        s->buf = new_buf;
        s->capacity = new_cap;
    }

    s->buf[s->len++] = byte;
}

// Try to extract literal prefix from a node
// Returns true if we should continue extracting (node is "passthrough")
// Returns false if extraction should stop (hit variable-length or branching)
static bool extract_prefix(const AstNode *node, ExtractState *s);

static bool extract_prefix(const AstNode *node, ExtractState *s) {
    if (!node || s->failed) return false;

    switch (node->type) {
        case AST_LITERAL:
            state_append(s, node->data.literal);
            return true;

        case AST_CONCAT:
            // Extract from each child in order
            for (size_t i = 0; i < node->data.list.count; i++) {
                if (!extract_prefix(node->data.list.children[i], s)) {
                    return false;  // Stop at first non-literal
                }
            }
            return true;

        case AST_GROUP:
            return extract_prefix(node->data.child, s);

        case AST_ANCHOR_START:
            // ^ doesn't contribute bytes but we can continue
            return true;

        case AST_EMPTY:
            return true;

        case AST_ALT:
            // Alternation stops prefix extraction
            // (unless all branches have same prefix, which we don't handle)
            return false;

        case AST_DOT:
        case AST_CLASS:
            // Variable: can't extract
            return false;

        case AST_QUEST:
        case AST_STAR:
            // Optional: stops prefix extraction
            return false;

        case AST_PLUS:
            // At least one: extract from child, then stop
            extract_prefix(node->data.child, s);
            return false;

        case AST_REPEAT:
            // Extract min copies if min > 0
            if (node->data.repeat.min > 0) {
                for (int i = 0; i < node->data.repeat.min; i++) {
                    if (!extract_prefix(node->data.repeat.child, s)) {
                        return false;
                    }
                }
            }
            return false;

        case AST_ANCHOR_END:
            // $ stops extraction (nothing can follow for prefix purposes)
            return false;
    }

    return false;
}

// Check if an AST represents exactly a literal (no regex features)
static bool is_exact_literal(const AstNode *node, ExtractState *s) {
    if (!node) return true;

    switch (node->type) {
        case AST_LITERAL:
            state_append(s, node->data.literal);
            return true;

        case AST_CONCAT:
            for (size_t i = 0; i < node->data.list.count; i++) {
                if (!is_exact_literal(node->data.list.children[i], s)) {
                    return false;
                }
            }
            return true;

        case AST_GROUP:
            return is_exact_literal(node->data.child, s);

        case AST_EMPTY:
            return true;

        default:
            return false;
    }
}

bool literal_extract(const AstNode *ast, LiteralInfo *info) {
    memset(info, 0, sizeof(*info));

    if (!ast) return false;

    ExtractState s;
    state_init(&s);

    // First check if it's an exact literal
    if (is_exact_literal(ast, &s) && !s.failed && s.len >= MIN_LITERAL_LEN) {
        info->bytes = s.buf;
        info->len = s.len;
        info->is_prefix = true;
        info->is_exact = true;
        return true;
    }

    // Reset and try prefix extraction
    s.len = 0;
    s.failed = false;

    extract_prefix(ast, &s);

    if (!s.failed && s.len >= MIN_LITERAL_LEN) {
        info->bytes = s.buf;
        info->len = s.len;
        info->is_prefix = true;
        info->is_exact = false;
        return true;
    }

    state_free(&s);
    return false;
}

void literal_info_free(LiteralInfo *info) {
    if (info->bytes) {
        free(info->bytes);
        info->bytes = NULL;
    }
}

// =============================================================================
// Multi-literal extraction (for alternations like "error|warning|fatal")
// =============================================================================

// Try to extract a single literal from an AST branch
static bool extract_branch_literal(const AstNode *node, uint8_t **out_bytes, size_t *out_len) {
    ExtractState s;
    state_init(&s);

    if (is_exact_literal(node, &s) && !s.failed && s.len >= MIN_LITERAL_LEN) {
        *out_bytes = s.buf;
        *out_len = s.len;
        return true;
    }

    state_free(&s);
    return false;
}

// Recursively collect alternation branches
static bool collect_alt_branches(const AstNode *node, MultiLiteralInfo *info) {
    if (!node) return false;

    // Unwrap groups
    if (node->type == AST_GROUP) {
        return collect_alt_branches(node->data.child, info);
    }

    // If it's an alternation, collect from all children
    if (node->type == AST_ALT) {
        for (size_t i = 0; i < node->data.list.count; i++) {
            if (!collect_alt_branches(node->data.list.children[i], info)) {
                return false;
            }
        }
        return true;
    }

    // Otherwise, try to extract literal from this branch
    if (info->count >= MAX_ALT_LITERALS) {
        return false;  // Too many alternatives
    }

    uint8_t *bytes;
    size_t len;
    if (extract_branch_literal(node, &bytes, &len)) {
        info->literals[info->count] = bytes;
        info->lens[info->count] = len;
        info->count++;
        return true;
    }

    return false;
}

bool multi_literal_extract(const AstNode *ast, MultiLiteralInfo *info) {
    memset(info, 0, sizeof(*info));

    if (!ast) return false;

    // Unwrap top-level group if present
    const AstNode *node = ast;
    while (node->type == AST_GROUP) {
        node = node->data.child;
    }

    // Must be an alternation at top level
    if (node->type != AST_ALT) {
        return false;
    }

    // Try to collect all branches as literals
    if (!collect_alt_branches(node, info)) {
        multi_literal_info_free(info);
        return false;
    }

    // Need at least 2 alternatives
    if (info->count < 2) {
        multi_literal_info_free(info);
        return false;
    }

    info->all_exact = true;  // All branches were exact literals
    return true;
}

void multi_literal_info_free(MultiLiteralInfo *info) {
    for (size_t i = 0; i < info->count; i++) {
        free(info->literals[i]);
        info->literals[i] = NULL;
    }
    info->count = 0;
}

// =============================================================================
// Required byte extraction
// =============================================================================

// Find a required byte from an AST node
// Returns true and sets *byte if a required literal byte is found
static bool find_required_byte(const AstNode *node, uint8_t *byte) {
    if (!node) return false;

    switch (node->type) {
        case AST_LITERAL:
            // A literal byte is always required
            *byte = node->data.literal;
            return true;

        case AST_CONCAT:
            // In a concatenation, any required byte from any child works
            for (size_t i = 0; i < node->data.list.count; i++) {
                if (find_required_byte(node->data.list.children[i], byte)) {
                    return true;
                }
            }
            return false;

        case AST_GROUP:
            return find_required_byte(node->data.child, byte);

        case AST_PLUS:
            // + requires at least one occurrence of child
            return find_required_byte(node->data.child, byte);

        case AST_REPEAT:
            // {m,n} requires child if m > 0
            if (node->data.repeat.min > 0) {
                return find_required_byte(node->data.repeat.child, byte);
            }
            return false;

        case AST_ALT:
            // For alternation, we need a byte required by ALL branches
            // This is complex, so skip for now
            return false;

        case AST_QUEST:
        case AST_STAR:
            // ? and * don't require their content
            return false;

        case AST_DOT:
        case AST_CLASS:
        case AST_ANCHOR_START:
        case AST_ANCHOR_END:
        case AST_EMPTY:
            return false;
    }

    return false;
}

bool required_byte_extract(const AstNode *ast, uint8_t *byte) {
    return find_required_byte(ast, byte);
}

// =============================================================================
// Inner literal extraction
// For patterns like .*foo.* or [a-z]+bar[0-9]+, extract the best literal
// =============================================================================

// Character frequency table (lower = rarer = better for prefiltering)
// Based on English letter frequency, normalized to 0-100
static const uint8_t char_frequency[256] = {
    // 0-31: control chars (rare in text)
    5, 5, 5, 5, 5, 5, 5, 5, 5, 50, 50, 5, 5, 50, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    // 32-47: space and punctuation
    100, 10, 10, 5, 5, 5, 5, 20, 20, 20, 10, 10, 50, 30, 50, 20,
    // 48-57: digits
    30, 30, 30, 30, 30, 30, 30, 30, 30, 30,
    // 58-64: punctuation
    30, 30, 10, 20, 10, 10, 5,
    // 65-90: uppercase letters (scored like lowercase)
    60, 15, 30, 35, 95, 20, 20, 45, 60, 5, 10, 40, 25, 60, 65, 20,
    10, 55, 55, 75, 30, 10, 15, 5, 20, 5,
    // 91-96: punctuation
    10, 10, 10, 5, 30, 5,
    // 97-122: lowercase letters (English frequency)
    60, 15, 30, 35, 95, 20, 20, 45, 60, 5, 10, 40, 25, 60, 65, 20,
    10, 55, 55, 75, 30, 10, 15, 5, 20, 5,
    // 123-127: punctuation
    10, 5, 10, 5, 5,
    // 128-255: high bytes (rare in ASCII text)
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
};

// Candidate literal found during extraction
typedef struct {
    uint8_t *bytes;
    size_t len;
    size_t score;
} LiteralCandidate;

// Maximum candidates to track
#define MAX_CANDIDATES 16

// Score a literal for prefilter quality
// Higher score = better (longer, rarer chars)
static size_t score_literal(const uint8_t *bytes, size_t len) {
    if (len < MIN_LITERAL_LEN) return 0;

    // Base score from length (exponential - longer is much better)
    size_t score = len * len * 10;

    // Bonus for rare characters (inverse of frequency)
    for (size_t i = 0; i < len; i++) {
        score += (100 - char_frequency[bytes[i]]);
    }

    return score;
}

// Context for collecting literal candidates
typedef struct {
    LiteralCandidate candidates[MAX_CANDIDATES];
    size_t count;
    ExtractState current;  // Current literal being built
    bool in_required;      // Currently in a required context (not ?, *)
} CollectContext;

static void collect_init(CollectContext *ctx) {
    ctx->count = 0;
    ctx->in_required = true;
    state_init(&ctx->current);
}

static void collect_free(CollectContext *ctx) {
    state_free(&ctx->current);
    for (size_t i = 0; i < ctx->count; i++) {
        free(ctx->candidates[i].bytes);
    }
}

// Save current literal as a candidate if it's good enough
static void save_candidate(CollectContext *ctx) {
    if (ctx->current.len >= MIN_LITERAL_LEN && ctx->count < MAX_CANDIDATES) {
        size_t score = score_literal(ctx->current.buf, ctx->current.len);
        if (score > 0) {
            LiteralCandidate *c = &ctx->candidates[ctx->count];
            c->bytes = malloc(ctx->current.len);
            if (c->bytes) {
                memcpy(c->bytes, ctx->current.buf, ctx->current.len);
                c->len = ctx->current.len;
                c->score = score;
                ctx->count++;
            }
        }
    }
    // Reset current buffer
    ctx->current.len = 0;
    ctx->current.failed = false;
}

// Recursively collect literals from AST
static void collect_literals(const AstNode *node, CollectContext *ctx);

static void collect_literals(const AstNode *node, CollectContext *ctx) {
    if (!node) return;

    switch (node->type) {
        case AST_LITERAL:
            if (ctx->in_required) {
                state_append(&ctx->current, node->data.literal);
            }
            break;

        case AST_CONCAT:
            for (size_t i = 0; i < node->data.list.count; i++) {
                collect_literals(node->data.list.children[i], ctx);
            }
            break;

        case AST_GROUP:
            collect_literals(node->data.child, ctx);
            break;

        case AST_PLUS:
            // + requires at least one, so child is required
            collect_literals(node->data.child, ctx);
            // After the first occurrence, break the literal sequence
            save_candidate(ctx);
            break;

        case AST_REPEAT:
            if (node->data.repeat.min > 0) {
                // At least min copies are required
                collect_literals(node->data.repeat.child, ctx);
            }
            // After repeat, break the literal sequence
            save_candidate(ctx);
            break;

        case AST_STAR:
        case AST_QUEST:
            // Optional parts - save current, collect from child optionally
            save_candidate(ctx);
            {
                bool was_required = ctx->in_required;
                ctx->in_required = false;
                collect_literals(node->data.child, ctx);
                ctx->in_required = was_required;
            }
            break;

        case AST_ALT:
            // Save current before alternation
            save_candidate(ctx);
            // Collect from each branch - only literals common to ALL branches are required
            // For simplicity, just collect from each branch separately
            for (size_t i = 0; i < node->data.list.count; i++) {
                collect_literals(node->data.list.children[i], ctx);
                save_candidate(ctx);
            }
            break;

        case AST_DOT:
        case AST_CLASS:
            // Variable-width - break the literal sequence
            save_candidate(ctx);
            break;

        case AST_ANCHOR_START:
        case AST_ANCHOR_END:
        case AST_EMPTY:
            // Anchors don't affect literal collection
            break;
    }
}

bool inner_literal_extract(const AstNode *ast, InnerLiteralInfo *info) {
    memset(info, 0, sizeof(*info));

    if (!ast) return false;

    CollectContext ctx;
    collect_init(&ctx);

    // Collect all literal candidates
    collect_literals(ast, &ctx);
    save_candidate(&ctx);  // Save any remaining literal

    if (ctx.count == 0) {
        collect_free(&ctx);
        return false;
    }

    // Find the best candidate (highest score)
    size_t best_idx = 0;
    for (size_t i = 1; i < ctx.count; i++) {
        if (ctx.candidates[i].score > ctx.candidates[best_idx].score) {
            best_idx = i;
        }
    }

    // Copy the winner
    LiteralCandidate *best = &ctx.candidates[best_idx];
    info->bytes = malloc(best->len);
    if (!info->bytes) {
        collect_free(&ctx);
        return false;
    }
    memcpy(info->bytes, best->bytes, best->len);
    info->len = best->len;
    info->score = best->score;

    collect_free(&ctx);
    return true;
}

void inner_literal_info_free(InnerLiteralInfo *info) {
    if (info->bytes) {
        free(info->bytes);
        info->bytes = NULL;
    }
}

// =============================================================================
// All-literals extraction (for patterns like prefix.*suffix)
// =============================================================================

// Context for collecting all literals from an AST
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t capacity;
} LiteralBuf;

static void litbuf_init(LiteralBuf *b) {
    b->buf = malloc(64);
    b->len = 0;
    b->capacity = 64;
}

static void litbuf_free(LiteralBuf *b) {
    free(b->buf);
    b->buf = NULL;
    b->len = 0;
}

static void litbuf_append(LiteralBuf *b, uint8_t byte) {
    if (b->len >= b->capacity) {
        size_t new_cap = b->capacity * 2;
        uint8_t *new_buf = realloc(b->buf, new_cap);
        if (!new_buf) return;
        b->buf = new_buf;
        b->capacity = new_cap;
    }
    b->buf[b->len++] = byte;
}

static void litbuf_clear(LiteralBuf *b) {
    b->len = 0;
}

// Recursively collect all required literals from an AST
// Saves literal sequences separated by non-literals (.*, ., +, etc.)
static void collect_all_literals(const AstNode *node, LiteralBuf *current, AllLiteralsInfo *info) {
    if (!node || info->count >= MAX_ALT_LITERALS) return;

    switch (node->type) {
        case AST_LITERAL:
            litbuf_append(current, node->data.literal);
            break;

        case AST_CONCAT:
            for (size_t i = 0; i < node->data.list.count; i++) {
                collect_all_literals(node->data.list.children[i], current, info);
            }
            break;

        case AST_GROUP:
            collect_all_literals(node->data.child, current, info);
            break;

        case AST_PLUS:
            // At least one occurrence - extract from child, then save
            collect_all_literals(node->data.child, current, info);
            // Save current literal if any
            if (current->len >= MIN_LITERAL_LEN && info->count < MAX_ALT_LITERALS) {
                info->literals[info->count] = malloc(current->len);
                if (info->literals[info->count]) {
                    memcpy(info->literals[info->count], current->buf, current->len);
                    info->lens[info->count] = current->len;
                    info->count++;
                }
            }
            litbuf_clear(current);
            break;

        case AST_REPEAT:
            if (node->data.repeat.min > 0) {
                // At least min copies are required
                collect_all_literals(node->data.repeat.child, current, info);
            }
            // Save current literal if any
            if (current->len >= MIN_LITERAL_LEN && info->count < MAX_ALT_LITERALS) {
                info->literals[info->count] = malloc(current->len);
                if (info->literals[info->count]) {
                    memcpy(info->literals[info->count], current->buf, current->len);
                    info->lens[info->count] = current->len;
                    info->count++;
                }
            }
            litbuf_clear(current);
            break;

        case AST_STAR:
        case AST_QUEST:
        case AST_DOT:
        case AST_CLASS:
            // These break the literal sequence
            // Save current literal if any
            if (current->len >= MIN_LITERAL_LEN && info->count < MAX_ALT_LITERALS) {
                info->literals[info->count] = malloc(current->len);
                if (info->literals[info->count]) {
                    memcpy(info->literals[info->count], current->buf, current->len);
                    info->lens[info->count] = current->len;
                    info->count++;
                }
            }
            litbuf_clear(current);
            break;

        case AST_ALT:
            // For alternation, save current literal if any, then process branches
            if (current->len >= MIN_LITERAL_LEN && info->count < MAX_ALT_LITERALS) {
                info->literals[info->count] = malloc(current->len);
                if (info->literals[info->count]) {
                    memcpy(info->literals[info->count], current->buf, current->len);
                    info->lens[info->count] = current->len;
                    info->count++;
                }
            }
            litbuf_clear(current);
            // Collect from each branch
            for (size_t i = 0; i < node->data.list.count; i++) {
                collect_all_literals(node->data.list.children[i], current, info);
                // Save any literal from this branch
                if (current->len >= MIN_LITERAL_LEN && info->count < MAX_ALT_LITERALS) {
                    info->literals[info->count] = malloc(current->len);
                    if (info->literals[info->count]) {
                        memcpy(info->literals[info->count], current->buf, current->len);
                        info->lens[info->count] = current->len;
                        info->count++;
                    }
                }
                litbuf_clear(current);
            }
            break;

        case AST_ANCHOR_START:
        case AST_ANCHOR_END:
        case AST_EMPTY:
            // Don't affect literal collection
            break;
    }
}

bool all_literals_extract(const AstNode *ast, AllLiteralsInfo *info) {
    memset(info, 0, sizeof(*info));
    if (!ast) return false;

    LiteralBuf current;
    litbuf_init(&current);

    collect_all_literals(ast, &current, info);

    // Save any remaining literal
    if (current.len >= MIN_LITERAL_LEN && info->count < MAX_ALT_LITERALS) {
        info->literals[info->count] = malloc(current.len);
        if (info->literals[info->count]) {
            memcpy(info->literals[info->count], current.buf, current.len);
            info->lens[info->count] = current.len;
            info->count++;
        }
    }

    litbuf_free(&current);

    // Need at least 2 literals to be useful (otherwise single-literal is sufficient)
    if (info->count < 2) {
        all_literals_info_free(info);
        return false;
    }

    return true;
}

void all_literals_info_free(AllLiteralsInfo *info) {
    for (size_t i = 0; i < info->count; i++) {
        free(info->literals[i]);
        info->literals[i] = NULL;
    }
    info->count = 0;
}

// =============================================================================
// Pure inner literal detection
// =============================================================================

// Check if a character class is "all-but-newline" (how . is parsed in line mode)
static bool is_dot_class(const AstNode *node) {
    if (!node) return false;
    if (node->type == AST_DOT) return true;

    // In line mode, . is parsed as a character class with all bits set except newline
    if (node->type == AST_CLASS && node->data.char_class) {
        const CharClass *cc = node->data.char_class;
        if (cc->negated) return false;  // Negated classes are different

        // Check if all bytes except newline are set
        for (int i = 0; i < 256; i++) {
            bool set = charclass_test(cc, (uint8_t)i);
            if (i == '\n') {
                if (set) return false;  // Newline should NOT be set for dot
            } else {
                if (!set) return false;  // All other bytes should be set
            }
        }
        return true;
    }
    return false;
}

// Check if a node is .* or .+
static bool is_dot_star_or_plus(const AstNode *node) {
    if (!node) return false;
    if (node->type == AST_STAR || node->type == AST_PLUS) {
        const AstNode *child = node->data.child;
        return is_dot_class(child);
    }
    return false;
}

// Check if a node is just literals (returns the literal count)
// Also extracts the literal bytes if buf is non-NULL
static bool is_pure_literal_sequence(const AstNode *node, uint8_t *buf, size_t *len, size_t max_len) {
    if (!node) return false;

    switch (node->type) {
        case AST_LITERAL:
            if (buf && *len < max_len) {
                buf[*len] = node->data.literal;
            }
            (*len)++;
            return true;

        case AST_CONCAT:
            for (size_t i = 0; i < node->data.list.count; i++) {
                if (!is_pure_literal_sequence(node->data.list.children[i], buf, len, max_len)) {
                    return false;
                }
            }
            return true;

        case AST_GROUP:
            return is_pure_literal_sequence(node->data.child, buf, len, max_len);

        case AST_EMPTY:
            return true;

        default:
            return false;
    }
}

bool is_pure_inner_literal(const AstNode *ast, uint8_t **lit, size_t *out_len) {
    if (!ast) return false;

    // Unwrap top-level groups
    const AstNode *node = ast;
    while (node->type == AST_GROUP) {
        node = node->data.child;
        if (!node) return false;
    }

    // Case 1: Just a literal or literal sequence (no .*)
    // E.g., "function" or "func"
    size_t len = 0;
    if (is_pure_literal_sequence(node, NULL, &len, 0) && len >= MIN_LITERAL_LEN) {
        if (lit && out_len) {
            *lit = malloc(len);
            if (!*lit) return false;
            size_t dummy = 0;
            is_pure_literal_sequence(node, *lit, &dummy, len);
            *out_len = len;
        }
        return true;
    }

    // Case 2: CONCAT with optional leading .* and/or trailing .*
    // E.g., ".*function.*" or ".*function" or "function.*"
    if (node->type != AST_CONCAT) {
        return false;
    }

    size_t count = node->data.list.count;
    if (count == 0) return false;

    // Find the literal portion (skip leading .* and trailing .*)
    size_t start = 0;
    size_t end = count;

    // Skip leading .* or .+
    while (start < end && is_dot_star_or_plus(node->data.list.children[start])) {
        start++;
    }

    // Skip trailing .* or .+
    while (end > start && is_dot_star_or_plus(node->data.list.children[end - 1])) {
        end--;
    }

    // Must have something in the middle
    if (start >= end) {
        return false;
    }

    // Everything between start and end must be pure literals
    len = 0;
    for (size_t i = start; i < end; i++) {
        if (!is_pure_literal_sequence(node->data.list.children[i], NULL, &len, 0)) {
            return false;  // Found non-literal (anchor, class, alt, etc.)
        }
    }

    // Must have at least MIN_LITERAL_LEN bytes
    if (len < MIN_LITERAL_LEN) {
        return false;
    }

    // Extract the literal if requested
    if (lit && out_len) {
        *lit = malloc(len);
        if (!*lit) return false;
        size_t pos = 0;
        for (size_t i = start; i < end; i++) {
            is_pure_literal_sequence(node->data.list.children[i], *lit, &pos, len);
        }
        *out_len = len;
    }

    return true;
}
