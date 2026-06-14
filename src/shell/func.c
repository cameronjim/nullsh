// Contracts-commit stub; the eval agent replaces this file.

#include "func.h"

#include <stddef.h>

void func_define(const char *name, Node *body) {
    (void)name;
    ast_free(body);
}

Node *func_lookup(const char *name) {
    (void)name;
    return NULL;
}

void func_free_all(void) {
}
