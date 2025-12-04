#ifndef NOTAGREP_AHOCORASICK_H
#define NOTAGREP_AHOCORASICK_H

// Clean-room Aho-Corasick implementation
// Based on the algorithm from "Efficient String Matching: An Aid to
// Bibliographic Search" by Aho and Corasick (1975)
//
// This provides O(n + m + matches) multi-pattern string matching,
// where n is text length and m is total pattern length.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum number of patterns supported
#define AC_MAX_PATTERNS 64

// Maximum total size of all patterns
#define AC_MAX_TOTAL_LEN 4096

// Opaque Aho-Corasick automaton handle
typedef struct AhoCorasick AhoCorasick;

// Match callback: called for each pattern match found
// pos: position in text where match starts
// len: length of matched pattern
// pattern_id: index of matched pattern (as passed to ac_add_pattern)
// ctx: user context pointer
typedef void (*ac_match_cb)(size_t pos, size_t len, int pattern_id, void *ctx);

// Create a new Aho-Corasick automaton
// case_insensitive: if true, matching is case-insensitive
// Returns NULL on allocation failure
AhoCorasick *ac_create(bool case_insensitive);

// Add a pattern to the automaton
// pattern: the pattern bytes
// len: length of pattern
// id: user-defined pattern ID (returned in match callback)
// Returns 0 on success, -1 on error (too many patterns, etc.)
// Must be called before ac_compile()
int ac_add_pattern(AhoCorasick *ac, const uint8_t *pattern, size_t len, int id);

// Compile the automaton after adding all patterns
// Builds failure links for efficient matching
// Returns 0 on success, -1 on error
// Must be called before ac_search() or ac_contains()
int ac_compile(AhoCorasick *ac);

// Search text for all pattern matches
// text: input text to search
// len: length of text
// callback: function called for each match
// ctx: user context passed to callback
// Returns total number of matches found
size_t ac_search(const AhoCorasick *ac, const uint8_t *text, size_t len,
                 ac_match_cb callback, void *ctx);

// Check if any pattern matches in the text
// Returns true if at least one pattern is found
bool ac_contains(const AhoCorasick *ac, const uint8_t *text, size_t len);

// Free the automaton and all associated memory
void ac_free(AhoCorasick *ac);

// Get the number of patterns in the automaton
size_t ac_pattern_count(const AhoCorasick *ac);

#endif // NOTAGREP_AHOCORASICK_H
