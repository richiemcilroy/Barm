// Build strings with snprintf, join them with ",", split them back into owned strings and measure them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const long n = 3000000;

    char **parts = malloc(n * sizeof(char *));
    size_t *lens = malloc(n * sizeof(size_t));
    if (!parts || !lens) abort();
    char buf[64];
    for (long i = 0; i < n; i++) {
        int len = snprintf(buf, sizeof buf, "item-%ld-%ld", i, i % 7);
        parts[i] = malloc(len + 1);
        if (!parts[i]) abort();
        memcpy(parts[i], buf, len + 1);
        lens[i] = len;
    }

    size_t total = n > 0 ? n - 1 : 0;
    for (long i = 0; i < n; i++) total += lens[i];
    char *joined = malloc(total + 1);
    if (!joined) abort();
    char *w = joined;
    for (long i = 0; i < n; i++) {
        if (i > 0) *w++ = ',';
        memcpy(w, parts[i], lens[i]);
        w += lens[i];
    }
    *w = '\0';
    size_t joined_len = (size_t)(w - joined);

    size_t back_cap = 16, back_len = 0;
    char **back = malloc(back_cap * sizeof(char *));
    if (!back) abort();
    const char *start = joined;
    for (;;) {
        const char *comma = strchr(start, ',');
        size_t len = comma ? (size_t)(comma - start) : strlen(start);
        if (back_len == back_cap) {
            back_cap *= 2;
            back = realloc(back, back_cap * sizeof(char *));
            if (!back) abort();
        }
        back[back_len] = malloc(len + 1);
        if (!back[back_len]) abort();
        memcpy(back[back_len], start, len);
        back[back_len][len] = '\0';
        back_len++;
        if (!comma) break;
        start = comma + 1;
    }

    size_t bytes = 0;
    long threes = 0;
    for (size_t i = 0; i < back_len; i++) {
        size_t len = strlen(back[i]);
        bytes += len;
        if (len >= 2 && strcmp(back[i] + len - 2, "-3") == 0) threes++;
    }
    printf("%zu %zu %zu %ld\n", back_len, joined_len, bytes, threes);

    for (long i = 0; i < n; i++) free(parts[i]);
    for (size_t i = 0; i < back_len; i++) free(back[i]);
    free(parts);
    free(lens);
    free(back);
    free(joined);
    return 0;
}
