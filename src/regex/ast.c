#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =============================================================================
// Character class operations
// =============================================================================

CharClass *charclass_new(void) {
    CharClass *cc = calloc(1, sizeof(CharClass));
    return cc;
}

void charclass_free(CharClass *cc) {
    if (cc) {
        free(cc);
    }
}

void charclass_set(CharClass *cc, uint8_t byte) {
    cc->bits[byte / 8] |= (1 << (byte % 8));
}

void charclass_clear(CharClass *cc, uint8_t byte) {
    cc->bits[byte / 8] &= ~(1 << (byte % 8));
}

bool charclass_test(const CharClass *cc, uint8_t byte) {
    bool in_set = (cc->bits[byte / 8] & (1 << (byte % 8))) != 0;
    return cc->negated ? !in_set : in_set;
}

void charclass_add_range(CharClass *cc, uint8_t start, uint8_t end) {
    for (int i = start; i <= end; i++) {
        charclass_set(cc, (uint8_t)i);
    }
}

void charclass_negate(CharClass *cc) {
    cc->negated = !cc->negated;
}

void charclass_union(CharClass *dst, const CharClass *src) {
    for (int i = 0; i < 32; i++) {
        dst->bits[i] |= src->bits[i];
    }
}

// =============================================================================
// AST node construction
// =============================================================================

static AstNode *ast_alloc(AstNodeType type) {
    AstNode *node = calloc(1, sizeof(AstNode));
    if (node) {
        node->type = type;
    }
    return node;
}

AstNode *ast_literal(uint8_t byte) {
    AstNode *node = ast_alloc(AST_LITERAL);
    if (node) {
        node->data.literal = byte;
    }
    return node;
}

AstNode *ast_dot(void) {
    return ast_alloc(AST_DOT);
}

AstNode *ast_class(CharClass *cc) {
    AstNode *node = ast_alloc(AST_CLASS);
    if (node) {
        node->data.char_class = cc;
    }
    return node;
}

AstNode *ast_concat(void) {
    AstNode *node = ast_alloc(AST_CONCAT);
    if (node) {
        node->data.list.capacity = 4;
        node->data.list.children = malloc(4 * sizeof(AstNode *));
        node->data.list.count = 0;
    }
    return node;
}

AstNode *ast_alt(void) {
    AstNode *node = ast_alloc(AST_ALT);
    if (node) {
        node->data.list.capacity = 4;
        node->data.list.children = malloc(4 * sizeof(AstNode *));
        node->data.list.count = 0;
    }
    return node;
}

AstNode *ast_quest(AstNode *child) {
    AstNode *node = ast_alloc(AST_QUEST);
    if (node) {
        node->data.child = child;
    }
    return node;
}

AstNode *ast_star(AstNode *child) {
    AstNode *node = ast_alloc(AST_STAR);
    if (node) {
        node->data.child = child;
    }
    return node;
}

AstNode *ast_plus(AstNode *child) {
    AstNode *node = ast_alloc(AST_PLUS);
    if (node) {
        node->data.child = child;
    }
    return node;
}

AstNode *ast_repeat(AstNode *child, int min, int max) {
    AstNode *node = ast_alloc(AST_REPEAT);
    if (node) {
        node->data.repeat.child = child;
        node->data.repeat.min = min;
        node->data.repeat.max = max;
    }
    return node;
}

AstNode *ast_group(AstNode *child) {
    AstNode *node = ast_alloc(AST_GROUP);
    if (node) {
        node->data.child = child;
    }
    return node;
}

AstNode *ast_anchor_start(void) {
    return ast_alloc(AST_ANCHOR_START);
}

AstNode *ast_anchor_end(void) {
    return ast_alloc(AST_ANCHOR_END);
}

AstNode *ast_empty(void) {
    return ast_alloc(AST_EMPTY);
}

void ast_add_child(AstNode *parent, AstNode *child) {
    if (!parent || !child) return;
    if (parent->type != AST_CONCAT && parent->type != AST_ALT) return;

    // Grow array if needed
    if (parent->data.list.count >= parent->data.list.capacity) {
        size_t new_cap = parent->data.list.capacity * 2;
        AstNode **new_children = realloc(parent->data.list.children,
                                         new_cap * sizeof(AstNode *));
        if (!new_children) return;
        parent->data.list.children = new_children;
        parent->data.list.capacity = new_cap;
    }

    parent->data.list.children[parent->data.list.count++] = child;
}

void ast_free(AstNode *node) {
    if (!node) return;

    switch (node->type) {
        case AST_LITERAL:
        case AST_DOT:
        case AST_ANCHOR_START:
        case AST_ANCHOR_END:
        case AST_EMPTY:
            // No dynamic data
            break;

        case AST_CLASS:
            charclass_free(node->data.char_class);
            break;

        case AST_CONCAT:
        case AST_ALT:
            for (size_t i = 0; i < node->data.list.count; i++) {
                ast_free(node->data.list.children[i]);
            }
            free(node->data.list.children);
            break;

        case AST_QUEST:
        case AST_STAR:
        case AST_PLUS:
        case AST_GROUP:
            ast_free(node->data.child);
            break;

        case AST_REPEAT:
            ast_free(node->data.repeat.child);
            break;
    }

    free(node);
}

// =============================================================================
// Debug printing
// =============================================================================

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) {
        fprintf(stderr, "  ");
    }
}

void ast_debug_print(const AstNode *node, int indent) {
    if (!node) {
        print_indent(indent);
        fprintf(stderr, "(null)\n");
        return;
    }

    print_indent(indent);

    switch (node->type) {
        case AST_LITERAL:
            if (node->data.literal >= 32 && node->data.literal < 127) {
                fprintf(stderr, "LITERAL '%c'\n", node->data.literal);
            } else {
                fprintf(stderr, "LITERAL 0x%02x\n", node->data.literal);
            }
            break;

        case AST_DOT:
            fprintf(stderr, "DOT\n");
            break;

        case AST_CLASS:
            fprintf(stderr, "CLASS%s\n", node->data.char_class->negated ? " (negated)" : "");
            break;

        case AST_CONCAT:
            fprintf(stderr, "CONCAT (%zu children)\n", node->data.list.count);
            for (size_t i = 0; i < node->data.list.count; i++) {
                ast_debug_print(node->data.list.children[i], indent + 1);
            }
            break;

        case AST_ALT:
            fprintf(stderr, "ALT (%zu branches)\n", node->data.list.count);
            for (size_t i = 0; i < node->data.list.count; i++) {
                ast_debug_print(node->data.list.children[i], indent + 1);
            }
            break;

        case AST_QUEST:
            fprintf(stderr, "QUEST\n");
            ast_debug_print(node->data.child, indent + 1);
            break;

        case AST_STAR:
            fprintf(stderr, "STAR\n");
            ast_debug_print(node->data.child, indent + 1);
            break;

        case AST_PLUS:
            fprintf(stderr, "PLUS\n");
            ast_debug_print(node->data.child, indent + 1);
            break;

        case AST_REPEAT:
            fprintf(stderr, "REPEAT {%d,%d}\n",
                    node->data.repeat.min, node->data.repeat.max);
            ast_debug_print(node->data.repeat.child, indent + 1);
            break;

        case AST_GROUP:
            fprintf(stderr, "GROUP\n");
            ast_debug_print(node->data.child, indent + 1);
            break;

        case AST_ANCHOR_START:
            fprintf(stderr, "ANCHOR_START (^)\n");
            break;

        case AST_ANCHOR_END:
            fprintf(stderr, "ANCHOR_END ($)\n");
            break;

        case AST_EMPTY:
            fprintf(stderr, "EMPTY\n");
            break;
    }
}
