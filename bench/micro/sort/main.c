// Generate N pseudo-random floats with a Park-Miller LCG and sort them with a comparator.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int compare_doubles(const void *pa, const void *pb) {
    double a = *(const double *)pa;
    double b = *(const double *)pb;
    return (a > b) - (a < b);
}

int main(void) {
    const size_t n = 3000000;
    int64_t state = 42;
    double *xs = malloc(n * sizeof(double));
    if (!xs) abort();
    for (size_t i = 0; i < n; i++) {
        state = (state * 16807) % 2147483647;
        xs[i] = (double)state / 2147483647;
    }
    qsort(xs, n, sizeof(double), compare_doubles);
    double checksum = 0.0;
    for (size_t i = 0; i < n; i += 1000) {
        checksum += xs[i];
    }
    printf("%.9f %.9f %.6f\n", xs[0], xs[n - 1], checksum);
    free(xs);
    return 0;
}
