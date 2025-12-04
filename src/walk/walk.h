#ifndef NOTAGREP_WALK_H
#define NOTAGREP_WALK_H

#include "ignore.h"
#include <stdbool.h>
#include <stddef.h>

// Callback for each file found
// path: full path to file
// Returns: true to continue walking, false to stop
typedef bool (*walk_file_cb)(const char *path, void *user_data);

// Walker configuration
typedef struct {
    bool include_hidden;        // Include hidden files/directories
    bool skip_gitignore;        // Don't load .gitignore files
    bool follow_symlinks;       // Follow symbolic links (default: false)
    size_t max_depth;           // Maximum recursion depth (0 = unlimited)

    // File type filter (NULL = accept all)
    const char **type_extensions;  // e.g., {"c", "h", "cpp"}
    size_t type_count;
} WalkConfig;

// Walk a directory tree, calling callback for each file
// Returns number of files processed, or -1 on error
int walk_directory(const char *root_path,
                   const WalkConfig *config,
                   walk_file_cb callback,
                   void *user_data);

#endif // NOTAGREP_WALK_H
