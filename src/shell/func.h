// Function table: names to AST bodies, global like the job table.

#pragma once

#include "ast.h"

// Takes ownership of body and replaces any previous definition.
void func_define(const char *name, Node *body);

// Borrowed; NULL when name is not defined.
Node *func_lookup(const char *name);

void func_free_all(void);
