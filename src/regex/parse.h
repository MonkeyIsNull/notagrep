#ifndef NOTAGREP_REGEX_PARSE_H
#define NOTAGREP_REGEX_PARSE_H

#include "ast.h"
#include <stdbool.h>

// Parser options
typedef struct {
    bool case_insensitive;  // -i: case-insensitive matching
    bool bytes_mode;        // --bytes: . matches any byte including newline
} ParseOptions;

// Parser error information
typedef struct {
    const char *message;
    size_t position;        // Position in pattern where error occurred
} ParseError;

// Parse a regex pattern into an AST
// Returns NULL on error, sets error info if err is non-NULL
AstNode *regex_parse(const char *pattern, size_t len,
                     const ParseOptions *opts, ParseError *err);

// Get a human-readable error message
const char *parse_error_message(const ParseError *err);

#endif // NOTAGREP_REGEX_PARSE_H
