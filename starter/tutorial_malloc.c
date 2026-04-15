/*
 * Reference implementation based on Dan Luu tutorial:
 * https://github.com/danluu/malloc-tutorial/blob/master/malloc.c
 *
 * Kept as a baseline reference so you can compare behavior with starter/malloc.c.
 */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

struct block_meta {
    size_t size;
    struct block_meta *next;
    int free;
    int magic;
};

#define META_SIZE sizeof(struct block_meta)

static struct block_meta *global_base = NULL;

static struct block_meta *find_free_block(struct block_meta **last, size_t size) {
    struct block_meta *current = global_base;
    while (current && !(current->free && current->size >= size)) {
        *last = current;
        current = current->next;
    }
    return current;
}

static struct block_meta *request_space(struct block_meta *last, size_t size) {
    struct block_meta *block = sbrk(0);
    void *request = sbrk((intptr_t)(size + META_SIZE));
    if (request == (void *)-1) {
        return NULL;
    }
    assert((void *)block == request);

    if (last) {
        last->next = block;
    }

    block->size = size;
    block->next = NULL;
    block->free = 0;
    block->magic = 0x12345678;
    return block;
}

void *tutorial_malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    struct block_meta *block;
    if (!global_base) {
        block = request_space(NULL, size);
        if (!block) {
            return NULL;
        }
        global_base = block;
    } else {
        struct block_meta *last = global_base;
        block = find_free_block(&last, size);
        if (!block) {
            block = request_space(last, size);
            if (!block) {
                return NULL;
            }
        } else {
            block->free = 0;
            block->magic = 0x77777777;
        }
    }

    return block + 1;
}

static struct block_meta *get_block_ptr(void *ptr) {
    return (struct block_meta *)ptr - 1;
}

void tutorial_free(void *ptr) {
    if (!ptr) {
        return;
    }

    struct block_meta *block_ptr = get_block_ptr(ptr);
    assert(block_ptr->free == 0);
    block_ptr->free = 1;
    block_ptr->magic = 0x55555555;
}

void *tutorial_calloc(size_t nelem, size_t elsize) {
    if (nelem == 0 || elsize == 0) {
        return NULL;
    }
    if (nelem > SIZE_MAX / elsize) {
        return NULL;
    }

    size_t size = nelem * elsize;
    void *ptr = tutorial_malloc(size);
    if (!ptr) {
        return NULL;
    }
    memset(ptr, 0, size);
    return ptr;
}

void *tutorial_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return tutorial_malloc(size);
    }

    struct block_meta *block_ptr = get_block_ptr(ptr);
    if (block_ptr->size >= size) {
        return ptr;
    }

    void *new_ptr = tutorial_malloc(size);
    if (!new_ptr) {
        return NULL;
    }

    memcpy(new_ptr, ptr, block_ptr->size);
    tutorial_free(ptr);
    return new_ptr;
}
