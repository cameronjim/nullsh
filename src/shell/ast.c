// AST constructors, recursive free and deep clone.

#include "ast.h"

#include <stddef.h>
#include <string.h>

#include "../alloc/alloc.h"

static char *dup_cstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *out = nsh_malloc(n);
    memcpy(out, s, n);
    return out;
}

static void free_node_item(void *p) {
    ast_free(p);
}

static void free_andor_item(void *p) {
    AndOrItem *it = p;
    ast_free(it->node);
    nsh_free(it);
}

static void free_token_item(void *p) {
    token_free(p);
}

Node *ast_node_new(NodeKind kind) {
    Node *n = nsh_calloc(1, sizeof(*n));
    n->kind = kind;
    switch (kind) {
    case NODE_PIPELINE:
        vec_init(&n->u.pipeline.pl.cmds);
        break;
    case NODE_ANDOR:
        vec_init(&n->u.andor.items);
        break;
    case NODE_LIST:
        vec_init(&n->u.list.nodes);
        break;
    case NODE_IF:
        vec_init(&n->u.nif.conds);
        vec_init(&n->u.nif.bodies);
        break;
    case NODE_FOR:
        vec_init(&n->u.nfor.words);
        break;
    case NODE_WHILE:
    case NODE_FUNCDEF:
        break;
    }
    return n;
}

void ast_free(Node *n) {
    if (n == NULL) {
        return;
    }
    switch (n->kind) {
    case NODE_PIPELINE:
        pipeline_free(&n->u.pipeline.pl);
        break;
    case NODE_ANDOR:
        vec_free_deep(&n->u.andor.items, free_andor_item);
        break;
    case NODE_LIST:
        vec_free_deep(&n->u.list.nodes, free_node_item);
        break;
    case NODE_IF:
        vec_free_deep(&n->u.nif.conds, free_node_item);
        vec_free_deep(&n->u.nif.bodies, free_node_item);
        ast_free(n->u.nif.else_body);
        break;
    case NODE_WHILE:
        ast_free(n->u.nwhile.cond);
        ast_free(n->u.nwhile.body);
        break;
    case NODE_FOR:
        nsh_free(n->u.nfor.var);
        vec_free_deep(&n->u.nfor.words, free_token_item);
        ast_free(n->u.nfor.body);
        break;
    case NODE_FUNCDEF:
        nsh_free(n->u.funcdef.name);
        ast_free(n->u.funcdef.body);
        break;
    }
    nsh_free(n);
}

static Token *token_clone(const Token *t) {
    Token *out = nsh_malloc(sizeof(*out));
    out->kind = t->kind;
    vec_init(&out->segs);
    for (size_t i = 0; i < t->segs.len; i++) {
        const WordSeg *s = t->segs.items[i];
        WordSeg *c = nsh_malloc(sizeof(*c));
        c->text = dup_cstr(s->text);
        c->expand = s->expand;
        vec_push(&out->segs, c);
    }
    return out;
}

static Command *cmd_clone(const Command *c) {
    Command *out = nsh_calloc(1, sizeof(*out));
    vec_init(&out->words);
    for (size_t i = 0; i < c->words.len; i++) {
        vec_push(&out->words, token_clone(c->words.items[i]));
    }
    if (c->redir_in != NULL) {
        out->redir_in = token_clone(c->redir_in);
    }
    if (c->redir_out != NULL) {
        out->redir_out = token_clone(c->redir_out);
    }
    if (c->redir_err != NULL) {
        out->redir_err = token_clone(c->redir_err);
    }
    out->redir_append = c->redir_append;
    return out;
}

static void pipeline_clone(const Pipeline *src, Pipeline *dst) {
    for (size_t i = 0; i < src->cmds.len; i++) {
        vec_push(&dst->cmds, cmd_clone(src->cmds.items[i]));
    }
    dst->background = src->background;
}

Node *ast_clone(const Node *n) {
    if (n == NULL) {
        return NULL;
    }
    Node *out = ast_node_new(n->kind);
    switch (n->kind) {
    case NODE_PIPELINE:
        pipeline_clone(&n->u.pipeline.pl, &out->u.pipeline.pl);
        out->u.pipeline.negate = n->u.pipeline.negate;
        break;
    case NODE_ANDOR:
        for (size_t i = 0; i < n->u.andor.items.len; i++) {
            const AndOrItem *it = n->u.andor.items.items[i];
            AndOrItem *c = nsh_malloc(sizeof(*c));
            c->op = it->op;
            c->node = ast_clone(it->node);
            vec_push(&out->u.andor.items, c);
        }
        break;
    case NODE_LIST:
        for (size_t i = 0; i < n->u.list.nodes.len; i++) {
            vec_push(&out->u.list.nodes, ast_clone(n->u.list.nodes.items[i]));
        }
        break;
    case NODE_IF:
        for (size_t i = 0; i < n->u.nif.conds.len; i++) {
            vec_push(&out->u.nif.conds, ast_clone(n->u.nif.conds.items[i]));
            vec_push(&out->u.nif.bodies, ast_clone(n->u.nif.bodies.items[i]));
        }
        out->u.nif.else_body = ast_clone(n->u.nif.else_body);
        break;
    case NODE_WHILE:
        out->u.nwhile.cond = ast_clone(n->u.nwhile.cond);
        out->u.nwhile.body = ast_clone(n->u.nwhile.body);
        break;
    case NODE_FOR:
        out->u.nfor.var = dup_cstr(n->u.nfor.var);
        for (size_t i = 0; i < n->u.nfor.words.len; i++) {
            vec_push(&out->u.nfor.words, token_clone(n->u.nfor.words.items[i]));
        }
        out->u.nfor.body = ast_clone(n->u.nfor.body);
        break;
    case NODE_FUNCDEF:
        out->u.funcdef.name = dup_cstr(n->u.funcdef.name);
        out->u.funcdef.body = ast_clone(n->u.funcdef.body);
        break;
    }
    return out;
}
