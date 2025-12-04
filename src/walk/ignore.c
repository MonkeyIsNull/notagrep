#include "ignore.h"
#include "glob.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

IgnoreRules *ignore_rules_new(const char *base_path, IgnoreRules *parent) {
    IgnoreRules *rules = calloc(1, sizeof(IgnoreRules));
    if (!rules) return NULL;

    rules->capacity = 16;
    rules->patterns = malloc(rules->capacity * sizeof(IgnorePattern));
    if (!rules->patterns) {
        free(rules);
        return NULL;
    }

    rules->parent = parent;

    if (base_path) {
        rules->base_path = strdup(base_path);
    }

    return rules;
}

void ignore_rules_free(IgnoreRules *rules) {
    if (!rules) return;

    for (size_t i = 0; i < rules->count; i++) {
        free(rules->patterns[i].pattern);
    }
    free(rules->patterns);
    free(rules->base_path);
    free(rules);
}

void ignore_rules_add(IgnoreRules *rules, const char *pattern) {
    if (!rules || !pattern) return;

    // Skip empty lines and comments
    while (*pattern == ' ' || *pattern == '\t') pattern++;
    if (*pattern == '\0' || *pattern == '#') return;

    // Trim trailing whitespace (unless escaped)
    size_t len = strlen(pattern);
    while (len > 0 && (pattern[len-1] == ' ' || pattern[len-1] == '\t' ||
                       pattern[len-1] == '\r' || pattern[len-1] == '\n')) {
        if (len > 1 && pattern[len-2] == '\\') break;  // Escaped space
        len--;
    }
    if (len == 0) return;

    // Grow array if needed
    if (rules->count >= rules->capacity) {
        size_t new_cap = rules->capacity * 2;
        IgnorePattern *new_patterns = realloc(rules->patterns,
                                               new_cap * sizeof(IgnorePattern));
        if (!new_patterns) return;
        rules->patterns = new_patterns;
        rules->capacity = new_cap;
    }

    IgnorePattern *pat = &rules->patterns[rules->count];
    memset(pat, 0, sizeof(*pat));

    // Check for negation
    const char *p = pattern;
    if (*p == '!') {
        pat->negated = true;
        p++;
        len--;
    }

    // Check for directory-only
    if (len > 0 && p[len-1] == '/') {
        pat->dir_only = true;
    }

    // Check for anchoring (contains / not at end)
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '/' && i != len - 1) {
            pat->anchored = true;
            break;
        }
    }

    // Handle leading / (also means anchored)
    if (*p == '/') {
        pat->anchored = true;
        p++;
        len--;
    }

    // Copy pattern
    pat->pattern = malloc(len + 1);
    if (!pat->pattern) return;
    memcpy(pat->pattern, p, len);
    pat->pattern[len] = '\0';

    rules->count++;
}

int ignore_rules_load(IgnoreRules *rules, const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return -1;

    char line[4096];
    int count = 0;

    while (fgets(line, sizeof(line), f)) {
        ignore_rules_add(rules, line);
        count++;
    }

    fclose(f);
    return count;
}

bool ignore_rules_match(const IgnoreRules *rules, const char *path, bool is_dir) {
    if (!rules || !path) return false;

    // Start with parent's decision
    bool ignored = false;
    if (rules->parent) {
        ignored = ignore_rules_match(rules->parent, path, is_dir);
    }

    // Apply this level's patterns in order
    for (size_t i = 0; i < rules->count; i++) {
        const IgnorePattern *pat = &rules->patterns[i];

        // Directory-only patterns only match directories
        if (pat->dir_only && !is_dir) {
            continue;
        }

        bool matches;
        if (pat->anchored) {
            // Must match from the start of the path
            matches = gitignore_match(pat->pattern, path, is_dir);
        } else {
            // Can match at any level - try matching the basename
            const char *basename = strrchr(path, '/');
            basename = basename ? basename + 1 : path;
            matches = gitignore_match(pat->pattern, basename, is_dir);

            // Also try matching the full path
            if (!matches) {
                matches = gitignore_match(pat->pattern, path, is_dir);
            }
        }

        if (matches) {
            ignored = !pat->negated;
        }
    }

    return ignored;
}
