#include "io.h"

// Default number of bytes to check for binary detection
#define BINARY_CHECK_SIZE 4096

bool file_is_binary_default(const FileBuffer *fb) {
    return file_is_binary(fb, BINARY_CHECK_SIZE);
}
