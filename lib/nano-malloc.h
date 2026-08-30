#ifndef ARENA_LENGTH
// stdlib.h
#define ARENA_LENGTH 3200000 // 3MB should suffice for everyone
//#define MALLOC_TRACE 1
unsigned char arena[ARENA_LENGTH];
size_t arena_top, arena_last;
void *malloc(size_t size) {
    size = (size + 7) & ~7;
#ifdef MALLOC_TRACE
    if (size >= MALLOC_TRACE) fprintf(stderr, "malloc(%zu) ", size);
#endif
    void *p = 0;
    if (arena_top + size <= sizeof(arena)) {
        arena_last = arena_top;
        arena_top += size;
        p = arena + arena_last;
    }
#ifdef MALLOC_TRACE
    if (size >= MALLOC_TRACE) fprintf(stderr, "-> %p (top=%zu)\n", p, arena_top);
#endif
    return p;
}
void free(void *p) {
#ifdef MALLOC_TRACE
    fprintf(stderr, "free(%p) (last=%p)\n", p, arena + arena_last);
#endif
    if (p == arena + arena_last) {
        memset(p, 0, arena_top - arena_last); arena_top = arena_last;
    }
}
void *calloc(size_t nmemb, size_t size) {
    return malloc(size * nmemb);
}
void *realloc(void *p, size_t size) {
    size = (size + 7) & ~7;
#ifdef MALLOC_TRACE
    if (size >= MALLOC_TRACE) fprintf(stderr, "realloc(%p, %zu) ", p, size);
#endif
    void *new_p = 0;
    if (p == arena + arena_last) {
        if (arena_last + size > ARENA_LENGTH) goto done;
        new_p = p;
        size_t last_top = arena_top; arena_top = arena_last + size;
        if (arena_top < last_top) memset(arena + arena_top, 0, last_top - arena_top);
    } else {
        new_p = malloc(size);
        if (new_p && p) {
            // size may be larger than original block size but OK
            p = memcpy(new_p, p, size);
        }
    }
done:
#ifdef MALLOC_TRACE
    if (size >= MALLOC_TRACE) fprintf(stderr, "-> %p, (top=%zu)\n", new_p, arena_top);
#endif
    return new_p;
}
char *strdup(const char *s) {
    size_t size = strlen(s) + 1; char *p = malloc(size);
    if (p) return memcpy(p, s, size); return p;
}
void malloc_stats(void) {
    fprintf(stderr, "system bytes = %ld\n"
            "in use bytes = %ld\n", ARENA_LENGTH, arena_top);
}
#endif
