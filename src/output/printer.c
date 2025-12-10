#include "output.h"
#include <stdio.h>
#include <string.h>

void printer_init(Printer *p, const OutputConfig *config, const char *filename,
                  const uint8_t *data, size_t data_len) {
    p->config = config;
    p->filename = filename;
    p->data = data;
    p->data_len = data_len;
    p->match_count = 0;
    p->lines_matched = 0;
    p->last_line_start = 0;
    p->last_line_end = 0;  // End of current tracked line (position of newline)
    p->printed_filename = false;
    p->printed_any_line = false;
}

// Fallback: scan backward for line start (used when match is before cached position)
static size_t find_line_start_backward(const uint8_t *data, size_t pos) {
    if (pos == 0) return 0;
    size_t i = pos;
    while (i > 0 && data[i - 1] != '\n') {
        i--;
    }
    return i;
}

// Optimized: find line containing pos, using cached line boundaries when possible
// Returns line_start, updates *line_end_out
static size_t find_line_fast(Printer *p, size_t pos, size_t *line_end_out) {
    // If position is within the cached line, return cached values
    if (p->printed_any_line && pos >= p->last_line_start && pos <= p->last_line_end) {
        *line_end_out = p->last_line_end;
        return p->last_line_start;
    }

    // If position is after the cached line end, scan forward
    if (p->printed_any_line && pos > p->last_line_end) {
        // Start from after the last newline
        size_t scan_start = p->last_line_end + 1;
        size_t line_start = scan_start;

        // Scan forward through lines until we find the one containing pos
        while (line_start <= pos) {
            // Find end of this line
            size_t line_end = line_start;
            while (line_end < p->data_len && p->data[line_end] != '\n') {
                line_end++;
            }

            // Is pos within this line?
            if (pos >= line_start && pos <= line_end) {
                *line_end_out = line_end;
                return line_start;
            }

            // Move to next line
            line_start = line_end + 1;
        }
    }

    // Fallback: scan backward (for first match or out-of-order matches)
    size_t line_start = find_line_start_backward(p->data, pos);

    // Find line end
    size_t line_end = pos;
    while (line_end < p->data_len && p->data[line_end] != '\n') {
        line_end++;
    }
    *line_end_out = line_end;
    return line_start;
}

// Legacy function for compatibility (used by find_line_number)
size_t find_line_start(const uint8_t *data, size_t pos) {
    return find_line_start_backward(data, pos);
}

size_t find_line_end(const uint8_t *data, size_t data_len, size_t pos) {
    // Search forwards for newline
    size_t i = pos;
    while (i < data_len && data[i] != '\n') {
        i++;
    }
    return i;
}

size_t find_line_number(const uint8_t *data, size_t data_len, size_t pos) {
    (void)data_len;  // Unused but kept for consistency
    size_t line = 1;
    for (size_t i = 0; i < pos; i++) {
        if (data[i] == '\n') {
            line++;
        }
    }
    return line;
}

void printer_match(Printer *p, size_t match_pos, size_t match_len) {
    (void)match_len;  // Not used for basic output (could highlight match later)

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

    // Find the line containing the match using fast forward scanning
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

    // Print line number if configured
    if (p->config->show_line_numbers) {
        size_t line_num = find_line_number(p->data, p->data_len, match_pos);
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
