// Small record value churn: every iteration builds a new {x, y} record.
#include <stdio.h>

typedef struct {
    double x, y;
} P;

static P add(P a, P b) {
    return (P){a.x + b.x, a.y + b.y};
}

int main(void) {
    const long n = 600000000;
    const P one = {1.0, 0.5};
    P v = {0.0, 0.0};
    for (long i = 0; i < n; i++) {
        v = add(v, one);
    }
    printf("%.1f %.1f\n", v.x, v.y);
    return 0;
}
