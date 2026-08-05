// AST for the interpreter: what the parser builds and the evaluator walks.

#pragma once

#include <stdbool.h>

#include "parser.h"
#include "token.h"

#include "../util/vec.h"

typedef enum {
    NODE_PIPELINE,
    NODE_ANDOR,
    NODE_LIST,
    NODE_IF,
    NODE_WHILE,
    NODE_FOR,
    NODE_FUNCDEF
} NodeKind;

// How an item chains to the one before it; items[0] is always ANDOR_FIRST.
typedef enum { ANDOR_FIRST, ANDOR_AND, ANDOR_OR } AndOrOp;

typedef struct {
    AndOrOp op;
    Node *node;  // always NODE_PIPELINE
} AndOrItem;

struct Node {
    NodeKind kind;
    union {
        struct { Pipeline pl; bool negate; } pipeline;
        struct { Vec items; } andor;                             // AndOrItem*
        struct { Vec nodes; } list;                              // Node*, in order
        struct { Vec conds; Vec bodies; Node *else_body; } nif;  // Node*, parallel
        struct { Node *cond; Node *body; } nwhile;
        struct { char *var; Vec words; Node *body; } nfor;       // words: Token*
        struct { char *name; Node *body; } funcdef;
    } u;
};

// kind is set and every vector is initialised; everything else is zero.
Node *ast_node_new(NodeKind kind);

// Recursive, safe on NULL.
void ast_free(Node *n);

// Deep copy: the function table keeps bodies alive past their parse tree.
Node *ast_clone(const Node *n);
