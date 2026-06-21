// File redirection: expands the target words and moves fds 0, 1 and 2.

#pragma once

#include "expand.h"
#include "parser.h"

#include "../util/error.h"

// dup copies of fds 0, 1 and 2, or -1 for a slot that was never saved.
typedef struct {
    int fds[3];
} RedirSave;

// Every slot -1, which redirect_restore treats as nothing to put back.
#define REDIR_SAVE_INIT {{-1, -1, -1}}

// A NULL save is child mode: apply and never come back. A non-NULL save is
// parent mode, and the caller must redirect_restore it even on failure.
// NSH_ERR_IO means an open or dup failed and was reported on stderr; any
// other error came from expanding a target word and said nothing.
NshError redirect_apply(const Command *c, const ExpandCtx *ctx,
                        RedirSave *save);

// Puts 0, 1 and 2 back and drops the copies. Safe on a zeroed init.
void redirect_restore(RedirSave *save);
