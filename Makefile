# notagrep - A fast, minimal grep alternative
# Build system for macOS (Intel/Apple Silicon) and Linux

CC ?= cc

# Detect OS and architecture
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Base flags
CFLAGS_COMMON = -std=c11 -Wall -Wextra -Wpedantic

# macOS doesn't need _POSIX_C_SOURCE and it causes conflicts
ifneq ($(UNAME_S),Darwin)
    CFLAGS_COMMON += -D_POSIX_C_SOURCE=200809L
endif

CFLAGS_RELEASE = $(CFLAGS_COMMON) -O3 -flto
CFLAGS_DEBUG = $(CFLAGS_COMMON) -g -O0 -fsanitize=address,undefined -DDEBUG
LDFLAGS = -lpthread
LDFLAGS_DEBUG = -lpthread -fsanitize=address,undefined

# Architecture-specific optimizations
ifeq ($(UNAME_M),arm64)
    CFLAGS_COMMON += -DUSE_NEON
    # Apple Silicon doesn't support -march=native the same way
    ifeq ($(UNAME_S),Darwin)
        CFLAGS_RELEASE += -mcpu=apple-m1
    endif
else ifeq ($(UNAME_M),x86_64)
    CFLAGS_COMMON += -DUSE_SSE2
    CFLAGS_RELEASE += -march=native
endif

SRCDIR = src
OBJDIR = obj
TARGET = notagrep

# Find all source files
SOURCES = $(shell find $(SRCDIR) -name '*.c')
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
OBJECTS_DEBUG = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/debug/%.o)

# Default target: release build
all: CFLAGS = $(CFLAGS_RELEASE)
all: $(TARGET)

# Debug build with sanitizers
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: LDFLAGS = $(LDFLAGS_DEBUG)
debug: $(TARGET)-debug

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)

$(TARGET)-debug: $(OBJECTS_DEBUG)
	$(CC) $(CFLAGS) $(OBJECTS_DEBUG) -o $@ $(LDFLAGS)

# Release object files
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_RELEASE) -I$(SRCDIR) -c $< -o $@

# Debug object files
$(OBJDIR)/debug/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_DEBUG) -I$(SRCDIR) -c $< -o $@

# Run tests
test: $(TARGET)
	@echo "Running tests..."
	@$(MAKE) -C tests

# Clean build artifacts
clean:
	rm -rf $(OBJDIR) $(TARGET) $(TARGET)-debug

# Install to /usr/local/bin
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# Show configuration
info:
	@echo "Architecture: $(UNAME_M)"
	@echo "Compiler: $(CC)"
	@echo "CFLAGS: $(CFLAGS_RELEASE)"
	@echo "Sources: $(SOURCES)"

# Generate benchmark data
bench-data:
	@mkdir -p bench/data
	@./bench/gen_data.sh

# Run benchmarks
bench: $(TARGET) bench-data
	@./bench/run.sh

.PHONY: all debug test clean install info bench bench-data
