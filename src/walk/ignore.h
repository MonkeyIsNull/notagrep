#ifndef NOTAGREP_IGNORE_H
#define NOTAGREP_IGNORE_H

#include <stdbool.h>
#include <stddef.h>

// A single ignore pattern
typedef struct {
    char *pattern;      // The pattern string (owned)
    bool negated;       // Pattern starts with !
    bool dir_only;      // Pattern ends with /
    bool anchored;      // Pattern contains / (not at end)
} IgnorePattern;

// Collection of ignore rules for a directory
typedef struct IgnoreRules {
    IgnorePattern *patterns;
    size_t count;
    size_t capacity;
    struct IgnoreRules *parent;  // Parent directory's rules (not owned)
    char *base_path;             // Directory this ruleset applies to (owned)
} IgnoreRules;

// Create a new empty ruleset
IgnoreRules *ignore_rules_new(const char *base_path, IgnoreRules *parent);

// Free a ruleset (does not free parent)
void ignore_rules_free(IgnoreRules *rules);

// Load rules from a file (e.g., .gitignore)
// Returns number of patterns loaded, or -1 on error
int ignore_rules_load(IgnoreRules *rules, const char *filepath);

// Add a single pattern to the ruleset
void ignore_rules_add(IgnoreRules *rules, const char *pattern);

// Check if a path should be ignored
// path: relative path from base_path
// is_dir: true if path is a directory
// Returns true if the path should be ignored
bool ignore_rules_match(const IgnoreRules *rules, const char *path, bool is_dir);

#endif // NOTAGREP_IGNORE_H
