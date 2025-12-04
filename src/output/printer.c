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
    p->printed_filename = false;
}

size_t find_line_start(const uint8_t *data, size_t pos) {
    if (pos == 0) return 0;

    // Search backwards for newline
    size_t i = pos;
    while (i > 0 && data[i - 1] != '\n') {
        i--;
    }
    return i;
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

    // Count mode: just count, print at end
    if (p->config->count_only) {
        return;
    }

    // Find the line containing the match
    size_t line_start = find_line_start(p->data, match_pos);
    size_t line_end = find_line_end(p->data, p->data_len, match_pos);

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
        printf("%zu\n", p->match_count);
    }
}
