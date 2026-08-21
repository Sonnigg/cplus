#include "transpiler.h"
#include "common.h"

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <limits.h>
#endif

static void rewrite_line(Buffer *out, const char *line)
{
    buffer_puts(out, line);
}

/* ============================================================
 * PATH_MAX
 * ============================================================ */

#ifndef PATH_MAX
    #ifdef _WIN32
        #define PATH_MAX 32768
    #else
        #define PATH_MAX 4096
    #endif
#endif


/* ============================================================
 * Include cache
 * ============================================================ */

typedef struct IncludeCacheEntry
{
    char *path;
    char *source;
} IncludeCacheEntry;

static IncludeCacheEntry *include_cache = NULL;
static size_t include_cache_count = 0;
static size_t include_cache_capacity = 0;


static const char *include_cache_get(const char *path)
{
    for (size_t i = 0; i < include_cache_count; ++i)
    {
        if (strcmp(include_cache[i].path, path) == 0)
            return include_cache[i].source;
    }

    return NULL;
}


static void include_cache_put(const char *path, char *source)
{
    if (include_cache_count == include_cache_capacity)
    {
        include_cache_capacity =
            include_cache_capacity
                ? include_cache_capacity * 2
                : 16;

        include_cache = realloc(
            include_cache,
            include_cache_capacity * sizeof(*include_cache)
        );

        if (!include_cache)
            die("out of memory");
    }

    include_cache[include_cache_count].path = strdup(path);
    include_cache[include_cache_count].source = source;

    ++include_cache_count;
}


/* ============================================================
 * libc+ path
 *
 * Installation layout:
 *
 *     cplus/
 *     ├── bin/
 *     │   └── cplus.exe
 *     │
 *     └── libc+/
 *         ├── types.hp
 *         ├── io.hp
 *         └── ...
 *
 * ============================================================ */

static char libc_path[PATH_MAX];
static bool libc_path_initialized = false;

static void init_libc_path(void)
{
    if (libc_path_initialized)
        return;

    char exe_path[PATH_MAX];

#ifdef _WIN32

    DWORD len = GetModuleFileNameA(
        NULL,
        exe_path,
        sizeof(exe_path)
    );

    if (len == 0 || len >= sizeof(exe_path))
        die("could not determine compiler executable path");

#else

    ssize_t len = readlink(
        "/proc/self/exe",
        exe_path,
        sizeof(exe_path) - 1
    );

    if (len <= 0 || (size_t)len >= sizeof(exe_path) - 1)
        die("could not determine compiler executable path");

    exe_path[len] = '\0';

#endif

    /*
     * Find the executable's directory.
     *
     * Example:
     *
     *     C:\foo\cplus\bin\cplus.exe
     *                         ^
     *                         exe_path
     *
     * becomes:
     *
     *     C:\foo\cplus\bin
     */

    char *slash = strrchr(exe_path, '/');

#ifdef _WIN32
    char *backslash = strrchr(exe_path, '\\');

    if (!slash || (backslash && backslash > slash))
        slash = backslash;
#endif

    if (slash)
        *slash = '\0';
    else
        strcpy(exe_path, ".");

    /*
     * Now exe_path points to:
     *
     *     .../cplus/bin
     *
     * We need:
     *
     *     .../cplus/libc+
     *
     * Therefore remove the "bin" component.
     */

    slash = strrchr(exe_path, '/');

#ifdef _WIN32
    backslash = strrchr(exe_path, '\\');

    if (!slash || (backslash && backslash > slash))
        slash = backslash;
#endif

    if (!slash)
        die("could not determine compiler installation directory");

    *slash = '\0';

#ifdef _WIN32

    snprintf(
        libc_path,
        sizeof(libc_path),
        "%s\\libc+",
        exe_path
    );

#else

    snprintf(
        libc_path,
        sizeof(libc_path),
        "%s/libc+",
        exe_path
    );

#endif

    libc_path_initialized = true;
}

const char *get_libc_path(void)
{
    init_libc_path();
    return libc_path;
}

char *preprocess_source(const char *path, const char *src, int depth)
{
    Buffer out;
    const char *line_start = src;
    const char *end = src + strlen(src);

    buffer_init(&out);

    if (depth > 32)
        die("include nesting too deep");

    while (line_start < end)
    {
        const char *line_end = line_start;

        while (line_end < end && *line_end != '\n')
            ++line_end;

        {
            size_t n = (size_t)(line_end - line_start);
            char *line = xmalloc(n + 1);

            memcpy(line, line_start, n);
            line[n] = 0;

            if (n > 0 &&
                line[0] == '#' &&
                strncmp(line, "#include", 8) == 0)
            {
                char *spec = NULL;
                bool angle = false;
                char *p = line + 8;

                while (*p && isspace((unsigned char)*p))
                    ++p;

                if (*p == '<')
                {
                    const char *q = ++p;
                    angle = true;

                    while (*q && *q != '>')
                        ++q;

                    if (*q == '>')
                    {
                        size_t len = (size_t)(q - p);

                        spec = xmalloc(len + 1);
                        memcpy(spec, p, len);
                        spec[len] = 0;
                    }
                }
                else if (*p == '"')
                {
                    const char *q = ++p;

                    while (*q && *q != '"')
                        ++q;

                    if (*q == '"')
                    {
                        size_t len = (size_t)(q - p);

                        spec = xmalloc(len + 1);
                        memcpy(spec, p, len);
                        spec[len] = 0;
                    }
                }

                if (spec)
                {
                    bool do_inline = false;
                    size_t spec_len = strlen(spec);

                    /* Check if the included spec is targeted for inlining */
                    if (spec_len >= 3 && strcmp(spec + spec_len - 3, ".hp") == 0)
                        do_inline = true;
                    else if (spec_len >= 3 && strcmp(spec + spec_len - 3, ".h+") == 0)
                        do_inline = true;
                    else if (spec_len >= 0 && strcmp(spec + spec_len, "") == 0)
                        do_inline = true;
                    else
                    {
                        const char *slash = strrchr(spec, '/');
                        const char *backslash = strrchr(spec, '\\');
                        const char *dot = strrchr(spec, '.');

                        if (slash && backslash && backslash > slash) slash = backslash;
                        else if (!slash) slash = backslash;

                        /* If there is no dot, or the dot is part of a directory name, it has no extension */
                        if (!dot || (slash && dot < slash))
                            do_inline = true;
                    }

                    if (do_inline)
                    {
                        char *resolved =
                            resolve_include_path(path, spec, angle);

                        if (resolved)
                        {
                            bool resolved_inline = true;
                            size_t res_len = strlen(resolved);

                            /* Prevent inlining if it resolved to a .h fallback */
                            if (res_len >= 2 && strcmp(resolved + res_len - 2, ".h") == 0)
                                resolved_inline = false;

                            if (resolved_inline)
                            {
                                /*
                                 * Check whether this file has already
                                 * been completely preprocessed.
                                 */
                                const char *cached =
                                    include_cache_get(resolved);

                                if (cached)
                                {
                                    /* 
                                     * Already seen/included! 
                                     * Do not re-inline and remove the #include line 
                                     * by not writing anything to `out`.
                                     */
                                }
                                else
                                {
                                    char *included = read_file(resolved);

                                    if (included)
                                    {
                                        char *child =
                                            preprocess_source(
                                                resolved,
                                                included,
                                                depth + 1
                                            );

                                        /*
                                         * Store the preprocessed source.
                                         * The cache takes ownership of `child`.
                                         */
                                        include_cache_put(
                                            resolved,
                                            child
                                        );

                                        buffer_puts(&out, child);
                                        buffer_putc(&out, '\n');

                                        free(included);
                                    }
                                }
                            }
                            else
                            {
                                /* Keep unresolved includes unchanged. */
                                buffer_puts(&out, line);
                            }

                            free(resolved);
                        }
                        else
                        {
                            /* Keep unresolved includes unchanged. */
                            buffer_puts(&out, line);
                        }
                    }
                    else
                    {
                        /* Do not resolve or inline files that have a .h or other unhandled extension. */
                        buffer_puts(&out, line);
                    }

                    free(spec);
                }
                else
                {
                    buffer_puts(&out, line);
                }
            }
            else
            {
                rewrite_line(&out, line);
            }

            free(line);
        }

        if (line_end < end)
        {
            buffer_putc(&out, '\n');
            line_start = line_end + 1;
        }
        else
        {
            break;
        }
    }

    return out.data;
}

void locals_init(LocalTable *v)
{
    memset(v, 0, sizeof(*v));
}

void locals_free(LocalTable *v)
{
    size_t i;
    for (i = 0; i < v->count; ++i) {
        free(v->items[i].name);
        free(v->items[i].type_qualified);
    }
    free(v->items);
    memset(v, 0, sizeof(*v));
}

static size_t locals_mark(const LocalTable *v)
{
    return v->count;
}

static void locals_restore(LocalTable *v, size_t mark)
{
    while (v->count > mark) {
        Local *x = &v->items[--v->count];
        free(x->name);
        free(x->type_qualified);
    }
}

static void local_add(LocalTable *v, const char *name, const char *type, int ptr, int depth)
{
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 32;
        v->items = xrealloc(v->items, v->cap * sizeof(*v->items));
    }
    v->items[v->count].name = xstrdup(name);
    v->items[v->count].type_qualified = xstrdup(type);
    v->items[v->count].pointer_depth = ptr;
    v->items[v->count].depth = depth;
    ++v->count;
}

static Local *local_lookup(LocalTable *v, const char *name)
{
    size_t i;
    for (i = v->count; i > 0; --i) {
        if (strcmp(v->items[i - 1].name, name) == 0) {
            return &v->items[i - 1];
        }
    }
    return NULL;
}

static void locals_leave(LocalTable *v, int depth)
{
    while (v->count && v->items[v->count - 1].depth >= depth) {
        Local *x = &v->items[--v->count];
        free(x->name);
        free(x->type_qualified);
    }
}

static void die_at(const Token *t, const char *msg)
{
    fprintf(stderr, "c+: error:%zu:%zu: %s\n", t->line, t->column, msg);
    exit(1);
}

static void warn_at(const Token *t, const char *msg)
{
    if (ALLOW_WARNINGS)
        fprintf(stderr, "c+: warning:%zu:%zu: %s\n", t->line, t->column, msg);
}

static Token *at(Transpiler *t, size_t p)
{
    return p < t->tokens.count ? &t->tokens.items[p] : &t->tokens.items[t->tokens.count - 1];
}

static void emit_ws(Buffer *o, const Token *t)
{
    if (t->ws_length) {
        buffer_reserve(o, t->ws_length);
        memcpy(o->data + o->len, t->ws_begin, t->ws_length);
        o->len += t->ws_length;
        o->data[o->len] = 0;
    }
}

static void emit_raw(Buffer *o, const Token *t)
{
    buffer_reserve(o, t->length);
    memcpy(o->data + o->len, t->begin, t->length);
    o->len += t->length;
    o->data[o->len] = 0;
}

static void emit_full(Buffer *o, const Token *t)
{
    emit_ws(o, t);
    emit_raw(o, t);
}

static void buffer_insert(Buffer *b, size_t at_pos, const char *s)
{
    size_t n = strlen(s);
    if (at_pos > b->len) at_pos = b->len;
    buffer_reserve(b, n);
    memmove(b->data + at_pos + n, b->data + at_pos, b->len - at_pos + 1);
    memcpy(b->data + at_pos, s, n);
    b->len += n;
}

static bool buffer_has_non_whitespace(const Buffer *b)
{
    if (!b || b->len == 0 || !b->data) return false;
    for (size_t i = 0; i < b->len; ++i) {
        if (!isspace((unsigned char)b->data[i])) {
            return true;
        }
    }
    return false;
}

static size_t matching(Transpiler *t, size_t open, TokenKind left, TokenKind right, size_t end)
{
    int d = 0;
    size_t p;
    for (p = open; p < end; ++p) {
        Token *x = at(t, p);
        if (x->kind == left) {
            ++d;
        } else if (x->kind == right && --d == 0) {
            return p;
        }
    }
    die_at(at(t, open), "unclosed delimiter");
    return end;
}

static char *read_qualified(Transpiler *t, size_t p, size_t end, size_t *used)
{
    Buffer b;
    size_t n = 0;
    Token *x = at(t, p);
    if (p >= end || x->kind != TOK_IDENTIFIER) return NULL;
    buffer_init(&b);
    buffer_reserve(&b, x->length);
    memcpy(b.data, x->begin, x->length);
    b.len = x->length;
    b.data[b.len] = 0;
    ++n;
    while (p + n + 1 < end && at(t, p + n)->kind == TOK_SCOPE && at(t, p + n + 1)->kind == TOK_IDENTIFIER) {
        Token *next = at(t, p + n + 1);
        buffer_puts(&b, "::");
        buffer_reserve(&b, next->length);
        memcpy(b.data + b.len, next->begin, next->length);
        b.len += next->length;
        b.data[b.len] = 0;
        n += 2;
    }
    *used = n;
    return b.data;
}

static bool is_qualifier(const Token *x)
{
    return token_is(x, "static") || token_is(x, "extern") || token_is(x, "const") || token_is(x, "volatile") || token_is(x, "restrict") || token_is(x, "inline");
}

static Symbol *parse_resolved_type(Transpiler *t, const NamespaceStack *ns, size_t p, size_t end, size_t *after, int *pointer_depth)
{
    size_t used = 0;
    char *q;
    Symbol *type;

    while (p < end && is_qualifier(at(t, p))) ++p;
    if (p < end && (token_is(at(t, p), "struct") || token_is(at(t, p), "enum"))) ++p;
    
    q = read_qualified(t, p, end, &used);
    if (!q) return NULL;
    
    type = resolve_name(&t->symbols, ns, q, SYM_TYPE | SYM_ENUM);
    free(q);
    if (!type) return NULL;
    
    p += used;
    
    if (pointer_depth) *pointer_depth = 0;
    
    while (p < end && (token_char(at(t, p), '*') || is_qualifier(at(t, p)))) {
        if (token_char(at(t, p), '*')) {
            if (pointer_depth) ++*pointer_depth;
        }
        ++p;
    }
    
    if (after) *after = p;
    return type;
}

typedef struct {
    bool ok, is_static;
    size_t name, lparen, rparen, body, close, after;
} Callable;

typedef struct {
    bool ok;
    size_t name, after_type;
    Symbol *type;
    int pointer_depth;
} VarDecl;

static bool parse_callable(Transpiler *t, size_t p, size_t end, Callable *c)
{
    size_t i, lp = end, rp, a;
    bool bad = false;
    memset(c, 0, sizeof(*c));
    for (i = p; i < end; ++i) {
        Token *x = at(t, i);
        if (x->kind == TOK_SEMICOLON || x->kind == TOK_LBRACE) return false;
        if (token_char(x, '=') || x->kind == TOK_COMMA) bad = true;
        if (x->kind == TOK_LPAREN) {
            lp = i;
            break;
        }
    }
    if (bad || lp == end || lp == p || at(t, lp - 1)->kind != TOK_IDENTIFIER) return false;
    rp = matching(t, lp, TOK_LPAREN, TOK_RPAREN, end);
    a = rp + 1;
    if (a >= end || (at(t, a)->kind != TOK_LBRACE && at(t, a)->kind != TOK_SEMICOLON)) return false;
    c->name = lp - 1;
    c->lparen = lp;
    c->rparen = rp;
    c->after = a;
    for (i = p; i < c->name; ++i) {
        if (token_is(at(t, i), "static")) c->is_static = true;
    }
    if (at(t, a)->kind == TOK_LBRACE) {
        c->body = a;
        c->close = matching(t, a, TOK_LBRACE, TOK_RBRACE, end);
        c->after = c->close + 1;
    } else {
        c->body = c->close = end;
        c->after = a + 1;
    }
    c->ok = true;
    return true;
}

static bool parse_var_decl(Transpiler *t, const NamespaceStack *ns, size_t p, size_t end, VarDecl *d)
{
    size_t i = p;
    Symbol *type;
    memset(d, 0, sizeof(*d));
    type = parse_resolved_type(t, ns, i, end, &i, &d->pointer_depth);
    if (!type) return false;
    d->after_type = i - (size_t)d->pointer_depth;
    if (i >= end || at(t, i)->kind != TOK_IDENTIFIER || is_c_keyword(at(t, i))) return false;
    if (i + 1 < end && at(t, i + 1)->kind == TOK_LPAREN) return false;
    d->name = i;
    d->type = type;
    d->ok = true;
    return true;
}

static bool parse_typedef_decl(Transpiler *t, const NamespaceStack *ns, size_t p, size_t end, size_t *alias)
{
    size_t i = p + 1;
    if (!token_is(at(t, p), "typedef")) return false;
    while (i < end && at(t, i)->kind != TOK_SEMICOLON) ++i;
    if (i >= end) return false;
    while (i > p + 1) {
        --i;
        if (at(t, i)->kind == TOK_IDENTIFIER && !is_c_keyword(at(t, i))) {
            *alias = i;
            return true;
        }
    }
    return false;
}

static void discover_range(Transpiler *, NamespaceStack *, size_t, size_t);

static void discover_enum(Transpiler *t, NamespaceStack *ns, size_t start, size_t open, size_t close, Symbol *owner)
{
    size_t p;
    bool want = true;
    (void)start;
    for (p = open + 1; p < close; ++p) {
        Token *x = at(t, p);
        if (want && x->kind == TOK_IDENTIFIER) {
            char *n = token_text(x), *q;
            Buffer b;
            buffer_init(&b);
            buffer_puts(&b, owner->qualified_name);
            buffer_puts(&b, "::");
            buffer_puts(&b, n);
            q = b.data;
            symbols_add(&t->symbols, q, SYM_ENUM_MEMBER, owner->qualified_name, NULL, 0, false);
            free(n);
            free(q);
            want = false;
        }
        if (x->kind == TOK_COMMA) want = true;
    }
    (void)ns;
}

static void discover_struct(Transpiler *t, NamespaceStack *ns, size_t start, size_t open, size_t close, Symbol *owner)
{
    size_t p = open + 1;
    (void)start;
    while (p < close) {
        Callable c;
        if (parse_callable(t, p, close, &c)) {
            char *name = token_text(at(t, c.name));
            Buffer b;
            char *q;
            size_t ignored;
            int return_depth = 0;
            Symbol *return_type = parse_resolved_type(t, ns, p, c.name, &ignored, &return_depth);
            buffer_init(&b);
            buffer_puts(&b, owner->qualified_name);
            buffer_puts(&b, "::");
            buffer_puts(&b, name);
            q = b.data;
            symbols_add(&t->symbols, q, SYM_METHOD, owner->qualified_name, return_type ? return_type->qualified_name : NULL, return_depth, c.is_static);
            free(name);
            free(q);
            p = c.after;
            continue;
        }
        {
            size_t semi = p, name = close;
            while (semi < close && at(t, semi)->kind != TOK_SEMICOLON) ++semi;
            if (semi < close) {
                size_t i = semi;
                while (i > p) {
                    --i;
                    if (at(t, i)->kind == TOK_IDENTIFIER && !is_c_keyword(at(t, i))) {
                        name = i;
                        break;
                    }
                }
                if (name < close) {
                    char *field = token_text(at(t, name));
                    Buffer b;
                    char *q;
                    int ptr = 0;
                    size_t after_type;
                    
                    Symbol *type = parse_resolved_type(t, ns, p, name, &after_type, &ptr);
                    
                    buffer_init(&b);
                    buffer_puts(&b, owner->qualified_name);
                    buffer_puts(&b, "::");
                    buffer_puts(&b, field);
                    q = b.data;
                    
                    symbols_add(&t->symbols, q, SYM_FIELD, owner->qualified_name, type ? type->qualified_name : NULL, ptr, false);
                    free(field);
                    free(q);
                }
                p = semi + 1;
                continue;
            }
        }
        ++p;
    }
    (void)ns;
}

static void discover_range(Transpiler *t, NamespaceStack *ns, size_t begin, size_t end)
{
    size_t p = begin;
    while (p < end) {
        Token *x = at(t, p);
        Callable c;
        VarDecl d;

        if (token_is(x, "namespace") && p + 2 < end && at(t, p + 1)->kind == TOK_IDENTIFIER && at(t, p + 2)->kind == TOK_LBRACE) {
            size_t close = matching(t, p + 2, TOK_LBRACE, TOK_RBRACE, end);
            ns_push(ns, at(t, p + 1));
            discover_range(t, ns, p + 3, close);
            ns_pop(ns);
            p = close + 1;
            continue;
        }
        if ((token_is(x, "struct") || token_is(x, "enum")) && p + 2 < end && at(t, p + 1)->kind == TOK_IDENTIFIER && at(t, p + 2)->kind == TOK_LBRACE) {
            size_t close = matching(t, p + 2, TOK_LBRACE, TOK_RBRACE, end);
            char *name = token_text(at(t, p + 1)), *q = qualify(ns, ns->count, name);
            Symbol *s = symbols_add(&t->symbols, q, token_is(x, "enum") ? SYM_ENUM : SYM_TYPE, NULL, NULL, 0, false);
            if (token_is(x, "enum")) {
                discover_enum(t, ns, p, p + 2, close, s);
            } else {
                discover_struct(t, ns, p, p + 2, close, s);
            }
            free(name);
            free(q);
            p = close + 1;
            if (p < end && at(t, p)->kind == TOK_SEMICOLON) ++p;
            continue;
        }
        if (token_is(x, "struct") && p + 1 < end && at(t, p + 1)->kind == TOK_IDENTIFIER && p + 2 < end && at(t, p + 2)->kind == TOK_SEMICOLON) {
            char *name = token_text(at(t, p + 1)), *q = qualify(ns, ns->count, name);
            symbols_add(&t->symbols, q, SYM_TYPE, NULL, NULL, 0, false);
            free(name);
            free(q);
            p += 3;
            continue;
        }
        if (token_is(x, "enum") && p + 1 < end && at(t, p + 1)->kind == TOK_IDENTIFIER && p + 2 < end && at(t, p + 2)->kind == TOK_SEMICOLON) {
            char *name = token_text(at(t, p + 1)), *q = qualify(ns, ns->count, name);
            symbols_add(&t->symbols, q, SYM_ENUM, NULL, NULL, 0, false);
            free(name);
            free(q);
            p += 3;
            continue;
        }
        if (parse_callable(t, p, end, &c)) {
            size_t q_start = c.name;
            while (q_start >= 2 && at(t, q_start - 1)->kind == TOK_SCOPE && at(t, q_start - 2)->kind == TOK_IDENTIFIER) {
                q_start -= 2;
            }
            {
                size_t used = 0;
                char *q_name = read_qualified(t, q_start, c.name + 1, &used);
                char *q = qualify(ns, ns->count, q_name);
                size_t ignored;
                int return_depth = 0;
                Symbol *return_type = parse_resolved_type(t, ns, p, q_start, &ignored, &return_depth);
                
                // Determine if it is a method or function based on qualification depth / type owner
                Symbol *existing = symbol_exact(&t->symbols, q, SYM_FUNCTION | SYM_METHOD);
                if (!existing) {
                    symbols_add(&t->symbols, q, q_start > 0 ? SYM_METHOD : SYM_FUNCTION, NULL, return_type ? return_type->qualified_name : NULL, return_depth, c.is_static);
                }
                free(q_name);
                free(q);
            }
            p = c.after;
            continue;
        }
        if (parse_typedef_decl(t, ns, p, end, &d.name)) {
            char *name = token_text(at(t, d.name));
            char *q = qualify(ns, ns->count, name);
            symbols_add(&t->symbols, q, SYM_TYPE, NULL, NULL, 0, false);
            free(name);
            free(q);
            while (p < end && at(t, p)->kind != TOK_SEMICOLON) ++p;
            if (p < end) ++p;
            continue;
        }
        if (parse_var_decl(t, ns, p, end, &d)) {
            char *name = token_text(at(t, d.name)), *q = qualify(ns, ns->count, name);
            symbols_add(&t->symbols, q, SYM_VARIABLE, NULL, d.type->qualified_name, d.pointer_depth, false);
            free(name);
            free(q);
            while (p < end && at(t, p)->kind != TOK_SEMICOLON) ++p;
            if (p < end) ++p;
            continue;
        }
        ++p;
    }
}

static Symbol *lookup_value(Transpiler *t, NamespaceStack *ns, const char *name, int *ptr)
{
    Local *l = local_lookup(&t->locals, name);
    Symbol *s;
    if (l) {
        if (ptr) *ptr = l->pointer_depth;
        return symbol_exact(&t->symbols, l->type_qualified, SYM_TYPE | SYM_ENUM);
    }
    s = resolve_name(&t->symbols, ns, name, SYM_VARIABLE);
    if (s && ptr) *ptr = s->pointer_depth;
    if (s) return symbol_exact(&t->symbols, s->type_qualified, SYM_TYPE | SYM_ENUM);
    return NULL;
}

static Symbol *method_for(Transpiler *t, const char *typeq, const char *name)
{
    Buffer b;
    Symbol *s;
    buffer_init(&b);
    buffer_puts(&b, typeq);
    buffer_puts(&b, "::");
    buffer_puts(&b, name);
    s = symbol_exact(&t->symbols, b.data, SYM_METHOD);
    buffer_free(&b);
    return s;
}

static Symbol *field_for(Transpiler *t, const char *typeq, const char *name)
{
    Buffer b;
    Symbol *s;
    buffer_init(&b);
    buffer_puts(&b, typeq);
    buffer_puts(&b, "::");
    buffer_puts(&b, name);
    s = symbol_exact(&t->symbols, b.data, SYM_FIELD);
    buffer_free(&b);
    return s;
}

static void emit_one(Transpiler *t, NamespaceStack *ns, size_t *pp, size_t end);
static void emit_fragment(Transpiler *t, NamespaceStack *ns, size_t begin, size_t end)
{
    size_t p = begin;
    while (p < end) emit_one(t, ns, &p, end);
}

static void emit_fragment_to_buffer(Buffer *out, Transpiler *t, NamespaceStack *ns, size_t begin, size_t end)
{
    Buffer saved = t->output;
    Buffer tmp;
    buffer_init(&tmp);
    t->output = tmp;
    emit_fragment(t, ns, begin, end);
    tmp = t->output;
    t->output = saved;
    if (tmp.len) {
        buffer_reserve(out, tmp.len);
        memcpy(out->data + out->len, tmp.data, tmp.len);
        out->len += tmp.len;
        out->data[out->len] = 0;
    }
    buffer_free(&tmp);
}

static void defer_stack_push(DeferStack *d)
{
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->items = xrealloc(d->items, d->cap * sizeof(*d->items));
    }
    memset(&d->items[d->count], 0, sizeof(d->items[d->count]));
    ++d->count;
}

static DeferScope *defer_stack_current(DeferStack *d)
{
    return d->count ? &d->items[d->count - 1] : NULL;
}

static void defer_stack_pop(DeferStack *d)
{
    if (!d->count) return;
    buffer_free(&d->items[d->count - 1].output);
    --d->count;
}

static bool try_emit_struct_literal(Transpiler *t, NamespaceStack *ns, size_t *pp, size_t end)
{
    size_t p = *pp, used = 0, open, close, item;
    char *q;
    Symbol *type;

    if (at(t, p)->kind != TOK_IDENTIFIER) return false;
    q = read_qualified(t, p, end, &used);
    if (!q) return false;
    type = resolve_name(&t->symbols, ns, q, SYM_TYPE);
    free(q);
    open = p + used;
    if (!type || open >= end || at(t, open)->kind != TOK_LBRACE) return false;
    close = matching(t, open, TOK_LBRACE, TOK_RBRACE, end);
    emit_ws(&t->output, at(t, p));
    buffer_putc(&t->output, '(');
    buffer_puts(&t->output, type->mangled_name);
    buffer_puts(&t->output, "){");
    item = open + 1;
    while (item < close) {
        size_t colon, value_end, scan;
        char *field;
        if (at(t, item)->kind == TOK_COMMA) {
            emit_full(&t->output, at(t, item));
            ++item;
            continue;
        }
        if (at(t, item)->kind != TOK_IDENTIFIER) die_at(at(t, item), "struct literal expects a field name");
        colon = item + 1;
        if (colon >= close || at(t, colon)->kind != TOK_COLON) die_at(at(t, item), "struct literal fields use 'field: value'");
        field = token_text(at(t, item));
        if (!field_for(t, type->qualified_name, field)) {
            free(field);
            die_at(at(t, item), "unknown field in struct literal");
        }
        buffer_putc(&t->output, '.');
        buffer_puts(&t->output, field);
        buffer_puts(&t->output, " =");
        free(field);
        value_end = close;
        scan = colon + 1;
        {
            int paren = 0, bracket = 0, brace = 0;
            for (; scan < close; ++scan) {
                Token *z = at(t, scan);
                if (z->kind == TOK_LPAREN) ++paren;
                else if (z->kind == TOK_RPAREN) --paren;
                else if (z->kind == TOK_LBRACKET) ++bracket;
                else if (z->kind == TOK_RBRACKET) --bracket;
                else if (z->kind == TOK_LBRACE) ++brace;
                else if (z->kind == TOK_RBRACE) --brace;
                else if (z->kind == TOK_COMMA && !paren && !bracket && !brace) {
                    value_end = scan;
                    break;
                }
            }
        }
        emit_fragment(t, ns, colon + 1, value_end);
        if (value_end < close) {
            buffer_putc(&t->output, ',');
            item = value_end + 1;
        } else {
            item = close;
        }
    }
    emit_ws(&t->output, at(t, close));
    buffer_putc(&t->output, '}');
    *pp = close + 1;
    return true;
}

static void emit_fragment_without_first_ws(Transpiler *t, NamespaceStack *ns, size_t begin, size_t end)
{
    Token *first = at(t, begin);
    size_t ws = first->ws_length;
    first->ws_length = 0;
    emit_fragment(t, ns, begin, end);
    first->ws_length = ws;
}

static void emit_value_receiver_call(Transpiler *t, NamespaceStack *ns, size_t begin, size_t receiver_end, Symbol *type, Symbol *method, size_t method_lparen, size_t method_rparen, size_t *pp)
{
    char temp[64];
    bool pass_receiver = !method->is_static || method->receiver_pointer_depth > 0;
    snprintf(temp, sizeof(temp), "__cplus_receiver_%zu", t->next_temporary++);
    emit_ws(&t->output, at(t, begin));
    if (pass_receiver) {
        buffer_puts(&t->output, "({ ");
        buffer_puts(&t->output, type->mangled_name);
        buffer_putc(&t->output, ' ');
        buffer_puts(&t->output, temp);
        buffer_puts(&t->output, " = ");
        emit_fragment_without_first_ws(t, ns, begin, receiver_end);
        buffer_puts(&t->output, "; ");
        buffer_puts(&t->output, method->mangled_name);
        buffer_puts(&t->output, "(&");
        buffer_puts(&t->output, temp);
        if (method_lparen + 1 < method_rparen) buffer_putc(&t->output, ',');
        emit_fragment(t, ns, method_lparen + 1, method_rparen);
        buffer_puts(&t->output, "); })");
    } else {
        buffer_puts(&t->output, "((void)(");
        emit_fragment_without_first_ws(t, ns, begin, receiver_end);
        buffer_puts(&t->output, "), ");
        buffer_puts(&t->output, method->mangled_name);
        buffer_putc(&t->output, '(');
        emit_fragment(t, ns, method_lparen + 1, method_rparen);
        buffer_puts(&t->output, "))");
    }
    *pp = method_rparen + 1;
}

static bool try_emit_parenthesized_method_call(Transpiler *t, NamespaceStack *ns, size_t *pp, size_t end)
{
    size_t p = *pp, close, method_name, method_lparen, method_rparen;
    char *name, *mn;
    Symbol *type, *method;
    int ptr = 0;
    bool arrow = false;

    if (at(t, p)->kind != TOK_LPAREN) return false;
    close = matching(t, p, TOK_LPAREN, TOK_RPAREN, end);
    if (close != p + 2 || close + 3 >= end) return false;
    
    if (at(t, close + 1)->kind == TOK_DOT) {
        method_name = close + 2;
        method_lparen = close + 3;
    } else if (token_char(at(t, close + 1), '-') && at(t, close + 2)->kind == TOK_GT) {
        arrow = true;
        method_name = close + 3;
        method_lparen = close + 4;
        if (method_lparen >= end) return false;
    } else {
        return false;
    }

    if (at(t, method_name)->kind != TOK_IDENTIFIER || at(t, method_lparen)->kind != TOK_LPAREN) return false;

    name = token_text(at(t, p + 1));
    type = lookup_value(t, ns, name, &ptr);
    if (!type) {
        free(name);
        return false;
    }
    
    mn = token_text(at(t, method_name));
    method = method_for(t, type->qualified_name, mn);
    free(mn);
    if (!method) {
        free(name);
        return false;
    }
    
    if (arrow && ptr < 1) {
        free(name);
        die_at(at(t, p), "'->' method call requires a pointer receiver");
    }

    method_rparen = matching(t, method_lparen, TOK_LPAREN, TOK_RPAREN, end);
    emit_ws(&t->output, at(t, p));
    buffer_puts(&t->output, method->mangled_name);
    buffer_putc(&t->output, '(');
    
    if (!method->is_static || method->receiver_pointer_depth > 0) {
        if (!arrow) buffer_putc(&t->output, '&');
        buffer_puts(&t->output, name);
        if (method_lparen + 1 < method_rparen) buffer_putc(&t->output, ',');
    }
    
    emit_fragment(t, ns, method_lparen + 1, method_rparen);
    buffer_putc(&t->output, ')');
    free(name);
    *pp = method_rparen + 1;
    return true;
}

static bool try_emit_expression_method_call(Transpiler *t, NamespaceStack *ns, size_t *pp, size_t end)
{
    size_t p = *pp, used = 0, receiver_end, op, method_name, method_lparen, method_rparen;
    char *q, *mn;
    Symbol *callee, *type, *method;
    bool arrow = false;

    if (at(t, p)->kind != TOK_IDENTIFIER) return false;
    q = read_qualified(t, p, end, &used);
    if (!q || p + used >= end || at(t, p + used)->kind != TOK_LPAREN) {
        free(q);
        return false;
    }
    
    callee = resolve_name(&t->symbols, ns, q, SYM_FUNCTION | SYM_METHOD);
    free(q);
    if (!callee || !callee->type_qualified) return false;
    
    receiver_end = matching(t, p + used, TOK_LPAREN, TOK_RPAREN, end) + 1;
    if (receiver_end >= end) return false;
    
    if (at(t, receiver_end)->kind == TOK_DOT) {
        op = receiver_end;
        method_name = receiver_end + 1;
        method_lparen = receiver_end + 2;
    } else if (token_char(at(t, receiver_end), '-') && receiver_end + 3 < end && at(t, receiver_end + 1)->kind == TOK_GT) {
        arrow = true;
        op = receiver_end;
        method_name = receiver_end + 2;
        method_lparen = receiver_end + 3;
    } else {
        return false;
    }
    
    (void)op;
    if (method_lparen >= end || at(t, method_name)->kind != TOK_IDENTIFIER || at(t, method_lparen)->kind != TOK_LPAREN) return false;
    
    type = symbol_exact(&t->symbols, callee->type_qualified, SYM_TYPE);
    if (!type) return false;
    
    mn = token_text(at(t, method_name));
    method = method_for(t, type->qualified_name, mn);
    free(mn);
    if (!method) return false;
    
    method_rparen = matching(t, method_lparen, TOK_LPAREN, TOK_RPAREN, end);
    if (arrow && callee->pointer_depth < 1) die_at(at(t, p), "'->' method call requires a pointer expression");
    
    if (callee->pointer_depth > 0 && (!method->is_static || method->receiver_pointer_depth > 0)) {
        emit_ws(&t->output, at(t, p));
        buffer_puts(&t->output, method->mangled_name);
        buffer_putc(&t->output, '(');
        
        if (!arrow) buffer_putc(&t->output, '&');
        
        emit_fragment_without_first_ws(t, ns, p, receiver_end);
        if (method_lparen + 1 < method_rparen) buffer_putc(&t->output, ',');
        emit_fragment(t, ns, method_lparen + 1, method_rparen);
        buffer_putc(&t->output, ')');
        *pp = method_rparen + 1;
        return true;
    }
    
    emit_value_receiver_call(t, ns, p, receiver_end, type, method, method_lparen, method_rparen, pp);
    return true;
}

static bool try_emit_struct_literal_method_call(Transpiler *t, NamespaceStack *ns, size_t *pp, size_t end)
{
    size_t p = *pp, used = 0, open, close, method_name, method_lparen, method_rparen;
    char *q, *mn;
    Symbol *type, *method;

    if (at(t, p)->kind != TOK_IDENTIFIER) return false;
    q = read_qualified(t, p, end, &used);
    if (!q) return false;
    type = resolve_name(&t->symbols, ns, q, SYM_TYPE);
    free(q);
    open = p + used;
    if (!type || open >= end || at(t, open)->kind != TOK_LBRACE) return false;
    close = matching(t, open, TOK_LBRACE, TOK_RBRACE, end);
    if (close + 3 >= end || at(t, close + 1)->kind != TOK_DOT || at(t, close + 2)->kind != TOK_IDENTIFIER || at(t, close + 3)->kind != TOK_LPAREN) return false;
    method_name = close + 2;
    method_lparen = close + 3;
    mn = token_text(at(t, method_name));
    method = method_for(t, type->qualified_name, mn);
    free(mn);
    if (!method) return false;
    method_rparen = matching(t, method_lparen, TOK_LPAREN, TOK_RPAREN, end);
    emit_value_receiver_call(t, ns, p, close + 1, type, method, method_lparen, method_rparen, pp);
    return true;
}

static bool builtin_type_token(const Token *x)
{
    return token_is(x, "void") || token_is(x, "char") || token_is(x, "short") || token_is(x, "int") || token_is(x, "long") || token_is(x, "float") || token_is(x, "double") || token_is(x, "bool") || token_is(x, "signed") || token_is(x, "unsigned");
}

static bool looks_like_c_style_cast(Transpiler *t, NamespaceStack *ns, size_t p, size_t end)
{
    size_t q, after;
    int depth;
    Symbol *type;
    if (at(t, p)->kind != TOK_LPAREN || p + 2 >= end || (p && token_is(at(t, p - 1), "sizeof"))) return false;
    q = p + 1;
    type = parse_resolved_type(t, ns, q, end, &after, &depth);
    if (type) return after < end && at(t, after)->kind == TOK_RPAREN;
    if (!builtin_type_token(at(t, q))) return false;
    while (q < end && (builtin_type_token(at(t, q)) || token_is(at(t, q), "const") || token_is(at(t, q), "volatile") || token_char(at(t, q), '*'))) ++q;
    return q < end && at(t, q)->kind == TOK_RPAREN;
}

static bool try_emit_defer_block(Transpiler *t, NamespaceStack *ns, size_t *pp, size_t end)
{
    size_t p = *pp;
    size_t open, close;
    DeferScope *scope;
    Buffer emitted;

    if (!token_is(at(t, p), "defer") || p + 1 >= end || at(t, p + 1)->kind != TOK_LBRACE) return false;
    
    open = p + 1;
    close = matching(t, open, TOK_LBRACE, TOK_RBRACE, end);
    
    scope = defer_stack_current(&t->defers);
    if (!scope) return false;

    buffer_init(&emitted);
    emit_fragment_to_buffer(&emitted, t, ns, open + 1, close);

    buffer_puts(&scope->output, emitted.data);
    
    buffer_free(&emitted);
    
    *pp = close + 1;
    return true;
}

static void emit_fragment_with_substitution(Transpiler *t, NamespaceStack *ns, size_t begin, size_t end)
{
    size_t p = begin;
    while (p < end) {
        Token *x = at(t, p);
        if (x->kind == TOK_IDENTIFIER && token_is(x, "_")) {
            emit_ws(&t->output, x);
            buffer_puts(&t->output, "__switch_value");
            ++p;
        } else {
            emit_one(t, ns, &p, end);
        }
    }
}

static bool is_tok_str(const Token *t, const char *s)
{
    size_t len = strlen(s);
    return t->length == len && memcmp(t->begin, s, len) == 0;
}

static bool is_or_op(Transpiler *t, size_t pos, size_t end, size_t *consumed)
{
    Token *x;
    if (pos >= end) return false;
    x = at(t, pos);
    if (is_tok_str(x, "||")) {
        if (consumed) *consumed = 1;
        return true;
    }
    if (pos + 1 < end && token_char(x, '|') && token_char(at(t, pos + 1), '|') && at(t, pos + 1)->ws_length == 0) {
        if (consumed) *consumed = 2;
        return true;
    }
    return false;
}

static bool is_and_op(Transpiler *t, size_t pos, size_t end, size_t *consumed)
{
    Token *x;
    if (pos >= end) return false;
    x = at(t, pos);
    if (is_tok_str(x, "&&")) {
        if (consumed) *consumed = 1;
        return true;
    }
    if (pos + 1 < end && token_char(x, '&') && token_char(at(t, pos + 1), '&') && at(t, pos + 1)->ws_length == 0) {
        if (consumed) *consumed = 2;
        return true;
    }
    return false;
}

static void emit_case_expr_primary(Transpiler *t, NamespaceStack *ns, size_t begin, size_t end)
{
    size_t range_pos = end;
    bool range_found = false;
    TokenKind range_kind = TOK_OTHER;
    int d = 0;
    size_t k;

    while (begin + 1 < end && at(t, begin)->kind == TOK_LPAREN) {
        size_t match = matching(t, begin, TOK_LPAREN, TOK_RPAREN, end);
        if (match == end - 1) {
            begin++;
            end--;
        } else {
            break;
        }
    }
    if (begin >= end) return;

    d = 0;
    for (k = begin; k < end; ++k) {
        Token *z = at(t, k);
        if (z->kind == TOK_LPAREN || z->kind == TOK_LBRACKET || z->kind == TOK_LBRACE) {
            d++;
        } else if (z->kind == TOK_RPAREN || z->kind == TOK_RBRACKET || z->kind == TOK_RBRACE) {
            if (d > 0) d--;
        } else if (d == 0 && (z->kind == TOK_RANGE || z->kind == TOK_RANGE_INCLUSIVE)) {
            range_pos = k;
            range_found = true;
            range_kind = z->kind;
            break;
        }
    }

    if (range_found) {
        buffer_puts(&t->output, "(__switch_value >= ");
        emit_fragment_with_substitution(t, ns, begin, range_pos);
        buffer_puts(&t->output, " && __switch_value ");
        if (range_kind == TOK_RANGE_INCLUSIVE) {
            buffer_puts(&t->output, "<= ");
        } else {
            buffer_puts(&t->output, "< ");
        }
        emit_fragment_with_substitution(t, ns, range_pos + 1, end);
        buffer_puts(&t->output, ")");
    } else {
        bool predicate = false;
        for (k = begin; k < end; ++k) {
            Token *z = at(t, k);
            if (z->kind == TOK_IDENTIFIER && token_is(z, "_")) {
                predicate = true;
                break;
            }
        }
        if (predicate) {
            emit_fragment_with_substitution(t, ns, begin, end);
        } else {
            buffer_puts(&t->output, "(__switch_value == ");
            emit_fragment_with_substitution(t, ns, begin, end);
            buffer_puts(&t->output, ")");
        }
    }
}

static void emit_case_expr_and(Transpiler *t, NamespaceStack *ns, size_t begin, size_t end)
{
    size_t i = begin;
    int d = 0;
    bool has_and = false;
    size_t seg_start;
    bool first;

    while (i < end) {
        Token *z = at(t, i);
        if (z->kind == TOK_LPAREN || z->kind == TOK_LBRACKET || z->kind == TOK_LBRACE) {
            d++;
            i++;
        } else if (z->kind == TOK_RPAREN || z->kind == TOK_RBRACKET || z->kind == TOK_RBRACE) {
            if (d > 0) d--;
            i++;
        } else if (d == 0) {
            size_t consumed = 0;
            if (is_and_op(t, i, end, &consumed)) {
                has_and = true;
                break;
            }
            i++;
        } else {
            i++;
        }
    }

    if (!has_and) {
        emit_case_expr_primary(t, ns, begin, end);
        return;
    }

    i = begin;
    seg_start = begin;
    d = 0;
    first = true;

    while (i < end) {
        Token *z = at(t, i);
        if (z->kind == TOK_LPAREN || z->kind == TOK_LBRACKET || z->kind == TOK_LBRACE) {
            d++;
            i++;
        } else if (z->kind == TOK_RPAREN || z->kind == TOK_RBRACKET || z->kind == TOK_RBRACE) {
            if (d > 0) d--;
            i++;
        } else if (d == 0) {
            size_t consumed = 0;
            if (is_and_op(t, i, end, &consumed)) {
                if (!first) {
                    buffer_puts(&t->output, " && ");
                } else {
                    buffer_puts(&t->output, "(");
                    first = false;
                }
                emit_case_expr_primary(t, ns, seg_start, i);
                i += consumed;
                seg_start = i;
                continue;
            }
            i++;
        } else {
            i++;
        }
    }

    if (seg_start < end) {
        if (!first) {
            buffer_puts(&t->output, " && ");
        }
        emit_case_expr_primary(t, ns, seg_start, end);
    }
    if (!first) {
        buffer_puts(&t->output, ")");
    }
}

static void emit_case_expr_or(Transpiler *t, NamespaceStack *ns, size_t begin, size_t end)
{
    size_t i = begin;
    int d = 0;
    bool has_or = false;
    size_t seg_start;
    bool first;

    while (i < end) {
        Token *z = at(t, i);
        if (z->kind == TOK_LPAREN || z->kind == TOK_LBRACKET || z->kind == TOK_LBRACE) {
            d++;
            i++;
        } else if (z->kind == TOK_RPAREN || z->kind == TOK_RBRACKET || z->kind == TOK_RBRACE) {
            if (d > 0) d--;
            i++;
        } else if (d == 0) {
            size_t consumed = 0;
            if (is_or_op(t, i, end, &consumed)) {
                has_or = true;
                break;
            }
            i++;
        } else {
            i++;
        }
    }

    if (!has_or) {
        emit_case_expr_and(t, ns, begin, end);
        return;
    }

    i = begin;
    seg_start = begin;
    d = 0;
    first = true;

    while (i < end) {
        Token *z = at(t, i);
        if (z->kind == TOK_LPAREN || z->kind == TOK_LBRACKET || z->kind == TOK_LBRACE) {
            d++;
            i++;
        } else if (z->kind == TOK_RPAREN || z->kind == TOK_RBRACKET || z->kind == TOK_RBRACE) {
            if (d > 0) d--;
            i++;
        } else if (d == 0) {
            size_t consumed = 0;
            if (is_or_op(t, i, end, &consumed)) {
                if (!first) {
                    buffer_puts(&t->output, " || ");
                } else {
                    first = false;
                }
                emit_case_expr_and(t, ns, seg_start, i);
                i += consumed;
                seg_start = i;
                continue;
            }
            i++;
        } else {
            i++;
        }
    }

    if (seg_start < end) {
        if (!first) {
            buffer_puts(&t->output, " || ");
        }
        emit_case_expr_and(t, ns, seg_start, end);
    }
}

static bool try_emit_switch(Transpiler *t, NamespaceStack *ns, size_t *pp, size_t end)
{
    size_t p = *pp, lp, rp, body_open, body_close, i;
    bool has_break = false;
    bool saw_case = false;

    if (!token_is(at(t, p), "switch") || p + 1 >= end || at(t, p + 1)->kind != TOK_LPAREN) return false;
    lp = p + 1;
    rp = matching(t, lp, TOK_LPAREN, TOK_RPAREN, end);
    if (rp + 1 >= end || at(t, rp + 1)->kind != TOK_LBRACE) return false;
    body_open = rp + 1;
    body_close = matching(t, body_open, TOK_LBRACE, TOK_RBRACE, end);

    for (i = body_open + 1; i < body_close; ++i) {
        Token *x = at(t, i);
        if (x->kind == TOK_IDENTIFIER && token_is(x, "break")) {
            has_break = true;
            break;
        }
    }
    if (has_break) return false;

    emit_ws(&t->output, at(t, p));
    buffer_puts(&t->output, "({ int __switch_value = ");
    emit_fragment_with_substitution(t, ns, p + 2, rp);
    buffer_puts(&t->output, "; ");

    i = body_open + 1;
    while (i < body_close) {
        Token *x = at(t, i);
        if (x->kind == TOK_IDENTIFIER && token_is(x, "case")) {
            size_t label_begin = i + 1, label_end = i + 1, colon = body_close, j;
            int d = 0;
            if (label_begin >= body_close) break;

            while (label_end < body_close) {
                Token *z = at(t, label_end);
                if (z->kind == TOK_LPAREN || z->kind == TOK_LBRACKET || z->kind == TOK_LBRACE) {
                    d++;
                } else if (z->kind == TOK_RPAREN || z->kind == TOK_RBRACKET || z->kind == TOK_RBRACE) {
                    if (d > 0) d--;
                } else if (d == 0 && z->kind == TOK_COLON) {
                    colon = label_end;
                    break;
                }
                ++label_end;
            }
            if (colon >= body_close) break;

            if (!saw_case) {
                buffer_puts(&t->output, "if (");
            } else {
                buffer_puts(&t->output, "else if (");
            }

            emit_case_expr_or(t, ns, label_begin, colon);

            buffer_puts(&t->output, ") { ");
            saw_case = true;

            j = colon + 1;
            d = 0;
            while (j < body_close) {
                Token *z = at(t, j);
                if (z->kind == TOK_LPAREN || z->kind == TOK_LBRACKET || z->kind == TOK_LBRACE) {
                    d++;
                } else if (z->kind == TOK_RPAREN || z->kind == TOK_RBRACKET || z->kind == TOK_RBRACE) {
                    if (d > 0) d--;
                } else if (d == 0 && z->kind == TOK_IDENTIFIER && (token_is(z, "case") || token_is(z, "default"))) {
                    break;
                }
                ++j;
            }

            emit_fragment(t, ns, colon + 1, j);
            
            buffer_puts(&t->output, " } ");
            i = j;
            continue;
        }
        
        if (x->kind == TOK_IDENTIFIER && token_is(x, "default")) {
            size_t colon = i + 1, j;
            if (colon < body_close && at(t, colon)->kind == TOK_COLON) {
                int d = 0;
                
                buffer_puts(&t->output, "else { ");
                
                j = colon + 1;
                while (j < body_close) {
                    Token *z = at(t, j);
                    if (z->kind == TOK_LPAREN || z->kind == TOK_LBRACKET || z->kind == TOK_LBRACE) {
                        d++;
                    } else if (z->kind == TOK_RPAREN || z->kind == TOK_RBRACKET || z->kind == TOK_RBRACE) {
                        if (d > 0) d--;
                    } else if (d == 0 && z->kind == TOK_IDENTIFIER && (token_is(z, "case") || token_is(z, "default"))) {
                        break;
                    }
                    ++j;
                }

                emit_fragment(t, ns, colon + 1, j);

                buffer_puts(&t->output, " } ");
                i = j;
                continue;
            }
        }
        ++i;
    }
    buffer_puts(&t->output, " });");
    *pp = body_close + 1;
    return true;
}

static bool try_emit_header_include(Transpiler *t, size_t *pp)
{
    size_t p = *pp, n, dot = 0;
    Token *x = at(t, p);
    char *s;
    if (x->kind != TOK_STRING || p == 0 || !token_is(at(t, p - 1), "include") || x->length < 5 || x->begin[0] != '"') return false;
    for (n = 1; n + 1 < x->length; ++n) {
        if (x->begin[n] == '.') dot = n;
    }
    if (!dot || n + 1 != x->length || !((x->length - dot == 4 && memcmp(x->begin + dot, ".hx\"", 4) == 0) || (x->length - dot == 4 && memcmp(x->begin + dot, ".h+\"", 4) == 0))) return false;
    s = xmalloc(dot + 4);
    memcpy(s, x->begin, dot);
    memcpy(s + dot, ".h\"", 3);
    s[dot + 3] = 0;
    emit_ws(&t->output, x);
    buffer_puts(&t->output, s);
    free(s);
    ++*pp;
    return true;
}

static bool try_emit_method_call(Transpiler *t, NamespaceStack *ns, size_t *pp, size_t end)
{
    size_t p = *pp, scan = p;
    Token *x = at(t, p);
    char *mn;
    Symbol *current_type, *m;
    int ptr = 0;
    bool arrow = false, pass_receiver;

    if (x->kind != TOK_IDENTIFIER) return false;
    
    {
        char *base_name = token_text(x);
        current_type = lookup_value(t, ns, base_name, &ptr);
        free(base_name);
    }
    
    if (!current_type) return false;
    
    scan++;
    
    while (scan < end) {
        bool is_dot = at(t, scan)->kind == TOK_DOT;
        bool is_arrow = token_char(at(t, scan), '-') && scan + 1 < end && at(t, scan + 1)->kind == TOK_GT;
        
        if (is_dot || is_arrow) {
            size_t op_len = is_arrow ? 2 : 1;
            if (scan + op_len < end && at(t, scan + op_len)->kind == TOK_IDENTIFIER) {
                if (scan + op_len + 1 < end && at(t, scan + op_len + 1)->kind == TOK_LPAREN) {
                    arrow = is_arrow;
                    break;
                }
            }
        }
        
        if (is_dot) {
            if (scan + 1 >= end || at(t, scan + 1)->kind != TOK_IDENTIFIER) return false;
            char *field = token_text(at(t, scan + 1));
            Symbol *fsym = field_for(t, current_type->qualified_name, field);
            free(field);
            
            if (!fsym || !fsym->type_qualified) return false;
            current_type = symbol_exact(&t->symbols, fsym->type_qualified, SYM_TYPE | SYM_ENUM);
            if (!current_type) return false;
            
            ptr = fsym->pointer_depth;
            scan += 2;
        } else if (is_arrow) {
            if (ptr < 1) return false;
            if (scan + 2 >= end || at(t, scan + 2)->kind != TOK_IDENTIFIER) return false;
            char *field = token_text(at(t, scan + 2));
            Symbol *fsym = field_for(t, current_type->qualified_name, field);
            free(field);
            
            if (!fsym || !fsym->type_qualified) return false;
            current_type = symbol_exact(&t->symbols, fsym->type_qualified, SYM_TYPE | SYM_ENUM);
            if (!current_type) return false;
            
            ptr = fsym->pointer_depth;
            scan += 3;
        } else if (at(t, scan)->kind == TOK_LBRACKET) {
            size_t rbracket = matching(t, scan, TOK_LBRACKET, TOK_RBRACKET, end);
            if (rbracket >= end) return false;
            if (ptr > 0) ptr--;
            scan = rbracket + 1;
        } else {
            return false;
        }
    }
    
    if (scan >= end) return false;
    
    size_t op = scan;
    size_t method = op + (arrow ? 2 : 1);
    size_t lp = method + 1;
    size_t rp = matching(t, lp, TOK_LPAREN, TOK_RPAREN, end);
    
    mn = token_text(at(t, method));
    m = method_for(t, current_type->qualified_name, mn);
    free(mn);
    
    if (!m) return false;
    
    if (arrow && ptr < 1) {
        die_at(at(t, op), "'->' method call requires a pointer receiver");
    }
    
    pass_receiver = !m->is_static || m->receiver_pointer_depth > 0;
    if (pass_receiver && m->receiver_pointer_depth > 0 && (ptr ? ptr : 1) != m->receiver_pointer_depth) {
        warn_at(x, "receiver pointer depth differs from the method's declared receiver; generated C may warn");
    }
    
    emit_ws(&t->output, x);
    buffer_puts(&t->output, m->mangled_name);
    buffer_putc(&t->output, '(');
    
    if (pass_receiver) {
        if (!arrow) buffer_putc(&t->output, '&');
        emit_fragment_without_first_ws(t, ns, p, op);
        if (lp + 1 < rp) buffer_putc(&t->output, ',');
    }
    
    emit_fragment(t, ns, lp + 1, rp);
    buffer_putc(&t->output, ')');
    
    *pp = rp + 1;
    return true;
}

static void emit_one(Transpiler *t, NamespaceStack *ns, size_t *pp, size_t end)
{
    size_t p = *pp, used = 0;
    Token *x = at(t, p);
    char *q;
    Symbol *s;
    bool qualified;

    if (try_emit_expression_method_call(t, ns, pp, end)) return;
    if (try_emit_parenthesized_method_call(t, ns, pp, end)) return;
    if (try_emit_struct_literal_method_call(t, ns, pp, end)) return;
    if (try_emit_method_call(t, ns, pp, end)) return;
    if (try_emit_struct_literal(t, ns, pp, end)) return;
    if (try_emit_defer_block(t, ns, pp, end)) return;
    if (try_emit_switch(t, ns, pp, end)) return;
    if (try_emit_header_include(t, pp)) return;

    if (x->kind == TOK_IDENTIFIER && token_is(x, "return")) {
        emit_ws(&t->output, x);
        
        if (t->defers.count > 0) {
            bool has_defer_content = false;
            for (size_t d = t->defers.count; d > 0; --d) {
                if (buffer_has_non_whitespace(&t->defers.items[d - 1].output)) {
                    has_defer_content = true;
                    break;
                }
            }

            if (has_defer_content) {
                buffer_puts(&t->output, "{ ");
                for (size_t d = t->defers.count; d > 0; --d) {
                    DeferScope *scope = &t->defers.items[d - 1];
                    if (buffer_has_non_whitespace(&scope->output)) {
                        buffer_puts(&t->output, scope->output.data);
                    }
                }
                buffer_puts(&t->output, " } ");
            }
        }

        buffer_puts(&t->output, "return");
        ++*pp;
        return;
    }

    if (looks_like_c_style_cast(t, ns, p, end)) {
        warn_at(x, "C-style cast; prefer static_cast<Type>(expression)");
    }
    if (token_is(x, "static_cast") && p + 1 < end && at(t, p + 1)->kind == TOK_LT) {
        size_t close = matching(t, p + 1, TOK_LT, TOK_GT, end), i;
        emit_ws(&t->output, x);
        buffer_putc(&t->output, '(');
        for (i = p + 2; i < close; ) {
            Token *z = at(t, i);
            if (z->kind == TOK_IDENTIFIER) {
                size_t u = 0;
                char *qq = read_qualified(t, i, close, &u);
                Symbol *ts = qq ? resolve_name(&t->symbols, ns, qq, SYM_TYPE | SYM_ENUM) : NULL;
                if (ts) {
                    emit_ws(&t->output, z);
                    buffer_puts(&t->output, ts->mangled_name);
                    free(qq);
                    i += u;
                    continue;
                }
                free(qq);
            }
            emit_full(&t->output, z);
            ++i;
        }
        buffer_putc(&t->output, ')');
        *pp = close + 1;
        return;
    }
    if (x->kind == TOK_IDENTIFIER && token_is(x, "nullptr")) {
        emit_ws(&t->output, x);
        buffer_puts(&t->output, "((void *)0)");
        ++*pp;
        return;
    }
    if (x->kind == TOK_IDENTIFIER) {
        q = read_qualified(t, p, end, &used);
        qualified = q && strstr(q, "::") != NULL;
        if (qualified) {
            s = resolve_name(&t->symbols, ns, q, SYM_TYPE | SYM_ENUM | SYM_ENUM_MEMBER | SYM_FUNCTION | SYM_METHOD | SYM_VARIABLE);
            if (!s) {
                free(q);
                die_at(x, "unknown qualified name");
            }
            emit_ws(&t->output, x);
            buffer_puts(&t->output, s->mangled_name);
            free(q);
            *pp += used;
            return;
        }
        free(q);
        {
            char *name = token_text(x);
            Local *local = local_lookup(&t->locals, name);
            Symbol *var = resolve_name(&t->symbols, ns, name, SYM_VARIABLE);
            if (local) {
                emit_full(&t->output, x);
                free(name);
                ++*pp;
                return;
            }
            if (var) {
                emit_ws(&t->output, x);
                buffer_puts(&t->output, var->mangled_name);
                free(name);
                ++*pp;
                return;
            }
            s = resolve_name(&t->symbols, ns, name, SYM_TYPE | SYM_ENUM);
            if (s) {
                emit_ws(&t->output, x);
                buffer_puts(&t->output, s->mangled_name);
                free(name);
                ++*pp;
                return;
            }
            if (p + 1 < end && at(t, p + 1)->kind == TOK_LPAREN && !is_c_keyword(x)) {
                s = resolve_name(&t->symbols, ns, name, SYM_FUNCTION);
                if (s) {
                    emit_ws(&t->output, x);
                    buffer_puts(&t->output, s->mangled_name);
                    free(name);
                    ++*pp;
                    return;
                }
            }
            free(name);
        }
    }
    emit_full(&t->output, x);
    ++*pp;
}

static void add_parameters(Transpiler *t, NamespaceStack *ns, size_t begin, size_t end, const Symbol *owner, Symbol *method, bool method_static, const Token *where)
{
    size_t p = begin, ordinal = 0;
    if (begin == end) {
        if (owner && !method_static) die_at(where, "non-static method requires its first parameter to be a pointer to its struct");
        return;
    }
    if (end == begin + 1 && token_is(at(t, begin), "void")) {
        if (owner && !method_static) die_at(where, "non-static method requires its first parameter to be a pointer to its struct");
        return;
    }
    while (p < end) {
        size_t q = p;
        VarDecl d;
        while (q < end && at(t, q)->kind != TOK_COMMA) ++q;
        if (parse_var_decl(t, ns, p, q, &d)) {
            char *name = token_text(at(t, d.name));
            if (owner && ordinal == 0) {
                if (!method_static && (strcmp(d.type->qualified_name, owner->qualified_name) != 0 || d.pointer_depth < 1)) {
                    free(name);
                    die_at(where, "non-static method requires its first parameter to be a pointer to its struct");
                }
                if (method && strcmp(d.type->qualified_name, owner->qualified_name) == 0 && d.pointer_depth >= 1) {
                    method->receiver_pointer_depth = d.pointer_depth;
                }
            }
            local_add(&t->locals, name, d.type->qualified_name, d.pointer_depth, 1);
            free(name);
        } else if (owner && ordinal == 0 && !method_static) {
            die_at(where, "non-static method requires its first parameter to be a pointer to its struct");
        }
        ++ordinal;
        p = q + 1;
    }
}

static void emit_body(Transpiler *t, NamespaceStack *ns, size_t begin, size_t close)
{
    size_t p = begin;
    t->local_depth = 1;
    defer_stack_push(&t->defers);
    
    while (p < close) {
        VarDecl d;
        if (parse_var_decl(t, ns, p, close, &d)) {
            char *name = token_text(at(t, d.name));
            local_add(&t->locals, name, d.type->qualified_name, d.pointer_depth, t->local_depth);
            free(name);
        }
        
        if (at(t, p)->kind == TOK_LBRACE) {
            defer_stack_push(&t->defers);
            emit_full(&t->output, at(t, p));
            ++t->local_depth;
            ++p;
            continue;
        }
        
        if (at(t, p)->kind == TOK_RBRACE) {
            DeferScope *scope = defer_stack_current(&t->defers);
            
            if (buffer_has_non_whitespace(&scope->output)) {
                buffer_puts(&t->output, scope->output.data);
            }
            
            defer_stack_pop(&t->defers);
            locals_leave(&t->locals, t->local_depth);
            --t->local_depth;
            emit_full(&t->output, at(t, p));
            ++p;
            continue;
        }
        
        emit_one(t, ns, &p, close);
    }
    
    if (t->defers.count) {
        DeferScope *scope = defer_stack_current(&t->defers);
        
        if (buffer_has_non_whitespace(&scope->output)) {
            buffer_puts(&t->output, scope->output.data);
        }
    }
    
    defer_stack_pop(&t->defers);
}

static void transpile_function(Transpiler *t, NamespaceStack *ns, size_t start, const Callable *c)
{
    size_t p = start, mark = locals_mark(&t->locals);
    
    size_t q_start = c->name;
    while (q_start >= 2 && at(t, q_start - 1)->kind == TOK_SCOPE && at(t, q_start - 2)->kind == TOK_IDENTIFIER) {
        q_start -= 2;
    }
    size_t used = 0;
    char *q_name = read_qualified(t, q_start, c->name + 1, &used);
    char *q = qualify(ns, ns->count, q_name);
    Symbol *sym = symbol_exact(&t->symbols, q, SYM_FUNCTION | SYM_METHOD);

    add_parameters(t, ns, c->lparen + 1, c->rparen, NULL, NULL, false, at(t, c->name));
    
    while (p < c->body) {
        if (p == start && c->is_static && token_is(at(t, p), "static")) {
            ++p;
            continue;
        }
        if (p >= q_start && p <= c->name) {
            if (p == q_start) {
                emit_ws(&t->output, at(t, p));
                buffer_puts(&t->output, sym ? sym->mangled_name : q_name);
            }
            p++;
            continue;
        }
        emit_one(t, ns, &p, c->body);
    }
    emit_full(&t->output, at(t, c->body));
    emit_body(t, ns, c->body + 1, c->close);
    emit_full(&t->output, at(t, c->close));
    locals_restore(&t->locals, mark);
    free(q_name);
    free(q);
}

static void transpile_method(Transpiler *t, NamespaceStack *ns, size_t start, const Callable *c, Symbol *owner, Symbol *method)
{
    size_t p = start, mark = locals_mark(&t->locals);
    const char *old_struct = t->current_struct, *old_method = t->current_method;
    bool old_static = t->current_method_static;

    t->current_struct = owner->qualified_name;
    t->current_method = method->qualified_name;
    t->current_method_static = method->is_static;
    add_parameters(t, ns, c->lparen + 1, c->rparen, owner, method, method->is_static, at(t, c->name));

    while (p < c->body) {
        if (p == start && c->is_static && token_is(at(t, p), "static")) {
            ++p;
            continue;
        }
        if (p == c->name) {
            emit_ws(&t->output, at(t, p));
            buffer_puts(&t->output, method->mangled_name);
            ++p;
            continue;
        }
        emit_one(t, ns, &p, c->body);
    }
    emit_full(&t->output, at(t, c->body));
    emit_body(t, ns, c->body + 1, c->close);
    emit_full(&t->output, at(t, c->close));
    locals_restore(&t->locals, mark);
    t->current_struct = old_struct;
    t->current_method = old_method;
    t->current_method_static = old_static;
}

static void transpile_struct(Transpiler *t, NamespaceStack *ns, size_t start, size_t open, size_t close)
{
    size_t p, after = close + 1, nused;
    char *name = token_text(at(t, start + 1)), *q = qualify(ns, ns->count, name);
    Symbol *owner = symbol_exact(&t->symbols, q, SYM_TYPE);

    emit_ws(&t->output, at(t, start));
    buffer_puts(&t->output, "typedef struct ");
    buffer_puts(&t->output, owner->mangled_name);
    emit_full(&t->output, at(t, open));

    for (p = open + 1; p < close; ) {
        Callable c;
        if (parse_callable(t, p, close, &c)) {
            p = c.after;
            continue;
        }
        emit_one(t, ns, &p, close);
    }
    emit_ws(&t->output, at(t, close));
    buffer_puts(&t->output, "} ");
    buffer_puts(&t->output, owner->mangled_name);
    buffer_putc(&t->output, ';');
    if (after < t->tokens.count && at(t, after)->kind == TOK_SEMICOLON) ++after;

    for (p = open + 1; p < close; ) {
        Callable c;
        if (parse_callable(t, p, close, &c)) {
            char *mn = token_text(at(t, c.name));
            Buffer b;
            Symbol *method;
            buffer_init(&b);
            buffer_puts(&b, owner->qualified_name);
            buffer_puts(&b, "::");
            buffer_puts(&b, mn);
            method = symbol_exact(&t->symbols, b.data, SYM_METHOD);
            free(mn);
            buffer_free(&b);
            if (c.body < close) {
                buffer_putc(&t->output, '\n');
                transpile_method(t, ns, p, &c, owner, method);
            }
            p = c.after;
            continue;
        }
        ++p;
    }
    (void)nused;
    free(name);
    free(q);
}

static void transpile_enum(Transpiler *t, NamespaceStack *ns, size_t start, size_t open, size_t close)
{
    size_t p, after = close + 1;
    bool want = true;
    char *name = token_text(at(t, start + 1)), *q = qualify(ns, ns->count, name);
    Symbol *owner = symbol_exact(&t->symbols, q, SYM_ENUM);

    emit_ws(&t->output, at(t, start));
    buffer_puts(&t->output, "typedef enum ");
    buffer_puts(&t->output, owner->mangled_name);
    emit_full(&t->output, at(t, open));

    for (p = open + 1; p < close; ) {
        if (want && at(t, p)->kind == TOK_IDENTIFIER) {
            char *field = token_text(at(t, p));
            Buffer b;
            Symbol *member;
            buffer_init(&b);
            buffer_puts(&b, owner->qualified_name);
            buffer_puts(&b, "::");
            buffer_puts(&b, field);
            member = symbol_exact(&t->symbols, b.data, SYM_ENUM_MEMBER);
            emit_ws(&t->output, at(t, p));
            buffer_puts(&t->output, member->mangled_name);
            free(field);
            buffer_free(&b);
            ++p;
            want = false;
            continue;
        }
        if (at(t, p)->kind == TOK_COMMA) want = true;
        emit_one(t, ns, &p, close);
    }
    emit_ws(&t->output, at(t, close));
    buffer_puts(&t->output, "} ");
    buffer_puts(&t->output, owner->mangled_name);
    buffer_putc(&t->output, ';');
    if (after < t->tokens.count && at(t, after)->kind == TOK_SEMICOLON) ++after;
    free(name);
    free(q);
}

static void transpile_function_decl(Transpiler *t, NamespaceStack *ns, size_t start, const Callable *c)
{
    size_t p = start;
    size_t q_start = c->name;
    while (q_start >= 2 && at(t, q_start - 1)->kind == TOK_SCOPE && at(t, q_start - 2)->kind == TOK_IDENTIFIER) {
        q_start -= 2;
    }
    size_t used = 0;
    char *q_name = read_qualified(t, q_start, c->name + 1, &used);
    char *q = qualify(ns, ns->count, q_name);
    Symbol *sym = symbol_exact(&t->symbols, q, SYM_FUNCTION | SYM_METHOD);
    
    while (p < c->after) {
        if (p == start && c->is_static && token_is(at(t, p), "static")) {
            ++p;
            continue;
        }
        if (p >= q_start && p <= c->name) {
            if (p == q_start) {
                emit_ws(&t->output, at(t, p));
                buffer_puts(&t->output, sym ? sym->mangled_name : q_name);
            }
            p++;
            continue;
        }
        emit_one(t, ns, &p, c->after);
    }
    free(q_name);
    free(q);
}

static void transpile_range(Transpiler *t, NamespaceStack *ns, size_t begin, size_t end)
{
    size_t p = begin;
    while (p < end) {
        Token *x = at(t, p);
        Callable c;

        if (token_is(x, "namespace") && p + 2 < end && at(t, p + 1)->kind == TOK_IDENTIFIER && at(t, p + 2)->kind == TOK_LBRACE) {
            size_t close = matching(t, p + 2, TOK_LBRACE, TOK_RBRACE, end);
            emit_ws(&t->output, x);
            ns_push(ns, at(t, p + 1));
            transpile_range(t, ns, p + 3, close);
            ns_pop(ns);
            emit_ws(&t->output, at(t, close));
            p = close + 1;
            continue;
        }
        if ((token_is(x, "struct") || token_is(x, "enum")) && p + 2 < end && at(t, p + 1)->kind == TOK_IDENTIFIER && at(t, p + 2)->kind == TOK_LBRACE) {
            size_t close = matching(t, p + 2, TOK_LBRACE, TOK_RBRACE, end);
            if (token_is(x, "struct")) {
                transpile_struct(t, ns, p, p + 2, close);
            } else {
                transpile_enum(t, ns, p, p + 2, close);
            }
            p = close + 1;
            if (p < end && at(t, p)->kind == TOK_SEMICOLON) ++p;
            continue;
        }
        if (token_is(x, "struct") && p + 1 < end && at(t, p + 1)->kind == TOK_IDENTIFIER && p + 2 < end && at(t, p + 2)->kind == TOK_SEMICOLON) {
            char *name = token_text(at(t, p + 1)), *q = qualify(ns, ns->count, name);
            Symbol *owner = symbol_exact(&t->symbols, q, SYM_TYPE);
            emit_ws(&t->output, x);
            buffer_puts(&t->output, "typedef struct ");
            buffer_puts(&t->output, owner ? owner->mangled_name : name);
            buffer_puts(&t->output, " ");
            buffer_puts(&t->output, owner ? owner->mangled_name : name);
            buffer_puts(&t->output, ";");
            free(name);
            free(q);
            p += 3;
            continue;
        }
        if (token_is(x, "enum") && p + 1 < end && at(t, p + 1)->kind == TOK_IDENTIFIER && p + 2 < end && at(t, p + 2)->kind == TOK_SEMICOLON) {
            char *name = token_text(at(t, p + 1)), *q = qualify(ns, ns->count, name);
            Symbol *owner = symbol_exact(&t->symbols, q, SYM_ENUM);
            emit_ws(&t->output, x);
            buffer_puts(&t->output, "typedef enum ");
            buffer_puts(&t->output, owner ? owner->mangled_name : name);
            buffer_puts(&t->output, " ");
            buffer_puts(&t->output, owner ? owner->mangled_name : name);
            buffer_puts(&t->output, ";");
            free(name);
            free(q);
            p += 3;
            continue;
        }
        if (parse_callable(t, p, end, &c)) {
            if (c.body < end) {
                transpile_function(t, ns, p, &c);
            } else {
                transpile_function_decl(t, ns, p, &c);
            }
            p = c.after;
            continue;
        }
        emit_one(t, ns, &p, end);
    }
}

void transpile(Transpiler *t)
{
    NamespaceStack ns;
    ns_init(&ns);
#if defined(_WIN32) || defined(_WIN64)
    buffer_puts(&t->output, "/* Generated by C+ compiler. DO NOT EDIT. */\r\n\r\n");
    buffer_puts(&t->output,
    "#ifndef CMPL__PTR_SIZE\r\n"
    "#if defined(_WIN64) || defined(__x86_64__)\r\n"
    "#define CMPL__PTR_SIZE 8\r\n"
    "#elif defined(_WIN16) || defined(__I86__)\r\n"
    "#define CMPL__PTR_SIZE 2\r\n"
    "#else\r\n"
    "#define CMPL__PTR_SIZE 4\r\n"
    "#endif\r\n"
    "#endif\r\n\r\n");
    buffer_puts(&t->output, "#ifndef true\r\n#define true 1\r\n#endif\r\n#ifndef false\r\n#define false 0\r\n#endif\r\n\r\n");
    buffer_puts(&t->output, 
    "#ifndef bool\r\n"
    "#if CMPL__PTR_SIZE == 8\r\n"
    "#define bool unsigned int\r\n"
    "#elif CMPL__PTR_SIZE == 4\r\n"
    "#define bool unsigned short\r\n"
    "#else\r\n"
    "#define bool unsigned char\r\n"
    "#endif\r\n"
    "#endif // #ifndef bool\r\n\r\n");
#else
    buffer_puts(&t->output, "/* Generated by C+ compiler. DO NOT EDIT. */\n\n");
    buffer_puts(&t->output,
    "#ifndef CMPL__PTR_SIZE\n"
    "#if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv64) || defined(__loongarch64)\n"
    "#define CMPL__PTR_SIZE 8\n"
    "#elif defined(__i386__) || defined(__arm__) || defined(__riscv) || defined(__mips__) || defined(__powerpc__)\n"
    "#define CMPL__PTR_SIZE 4\n"
    "#elif defined(__MSP430__) || defined(__AVR__) || defined(__m16c__) || defined(__RL78__)\n"
    "#define CMPL__PTR_SIZE 2\n"
    "#else\n"
    "#define CMPL__PTR_SIZE 4\n"
    "#endif\n"
    "#endif\n\n");
    buffer_puts(&t->output, "#ifndef true\n#define true 1\n#endif\n#ifndef false\n#define false 0\n#endif\n\n");
    buffer_puts(&t->output, 
    "#ifndef bool\n"
    "#if CMPL__PTR_SIZE == 8\n"
    "#define bool unsigned int\n"
    "#elif CMPL__PTR_SIZE == 4\n"
    "#define bool unsigned short\n"
    "#else\n"
    "#define bool unsigned char\n"
    "#endif\n"
    "#endif // #ifndef bool\n\n");
#endif
    discover_range(t, &ns, 0, t->tokens.count - 1);
    transpile_range(t, &ns, 0, t->tokens.count - 1);
    ns_free(&ns);
}