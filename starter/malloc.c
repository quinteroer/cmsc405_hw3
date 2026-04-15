/*
 * malloc.c — First-Fit Singly-Linked-List Allocator (global heap, mutex-protected)
 *
 * Design overview:
 *   The heap is managed as an intrusive singly-linked list of block_meta_t headers.
 *   Every allocation lives immediately after its header in memory:
 *
 *     [ block_meta_t | -------- user data -------- | block_meta_t | ... ]
 *
 *   The allocator grows the heap via sbrk(). Because sbrk() is not thread-safe,
 *   all operations are serialised with a single global mutex (alloc_lock).
 *   This is the simplest correct approach but is a throughput bottleneck under
 *   heavy concurrent load — the per-thread designs in the other files avoid this.
 */

#define _DEFAULT_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/*
 * block_meta_t — metadata stored just before every allocation.
 *
 *   size  : usable bytes available to the caller (does NOT include sizeof(block_meta_t))
 *   next  : pointer to the next block in the list; NULL for the tail
 *   free  : 1 if available for reuse, 0 if currently allocated
 *   magic : canary value used to catch corruption / double-free bugs during debugging
 */
typedef struct block_meta {
    size_t size;
    struct block_meta *next;
    int free;
    int magic;
} block_meta_t;

/* Bytes consumed by one metadata node — subtracted from every sbrk() request. */
#define META_SIZE sizeof(block_meta_t)

/*
 * Minimum leftover payload that makes splitting a block worthwhile.
 * Splitting a block smaller than this would waste space on an extra header
 * while providing negligibly small usable memory.
 */
#define MIN_SPLIT_REMAINDER 8

/* Head of the global free list; NULL until the first allocation. */
static block_meta_t *global_base = NULL;

/* Recorded once so callers can inspect total heap usage (heap_end - heap_start). */
static void *heap_start = NULL;

/*
 * Single global lock.
 * Ensures that concurrent malloc/free calls do not corrupt the linked list or
 * race on sbrk().  A finer-grained design would use per-size-class locks or
 * per-thread arenas (see the __thread variants in the other files).
 */
static pthread_mutex_t alloc_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * find_free_block — first-fit free list search.
 *
 * Walks the list from the head and returns the FIRST block whose size is
 * >= the requested size and is marked free.  First-fit is fast (O(n) but
 * returns early) and tends to keep large free blocks near the end of the heap,
 * though it can produce external fragmentation near the front over time.
 *
 * *last is updated to point at the final visited node so the caller can
 * append a new block without a second traversal.
 */
static block_meta_t *find_free_block(block_meta_t **last, size_t size) {
    block_meta_t *current = global_base;
    while (current) {
        if (current->free && current->size >= size) {
            return current;   /* first fit — stop as soon as we find a match */
        }
        *last = current;
        current = current->next;
    }
    return NULL;   /* no suitable block found; caller must extend the heap */
}

/*
 * request_space — extend the heap with sbrk() and initialise a new block.
 *
 * sbrk(0) returns the current program break (end of heap) without moving it.
 * sbrk(n) moves the break up by n bytes and returns the OLD break, which
 * becomes the address of our new block header.
 *
 * We request META_SIZE + size bytes so that the header fits before the
 * usable payload region.
 *
 * Returns NULL on failure (sbrk returns (void*)-1 when the OS refuses).
 */
static block_meta_t *request_space(block_meta_t *last, size_t size) {
    void *current_break = sbrk(0);                            /* snapshot break */
    void *request = sbrk((intptr_t)(META_SIZE + size));       /* extend heap    */
    if (request == (void *)-1) {
        return NULL;   /* OS denied the request (e.g., out of virtual memory) */
    }

    /* Initialise the header at the old break position. */
    block_meta_t *block = (block_meta_t *)current_break;
    block->size  = size;
    block->next  = NULL;
    block->free  = 0;               /* block is immediately in-use */
    block->magic = 0x12345678;      /* allocation sentinel */

    /* Record the very first heap address for diagnostic purposes. */
    if (!heap_start) {
        heap_start = current_break;
    }

    /* Link the new block onto the tail of the list. */
    if (last) {
        last->next = block;
    }
    return block;
}

/*
 * get_block_ptr — recover the block header from a user pointer.
 *
 * The user receives (block + 1), i.e. the address just past the header,
 * so subtracting 1 (one block_meta_t width) brings us back to the header.
 * This pointer arithmetic is the core of the intrusive header design.
 */
static block_meta_t *get_block_ptr(void *ptr) {
    return (block_meta_t *)ptr - 1;
}

/*
 * split_block — reduce internal fragmentation by dividing an oversized block.
 *
 * When a free block is much larger than the request we carve off a new free
 * block from the tail of the excess.  The guard condition ensures we only
 * split when the remainder can hold a full header PLUS at least
 * MIN_SPLIT_REMAINDER bytes of payload — otherwise the remnant block would
 * be too small to be useful and the overhead of an extra header wastes space.
 *
 *  Before:  [ header | <--------- size ---------> ]
 *  After:   [ header | <-- wanted --> ][ new hdr | <-- remainder --> ]
 */
static void split_block(block_meta_t *block, size_t wanted) {
    if (block->size <= wanted + META_SIZE + MIN_SPLIT_REMAINDER) {
        return;   /* not enough surplus to justify a split */
    }

    /* Place the new header immediately after the wanted payload region. */
    block_meta_t *new_block = (block_meta_t *)((char *)(block + 1) + wanted);
    new_block->size  = block->size - wanted - META_SIZE;
    new_block->next  = block->next;
    new_block->free  = 1;               /* the remainder goes back to the free pool */
    new_block->magic = 0x33333333;      /* split-block sentinel */

    /* Shrink the original block to the exact requested size. */
    block->size = wanted;
    block->next = new_block;
}

/*
 * are_adjacent — true when block b immediately follows block a in memory.
 *
 * This is required for safe coalescing: two free blocks should only be
 * merged if they are truly contiguous — there must be no gap between them.
 * (char *)(a + 1) points to a's payload start; adding a->size reaches the
 * end of a's payload, which must equal the start of b's header.
 */
static int are_adjacent(block_meta_t *a, block_meta_t *b) {
    return (char *)(a + 1) + a->size == (char *)b;
}

/*
 * coalesce_all — merge consecutive free blocks to combat external fragmentation.
 *
 * External fragmentation occurs when enough total free memory exists to satisfy
 * a request, but it is split into non-contiguous pieces too small to use.
 * Coalescing adjacent free blocks reconstructs larger usable regions.
 *
 * We restart the inner loop (via 'continue' without advancing 'current') after
 * each merge because the newly enlarged block may now also be adjacent to the
 * NEXT block, allowing further merging in a single pass.
 */
static void coalesce_all(void) {
    block_meta_t *current = global_base;
    while (current && current->next) {
        if (current->free && current->next->free && are_adjacent(current, current->next)) {
            /* Absorb the next block: reclaim its header + payload into current. */
            current->size += META_SIZE + current->next->size;
            current->next  = current->next->next;
            continue;   /* re-check: merged block might be adjacent to its new next */
        }
        current = current->next;
    }
}

/*
 * my_malloc — allocate 'size' bytes and return a pointer to the payload.
 *
 * Algorithm (first-fit):
 *   1. If the heap is empty, request the first block from the OS.
 *   2. Otherwise, search the free list for the first adequately-sized block.
 *   3. If found: mark it allocated, optionally split off surplus.
 *   4. If not found: extend the heap with sbrk().
 *
 * The returned pointer points PAST the header so the caller never sees the
 * metadata — they just get a plain memory region.
 */
void *my_malloc(size_t size) {
    if (size == 0) {
        return NULL;   /* standard: malloc(0) may return NULL */
    }

    pthread_mutex_lock(&alloc_lock);   /* serialise heap access */

    block_meta_t *block;
    if (!global_base) {
        /* First ever allocation — initialise the heap. */
        block = request_space(NULL, size);
        if (!block) {
            pthread_mutex_unlock(&alloc_lock);
            return NULL;
        }
        global_base = block;
    } else {
        block_meta_t *last = global_base;
        block = find_free_block(&last, size);   /* first-fit search */
        if (!block) {
            /* Free list exhausted — grow the heap. */
            block = request_space(last, size);
            if (!block) {
                pthread_mutex_unlock(&alloc_lock);
                return NULL;
            }
        } else {
            /* Reuse an existing free block. */
            block->free  = 0;
            block->magic = 0x77777777;   /* reuse sentinel */
            split_block(block, size);    /* trim excess to reduce internal fragmentation */
        }
    }

    pthread_mutex_unlock(&alloc_lock);
    return (void *)(block + 1);   /* return pointer past the header */
}

/*
 * my_free — mark a block as free and coalesce the heap.
 *
 * We deliberately call coalesce_all() on every free rather than deferring it,
 * which keeps fragmentation low at the cost of O(n) work per free.
 * The assert guards against double-free bugs during development.
 */
void my_free(void *ptr) {
    if (!ptr) {
        return;   /* free(NULL) is a no-op per the C standard */
    }

    pthread_mutex_lock(&alloc_lock);

    block_meta_t *block_ptr = get_block_ptr(ptr);
    assert(block_ptr->free == 0);   /* catch double-free / corruption */
    block_ptr->free  = 1;
    block_ptr->magic = 0x55555555;  /* freed sentinel */
    coalesce_all();                 /* eagerly merge adjacent free blocks */

    pthread_mutex_unlock(&alloc_lock);
}

/*
 * my_calloc — allocate and zero-initialise nelem * elsize bytes.
 *
 * The overflow check (nelem > SIZE_MAX / elsize) prevents a classic integer
 * overflow attack where a large product wraps around to a small size, causing
 * my_malloc to allocate too little memory while the caller writes past it.
 */
void *my_calloc(size_t nelem, size_t elsize) {
    if (nelem == 0 || elsize == 0) {
        return NULL;
    }
    if (nelem > SIZE_MAX / elsize) {
        return NULL;   /* multiplication would overflow size_t */
    }

    size_t size = nelem * elsize;
    void *ptr = my_malloc(size);
    if (!ptr) {
        return NULL;
    }
    memset(ptr, 0, size);   /* zero-fill the payload (calloc guarantee) */
    return ptr;
}

/*
 * my_realloc — resize an existing allocation.
 *
 * Handles four cases:
 *   ptr == NULL          → equivalent to malloc(size)
 *   size == 0            → equivalent to free(ptr), returns NULL
 *   block already fits   → try to split (shrink in-place), no copy needed
 *   block too small      → allocate new region, copy, free old region
 *
 * The in-place path avoids a malloc+memcpy+free cycle when the existing block
 * is large enough, which is a meaningful optimisation for repeated realloc growth.
 */
void *my_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return my_malloc(size);
    }
    if (size == 0) {
        my_free(ptr);
        return NULL;
    }

    block_meta_t *block_ptr = get_block_ptr(ptr);
    if (block_ptr->size >= size) {
        /* Block is already large enough — just split off any surplus. */
        split_block(block_ptr, size);
        return ptr;
    }

    /* Must allocate a larger region and migrate the data. */
    void *new_ptr = my_malloc(size);
    if (!new_ptr) {
        return NULL;   /* original ptr remains valid on failure (C standard) */
    }

    /* Copy only the old payload size — new region may be larger. */
    memcpy(new_ptr, ptr, block_ptr->size);
    my_free(ptr);
    return new_ptr;
}

/* ── Diagnostic helpers ─────────────────────────────────────────────────── */

/* Returns the address of the first byte ever handed to sbrk(). */
void *allocator_heap_start(void) {
    return heap_start;
}

/* Returns the current program break — the end of the managed heap region. */
void *allocator_heap_end(void) {
    return sbrk(0);
}

/*
 * allocator_leaked_bytes — sum of payload bytes in blocks still marked allocated.
 * A non-zero value after all frees indicates a memory leak.
 */
size_t allocator_leaked_bytes(void) {
    size_t leaked = 0;
    block_meta_t *current = global_base;
    while (current) {
        if (!current->free) {
            leaked += current->size;
        }
        current = current->next;
    }
    return leaked;
}

/*
 * allocator_block_count — total number of blocks (free + allocated) in the list.
 * Useful for observing how splitting and coalescing affect list length over time.
 */
size_t allocator_block_count(void) {
    size_t count = 0;
    block_meta_t *current = global_base;
    while (current) {
        ++count;
        current = current->next;
    }
    return count;
}