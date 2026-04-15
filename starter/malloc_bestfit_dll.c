/*
 * malloc_bestfit_dll.c — Best-Fit Doubly-Linked-List Allocator (per-thread heap)
 *
 * Design overview:
 *   Like the first-fit SLL variant, this allocator uses __thread storage for
 *   a fully per-thread heap.  The differences are:
 *
 *   Fit strategy — BEST FIT:
 *     Scan the ENTIRE free list and select the block whose size is the
 *     SMALLEST that still satisfies the request.
 *     Advantage : minimises wasted space inside each allocated block
 *                 (internal fragmentation), leaving larger blocks intact
 *                 for future big requests.
 *     Disadvantage: must scan the whole list every time — O(n) with no early
 *                   exit — and may leave tiny unusable slivers that are too
 *                   small to split, contributing to external fragmentation.
 *
 *   List structure — DOUBLY LINKED:
 *     Each header stores both 'next' and 'prev' pointers.
 *     This allows O(1) unlinking of a node, which would be useful for
 *     a segregated free list or boundary-tag coalescing design.
 *     In this implementation it is used to update 'prev' links during
 *     splits and coalescing, keeping the DLL invariants intact.
 *     Cost: 8 extra bytes per block (one pointer) compared to the SLL design.
 */

#define _DEFAULT_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Round up to the next multiple of 8 for alignment (see firstfit_sll for rationale). */
#define ALIGN8(x) (((x) + 7U) & ~7U)

/* Minimum worthwhile payload after a split — prevents degenerate tiny blocks. */
#define MIN_SPLIT_REMAINDER 16U

/*
 * block_meta_t — doubly-linked intrusive header.
 *
 *   prev : pointer to the preceding block (NULL at the head).
 *          Enables O(1) update of the previous block's 'next' pointer
 *          during coalescing and splitting without a separate backward scan.
 *
 * Memory layout:
 *   [ prev | next | size | free ]  ← block_meta_t (header)
 *   [ ---- user payload ----- ]
 *   [ prev | next | size | free ]  ← next block_meta_t
 *   ...
 */
typedef struct block_meta {
    size_t size;
    struct block_meta *next;
    struct block_meta *prev;   /* extra pointer vs SLL — enables O(1) backward link repair */
    int free;
} block_meta_t;

/* Per-thread heap state — each thread has independent copies of these. */
static __thread block_meta_t *thread_base       = NULL;
static __thread void         *thread_heap_start = NULL;

/* Narrow lock protecting only the sbrk() call; released immediately after. */
static pthread_mutex_t sbrk_lock = PTHREAD_MUTEX_INITIALIZER;

/* Recover the block header from a user pointer (payload starts at block+1). */
static block_meta_t *data_to_block(void *ptr) { return (block_meta_t *)ptr - 1; }

/*
 * are_adjacent — true when b begins at the exact byte after a's payload ends.
 * Safe coalescing requires this; a gap means another block lives between them.
 */
static int are_adjacent(block_meta_t *a, block_meta_t *b) {
    return (char *)(a + 1) + a->size == (char *)b;
}

/*
 * request_space — grow this thread's heap via sbrk() and initialise a new block.
 *
 * DLL addition: we set block->prev = last so the new tail knows its predecessor,
 * maintaining the bidirectional invariant from the moment of creation.
 */
static block_meta_t *request_space(block_meta_t *last, size_t size) {
    pthread_mutex_lock(&sbrk_lock);
    void *brk_before = sbrk(0);
    if (sbrk((intptr_t)(sizeof(block_meta_t) + size)) == (void *)-1) {
        pthread_mutex_unlock(&sbrk_lock);
        return NULL;
    }
    pthread_mutex_unlock(&sbrk_lock);

    block_meta_t *block = (block_meta_t *)brk_before;
    block->size = size;
    block->next = NULL;
    block->prev = last;   /* DLL: new tail points back to its predecessor */
    block->free = 0;
    if (last) last->next = block;   /* link old tail forward to the new block */
    if (!thread_heap_start) thread_heap_start = brk_before;
    return block;
}

/*
 * find_free_block_best_fit — O(n) full-list scan for the smallest fitting block.
 *
 * Unlike first-fit, there is NO early termination — we must check every free
 * block to guarantee we find the closest match.  The tradeoff:
 *   • Less internal fragmentation (tighter fit → less wasted space per block).
 *   • Slower allocation under a long free list.
 *   • May scatter tiny leftover slivers throughout the heap (external fragmentation).
 */
static block_meta_t *find_free_block_best_fit(size_t size) {
    block_meta_t *best = NULL;
    for (block_meta_t *cur = thread_base; cur; cur = cur->next) {
        if (cur->free && cur->size >= size) {
            /* Update best if this block is a closer fit than the current best. */
            if (!best || cur->size < best->size) best = cur;
        }
    }
    return best;   /* NULL if no free block is large enough */
}

/*
 * split_block — divide an oversized block to reduce internal fragmentation.
 *
 * DLL addition compared to SLL version:
 *   After inserting new_block between block and block->next, we must also
 *   update block->next->prev to point at new_block (if next exists).
 *   This keeps the backward links consistent for any future backward traversal
 *   or O(1) unlink operation.
 *
 *  Before:  [ block ]--next-->[  block->next  ]
 *  After:   [ block ]--next-->[ new_block ]--next-->[ block->next ]
 *                             [ new_block ]<--prev--[ block->next ]  (if exists)
 *           [ block ]<--prev--[ new_block ]
 */
static void split_block(block_meta_t *block, size_t wanted) {
    if (!block || block->size <= wanted + sizeof(block_meta_t) + MIN_SPLIT_REMAINDER) return;

    block_meta_t *new_block = (block_meta_t *)((char *)(block + 1) + wanted);
    new_block->size = block->size - wanted - sizeof(block_meta_t);
    new_block->next = block->next;
    new_block->prev = block;        /* DLL: new block's prev = original block */
    new_block->free = 1;
    if (block->next) block->next->prev = new_block;  /* DLL: fix successor's prev */
    block->next = new_block;
    block->size = wanted;
}

/*
 * coalesce_free_blocks — merge adjacent free blocks to fight external fragmentation.
 *
 * DLL advantage: when we remove 'n' (cur->next) by merging it into 'cur', we
 * must update n->next->prev to point at cur instead of n.  The 'prev' pointer
 * makes this O(1) without a separate backward scan — one of the main reasons to
 * pay the extra 8-byte cost of a doubly-linked list.
 */
static void coalesce_free_blocks(void) {
    block_meta_t *cur = thread_base;
    while (cur && cur->next) {
        if (cur->free && cur->next->free && are_adjacent(cur, cur->next)) {
            block_meta_t *n = cur->next;
            cur->size += sizeof(block_meta_t) + n->size;   /* absorb n's header+payload */
            cur->next  = n->next;
            if (cur->next) cur->next->prev = cur;  /* DLL: repair the successor's prev */
            continue;   /* retry: newly enlarged block might also be adjacent to next */
        }
        cur = cur->next;
    }
}

/*
 * bf_malloc — best-fit allocation entry point.
 *
 * After finding the best-fit free block, we mark it allocated and split off
 * any surplus so it is not wasted (split_block handles the MIN threshold check).
 */
void *bf_malloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN8(size);   /* align to prevent unaligned-access penalties */

    block_meta_t *block;
    if (!thread_base) {
        block = request_space(NULL, size);
        if (!block) return NULL;
        thread_base = block;
    } else {
        block = find_free_block_best_fit(size);   /* full scan for closest match */
        if (block) {
            block->free = 0;
            split_block(block, size);   /* recover excess as a new free block */
        } else {
            /* No suitable block exists — extend the heap. */
            block_meta_t *last = thread_base;
            while (last->next) last = last->next;   /* walk to tail */
            block = request_space(last, size);
            if (!block) return NULL;
        }
    }

    return (void *)(block + 1);   /* return pointer past the header */
}

/*
 * bf_free — return a block to the free pool, then coalesce.
 *
 * The assert is a development-time guard against double-free bugs.
 * In a production allocator this might be replaced with a less expensive check.
 */
void bf_free(void *ptr) {
    if (!ptr) return;
    block_meta_t *block = data_to_block(ptr);
    assert(block->free == 0);   /* double-free / use-after-free detection */
    block->free = 1;
    coalesce_free_blocks();
}

/*
 * bf_calloc — allocate nmemb * size zeroed bytes.
 * Overflow guard prevents the allocation of under-sized memory on wrap-around.
 */
void *bf_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0 || nmemb > SIZE_MAX / size) return NULL;
    void *ptr = bf_malloc(nmemb * size);
    if (ptr) memset(ptr, 0, nmemb * size);
    return ptr;
}

/*
 * bf_realloc — resize a block in place if possible, otherwise relocate.
 *
 * In-place path (block->size >= size): split off any surplus, return same ptr.
 *   Avoids an O(size) memcpy, which matters for large reallocations.
 * Relocation path: allocate fresh, copy old content, free old block.
 *   Only copies block->size bytes (old content), not the full new size.
 */
void *bf_realloc(void *ptr, size_t size) {
    if (!ptr) return bf_malloc(size);
    if (size == 0) {
        bf_free(ptr);
        return NULL;
    }
    size = ALIGN8(size);
    block_meta_t *block = data_to_block(ptr);
    if (block->size >= size) {
        split_block(block, size);
        return ptr;
    }

    void *new_ptr = bf_malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block->size);
    bf_free(ptr);
    return new_ptr;
}

/* ── Diagnostic helpers ─────────────────────────────────────────────────── */

static size_t leaked_bytes(void) {
    size_t leaked = 0;
    for (block_meta_t *b = thread_base; b; b = b->next) if (!b->free) leaked += b->size;
    return leaked;
}

static void *heap_end(void) {
    block_meta_t *last = thread_base;
    if (!last) return NULL;
    while (last->next) last = last->next;
    return (void *)((char *)(last + 1) + last->size);
}

/* ── Workload & threading harness ───────────────────────────────────────── */

static void run_workload(int seed) {
    void *m[10] = {0};
    void *c[10] = {0};
    for (int i = 0; i < 10; ++i) m[i] = bf_malloc((size_t)(20 + seed + i * 6));
    for (int i = 0; i < 10; ++i) c[i] = bf_calloc((size_t)(i + 1), sizeof(int));
    for (int i = 0; i < 10; ++i) m[i] = bf_realloc(m[i], (size_t)(72 + seed + i * 10));
    for (int i = 0; i < 10; ++i) {
        bf_free(m[i]);
        bf_free(c[i]);
    }
}

typedef struct { int id; } thread_arg_t;
static void *thread_worker(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    run_workload(10 * t->id);
    printf("[best-fit/dll] thread %d heap start: %p, heap end: %p, leaked bytes: %zu\n",
           t->id, thread_heap_start, heap_end(), leaked_bytes());
    return NULL;
}

int main(void) {
    run_workload(0);
    printf("[best-fit/dll] main heap start: %p, heap end: %p, leaked bytes: %zu\n",
           thread_heap_start, heap_end(), leaked_bytes());

    /* Each thread operates on its own heap — no list-level locking needed. */
    pthread_t t1, t2;
    thread_arg_t a1 = {1}, a2 = {2};
    pthread_create(&t1, NULL, thread_worker, &a1);
    pthread_create(&t2, NULL, thread_worker, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}