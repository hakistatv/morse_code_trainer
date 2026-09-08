/*
 * Dichotomic Morse tree, stored as a flat heap-indexed array:
 *   index 0        = root
 *   dot child of i = 2*i + 1
 *   dash child     = 2*i + 2
 * With MORSE_TREE_MAX_DEPTH == 6 the deepest index is 2^7 - 2 = 126, so a
 * 127-entry array covers every reachable node. s_letters[i] holds the
 * character at node i, or 0 for a branch point with no character.
 *
 * The ITU table below is copied verbatim from the sibling project
 * (../morse_code _listener/components/morse_decoder/morse_decoder.c:127) so
 * the trainer decodes exactly what that audio decoder does.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "morse_tree.h"

#define MORSE_TREE_MAX_NODES 127

typedef struct {
    const char *code; /* '.'/'-' string */
    char ascii;
} morse_entry_t;

static const morse_entry_t MORSE_TABLE[] = {
    {".-",'A'},{"-...",'B'},{"-.-.",'C'},{"-..",'D'},{".",'E'},{"..-.",'F'},
    {"--.",'G'},{"....",'H'},{"..",'I'},{".---",'J'},{"-.-",'K'},{".-..",'L'},
    {"--",'M'},{"-.",'N'},{"---",'O'},{".--.",'P'},{"--.-",'Q'},{".-.",'R'},
    {"...",'S'},{"-",'T'},{"..-",'U'},{"...-",'V'},{".--",'W'},{"-..-",'X'},
    {"-.--",'Y'},{"--..",'Z'},
    {"-----",'0'},{".----",'1'},{"..---",'2'},{"...--",'3'},{"....-",'4'},
    {".....",'5'},{"-....",'6'},{"--...",'7'},{"---..",'8'},{"----.",'9'},
    {".-.-.-",'.'},{"--..--",','},{"..--..",'?'},{"-..-.",'/'},{"-...-",'='},
};

static char s_letters[MORSE_TREE_MAX_NODES];

static int s_pos;                              /* current heap index */
static int s_depth;                            /* 0 at root */
static bool s_overflow;                        /* keyed past MAX_DEPTH */
static char s_code[MORSE_TREE_MAX_DEPTH + 1];  /* entered '.'/'-' so far */

static int child_index(int idx, char symbol)
{
    return (symbol == '.') ? (2 * idx + 1) : (2 * idx + 2);
}

void morse_tree_init(void)
{
    memset(s_letters, 0, sizeof(s_letters));
    for (size_t e = 0; e < sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]); e++) {
        int idx = 0;
        const char *c = MORSE_TABLE[e].code;
        for (; *c; c++) {
            idx = child_index(idx, *c);
        }
        if (idx >= 0 && idx < MORSE_TREE_MAX_NODES) {
            s_letters[idx] = MORSE_TABLE[e].ascii;
        }
    }
    morse_tree_reset();
}

void morse_tree_reset(void)
{
    s_pos = 0;
    s_depth = 0;
    s_overflow = false;
    s_code[0] = '\0';
}

static bool descend(char symbol)
{
    if (s_depth >= MORSE_TREE_MAX_DEPTH) {
        s_overflow = true;
        return false;
    }
    s_pos = child_index(s_pos, symbol);
    s_code[s_depth++] = symbol;
    s_code[s_depth] = '\0';
    return true;
}

bool morse_tree_dot(void)  { return descend('.'); }
bool morse_tree_dash(void) { return descend('-'); }

char morse_tree_current_letter(void)
{
    if (s_overflow || s_pos < 0 || s_pos >= MORSE_TREE_MAX_NODES) {
        return 0;
    }
    return s_letters[s_pos];
}

const char *morse_tree_current_code(void)
{
    return s_code;
}

int morse_tree_depth(void)
{
    return s_depth;
}

typedef struct {
    char letter;
    const char *code;
    int remaining; /* symbols still needed after the entered prefix */
} candidate_t;

static int candidate_cmp(const void *a, const void *b)
{
    const candidate_t *x = a;
    const candidate_t *y = b;
    if (x->remaining != y->remaining) {
        return x->remaining - y->remaining; /* closest completions first */
    }
    return (unsigned char)x->letter - (unsigned char)y->letter;
}

void morse_tree_render_candidates(char *buf, size_t n)
{
    if (n == 0) {
        return;
    }
    buf[0] = '\0';

    if (s_overflow) {
        snprintf(buf, n, "no match");
        return;
    }
    size_t plen = strlen(s_code);
    if (plen == 0) {
        snprintf(buf, n, "key a dot ( . ) or dash ( - )");
        return;
    }

    candidate_t list[64];
    int count = 0;
    for (size_t e = 0; e < sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]); e++) {
        const char *code = MORSE_TABLE[e].code;
        size_t clen = strlen(code);
        if (clen >= plen && strncmp(code, s_code, plen) == 0 && count < (int)(sizeof(list) / sizeof(list[0]))) {
            list[count].letter = MORSE_TABLE[e].ascii;
            list[count].code = code;
            list[count].remaining = (int)(clen - plen);
            count++;
        }
    }
    if (count == 0) {
        snprintf(buf, n, "no match for %s", s_code);
        return;
    }

    qsort(list, (size_t)count, sizeof(list[0]), candidate_cmp);

    size_t used = 0;
    int w = snprintf(buf, n, "%d possible", count);
    used = (w > 0 && (size_t)w < n) ? (size_t)w : n - 1;

    const int max_lines = 12; /* header + this many fits the on-screen list at 20px */
    for (int i = 0; i < count && i < max_lines && used < n - 1; i++) {
        w = snprintf(buf + used, n - used, "\n%s %-6s %c",
                     list[i].remaining == 0 ? ">" : " ", list[i].code, list[i].letter);
        if (w <= 0) {
            break;
        }
        used += ((size_t)w < n - used) ? (size_t)w : (n - used - 1);
    }
    if (count > max_lines && used < n - 1) {
        snprintf(buf + used, n - used, "\n  ...");
    }
}
