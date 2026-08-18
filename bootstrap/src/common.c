#include "common.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

void die(const char *s)
{
    fprintf(stderr, "c+: error: %s\n", s);
    exit(1);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

void buffer_init(Buffer *b)
{
    memset(b, 0, sizeof(*b));
}

void buffer_reserve(Buffer *b, size_t n)
{
    size_t need = b->len + n + 1, cap;
    if (need <= b->cap) return;
    cap = b->cap ? b->cap : 256;
    while (cap < need) cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buffer_putc(Buffer *b, char c)
{
    buffer_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = 0;
}

void buffer_puts(Buffer *b, const char *s)
{
    size_t n = strlen(s);
    buffer_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

void buffer_free(Buffer *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *s;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    s = xmalloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(s);
        return NULL;
    }
    fclose(f);
    s[n] = 0;
    return s;
}

char *path_join(const char *a, const char *b)
{
    size_t len_a = strlen(a), len_b = strlen(b);
    char *s = xmalloc(len_a + len_b + 2);
    memcpy(s, a, len_a);
    if (len_a && a[len_a - 1] != '/' && a[len_a - 1] != '\\') s[len_a++] = '/';
    memcpy(s + len_a, b, len_b + 1);
    return s;
}

char *path_join3(const char *a, const char *b, const char *c)
{
    char *tmp = path_join(a, b);
    char *out = path_join(tmp, c);
    free(tmp);
    return out;
}

bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

char *get_cwd(void)
{
#ifdef _WIN32
    char buf[4096];
    if (!_getcwd(buf, sizeof(buf))) return NULL;
    return xstrdup(buf);
#else
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return NULL;
    return xstrdup(buf);
#endif
}

char *dir_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *back = strrchr(path, '\\');
    size_t n;
    if (slash && back && back > slash) slash = back;
    else if (!slash) slash = back;
    if (!slash) return xstrdup(".");
    n = (size_t)(slash - path);
    if (!n) return xstrdup(".");
    {
        char *s = xmalloc(n + 1);
        memcpy(s, path, n);
        s[n] = 0;
        return s;
    }
}

// Declare the external getter from transpiler.c
const char *get_libc_path(void);

char *resolve_include_path(const char *from_file, const char *spec, bool angle)
{
    char *cwd = get_cwd();
    char *from_dir = dir_name(from_file);
    char *base = from_dir ? from_dir : xstrdup(".");
    size_t i;
    const char *variants[] = {"", ".hp", ".h+", ".h"};
    char *candidates[32];
    size_t count = 0;

    if (!cwd) cwd = xstrdup(".");

    for (i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
        char *filename = xmalloc(strlen(spec) + strlen(variants[i]) + 1);
        strcpy(filename, spec);
        strcat(filename, variants[i]);
        candidates[count++] = path_join(base, filename);
        free(filename);
    }
    
    if (angle) {
        const char *lib_dir = get_libc_path();
        for (i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
            char *filename = xmalloc(strlen(spec) + strlen(variants[i]) + 1);
            strcpy(filename, spec);
            strcat(filename, variants[i]);
            candidates[count++] = path_join(cwd, filename);
            // Use the absolute installation libc+ path instead of cwd/libc+
            candidates[count++] = path_join(lib_dir, filename);
            free(filename);
        }
    }

    for (i = 0; i < count; ++i) {
        if (file_exists(candidates[i])) {
            char *resolved = candidates[i];
            size_t j;
            for (j = i + 1; j < count; ++j) free(candidates[j]);
            free(cwd);
            free(base);
            return resolved;
        }
    }

    for (i = 0; i < count; ++i) free(candidates[i]);
    free(cwd);
    free(base);
    return NULL;
}