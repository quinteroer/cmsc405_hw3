/*
 * malloc_firstfit_sll.c — First-Fit Singly-Linked-List Allocator (per-thread heap)
 *
 * Design overview:
 *   Uses __thread (thread-local storage) so every thread maintains its OWN
 *   heap and free list.  This eliminates contention on the free list itself —
 *   each thread's list is private and requires no locking.  The only shared
 *   resource is sbrk(), which is protected by a narrow sbrk_lock held only
 *   during heap extension.
 *
 *   Fit strategy — FIRST FIT:
 *     Scan the list from the head and pick the FIRST block whose size >= request.
 *     Advantage : fast allocation (early termination on match).
 *     Disadvantage: tends to fragment the front of the list with small leftovers,
 *                   potentially forcing later large requests all the way to the tail.
 *
 *   List structure — SINGLY LINKED:
 *     Each header stores only a 'next' pointer.
 *     Coalescing must walk forward from the head (O(n) per free).
 *     Simpler and smaller than a doubly-linked list, but O(n) removal.
 */

#define _DEFAULT_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * ALIGN8 — round a size up to the nearest multiple of 8.
 *
 * Most architectures require or strongly prefer naturally aligned accesses.
 * Returning mis-aligned pointers would cause bus errors on strict platforms
 * and silent performance penalties everywhere else.
 * The bitmask trick works because 8 is a power of two:
 *   (x + 7) & ~7  clears the low three bits after adding 7, rounding up.
 */
#define ALIGN8(x) (((x) + 7U) & ~7U)

/*
 * MIN_SPLIT_REMAINDER — smallest payload (in bytes) that justifies a split.
 *
 * Splitting a block produces two headers.  If the leftover payload is smaller
 * than this threshold the overhead of an extra header outweighs the benefit,
 * so we skip the split and accept minor internal fragmentation instead.
 */
#define MIN_SPLIT_REMAINDER 16U

/*
 * block_meta_t — intrusive header stored immediately before each payload.
 *
 *   size : usable bytes available to caller (excludes sizeof(block_meta_t))
 *   next : next block in the list (NULL at tail)
 *   free : 0 = allocated, 1 = available for reuse
 *
 * Memory layout (SLL — no 'prev' pointer, saving 8 bytes per block vs DLL):
 *   [ block_meta_t | <---- size bytes of payload ---> ][ block_meta_t | ... ]
 */
typedef struct block_meta {
    size_t size;
    struct block_meta *next;
    int free;
} block_meta_t;

/*
 * Per-thread heap state.  __thread gives each OS thread its own copy of
 * these variables — reads/writes are completely independent, so no mutex
 * is needed to protect the list itself.
 */
static __thread block_meta_t *thread_base       = NULL;   /* head of this thread's list */
static __thread void         *thread_heap_start = NULL;   /* first sbrk address for this thread */

/*
 * sbrk_lock — protects the single global program-break pointer.
 *
 * sbrk() is a process-wide syscall; concurrent calls from two threads would
 * interleave their heap regions.  We hold this lock only for the brief sbrk
 * call, so contention is minimal.
 */
static pthread_mutex_t sbrk_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * data_to_block — recover the block header from a user-visible pointer.
 *
 * Callers receive (block + 1), the address immediately after the header.
 * Subtracting 1 (one block_meta_t width) reverses this to reach the header.
 */
static block_meta_t *data_to_block(void *ptr) { return (block_meta_t *)ptr - 1; }

/*
 * find_free_block_first_fit — O(n) scan returning the first suitable block.
 *
 * *last is advanced to the final visited node so the caller can cheaply
 * append a new block at the tail without a second traversal.
 *
 * First-fit characteristic: the returned block may be much larger than
 * 'size', introducing internal fragmentation (mitigated by split_block).
 */
static block_meta_t *find_free_block_first_fit(block_meta_t **last, size_t size) {
    block_meta_t *current = thread_base;
    while (current) {
        if (current->free && current->size >= size) return current;   /* first match wins */
        *last = current;
        current = current->next;
    }
    return NULL;
}

/*
 * request_space — extend this thread's heap with a new sbrk() allocation.
 *
 * Steps:
 *   1. Lock sbrk_lock to serialise process-wide break movement.
 *   2. Read the current break (will become our block header address).
 *   3. Move the break up by sizeof(block_meta_t) + size.
 *   4. Unlock and initialise the header at the old break address.
 *
 * The lock is released before initialising the header — that memory is
 * private to this thread now, so no other thread can touch it.
 */
static block_meta_t *request_space(block_meta_t *last, size_t size) {
    pthread_mutex_lock(&sbrk_lock);
    void *brk_before = sbrk(0);   /* snapshot: this will be our block's address */
    if (sbrk((intptr_t)(sizeof(block_meta_t) + size)) == (void *)-1) {
        pthread_mutex_unlock(&sbrk_lock);
        return NULL;   /* OS refused — out of virtual address space */
    }
    pthread_mutex_unlock(&sbrk_lock);

    block_meta_t *block = (block_meta_t *)brk_before;
    block->size = size;
    block->next = NULL;
    block->free = 0;

    /* Record the first address for this thread's heap diagnostics. */
    if (!thread_heap_start) thread_heap_start = brk_before;
    if (last) last->next = block;   /* link to the tail of the list */
    return block;
}

/*
 * are_adjacent — true when block b begins immediately after block a ends.
 *
 * Required for correct coalescing: only physically contiguous free blocks
 * can be merged into one.  A gap between them would mean there is another
 * block (possibly allocated) in between.
 */
static int are_adjacent(block_meta_t *a, block_meta_t *b) {
    return (char *)(a + 1) + a->size == (char *)b;
}

/*
 * split_block — carve off unused tail of an oversized free block.
 *
 * Internal fragmentation occurs when a block is larger than needed and the
 * excess is wasted.  Splitting recovers that excess as a separate free block.
 *
 *  Before split:  [ hdr | <---------- block->size ----------> ]
 *  After split:   [ hdr | <-- wanted --> ][ new_hdr | <-- remainder --> ]
 *
 * The new block is inserted between 'block' and block->next, preserving
 * list order (important for coalescing to work correctly in a single pass).
 */
static void split_block(block_meta_t *block, size_t wanted) {
    if (!block || block->size <= wanted + sizeof(block_meta_t) + MIN_SPLIT_REMAINDER) return;

    block_meta_t *new_block = (block_meta_t *)((char *)(block + 1) + wanted);
    new_block->size = block->size - wanted - sizeof(block_meta_t);
    new_block->next = block->next;
    new_block->free = 1;   /* remainder immediately returns to the free pool */

    block->size = wanted;
    block->next = new_block;
}

/*
 * coalesce_free_blocks — merge adjacent free blocks to reduce external fragmentation.
 *
 * External fragmentation: total free bytes are sufficient for a request, but
 * no single contiguous block is large enough.  Coalescing neighbours fixes this.
 *
 * Single-pass forward scan (SLL limitation): because we have no 'prev' pointer,
 * we can only merge forward (current absorbs current->next).  The 'continue'
 * without advancing 'cur' allows a chain of free blocks to collapse in one pass.
 */
static void coalesce_free_blocks(void) {
    block_meta_t *current = thread_base;
    while (current && current->next) {
        if (current->free && current->next->free && are_adjacent(current, current->next)) {
            /* Absorb next: add its header + payload into current's size. */
            current->size += sizeof(block_meta_t) + current->next->size;
            current->next  = current->next->next;
            continue;   /* don't advance — new next may also be free and adjacent */
        }
        current = current->next;
    }
}

/*
 * ff_malloc — first-fit allocator entry point.
 *
 * Aligns the request to 8 bytes, then either reuses a free block or extends
 * the heap.  Returns a pointer to the payload (just past the header).
 */
void *ff_malloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN8(size);   /* ensure 8-byte alignment for the returned pointer */

    block_meta_t *block;
    if (!thread_base) {
        /* First allocation on this thread — initialise the per-thread heap. */
        block = request_space(NULL, size);
        if (!block) return NULL;
        thread_base = block;
    } else {
        block_meta_t *last = thread_base;
        block = find_free_block_first_fit(&last, size);
        if (block) {
            block->free = 0;          /* claim the block */
            split_block(block, size); /* recycle surplus as a new free block */
        } else {
            /* No suitable free block — grow the heap. */
            block = request_space(last, size);
            if (!block) return NULL;
        }
    }
    return (void *)(block + 1);   /* return pointer to payload, skipping the header */
}

/*
 * ff_free — release a block back to the free pool and coalesce neighbours.
 *
 * The assert catches double-free errors: freeing an already-free block
 * corrupts the list and is undefined behaviour in C.
 */
void ff_free(void *ptr) {
    if (!ptr) return;
    block_meta_t *block = data_to_block(ptr);
    assert(block->free == 0);   /* debug guard: detect double-free */
    block->free = 1;
    coalesce_free_blocks();     /* eagerly merge to maximise future reuse */
}

/*
 * ff_calloc — allocate nelem * size zeroed bytes.
 *
 * Overflow check prevents the classic wrap-around vulnerability:
 * if nelem * size overflows size_t, we'd allocate too little memory.
 */
void *ff_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0 || nmemb > SIZE_MAX / size) return NULL;
    void *ptr = ff_malloc(nmemb * size);
    if (ptr) memset(ptr, 0, nmemb * size);   /* zero-fill (calloc guarantee) */
    return ptr;
}

/*
 * ff_realloc — resize an existing allocation.
 *
 * In-place fast path: if the current block is already large enough,
 * split off the excess and return the same pointer — no copy needed.
 * Otherwise allocate a new larger block, copy, and free the old one.
 */
void *ff_realloc(void *ptr, size_t size) {
    if (!ptr) return ff_malloc(size);
    if (size == 0) {
        ff_free(ptr);
        return NULL;
    }

    size = ALIGN8(size);
    block_meta_t *block = data_to_block(ptr);
    if (block->size >= size) {
        split_block(block, size);   /* shrink in-place, return excess to free list */
        return ptr;
    }

    /* Block too small — must relocate. */
    void *new_ptr = ff_malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block->size);   /* copy only the old payload */
    ff_free(ptr);
    return new_ptr;
}

/* ── Diagnostic helpers ─────────────────────────────────────────────────── */

/* Sum of payload bytes still allocated — non-zero after cleanup = leak. */
static size_t leaked_bytes(void) {
    size_t leaked = 0;
    for (block_meta_t *b = thread_base; b; b = b->next) {
        if (!b->free) leaked += b->size;
    }
    return leaked;
}

/*
 * heap_end — address of the first byte past the last block's payload.
 * (heap_end - thread_heap_start) gives total heap bytes consumed by this thread.
 */
static void *heap_end(void) {
    block_meta_t *last = thread_base;
    if (!last) return NULL;
    while (last->next) last = last->next;
    return (void *)((char *)(last + 1) + last->size);
}

/* ── Workload & threading harness ───────────────────────────────────────── */

/*
 * run_workload — exercises malloc, calloc, realloc, and free in sequence.
 *
 * 'seed' offsets sizes so different threads produce different allocation
 * patterns, stress-testing the allocator under varied conditions.
 */
static void run_workload(int seed) {
    void *m[10] = {0};
    void *c[10] = {0};
    /* Phase 1: allocate 10 differently-sized blocks */
    for (int i = 0; i < 10; ++i) m[i] = ff_malloc((size_t)(24 + seed + i * 8));
    /* Phase 2: allocate 10 zeroed blocks via calloc */
    for (int i = 0; i < 10; ++i) c[i] = ff_calloc((size_t)(3 + i), sizeof(int));
    /* Phase 3: grow each malloc'd block (exercises realloc copy path) */
    for (int i = 0; i < 10; ++i) m[i] = ff_realloc(m[i], (size_t)(96 + seed + i * 8));
    /* Phase 4: free everything — leaked_bytes() should return 0 afterward */
    for (int i = 0; i < 10; ++i) {
        ff_free(m[i]);
        ff_free(c[i]);
    }
}

typedef struct { int id; } thread_arg_t;

static void *thread_worker(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    run_workload(10 * t->id);   /* each thread uses a distinct seed */
    printf("[first-fit/sll] thread %d heap start: %p, heap end: %p, leaked bytes: %zu\n",
           t->id, thread_heap_start, heap_end(), leaked_bytes());
    return NULL;
}

int main(void) {
    run_workload(0);
    printf("[first-fit/sll] main heap start: %p, heap end: %p, leaked bytes: %zu\n",
           thread_heap_start, heap_end(), leaked_bytes());

    /* Spawn two worker threads — each gets its own thread-local heap. */
    pthread_t th1, th2;
    thread_arg_t a1 = {1}, a2 = {2};
    pthread_create(&th1, NULL, thread_worker, &a1);
    pthread_create(&th2, NULL, thread_worker, &a2);
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    return 0;
}