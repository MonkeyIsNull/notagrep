#include "cli.h"
#include "io/io.h"
#include "search/prefilter.h"
#include "regex/regex.h"
#include "output/output.h"
#include "walk/walk.h"
#include "parallel/parallel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Binary detection threshold (4KB)
#define BINARY_CHECK_BYTES 4096

// Search mode: either prefilter (fixed string) or regex
typedef struct {
    bool is_regex;
    union {
        Prefilter *prefilter;
        CompiledRegex *regex;
    };
} SearchPattern;

// Global search context (shared across threads)
typedef struct {
    SearchPattern *pattern;
    const OutputConfig *out_cfg;
    const Config *cfg;
    ThreadPool *pool;           // NULL for single-threaded
    pthread_mutex_t output_mutex;  // Protect stdout
    atomic_bool stop_requested;
} GlobalContext;

// Per-thread context
typedef struct {
    GlobalContext *global;
    FileBuffer fb;
    ExecContext exec_ctx;       // Thread-local regex execution context
    bool exec_ctx_initialized;
} ThreadContext;

// Thread-local storage key
static pthread_key_t thread_ctx_key;
static pthread_once_t thread_ctx_once = PTHREAD_ONCE_INIT;

static void thread_ctx_destroy(void *ptr) {
    ThreadContext *ctx = (ThreadContext *)ptr;
    if (ctx) {
        file_buffer_free(&ctx->fb);
        if (ctx->exec_ctx_initialized) {
            regex_exec_ctx_free(&ctx->exec_ctx);
        }
        free(ctx);
    }
}

static void thread_ctx_init_once(void) {
    pthread_key_create(&thread_ctx_key, thread_ctx_destroy);
}

static ThreadContext *get_thread_context(GlobalContext *global) {
    pthread_once(&thread_ctx_once, thread_ctx_init_once);

    ThreadContext *ctx = pthread_getspecific(thread_ctx_key);
    if (!ctx) {
        ctx = calloc(1, sizeof(ThreadContext));
        if (ctx) {
            ctx->global = global;
            file_buffer_init(&ctx->fb, 64 * 1024);
            pthread_setspecific(thread_ctx_key, ctx);
        }
    }
    return ctx;
}

// Match callback context
typedef struct {
    Printer *printer;
    size_t match_len;
    bool found_any;
} MatchContext;

static void prefilter_match_handler(size_t pos, void *ctx) {
    MatchContext *mc = (MatchContext *)ctx;
    mc->found_any = true;
    printer_match(mc->printer, pos, mc->match_len);
}

static void regex_match_handler(const Match *match, void *ctx) {
    MatchContext *mc = (MatchContext *)ctx;
    mc->found_any = true;
    printer_match(mc->printer, match->start, match->end - match->start);
}

// Ensure thread-local ExecContext is initialized for regex patterns
static ExecContext *get_exec_context(ThreadContext *tctx) {
    if (!tctx->exec_ctx_initialized && tctx->global->pattern->is_regex) {
        if (regex_exec_ctx_create(&tctx->exec_ctx, tctx->global->pattern->regex) == 0) {
            tctx->exec_ctx_initialized = true;
        }
    }
    return tctx->exec_ctx_initialized ? &tctx->exec_ctx : NULL;
}

// Search a single file (thread-safe)
static bool search_file_worker(const char *path, ThreadContext *tctx) {
    GlobalContext *global = tctx->global;
    FileBuffer *fb = &tctx->fb;

    if (atomic_load(&global->stop_requested)) {
        return false;
    }

    // Open and read file
    if (file_open(fb, path, global->cfg->max_filesize) < 0) {
        return false;
    }

    // Skip binary files unless --binary
    if (!global->cfg->search_binary && file_is_binary(fb, BINARY_CHECK_BYTES)) {
        file_close(fb);
        return false;
    }

    // Set up printer
    Printer printer;
    printer_init(&printer, global->out_cfg, path, fb->data, fb->size);

    MatchContext mctx = {
        .printer = &printer,
        .match_len = 0,
        .found_any = false
    };

    if (global->pattern->is_regex) {
        ExecContext *ctx = get_exec_context(tctx);
        if (!ctx) {
            file_close(fb);
            return false;
        }

        if (global->cfg->quiet || global->cfg->list_files) {
            if (regex_contains_ts(global->pattern->regex, ctx, fb->data, fb->size)) {
                mctx.found_any = true;
            }
        } else if (global->cfg->count_only) {
            // Optimized path for regex count mode
            size_t lines = regex_count_lines_ts(global->pattern->regex, ctx,
                                                fb->data, fb->size);
            if (lines > 0) {
                mctx.found_any = true;
                printer.lines_matched = lines;
            }
        } else {
            regex_find_all_ts(global->pattern->regex, ctx, fb->data, fb->size,
                              regex_match_handler, &mctx);
        }
    } else {
        mctx.match_len = global->pattern->prefilter->needle_len;

        if (global->cfg->quiet || global->cfg->list_files) {
            if (prefilter_contains(global->pattern->prefilter, fb->data, fb->size)) {
                mctx.found_any = true;
            }
        } else if (global->cfg->count_only) {
            // Optimized path for count mode - uses SIMD with integrated line counting
            size_t lines = prefilter_count_lines(global->pattern->prefilter, fb->data, fb->size);
            if (lines > 0) {
                mctx.found_any = true;
                // Directly update printer state for count mode (bypass callback overhead)
                printer.lines_matched = lines;
            }
        } else {
            prefilter_search(global->pattern->prefilter, fb->data, fb->size,
                            prefilter_match_handler, &mctx);
        }
    }

    // Output results (lock for thread safety)
    if (mctx.found_any && global->pool) {
        pthread_mutex_lock(&global->output_mutex);
    }

    if (mctx.found_any) {
        if (global->cfg->list_files) {
            printf("%s\n", path);
        }
        printer_finish(&printer);

        if (global->pool) {
            threadpool_report_match(global->pool);
        }

        if (global->cfg->quiet) {
            atomic_store(&global->stop_requested, true);
        }
    } else {
        printer_finish(&printer);
    }

    if (mctx.found_any && global->pool) {
        pthread_mutex_unlock(&global->output_mutex);
    }

    file_close(fb);
    return mctx.found_any;
}

// Parallel worker function
static void parallel_worker(const char *path, void *user_data) {
    GlobalContext *global = (GlobalContext *)user_data;
    ThreadContext *ctx = get_thread_context(global);

    if (ctx) {
        search_file_worker(path, ctx);
    }
}

// Walker callback for collecting files to process
typedef struct {
    GlobalContext *global;
    ThreadContext *tctx;        // For single-threaded mode
    int found_count;
} WalkContext;

static bool walk_callback_parallel(const char *path, void *user_data) {
    WalkContext *ctx = (WalkContext *)user_data;

    if (atomic_load(&ctx->global->stop_requested)) {
        return false;
    }

    // Submit to thread pool
    threadpool_submit(ctx->global->pool, path);
    return true;
}

static bool walk_callback_single(const char *path, void *user_data) {
    WalkContext *ctx = (WalkContext *)user_data;

    if (atomic_load(&ctx->global->stop_requested)) {
        return false;
    }

    if (search_file_worker(path, ctx->tctx)) {
        ctx->found_count++;
    }

    return true;
}

int main(int argc, char **argv) {
    Config cfg;
    int result = config_parse(&cfg, argc, argv);

    if (result != 0) {
        return result < 0 ? 1 : 0;
    }

    SearchPattern pat = {0};

    if (cfg.fixed_string) {
        pat.is_regex = false;
        pat.prefilter = malloc(sizeof(Prefilter));
        if (!pat.prefilter) {
            fprintf(stderr, "error: out of memory\n");
            config_free(&cfg);
            return 1;
        }
        if (prefilter_init(pat.prefilter, (const uint8_t *)cfg.pattern,
                          strlen(cfg.pattern), cfg.ignore_case) < 0) {
            fprintf(stderr, "error: invalid pattern\n");
            free(pat.prefilter);
            config_free(&cfg);
            return 1;
        }
    } else {
        pat.is_regex = true;
        ParseError err = {0};
        pat.regex = regex_compile(cfg.pattern, strlen(cfg.pattern),
                                  cfg.ignore_case, cfg.bytes_mode, &err);
        if (!pat.regex) {
            fprintf(stderr, "error: invalid regex at position %zu: %s\n",
                    err.position, parse_error_message(&err));
            config_free(&cfg);
            return 1;
        }

        // Debug output if requested
        if (cfg.debug_regex) {
            regex_debug_print(pat.regex, cfg.pattern);
        }
    }

    OutputConfig out_cfg = {
        .show_filename = cfg.always_filename,
        .show_line_numbers = cfg.line_numbers,
        .list_files_only = cfg.list_files,
        .count_only = cfg.count_only,
        .quiet = cfg.quiet
    };

    GlobalContext global = {
        .pattern = &pat,
        .out_cfg = &out_cfg,
        .cfg = &cfg,
        .pool = NULL
    };
    atomic_store(&global.stop_requested, false);
    pthread_mutex_init(&global.output_mutex, NULL);

    WalkConfig walk_cfg = {
        .include_hidden = cfg.include_hidden,
        .skip_gitignore = false,
        .follow_symlinks = false,
        .max_depth = 0,
        .type_extensions = cfg.type_include,
        .type_count = cfg.type_include_count
    };

    int found_count = 0;
    bool use_parallel = (cfg.thread_count > 1);

    ThreadPool pool;
    ThreadContext single_ctx = {0};

    if (use_parallel) {
        // Create thread pool
        if (threadpool_create(&pool, cfg.thread_count, parallel_worker, &global) < 0) {
            fprintf(stderr, "error: failed to create thread pool\n");
            use_parallel = false;
        } else {
            global.pool = &pool;
        }
    }

    if (!use_parallel) {
        single_ctx.global = &global;
        file_buffer_init(&single_ctx.fb, 64 * 1024);
        single_ctx.exec_ctx_initialized = false;
    }

    WalkContext walk_ctx = {
        .global = &global,
        .tctx = use_parallel ? NULL : &single_ctx,
        .found_count = 0
    };

    // Process each path
    for (size_t i = 0; i < cfg.path_count; i++) {
        const char *path = cfg.paths[i];

        struct stat st;
        if (stat(path, &st) < 0) {
            fprintf(stderr, "error: cannot access '%s'\n", path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            walk_directory(path, &walk_cfg,
                          use_parallel ? walk_callback_parallel : walk_callback_single,
                          &walk_ctx);
        } else if (S_ISREG(st.st_mode)) {
            if (use_parallel) {
                threadpool_submit(&pool, path);
            } else {
                if (search_file_worker(path, &single_ctx)) {
                    walk_ctx.found_count++;
                }
            }
        }

        if (atomic_load(&global.stop_requested)) {
            break;
        }
    }

    // Cleanup
    if (use_parallel) {
        // Get match count before destroying the pool
        found_count = threadpool_files_matched(&pool);
        threadpool_destroy(&pool);
    } else {
        file_buffer_free(&single_ctx.fb);
        if (single_ctx.exec_ctx_initialized) {
            regex_exec_ctx_free(&single_ctx.exec_ctx);
        }
        found_count = walk_ctx.found_count;
    }

    pthread_mutex_destroy(&global.output_mutex);

    if (pat.is_regex) {
        regex_free(pat.regex);
    } else {
        prefilter_free(pat.prefilter);
        free(pat.prefilter);
    }
    config_free(&cfg);

    return found_count > 0 ? 0 : 1;
}
