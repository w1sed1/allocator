// SPDX-License-Identifier: BSD-3-Clause

#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

#include "osmem.h"
#include "block_meta.h"

#define ALIGNMENT	8
#define ALIGN(size)	(((size) + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1))
#define META_SIZE	ALIGN(sizeof(struct block_meta))
#define MMAP_THRESHOLD	(128 * 1024)
/* Largest request whose aligned size plus metadata does not overflow size_t */
#define MAX_REQUEST	(SIZE_MAX - META_SIZE - ALIGNMENT)
#define MIN(a, b)	((a) < (b) ? (a) : (b))

/* List of all blocks placed on the heap (brk), ordered by address */
static struct block_meta *heap_head;

static struct block_meta *get_block(void *ptr)
{
	return (struct block_meta *)((char *)ptr - META_SIZE);
}

static void *get_payload(struct block_meta *block)
{
	return (char *)block + META_SIZE;
}

static void *heap_grow(size_t increment)
{
	void *old_brk = sbrk(increment);

	DIE(old_brk == (void *)-1, "sbrk");
	return old_brk;
}

static struct block_meta *heap_last(void)
{
	struct block_meta *block = heap_head;

	while (block && block->next)
		block = block->next;
	return block;
}

/* Merge block with the free block that follows it */
static void merge_next(struct block_meta *block)
{
	struct block_meta *next = block->next;

	block->size += META_SIZE + next->size;
	block->next = next->next;
	if (block->next)
		block->next->prev = block;
}

static void coalesce_all(void)
{
	struct block_meta *block = heap_head;

	while (block && block->next) {
		if (block->status == STATUS_FREE && block->next->status == STATUS_FREE)
			merge_next(block);
		else
			block = block->next;
	}
}

/* Truncate block to size; the remainder becomes a new free block if it fits one */
static void split_block(struct block_meta *block, size_t size)
{
	struct block_meta *rest;

	if (block->size < size + META_SIZE + ALIGN(1))
		return;

	rest = (struct block_meta *)((char *)get_payload(block) + size);
	rest->size = block->size - size - META_SIZE;
	rest->status = STATUS_FREE;
	rest->prev = block;
	rest->next = block->next;
	if (rest->next)
		rest->next->prev = rest;

	block->next = rest;
	block->size = size;
}

static struct block_meta *find_best(size_t size)
{
	struct block_meta *best = NULL;

	for (struct block_meta *block = heap_head; block; block = block->next)
		if (block->status == STATUS_FREE && block->size >= size &&
		    (!best || block->size < best->size))
			best = block;
	return best;
}

/* The first heap allocation reserves MMAP_THRESHOLD bytes at once */
static void heap_preallocate(void)
{
	heap_head = heap_grow(MMAP_THRESHOLD);
	heap_head->size = MMAP_THRESHOLD - META_SIZE;
	heap_head->status = STATUS_FREE;
	heap_head->prev = NULL;
	heap_head->next = NULL;
}

static struct block_meta *heap_alloc(size_t size)
{
	struct block_meta *block, *last;

	if (!heap_head)
		heap_preallocate();

	coalesce_all();
	block = find_best(size);
	if (block) {
		split_block(block, size);
		block->status = STATUS_ALLOC;
		return block;
	}

	/* No free block fits: grow the heap, reusing the last block if free */
	last = heap_last();
	if (last->status == STATUS_FREE) {
		heap_grow(size - last->size);
		last->size = size;
		last->status = STATUS_ALLOC;
		return last;
	}

	block = heap_grow(META_SIZE + size);
	block->size = size;
	block->status = STATUS_ALLOC;
	block->prev = last;
	block->next = NULL;
	last->next = block;
	return block;
}

static struct block_meta *map_alloc(size_t size)
{
	struct block_meta *block = mmap(NULL, META_SIZE + size, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANON, -1, 0);

	DIE(block == MAP_FAILED, "mmap");
	block->size = size;
	block->status = STATUS_MAPPED;
	block->prev = NULL;
	block->next = NULL;
	return block;
}

/* Blocks whose total size reaches threshold are mapped, the rest go on the heap */
static void *alloc(size_t size, size_t threshold)
{
	struct block_meta *block;

	if (size > MAX_REQUEST) {
		errno = ENOMEM;
		return NULL;
	}

	size = ALIGN(size);
	if (META_SIZE + size < threshold)
		block = heap_alloc(size);
	else
		block = map_alloc(size);
	return get_payload(block);
}

void *os_malloc(size_t size)
{
	if (!size)
		return NULL;
	return alloc(size, MMAP_THRESHOLD);
}

void os_free(void *ptr)
{
	struct block_meta *block;

	if (!ptr)
		return;

	block = get_block(ptr);
	if (block->status == STATUS_MAPPED)
		DIE(munmap(block, META_SIZE + block->size) == -1, "munmap");
	else
		block->status = STATUS_FREE;
}

void *os_calloc(size_t nmemb, size_t size)
{
	void *ptr;

	if (!nmemb || !size || nmemb > SIZE_MAX / size)
		return NULL;

	size *= nmemb;
	ptr = alloc(size, getpagesize());
	if (ptr)
		memset(ptr, 0, size);
	return ptr;
}

/* Try to grow a heap block in place; returns 0 if it has to be moved */
static int expand_block(struct block_meta *block, size_t size)
{
	while (block->next && block->next->status == STATUS_FREE) {
		merge_next(block);
		if (block->size >= size) {
			split_block(block, size);
			return 1;
		}
	}

	if (block->next)
		return 0;

	/* Last block on the heap: extend the program break */
	heap_grow(size - block->size);
	block->size = size;
	return 1;
}

void *os_realloc(void *ptr, size_t size)
{
	struct block_meta *block;
	void *new_ptr;

	if (!ptr)
		return os_malloc(size);
	if (!size) {
		os_free(ptr);
		return NULL;
	}

	block = get_block(ptr);
	if (block->status == STATUS_FREE)
		return NULL;
	if (size > MAX_REQUEST) {
		errno = ENOMEM;
		return NULL;
	}

	size = ALIGN(size);
	if (block->status == STATUS_ALLOC && META_SIZE + size < MMAP_THRESHOLD) {
		if (block->size >= size) {
			split_block(block, size);
			return ptr;
		}
		if (expand_block(block, size))
			return ptr;
	}

	new_ptr = os_malloc(size);
	memcpy(new_ptr, ptr, MIN(block->size, size));
	os_free(ptr);
	return new_ptr;
}
