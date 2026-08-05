// Evaluator: walks the AST, runs pipelines through exec, owns control flow.

#define _POSIX_C_SOURCE 200809L

#include "eval.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exec.h"
#include "expand.h"
#include "func.h"
#include "signals.h"

#include "../alloc/alloc.h"
#include "../util/vec.h"

// 128 plus SIGINT: what a child killed by Ctrl-C reports.
#define STATUS_INTERRUPTED 130

// Deep enough for real recursion, shallow enough never to blow the C stack.
#define FUNC_DEPTH_MAX 64

// What a loop does once it has looked at sh->flow.
typedef enum { LOOP_RUN, LOOP_NEXT, LOOP_BROKE, LOOP_DONE } LoopAction;

static NshError eval_node(Shell *sh, Node *n);

static void say(const char *reason) {
    fprintf(stderr, "nullsh: %s\n", reason);
    fflush(stderr);
}

// Nothing further in the current list runs while one of these is pending.
static bool halted(const Shell *sh) {
    return sh->flow != FLOW_NONE || sh->want_exit;
}

static NshError eval_pipeline(Shell *sh, Node *n) {
    NshError err = exec_pipeline(sh, &n->u.pipeline.pl);
    if (err == NSH_OK && n->u.pipeline.negate) {
        sh->last_status = (sh->last_status == 0) ? 1 : 0;
    }
    return err;
}

static NshError eval_andor(Shell *sh, Node *n) {
    for (size_t i = 0; i < n->u.andor.items.len; i++) {
        const AndOrItem *it = vec_get(&n->u.andor.items, i);
        if (it == NULL) {
            continue;
        }
        if (it->op == ANDOR_AND && sh->last_status != 0) {
            continue;
        }
        if (it->op == ANDOR_OR && sh->last_status == 0) {
            continue;
        }
        NshError err = eval_node(sh, it->node);
        if (err != NSH_OK) {
            return err;
        }
        if (halted(sh)) {
            break;
        }
    }
    return NSH_OK;
}

static NshError eval_list(Shell *sh, Node *n) {
    for (size_t i = 0; i < n->u.list.nodes.len; i++) {
        if (halted(sh)) {
            break;
        }
        NshError err = eval_node(sh, vec_get(&n->u.list.nodes, i));
        if (err != NSH_OK) {
            return err;
        }
    }
    return NSH_OK;
}

static NshError eval_if(Shell *sh, Node *n) {
    for (size_t i = 0; i < n->u.nif.conds.len; i++) {
        NshError err = eval_node(sh, vec_get(&n->u.nif.conds, i));
        if (err != NSH_OK) {
            return err;
        }
        if (halted(sh)) {
            return NSH_OK;
        }
        if (sh->last_status == 0) {
            return eval_node(sh, vec_get(&n->u.nif.bodies, i));
        }
    }
    if (n->u.nif.else_body != NULL) {
        return eval_node(sh, n->u.nif.else_body);
    }
    sh->last_status = 0;
    return NSH_OK;
}

// Nested loops toggle the SIGINT watch once, at the outermost entry and exit.
static void loop_enter(Shell *sh) {
    sh->loop_depth++;
    if (sh->loop_depth == 1) {
        signals_int_watch(1);
    }
}

static void loop_leave(Shell *sh) {
    sh->loop_depth--;
    if (sh->loop_depth == 0) {
        signals_int_watch(0);
    }
}

// break and continue belong to the innermost loop; return and exit pass through.
static LoopAction loop_flow(Shell *sh) {
    if (sh->want_exit) {
        return LOOP_DONE;
    }
    if (sh->flow == FLOW_BREAK) {
        sh->flow = FLOW_NONE;
        sh->last_status = 0;
        return LOOP_BROKE;
    }
    if (sh->flow == FLOW_CONTINUE) {
        sh->flow = FLOW_NONE;
        return LOOP_NEXT;
    }
    if (sh->flow != FLOW_NONE) {
        return LOOP_DONE;
    }
    return LOOP_RUN;
}

static NshError eval_while(Shell *sh, Node *n) {
    NshError err = NSH_OK;
    int body_status = 0;
    // Set once the exit path has already decided what $? should be.
    bool settled = false;

    loop_enter(sh);
    for (;;) {
        if (signals_int_take() != 0) {
            sh->last_status = STATUS_INTERRUPTED;
            settled = true;
            break;
        }
        err = eval_node(sh, n->u.nwhile.cond);
        if (err != NSH_OK) {
            settled = true;
            break;
        }
        LoopAction act = loop_flow(sh);
        if (act == LOOP_NEXT) {
            continue;
        }
        if (act != LOOP_RUN || sh->last_status == STATUS_INTERRUPTED) {
            settled = true;
            break;
        }
        if (sh->last_status != 0) {
            break;
        }

        err = eval_node(sh, n->u.nwhile.body);
        if (err != NSH_OK) {
            settled = true;
            break;
        }
        body_status = sh->last_status;
        act = loop_flow(sh);
        if (act == LOOP_NEXT) {
            continue;
        }
        if (act != LOOP_RUN || body_status == STATUS_INTERRUPTED) {
            settled = true;
            break;
        }
    }
    if (!settled) {
        sh->last_status = body_status;
    }
    loop_leave(sh);
    return err;
}

static NshError eval_for(Shell *sh, Node *n) {
    NshError err = NSH_OK;
    int body_status = 0;
    bool settled = false;
    // The word list expands against the status the loop started with.
    ExpandCtx ctx = {sh->last_status, sh->argc, sh->argv};

    loop_enter(sh);
    for (size_t i = 0; i < n->u.nfor.words.len; i++) {
        if (signals_int_take() != 0) {
            sh->last_status = STATUS_INTERRUPTED;
            settled = true;
            break;
        }

        char *value = NULL;
        if (expand_word(vec_get(&n->u.nfor.words, i), &ctx, &value) != NSH_OK) {
            say("bad substitution");
            sh->last_status = 1;
            settled = true;
            break;
        }
        const char *var = (n->u.nfor.var != NULL) ? n->u.nfor.var : "";
        int rc = setenv(var, value, 1);
        int why = errno;
        nsh_free(value);
        if (rc != 0) {
            say(strerror(why));
            sh->last_status = 1;
            settled = true;
            break;
        }

        err = eval_node(sh, n->u.nfor.body);
        if (err != NSH_OK) {
            settled = true;
            break;
        }
        body_status = sh->last_status;
        LoopAction act = loop_flow(sh);
        if (act == LOOP_NEXT) {
            continue;
        }
        if (act != LOOP_RUN || body_status == STATUS_INTERRUPTED) {
            settled = true;
            break;
        }
    }
    if (!settled) {
        sh->last_status = body_status;
    }
    loop_leave(sh);
    return err;
}

// A funcdef inside a loop evaluates again every pass, so the table gets a copy.
static NshError eval_funcdef(Shell *sh, Node *n) {
    func_define(n->u.funcdef.name, ast_clone(n->u.funcdef.body));
    sh->last_status = 0;
    return NSH_OK;
}

static NshError eval_node(Shell *sh, Node *n) {
    if (n == NULL) {
        return NSH_ERR_INVALID;
    }
    switch (n->kind) {
    case NODE_PIPELINE:
        return eval_pipeline(sh, n);
    case NODE_ANDOR:
        return eval_andor(sh, n);
    case NODE_LIST:
        return eval_list(sh, n);
    case NODE_IF:
        return eval_if(sh, n);
    case NODE_WHILE:
        return eval_while(sh, n);
    case NODE_FOR:
        return eval_for(sh, n);
    case NODE_FUNCDEF:
        return eval_funcdef(sh, n);
    }
    return NSH_ERR_INVALID;
}

NshError eval_run(Shell *sh, Node *n) {
    if (sh == NULL) {
        return NSH_ERR_INVALID;
    }
    if (n == NULL) {
        return NSH_OK;
    }
    return eval_node(sh, n);
}

// $0 never changes across a call, so it comes from the caller's own argv.
static char **call_argv_new(const Shell *sh, int argc, char **argv) {
    char **out = nsh_calloc((size_t)argc + 1, sizeof(*out));
    out[0] = (sh->argc > 0 && sh->argv != NULL) ? sh->argv[0] : argv[0];
    for (int i = 1; i < argc; i++) {
        out[i] = argv[i];
    }
    return out;
}

// break or continue that reached the top of a function body is a stray.
static void drain_stray_flow(Shell *sh) {
    if (sh->flow != FLOW_BREAK && sh->flow != FLOW_CONTINUE) {
        return;
    }
    fprintf(stderr, "nullsh: %s: only meaningful in a loop\n",
            (sh->flow == FLOW_BREAK) ? "break" : "continue");
    fflush(stderr);
    sh->flow = FLOW_NONE;
}

bool eval_maybe_call_function(Shell *sh, int argc, char **argv, int *status) {
    if (sh == NULL || argv == NULL || argc < 1 || status == NULL) {
        return false;
    }
    Node *body = func_lookup(argv[0]);
    if (body == NULL) {
        return false;
    }
    if (sh->func_depth >= FUNC_DEPTH_MAX) {
        say("function recursion too deep");
        *status = 1;
        return true;
    }

    char **call_argv = call_argv_new(sh, argc, argv);
    int saved_argc = sh->argc;
    char **saved_argv = sh->argv;
    sh->argc = argc;
    sh->argv = call_argv;
    sh->func_depth++;

    eval_run(sh, body);

    sh->func_depth--;
    sh->argc = saved_argc;
    sh->argv = saved_argv;
    // The strings belong to the caller; only the borrowed array is ours.
    nsh_free(call_argv);

    *status = sh->last_status;
    if (sh->flow == FLOW_RETURN) {
        sh->flow = FLOW_NONE;
        *status = sh->flow_status;
    } else {
        drain_stray_flow(sh);
    }
    return true;
}
