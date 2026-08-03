// Fixed-capacity ring of command lines with load/save persistence. Index 0 is
// always the oldest live entry, so callers walk history in the order it was
// typed without knowing where the ring wrapped.

#pragma once

#include <stddef.h>

#include "../util/error.h"

typedef struct {
    char **lines;   // ring storage, entries are nsh-allocated copies
    size_t cap;     // fixed at init
    size_t len;     // number of live entries, <= cap
    size_t start;   // index of oldest entry in the ring
} History;

// NSH_ERR_INVALID when cap is 0. On success every slot starts empty.
NshError history_init(History *h, size_t cap);

// Frees the entries and the array and leaves h empty, so a second call and a
// call on a zeroed History are both safe.
void history_free(History *h);

// Copies line into the ring. NULL, the empty string, and a line identical to
// the newest entry are ignored. A full ring drops its oldest entry.
void history_add(History *h, const char *line);

size_t history_count(const History *h);

// i == 0 is the oldest live entry. NULL once i reaches the count.
const char *history_get(const History *h, size_t i);

// Appends each line of the file through history_add. A missing file is NSH_OK
// because a fresh machine has no history yet. A file that exists but cannot be
// read is NSH_ERR_IO.
NshError history_load(History *h, const char *path);

// Writes every entry oldest first, one per line. NSH_ERR_IO on any failure.
NshError history_save(const History *h, const char *path);
