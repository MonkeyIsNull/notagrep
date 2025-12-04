#ifndef NOTAGREP_CLI_H
#define NOTAGREP_CLI_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *pattern;           // Search pattern
    const char **paths;            // Paths to search (NULL-terminated)
    size_t path_count;             // Number of paths

    // Search mode flags
    bool fixed_string;             // -F: treat pattern as literal string
    bool ignore_case;              // -i: case-insensitive search
    bool bytes_mode;               // --bytes: pure bytes mode (. matches any byte)

    // Output mode flags
    bool list_files;               // -l: only list files with matches
    bool line_numbers;             // -n: show line numbers
    bool always_filename;          // -H: always show filename
    bool count_only;               // -c: only show match count
    bool quiet;                    // -q: quiet mode (exit on first match)

    // Input mode flags
    bool from_stdin;               // --from-stdin: read NUL-delimited paths from stdin
    bool include_hidden;           // --hidden: include hidden files/dirs
    bool search_binary;            // --binary: search binary files

    // Limits
    size_t max_filesize;           // --max-filesize: skip files larger than this (0 = no limit)
    int thread_count;              // --threads: number of threads (0 = auto)

    // Type filters (NULL-terminated array)
    const char **type_include;     // --type: only search these types
    size_t type_include_count;
} Config;

// Parse command line arguments into Config
// Returns 0 on success, non-zero on error
// Prints usage/errors to stderr
int config_parse(Config *cfg, int argc, char **argv);

// Free any memory allocated by config_parse
void config_free(Config *cfg);

// Print usage information
void config_print_usage(const char *program_name);

// Print version information
void config_print_version(void);

#endif // NOTAGREP_CLI_H
