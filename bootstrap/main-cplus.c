#include "common.h"
#include "lexer.h"
#include "transpiler.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#endif

bool ALLOW_WARNINGS = true;

/* --- TIMER UTILITY --- */

#ifdef _WIN32
static double timer_now(void)
{
    static LARGE_INTEGER freq;
    static bool initialized = false;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = true;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}
#else
static double timer_now(void)
{
    return (double)clock() / (double)CLOCKS_PER_SEC;
}
#endif

/* --- DYNAMIC ARGUMENT ARRAY --- */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} Arguments;

static void args_init(Arguments *a)
{
    memset(a, 0, sizeof(*a));
}

static void args_push(Arguments *a, const char *s)
{
    if (a->count == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->items = xrealloc(a->items, a->cap * sizeof(char *));
    }
    a->items[a->count++] = xstrdup(s);
}

static void args_free(Arguments *a)
{
    for (size_t i = 0; i < a->count; ++i) {
        free(a->items[i]);
    }
    free(a->items);
    memset(a, 0, sizeof(*a));
}

/* --- PROCESS EXECUTION --- */

#ifdef _WIN32
static int run_process(Arguments *a)
{
    Buffer c;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    
    buffer_init(&c);
    for (size_t i = 0; i < a->count; ++i) {
        const char *s = a->items[i];
        if (i > 0) buffer_putc(&c, ' ');
        buffer_putc(&c, '"');
        while (*s) {
            if (*s == '"' || *s == '\\') buffer_putc(&c, '\\');
            buffer_putc(&c, *s++);
        }
        buffer_putc(&c, '"');
    }

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessA(NULL, c.data, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        buffer_free(&c);
        fprintf(stderr, "c+: error: could not execute backend\n");
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    buffer_free(&c);
    return (int)code;
}
#else
static int run_process(Arguments *a)
{
    char **v = xmalloc((a->count + 1) * sizeof(char *));
    for (size_t i = 0; i < a->count; ++i) {
        v[i] = a->items[i];
    }
    v[a->count] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        free(v);
        perror("c+: fork");
        return 1;
    }

    if (pid == 0) {
        execvp(v[0], v);
        perror("c+: exec");
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        free(v);
        perror("c+: waitpid");
        return 1;
    }

    free(v);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif

/* --- PLATFORM-SPECIFIC GLOBBING IMPLEMENTATION --- */

static void add_globbed_argument(Arguments *dst, const char *pattern)
{
    if (!strchr(pattern, '*') && !strchr(pattern, '?')) {
        args_push(dst, pattern);
        return;
    }

#ifdef _WIN32
    char dir_path[MAX_PATH];
    const char *last_slash = strrchr(pattern, '/');
    const char *last_backslash = strrchr(pattern, '\\');
    const char *slash = last_slash > last_backslash ? last_slash : last_backslash;

    size_t dir_len = 0;
    if (slash) {
        dir_len = (size_t)(slash - pattern + 1);
        if (dir_len >= sizeof(dir_path)) dir_len = sizeof(dir_path) - 1;
        memcpy(dir_path, pattern, dir_len);
        dir_path[dir_len] = '\0';
    } else {
        strcpy_s(dir_path, sizeof(dir_path), ".\\");
    }

    WIN32_FIND_DATAA find_data;
    HANDLE h_find = FindFirstFileA(pattern, &find_data);
    if (h_find == INVALID_HANDLE_VALUE) {
        args_push(dst, pattern);
        return;
    }

    bool matched = false;
    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char full_path[MAX_PATH];
            if (slash) {
                snprintf(full_path, sizeof(full_path), "%.*s%s", (int)dir_len, pattern, find_data.cFileName);
            } else {
                snprintf(full_path, sizeof(full_path), "%s", find_data.cFileName);
            }
            args_push(dst, full_path);
            matched = true;
        }
    } while (FindNextFileA(h_find, &find_data));
    FindClose(h_find);

    if (!matched) {
        args_push(dst, pattern);
    }
#else
    char dir_path[1024];
    const char *slash = strrchr(pattern, '/');
    const char *file_pattern = slash ? slash + 1 : pattern;

    if (slash) {
        size_t len = (size_t)(slash - pattern);
        if (len >= sizeof(dir_path)) len = sizeof(dir_path) - 1;
        memcpy(dir_path, pattern, len);
        dir_path[len] = '\0';
    } else {
        strcpy(dir_path, ".");
    }

    DIR *d = opendir(dir_path);
    if (!d) {
        args_push(dst, pattern);
        return;
    }

    bool matched = false;
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (fnmatch(file_pattern, dir->d_name, 0) == 0) {
            char full_path[2048];
            if (slash) {
                snprintf(full_path, sizeof(full_path), "%.*s/%s", (int)(slash - pattern), pattern, dir->d_name);
            } else {
                snprintf(full_path, sizeof(full_path), "%s", dir->d_name);
            }
            
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
                args_push(dst, full_path);
                matched = true;
            }
        }
    }
    closedir(d);

    if (!matched) {
        args_push(dst, pattern);
    }
#endif
}

/* --- JSON MANIFEST PARSING UTILITIES --- */

static bool extract_json_value(const char *json, const char *key, char *dest, size_t dest_size)
{
    char search_pattern[128];
    snprintf(search_pattern, sizeof(search_pattern), "\"%s\"", key);
    const char *p = strstr(json, search_pattern);
    if (!p) return false;
    
    p = strchr(p, ':');
    if (!p) return false;

    while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '"')) {
        p++;
    }

    size_t i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && i < dest_size - 1) {
        dest[i++] = *p++;
    }
    dest[i] = '\0';
    return i > 0;
}

static bool extract_nested_dependency_version(const char *json, const char *dep_name, char *dest, size_t dest_size)
{
    char dep_pattern[128];
    snprintf(dep_pattern, sizeof(dep_pattern), "\"%s\"", dep_name);
    const char *p = strstr(json, dep_pattern);
    if (!p) return false;
    return extract_json_value(p, "version", dest, dest_size);
}

/* --- CLI & HELPERS --- */

static void usage(void)
{
    char cplus_version[32] = "unknown";
    char *manifest = read_file("manifest.json");
    if (manifest) {
        extract_json_value(manifest, "version", cplus_version, sizeof(cplus_version));
        free(manifest);
    }
    printf(
        "C+ compiler (v%s)\n\n"
        "Version:\n  %s\n"
        "Extensions:\n  .cp\n  .c+\n  .hp\n  .h+\n\n"
        "Usage:\n"
        "  c+    [options] file.('cp'|'c+') [...files]\n"
        "  cc+   [options] file.('cp'|'c+') [...files]\n"
        "  cplus [options] file.('cp'|'c+') [...files]\n\n"
        "  Options:\n"
        "  -v, --version Outputs the installed C+ version and exits\n"
        "  -h, --help    Outputs this message and exits\n"
        "  -o <file>     Output executable or file\n"
        "  -c            Compile to an object file\n"
        "  -C            Emit generated C only and exit\n"
        "  -D <macro>    Define preprocessor macro (forwarded to TCC)\n"
        "  -I <dir>      Add include directory (forwarded to TCC)\n"
        "  --keep-c      Keep generated .gencx.c file\n"
        "  --index       Emit token index for IDE highlighting services\n"
        "  --            Stop processing C+ options\n",
        cplus_version, cplus_version
    );
}

static void version(void)
{
    char cplus_version[32] = "unknown";
    char libcplus_version[32] = "unknown";
    char tcc_version[32] = "unknown";
    char *manifest = read_file("manifest.json");
    char *manifest2 = read_file("libc+/manifest.json");
    if (manifest) {
        extract_json_value(manifest, "version", cplus_version, sizeof(cplus_version));
        extract_nested_dependency_version(manifest, "tcc", tcc_version, sizeof(tcc_version));
        free(manifest);
    }
    if (manifest2) {
        extract_json_value(manifest2, "version", libcplus_version, sizeof(libcplus_version));
        free(manifest2);
    }
    printf("C+ version:\n  %s\nlibc+ version:\n  %s\nTinyCC version:\n  %s\n", cplus_version, libcplus_version, tcc_version);
}

static char *generated_name(const char *in)
{
    size_t n = strlen(in);
    char *s = xmalloc(n + 9);
    snprintf(s, n + 9, "%s.gencx.c", in);
    return s;
}

static char *replace_extension(const char *s, const char *ext)
{
    const char *dot = strrchr(s, '.');
    size_t n = dot ? (size_t)(dot - s) : strlen(s);
    size_t e = strlen(ext);
    char *r = xmalloc(n + e + 1);
    memcpy(r, s, n);
    memcpy(r + n, ext, e + 1);
    return r;
}

/* --- CENTRALIZED STATISTICS SYSTEM --- */

typedef struct {
    size_t input_files;
    size_t source_lines;
    size_t source_bytes;
    size_t source_tokens;

    size_t generated_files;
    size_t generated_lines;
    size_t generated_bytes;

    double preprocess_time;
    double lexer_time;
    double transpiler_time;
    double backend_time;
    double total_time;
} CompilerStats;

static void format_commas(size_t val, char *out, size_t out_size)
{
    char raw[32];
    snprintf(raw, sizeof(raw), "%zu", val);
    size_t len = strlen(raw);
    size_t commas = (len > 0) ? (len - 1) / 3 : 0;
    size_t total_len = len + commas;

    if (total_len >= out_size) {
        snprintf(out, out_size, "%zu", val);
        return;
    }

    out[total_len] = '\0';
    size_t in_i = len;
    size_t out_i = total_len;
    size_t digit_count = 0;

    while (in_i > 0) {
        if (digit_count > 0 && digit_count % 3 == 0) {
            out[--out_i] = ',';
        }
        out[--out_i] = raw[--in_i];
        digit_count++;
    }
}

static void print_stats(const CompilerStats *s)
{
    double total_time = s->total_time > 0.0 ? s->total_time : 0.000001;
    double frontend_time = s->preprocess_time + s->lexer_time + s->transpiler_time;

    char f_in_files[32], f_src_lines[32], f_src_bytes[32], f_tokens[32];
    char f_gen_files[32], f_gen_lines[32], f_gen_bytes[32];
    char f_lines_sec[32], f_tokens_sec[32], f_src_bytes_sec[32], f_gen_bytes_sec[32], f_gen_lines_sec[32];

    format_commas(s->input_files, f_in_files, sizeof(f_in_files));
    format_commas(s->source_lines, f_src_lines, sizeof(f_src_lines));
    format_commas(s->source_bytes, f_src_bytes, sizeof(f_src_bytes));
    format_commas(s->source_tokens, f_tokens, sizeof(f_tokens));

    format_commas(s->generated_files, f_gen_files, sizeof(f_gen_files));
    format_commas(s->generated_lines, f_gen_lines, sizeof(f_gen_lines));
    format_commas(s->generated_bytes, f_gen_bytes, sizeof(f_gen_bytes));

    double byte_ratio = s->source_bytes > 0 ? (double)s->generated_bytes / (double)s->source_bytes : 0.0;
    double line_ratio = s->source_lines > 0 ? (double)s->generated_lines / (double)s->source_lines : 0.0;
    double tokens_per_line = s->source_lines > 0 ? (double)s->source_tokens / (double)s->source_lines : 0.0;
    double avg_bytes_per_line = s->source_lines > 0 ? (double)s->source_bytes / (double)s->source_lines : 0.0;
    double gen_bytes_per_line = s->source_lines > 0 ? (double)s->generated_bytes / (double)s->source_lines : 0.0;

    double lines_sec = (double)s->source_lines / total_time;
    double tokens_sec = (double)s->source_tokens / total_time;
    double src_bytes_sec = (double)s->source_bytes / total_time;
    double gen_bytes_sec = (double)s->generated_bytes / total_time;
    double gen_lines_sec = (double)s->generated_lines / total_time;

    format_commas((size_t)(lines_sec + 0.5), f_lines_sec, sizeof(f_lines_sec));
    format_commas((size_t)(tokens_sec + 0.5), f_tokens_sec, sizeof(f_tokens_sec));
    format_commas((size_t)(src_bytes_sec + 0.5), f_src_bytes_sec, sizeof(f_src_bytes_sec));
    format_commas((size_t)(gen_bytes_sec + 0.5), f_gen_bytes_sec, sizeof(f_gen_bytes_sec));
    format_commas((size_t)(gen_lines_sec + 0.5), f_gen_lines_sec, sizeof(f_gen_lines_sec));

    printf("\n==================================================\n");
    printf("                  C+ STATISTICS                   \n");
    printf("==================================================\n\n");

    printf("[ Input ]\n");
    printf("  Files         : %10s\n", f_in_files);
    printf("  Source Lines  : %10s\n", f_src_lines);
    printf("  Source Bytes  : %10s bytes\n", f_src_bytes);
    printf("  Tokens        : %10s\n\n", f_tokens);

    printf("[ Output ]\n");
    printf("  Generated C Files : %6s\n", f_gen_files);
    printf("  Generated Lines   : %10s\n", f_gen_lines);
    printf("  Generated Bytes   : %10s bytes\n\n", f_gen_bytes);

    printf("[ Expansion ]\n");
    printf("  Byte Ratio    : %10.2fx\n", byte_ratio);
    printf("  Line Ratio    : %10.2fx\n", line_ratio);
    printf("  Tokens/Line   : %10.2f\n", tokens_per_line);
    printf("  Avg Bytes/Line: %10.2f\n", avg_bytes_per_line);
    printf("  Gen Bytes/Line: %10.2f\n\n", gen_bytes_per_line);

    printf("[ Timing ]\n");
    printf("  Preprocessor  : %8.3f ms (%5.1f%%)\n", s->preprocess_time * 1000.0, (s->preprocess_time / total_time) * 100.0);
    printf("  Lexer         : %8.3f ms (%5.1f%%)\n", s->lexer_time * 1000.0, (s->lexer_time / total_time) * 100.0);
    printf("  Transpiler    : %8.3f ms (%5.1f%%)\n", s->transpiler_time * 1000.0, (s->transpiler_time / total_time) * 100.0);
    printf("  ------------------------------------\n");
    printf("  Frontend      : %8.3f ms (%5.1f%%)\n", frontend_time * 1000.0, (frontend_time / total_time) * 100.0);
    if (s->backend_time > 0.0) {
        printf("  Backend (TCC) : %8.3f ms (%5.1f%%)\n", s->backend_time * 1000.0, (s->backend_time / total_time) * 100.0);
    }
    printf("  ------------------------------------\n");
    printf("  Total         : %8.3f ms\n\n", total_time * 1000.0);

    printf("[ Throughput ]\n");
    printf("  Lines/sec         : %11s\n", f_lines_sec);
    printf("  Tokens/sec        : %11s\n", f_tokens_sec);
    printf("  Source Bytes/sec  : %11s\n", f_src_bytes_sec);
    printf("  Generated Bytes/sec : %11s\n", f_gen_bytes_sec);
    printf("  Generated Lines/sec : %11s\n\n", f_gen_lines_sec);

    printf("==================================================\n");
}

/* --- IDE INDEX EMISSION --- */

static void emit_ide_index(const TokenList *list)
{
    printf("[\n");
    for (size_t i = 0; i < list->count && list->items[i].kind != TOK_EOF; ++i) {
        const Token *t = &list->items[i];
        const char *k = "OTHER";

        if (t->kind == TOK_IDENTIFIER) {
            k = is_c_keyword(t) ? "KEYWORD" : "IDENTIFIER";
        } else if (t->kind == TOK_NUMBER) {
            k = "NUMBER";
        } else if (t->kind == TOK_STRING) {
            k = "STRING";
        } else if (t->kind == TOK_CHAR) {
            k = "CHAR";
        } else if (t->kind >= TOK_LBRACE && t->kind <= TOK_GT) {
            k = "PUNCTUATION";
        }

        printf("  { \"line\": %zu, \"column\": %zu, \"length\": %zu, \"kind\": \"%s\", \"text\": \"",
               t->line, t->column, t->length, k);

        for (size_t j = 0; j < t->length; ++j) {
            char c = t->begin[j];
            if (c == '"') printf("\\\"");
            else if (c == '\\') printf("\\\\");
            else if (c == '\n') printf("\\n");
            else putchar(c);
        }
        printf("\" }%s\n", (i + 1 < list->count && list->items[i + 1].kind != TOK_EOF) ? "," : "");
    }
    printf("]\n");
}

/* --- MAIN ENTRY POINT --- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *output = NULL;
    char *allocated_output = NULL;
    bool emit_c = false, compile_only = false, keep = false;
    bool index = false, endopt = false, notify = false, stats = false;

    Arguments backend, inputs, generated_files, command;
    args_init(&backend);
    args_init(&inputs);
    args_init(&generated_files);
    args_init(&command);

    CompilerStats cstats;
    memset(&cstats, 0, sizeof(cstats));

    double total_start = timer_now();
    int status = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!endopt && strcmp(a, "--") == 0) {
            endopt = true;
            continue;
        }
        if (!endopt && (!strcmp(a, "-h") || !strcmp(a, "--help"))) {
            usage();
            return 0;
        }
        if (!endopt && (!strcmp(a, "-v") || !strcmp(a, "--version"))) {
            version();
            return 0;
        }
        if (!endopt && !strcmp(a, "--stats")) {
            stats = true;
            continue;
        }
        if (!endopt && !strcmp(a, "-o")) {
            if (++i >= argc) die("-o requires an argument");
            output = argv[i];
            continue;
        }
        if (!endopt && !strcmp(a, "-w")) {
            ALLOW_WARNINGS = false;
            continue;
        }
        if (!endopt && !strcmp(a, "-C")) {
            emit_c = true;
            continue;
        }
        if (!endopt && !strcmp(a, "-c")) {
            compile_only = true;
            args_push(&backend, a);
            continue;
        }
        if (!endopt && (!strcmp(a, "-D") || !strcmp(a, "-I"))) {
            if (++i >= argc) die("-D/-I requires an argument");
            args_push(&backend, a);
            args_push(&backend, argv[i]);
            continue;
        }
        if (!endopt && !strcmp(a, "--keep-c")) {
            keep = true;
            continue;
        }
        if (!endopt && !strcmp(a, "--index")) {
            index = true;
            continue;
        }
        if (!endopt && !strcmp(a, "--notify")) {
            notify = true;
            continue;
        }

        if (!endopt && a[0] != '-') {
            add_globbed_argument(&inputs, a);
            continue;
        }

        args_push(&backend, a);
    }

    if (inputs.count == 0) {
        die("no input .c+ file specified");
    }

    cstats.input_files = inputs.count;

    /* Output default deduction */
    if (!output) {
        if (!compile_only) {
            output = "out"
#ifdef _WIN32
            ".exe"
#endif
            ;
        } else if (inputs.count == 1) {
            allocated_output = replace_extension(inputs.items[0], ".o");
            output = allocated_output;
        }
    }

    /* --- COMPILE EVERY INPUT FILE TO .c --- */
    for (size_t fidx = 0; fidx < inputs.count; fidx++) {
        const char *input_file = inputs.items[fidx];

        char *source = read_file(input_file);
        if (!source) {
            fprintf(stderr, "c+: error: could not read '%s': %s\n", input_file, strerror(errno));
            status = 1;
            goto cleanup;
        }

        size_t s_bytes = strlen(source);
        size_t s_lines = 1;
        for (char *p = source; *p; p++) {
            if (*p == '\n') s_lines++;
        }

        cstats.source_bytes += s_bytes;
        cstats.source_lines += s_lines;

        double preprocess_start = timer_now();
        char *preprocessed = preprocess_source(input_file, source, 0);
        free(source);
        source = preprocessed;
        cstats.preprocess_time += (timer_now() - preprocess_start);

        double lex_start = timer_now();
        Lexer lex;
        TokenList toks;
        lexer_init(&lex, source);
        tokens_init(&toks);

        for (;;) {
            Token x = lexer_next(&lex);
            cstats.source_tokens++;
            tokens_push(&toks, x);
            if (x.kind == TOK_EOF) break;
        }
        cstats.lexer_time += (timer_now() - lex_start);

        if (index) {
            emit_ide_index(&toks);
            tokens_free(&toks);
            free(source);
            continue;
        }

        double transpile_start = timer_now();
        Transpiler t;
        memset(&t, 0, sizeof(t));
        t.source = source;
        t.tokens = toks;
        symbols_init(&t.symbols);
        locals_init(&t.locals);
        buffer_init(&t.output);

        transpile(&t);
        cstats.transpiler_time += (timer_now() - transpile_start);

        size_t g_bytes = t.output.len;
        size_t g_lines = 1;
        for (size_t x = 0; x < g_bytes; x++) {
            if (t.output.data[x] == '\n') g_lines++;
        }

        cstats.generated_bytes += g_bytes;
        cstats.generated_lines += g_lines;
        cstats.generated_files++;

        char *generated_tmp = generated_name(input_file);
        args_push(&generated_files, generated_tmp);

        FILE *f = fopen(generated_tmp, "wb");
        if (!f) die("could not create generated C file");
        fwrite(t.output.data, 1, t.output.len, f);
        fclose(f);
        free(generated_tmp);

        if (emit_c) {
            printf("C+ -> C: %s\n", generated_files.items[generated_files.count - 1]);
        }

        buffer_free(&t.output);
        locals_free(&t.locals);
        symbols_free(&t.symbols);
        tokens_free(&toks);
        free(source);
    }

    if (index || status != 0) {
        goto cleanup;
    }

    cstats.total_time = timer_now() - total_start;

    if (emit_c) {
        if (stats) print_stats(&cstats);
        goto cleanup;
    }

    /* --- BACKEND BATCH EXECUTION --- */
    double backend_start = timer_now();
    args_push(&command, "tcc");
    args_push(&command, "-w");

    for (size_t i = 0; i < backend.count; i++) {
        args_push(&command, backend.items[i]);
    }

    if (output) {
        args_push(&command, "-o");
        args_push(&command, output);
    }

    for (size_t i = 0; i < generated_files.count; i++) {
        args_push(&command, generated_files.items[i]);
    }

    status = run_process(&command);
    cstats.backend_time = timer_now() - backend_start;
    cstats.total_time = timer_now() - total_start;

    if (stats) {
        print_stats(&cstats);
    }

    for (size_t i = 0; i < generated_files.count; i++) {
        if (status == 0 && !keep) {
            remove(generated_files.items[i]);
        }
    }

    if (status != 0) {
        fprintf(stderr, "c+: error: backend compilation failed (exit code %d)\n", status);
        fprintf(stderr, "c+: generated C files were kept.\n");
    }

    if (!status && notify) {
        if (inputs.count == 1) {
            printf("[C+] compilation of '%s' successfully finished!\n", inputs.items[0]);
        } else {
            printf("[C+] compilation of %zu files successfully finished!\n", inputs.count);
        }
    }

cleanup:
    if (allocated_output) free(allocated_output);
    args_free(&command);
    args_free(&generated_files);
    args_free(&inputs);
    args_free(&backend);

    return status;
}