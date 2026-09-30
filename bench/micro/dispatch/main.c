// Virtual method calls: an array of shapes of three classes, `area()` called 50M times.
#include <stdio.h>
#include <stdlib.h>

typedef struct Shape Shape;
typedef struct { double (*area)(const Shape *); } VTable;
struct Shape { const VTable *vt; double a, b; };

static double circle_area(const Shape *s) { return 3.141592653589793 * s->a * s->a; }
static double rect_area(const Shape *s) { return s->a * s->b; }
static double tri_area(const Shape *s) { return 0.5 * s->a * s->b; }
static const VTable CIRCLE = { circle_area }, RECT = { rect_area }, TRI = { tri_area };

int main(void) {
    long long state = 42;
    Shape *shapes[1000];
    for (int i = 0; i < 1000; i++) {
        state = (state * 16807) % 2147483647;
        double a = (double)(state % 100) / 10 + 1;
        state = (state * 16807) % 2147483647;
        double b = (double)(state % 100) / 10 + 1;
        long long kind = state % 3;
        Shape *s = malloc(sizeof(Shape));
        s->vt = kind == 0 ? &CIRCLE : kind == 1 ? &RECT : &TRI;
        s->a = a;
        s->b = b;
        shapes[i] = s;
    }
    double total = 0.0;
    for (int round = 0; round < 50000; round++)
        for (int i = 0; i < 1000; i++) total += shapes[i]->vt->area(shapes[i]);
    printf("%.3f\n", total);
    return 0;
}
