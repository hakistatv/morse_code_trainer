#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The dichotomic ("Koch") Morse tree: start at the root, a dot descends
 * one way, a dash the other, and most nodes carry a letter/digit. This is
 * the same structure engraved on the physical Morse Code Trainer Card --
 * keying dots/dashes walks a pointer down the tree and whatever node you
 * land on is the decoded character.
 *
 * Pure C, no ESP-IDF dependencies, so components/morse_tree/test/ can
 * build and run it on the host. All state is a single module-global
 * pointer -- there is exactly one tree and one traversal position. Not
 * thread-safe: morse_key owns it and drives it from one task (see
 * morse_key.c).
 */

/* Longest code in the table is 6 symbols (e.g. "-.-.-." style
 * punctuation), so the tree is 6 levels deep below the root. */
#define MORSE_TREE_MAX_DEPTH 6

/* Builds the tree from the internal ITU table and resets the traversal
 * position to the root. Call once at startup before anything else here. */
void morse_tree_init(void);

/* Moves the traversal position back to the root and clears the entered
 * dot/dash sequence. Call after committing a character. */
void morse_tree_reset(void);

/* Descend one level. Returns false (and does not move) if that would go
 * past MORSE_TREE_MAX_DEPTH -- the sequence is then "overflowed" and
 * morse_tree_current_letter() will report 0 until the next reset. */
bool morse_tree_dot(void);
bool morse_tree_dash(void);

/* The character at the current node, or 0 if the current node has none
 * (an intermediate branch point, or an overflowed sequence). A caller
 * committing a letter treats 0 as '?'. */
char morse_tree_current_letter(void);

/* The dot/dash sequence entered so far as an ASCII string of '.' and '-'
 * (e.g. ".-" after a dot then a dash). Empty string at the root. The
 * returned pointer is to internal storage, valid until the next
 * dot/dash/reset call. */
const char *morse_tree_current_code(void);

/* Current depth: 0 at the root, 1 after one symbol, etc. */
int morse_tree_depth(void);

/* Renders an "autocomplete" list: every letter/digit whose ITU code
 * begins with the dot/dash sequence entered so far, closest completions
 * first, one per line, e.g. after keying "-." :
 *
 *   6 possible
 *   > -.     N
 *     -..    D
 *     -.-    K
 *     -.-.   C
 *     -..-   X
 *     -.--   Y
 *
 * ('>' marks the code that exactly equals what's been keyed -- i.e. the
 * character that would commit right now). At the root it returns a short
 * hint; an overflowed / impossible sequence returns "no match". Writes at
 * most `n` bytes including the NUL. */
void morse_tree_render_candidates(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
