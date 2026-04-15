#define _DEFAULT_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef struct block_meta {
    size_t size;
    struct block_meta *next;
    int free;
    int magic;
} block_meta_t;

#define META_SIZE sizeof(block_meta_t)
#define MIN_SPLIT_REMAINDER 8

static block_meta_t *global_base = NULL;
static void *heap_start = NULL;
static pthread_mutex_t alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static block_meta_t *find_free_block(block_meta_t **last, size_t size) {
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
    void *current_break = sbrk(0);
    void *request = sbrk((intptr_t)(META_SIZE + size));
    if (request == (void *)-1) {
        return NULL;
    }

    block_meta_t *block = (block_meta_t *)current_break;
    block->size = size;
    block->next = NULL;
    block->free = 0;
    block->magic = 0x12345678;

    if (!heap_start) {
        heap_start = current_break;
    }
    if (last) {
        last->next = block;
    }
    return block;
}

static block_meta_t *get_block_ptr(void *ptr) {
    return (block_meta_t *)ptr - 1;
}

static void split_block(block_meta_t *block, size_t wanted) {
    if (block->size <= wanted + META_SIZE + MIN_SPLIT_REMAINDER) {
        return;
    }

    block_meta_t *new_block = (block_meta_t *)((char *)(block + 1) + wanted);
    new_block->size = block->size - wanted - META_SIZE;
    new_block->next = block->next;
    new_block->free = 1;
    new_block->magic = 0x33333333;

    block->size = wanted;
    block->next = new_block;
}

static int are_adjacent(block_meta_t *a, block_meta_t *b) {
    return (char *)(a + 1) + a->size == (char *)b;
}

static void coalesce_all(void) {
    block_meta_t *current = global_base;
    while (current && current->next) {
        if (current->free && current->next->free && are_adjacent(current, current->next)) {
            current->size += META_SIZE + current->next->size;
            current->next = current->next->next;
            continue;
        }
        current = current->next;
    }
}

void *my_malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    pthread_mutex_lock(&alloc_lock);

    block_meta_t *block;
    if (!global_base) {
        block = request_space(NULL, size);
        if (!block) {
            pthread_mutex_unlock(&alloc_lock);
            return NULL;
        }
        global_base = block;
    } else {
        block_meta_t *last = global_base;
        block = find_free_block(&last, size);
        if (!block) {
            block = request_space(last, size);
            if (!block) {
                pthread_mutex_unlock(&alloc_lock);
                return NULL;
            }
        } else {
            block->free = 0;
            block->magic = 0x77777777;
            split_block(block, size);
        }
    }

    pthread_mutex_unlock(&alloc_lock);
    return (void *)(block + 1);
}

void my_free(void *ptr) {
    if (!ptr) {
        return;
    }

    pthread_mutex_lock(&alloc_lock);

    block_meta_t *block_ptr = get_block_ptr(ptr);
    assert(block_ptr->free == 0);
    block_ptr->free = 1;
    block_ptr->magic = 0x55555555;
    coalesce_all();

    pthread_mutex_unlock(&alloc_lock);
}

void *my_calloc(size_t nelem, size_t elsize) {
    if (nelem == 0 || elsize == 0) {
        return NULL;
    }
    if (nelem > SIZE_MAX / elsize) {
        return NULL;
    }

    size_t size = nelem * elsize;
    void *ptr = my_malloc(size);
    if (!ptr) {
        return NULL;
    }
    memset(ptr, 0, size);
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

    block_meta_t *block_ptr = get_block_ptr(ptr);
    if (block_ptr->size >= size) {
        split_block(block_ptr, size);
        return ptr;
    }

    void *new_ptr = my_malloc(size);
    if (!new_ptr) {
        return NULL;
    }

    memcpy(new_ptr, ptr, block_ptr->size);
    my_free(ptr);
    return new_ptr;
}

void *allocator_heap_start(void) {
    return heap_start;
}

void *allocator_heap_end(void) {
    return sbrk(0);
}

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

size_t allocator_block_count(void) {
    size_t count = 0;
    block_meta_t *current = global_base;
    while (current) {
        ++count;
        current = current->next;
    }
    return count;
}
