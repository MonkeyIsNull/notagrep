#include "glob.h"
#include <string.h>

// =============================================================================
// Basic glob matching (*, ?, [abc])
// =============================================================================

static bool match_class(const char **pattern, char c) {
    const char *p = *pattern;
    bool negated = false;
    bool matched = false;

    if (*p == '!') {
        negated = true;
        p++;
    }

    char prev = 0;
    while (*p && *p != ']') {
        if (*p == '-' && prev && p[1] && p[1] != ']') {
            // Range: a-z
            p++;
            if (c >= prev && c <= *p) {
                matched = true;
            }
            prev = 0;
            p++;
        } else {
            if (c == *p) {
                matched = true;
            }
            prev = *p;
            p++;
        }
    }

    if (*p == ']') {
        p++;
    }

    *pattern = p;
    return negated ? !matched : matched;
}

// Match simple glob pattern (no **)
static bool glob_match_simple(const char *pattern, const char *string) {
    while (*pattern && *string) {
        if (*pattern == '*') {
            pattern++;
            // * matches zero or more characters (but not /)
            if (*pattern == '\0') {
                // * at end - match if no more slashes
                while (*string) {
                    if (*string == '/') return false;
                    string++;
                }
                return true;
            }
            // Try matching rest of pattern at each position
            while (*string) {
                if (*string == '/') {
                    // * doesn't match across /
                    break;
                }
                if (glob_match_simple(pattern, string)) {
                    return true;
                }
                string++;
            }
            // Also try matching with * consuming nothing more
            return glob_match_simple(pattern, string);
        }

        if (*pattern == '?') {
            // ? matches any single character except /
            if (*string == '/') return false;
            pattern++;
            string++;
            continue;
        }

        if (*pattern == '[') {
            pattern++;  // Skip [
            if (!match_class(&pattern, *string)) {
                return false;
            }
            string++;
            continue;
        }

        if (*pattern == '\\' && pattern[1]) {
            // Escaped character
            pattern++;
        }

        if (*pattern != *string) {
            return false;
        }

        pattern++;
        string++;
    }

    // Skip trailing *
    while (*pattern == '*') pattern++;

    return *pattern == '\0' && *string == '\0';
}

bool glob_match(const char *pattern, const char *string) {
    return glob_match_simple(pattern, string);
}

// =============================================================================
// Gitignore-style matching
// =============================================================================

// Check if pattern contains a slash (excluding trailing)
static bool has_slash(const char *pattern) {
    size_t len = strlen(pattern);
    for (size_t i = 0; i < len; i++) {
        if (pattern[i] == '/') {
            // Ignore trailing slash
            if (i == len - 1) continue;
            return true;
        }
    }
    return false;
}

// Match ** pattern (matches any path components)
static bool match_doublestar(const char *pattern, const char *path) {
    // ** matches zero or more path components
    const char *after = pattern + 2;

    if (*after == '/') after++;  // Skip / after **

    if (*after == '\0') {
        // ** at end matches everything
        return true;
    }

    // Try matching rest of pattern at each path component
    while (*path) {
        if (gitignore_match(after, path, false)) {
            return true;
        }
        // Skip to next component
        while (*path && *path != '/') path++;
        if (*path == '/') path++;
    }

    return false;
}

bool gitignore_match(const char *pattern, const char *path, bool is_dir) {
    // Handle directory-only patterns (trailing /)
    size_t plen = strlen(pattern);
    bool dir_only = (plen > 0 && pattern[plen - 1] == '/');

    if (dir_only && !is_dir) {
        return false;
    }

    // Remove trailing / from pattern for matching
    char pattern_buf[1024];
    if (dir_only) {
        if (plen >= sizeof(pattern_buf)) return false;
        memcpy(pattern_buf, pattern, plen - 1);
        pattern_buf[plen - 1] = '\0';
        pattern = pattern_buf;
        plen--;
    }

    // Handle leading / (anchored to root)
    bool anchored = (pattern[0] == '/');
    if (anchored) {
        pattern++;
        plen--;
    }

    // If pattern contains /, it must match the full path structure
    // If not, it can match just the basename
    if (!anchored && !has_slash(pattern)) {
        // Match against basename only
        const char *basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        path = basename;
    }

    // Handle ** in pattern
    const char *p = pattern;
    const char *s = path;

    while (*p && *s) {
        if (p[0] == '*' && p[1] == '*') {
            return match_doublestar(p, s);
        }

        if (*p == '*') {
            p++;
            // * matches any non-/ characters
            if (*p == '\0') {
                // * at end
                while (*s && *s != '/') s++;
                return *s == '\0';
            }
            // Try matching rest at each position
            while (*s && *s != '/') {
                if (gitignore_match(p, s, is_dir)) return true;
                s++;
            }
            return gitignore_match(p, s, is_dir);
        }

        if (*p == '?') {
            if (*s == '/' || *s == '\0') return false;
            p++;
            s++;
            continue;
        }

        if (*p == '[') {
            p++;
            if (!match_class(&p, *s)) return false;
            s++;
            continue;
        }

        if (*p == '\\' && p[1]) {
            p++;
        }

        if (*p != *s) {
            return false;
        }

        p++;
        s++;
    }

    // Skip trailing *
    while (*p == '*') p++;

    // Handle trailing ** matching empty
    if (p[0] == '*' && p[1] == '*' && p[2] == '\0') {
        return true;
    }

    return *p == '\0' && *s == '\0';
}
