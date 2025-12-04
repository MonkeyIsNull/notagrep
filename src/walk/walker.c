#include "walk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

// Check if name is hidden (starts with .)
static bool is_hidden(const char *name) {
    return name[0] == '.';
}

// Check if path matches type filter
static bool matches_type(const char *path, const WalkConfig *config) {
    if (!config->type_extensions || config->type_count == 0) {
        return true;
    }

    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    ext++;  // Skip the dot

    for (size_t i = 0; i < config->type_count; i++) {
        if (strcasecmp(ext, config->type_extensions[i]) == 0) {
            return true;
        }
    }

    return false;
}

// Load gitignore files for a directory
static IgnoreRules *load_ignore_rules(const char *dir_path, IgnoreRules *parent,
                                       bool skip_gitignore) {
    IgnoreRules *rules = ignore_rules_new(dir_path, parent);
    if (!rules) return parent;  // Fall back to parent on error

    if (!skip_gitignore) {
        char path[PATH_MAX];

        // Load .gitignore
        snprintf(path, sizeof(path), "%s/.gitignore", dir_path);
        ignore_rules_load(rules, path);

        // Load .notagrepignore
        snprintf(path, sizeof(path), "%s/.notagrepignore", dir_path);
        ignore_rules_load(rules, path);

        // Load .ignore (fd-style)
        snprintf(path, sizeof(path), "%s/.ignore", dir_path);
        ignore_rules_load(rules, path);
    }

    return rules;
}

// Get relative path from base
static const char *get_relative_path(const char *full_path, const char *base_path) {
    size_t base_len = strlen(base_path);
    if (strncmp(full_path, base_path, base_len) == 0) {
        const char *rel = full_path + base_len;
        while (*rel == '/') rel++;
        return rel;
    }
    return full_path;
}

// Recursive walk implementation
static int walk_recursive(const char *dir_path,
                          const char *base_path,
                          const WalkConfig *config,
                          IgnoreRules *parent_rules,
                          size_t depth,
                          walk_file_cb callback,
                          void *user_data,
                          int *file_count) {
    // Check depth limit
    if (config->max_depth > 0 && depth > config->max_depth) {
        return 0;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        return 0;
    }

    // Load ignore rules for this directory
    IgnoreRules *rules = load_ignore_rules(dir_path, parent_rules,
                                            config->skip_gitignore);

    struct dirent *entry;
    char full_path[PATH_MAX];
    int result = 0;

    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Skip hidden unless configured
        if (!config->include_hidden && is_hidden(entry->d_name)) {
            continue;
        }

        // Build full path
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (config->follow_symlinks) {
            if (stat(full_path, &st) < 0) continue;
        } else {
            if (lstat(full_path, &st) < 0) continue;
            // Skip symlinks
            if (S_ISLNK(st.st_mode)) continue;
        }

        // Get relative path for ignore matching
        const char *rel_path = get_relative_path(full_path, base_path);

        bool is_dir = S_ISDIR(st.st_mode);

        // Check ignore rules
        if (ignore_rules_match(rules, rel_path, is_dir)) {
            continue;
        }

        if (is_dir) {
            // Recurse into subdirectory
            result = walk_recursive(full_path, base_path, config, rules,
                                   depth + 1, callback, user_data, file_count);
            if (result < 0) break;  // Callback requested stop
        } else if (S_ISREG(st.st_mode)) {
            // Regular file - check type filter
            if (!matches_type(full_path, config)) {
                continue;
            }

            (*file_count)++;

            // Call the callback
            if (!callback(full_path, user_data)) {
                result = -1;  // Callback requested stop
                break;
            }
        }
    }

    closedir(dir);

    // Free rules if we created them (not if we're using parent's)
    if (rules != parent_rules) {
        ignore_rules_free(rules);
    }

    return result;
}

int walk_directory(const char *root_path,
                   const WalkConfig *config,
                   walk_file_cb callback,
                   void *user_data) {
    if (!root_path || !callback) {
        return -1;
    }

    WalkConfig default_config = {0};
    if (!config) {
        config = &default_config;
    }

    // Resolve to absolute path for consistent relative paths
    char abs_path[PATH_MAX];
    if (!realpath(root_path, abs_path)) {
        return -1;
    }

    int file_count = 0;
    walk_recursive(abs_path, abs_path, config, NULL, 0,
                   callback, user_data, &file_count);

    return file_count;
}
