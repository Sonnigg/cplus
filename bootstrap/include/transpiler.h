#ifndef TRANSPILER_H
#define TRANSPILER_H

#include "common.h"
#include "lexer.h"
#include "symbols.h"

typedef struct {
    char *name, *type_qualified;
    int pointer_depth, depth;
} Local;

typedef struct {
    Local *items;
    size_t count, cap;
} LocalTable;

typedef struct {
    Buffer output;
} DeferScope;

typedef struct {
    DeferScope *items;
    size_t count, cap;
} DeferStack;

typedef struct {
    const char *source;
    TokenList tokens;
    SymbolRegistry symbols;
    Buffer output;
    LocalTable locals;
    int local_depth;
    const char *current_struct, *current_method;
    bool current_method_static;
    size_t next_temporary;
    DeferStack defers;
} Transpiler;

char *preprocess_source(const char *path, const char *src, int depth);
void transpile(Transpiler *t);

void locals_init(LocalTable *v);
void locals_free(LocalTable *v);
#endif