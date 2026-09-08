/*
 * Host unit test for morse_tree -- no ESP-IDF, no hardware.
 *
 *   cc -I.. -Wall -Wextra test_morse_tree.c ../morse_tree.c -o /tmp/test_morse_tree
 *   /tmp/test_morse_tree
 *
 * (or run ./run.sh from this directory)
 */

#include <stdio.h>
#include <string.h>
#include "morse_tree.h"

static int g_fail;

static void key(const char *seq)
{
    for (const char *c = seq; *c; c++) {
        if (*c == '.') {
            morse_tree_dot();
        } else if (*c == '-') {
            morse_tree_dash();
        }
    }
}

static void expect_decode(const char *seq, char want)
{
    morse_tree_reset();
    key(seq);
    char got = morse_tree_current_letter();
    if (got != want) {
        printf("  FAIL  \"%s\" -> '%c' (0x%02x), wanted '%c'\n",
               seq, got ? got : '?', (unsigned char)got, want);
        g_fail++;
    } else {
        printf("  ok    \"%s\" -> '%c'\n", seq, want ? want : '?');
    }
}

static void expect_code_string(const char *seq, const char *want)
{
    morse_tree_reset();
    key(seq);
    const char *got = morse_tree_current_code();
    if (strcmp(got, want) != 0) {
        printf("  FAIL  code after \"%s\" is \"%s\", wanted \"%s\"\n", seq, got, want);
        g_fail++;
    } else {
        printf("  ok    code after \"%s\" is \"%s\"\n", seq, want);
    }
}

static void expect_candidates(const char *seq, const char *want_substr)
{
    morse_tree_reset();
    key(seq);
    char buf[512];
    morse_tree_render_candidates(buf, sizeof(buf));
    if (strstr(buf, want_substr) == NULL) {
        printf("  FAIL  candidates after \"%s\" = [%s], expected to contain \"%s\"\n",
               seq, buf, want_substr);
        g_fail++;
    } else {
        printf("  ok    candidates after \"%s\" contain \"%s\"\n", seq, want_substr);
    }
}

int main(void)
{
    morse_tree_init();

    /* Every ITU letter and digit round-trips. */
    const char *alpha[26] = {
        ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---",
        "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-",
        "..-", "...-", ".--", "-..-", "-.--", "--..",
    };
    for (int i = 0; i < 26; i++) {
        expect_decode(alpha[i], (char)('A' + i));
    }
    const char *digit[10] = {
        "-----", ".----", "..---", "...--", "....-",
        ".....", "-....", "--...", "---..", "----.",
    };
    for (int i = 0; i < 10; i++) {
        expect_decode(digit[i], (char)('0' + i));
    }

    /* A branch point with no character of its own decodes as 0 ('?'). */
    expect_decode("", 0);       /* root */
    expect_decode("...-.", 0);  /* not a real code */

    /* Over-long sequence: past MORSE_TREE_MAX_DEPTH (6) -> overflow -> 0. */
    expect_decode(".......", 0);

    /* The entered dot/dash string is tracked verbatim. */
    expect_code_string(".-", ".-");
    expect_code_string("-...", "-...");
    expect_code_string("", "");

    /* Depth counter. */
    morse_tree_reset();
    key("-.-.");
    if (morse_tree_depth() != 4) {
        printf("  FAIL  depth after \"-.-.\" is %d, wanted 4\n", morse_tree_depth());
        g_fail++;
    } else {
        printf("  ok    depth after \"-.-.\" is 4\n");
    }

    /* Autocomplete candidate list. */
    expect_candidates("-.", "> -.     N");   /* exact match marked */
    expect_candidates("-.", "-.-.   C");     /* deeper completion listed */
    expect_candidates("", "key a dot");      /* hint at the root */
    expect_candidates(".......", "no match"); /* overflow */
    expect_candidates("...-.", "no match for ...-."); /* real prefix, no code */

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
