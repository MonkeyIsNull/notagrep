#ifndef NOTAGREP_REGEX_LITERAL_H
#define NOTAGREP_REGEX_LITERAL_H

#include "ast.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Maximum number of alternation literals to extract
#define MAX_ALT_LITERALS 16

// Result of literal extraction
typedef struct {
    uint8_t *bytes;     // Extracted literal bytes (owned, must be freed)
    size_t len;         // Length of literal
    bool is_prefix;     // True if literal is a guaranteed prefix
    bool is_exact;      // True if the entire pattern is just this literal
} LiteralInfo;

// Result of multi-literal extraction (for alternations)
typedef struct {
    uint8_t *literals[MAX_ALT_LITERALS];  // Array of literal strings
    size_t lens[MAX_ALT_LITERALS];        // Length of each literal
    size_t count;                          // Number of literals
    bool all_exact;                        // True if all branches are exact literals
} MultiLiteralInfo;

// Extract a literal from an AST for use as a prefilter
// Returns true if a useful literal was found
// Caller must free info->bytes if returns true
bool literal_extract(const AstNode *ast, LiteralInfo *info);

// Extract multiple literals from an alternation pattern
// Returns true if useful literals were found
// Caller must call multi_literal_info_free if returns true
bool multi_literal_extract(const AstNode *ast, MultiLiteralInfo *info);

// Free resources in LiteralInfo
void literal_info_free(LiteralInfo *info);

// Free resources in MultiLiteralInfo
void multi_literal_info_free(MultiLiteralInfo *info);

// Extract a "required byte" from a pattern - a byte that MUST appear in any match
// This is useful as a fallback when no literal prefix can be extracted
// Returns true if a required byte was found
bool required_byte_extract(const AstNode *ast, uint8_t *byte);

// Result of inner literal extraction
// The literal might not be at the start of the pattern
typedef struct {
    uint8_t *bytes;      // Extracted literal bytes (owned, must be freed)
    size_t len;          // Length of literal
    size_t score;        // Quality score (higher = better for prefiltering)
} InnerLiteralInfo;

// Extract the best inner literal from a pattern
// This finds literals inside patterns like .*foo.* or [a-z]+bar[0-9]+
// Prioritizes longer literals with rarer characters
// Returns true if a useful literal was found
// Caller must free info->bytes if returns true
bool inner_literal_extract(const AstNode *ast, InnerLiteralInfo *info);

// Free resources in InnerLiteralInfo
void inner_literal_info_free(InnerLiteralInfo *info);

#endif // NOTAGREP_REGEX_LITERAL_H
