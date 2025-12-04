#ifndef NOTAGREP_GLOB_H
#define NOTAGREP_GLOB_H

#include <stdbool.h>
#include <stddef.h>

// Match a glob pattern against a string
// Supports: * (any chars), ? (single char), [abc] (char class), ** (recursive)
// Returns true if pattern matches string
bool glob_match(const char *pattern, const char *string);

// Match a gitignore-style pattern against a path
// pattern: the gitignore pattern (may include /)
// path: the relative path to match
// is_dir: true if path is a directory
// Returns true if pattern matches
bool gitignore_match(const char *pattern, const char *path, bool is_dir);

#endif // NOTAGREP_GLOB_H
