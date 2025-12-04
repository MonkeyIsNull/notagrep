#ifndef NOTAGREP_REGEX_AST_H
#define NOTAGREP_REGEX_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// AST node types
typedef enum {
    AST_LITERAL,        // Single byte literal
    AST_DOT,            // . (any byte, or any non-newline)
    AST_CLASS,          // Character class [abc] or [^abc]
    AST_CONCAT,         // Concatenation of nodes
    AST_ALT,            // Alternation (|)
    AST_QUEST,          // ? (zero or one)
    AST_STAR,           // * (zero or more)
    AST_PLUS,           // + (one or more)
    AST_REPEAT,         // {m,n} bounded repetition
    AST_GROUP,          // (...) grouping
    AST_ANCHOR_START,   // ^ start of line
    AST_ANCHOR_END,     // $ end of line
    AST_EMPTY,          // Empty string (epsilon)
} AstNodeType;

// Character class bitmap (256 bits for all byte values)
typedef struct {
    uint8_t bits[32];   // 256 / 8 = 32 bytes
    bool negated;
} CharClass;

// AST node
typedef struct AstNode {
    AstNodeType type;

    union {
        // AST_LITERAL
        uint8_t literal;

        // AST_CLASS
        CharClass *char_class;

        // AST_CONCAT, AST_ALT (multiple children)
        struct {
            struct AstNode **children;
            size_t count;
            size_t capacity;
        } list;

        // AST_QUEST, AST_STAR, AST_PLUS, AST_GROUP (single child)
        struct AstNode *child;

        // AST_REPEAT
        struct {
            struct AstNode *child;
            int min;
            int max;  // -1 means unbounded
        } repeat;
    } data;
} AstNode;

// Character class operations
CharClass *charclass_new(void);
void charclass_free(CharClass *cc);
void charclass_set(CharClass *cc, uint8_t byte);
void charclass_clear(CharClass *cc, uint8_t byte);
bool charclass_test(const CharClass *cc, uint8_t byte);
void charclass_add_range(CharClass *cc, uint8_t start, uint8_t end);
void charclass_negate(CharClass *cc);
void charclass_union(CharClass *dst, const CharClass *src);

// AST node construction
AstNode *ast_literal(uint8_t byte);
AstNode *ast_dot(void);
AstNode *ast_class(CharClass *cc);  // Takes ownership of cc
AstNode *ast_concat(void);
AstNode *ast_alt(void);
AstNode *ast_quest(AstNode *child);
AstNode *ast_star(AstNode *child);
AstNode *ast_plus(AstNode *child);
AstNode *ast_repeat(AstNode *child, int min, int max);
AstNode *ast_group(AstNode *child);
AstNode *ast_anchor_start(void);
AstNode *ast_anchor_end(void);
AstNode *ast_empty(void);

// Add child to CONCAT or ALT node
void ast_add_child(AstNode *parent, AstNode *child);

// Free an AST node and all its children
void ast_free(AstNode *node);

// Debug: print AST to stderr
void ast_debug_print(const AstNode *node, int indent);

#endif // NOTAGREP_REGEX_AST_H
