// Lexer: one left-to-right pass over the input buffer, splitting it to tokens.

#include "lexer.h"

#include <stdbool.h>
#include <stddef.h>

#include "../alloc/alloc.h"
#include "../util/str.h"
#include "../util/vec.h"

// A real index can never reach this: the line would have to fill memory.
#define SCAN_UNTERMINATED ((size_t)-1)

// A word is pending while either part is non-empty, which makes "" a word.
typedef struct {
    Vec segs;  // WordSeg*
    Str cur;
} WordBuf;

static void seg_free(void *p) {
    WordSeg *s = p;
    nsh_free(s->text);
    nsh_free(s);
}

static void token_free_item(void *p) {
    token_free(p);
}

void token_free(Token *t) {
    if (t == NULL) {
        return;
    }
    vec_free_deep(&t->segs, seg_free);
    nsh_free(t);
}

void token_list_free(TokenList *tl) {
    if (tl == NULL) {
        return;
    }
    vec_free_deep(&tl->tokens, token_free_item);
}

static void wordbuf_init(WordBuf *w) {
    vec_init(&w->segs);
    str_init(&w->cur);
}

static void wordbuf_free(WordBuf *w) {
    vec_free_deep(&w->segs, seg_free);
    str_free(&w->cur);
}

static bool wordbuf_pending(const WordBuf *w) {
    return w->segs.len > 0 || w->cur.len > 0;
}

// Takes ownership of text, which must come from the nsh_* allocator.
static void wordbuf_push_seg(WordBuf *w, char *text, bool expand) {
    WordSeg *s = nsh_malloc(sizeof(*s));
    s->text = text;
    s->expand = expand;
    vec_push(&w->segs, s);
}

// Empty bare text is not a segment; only quotes can introduce an empty one.
static void wordbuf_flush_bare(WordBuf *w) {
    if (w->cur.len > 0) {
        wordbuf_push_seg(w, str_take(&w->cur), true);
    }
}

// Moves the pending word into tl and leaves w ready for the next one.
static void wordbuf_finish(WordBuf *w, TokenList *tl) {
    if (!wordbuf_pending(w)) {
        return;
    }
    wordbuf_flush_bare(w);
    Token *t = nsh_malloc(sizeof(*t));
    t->kind = TOK_WORD;
    t->segs = w->segs;
    vec_push(&tl->tokens, t);
    vec_init(&w->segs);
}

static void push_op(TokenList *tl, TokenKind kind) {
    Token *t = nsh_malloc(sizeof(*t));
    t->kind = kind;
    vec_init(&t->segs);
    vec_push(&tl->tokens, t);
}

static bool is_blank(char c) {
    return c == ' ' || c == '\t';
}

static bool is_op_char(char c) {
    return c == '>' || c == '<' || c == '|' || c == '&' || c == ';' ||
           c == '(' || c == ')';
}

// Only for the single-character operators; op_kind2 gets first refusal.
static TokenKind op_kind(char c) {
    switch (c) {
    case '<':
        return TOK_REDIR_IN;
    case '|':
        return TOK_PIPE;
    case '&':
        return TOK_AMP;
    case ';':
        return TOK_SEMI;
    case '(':
        return TOK_LPAREN;
    case ')':
        return TOK_RPAREN;
    default:
        return TOK_REDIR_OUT;
    }
}

// Longest match first. TOK_WORD is the no-match answer: no operator is a word.
static TokenKind op_kind2(char a, char b) {
    if (a != b) {
        return TOK_WORD;
    }
    switch (a) {
    case '>':
        return TOK_REDIR_APPEND;
    case '&':
        return TOK_AND_IF;
    case '|':
        return TOK_OR_IF;
    default:
        return TOK_WORD;
    }
}

// Inside double quotes a backslash escapes only these four.
static bool dq_escapes(char c) {
    return c == '$' || c == '"' || c == '\\' || c == '`';
}

// i points at the opening quote. Nothing inside is special, not even a \.
static size_t scan_single(const char *line, size_t i, WordBuf *w) {
    Str q;
    str_init(&q);
    i++;
    while (line[i] != '\0' && line[i] != '\'') {
        str_push(&q, line[i]);
        i++;
    }
    if (line[i] != '\'') {
        str_free(&q);
        return SCAN_UNTERMINATED;
    }
    wordbuf_flush_bare(w);
    wordbuf_push_seg(w, str_take(&q), false);
    str_free(&q);
    return i + 1;
}

// i points at the opening quote.
static size_t scan_double(const char *line, size_t i, WordBuf *w) {
    Str q;
    str_init(&q);
    i++;
    while (line[i] != '\0' && line[i] != '"') {
        if (line[i] == '\\' && dq_escapes(line[i + 1])) {
            str_push(&q, line[i + 1]);
            i += 2;
        } else {
            str_push(&q, line[i]);
            i++;
        }
    }
    if (line[i] != '"') {
        str_free(&q);
        return SCAN_UNTERMINATED;
    }
    wordbuf_flush_bare(w);
    wordbuf_push_seg(w, str_take(&q), true);
    str_free(&q);
    return i + 1;
}

// At end of a line there is nothing to escape, so the backslash is literal.
// nullsh has no line continuation, so a newline counts as an end of line here.
static size_t scan_escape(const char *line, size_t i, WordBuf *w) {
    i++;
    if (line[i] == '\0' || line[i] == '\n') {
        str_push(&w->cur, '\\');
        return i;
    }
    str_push(&w->cur, line[i]);
    return i + 1;
}

NshError lexer_scan(const char *line, TokenList *out) {
    token_list_free(out);
    vec_init(&out->tokens);

    WordBuf w;
    wordbuf_init(&w);

    // Only at a word start can a 2 open a "2>" instead of being word text.
    bool at_word_start = true;
    size_t i = 0;

    while (line[i] != '\0') {
        char c = line[i];
        if (is_blank(c)) {
            wordbuf_finish(&w, out);
            at_word_start = true;
            i++;
            continue;
        }
        if (c == '\n') {
            wordbuf_finish(&w, out);
            push_op(out, TOK_NEWLINE);
            at_word_start = true;
            i++;
            continue;
        }
        // A comment stops at the newline, which still becomes its own token.
        if (at_word_start && c == '#') {
            while (line[i] != '\0' && line[i] != '\n') {
                i++;
            }
            continue;
        }
        if (at_word_start && c == '2' && line[i + 1] == '>') {
            push_op(out, TOK_REDIR_ERR);
            i += 2;
            continue;
        }
        if (is_op_char(c)) {
            wordbuf_finish(&w, out);
            TokenKind two = op_kind2(c, line[i + 1]);
            if (two != TOK_WORD) {
                push_op(out, two);
                i += 2;
            } else {
                push_op(out, op_kind(c));
                i++;
            }
            at_word_start = true;
            continue;
        }

        size_t next;
        if (c == '\'') {
            next = scan_single(line, i, &w);
        } else if (c == '"') {
            next = scan_double(line, i, &w);
        } else if (c == '\\') {
            next = scan_escape(line, i, &w);
        } else {
            str_push(&w.cur, c);
            next = i + 1;
        }
        if (next == SCAN_UNTERMINATED) {
            wordbuf_free(&w);
            token_list_free(out);
            vec_init(&out->tokens);
            return NSH_ERR_INCOMPLETE;
        }
        i = next;
        at_word_start = false;
    }

    wordbuf_finish(&w, out);
    wordbuf_free(&w);
    return NSH_OK;
}
