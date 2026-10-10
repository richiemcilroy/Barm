// Insert N string keys into a string -> int map, then look every key up again.
// The map is a plain open-addressing hash table (FNV-1a, linear probing, cached hashes, owned keys).
#define _POSIX_C_SOURCE 200809L // strdup (glibc declares it only for POSIX under -std=c11)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key; // NULL = empty slot
    uint64_t hash;
    int64_t value;
} Entry;

typedef struct {
    Entry *entries;
    size_t cap; // power of two
    size_t len;
} Map;

static uint64_t hash(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static Entry *find_slot(Entry *entries, size_t cap, const char *key, uint64_t h) {
    size_t i = h & (cap - 1);
    while (entries[i].key && (entries[i].hash != h || strcmp(entries[i].key, key) != 0)) {
        i = (i + 1) & (cap - 1);
    }
    return &entries[i];
}

static void map_grow(Map *m) {
    size_t cap = m->cap ? m->cap * 2 : 16;
    Entry *entries = calloc(cap, sizeof(Entry));
    if (!entries) abort();
    for (size_t i = 0; i < m->cap; i++) {
        if (m->entries[i].key) {
            *find_slot(entries, cap, m->entries[i].key, m->entries[i].hash) = m->entries[i];
        }
    }
    free(m->entries);
    m->entries = entries;
    m->cap = cap;
}

static void map_set(Map *m, const char *key, int64_t value) {
    if ((m->len + 1) * 4 > m->cap * 3) map_grow(m);
    uint64_t h = hash(key);
    Entry *e = find_slot(m->entries, m->cap, key, h);
    if (!e->key) {
        e->key = strdup(key);
        e->hash = h;
        m->len++;
    }
    e->value = value;
}

static const int64_t *map_get(const Map *m, const char *key) {
    if (m->cap == 0) return NULL;
    Entry *e = find_slot(m->entries, m->cap, key, hash(key));
    return e->key ? &e->value : NULL;
}

static void map_free(Map *m) {
    for (size_t i = 0; i < m->cap; i++) free(m->entries[i].key);
    free(m->entries);
}

int main(void) {
    const int64_t n = 2000000;
    Map m = {0};
    char key[32];
    for (int64_t i = 0; i < n; i++) {
        snprintf(key, sizeof key, "k%lld", (long long)i);
        map_set(&m, key, i);
    }
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        snprintf(key, sizeof key, "k%lld", (long long)i);
        const int64_t *v = map_get(&m, key);
        if (!v) abort();
        sum += *v;
    }
    printf("%zu %lld\n", m.len, (long long)sum);
    map_free(&m);
    return 0;
}
