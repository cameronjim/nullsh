// Fixed-capacity ring of command lines, with load and save.

#pragma once

#include <stddef.h>

#include "../util/error.h"

typedef struct {
    char **lines;   // ring storage, entries are nsh-allocated copies
    size_t cap;     // fixed at init
    size_t len;     // number of live entries, <= cap
    size_t start;   // index of oldest entry in the ring
} History;

// NSH_ERR_INVALID when cap is 0.
NshError history_init(History *h, size_t cap);

// Safe on a zeroed History and safe to call twice.
void history_free(History *h);

// NULL, empty and a repeat of the newest entry are ignored. A full ring evicts.
void history_add(History *h, const char *line);

size_t history_count(const History *h);

// i == 0 is the oldest live entry. NULL once i reaches the count.
const char *history_get(const History *h, size_t i);

// A missing file is NSH_OK; one that exists but cannot be read is NSH_ERR_IO.
NshError history_load(History *h, const char *path);

// Writes every entry oldest first, one per line.
NshError history_save(const History *h, const char *path);
