#include <stdio.h>
#include <stdlib.h>

void *my_malloc(size_t size);
void my_free(void *ptr);
void *my_calloc(size_t nelem, size_t elsize);
void *my_realloc(void *ptr, size_t size);

void *allocator_heap_start(void);
void *allocator_heap_end(void);
size_t allocator_leaked_bytes(void);
size_t allocator_block_count(void);

int main(void) {
    void *malloc_ptrs[12] = {0};
    void *calloc_ptrs[12] = {0};

    for (int i = 0; i < 12; ++i) {
        malloc_ptrs[i] = my_malloc((size_t)(24 + i * 8));
    }

    for (int i = 0; i < 12; ++i) {
        calloc_ptrs[i] = my_calloc((size_t)(i + 2), sizeof(int));
    }

    for (int i = 0; i < 12; ++i) {
        malloc_ptrs[i] = my_realloc(malloc_ptrs[i], (size_t)(64 + i * 16));
    }

    for (int i = 0; i < 12; ++i) {
        my_free(malloc_ptrs[i]);
        my_free(calloc_ptrs[i]);
    }

    printf("Heap start: %p\n", allocator_heap_start());
    printf("Heap end  : %p\n", allocator_heap_end());
    printf("Leaked bytes: %zu\n", allocator_leaked_bytes());
    printf("Block count : %zu\n", allocator_block_count());

    return 0;
}
