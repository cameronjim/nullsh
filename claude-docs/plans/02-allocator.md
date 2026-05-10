# Phase 2: The memory allocator

## Goal

Delete the libc wrapper in `src/alloc/alloc.c` and replace it with a real allocator: memory comes from the kernel via `mmap`, carved up by one of two switchable strategies (first-fit free list, buddy system), with a `heap` builtin that exposes the internals. Nothing outside `src/alloc/` changes; the whole shell becomes the allocator's stress test.

## Concepts this phase teaches

### malloc is a librarian, not a source

Userspace gets memory from the kernel in big pages, through `mmap` (or historically `sbrk`). An allocator's whole job is bookkeeping: hand out small pieces of a big region, remember what is free, and reuse it. nullsh grabs one large region per strategy up front (an arena) and never asks the kernel again.

### Block headers

The allocator must know, given only the pointer handed back to it in free(), how big the block is. The universal trick: store a small header immediately BEFORE the pointer the caller sees. `nsh_free(p)` steps back `sizeof(header)` bytes and reads the bookkeeping. Corrupt that header (heap overflow) and the allocator breaks, which is exactly why heap overflows are so destructive in real programs.

### Alignment

`nsh_malloc` must return pointers aligned for any type; 16 bytes on x86-64. Every size gets rounded up, every header is a multiple of 16. Misaligned pointers are undefined behavior that UBSan will catch.

### First-fit free list

Free blocks form a linked list threaded through the free memory itself. malloc walks the list, takes the first block big enough, splits off the remainder as a new free block. free() marks the block free and COALESCES: if the neighbor on either side is also free, merge them into one block. Skipping coalescing is the classic bug, and its symptom is external fragmentation: plenty of free bytes, but in pieces too small to use. Finding the previous neighbor requires boundary tags (a size footer at the end of each block) so free() can step backward.

### Buddy system

The arena is one power-of-2 region. Allocation rounds up to the next power of 2 (min 32 bytes) and repeatedly halves a bigger block until the size fits. The two halves of any split are "buddies", and their addresses differ in exactly one bit: `buddy = addr XOR size`. That makes merging trivial: on free, compute the buddy's address, and if it is also free and the same size, merge and repeat upward. Cost: internal fragmentation (a 33-byte request burns a 64-byte block). Benefit: allocation and merging are O(log n) with no list walking.

### Fragmentation, measured

`heap stats` reports both flavors: external (free bytes vs largest free block, first-fit's weakness) and the block counts that reveal internal waste (buddy's weakness). Watching the numbers change while using the shell is the point of the exercise.

### Poisoning and canaries

ASan cannot see inside our arena (it interposes on libc malloc, which we no longer use for shell allocations). So the allocator carries its own tripwires: freed memory is filled with a poison byte pattern so use-after-free reads garbage loudly, and each block header ends in a canary value checked on free so overflows are detected at the source. A corrupted canary aborts with a message, which is a bug found, not a crash.

### Strategy switching without moving memory

`heap strategy buddy` cannot move live blocks (the shell holds raw pointers). Design: each strategy owns its own arena. New allocations go to the active strategy; `nsh_free`/`nsh_realloc` route each pointer to whichever arena contains its address. Both arenas can be live at once and drain naturally.

## Structure

```
src/alloc/
  alloc.h        public API: nsh_* plus stats/strategy/dump (written by Fable)
  internal.h     Arena struct + strategy vtable contract (written by Fable)
  alloc.c        arena acquisition (mmap), dispatch, realloc, poisoning, counters
  firstfit.c     first-fit strategy behind the vtable
  buddy.c        buddy strategy behind the vtable
  heap_builtin.c the heap builtin: stats, strategy, dump
```

Arena size: 16 MiB per strategy (power of 2 for buddy), acquired lazily on first use with `mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)`. Arena exhaustion aborts, consistent with the abort-on-OOM policy. Strategy-private state lives inside the arena itself (strategies cannot call nsh_malloc; chicken and egg).

Contracts that must not change (alloc_test asserts them): abort on OOM rather than returning NULL, `nsh_malloc(0)` returns a unique writable block, free(NULL) is a no-op, realloc handles NULL/0/shrink/grow.

## Waves

- Wave A (parallel): comment sweep of the existing tree to the new minimal style; firstfit.c + its tests; buddy.c + its tests. Strategy agents build against internal.h using a caller-provided region, no mmap of their own, no changes to alloc.c.
- Wave B: alloc.c rewrite wiring arenas + dispatch + realloc + poison/canary + counters. Full suite must pass under BOTH strategies (env var NSH_ALLOC_STRATEGY selects the boot strategy for the test runs). Randomized soak test with a shadow table.
- Wave C: heap builtin, integration tests, docs update.

## The heap builtin

```
heap stats            strategy, arena size, used/free bytes, block counts, largest free, malloc/free counters
heap strategy         print the active strategy
heap strategy NAME    switch (firstfit | buddy)
heap dump             one line per block region, address-ordered free/used map
```

## Verification

- `make test` green under ASan/UBSan with NSH_ALLOC_STRATEGY=firstfit and again with buddy (Makefile runs both).
- Soak test: tens of thousands of randomized alloc/realloc/free ops mirrored in a shadow table, verifying content integrity and that free bytes return to baseline after freeing everything.
- Canary test: deliberately overflow a block in a child process, assert the abort fires.
- Manual: run the shell, use it hard (history, long lines, big pipelines later), watch `heap stats` move, switch strategies live, keep working.
