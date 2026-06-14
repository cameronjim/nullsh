// Parser: recursive descent over the token list, building the AST.

#include "parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "ast.h"

#include "../alloc/alloc.h"
#include "../util/vec.h"

#define NELEMS(a) (sizeof(a) / sizeof(*(a)))

// Keywords that close a body, so a list stops when one is in command position.
static const char *const KW_END[] = {"then", "elif", "else", "fi",
                                     "do",   "done", "}"};
// Keywords that open a compound, and so can never open a simple command.
static const char *const KW_OPEN[] = {"if", "while", "for", "{"};

static void token_free_item(void *p) {
    token_free(p);
}

static Command *cmd_new(void) {
    Command *c = nsh_calloc(1, sizeof(*c));
    vec_init(&c->words);
    return c;
}

static void cmd_free(Command *c) {
    if (c == NULL) {
        return;
    }
    vec_free_deep(&c->words, token_free_item);
    token_free(c->redir_in);
    token_free(c->redir_out);
    token_free(c->redir_err);
    nsh_free(c);
}

static void cmd_free_item(void *p) {
    cmd_free(p);
}

void pipeline_free(Pipeline *p) {
    if (p == NULL) {
        return;
    }
    vec_free_deep(&p->cmds, cmd_free_item);
    p->background = false;
}

// Owns every token; a consumed slot is NULLed so the final sweep skips it.
typedef struct {
    Vec toks;
    size_t pos;
} Parser;

static bool at_end(const Parser *p) {
    return p->pos >= p->toks.len;
}

static Token *pk(const Parser *p) {
    return at_end(p) ? NULL : p->toks.items[p->pos];
}

static bool at_kind(const Parser *p, TokenKind k) {
    const Token *t = pk(p);
    return t != NULL && t->kind == k;
}

static Token *take(Parser *p) {
    Token *t = p->toks.items[p->pos];
    p->toks.items[p->pos] = NULL;
    p->pos++;
    return t;
}

static void drop(Parser *p) {
    token_free(take(p));
}

// A keyword is one bare segment; the token model cannot see the quoting.
static bool tok_is(const Token *t, const char *kw) {
    if (t == NULL || t->kind != TOK_WORD || t->segs.len != 1) {
        return false;
    }
    const WordSeg *s = t->segs.items[0];
    return strcmp(s->text, kw) == 0;
}

static bool kw_at(const Parser *p, const char *kw) {
    return tok_is(pk(p), kw);
}

static bool in_set(const Token *t, const char *const *set, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (tok_is(t, set[i])) {
            return true;
        }
    }
    return false;
}

static bool at_ender(const Parser *p) {
    return in_set(pk(p), KW_END, NELEMS(KW_END));
}

static bool is_reserved(const Token *t) {
    return in_set(t, KW_END, NELEMS(KW_END)) ||
           in_set(t, KW_OPEN, NELEMS(KW_OPEN));
}

// NULL unless the token is one bare segment matching [A-Za-z_][A-Za-z0-9_]*.
static char *name_dup(const Token *t) {
    if (t == NULL || t->kind != TOK_WORD || t->segs.len != 1) {
        return NULL;
    }
    const char *s = ((const WordSeg *)t->segs.items[0])->text;
    if (s[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; s[i] != '\0'; i++) {
        bool ok = (s[i] >= 'A' && s[i] <= 'Z') ||
                  (s[i] >= 'a' && s[i] <= 'z') || s[i] == '_' ||
                  (i > 0 && s[i] >= '0' && s[i] <= '9');
        if (!ok) {
            return NULL;
        }
    }
    size_t n = strlen(s) + 1;
    char *out = nsh_malloc(n);
    memcpy(out, s, n);
    return out;
}

static bool at_sep(const Parser *p) {
    return at_kind(p, TOK_SEMI) || at_kind(p, TOK_NEWLINE);
}

static void skip_seps(Parser *p) {
    while (at_sep(p)) {
        drop(p);
    }
}

static void skip_nl(Parser *p) {
    while (at_kind(p, TOK_NEWLINE)) {
        drop(p);
    }
}

// Hands the finished node over, or tears it down on any error.
static NshError finish(Node *n, Node **out, NshError err) {
    if (err != NSH_OK) {
        ast_free(n);
        return err;
    }
    *out = n;
    return NSH_OK;
}

// Takes ownership of target either way; > and >> share the redir_out slot.
static NshError cmd_set_redir(Command *c, TokenKind kind, Token *target) {
    Token **slot = kind == TOK_REDIR_IN    ? &c->redir_in
                   : kind == TOK_REDIR_ERR ? &c->redir_err
                                           : &c->redir_out;
    if (*slot != NULL) {
        token_free(target);
        return NSH_ERR_SYNTAX;
    }
    *slot = target;
    if (slot == &c->redir_out) {
        c->redir_append = (kind == TOK_REDIR_APPEND);
    }
    return NSH_OK;
}

// Redirects alone are not a command, so an empty argv is the error.
static NshError parse_simple(Parser *p, Pipeline *pl) {
    Command *c = cmd_new();
    NshError err = NSH_OK;
    while (err == NSH_OK && !at_end(p)) {
        Token *t = pk(p);
        if (t->kind == TOK_WORD) {
            if (c->words.len == 0 && is_reserved(t)) {
                err = NSH_ERR_SYNTAX;
                break;
            }
            vec_push(&c->words, take(p));
            continue;
        }
        TokenKind rk = t->kind;
        if (rk != TOK_REDIR_IN && rk != TOK_REDIR_OUT &&
            rk != TOK_REDIR_APPEND && rk != TOK_REDIR_ERR) {
            break;
        }
        drop(p);
        if (at_end(p)) {
            err = NSH_ERR_INCOMPLETE;
        } else if (!at_kind(p, TOK_WORD)) {
            err = NSH_ERR_SYNTAX;
        } else {
            err = cmd_set_redir(c, rk, take(p));
        }
    }
    if (err == NSH_OK && c->words.len == 0) {
        err = NSH_ERR_SYNTAX;
    }
    if (err != NSH_OK) {
        cmd_free(c);
        return err;
    }
    vec_push(&pl->cmds, c);
    return NSH_OK;
}

static NshError parse_pipeline(Parser *p, Node **out) {
    Node *n = ast_node_new(NODE_PIPELINE);
    NshError err = NSH_OK;
    for (;;) {
        err = parse_simple(p, &n->u.pipeline.pl);
        if (err != NSH_OK || !at_kind(p, TOK_PIPE)) {
            break;
        }
        drop(p);
        skip_nl(p);
        if (at_end(p)) {
            err = NSH_ERR_INCOMPLETE;
            break;
        }
    }
    return finish(n, out, err);
}

// A lone pipeline stays a bare NODE_PIPELINE; two or more become NODE_ANDOR.
static NshError parse_andor(Parser *p, Node **out) {
    Node *n = ast_node_new(NODE_ANDOR);
    Vec *items = &n->u.andor.items;
    AndOrOp op = ANDOR_FIRST;
    NshError err = NSH_OK;
    for (;;) {
        bool neg = kw_at(p, "!");
        if (neg) {
            drop(p);
            err = at_end(p) ? NSH_ERR_INCOMPLETE
                            : kw_at(p, "!") ? NSH_ERR_SYNTAX : NSH_OK;
            if (err != NSH_OK) {
                break;
            }
        }
        Node *sub = NULL;
        err = parse_pipeline(p, &sub);
        if (err != NSH_OK) {
            break;
        }
        sub->u.pipeline.negate = neg;
        AndOrItem *it = nsh_malloc(sizeof(*it));
        it->op = op;
        it->node = sub;
        vec_push(items, it);
        if (!at_kind(p, TOK_AND_IF) && !at_kind(p, TOK_OR_IF)) {
            break;
        }
        op = at_kind(p, TOK_AND_IF) ? ANDOR_AND : ANDOR_OR;
        drop(p);
        skip_nl(p);
        if (at_end(p)) {
            err = NSH_ERR_INCOMPLETE;
            break;
        }
    }
    if (err == NSH_OK && items->len == 1) {
        AndOrItem *it = items->items[0];
        Node *only = it->node;
        it->node = NULL;
        ast_free(n);
        *out = only;
        return NSH_OK;
    }
    return finish(n, out, err);
}

static NshError parse_list(Parser *p, Node **out);

// Bodies must be non-empty; running out of tokens instead means unfinished.
static NshError parse_body(Parser *p, Node **out) {
    Node *n = NULL;
    NshError err = parse_list(p, &n);
    if (err == NSH_OK && n == NULL) {
        err = at_end(p) ? NSH_ERR_INCOMPLETE : NSH_ERR_SYNTAX;
    }
    if (err != NSH_OK) {
        return err;
    }
    *out = n;
    return NSH_OK;
}

static NshError expect_kw(Parser *p, const char *kw) {
    if (!kw_at(p, kw)) {
        return at_end(p) ? NSH_ERR_INCOMPLETE : NSH_ERR_SYNTAX;
    }
    drop(p);
    return NSH_OK;
}

static NshError then_kw(NshError err, Parser *p, const char *kw) {
    return err != NSH_OK ? err : expect_kw(p, kw);
}

static NshError then_body(NshError err, Parser *p, Node **out) {
    return err != NSH_OK ? err : parse_body(p, out);
}

static NshError parse_if(Parser *p, Node **out) {
    drop(p);
    Node *n = ast_node_new(NODE_IF);
    NshError err = NSH_OK;
    for (;;) {
        Node *part = NULL;
        err = parse_body(p, &part);
        if (err != NSH_OK) {
            break;
        }
        vec_push(&n->u.nif.conds, part);
        part = NULL;
        err = then_body(expect_kw(p, "then"), p, &part);
        if (err != NSH_OK) {
            break;
        }
        vec_push(&n->u.nif.bodies, part);
        if (!kw_at(p, "elif")) {
            break;
        }
        drop(p);
    }
    if (err == NSH_OK && kw_at(p, "else")) {
        drop(p);
        err = parse_body(p, &n->u.nif.else_body);
    }
    return finish(n, out, then_kw(err, p, "fi"));
}

static NshError parse_while(Parser *p, Node **out) {
    drop(p);
    Node *n = ast_node_new(NODE_WHILE);
    NshError err = parse_body(p, &n->u.nwhile.cond);
    err = then_kw(err, p, "do");
    err = then_body(err, p, &n->u.nwhile.body);
    return finish(n, out, then_kw(err, p, "done"));
}

static NshError parse_for(Parser *p, Node **out) {
    drop(p);
    char *var = name_dup(pk(p));
    if (var == NULL) {
        return at_end(p) ? NSH_ERR_INCOMPLETE : NSH_ERR_SYNTAX;
    }
    drop(p);
    Node *n = ast_node_new(NODE_FOR);
    n->u.nfor.var = var;
    skip_nl(p);
    NshError err = expect_kw(p, "in");
    while (err == NSH_OK && at_kind(p, TOK_WORD)) {
        vec_push(&n->u.nfor.words, take(p));
    }
    if (err == NSH_OK) {
        err = at_end(p) ? NSH_ERR_INCOMPLETE
                        : at_sep(p) ? NSH_OK : NSH_ERR_SYNTAX;
    }
    if (err == NSH_OK) {
        drop(p);
        skip_nl(p);
    }
    err = then_kw(err, p, "do");
    err = then_body(err, p, &n->u.nfor.body);
    return finish(n, out, then_kw(err, p, "done"));
}

// Entered on NAME '('; the caller already checked for the paren.
static NshError parse_funcdef(Parser *p, Node **out) {
    char *name = name_dup(pk(p));
    if (name == NULL) {
        return NSH_ERR_SYNTAX;
    }
    drop(p);
    drop(p);
    Node *n = ast_node_new(NODE_FUNCDEF);
    n->u.funcdef.name = name;
    NshError err = at_end(p) ? NSH_ERR_INCOMPLETE
                   : at_kind(p, TOK_RPAREN) ? NSH_OK : NSH_ERR_SYNTAX;
    if (err == NSH_OK) {
        drop(p);
        skip_nl(p);
    }
    err = then_kw(err, p, "{");
    err = then_body(err, p, &n->u.funcdef.body);
    return finish(n, out, then_kw(err, p, "}"));
}

// Compounds stand alone: no pipes, no redirects, no && chaining onto them.
static NshError parse_item(Parser *p, Node **out) {
    if (kw_at(p, "if")) {
        return parse_if(p, out);
    }
    if (kw_at(p, "while")) {
        return parse_while(p, out);
    }
    if (kw_at(p, "for")) {
        return parse_for(p, out);
    }
    const Token *nx = p->pos + 1 < p->toks.len ? p->toks.items[p->pos + 1]
                                               : NULL;
    if (at_kind(p, TOK_WORD) && nx != NULL && nx->kind == TOK_LPAREN) {
        return parse_funcdef(p, out);
    }
    return parse_andor(p, out);
}

static NshError parse_list(Parser *p, Node **out) {
    Node *n = ast_node_new(NODE_LIST);
    Vec *nodes = &n->u.list.nodes;
    NshError err = NSH_OK;
    for (;;) {
        skip_seps(p);
        if (at_end(p) || at_ender(p)) {
            break;
        }
        Node *item = NULL;
        err = parse_item(p, &item);
        if (err != NSH_OK) {
            break;
        }
        vec_push(nodes, item);
        bool amp = at_kind(p, TOK_AMP);
        if (amp) {
            if (item->kind != NODE_PIPELINE) {
                err = NSH_ERR_SYNTAX;
                break;
            }
            item->u.pipeline.pl.background = true;
            drop(p);
        } else if (!at_end(p) && !at_ender(p) && !at_sep(p)) {
            err = NSH_ERR_SYNTAX;
            break;
        }
    }
    if (err == NSH_OK && nodes->len < 2) {
        Node *only = nodes->len == 1 ? nodes->items[0] : NULL;
        nodes->len = 0;
        ast_free(n);
        *out = only;
        return NSH_OK;
    }
    return finish(n, out, err);
}

NshError parser_parse_program(TokenList *tl, Node **out) {
    // Owning the tokens up front leaves tl valid and empty on every path.
    Parser p;
    p.toks = tl->tokens;
    p.pos = 0;
    vec_init(&tl->tokens);
    Node *n = NULL;
    NshError err = parse_list(&p, &n);
    if (err == NSH_OK && !at_end(&p)) {
        // A closing keyword with nothing to close.
        err = NSH_ERR_SYNTAX;
    }
    if (err != NSH_OK) {
        ast_free(n);
        n = NULL;
    }
    *out = n;
    vec_free_deep(&p.toks, token_free_item);
    return err;
}

NshError parser_parse(TokenList *tl, Pipeline *out) {
    Node *n = NULL;
    NshError err = parser_parse_program(tl, &n);
    bool ok = err == NSH_OK &&
              (n == NULL ||
               (n->kind == NODE_PIPELINE && !n->u.pipeline.negate));
    pipeline_free(out);
    if (ok && n != NULL) {
        // Moved out of the tree, so ast_free must not see the commands again.
        *out = n->u.pipeline.pl;
        vec_init(&n->u.pipeline.pl.cmds);
    } else {
        vec_init(&out->cmds);
    }
    ast_free(n);
    return ok ? NSH_OK : NSH_ERR_SYNTAX;
}
