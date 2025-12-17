#include "regex.h"
#include "dfa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Check if a character is a regex metacharacter that requires parsing
static bool is_regex_metachar(char c) {
    switch (c) {
        case '.': case '*': case '+': case '?':
        case '[': case ']': case '(': case ')':
        case '{': case '}': case '|': case '^':
        case '$': case '\\':
            return true;
        default:
            return false;
    }
}

// Check if pattern contains NO regex metacharacters (pure literal)
// This allows us to skip regex parsing entirely for simple strings
static bool is_pure_literal_pattern(const char *pattern, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (is_regex_metachar(pattern[i])) {
            return false;
        }
    }
    return len > 0;  // Empty pattern is not a pure literal
}

CompiledRegex *regex_compile(const char *pattern, size_t len,
                             bool case_insensitive, bool bytes_mode,
                             ParseError *err) {
    CompiledRegex *re = calloc(1, sizeof(CompiledRegex));
    if (!re) return NULL;

    re->case_insensitive = case_insensitive;

    // FAST PATH: Pure literal patterns (no regex metacharacters)
    // Skip regex parsing entirely - use direct prefilter search
    if (is_pure_literal_pattern(pattern, len)) {
        if (prefilter_init(&re->prefilter, (const uint8_t *)pattern, len,
                          case_insensitive) == 0) {
            re->has_prefilter = true;
            re->prefilter_is_exact = true;

            // We still need a minimal NFA for correctness in edge cases
            // but the hot paths (count_lines, contains, find_all) will
            // bypass NFA entirely due to prefilter_is_exact
            ParseOptions opts = {
                .case_insensitive = case_insensitive,
                .bytes_mode = bytes_mode
            };
            AstNode *ast = regex_parse(pattern, len, &opts, err);
            if (!ast) {
                prefilter_free(&re->prefilter);
                free(re);
                return NULL;
            }
            re->nfa = nfa_compile(ast);
            ast_free(ast);

            if (!re->nfa) {
                prefilter_free(&re->prefilter);
                free(re);
                return NULL;
            }

            if (exec_context_init(&re->exec_ctx, re->nfa->count) < 0) {
                nfa_free(re->nfa);
                prefilter_free(&re->prefilter);
                free(re);
                return NULL;
            }

            return re;  // Early return - skip all other prefilter extraction
        }
        // If prefilter_init failed, fall through to normal path
    }

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

            // For non-exact prefix patterns (like func.*return), also extract
            // ALL literals to use as a more efficient multi-literal prefilter.
            // This allows us to quickly find candidate lines by searching for
            // ANY literal, then run NFA only once per line containing a match.
            if (!re->prefilter_is_exact && !case_insensitive) {
                AllLiteralsInfo all_info;
                if (all_literals_extract(ast, &all_info)) {
                    if (multi_prefilter_init(&re->all_literals_prefilter,
                                             all_info.literals, all_info.lens,
                                             all_info.count, false) == 0) {
                        re->has_all_literals_prefilter = true;
                    }
                    all_literals_info_free(&all_info);
                }
            }
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

                // Check if this is a PURE inner literal (just .*X.*)
                // If so, we can skip NFA entirely - any line containing X matches
                if (!case_insensitive) {
                    re->is_pure_inner_literal = is_pure_inner_literal(ast, NULL, NULL);
                }
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

    // Initialize execution context (for NFA fallback)
    if (exec_context_init(&re->exec_ctx, re->nfa->count) < 0) {
        nfa_free(re->nfa);
        if (re->has_prefilter) {
            prefilter_free(&re->prefilter);
        }
        free(re);
        return NULL;
    }

    // Try to compile DFA for fast matching
    // Skip DFA if pattern is likely to cause state explosion
    if (!dfa_will_explode(re->nfa)) {
        re->dfa = dfa_compile(re->nfa, DFA_MAX_STATES_DEFAULT);
        if (re->dfa && re->dfa->is_complete) {
            re->use_dfa = true;

            // Sheng SIMD DFA disabled - benchmarking shows it's ~24% SLOWER than
            // regular DFA due to poor cache locality (4KB of masks vs 512B per state)
            // The SIMD shuffle doesn't compensate for the cache misses.
            // State acceleration (memchr skip) is a better optimization for grep.
            #if 0
            if (re->dfa->state_count <= SHENG_MAX_STATES &&
                !re->dfa->has_start_anchor && !re->dfa->has_end_anchor) {
                if (sheng_compile(&re->sheng, re->dfa)) {
                    re->use_sheng = true;
                }
            }
            #endif
        }
    }

    return re;
}

void regex_free(CompiledRegex *re) {
    if (!re) return;

    nfa_free(re->nfa);
    dfa_free(re->dfa);
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

    if (re->has_all_literals_prefilter) {
        multi_prefilter_free(&re->all_literals_prefilter);
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

    // For inner prefilter (patterns like .*foo.*), must start at line start
    size_t match_start = pos;
    if (fctx->is_inner_prefilter) {
        match_start = find_line_start_before(fctx->input, pos);
        if (match_start < fctx->last_match_end) {
            return;
        }
    }

    Match m;
    bool matched;

    // Use Sheng SIMD DFA if available (fastest), else DFA, else NFA
    if (fctx->re->use_sheng) {
        matched = sheng_match_at(&fctx->re->sheng, fctx->input, fctx->input_len,
                                 match_start, &m);
    } else if (fctx->re->use_dfa) {
        matched = dfa_match_at(fctx->re->dfa, fctx->input, fctx->input_len,
                               match_start, &m);
    } else {
        matched = nfa_match_at(fctx->re->nfa, &fctx->re->exec_ctx,
                               fctx->input, fctx->input_len, match_start, &m);
    }

    if (matched) {
        fctx->count++;
        if (fctx->user_cb) {
            fctx->user_cb(&m, fctx->user_data);
        }
        fctx->last_match_end = m.end > match_start ? m.end : match_start + 1;
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

    // For inner prefilter (patterns like .*foo.*), must start at line start
    size_t match_start = pos;
    if (fctx->is_inner_prefilter) {
        match_start = find_line_start_before(fctx->input, pos);
        if (match_start < fctx->last_match_end) {
            return;
        }
    }

    Match m;
    bool matched;

    // Use Sheng SIMD DFA if available (fastest), else DFA, else NFA
    if (fctx->re->use_sheng) {
        matched = sheng_match_at(&fctx->re->sheng, fctx->input, fctx->input_len,
                                 match_start, &m);
    } else if (fctx->re->use_dfa) {
        matched = dfa_match_at(fctx->re->dfa, fctx->input, fctx->input_len,
                               match_start, &m);
    } else {
        matched = nfa_match_at(fctx->re->nfa, fctx->ctx,
                               fctx->input, fctx->input_len, match_start, &m);
    }

    if (matched) {
        fctx->count++;
        if (fctx->user_cb) {
            fctx->user_cb(&m, fctx->user_data);
        }
        fctx->last_match_end = m.end > match_start ? m.end : match_start + 1;
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
        // Fast path: PURE inner literal (.*X.*) - no NFA verification needed!
        if (re->is_pure_inner_literal) {
            // Every match of the literal is a valid regex match
            size_t count = 0;
            size_t pos = 0;
            while (pos < input_len) {
                ssize_t found = prefilter_find_first(&re->inner_prefilter,
                                                      input + pos, input_len - pos);
                if (found < 0) break;

                count++;
                if (cb) {
                    Match m = { .start = pos + found,
                                .end = pos + found + re->inner_prefilter.needle_len };
                    cb(&m, user_data);
                }
                pos += found + re->inner_prefilter.needle_len;
            }
            return count;
        }

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

    // No prefilter: try DFA if available, otherwise NFA
    if (re->use_dfa) {
        // DFA single-pass search - faster than NFA for patterns without prefilters
        size_t count = 0;
        size_t pos = 0;

        while (pos < input_len) {
            Match m;
            // Use line-scoped DFA search
            const uint8_t *line_end_ptr = memchr(input + pos, '\n', input_len - pos);
            size_t line_end = line_end_ptr ? (size_t)(line_end_ptr - input) + 1 : input_len;

            if (dfa_find_first(re->dfa, input + pos, line_end - pos, &m)) {
                m.start += pos;
                m.end += pos;
                count++;
                if (cb) cb(&m, user_data);
                // Skip to after the match
                pos = m.end > pos ? m.end : pos + 1;
            } else {
                // No match on this line, skip to next
                pos = line_end;
            }
        }
        return count;
    }

    // Fallback: full NFA search
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

        // Prefilter found candidate, verify with NFA at candidate position
        // (NFA verification at a point is faster than DFA full scan)
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

        // Fast path: PURE inner literal (.*X.*) - no NFA needed!
        if (re->is_pure_inner_literal) {
            return true;  // Literal found = match found
        }

        // Inner literal found, verify with NFA
        return regex_find_first_ts(re, ctx, input, input_len, NULL);
    }

    // No prefilter: Sheng SIMD or DFA for full input scan
    if (re->use_sheng) {
        return sheng_contains(&re->sheng, input, input_len);
    }
    if (re->use_dfa) {
        return dfa_contains(re->dfa, input, input_len);
    }

    return nfa_contains(re->nfa, ctx, input, input_len);
}

// Count unique lines with matches (optimized for -c mode)
size_t regex_count_lines_ts(CompiledRegex *re, ExecContext *ctx,
                            const uint8_t *input, size_t input_len) {
    // Fast path: exact single literal - use optimized SIMD count
    if (re->has_prefilter && re->prefilter_is_exact) {
        return prefilter_count_lines(&re->prefilter, input, input_len);
    }

    // Fast path: exact alternation of literals - use AC with line dedup
    if (re->has_multi_prefilter && re->multi_prefilter_is_exact) {
        return multi_prefilter_count_lines(&re->multi_prefilter, input, input_len);
    }

    // Fast path: PURE inner literal (.*X.*) - no NFA needed!
    // Any line containing the literal matches.
    if (re->has_inner_prefilter && re->is_pure_inner_literal) {
        return prefilter_count_lines(&re->inner_prefilter, input, input_len);
    }

    // Fast path: inner literal for .*X.* patterns (but not pure - need NFA)
    if (re->has_inner_prefilter) {
        // Use inner literal prefilter with NFA verification
        size_t lines = 0;
        size_t last_line_end = SIZE_MAX;  // Initialize to SIZE_MAX so first match is counted
        size_t pos = 0;

        while (pos < input_len) {
            ssize_t lit_pos = prefilter_find_first(&re->inner_prefilter,
                                                    input + pos, input_len - pos);
            if (lit_pos < 0) break;

            size_t candidate = pos + (size_t)lit_pos;

            // Skip if we already counted this line
            if (last_line_end != SIZE_MAX && candidate <= last_line_end) {
                pos = candidate + 1;
                continue;
            }

            // For inner prefilter, start NFA at line start
            size_t line_start = candidate;
            while (line_start > 0 && input[line_start - 1] != '\n') {
                line_start--;
            }

            // Skip if line already counted
            if (last_line_end != SIZE_MAX && line_start <= last_line_end) {
                pos = candidate + 1;
                continue;
            }

            // Verify with Sheng/DFA/NFA (fastest to slowest)
            Match m;
            bool matched = re->use_sheng
                ? sheng_match_at(&re->sheng, input, input_len, line_start, &m)
                : re->use_dfa
                    ? dfa_match_at(re->dfa, input, input_len, line_start, &m)
                    : nfa_match_at(re->nfa, ctx, input, input_len, line_start, &m);
            if (matched) {
                lines++;
                // Find end of line
                const uint8_t *nl = memchr(input + candidate, '\n', input_len - candidate);
                last_line_end = nl ? (size_t)(nl - input) : input_len;
            }

            pos = candidate + 1;
        }
        return lines;
    }

    // Prefix prefilter + verification with line counting
    if (re->has_prefilter) {
        // If we have all-literals prefilter (e.g., for "func.*return"),
        // use multi-literal SIMD to find candidate lines more efficiently
        if (re->has_all_literals_prefilter && re->all_literals_prefilter.use_teddy_multi) {
            // Use TeddyMulti SIMD to find ANY literal, then verify
            size_t lines = 0;
            size_t last_line_end = SIZE_MAX;
            size_t pos = 0;

            while (pos < input_len) {
                // Find next occurrence of ANY literal
                size_t match_pos;
                size_t pattern_idx;
                if (!teddy_multi_find_first(&re->all_literals_prefilter.teddy_multi,
                                            input + pos, input_len - pos,
                                            &match_pos, &pattern_idx)) {
                    break;  // No more matches
                }

                size_t candidate = pos + match_pos;

                // Find line boundaries
                size_t line_st = candidate;
                while (line_st > 0 && input[line_st - 1] != '\n') {
                    line_st--;
                }
                const uint8_t *nl = memchr(input + candidate, '\n', input_len - candidate);
                size_t line_ed = nl ? (size_t)(nl - input) : input_len;

                // Skip if we already counted this line
                if (last_line_end != SIZE_MAX && line_st <= last_line_end) {
                    pos = line_ed + 1;
                    continue;
                }

                // Now find the prefix literal on this line for proper starting position
                ssize_t prefix_pos = prefilter_find_first(&re->prefilter,
                                                           input + line_st, line_ed - line_st);
                if (prefix_pos < 0) {
                    // No prefix on this line - skip
                    pos = line_ed + 1;
                    continue;
                }

                // Verify with Sheng/DFA/NFA starting at prefix position
                size_t verify_start = line_st + (size_t)prefix_pos;
                Match m;
                bool matched = re->use_sheng
                    ? sheng_match_at(&re->sheng, input, input_len, verify_start, &m)
                    : re->use_dfa
                        ? dfa_match_at(re->dfa, input, input_len, verify_start, &m)
                        : nfa_match_at(re->nfa, ctx, input, input_len, verify_start, &m);
                if (matched) {
                    lines++;
                    last_line_end = line_ed;
                }

                pos = line_ed + 1;
            }
            return lines;
        }

        // Standard prefix prefilter path
        size_t lines = 0;
        size_t last_line_end = SIZE_MAX;  // Initialize to SIZE_MAX so first match is counted
        size_t pos = 0;

        while (pos < input_len) {
            ssize_t lit_pos = prefilter_find_first(&re->prefilter,
                                                    input + pos, input_len - pos);
            if (lit_pos < 0) break;

            size_t candidate = pos + (size_t)lit_pos;

            // Skip if we already counted this line
            if (last_line_end != SIZE_MAX && candidate <= last_line_end) {
                pos = candidate + 1;
                continue;
            }

            // Verify with Sheng/DFA/NFA (fastest to slowest)
            Match m;
            bool matched = re->use_sheng
                ? sheng_match_at(&re->sheng, input, input_len, candidate, &m)
                : re->use_dfa
                    ? dfa_match_at(re->dfa, input, input_len, candidate, &m)
                    : nfa_match_at(re->nfa, ctx, input, input_len, candidate, &m);
            if (matched) {
                lines++;
                // Find end of line
                const uint8_t *nl = memchr(input + candidate, '\n', input_len - candidate);
                last_line_end = nl ? (size_t)(nl - input) : input_len;
            }

            pos = candidate + 1;
        }
        return lines;
    }

    // DFA fast path for count mode (no prefilter)
    if (re->use_dfa) {
        return dfa_count_lines(re->dfa, input, input_len);
    }

    // No prefilter: full NFA search with line counting
    // For now, fall back to find_all (could optimize with a dedicated NFA line counter)
    return regex_find_all_ts(re, ctx, input, input_len, NULL, NULL);
}

// Debug: print regex compilation info
void regex_debug_print(const CompiledRegex *re, const char *pattern) {
    fprintf(stderr, "\n=== Regex Debug Info ===\n");
    fprintf(stderr, "Pattern: %s\n", pattern);
    fprintf(stderr, "Case insensitive: %s\n", re->case_insensitive ? "yes" : "no");
    fprintf(stderr, "Anchored start: %s\n", re->anchored_start ? "yes" : "no");
    fprintf(stderr, "Anchored end: %s\n", re->anchored_end ? "yes" : "no");

    // Prefilter info
    if (re->has_prefilter) {
        fprintf(stderr, "\nPrefilter: PREFIX_LITERAL\n");
        fprintf(stderr, "  Literal: \"");
        for (size_t i = 0; i < re->prefilter.needle_len; i++) {
            uint8_t c = re->prefilter.needle[i];
            if (c >= 32 && c < 127) {
                fputc(c, stderr);
            } else {
                fprintf(stderr, "\\x%02x", c);
            }
        }
        fprintf(stderr, "\" (%zu bytes)\n", re->prefilter.needle_len);
        fprintf(stderr, "  Is exact: %s\n", re->prefilter_is_exact ? "YES (no NFA needed)" : "no");
        if (re->has_all_literals_prefilter) {
            fprintf(stderr, "  All-literals prefilter: %zu patterns (for fast candidate finding)\n",
                    re->all_literals_prefilter.count);
            // Access TeddyMulti patterns if using TeddyMulti
            if (re->all_literals_prefilter.use_teddy_multi) {
                for (size_t i = 0; i < re->all_literals_prefilter.teddy_multi.pattern_count && i < 8; i++) {
                    fprintf(stderr, "    [%zu] \"", i);
                    for (size_t j = 0; j < re->all_literals_prefilter.teddy_multi.pattern_lens[i]; j++) {
                        uint8_t c = re->all_literals_prefilter.teddy_multi.patterns[i][j];
                        if (c >= 32 && c < 127) {
                            fputc(c, stderr);
                        } else {
                            fprintf(stderr, "\\x%02x", c);
                        }
                    }
                    fprintf(stderr, "\" (%zu bytes)\n", re->all_literals_prefilter.teddy_multi.pattern_lens[i]);
                }
            } else if (re->all_literals_prefilter.pattern_lens) {
                // Fall back to pattern_lens for display
                for (size_t i = 0; i < re->all_literals_prefilter.count && i < 8; i++) {
                    fprintf(stderr, "    [%zu] (%zu bytes)\n", i, re->all_literals_prefilter.pattern_lens[i]);
                }
            }
        }
    } else if (re->has_multi_prefilter) {
        fprintf(stderr, "\nPrefilter: MULTI_LITERAL (alternation)\n");
        fprintf(stderr, "  Pattern count: %zu\n", re->multi_prefilter.count);
        fprintf(stderr, "  Uses Aho-Corasick: %s\n", re->multi_prefilter.use_ac ? "yes" : "no");
        fprintf(stderr, "  Is exact: %s\n", re->multi_prefilter_is_exact ? "YES (no NFA needed)" : "no");
        for (size_t i = 0; i < re->multi_prefilter.count && i < 8; i++) {
            fprintf(stderr, "  [%zu] \"", i);
            for (size_t j = 0; j < re->multi_prefilter.filters[i].needle_len; j++) {
                uint8_t c = re->multi_prefilter.filters[i].needle[j];
                if (c >= 32 && c < 127) {
                    fputc(c, stderr);
                } else {
                    fprintf(stderr, "\\x%02x", c);
                }
            }
            fprintf(stderr, "\" (%zu bytes)\n", re->multi_prefilter.filters[i].needle_len);
        }
    } else if (re->has_inner_prefilter) {
        fprintf(stderr, "\nPrefilter: INNER_LITERAL (for .*X.* patterns)\n");
        fprintf(stderr, "  Literal: \"");
        for (size_t i = 0; i < re->inner_prefilter.needle_len; i++) {
            uint8_t c = re->inner_prefilter.needle[i];
            if (c >= 32 && c < 127) {
                fputc(c, stderr);
            } else {
                fprintf(stderr, "\\x%02x", c);
            }
        }
        fprintf(stderr, "\" (%zu bytes)\n", re->inner_prefilter.needle_len);
        fprintf(stderr, "  Is pure inner: %s\n", re->is_pure_inner_literal ? "YES (no NFA needed)" : "no");
    } else if (re->has_required_byte) {
        fprintf(stderr, "\nPrefilter: REQUIRED_BYTE (fallback)\n");
        fprintf(stderr, "  Byte: 0x%02x", re->required_byte);
        if (re->required_byte >= 32 && re->required_byte < 127) {
            fprintf(stderr, " ('%c')", re->required_byte);
        }
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "\nPrefilter: NONE (full NFA scan)\n");
    }

    // NFA info
    fprintf(stderr, "\nNFA states: %zu\n", re->nfa->count);

    // DFA info
    if (re->dfa) {
        fprintf(stderr, "DFA: %u states (", re->dfa->state_count);
        if (re->use_dfa) {
            fprintf(stderr, "ACTIVE");
        } else {
            fprintf(stderr, "compiled but not used");
        }
        fprintf(stderr, ")\n");
        if (re->dfa->has_start_anchor) fprintf(stderr, "  Start anchor: yes\n");
        if (re->dfa->has_end_anchor) fprintf(stderr, "  End anchor: yes\n");
    } else {
        fprintf(stderr, "DFA: not compiled\n");
    }

    fprintf(stderr, "========================\n\n");
}
