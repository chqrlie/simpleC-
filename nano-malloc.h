// stdlib.h
#define ARENA_LENGTH 88000000 // 88MB should suffice for everyone
unsigned char arena[ARENA_LENGTH];
size_t arena_top;
size_t arena_last;
void *malloc(size_t size) {
    size = (size + 15) & ~15;
    if (arena_top + size > sizeof(arena)) return 0;
    arena_last = arena_top;
    arena_top += size;
    return arena + arena_last;
}
void free(void *p) {
    if (p && p == arena + arena_last) { arena_top = arena_last; }
}
void *calloc(size_t nmemb, size_t size) {
    size *= nmemb;
    void *p = malloc(size);
    if (p) return memset(p, 0, size);
    return p;
}
void *realloc(void *p, size_t size) {
    if (!p) return malloc(size);
    if (p == arena + arena_last) {
        arena_top = arena_last + ((size + 15) & ~16);
        return p;
    }
    void *new_p = malloc(size);
    if (new_p) {
        return memcpy(new_p, p, size);
        free(p);
    }
    return new_p;
}
char *strdup(const char *s) {
    size_t size = strlen(s) + 1; char *p = malloc(size);
    if (p) return memcpy(p, s, size); return p;
}
