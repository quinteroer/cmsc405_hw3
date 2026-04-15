#define _DEFAULT_SOURCE
/*
 * CMSC 405 Assignment 3 Starter
 * Variant: Singly Linked List + First Fit
 *
 * TODOs are marked with "TODO(A3)".
 */

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALIGN4(x) (((((x)-1U) >> 2U) << 2U) + 4U)
#define BLOCK_DATA(b) ((void *)((char *)(b) + sizeof(block_meta_t)))
#define BLOCK_META(p) ((block_meta_t *)((char *)(p) - sizeof(block_meta_t)))

typedef struct block_meta {
    size_t size;
    struct block_meta *next;
    int free;
    int magic;
} block_meta_t;

static block_meta_t *global_base = NULL;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

static block_meta_t *find_free_block_first_fit(block_meta_t **last, size_t size) {
    block_meta_t *current = global_base;
    while (current) {
        if (current->free && current->size >= size) {
            return current;
        }
        *last = current;
        current = current->next;
    }
    return NULL;
}

static block_meta_t *request_space(block_meta_t *last, size_t size) {
    void *request = sbrk(0);
    void *extend = sbrk((intptr_t)(sizeof(block_meta_t) + size));
    if (extend == (void *)-1) {
        return NULL;
    }

    block_meta_t *block = (block_meta_t *)request;
    block->size = size;
    block->next = NULL;
    block->free = 0;
    block->magic = 0x12345678;

    if (last) {
        last->next = block;
    }
    return block;
}

static void split_block(block_meta_t *block, size_t wanted) {
    /* TODO(A3-4): implement block split per assignment requirement #4. */
    (void)block;
    (void)wanted;
}

static void coalesce_free_blocks(void) {
    /* TODO(A3-5): implement adjacent free block coalescing. */
}

void *my_malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    size = ALIGN4(size);
    pthread_mutex_lock(&heap_lock);

    block_meta_t *block;
    if (!global_base) {
        block = request_space(NULL, size);
        if (!block) {
            pthread_mutex_unlock(&heap_lock);
            return NULL;
        }
        global_base = block;
    } else {
        block_meta_t *last = global_base;
        block = find_free_block_first_fit(&last, size);
        if (!block) {
            block = request_space(last, size);
            if (!block) {
                pthread_mutex_unlock(&heap_lock);
                return NULL;
            }
        } else {
            block->free = 0;
            split_block(block, size);
        }
    }

    pthread_mutex_unlock(&heap_lock);
    return BLOCK_DATA(block);
}

void my_free(void *ptr) {
    if (!ptr) {
        return;
    }

    pthread_mutex_lock(&heap_lock);
    block_meta_t *block = BLOCK_META(ptr);
    assert(block->free == 0);
    block->free = 1;
    block->magic = 0x55555555;
    coalesce_free_blocks();
    pthread_mutex_unlock(&heap_lock);
}

void *my_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) {
        return NULL;
    }
    if (nmemb > SIZE_MAX / size) {
        return NULL;
    }

    size_t total = nmemb * size;
    void *ptr = my_malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *my_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return my_malloc(size);
    }
    if (size == 0) {
        my_free(ptr);
        return NULL;
    }

    block_meta_t *block = BLOCK_META(ptr);
    if (block->size >= size) {
        return ptr;
    }

    void *new_ptr = my_malloc(size);
    if (!new_ptr) {
        return NULL;
    }
    memcpy(new_ptr, ptr, block->size);
    my_free(ptr);
    return new_ptr;
}

static void print_heap_bounds(const char *tag) {
    printf("[%s] program break: %p\n", tag, sbrk(0));
}

static size_t compute_leaked_bytes(void) {
    size_t leaked = 0;
    for (block_meta_t *b = global_base; b; b = b->next) {
        if (!b->free) {
            leaked += b->size;
        }
    }
    return leaked;
}

int main(void) {
    print_heap_bounds("start");

    void *malloc_ptrs[10] = {0};
    void *calloc_ptrs[10] = {0};

    for (int i = 0; i < 10; ++i) {
        malloc_ptrs[i] = my_malloc((size_t)(16 + i * 8));
    }
    for (int i = 0; i < 10; ++i) {
        calloc_ptrs[i] = my_calloc((size_t)(4 + i), sizeof(int));
    }
    for (int i = 0; i < 10; ++i) {
        malloc_ptrs[i] = my_realloc(malloc_ptrs[i], (size_t)(64 + i * 16));
    }
    for (int i = 0; i < 10; ++i) {
        my_free(malloc_ptrs[i]);
        my_free(calloc_ptrs[i]);
    }

    print_heap_bounds("end");
    printf("Leak estimate (bytes still allocated): %zu\n", compute_leaked_bytes());
    return 0;
}
