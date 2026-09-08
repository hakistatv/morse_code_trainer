#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A tiny bundled dictionary for Practice mode, bucketed by word length.
 * Every word is uppercase A-Z only -- the Morse table in morse_tree.c
 * carries A-Z, 0-9 and a little punctuation, and Practice only scores
 * letters, so the list is deliberately letters-only.
 *
 * Pure C, no ESP-IDF dependencies, so components/word_list/test/ builds
 * and runs it on the host. All data is static const; nothing here has
 * mutable state, so it is safe to call from any task.
 */

/* Shortest / longest word length the picker will serve. */
#define WORD_LIST_MIN_LEN 1
#define WORD_LIST_MAX_LEN 6

/* How many words are in the length-`len` bucket. 0 if `len` is out of
 * [WORD_LIST_MIN_LEN, WORD_LIST_MAX_LEN]. */
int word_list_bucket_size(int len);

/* A pointer to a static uppercase word of exactly `len` letters, chosen
 * as bucket[rnd % bucket_size] -- pass a fresh esp_random() (or any
 * value) for `rnd`. Returns NULL if `len` is out of range. The returned
 * pointer is to static storage and is valid forever. */
const char *word_list_pick(int len, uint32_t rnd);

#ifdef __cplusplus
}
#endif
