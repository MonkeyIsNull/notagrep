#include "parse.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// =============================================================================
// Parser state
// =============================================================================

typedef struct {
    const char *pattern;
    size_t len;
    size_t pos;
    const ParseOptions *opts;
    ParseError *err;
} Parser;

static void set_error(Parser *p, const char *msg) {
    if (p->err) {
        p->err->message = msg;
        p->err->position = p->pos;
    }
}

static inline bool at_end(Parser *p) {
    return p->pos >= p->len;
}

static inline char peek(Parser *p) {
    return at_end(p) ? '\0' : p->pattern[p->pos];
}

static inline char peek_ahead(Parser *p, size_t n) {
    return (p->pos + n >= p->len) ? '\0' : p->pattern[p->pos + n];
}

static inline char advance(Parser *p) {
    return at_end(p) ? '\0' : p->pattern[p->pos++];
}

static inline bool match(Parser *p, char c) {
    if (peek(p) == c) {
        advance(p);
        return true;
    }
    return false;
}

// =============================================================================
// Forward declarations
// =============================================================================

static AstNode *parse_regex(Parser *p);
static AstNode *parse_alt(Parser *p);
static AstNode *parse_concat(Parser *p);
static AstNode *parse_repeat(Parser *p);
static AstNode *parse_atom(Parser *p);
static AstNode *parse_group(Parser *p);
static AstNode *parse_class(Parser *p);
static AstNode *parse_escape(Parser *p);

// =============================================================================
// Character class helpers
// =============================================================================

// Build a character class for \d (digits)
static CharClass *make_digit_class(void) {
    CharClass *cc = charclass_new();
    if (cc) {
        charclass_add_range(cc, '0', '9');
    }
    return cc;
}

// Build a character class for \w (word characters)
static CharClass *make_word_class(void) {
    CharClass *cc = charclass_new();
    if (cc) {
        charclass_add_range(cc, 'a', 'z');
        charclass_add_range(cc, 'A', 'Z');
        charclass_add_range(cc, '0', '9');
        charclass_set(cc, '_');
    }
    return cc;
}

// Build a character class for \s (whitespace)
static CharClass *make_space_class(void) {
    CharClass *cc = charclass_new();
    if (cc) {
        charclass_set(cc, ' ');
        charclass_set(cc, '\t');
        charclass_set(cc, '\n');
        charclass_set(cc, '\r');
        charclass_set(cc, '\f');
        charclass_set(cc, '\v');
    }
    return cc;
}

// Add case variants of a character to a class
static void add_case_insensitive(CharClass *cc, uint8_t c) {
    charclass_set(cc, c);
    if (c >= 'a' && c <= 'z') {
        charclass_set(cc, c - 32);  // uppercase
    } else if (c >= 'A' && c <= 'Z') {
        charclass_set(cc, c + 32);  // lowercase
    }
}

// =============================================================================
// Parsing functions
// =============================================================================

// Parse a complete regex (top level)
static AstNode *parse_regex(Parser *p) {
    return parse_alt(p);
}

// Parse alternation: a|b|c
static AstNode *parse_alt(Parser *p) {
    AstNode *left = parse_concat(p);
    if (!left) return NULL;

    if (!match(p, '|')) {
        return left;
    }

    // We have alternation
    AstNode *alt = ast_alt();
    if (!alt) {
        ast_free(left);
        return NULL;
    }

    ast_add_child(alt, left);

    do {
        AstNode *branch = parse_concat(p);
        if (!branch) {
            // Empty branch after | is valid (matches empty string)
            branch = ast_empty();
        }
        ast_add_child(alt, branch);
    } while (match(p, '|'));

    return alt;
}

// Parse concatenation: abc
static AstNode *parse_concat(Parser *p) {
    AstNode *concat = ast_concat();
    if (!concat) return NULL;

    while (!at_end(p) && peek(p) != '|' && peek(p) != ')') {
        AstNode *node = parse_repeat(p);
        if (!node) {
            // This can happen at end of input or before special chars
            break;
        }
        ast_add_child(concat, node);
    }

    // Simplify: if only one child, return it directly
    if (concat->data.list.count == 0) {
        ast_free(concat);
        return ast_empty();
    }
    if (concat->data.list.count == 1) {
        AstNode *child = concat->data.list.children[0];
        concat->data.list.children[0] = NULL;
        concat->data.list.count = 0;
        ast_free(concat);
        return child;
    }

    return concat;
}

// Parse repetition: a?, a*, a+, a{m,n}
static AstNode *parse_repeat(Parser *p) {
    AstNode *atom = parse_atom(p);
    if (!atom) return NULL;

    char c = peek(p);

    if (c == '?') {
        advance(p);
        return ast_quest(atom);
    }

    if (c == '*') {
        advance(p);
        return ast_star(atom);
    }

    if (c == '+') {
        advance(p);
        return ast_plus(atom);
    }

    if (c == '{') {
        // Parse {m}, {m,}, or {m,n}
        advance(p);

        // Parse min
        int min = 0;
        while (isdigit(peek(p))) {
            min = min * 10 + (advance(p) - '0');
            if (min > 255) {
                set_error(p, "repetition count too large");
                ast_free(atom);
                return NULL;
            }
        }

        int max = min;  // Default: {m} means exactly m

        if (match(p, ',')) {
            // {m,} or {m,n}
            if (peek(p) == '}') {
                max = -1;  // Unbounded
            } else {
                max = 0;
                while (isdigit(peek(p))) {
                    max = max * 10 + (advance(p) - '0');
                    if (max > 255) {
                        set_error(p, "repetition count too large");
                        ast_free(atom);
                        return NULL;
                    }
                }
                if (max < min) {
                    set_error(p, "invalid repetition range");
                    ast_free(atom);
                    return NULL;
                }
            }
        }

        if (!match(p, '}')) {
            set_error(p, "expected '}' after repetition");
            ast_free(atom);
            return NULL;
        }

        return ast_repeat(atom, min, max);
    }

    return atom;
}

// Parse an atom: literal, ., group, class, anchor
static AstNode *parse_atom(Parser *p) {
    char c = peek(p);

    // End of input or special characters
    if (c == '\0' || c == '|' || c == ')' || c == '*' || c == '+' ||
        c == '?' || c == '{' || c == '}') {
        return NULL;
    }

    // Dot: any character
    if (c == '.') {
        advance(p);
        if (p->opts->bytes_mode) {
            // . matches any byte
            return ast_dot();
        } else {
            // . matches any byte except newline
            CharClass *cc = charclass_new();
            if (!cc) return NULL;
            // Set all bits
            memset(cc->bits, 0xFF, 32);
            // Clear newline
            charclass_clear(cc, '\n');
            return ast_class(cc);
        }
    }

    // Anchors
    if (c == '^') {
        advance(p);
        return ast_anchor_start();
    }

    if (c == '$') {
        advance(p);
        return ast_anchor_end();
    }

    // Group
    if (c == '(') {
        return parse_group(p);
    }

    // Character class
    if (c == '[') {
        return parse_class(p);
    }

    // Escape sequence
    if (c == '\\') {
        return parse_escape(p);
    }

    // Plain literal
    advance(p);

    if (p->opts->case_insensitive && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        // Create a character class with both cases
        CharClass *cc = charclass_new();
        if (!cc) return NULL;
        add_case_insensitive(cc, (uint8_t)c);
        return ast_class(cc);
    }

    return ast_literal((uint8_t)c);
}

// Parse a group: (...)
static AstNode *parse_group(Parser *p) {
    if (!match(p, '(')) {
        set_error(p, "expected '('");
        return NULL;
    }

    AstNode *inner = parse_regex(p);
    if (!inner) {
        inner = ast_empty();
    }

    if (!match(p, ')')) {
        set_error(p, "expected ')'");
        ast_free(inner);
        return NULL;
    }

    return ast_group(inner);
}

// Parse a character class: [abc], [^abc], [a-z]
static AstNode *parse_class(Parser *p) {
    if (!match(p, '[')) {
        set_error(p, "expected '['");
        return NULL;
    }

    CharClass *cc = charclass_new();
    if (!cc) return NULL;

    // Check for negation
    bool negated = match(p, '^');

    // Special case: ] at start is literal
    if (peek(p) == ']') {
        if (p->opts->case_insensitive) {
            add_case_insensitive(cc, ']');
        } else {
            charclass_set(cc, ']');
        }
        advance(p);
    }

    while (!at_end(p) && peek(p) != ']') {
        uint8_t start_char;

        if (peek(p) == '\\') {
            // Escape in character class
            advance(p);
            char esc = advance(p);

            switch (esc) {
                case 'd': {
                    CharClass *digit = make_digit_class();
                    if (digit) {
                        charclass_union(cc, digit);
                        charclass_free(digit);
                    }
                    continue;
                }
                case 'w': {
                    CharClass *word = make_word_class();
                    if (word) {
                        charclass_union(cc, word);
                        charclass_free(word);
                    }
                    continue;
                }
                case 's': {
                    CharClass *space = make_space_class();
                    if (space) {
                        charclass_union(cc, space);
                        charclass_free(space);
                    }
                    continue;
                }
                case 'n': start_char = '\n'; break;
                case 't': start_char = '\t'; break;
                case 'r': start_char = '\r'; break;
                case '\\': start_char = '\\'; break;
                case ']': start_char = ']'; break;
                case '[': start_char = '['; break;
                case '-': start_char = '-'; break;
                case '^': start_char = '^'; break;
                default:
                    start_char = (uint8_t)esc;
                    break;
            }
        } else {
            start_char = (uint8_t)advance(p);
        }

        // Check for range: a-z
        if (peek(p) == '-' && peek_ahead(p, 1) != ']') {
            advance(p);  // consume -

            uint8_t end_char;
            if (peek(p) == '\\') {
                advance(p);
                char esc = advance(p);
                switch (esc) {
                    case 'n': end_char = '\n'; break;
                    case 't': end_char = '\t'; break;
                    case 'r': end_char = '\r'; break;
                    default: end_char = (uint8_t)esc; break;
                }
            } else {
                end_char = (uint8_t)advance(p);
            }

            if (end_char < start_char) {
                set_error(p, "invalid character range");
                charclass_free(cc);
                return NULL;
            }

            if (p->opts->case_insensitive) {
                for (int i = start_char; i <= end_char; i++) {
                    add_case_insensitive(cc, (uint8_t)i);
                }
            } else {
                charclass_add_range(cc, start_char, end_char);
            }
        } else {
            // Single character
            if (p->opts->case_insensitive) {
                add_case_insensitive(cc, start_char);
            } else {
                charclass_set(cc, start_char);
            }
        }
    }

    if (!match(p, ']')) {
        set_error(p, "expected ']'");
        charclass_free(cc);
        return NULL;
    }

    if (negated) {
        charclass_negate(cc);
    }

    return ast_class(cc);
}

// Parse an escape sequence: \n, \t, \d, etc.
static AstNode *parse_escape(Parser *p) {
    if (!match(p, '\\')) {
        set_error(p, "expected '\\'");
        return NULL;
    }

    if (at_end(p)) {
        set_error(p, "unexpected end of pattern after '\\'");
        return NULL;
    }

    char c = advance(p);

    switch (c) {
        // Character escapes
        case 'n': return ast_literal('\n');
        case 't': return ast_literal('\t');
        case 'r': return ast_literal('\r');
        case 'f': return ast_literal('\f');
        case 'v': return ast_literal('\v');
        case '0': return ast_literal('\0');

        // Character classes
        case 'd': return ast_class(make_digit_class());
        case 'D': {
            CharClass *cc = make_digit_class();
            if (cc) charclass_negate(cc);
            return ast_class(cc);
        }
        case 'w': return ast_class(make_word_class());
        case 'W': {
            CharClass *cc = make_word_class();
            if (cc) charclass_negate(cc);
            return ast_class(cc);
        }
        case 's': return ast_class(make_space_class());
        case 'S': {
            CharClass *cc = make_space_class();
            if (cc) charclass_negate(cc);
            return ast_class(cc);
        }

        // Escaped metacharacters
        case '.':
        case '^':
        case '$':
        case '*':
        case '+':
        case '?':
        case '{':
        case '}':
        case '[':
        case ']':
        case '(':
        case ')':
        case '|':
        case '\\':
            if (p->opts->case_insensitive &&
                ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                CharClass *cc = charclass_new();
                if (cc) add_case_insensitive(cc, (uint8_t)c);
                return ast_class(cc);
            }
            return ast_literal((uint8_t)c);

        default:
            // Unknown escape - treat as literal
            if (p->opts->case_insensitive &&
                ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                CharClass *cc = charclass_new();
                if (cc) add_case_insensitive(cc, (uint8_t)c);
                return ast_class(cc);
            }
            return ast_literal((uint8_t)c);
    }
}

// =============================================================================
// Public API
// =============================================================================

AstNode *regex_parse(const char *pattern, size_t len,
                     const ParseOptions *opts, ParseError *err) {
    ParseOptions default_opts = {0};

    Parser p = {
        .pattern = pattern,
        .len = len,
        .pos = 0,
        .opts = opts ? opts : &default_opts,
        .err = err
    };

    if (err) {
        err->message = NULL;
        err->position = 0;
    }

    AstNode *ast = parse_regex(&p);

    // Check for leftover input
    if (!at_end(&p) && ast) {
        set_error(&p, "unexpected character");
        ast_free(ast);
        return NULL;
    }

    return ast;
}

const char *parse_error_message(const ParseError *err) {
    if (!err || !err->message) {
        return "unknown error";
    }
    return err->message;
}
