#ifndef NOTAGREP_OUTPUT_H
#define NOTAGREP_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Output configuration
typedef struct {
    bool show_filename;
    bool show_line_numbers;
    bool list_files_only;      // -l mode
    bool count_only;           // -c mode
    bool quiet;                // -q mode
} OutputConfig;

// Printer state for a single file
typedef struct {
    const OutputConfig *config;
    const char *filename;
    const uint8_t *data;
    size_t data_len;
    size_t match_count;
    size_t lines_matched;      // Number of unique lines with matches
    size_t last_line_start;    // Start of last printed line (to avoid duplicates)
    size_t last_line_end;      // End of last printed line (for forward scanning)
    bool printed_filename;     // For -l mode
    bool printed_any_line;     // Whether we've printed any line yet

    // Incremental line number tracking (for -n mode optimization)
    size_t last_line_number;      // Line number of last processed line
    size_t last_line_number_pos;  // Position (start) of that line
} Printer;

// Initialize printer for a file
void printer_init(Printer *p, const OutputConfig *config, const char *filename,
                  const uint8_t *data, size_t data_len);

// Print a match at the given byte position
// Finds the containing line and prints it with optional line number
void printer_match(Printer *p, size_t match_pos, size_t match_len);

// Finalize output for this file (print count if -c mode)
void printer_finish(Printer *p);

// Calculate line number for a byte position
// Returns 1-based line number
size_t find_line_number(const uint8_t *data, size_t data_len, size_t pos);

// Find start of line containing pos
size_t find_line_start(const uint8_t *data, size_t pos);

// Find end of line containing pos (position of newline or end of data)
size_t find_line_end(const uint8_t *data, size_t data_len, size_t pos);

#endif // NOTAGREP_OUTPUT_H
