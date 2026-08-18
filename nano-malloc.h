#ifndef ARENA_LENGTH
// stdlib.h
#define ARENA_LENGTH 4194304 // 4MB should suffice for everyone
unsigned char arena[ARENA_LENGTH];
size_t arena_top, arena_last;
void *malloc(size_t size) {
    size = (size + 7) & ~7;
    if (arena_top + size > sizeof(arena)) return 0;
    arena_last = arena_top;
    arena_top += size;
    return arena + arena_last;
}
void free(void *p) {
    if (p == arena + arena_last) {
        memset(p, 0, arena_top - arena_last); arena_top = arena_last;
    }
}
void *calloc(size_t nmemb, size_t size) {
    return malloc(size * nmemb);
}
void *realloc(void *p, size_t size) {
    size = (size + 7) & ~7;
    if (p == arena + arena_last) {
        size_t last_top = arena_top;
        arena_top = arena_last + size;
        if (arena_top < last_top) { memset(arena + arena_top, 0, last_top - arena_top); }
        return p;
    }
    void *new_p = malloc(size);
    if (new_p && p) {
        // size may be larger than original block size but OK
        return memcpy(new_p, p, size);
    }
    return new_p;
}
char *strdup(const char *s) {
    size_t size = strlen(s) + 1; char *p = malloc(size);
    if (p) return memcpy(p, s, size); return p;
}
#endif
