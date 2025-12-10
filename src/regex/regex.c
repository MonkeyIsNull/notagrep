#include "regex.h"
#include <stdlib.h>
#include <string.h>

CompiledRegex *regex_compile(const char *pattern, size_t len,
                             bool case_insensitive, bool bytes_mode,
                             ParseError *err) {
    CompiledRegex *re = calloc(1, sizeof(CompiledRegex));
    if (!re) return NULL;

    re->case_insensitive = case_insensitive;

    // Parse pattern
    ParseOptions opts = {
        .case_insensitive = case_insensitive,
        .bytes_mode = bytes_mode
    };

    AstNode *ast = regex_parse(pattern, len, &opts, err);
    if (!ast) {
        free(re);
        return NULL;
    }

    // Check for anchors
    if (ast->type == AST_CONCAT && ast->data.list.count > 0) {
        if (ast->data.list.children[0]->type == AST_ANCHOR_START) {
            re->anchored_start = true;
        }
        size_t last = ast->data.list.count - 1;
        if (ast->data.list.children[last]->type == AST_ANCHOR_END) {
            re->anchored_end = true;
        }
    } else if (ast->type == AST_ANCHOR_START) {
        re->anchored_start = true;
    } else if (ast->type == AST_ANCHOR_END) {
        re->anchored_end = true;
    }

    // Try to extract literal for prefiltering
    LiteralInfo lit_info;
    if (literal_extract(ast, &lit_info)) {
        if (prefilter_init(&re->prefilter, lit_info.bytes, lit_info.len,
                          case_insensitive) == 0) {
            re->has_prefilter = true;
            re->prefilter_is_exact = lit_info.is_exact;
        }
        literal_info_free(&lit_info);
    }

    // If no single literal, try multi-literal extraction (for alternations)
    if (!re->has_prefilter) {
        MultiLiteralInfo multi_info;
        if (multi_literal_extract(ast, &multi_info)) {
            if (multi_prefilter_init(&re->multi_prefilter,
                                     multi_info.literals, multi_info.lens,
                                     multi_info.count, case_insensitive) == 0) {
                re->has_multi_prefilter = true;
                re->multi_prefilter_is_exact = multi_info.all_exact;
            }
            multi_literal_info_free(&multi_info);
        }
    }

    // Inner literal extraction: for patterns like .*foo.* with no prefix literal
    // Extract the best inner literal for prefiltering
    if (!re->has_prefilter && !re->has_multi_prefilter) {
        InnerLiteralInfo inner_info;
        if (inner_literal_extract(ast, &inner_info)) {
            if (prefilter_init(&re->inner_prefilter, inner_info.bytes, inner_info.len,
                              case_insensitive) == 0) {
                re->has_inner_prefilter = true;
            }
            inner_literal_info_free(&inner_info);
        }
    }

    // Required byte fallback: for patterns with no extractable literal,
    // find any byte that MUST appear in any match
    if (!re->has_prefilter && !re->has_multi_prefilter && !re->has_inner_prefilter) {
        uint8_t req_byte;
        if (required_byte_extract(ast, &req_byte)) {
            re->has_required_byte = true;
            re->required_byte = req_byte;
        }
    }

    // Compile to NFA
    re->nfa = nfa_compile(ast);
    ast_free(ast);

    if (!re->nfa) {
        if (re->has_prefilter) {
            prefilter_free(&re->prefilter);
        }
        free(re);
        return NULL;
    }

    // Initialize execution context
    if (exec_context_init(&re->exec_ctx, re->nfa->count) < 0) {
        nfa_free(re->nfa);
        if (re->has_prefilter) {
            prefilter_free(&re->prefilter);
        }
        free(re);
        return NULL;
    }

    return re;
}

void regex_free(CompiledRegex *re) {
    if (!re) return;

    nfa_free(re->nfa);
    exec_context_free(&re->exec_ctx);

    if (re->has_prefilter) {
        prefilter_free(&re->prefilter);
    }

    if (re->has_multi_prefilter) {
        multi_prefilter_free(&re->multi_prefilter);
    }

    if (re->has_inner_prefilter) {
        prefilter_free(&re->inner_prefilter);
    }

    free(re);
}

bool regex_find_first(CompiledRegex *re,
                      const uint8_t *input, size_t input_len,
                      Match *match) {
    // If pattern is exact literal, just use prefilter
    if (re->has_prefilter && re->prefilter_is_exact) {
        ssize_t pos = prefilter_find_first(&re->prefilter, input, input_len);
        if (pos >= 0 && match) {
            match->start = (size_t)pos;
            match->end = (size_t)pos + re->prefilter.needle_len;
        }
        return pos >= 0;
    }

    // If we have a prefilter, use it to find candidates
    if (re->has_prefilter) {
        size_t pos = 0;
        while (pos < input_len) {
            // Find next literal candidate
            ssize_t lit_pos = prefilter_find_first(&re->prefilter,
                                                   input + pos, input_len - pos);
            if (lit_pos < 0) {
                return false;  // No more candidates
            }

            size_t candidate = pos + (size_t)lit_pos;

            // Try NFA match at this position
            if (nfa_match_at(re->nfa, &re->exec_ctx, input, input_len,
                            candidate, match)) {
                return true;
            }

            // Move past this candidate
            pos = candidate + 1;
        }
        return false;
    }

    // No prefilter: full NFA search
    return nfa_find_first(re->nfa, &re->exec_ctx, input, input_len, match);
}

// Callback wrapper for prefilter-accelerated search
typedef struct {
    CompiledRegex *re;
    const uint8_t *input;
    size_t input_len;
    regex_match_callback user_cb;
    void *user_data;
    size_t count;
    size_t last_match_end;
    bool is_inner_prefilter;  // True if using inner literal prefilter
} FindAllContext;

// Find line start before position (scan backward for newline)
static size_t find_line_start_before(const uint8_t *input, size_t pos) {
    while (pos > 0 && input[pos - 1] != '\n') {
        pos--;
    }
    return pos;
}

static void prefilter_match_handler(size_t pos, void *ctx) {
    FindAllContext *fctx = (FindAllContext *)ctx;

    // Skip if we already matched here
    if (pos < fctx->last_match_end) {
        return;
    }

    // For inner prefilter (patterns like .*foo.*), the NFA must start at
    // line start, not at the inner literal position. The .* at the beginning
    // needs to match from line start to the inner literal.
    size_t nfa_start = pos;
    if (fctx->is_inner_prefilter) {
        nfa_start = find_line_start_before(fctx->input, pos);
        // Skip if we already matched a line starting before this one
        if (nfa_start < fctx->last_match_end) {
            return;
        }
    }

    Match m;
    if (nfa_match_at(fctx->re->nfa, &fctx->re->exec_ctx,
                     fctx->input, fctx->input_len, nfa_start, &m)) {
        fctx->count++;
        if (fctx->user_cb) {
            fctx->user_cb(&m, fctx->user_data);
        }
        fctx->last_match_end = m.end > nfa_start ? m.end : nfa_start + 1;
    }
}

// Callback wrapper for multi-prefilter exact matches
typedef struct {
    regex_match_callback user_cb;
    void *user_data;
    size_t count;
} MultiExactContext;

static void multi_exact_handler(const MultiMatch *mm, void *ctx) {
    MultiExactContext *mctx = (MultiExactContext *)ctx;
    mctx->count++;
    if (mctx->user_cb) {
        Match m = { .start = mm->pos, .end = mm->pos + mm->len };
        mctx->user_cb(&m, mctx->user_data);
    }
}

size_t regex_find_all(CompiledRegex *re,
                      const uint8_t *input, size_t input_len,
                      regex_match_callback cb, void *user_data) {
    // If pattern is exact literal, just use prefilter
    if (re->has_prefilter && re->prefilter_is_exact) {
        size_t count = 0;
        size_t pos = 0;
        while (pos < input_len) {
            ssize_t found = prefilter_find_first(&re->prefilter,
                                                  input + pos, input_len - pos);
            if (found < 0) break;

            count++;
            if (cb) {
                Match m = { .start = pos + found, .end = pos + found + re->prefilter.needle_len };
                cb(&m, user_data);
            }
            pos += found + re->prefilter.needle_len;
        }
        return count;
    }

    // If pattern is exact alternation of literals, use multi-prefilter directly
    if (re->has_multi_prefilter && re->multi_prefilter_is_exact) {
        MultiExactContext ctx = {
            .user_cb = cb,
            .user_data = user_data,
            .count = 0
        };
        multi_prefilter_search(&re->multi_prefilter, input, input_len,
                               multi_exact_handler, &ctx);
        return ctx.count;
    }

    // If we have a prefilter, use it to find candidates
    if (re->has_prefilter) {
        FindAllContext ctx = {
            .re = re,
            .input = input,
            .input_len = input_len,
            .user_cb = cb,
            .user_data = user_data,
            .count = 0,
            .last_match_end = 0,
            .is_inner_prefilter = false
        };

        prefilter_search(&re->prefilter, input, input_len,
                        prefilter_match_handler, &ctx);
        return ctx.count;
    }

    // Inner literal prefilter: scan for inner literal, validate with NFA
    // For patterns like .*foo.*, we find "foo" but must start NFA at line start
    if (re->has_inner_prefilter) {
        FindAllContext ctx = {
            .re = re,
            .input = input,
            .input_len = input_len,
            .user_cb = cb,
            .user_data = user_data,
            .count = 0,
            .last_match_end = 0,
            .is_inner_prefilter = true  // Start NFA at line start
        };

        prefilter_search(&re->inner_prefilter, input, input_len,
                        prefilter_match_handler, &ctx);
        return ctx.count;
    }

    // Required byte fallback: scan for required byte, validate with NFA
    if (re->has_required_byte) {
        FindAllContext ctx = {
            .re = re,
            .input = input,
            .input_len = input_len,
            .user_cb = cb,
            .user_data = user_data,
            .count = 0,
            .last_match_end = 0
        };

        // Use byte_scan_all to find candidates, then validate each
        const uint8_t *p = input;
        const uint8_t *end = input + input_len;

        while (p < end) {
            const uint8_t *found = memchr(p, re->required_byte, end - p);
            if (!found) break;

            size_t pos = found - input;
            if (pos >= ctx.last_match_end) {
                Match m;
                if (nfa_match_at(re->nfa, &re->exec_ctx, input, input_len, pos, &m)) {
                    ctx.count++;
                    if (cb) cb(&m, ctx.user_data);
                    ctx.last_match_end = m.end > pos ? m.end : pos + 1;
                }
            }
            p = found + 1;
        }
        return ctx.count;
    }

    // No prefilter: full NFA search
    return nfa_find_all(re->nfa, &re->exec_ctx, input, input_len,
                       (match_callback)cb, user_data);
}

bool regex_contains(CompiledRegex *re,
                    const uint8_t *input, size_t input_len) {
    // Fast path with single-literal prefilter
    if (re->has_prefilter) {
        if (!prefilter_contains(&re->prefilter, input, input_len)) {
            return false;  // Prefilter says no match possible
        }

        if (re->prefilter_is_exact) {
            return true;  // Prefilter is exact, so we have a match
        }

        // Prefilter found candidate, verify with NFA
        return regex_find_first(re, input, input_len, NULL);
    }

    // Fast path with multi-literal prefilter (alternations)
    if (re->has_multi_prefilter) {
        if (!multi_prefilter_contains(&re->multi_prefilter, input, input_len)) {
            return false;  // No alternation branch matches
        }

        if (re->multi_prefilter_is_exact) {
            return true;  // All branches are exact literals, match found
        }

        // Multi-prefilter found candidate, verify with NFA
        return regex_find_first(re, input, input_len, NULL);
    }

    // Fast path with inner literal prefilter (for .*foo.* patterns)
    if (re->has_inner_prefilter) {
        if (!prefilter_contains(&re->inner_prefilter, input, input_len)) {
            return false;  // Inner literal not present, no match possible
        }

        // Inner literal found, verify with NFA
        return regex_find_first(re, input, input_len, NULL);
    }

    return nfa_contains(re->nfa, &re->exec_ctx, input, input_len);
}

// =============================================================================
// Thread-safe versions (use external ExecContext)
// =============================================================================

int regex_exec_ctx_create(ExecContext *ctx, const CompiledRegex *re) {
    return exec_context_init(ctx, re->nfa->count);
}

void regex_exec_ctx_free(ExecContext *ctx) {
    exec_context_free(ctx);
}

// Internal: thread-safe find_first using external context
static bool regex_find_first_ts(CompiledRegex *re, ExecContext *ctx,
                                const uint8_t *input, size_t input_len,
                                Match *match) {
    // If pattern is exact literal, just use prefilter
    if (re->has_prefilter && re->prefilter_is_exact) {
        ssize_t pos = prefilter_find_first(&re->prefilter, input, input_len);
        if (pos >= 0 && match) {
            match->start = (size_t)pos;
            match->end = (size_t)pos + re->prefilter.needle_len;
        }
        return pos >= 0;
    }

    // If we have a prefilter, use it to find candidates
    if (re->has_prefilter) {
        size_t pos = 0;
        while (pos < input_len) {
            ssize_t lit_pos = prefilter_find_first(&re->prefilter,
                                                   input + pos, input_len - pos);
            if (lit_pos < 0) {
                return false;
            }

            size_t candidate = pos + (size_t)lit_pos;

            if (nfa_match_at(re->nfa, ctx, input, input_len, candidate, match)) {
                return true;
            }

            pos = candidate + 1;
        }
        return false;
    }

    // No prefilter: full NFA search
    return nfa_find_first(re->nfa, ctx, input, input_len, match);
}

// Thread-safe context for prefilter-accelerated search
typedef struct {
    CompiledRegex *re;
    ExecContext *ctx;
    const uint8_t *input;
    size_t input_len;
    regex_match_callback user_cb;
    void *user_data;
    size_t count;
    size_t last_match_end;
    bool is_inner_prefilter;  // True if using inner literal prefilter
} FindAllContextTS;

static void prefilter_match_handler_ts(size_t pos, void *ctx) {
    FindAllContextTS *fctx = (FindAllContextTS *)ctx;

    if (pos < fctx->last_match_end) {
        return;
    }

    // For inner prefilter (patterns like .*foo.*), the NFA must start at
    // line start, not at the inner literal position.
    size_t nfa_start = pos;
    if (fctx->is_inner_prefilter) {
        nfa_start = find_line_start_before(fctx->input, pos);
        if (nfa_start < fctx->last_match_end) {
            return;
        }
    }

    Match m;
    if (nfa_match_at(fctx->re->nfa, fctx->ctx,
                     fctx->input, fctx->input_len, nfa_start, &m)) {
        fctx->count++;
        if (fctx->user_cb) {
            fctx->user_cb(&m, fctx->user_data);
        }
        fctx->last_match_end = m.end > nfa_start ? m.end : nfa_start + 1;
    }
}

size_t regex_find_all_ts(CompiledRegex *re, ExecContext *ctx,
                         const uint8_t *input, size_t input_len,
                         regex_match_callback cb, void *user_data) {
    // If pattern is exact literal, just use prefilter (no NFA needed)
    if (re->has_prefilter && re->prefilter_is_exact) {
        size_t count = 0;
        size_t pos = 0;
        while (pos < input_len) {
            ssize_t found = prefilter_find_first(&re->prefilter,
                                                  input + pos, input_len - pos);
            if (found < 0) break;

            count++;
            if (cb) {
                Match m = { .start = pos + found, .end = pos + found + re->prefilter.needle_len };
                cb(&m, user_data);
            }
            pos += found + re->prefilter.needle_len;
        }
        return count;
    }

    // If pattern is exact alternation of literals, use multi-prefilter directly
    if (re->has_multi_prefilter && re->multi_prefilter_is_exact) {
        MultiExactContext mctx = {
            .user_cb = cb,
            .user_data = user_data,
            .count = 0
        };
        multi_prefilter_search(&re->multi_prefilter, input, input_len,
                               multi_exact_handler, &mctx);
        return mctx.count;
    }

    // If we have a prefilter, use it to find candidates
    if (re->has_prefilter) {
        FindAllContextTS fctx = {
            .re = re,
            .ctx = ctx,
            .input = input,
            .input_len = input_len,
            .user_cb = cb,
            .user_data = user_data,
            .count = 0,
            .last_match_end = 0,
            .is_inner_prefilter = false
        };

        prefilter_search(&re->prefilter, input, input_len,
                        prefilter_match_handler_ts, &fctx);
        return fctx.count;
    }

    // Inner literal prefilter: scan for inner literal, validate with NFA
    // For patterns like .*foo.*, we find "foo" but must start NFA at line start
    if (re->has_inner_prefilter) {
        FindAllContextTS fctx = {
            .re = re,
            .ctx = ctx,
            .input = input,
            .input_len = input_len,
            .user_cb = cb,
            .user_data = user_data,
            .count = 0,
            .last_match_end = 0,
            .is_inner_prefilter = true  // Start NFA at line start
        };

        prefilter_search(&re->inner_prefilter, input, input_len,
                        prefilter_match_handler_ts, &fctx);
        return fctx.count;
    }

    // Required byte fallback: scan for required byte, validate with NFA
    if (re->has_required_byte) {
        FindAllContextTS fctx = {
            .re = re,
            .ctx = ctx,
            .input = input,
            .input_len = input_len,
            .user_cb = cb,
            .user_data = user_data,
            .count = 0,
            .last_match_end = 0
        };

        const uint8_t *p = input;
        const uint8_t *end = input + input_len;

        while (p < end) {
            const uint8_t *found = memchr(p, re->required_byte, end - p);
            if (!found) break;

            size_t pos = found - input;
            if (pos >= fctx.last_match_end) {
                Match m;
                if (nfa_match_at(re->nfa, ctx, input, input_len, pos, &m)) {
                    fctx.count++;
                    if (cb) cb(&m, fctx.user_data);
                    fctx.last_match_end = m.end > pos ? m.end : pos + 1;
                }
            }
            p = found + 1;
        }
        return fctx.count;
    }

    // No prefilter: full NFA search
    return nfa_find_all(re->nfa, ctx, input, input_len,
                       (match_callback)cb, user_data);
}

bool regex_contains_ts(CompiledRegex *re, ExecContext *ctx,
                       const uint8_t *input, size_t input_len) {
    // Fast path with single-literal prefilter
    if (re->has_prefilter) {
        if (!prefilter_contains(&re->prefilter, input, input_len)) {
            return false;
        }

        if (re->prefilter_is_exact) {
            return true;
        }

        // Prefilter found candidate, verify with NFA
        return regex_find_first_ts(re, ctx, input, input_len, NULL);
    }

    // Fast path with multi-literal prefilter (alternations)
    if (re->has_multi_prefilter) {
        if (!multi_prefilter_contains(&re->multi_prefilter, input, input_len)) {
            return false;
        }

        if (re->multi_prefilter_is_exact) {
            return true;
        }

        // Multi-prefilter found candidate, verify with NFA
        return regex_find_first_ts(re, ctx, input, input_len, NULL);
    }

    // Fast path with inner literal prefilter (for .*foo.* patterns)
    if (re->has_inner_prefilter) {
        if (!prefilter_contains(&re->inner_prefilter, input, input_len)) {
            return false;  // Inner literal not present, no match possible
        }

        // Inner literal found, verify with NFA
        return regex_find_first_ts(re, ctx, input, input_len, NULL);
    }

    return nfa_contains(re->nfa, ctx, input, input_len);
}
