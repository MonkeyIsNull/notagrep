#ifndef NOTAGREP_REGEX_H
#define NOTAGREP_REGEX_H

#include "ast.h"
#include "parse.h"
#include "nfa.h"
#include "exec.h"
#include "literal.h"
#include "../search/prefilter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Compiled regex pattern
typedef struct {
    Nfa *nfa;                   // NFA for matching
    ExecContext exec_ctx;       // Reusable execution context

    // Single-literal prefilter for acceleration
    bool has_prefilter;
    Prefilter prefilter;
    bool prefilter_is_exact;    // If true, prefilter match = regex match

    // Multi-literal prefilter (for alternation patterns)
    bool has_multi_prefilter;
    MultiPrefilter multi_prefilter;
    bool multi_prefilter_is_exact;  // If true, any match = regex match

    // Inner literal prefilter (for patterns like .*foo.*)
    bool has_inner_prefilter;
    Prefilter inner_prefilter;

    // Required byte fallback (for patterns with no extractable literal)
    bool has_required_byte;
    uint8_t required_byte;

    // Original pattern info
    bool case_insensitive;
    bool anchored_start;        // Pattern starts with ^
    bool anchored_end;          // Pattern ends with $
} CompiledRegex;

// Compile a regex pattern
// Returns NULL on error, sets err if non-NULL
CompiledRegex *regex_compile(const char *pattern, size_t len,
                             bool case_insensitive, bool bytes_mode,
                             ParseError *err);

// Free a compiled regex
void regex_free(CompiledRegex *re);

// Find the first match in input
// Returns true if match found
bool regex_find_first(CompiledRegex *re,
                      const uint8_t *input, size_t input_len,
                      Match *match);

// Find all matches in input
// Returns number of matches
typedef void (*regex_match_callback)(const Match *match, void *user_data);
size_t regex_find_all(CompiledRegex *re,
                      const uint8_t *input, size_t input_len,
                      regex_match_callback cb, void *user_data);

// Check if any match exists (fast path for -l/-q modes)
bool regex_contains(CompiledRegex *re,
                    const uint8_t *input, size_t input_len);

// =============================================================================
// Thread-safe versions (use external ExecContext)
// =============================================================================

// Create a new ExecContext for thread-local use
// Returns 0 on success, -1 on error
int regex_exec_ctx_create(ExecContext *ctx, const CompiledRegex *re);

// Free an ExecContext created with regex_exec_ctx_create
void regex_exec_ctx_free(ExecContext *ctx);

// Find all matches using an external exec context (thread-safe)
size_t regex_find_all_ts(CompiledRegex *re, ExecContext *ctx,
                         const uint8_t *input, size_t input_len,
                         regex_match_callback cb, void *user_data);

// Check if any match exists using an external exec context (thread-safe)
bool regex_contains_ts(CompiledRegex *re, ExecContext *ctx,
                       const uint8_t *input, size_t input_len);

#endif // NOTAGREP_REGEX_H
