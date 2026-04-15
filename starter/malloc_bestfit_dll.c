#define _DEFAULT_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALIGN8(x) (((x) + 7U) & ~7U)
#define MIN_SPLIT_REMAINDER 16U

typedef struct block_meta {
    size_t size;
    struct block_meta *next;
    struct block_meta *prev;
    int free;
} block_meta_t;

static block_meta_t *global_base = NULL;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static void *heap_start = NULL;

static block_meta_t *data_to_block(void *ptr) { return (block_meta_t *)ptr - 1; }

static int are_adjacent(block_meta_t *a, block_meta_t *b) {
    return (char *)(a + 1) + a->size == (char *)b;
}

static block_meta_t *request_space(block_meta_t *last, size_t size) {
    void *brk_before = sbrk(0);
    if (sbrk((intptr_t)(sizeof(block_meta_t) + size)) == (void *)-1) {
        return NULL;
    }
    block_meta_t *block = (block_meta_t *)brk_before;
    block->size = size;
    block->next = NULL;
    block->prev = last;
    block->free = 0;
    if (last) {
        last->next = block;
    }
    if (!heap_start) {
        heap_start = brk_before;
    }
    return block;
}

static block_meta_t *find_free_block_best_fit(size_t size) {
    block_meta_t *best = NULL;
    for (block_meta_t *cur = global_base; cur; cur = cur->next) {
        if (cur->free && cur->size >= size) {
            if (!best || cur->size < best->size) {
                best = cur;
            }
        }
    }
    return best;
}

static void split_block(block_meta_t *block, size_t wanted) {
    if (!block || block->size <= wanted + sizeof(block_meta_t) + MIN_SPLIT_REMAINDER) {
        return;
    }
    block_meta_t *new_block = (block_meta_t *)((char *)(block + 1) + wanted);
    new_block->size = block->size - wanted - sizeof(block_meta_t);
    new_block->next = block->next;
    new_block->prev = block;
    new_block->free = 1;
    if (block->next) {
        block->next->prev = new_block;
    }
    block->next = new_block;
    block->size = wanted;
}

static void coalesce_free_blocks(void) {
    block_meta_t *cur = global_base;
    while (cur && cur->next) {
        if (cur->free && cur->next->free && are_adjacent(cur, cur->next)) {
            block_meta_t *n = cur->next;
            cur->size += sizeof(block_meta_t) + n->size;
            cur->next = n->next;
            if (cur->next) {
                cur->next->prev = cur;
            }
            continue;
        }
        cur = cur->next;
    }
}

void *bf_malloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN8(size);
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
        block = find_free_block_best_fit(size);
        if (block) {
            block->free = 0;
            split_block(block, size);
        } else {
            block_meta_t *last = global_base;
            while (last->next) last = last->next;
            block = request_space(last, size);
            if (!block) {
                pthread_mutex_unlock(&heap_lock);
                return NULL;
            }
        }
    }

    pthread_mutex_unlock(&heap_lock);
    return (void *)(block + 1);
}

void bf_free(void *ptr) {
    if (!ptr) return;
    pthread_mutex_lock(&heap_lock);
    block_meta_t *block = data_to_block(ptr);
    assert(block->free == 0);
    block->free = 1;
    coalesce_free_blocks();
    pthread_mutex_unlock(&heap_lock);
}

void *bf_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0 || nmemb > SIZE_MAX / size) return NULL;
    void *ptr = bf_malloc(nmemb * size);
    if (ptr) memset(ptr, 0, nmemb * size);
    return ptr;
}

void *bf_realloc(void *ptr, size_t size) {
    if (!ptr) return bf_malloc(size);
    if (size == 0) {
        bf_free(ptr);
        return NULL;
    }
    size = ALIGN8(size);
    block_meta_t *block = data_to_block(ptr);
    if (block->size >= size) {
        pthread_mutex_lock(&heap_lock);
        split_block(block, size);
        pthread_mutex_unlock(&heap_lock);
        return ptr;
    }
    void *new_ptr = bf_malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block->size);
    bf_free(ptr);
    return new_ptr;
}

static size_t leaked_bytes(void) {
    size_t leaked = 0;
    pthread_mutex_lock(&heap_lock);
    for (block_meta_t *b = global_base; b; b = b->next) {
        if (!b->free) leaked += b->size;
    }
    pthread_mutex_unlock(&heap_lock);
    return leaked;
}

typedef struct { int id; } thread_arg_t;
static void *thread_worker(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    void *m[10] = {0};
    void *c[10] = {0};
    for (int i = 0; i < 10; ++i) {
        m[i] = bf_malloc((size_t)(40 + t->id * 8 + i * 4));
        c[i] = bf_calloc((size_t)(i + 2), sizeof(int));
    }
    for (int i = 0; i < 10; ++i) m[i] = bf_realloc(m[i], (size_t)(80 + i * 8));
    for (int i = 0; i < 10; ++i) {
        bf_free(m[i]);
        bf_free(c[i]);
    }
    return NULL;
}

int main(void) {
    void *m[10] = {0};
    void *c[10] = {0};
    for (int i = 0; i < 10; ++i) m[i] = bf_malloc((size_t)(20 + i * 6));
    for (int i = 0; i < 10; ++i) c[i] = bf_calloc((size_t)(i + 1), sizeof(int));
    for (int i = 0; i < 10; ++i) m[i] = bf_realloc(m[i], (size_t)(72 + i * 10));
    for (int i = 0; i < 10; ++i) {
        bf_free(m[i]);
        bf_free(c[i]);
    }

    pthread_t t1, t2;
    thread_arg_t a1 = {1}, a2 = {2};
    pthread_create(&t1, NULL, thread_worker, &a1);
    pthread_create(&t2, NULL, thread_worker, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("[best-fit/dll] heap start: %p\n", heap_start);
    printf("[best-fit/dll] heap end  : %p\n", sbrk(0));
    printf("[best-fit/dll] leaked bytes: %zu\n", leaked_bytes());
    return 0;
}
