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
    int free;
} block_meta_t;

static __thread block_meta_t *thread_base = NULL;
static __thread void *thread_heap_start = NULL;
static pthread_mutex_t sbrk_lock = PTHREAD_MUTEX_INITIALIZER;

static block_meta_t *data_to_block(void *ptr) { return (block_meta_t *)ptr - 1; }

static block_meta_t *find_free_block_first_fit(block_meta_t **last, size_t size) {
    block_meta_t *current = thread_base;
    while (current) {
        if (current->free && current->size >= size) return current;
        *last = current;
        current = current->next;
    }
    return NULL;
}

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
    block->free = 0;

    if (!thread_heap_start) thread_heap_start = brk_before;
    if (last) last->next = block;
    return block;
}

static int are_adjacent(block_meta_t *a, block_meta_t *b) {
    return (char *)(a + 1) + a->size == (char *)b;
}

static void split_block(block_meta_t *block, size_t wanted) {
    if (!block || block->size <= wanted + sizeof(block_meta_t) + MIN_SPLIT_REMAINDER) return;

    block_meta_t *new_block = (block_meta_t *)((char *)(block + 1) + wanted);
    new_block->size = block->size - wanted - sizeof(block_meta_t);
    new_block->next = block->next;
    new_block->free = 1;

    block->size = wanted;
    block->next = new_block;
}

static void coalesce_free_blocks(void) {
    block_meta_t *current = thread_base;
    while (current && current->next) {
        if (current->free && current->next->free && are_adjacent(current, current->next)) {
            current->size += sizeof(block_meta_t) + current->next->size;
            current->next = current->next->next;
            continue;
        }
        current = current->next;
    }
}

void *ff_malloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN8(size);

    block_meta_t *block;
    if (!thread_base) {
        block = request_space(NULL, size);
        if (!block) return NULL;
        thread_base = block;
    } else {
        block_meta_t *last = thread_base;
        block = find_free_block_first_fit(&last, size);
        if (block) {
            block->free = 0;
            split_block(block, size);
        } else {
            block = request_space(last, size);
            if (!block) return NULL;
        }
    }
    return (void *)(block + 1);
}

void ff_free(void *ptr) {
    if (!ptr) return;
    block_meta_t *block = data_to_block(ptr);
    assert(block->free == 0);
    block->free = 1;
    coalesce_free_blocks();
}

void *ff_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0 || nmemb > SIZE_MAX / size) return NULL;
    void *ptr = ff_malloc(nmemb * size);
    if (ptr) memset(ptr, 0, nmemb * size);
    return ptr;
}

void *ff_realloc(void *ptr, size_t size) {
    if (!ptr) return ff_malloc(size);
    if (size == 0) {
        ff_free(ptr);
        return NULL;
    }

    size = ALIGN8(size);
    block_meta_t *block = data_to_block(ptr);
    if (block->size >= size) {
        split_block(block, size);
        return ptr;
    }

    void *new_ptr = ff_malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block->size);
    ff_free(ptr);
    return new_ptr;
}

static size_t leaked_bytes(void) {
    size_t leaked = 0;
    for (block_meta_t *b = thread_base; b; b = b->next) {
        if (!b->free) leaked += b->size;
    }
    return leaked;
}

static void *heap_end(void) {
    block_meta_t *last = thread_base;
    if (!last) return NULL;
    while (last->next) last = last->next;
    return (void *)((char *)(last + 1) + last->size);
}

static void run_workload(int seed) {
    void *m[10] = {0};
    void *c[10] = {0};
    for (int i = 0; i < 10; ++i) m[i] = ff_malloc((size_t)(24 + seed + i * 8));
    for (int i = 0; i < 10; ++i) c[i] = ff_calloc((size_t)(3 + i), sizeof(int));
    for (int i = 0; i < 10; ++i) m[i] = ff_realloc(m[i], (size_t)(96 + seed + i * 8));
    for (int i = 0; i < 10; ++i) {
        ff_free(m[i]);
        ff_free(c[i]);
    }
}

typedef struct { int id; } thread_arg_t;
static void *thread_worker(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    run_workload(10 * t->id);
    printf("[first-fit/sll] thread %d heap start: %p, heap end: %p, leaked bytes: %zu\n",
           t->id, thread_heap_start, heap_end(), leaked_bytes());
    return NULL;
}

int main(void) {
    run_workload(0);
    printf("[first-fit/sll] main heap start: %p, heap end: %p, leaked bytes: %zu\n",
           thread_heap_start, heap_end(), leaked_bytes());

    pthread_t th1, th2;
    thread_arg_t a1 = {1}, a2 = {2};
    pthread_create(&th1, NULL, thread_worker, &a1);
    pthread_create(&th2, NULL, thread_worker, &a2);
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    return 0;
}
