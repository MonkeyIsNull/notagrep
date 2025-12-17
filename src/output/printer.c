#include "output.h"
#include <stdio.h>
#include <string.h>

// Portable memrchr implementation (reverse memchr)
// Searches for last occurrence of c in the first n bytes of s
static inline const void *portable_memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    const unsigned char uc = (unsigned char)c;
    while (n-- > 0) {
        if (*--p == uc) {
            return p;
        }
    }
    return NULL;
}

void printer_init(Printer *p, const OutputConfig *config, const char *filename,
                  const uint8_t *data, size_t data_len) {
    p->config = config;
    p->filename = filename;
    p->data = data;
    p->data_len = data_len;
    p->match_count = 0;
    p->lines_matched = 0;
    p->last_line_start = 0;
    p->last_line_end = 0;
    p->printed_filename = false;
    p->printed_any_line = false;

    // Initialize incremental line number tracking
    p->last_line_number = 0;
    p->last_line_number_pos = 0;
}

// =============================================================================
// Phase 3: Backward scan using memrchr (SIMD-optimized on most platforms)
// =============================================================================
static size_t find_line_start_backward(const uint8_t *data, size_t pos) {
    if (pos == 0) return 0;
    // Use portable_memrchr for reverse search
    const void *nl = portable_memrchr(data, '\n', pos);
    return nl ? (size_t)((const uint8_t *)nl - data) + 1 : 0;
}

// =============================================================================
// Phase 1: Forward scanning using memchr (SIMD-optimized)
// =============================================================================

// Find line containing pos, using cached line boundaries when possible
// Returns line_start, updates *line_end_out
static size_t find_line_fast(Printer *p, size_t pos, size_t *line_end_out) {
    // If position is within the cached line, return cached values
    if (p->printed_any_line && pos >= p->last_line_start && pos <= p->last_line_end) {
        *line_end_out = p->last_line_end;
        return p->last_line_start;
    }

    // If position is after the cached line end, scan forward using memchr
    if (p->printed_any_line && pos > p->last_line_end) {
        size_t line_start = p->last_line_end + 1;

        // Skip lines until we find the one containing pos
        while (line_start <= pos) {
            // Use memchr for SIMD-optimized newline search
            const uint8_t *nl = memchr(p->data + line_start, '\n', p->data_len - line_start);
            size_t line_end = nl ? (size_t)(nl - p->data) : p->data_len;

            if (pos >= line_start && pos <= line_end) {
                *line_end_out = line_end;
                return line_start;
            }
            line_start = line_end + 1;
        }
    }

    // Fallback: scan backward for line start
    size_t line_start = find_line_start_backward(p->data, pos);

    // Find line end using memchr
    const uint8_t *nl = memchr(p->data + pos, '\n', p->data_len - pos);
    *line_end_out = nl ? (size_t)(nl - p->data) : p->data_len;

    return line_start;
}

// Legacy function for compatibility
size_t find_line_start(const uint8_t *data, size_t pos) {
    return find_line_start_backward(data, pos);
}

size_t find_line_end(const uint8_t *data, size_t data_len, size_t pos) {
    const uint8_t *nl = memchr(data + pos, '\n', data_len - pos);
    return nl ? (size_t)(nl - data) : data_len;
}

// =============================================================================
// Phase 2: Incremental line number counting using memchr
// =============================================================================

// Count newlines in range [start, end) using SIMD-optimized memchr
static size_t count_newlines_in_range(const uint8_t *data, size_t start, size_t end) {
    size_t count = 0;
    const uint8_t *p = data + start;
    const uint8_t *limit = data + end;

    while (p < limit) {
        const uint8_t *nl = memchr(p, '\n', limit - p);
        if (!nl) break;
        count++;
        p = nl + 1;
    }
    return count;
}

// Get line number incrementally - O(distance) instead of O(position)
static size_t get_line_number_incremental(Printer *p, size_t line_start) {
    if (p->last_line_number == 0) {
        // First time: count from file start to line_start
        size_t newlines = count_newlines_in_range(p->data, 0, line_start);
        p->last_line_number = newlines + 1;  // 1-indexed
        p->last_line_number_pos = line_start;
        return p->last_line_number;
    }

    // Incremental: count newlines between last position and current
    if (line_start > p->last_line_number_pos) {
        size_t newlines = count_newlines_in_range(p->data, p->last_line_number_pos, line_start);
        p->last_line_number += newlines;
    } else if (line_start < p->last_line_number_pos) {
        // Out-of-order match (shouldn't happen with forward-only search, but defensive)
        size_t newlines = count_newlines_in_range(p->data, 0, line_start);
        p->last_line_number = newlines + 1;
    }
    // If line_start == p->last_line_number_pos, same line, no update needed

    p->last_line_number_pos = line_start;
    return p->last_line_number;
}

// Legacy function - still available for external use but printer_match uses incremental
size_t find_line_number(const uint8_t *data, size_t data_len, size_t pos) {
    (void)data_len;
    // Use memchr for counting (still O(n) but SIMD-optimized)
    return count_newlines_in_range(data, 0, pos) + 1;
}

// =============================================================================
// Main match handler
// =============================================================================

void printer_match(Printer *p, size_t match_pos, size_t match_len) {
    (void)match_len;

    p->match_count++;

    // Quiet mode: don't print anything
    if (p->config->quiet) {
        return;
    }

    // List files mode: just print filename once
    if (p->config->list_files_only) {
        if (!p->printed_filename) {
            printf("%s\n", p->filename);
            p->printed_filename = true;
        }
        return;
    }

    // Fast path: if match is within the current line, skip (duplicate)
    if (p->printed_any_line && match_pos >= p->last_line_start &&
        match_pos <= p->last_line_end) {
        return;  // Same line as last match, skip
    }

    // Find the line containing the match using memchr-accelerated scanning
    size_t line_end;
    size_t line_start = find_line_fast(p, match_pos, &line_end);

    // This is a new line with a match - update cache
    p->lines_matched++;
    p->last_line_start = line_start;
    p->last_line_end = line_end;
    p->printed_any_line = true;

    // Count mode: just count lines, print at end
    if (p->config->count_only) {
        return;
    }

    // Print filename if configured
    if (p->config->show_filename) {
        printf("%s:", p->filename);
    }

    // Print line number if configured - use incremental counting
    if (p->config->show_line_numbers) {
        size_t line_num = get_line_number_incremental(p, line_start);
        printf("%zu:", line_num);
    }

    // Print the line content
    fwrite(p->data + line_start, 1, line_end - line_start, stdout);
    putchar('\n');
}

void printer_finish(Printer *p) {
    if (p->config->count_only && !p->config->quiet) {
        if (p->config->show_filename) {
            printf("%s:", p->filename);
        }
        // -c counts matching lines (like grep), not individual matches
        printf("%zu\n", p->lines_matched);
    }
}
