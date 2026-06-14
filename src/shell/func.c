// Function table: names to AST bodies, global like the job table.

#include "func.h"

#include <stddef.h>
#include <string.h>

#include "../alloc/alloc.h"
#include "../util/vec.h"

typedef struct {
    char *name;
    Node *body;
} Func;

static Vec g_funcs;    // Func*, in definition order
static Vec g_retired;  // Node*, bodies a redefinition displaced

// strdup would use libc malloc, which is off limits outside src/alloc.
static char *dup_cstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *out = nsh_malloc(n);
    memcpy(out, s, n);
    return out;
}

static void free_func_item(void *p) {
    Func *f = p;
    nsh_free(f->name);
    ast_free(f->body);
    nsh_free(f);
}

static void free_node_item(void *p) {
    ast_free(p);
}

static Func *find(const char *name) {
    for (size_t i = 0; i < g_funcs.len; i++) {
        Func *f = g_funcs.items[i];
        if (strcmp(f->name, name) == 0) {
            return f;
        }
    }
    return NULL;
}

void func_define(const char *name, Node *body) {
    if (name == NULL) {
        ast_free(body);
        return;
    }
    Func *f = find(name);
    if (f != NULL) {
        // A function may redefine itself mid-call, so the old body has to
        // outlive the running eval; it goes for good in func_free_all.
        vec_push(&g_retired, f->body);
        f->body = body;
        return;
    }
    f = nsh_malloc(sizeof(*f));
    f->name = dup_cstr(name);
    f->body = body;
    vec_push(&g_funcs, f);
}

Node *func_lookup(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    Func *f = find(name);
    return (f != NULL) ? f->body : NULL;
}

void func_free_all(void) {
    vec_free_deep(&g_funcs, free_func_item);
    vec_free_deep(&g_retired, free_node_item);
}
