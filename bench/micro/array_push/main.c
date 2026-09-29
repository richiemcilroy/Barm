// Push N floats into a growable array, then sum it with a pointer walk and with an indexed loop.
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    double *data;
    size_t len;
    size_t cap;
} Vec;

static void vec_push(Vec *v, double x) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->data = realloc(v->data, v->cap * sizeof(double));
        if (!v->data) abort();
    }
    v->data[v->len++] = x;
}

int main(void) {
    const long n = 30000000;
    Vec xs = {0};
    for (long i = 0; i < n; i++) {
        vec_push(&xs, i * 0.5);
    }
    double sum_of = 0.0;
    for (const double *p = xs.data, *end = xs.data + xs.len; p < end; p++) {
        sum_of += *p;
    }
    double sum_idx = 0.0;
    for (size_t i = 0; i < xs.len; i++) {
        sum_idx += xs.data[i];
    }
    printf("%zu %.1f %.1f\n", xs.len, sum_of, sum_idx);
    free(xs.data);
    return 0;
}
