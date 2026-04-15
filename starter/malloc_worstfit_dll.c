/*
 * malloc_worstfit_dll.c — Worst-Fit Doubly-Linked-List Allocator (per-thread heap)
 *
 * Design overview:
 *   Shares the per-thread heap and DLL structure with the best-fit variant.
 *   The only algorithmic difference is the free-block selection policy:
 *
 *   Fit strategy — WORST FIT:
 *     Scan the ENTIRE free list and select the block with the LARGEST size
 *     that still satisfies the request.
 *     Rationale: allocating from the biggest available block leaves the
 *                largest possible remainder after splitting, giving future
 *                allocation requests more room and (in theory) reducing the
 *                chance of producing unusably tiny fragments.
 *     Disadvantage: quickly destroys large free blocks, which hurts
 *                   workloads that periodically need large allocations.
 *                   In practice, worst-fit often underperforms both first-fit
 *                   and best-fit on real workloads.
 *
 *   Comparison summary:
 *     First-fit  — fast (early exit), moderate fragmentation
 *     Best-fit   — slow (full scan), minimal internal fragmentation, small slivers
 *     Worst-fit  — slow (full scan), largest remainders, poor large-alloc locality
 *
 *   List structure — DOUBLY LINKED (same rationale as best-fit DLL):
 *     'prev' pointer enables O(1) link repair during splits and coalescing.
 */

#define _DEFAULT_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Round size up to next 8-byte boundary to guarantee alignment. */
#define ALIGN8(x) (((x) + 7U) & ~7U)

/* Minimum leftover payload that justifies introducing a new block header. */
#define MIN_SPLIT_REMAINDER 16U

/*
 * block_meta_t — doubly-linked intrusive header.
 *   prev : pointer to the preceding block; enables O(1) backward link repair.
 */
typedef struct block_meta {
    size_t size;
    struct block_meta *next;
    struct block_meta *prev;
    int free;
} block_meta_t;

/* Per-thread heap state — completely independent between threads. */
static __thread block_meta_t *thread_base       = NULL;
static __thread void         *thread_heap_start = NULL;

/* Protects only the process-wide sbrk() syscall. */
static pthread_mutex_t sbrk_lock = PTHREAD_MUTEX_INITIALIZER;

/* Recover block header: user pointer = block + 1, so subtract 1 to get header. */
static block_meta_t *data_to_block(void *ptr) { return (block_meta_t *)ptr - 1; }

/*
 * are_adjacent — true when b's header starts exactly where a's payload ends.
 * Guards coalescing from merging non-contiguous blocks.
 */
static int are_adjacent(block_meta_t *a, block_meta_t *b) {
    return (char *)(a + 1) + a->size == (char *)b;
}

/*
 * request_space — extend the heap via sbrk() and initialise a DLL node.
 *
 * Sets block->prev = last to maintain the doubly-linked invariant for the
 * new tail node.  The sbrk_lock is released before header initialisation
 * since the newly allocated memory region belongs exclusively to this thread.
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
    block->prev = last;    /* DLL: new tail knows its predecessor */
    block->free = 0;
    if (last) last->next = block;
    if (!thread_heap_start) thread_heap_start = brk_before;
    return block;
}

/*
 * find_free_block_worst_fit — O(n) full-list scan for the LARGEST fitting block.
 *
 * Key difference vs best-fit: comparison is '>' (want biggest) not '<' (want smallest).
 * Like best-fit, there is no early exit — the entire list must be scanned to
 * guarantee we find the true maximum.  Both are O(n); worst-fit just picks
 * the opposite extreme of the size spectrum.
 *
 * Theoretical goal: after splitting the largest block, the remainder is also
 * large, keeping the free list composed of larger fragments rather than tiny ones.
 * In practice, this destroys large contiguous regions rapidly.
 */
static block_meta_t *find_free_block_worst_fit(size_t size) {
    block_meta_t *worst = NULL;
    for (block_meta_t *cur = thread_base; cur; cur = cur->next) {
        if (cur->free && cur->size >= size) {
            /* Keep this block only if it is larger than our current candidate. */
            if (!worst || cur->size > worst->size) worst = cur;
        }
    }
    return worst;   /* NULL if no free block satisfies the request */
}

/*
 * split_block — carve out the requested region and recycle the excess.
 *
 * DLL invariant maintenance (same as best-fit DLL):
 *   new_block->prev = block           (backward link from new_block to block)
 *   block->next->prev = new_block     (backward link from successor to new_block)
 * These two assignments keep every node's 'prev' pointer accurate.
 *
 *  Before:  [block]--next-->[successor]
 *           [block]<--prev--[successor]
 *  After:   [block]--next-->[new_block]--next-->[successor]
 *           [block]<--prev--[new_block]<--prev--[successor]
 */
static void split_block(block_meta_t *block, size_t wanted) {
    if (!block || block->size <= wanted + sizeof(block_meta_t) + MIN_SPLIT_REMAINDER) return;

    block_meta_t *new_block = (block_meta_t *)((char *)(block + 1) + wanted);
    new_block->size = block->size - wanted - sizeof(block_meta_t);
    new_block->next = block->next;
    new_block->prev = block;                         /* DLL backward link */
    new_block->free = 1;
    if (block->next) block->next->prev = new_block;  /* DLL: repair successor's prev */
    block->size = wanted;
    block->next = new_block;
}

/*
 * coalesce_free_blocks — merge physically adjacent free blocks.
 *
 * Worst-fit tends to leave more large-ish free fragments than best-fit,
 * so coalescing may be less critical, but it is still essential to recover
 * space when adjacent free blocks accumulate.
 *
 * DLL coalescing: when 'n' (cur->next) is absorbed into 'cur', we set
 * cur->next->prev = cur to keep the backward link from n's successor correct.
 * Without this, the backward link would dangle to the now-consumed 'n'.
 */
static void coalesce_free_blocks(void) {
    block_meta_t *cur = thread_base;
    while (cur && cur->next) {
        if (cur->free && cur->next->free && are_adjacent(cur, cur->next)) {
            block_meta_t *n = cur->next;
            cur->size += sizeof(block_meta_t) + n->size;
            cur->next  = n->next;
            if (cur->next) cur->next->prev = cur;   /* DLL: repair backward link */
            continue;   /* re-check: merged block may be adjacent to new next */
        }
        cur = cur->next;
    }
}

/*
 * wf_malloc — worst-fit allocation entry point.
 *
 * Finds the largest available free block, marks it allocated, and splits
 * off the remainder.  The large remainder is the key behavioural distinction
 * from best-fit, which would have selected the smallest fitting block instead.
 */
void *wf_malloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN8(size);

    block_meta_t *block;
    if (!thread_base) {
        block = request_space(NULL, size);
        if (!block) return NULL;
        thread_base = block;
    } else {
        block = find_free_block_worst_fit(size);   /* full scan for largest block */
        if (block) {
            block->free = 0;
            split_block(block, size);   /* surplus becomes a new free block */
        } else {
            /* No free block large enough — grow the heap. */
            block_meta_t *last = thread_base;
            while (last->next) last = last->next;
            block = request_space(last, size);
            if (!block) return NULL;
        }
    }

    return (void *)(block + 1);
}

/*
 * wf_free — release a block and merge free neighbours.
 * assert(block->free == 0) catches use-after-free and double-free bugs.
 */
void wf_free(void *ptr) {
    if (!ptr) return;
    block_meta_t *block = data_to_block(ptr);
    assert(block->free == 0);
    block->free = 1;
    coalesce_free_blocks();
}

/* Overflow-checked zero-initialised allocation. */
void *wf_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0 || nmemb > SIZE_MAX / size) return NULL;
    void *ptr = wf_malloc(nmemb * size);
    if (ptr) memset(ptr, 0, nmemb * size);
    return ptr;
}

/*
 * wf_realloc — resize allocation in place when possible, otherwise relocate.
 *
 * In-place fast path avoids unnecessary data copying:
 *   if block is already large enough, just split off excess.
 * Relocation path:
 *   allocate new block, copy only the old payload (block->size bytes),
 *   free the old block.
 */
void *wf_realloc(void *ptr, size_t size) {
    if (!ptr) return wf_malloc(size);
    if (size == 0) {
        wf_free(ptr);
        return NULL;
    }
    size = ALIGN8(size);
    block_meta_t *block = data_to_block(ptr);
    if (block->size >= size) {
        split_block(block, size);
        return ptr;
    }
    void *new_ptr = wf_malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block->size);   /* copy old payload only */
    wf_free(ptr);
    return new_ptr;
}

/* ── Diagnostic helpers ─────────────────────────────────────────────────── */

/* Sum of payload bytes in blocks still marked allocated. */
static size_t leaked_bytes(void) {
    size_t leaked = 0;
    for (block_meta_t *b = thread_base; b; b = b->next) if (!b->free) leaked += b->size;
    return leaked;
}

/* Address one byte past the end of the last block (used to compute heap size). */
static void *heap_end(void) {
    block_meta_t *last = thread_base;
    if (!last) return NULL;
    while (last->next) last = last->next;
    return (void *)((char *)(last + 1) + last->size);
}

/* ── Workload & threading harness ───────────────────────────────────────── */

/*
 * run_workload — allocates, callocs, reallocs, and frees a variety of sizes.
 * 'seed' produces distinct allocation patterns per thread to avoid identical
 * heap states across threads (which would make concurrency bugs harder to spot).
 */
static void run_workload(int seed) {
    void *m[10] = {0};
    void *c[10] = {0};
    for (int i = 0; i < 10; ++i) m[i] = wf_malloc((size_t)(30 + seed + i * 7));
    for (int i = 0; i < 100; ++i) c[i] = wf_calloc((size_t)(2 + i), sizeof(int));
    for (int i = 0; i < 1000; ++i) m[i] = wf_realloc(m[i], (size_t)(70 + seed + i * 12));
    for (int i = 0; i < 10; ++i) {
        wf_free(m[i]);
        wf_free(c[i]);
    }
}

typedef struct { int id; } thread_arg_t;
static void *thread_worker(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    run_workload(10 * t->id);
    printf("[worst-fit/dll] thread %d heap start: %p, heap end: %p, leaked bytes: %zu\n",
           t->id, thread_heap_start, heap_end(), leaked_bytes());
    return NULL;
}

int main(void) {
    run_workload(0);
    printf("[worst-fit/dll] main heap start: %p, heap end: %p, leaked bytes: %zu\n",
           thread_heap_start, heap_end(), leaked_bytes());

    /* Spawn two worker threads — each gets its own thread-local heap. */
    pthread_t t1, t2;
    thread_arg_t a1 = {1}, a2 = {2};
    pthread_create(&t1, NULL, thread_worker, &a1);
    pthread_create(&t2, NULL, thread_worker, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}