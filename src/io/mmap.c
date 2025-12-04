#include "io.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Threshold below which we use read() instead of mmap()
// mmap has overhead for small files
#define MMAP_THRESHOLD (16 * 1024)

// Maximum buffer size for buffered reads
#define MAX_BUFFER_SIZE (256 * 1024 * 1024)  // 256 MB

static int file_open_mmap(FileBuffer *fb, int fd, size_t size) {
    void *addr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) {
        return -1;
    }

    // Advise the kernel we'll read sequentially
    madvise(addr, size, MADV_SEQUENTIAL);

    fb->mode = FILE_MODE_MMAP;
    fb->mmap_addr = addr;
    fb->mmap_len = size;
    fb->data = (const uint8_t *)addr;
    fb->size = size;
    return 0;
}

static int file_open_buffered(FileBuffer *fb, int fd, size_t size) {
    // Allocate or reallocate buffer
    if (fb->buffer_capacity < size) {
        size_t new_cap = size;
        if (new_cap > MAX_BUFFER_SIZE) {
            return -1;  // File too large for buffered mode
        }
        uint8_t *new_buf = realloc(fb->buffer, new_cap);
        if (!new_buf) {
            return -1;
        }
        fb->buffer = new_buf;
        fb->buffer_capacity = new_cap;
    }

    // Read entire file
    size_t total_read = 0;
    while (total_read < size) {
        ssize_t n = read(fd, fb->buffer + total_read, size - total_read);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;  // EOF
        total_read += n;
    }

    fb->mode = FILE_MODE_BUFFERED;
    fb->data = fb->buffer;
    fb->size = total_read;
    return 0;
}

int file_open(FileBuffer *fb, const char *path, size_t max_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }

    // Skip non-regular files
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }

    size_t size = (size_t)st.st_size;

    // Skip if exceeds max size
    if (max_size > 0 && size > max_size) {
        close(fd);
        return -1;
    }

    // Empty files
    if (size == 0) {
        fb->mode = FILE_MODE_BUFFERED;
        fb->fd = fd;
        fb->data = NULL;
        fb->size = 0;
        return 0;
    }

    fb->fd = fd;

    int result;
    // Use mmap for larger files, buffered for small files
    if (size >= MMAP_THRESHOLD) {
        result = file_open_mmap(fb, fd, size);
        if (result < 0) {
            // Fallback to buffered if mmap fails
            result = file_open_buffered(fb, fd, size);
        }
    } else {
        result = file_open_buffered(fb, fd, size);
    }

    if (result < 0) {
        close(fd);
        fb->fd = -1;
    }

    return result;
}

void file_close(FileBuffer *fb) {
    if (fb->mode == FILE_MODE_MMAP && fb->mmap_addr) {
        munmap(fb->mmap_addr, fb->mmap_len);
        fb->mmap_addr = NULL;
        fb->mmap_len = 0;
    }

    if (fb->fd >= 0) {
        close(fb->fd);
        fb->fd = -1;
    }

    fb->data = NULL;
    fb->size = 0;
    // Note: we don't free fb->buffer here - it's reused
}

bool file_is_binary(const FileBuffer *fb, size_t check_bytes) {
    if (!fb->data || fb->size == 0) {
        return false;
    }

    size_t to_check = fb->size < check_bytes ? fb->size : check_bytes;

    // Check for NUL bytes
    for (size_t i = 0; i < to_check; i++) {
        if (fb->data[i] == 0) {
            return true;
        }
    }

    return false;
}

void file_buffer_init(FileBuffer *fb, size_t initial_capacity) {
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;
    if (initial_capacity > 0) {
        fb->buffer = malloc(initial_capacity);
        if (fb->buffer) {
            fb->buffer_capacity = initial_capacity;
        }
    }
}

void file_buffer_free(FileBuffer *fb) {
    file_close(fb);
    if (fb->buffer) {
        free(fb->buffer);
        fb->buffer = NULL;
        fb->buffer_capacity = 0;
    }
}
