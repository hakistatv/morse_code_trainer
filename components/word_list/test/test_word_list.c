/*
 * Host unit test for word_list -- no ESP-IDF, no hardware.
 *
 *   cc -I.. -Wall -Wextra test_word_list.c ../word_list.c -o /tmp/test_word_list
 *   /tmp/test_word_list
 *
 * (or run ./run.sh from this directory)
 */

#include <stdio.h>
#include <string.h>
#include "word_list.h"

static int g_fail;

#define CHECK(cond, ...)                       \
    do {                                       \
        if (cond) {                            \
            printf("  ok    ");                \
            printf(__VA_ARGS__);               \
            printf("\n");                      \
        } else {                               \
            printf("  FAIL  ");                \
            printf(__VA_ARGS__);               \
            printf("\n");                      \
            g_fail++;                          \
        }                                      \
    } while (0)

static int is_upper_az(const char *w)
{
    for (const char *c = w; *c; c++) {
        if (*c < 'A' || *c > 'Z') {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    for (int len = WORD_LIST_MIN_LEN; len <= WORD_LIST_MAX_LEN; len++) {
        int n = word_list_bucket_size(len);
        CHECK(n > 0, "bucket %d has %d words", len, n);

        /* Every entry in the bucket is uppercase A-Z and exactly `len` long. */
        int bad_case = 0, bad_len = 0;
        for (int i = 0; i < n; i++) {
            const char *w = word_list_pick(len, (unsigned)i);
            if (!w || !is_upper_az(w)) {
                bad_case++;
            }
            if (!w || (int)strlen(w) != len) {
                bad_len++;
                printf("        (offender: \"%s\" in bucket %d)\n", w ? w : "(null)", len);
            }
        }
        CHECK(bad_case == 0, "bucket %d: all words are uppercase A-Z", len);
        CHECK(bad_len == 0, "bucket %d: all words are %d letters", len, len);

        /* pick() stays in range and is stable for a fixed rnd. */
        const char *a = word_list_pick(len, 12345u);
        const char *b = word_list_pick(len, 12345u);
        CHECK(a && a == b, "bucket %d: pick(rnd) is deterministic", len);
        CHECK(word_list_pick(len, 0u) == word_list_pick(len, (uint32_t)n),
              "bucket %d: rnd wraps modulo bucket size", len);
    }

    /* Out-of-range lengths are rejected, not crashed on. */
    CHECK(word_list_bucket_size(0) == 0, "bucket size for len 0 is 0");
    CHECK(word_list_bucket_size(WORD_LIST_MAX_LEN + 1) == 0,
          "bucket size for len %d is 0", WORD_LIST_MAX_LEN + 1);
    CHECK(word_list_pick(0, 0u) == NULL, "pick(len 0) is NULL");
    CHECK(word_list_pick(99, 0u) == NULL, "pick(len 99) is NULL");
    CHECK(word_list_pick(-1, 0u) == NULL, "pick(len -1) is NULL");

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
