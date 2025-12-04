#include "cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/types.h>

// For getting CPU count
#ifdef __APPLE__
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

#define NOTAGREP_VERSION "0.1.0"

static const struct option long_options[] = {
    {"fixed-strings", no_argument,       NULL, 'F'},
    {"ignore-case",   no_argument,       NULL, 'i'},
    {"files-with-matches", no_argument,  NULL, 'l'},
    {"line-number",   no_argument,       NULL, 'n'},
    {"with-filename", no_argument,       NULL, 'H'},
    {"count",         no_argument,       NULL, 'c'},
    {"quiet",         no_argument,       NULL, 'q'},
    {"silent",        no_argument,       NULL, 'q'},
    {"from-stdin",    no_argument,       NULL, 'S'},
    {"hidden",        no_argument,       NULL, 'D'},
    {"binary",        no_argument,       NULL, 'B'},
    {"bytes",         no_argument,       NULL, 'b'},
    {"max-filesize",  required_argument, NULL, 'M'},
    {"threads",       required_argument, NULL, 'j'},
    {"type",          required_argument, NULL, 't'},
    {"help",          no_argument,       NULL, 'h'},
    {"version",       no_argument,       NULL, 'V'},
    {NULL, 0, NULL, 0}
};

static const char *short_options = "FilnHcqj:t:hV";

void config_print_version(void) {
    printf("notagrep %s\n", NOTAGREP_VERSION);
}

void config_print_usage(const char *program_name) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] PATTERN [PATH...]\n"
        "\n"
        "A fast, minimal grep alternative with a non-backtracking regex engine.\n"
        "\n"
        "ARGUMENTS:\n"
        "    PATTERN     Search pattern (regex by default, literal with -F)\n"
        "    PATH        Files or directories to search (default: current directory)\n"
        "\n"
        "SEARCH OPTIONS:\n"
        "    -F, --fixed-strings    Treat PATTERN as a literal string\n"
        "    -i, --ignore-case      Case-insensitive search (ASCII)\n"
        "        --bytes            Pure bytes mode (. matches any byte including newline)\n"
        "\n"
        "OUTPUT OPTIONS:\n"
        "    -l, --files-with-matches  Only print filenames with matches\n"
        "    -n, --line-number         Print line numbers\n"
        "    -H, --with-filename       Always print filename\n"
        "    -c, --count               Only print count of matches per file\n"
        "    -q, --quiet               Suppress output, exit on first match\n"
        "\n"
        "INPUT OPTIONS:\n"
        "        --from-stdin       Read NUL-delimited file paths from stdin\n"
        "        --hidden           Include hidden files and directories\n"
        "        --binary           Search binary files\n"
        "        --max-filesize N   Skip files larger than N bytes\n"
        "    -j, --threads N        Number of threads (default: auto)\n"
        "    -t, --type TYPE        Only search files of TYPE (e.g., c, py, js)\n"
        "\n"
        "OTHER:\n"
        "    -h, --help             Print this help message\n"
        "    -V, --version          Print version information\n"
        "\n"
        "REGEX SYNTAX (non-backtracking subset):\n"
        "    .           Any byte (non-newline in text mode)\n"
        "    [abc]       Character class\n"
        "    [^abc]      Negated character class\n"
        "    a|b         Alternation\n"
        "    ?           Zero or one\n"
        "    *           Zero or more\n"
        "    +           One or more\n"
        "    {m,n}       Bounded repetition\n"
        "    ^           Start of line\n"
        "    $           End of line\n"
        "    (...)       Grouping\n"
        "    \\n \\t \\r   Escape sequences\n"
        "    \\s \\d \\w   Character classes (ASCII)\n"
        "\n"
        "NOT SUPPORTED: backreferences, lookahead/lookbehind, lazy quantifiers\n",
        program_name);
}

static size_t parse_size(const char *str) {
    char *end;
    long long val = strtoll(str, &end, 10);
    if (val < 0) return 0;

    switch (*end) {
        case 'k': case 'K': val *= 1024; break;
        case 'm': case 'M': val *= 1024 * 1024; break;
        case 'g': case 'G': val *= 1024 * 1024 * 1024; break;
    }
    return (size_t)val;
}

int config_parse(Config *cfg, int argc, char **argv) {
    // Initialize with defaults
    memset(cfg, 0, sizeof(*cfg));
    cfg->thread_count = 0;  // Auto-detect

    // Temporary storage for types
    const char *types[64];
    size_t type_count = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, short_options, long_options, NULL)) != -1) {
        switch (opt) {
            case 'F': cfg->fixed_string = true; break;
            case 'i': cfg->ignore_case = true; break;
            case 'l': cfg->list_files = true; break;
            case 'n': cfg->line_numbers = true; break;
            case 'H': cfg->always_filename = true; break;
            case 'c': cfg->count_only = true; break;
            case 'q': cfg->quiet = true; break;
            case 'S': cfg->from_stdin = true; break;
            case 'D': cfg->include_hidden = true; break;
            case 'B': cfg->search_binary = true; break;
            case 'b': cfg->bytes_mode = true; break;
            case 'M': cfg->max_filesize = parse_size(optarg); break;
            case 'j':
                cfg->thread_count = atoi(optarg);
                if (cfg->thread_count < 0) cfg->thread_count = 0;
                break;
            case 't':
                if (type_count < 64) {
                    types[type_count++] = optarg;
                }
                break;
            case 'h':
                config_print_usage(argv[0]);
                return 1;  // Signal to exit after help
            case 'V':
                config_print_version();
                return 1;  // Signal to exit after version
            default:
                config_print_usage(argv[0]);
                return -1;  // Error
        }
    }

    // Check for pattern
    if (optind >= argc) {
        fprintf(stderr, "error: missing PATTERN argument\n\n");
        config_print_usage(argv[0]);
        return -1;
    }
    cfg->pattern = argv[optind++];

    // Collect paths (remaining arguments)
    if (optind < argc) {
        cfg->path_count = argc - optind;
        cfg->paths = (const char **)&argv[optind];
    } else {
        // Default to current directory
        static const char *default_path[] = {".", NULL};
        cfg->paths = default_path;
        cfg->path_count = 1;
    }

    // Copy type filters if any
    if (type_count > 0) {
        cfg->type_include = malloc((type_count + 1) * sizeof(char *));
        if (cfg->type_include) {
            memcpy(cfg->type_include, types, type_count * sizeof(char *));
            cfg->type_include[type_count] = NULL;
            cfg->type_include_count = type_count;
        }
    }

    // Auto-detect thread count
    if (cfg->thread_count == 0) {
#ifdef __APPLE__
        int nprocs;
        size_t len = sizeof(nprocs);
        if (sysctlbyname("hw.ncpu", &nprocs, &len, NULL, 0) == 0) {
            cfg->thread_count = (nprocs > 0 && nprocs <= 16) ? nprocs : 4;
        } else {
            cfg->thread_count = 4;
        }
#else
        long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        cfg->thread_count = (nprocs > 0 && nprocs <= 16) ? (int)nprocs : 4;
#endif
    }

    // If searching multiple files/dirs, show filename by default
    if (cfg->path_count > 1 || !cfg->from_stdin) {
        cfg->always_filename = true;
    }

    return 0;
}

void config_free(Config *cfg) {
    if (cfg->type_include) {
        free((void *)cfg->type_include);
        cfg->type_include = NULL;
    }
}
