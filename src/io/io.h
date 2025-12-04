#ifndef NOTAGREP_IO_H
#define NOTAGREP_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FILE_MODE_MMAP,      // Memory-mapped file
    FILE_MODE_BUFFERED,  // Buffered read into heap buffer
} FileMode;

typedef struct {
    FileMode mode;
    int fd;
    const uint8_t *data;   // Pointer to file contents
    size_t size;           // File size

    // For mmap mode
    void *mmap_addr;
    size_t mmap_len;

    // For buffered mode
    uint8_t *buffer;
    size_t buffer_capacity;
} FileBuffer;

// Open a file and load its contents
// Tries mmap first, falls back to buffered read if mmap fails
// Returns 0 on success, -1 on error
int file_open(FileBuffer *fb, const char *path, size_t max_size);

// Close file and free resources
void file_close(FileBuffer *fb);

// Check if first N bytes contain NUL (binary detection)
// Returns true if file appears to be binary
bool file_is_binary(const FileBuffer *fb, size_t check_bytes);

// Initialize a reusable FileBuffer for buffered mode
// Call this once per thread for scratch buffer
void file_buffer_init(FileBuffer *fb, size_t initial_capacity);

// Free a reusable FileBuffer's internal buffer
void file_buffer_free(FileBuffer *fb);

#endif // NOTAGREP_IO_H
