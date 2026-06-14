// Parser: one left-to-right pass over the token list. A command accumulates
// words and redirects until a pipe closes it; the grammar is small enough that
// no lookahead beyond the word after a redirect operator is needed.

#include "parser.h"

#include <stdbool.h>
#include <stddef.h>

#include "../alloc/alloc.h"
#include "../util/vec.h"

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

static bool is_redir(TokenKind k) {
    return k == TOK_REDIR_IN || k == TOK_REDIR_OUT || k == TOK_REDIR_APPEND ||
           k == TOK_REDIR_ERR;
}

// Takes ownership of target either way: on a duplicate slot it is freed here,
// so the caller never has to clean up after a rejected redirect. > and >> share
// the redir_out slot, which is what makes "> a >> b" a duplicate.
static NshError cmd_set_redir(Command *c, TokenKind kind, Token *target) {
    Token **slot;
    if (kind == TOK_REDIR_IN) {
        slot = &c->redir_in;
    } else if (kind == TOK_REDIR_ERR) {
        slot = &c->redir_err;
    } else {
        slot = &c->redir_out;
    }
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

// A command is only real once it holds an argv word. Redirects alone are a
// syntax error, so this is the test both the pipe case and the tail use.
static bool cmd_has_argv(const Command *c) {
    return c != NULL && c->words.len > 0;
}

// The word after a redirect operator. Claims the slot in toks so the shared
// cleanup pass does not double free it. Returns NULL when the next token is
// missing or is not a word, which is the caller's syntax error.
static Token *take_redir_target(Vec *toks, size_t *i) {
    if (*i + 1 >= toks->len) {
        return NULL;
    }
    Token *target = toks->items[*i + 1];
    if (target->kind != TOK_WORD) {
        return NULL;
    }
    toks->items[*i + 1] = NULL;
    (*i)++;
    return target;
}

NshError parser_parse(TokenList *tl, Pipeline *out) {
    // Take the tokens away from tl up front. From here on this function owns
    // every one of them, and tl is already the valid empty list it has to be
    // on both the success and the error path.
    Vec toks = tl->tokens;
    vec_init(&tl->tokens);

    pipeline_free(out);
    vec_init(&out->cmds);

    NshError err = NSH_OK;
    Command *cur = NULL;
    bool background = false;

    for (size_t i = 0; i < toks.len; i++) {
        Token *t = toks.items[i];
        toks.items[i] = NULL;
        TokenKind kind = t->kind;

        if (kind == TOK_WORD) {
            if (cur == NULL) {
                cur = cmd_new();
            }
            vec_push(&cur->words, t);
            continue;
        }

        token_free(t);

        if (is_redir(kind)) {
            Token *target = take_redir_target(&toks, &i);
            if (target == NULL) {
                err = NSH_ERR_SYNTAX;
                break;
            }
            if (cur == NULL) {
                cur = cmd_new();
            }
            err = cmd_set_redir(cur, kind, target);
            if (err != NSH_OK) {
                break;
            }
        } else if (kind == TOK_PIPE) {
            if (!cmd_has_argv(cur)) {
                err = NSH_ERR_SYNTAX;
                break;
            }
            vec_push(&out->cmds, cur);
            cur = NULL;
        } else {
            // TOK_AMP. Legal only as the very last token of a real command.
            if (i + 1 != toks.len || !cmd_has_argv(cur)) {
                err = NSH_ERR_SYNTAX;
                break;
            }
            background = true;
        }
    }

    if (err == NSH_OK) {
        if (cur != NULL) {
            // A trailing command must have argv words, not just redirects.
            if (cmd_has_argv(cur)) {
                vec_push(&out->cmds, cur);
                cur = NULL;
            } else {
                err = NSH_ERR_SYNTAX;
            }
        } else if (out->cmds.len > 0) {
            // Nothing followed the last pipe.
            err = NSH_ERR_SYNTAX;
        }
    }

    if (err != NSH_OK) {
        cmd_free(cur);
        pipeline_free(out);
        vec_init(&out->cmds);
    } else {
        out->background = background;
    }

    vec_free_deep(&toks, token_free_item);
    return err;
}
