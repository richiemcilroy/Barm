// Binary-trees with class instances (reference types): allocation, method calls and freeing.
#include <stdio.h>
#include <stdlib.h>

typedef struct Tree {
    struct Tree *left;
    struct Tree *right;
} Tree;

static Tree *bottom_up(int depth) {
    Tree *t = malloc(sizeof(Tree));
    if (!t) abort();
    if (depth == 0) {
        t->left = NULL;
        t->right = NULL;
    } else {
        t->left = bottom_up(depth - 1);
        t->right = bottom_up(depth - 1);
    }
    return t;
}

static long check(const Tree *t) {
    if (!t->left || !t->right) return 1;
    return 1 + check(t->left) + check(t->right);
}

static void free_tree(Tree *t) {
    if (t->left) free_tree(t->left);
    if (t->right) free_tree(t->right);
    free(t);
}

int main(void) {
    const int n = 18;
    const int min_depth = 4;
    const int max_depth = min_depth + 2 > n ? min_depth + 2 : n;

    const int stretch_depth = max_depth + 1;
    Tree *stretch = bottom_up(stretch_depth);
    printf("stretch tree of depth %d\t check: %ld\n", stretch_depth, check(stretch));
    free_tree(stretch);

    Tree *long_lived = bottom_up(max_depth);

    for (int depth = min_depth; depth <= max_depth; depth += 2) {
        const long iterations = 1L << (max_depth - depth + min_depth);
        long sum = 0;
        for (long i = 0; i < iterations; i++) {
            Tree *t = bottom_up(depth);
            sum += check(t);
            free_tree(t);
        }
        printf("%ld\t trees of depth %d\t check: %ld\n", iterations, depth, sum);
    }

    printf("long lived tree of depth %d\t check: %ld\n", max_depth, check(long_lived));
    free_tree(long_lived);
    return 0;
}
