/*
 * wrapper.c — Integration test harness for the my_* allocator (malloc.c)
 *
 * Purpose:
 *   Exercises all four allocator entry points (my_malloc, my_calloc,
 *   my_realloc, my_free) in a realistic sequence and then queries the
 *   diagnostic helpers to verify heap correctness:
 *     • allocator_heap_start / allocator_heap_end  — heap address range
 *     • allocator_leaked_bytes                     — detects memory leaks
 *     • allocator_block_count                      — observes list length
 *
 *   This file is intentionally separate from malloc.c so the allocator
 *   implementation can be swapped without touching the test code
 *   (classic separation of interface from implementation).
 */

#include <stdio.h>
#include <stdlib.h>

/*
 * Forward declarations of the allocator API defined in malloc.c.
 * These match the signatures in malloc.c exactly; a mismatch would be
 * caught at link time (undefined symbol) or cause silent UB at runtime
 * (wrong calling convention / argument sizes).
 */
void *my_malloc(size_t size);
void  my_free(void *ptr);
void *my_calloc(size_t nelem, size_t elsize);
void *my_realloc(void *ptr, size_t size);

/* Diagnostic helpers also defined in malloc.c. */
void  *allocator_heap_start(void);
void  *allocator_heap_end(void);
size_t allocator_leaked_bytes(void);
size_t allocator_block_count(void);

int main(void) {
    /*
     * Pointer arrays initialised to NULL.
     * Initialising to 0/NULL is important: if an allocation fails and we
     * pass the NULL to my_free(), that is a safe no-op.  Passing an
     * uninitialised pointer would be undefined behaviour.
     */
    void *malloc_ptrs[12] = {0};
    void *calloc_ptrs[12] = {0};

    /*
     * Phase 1 — my_malloc
     *
     * Allocate 12 blocks of increasing sizes (24, 32, 40, … 112 bytes).
     * Varying sizes stress the free-list search and splitting logic.
     * After this phase the heap has 12 in-use blocks and no free blocks
     * (assuming this is the first workload on a fresh heap).
     */
    for (int i = 0; i < 12; ++i) {
        malloc_ptrs[i] = my_malloc((size_t)(24 + i * 8));
    }

    /*
     * Phase 2 — my_calloc
     *
     * Allocate 12 more blocks, this time zeroed.  calloc multiplies its
     * two arguments (nelem × elsize) and zero-fills the result.
     * The overflow check inside my_calloc prevents integer-overflow attacks.
     * After this phase the heap has 24 in-use blocks.
     */
    for (int i = 0; i < 12; ++i) {
        calloc_ptrs[i] = my_calloc((size_t)(i + 2), sizeof(int));
    }

    /*
     * Phase 3 — my_realloc (growth)
     *
     * Grow each malloc'd block to a larger size (64, 80, 96, … 240 bytes).
     * Because the new sizes exceed the original allocations, realloc must:
     *   1. Allocate a new larger block.
     *   2. Copy the old contents (memcpy).
     *   3. Free the old block.
     * The old pointer is replaced by the returned new pointer.
     * After this phase the heap has freed the original 12 malloc blocks
     * (now marked free, potentially coalesced) and allocated 12 new larger ones.
     */
    for (int i = 0; i < 12; ++i) {
        malloc_ptrs[i] = my_realloc(malloc_ptrs[i], (size_t)(64 + i * 16));
    }

    /*
     * Phase 4 — my_free (cleanup)
     *
     * Free all remaining live allocations.  After all frees, leaked_bytes()
     * should report 0.  The allocator calls coalesce_all() on each free,
     * so the free list should ideally collapse to a single large block
     * covering the entire heap.
     */
    for (int i = 0; i < 12; ++i) {
        my_free(malloc_ptrs[i]);
        my_free(calloc_ptrs[i]);
    }

    /*
     * Diagnostics:
     *   heap_start  — address where the first sbrk() call placed the heap.
     *   heap_end    — current program break (sbrk(0)); difference from start
     *                 gives total heap bytes ever requested from the OS.
     *   leaked_bytes— sum of payload bytes in blocks still marked in-use.
     *                 Non-zero here would indicate a bug in the test or allocator.
     *   block_count — total blocks (free + allocated) in the linked list.
     *                 After coalescing, this ideally approaches 1 (one big free block).
     */
    printf("Heap start: %p\n",   allocator_heap_start());
    printf("Heap end  : %p\n",   allocator_heap_end());
    printf("Leaked bytes: %zu\n", allocator_leaked_bytes());
    printf("Block count : %zu\n", allocator_block_count());

    return 0;
}