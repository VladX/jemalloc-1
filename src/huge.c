#define	JEMALLOC_HUGE_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

/* Protects chunk-related data structures. */
static malloc_mutex_t	huge_mtx;

/******************************************************************************/

/* Tree of chunks that are stand-alone huge allocations. */
static extent_tree_t	huge;

void *
huge_malloc(tsd_t *tsd, arena_t *arena, size_t size, bool zero)
{
	size_t usize;

	usize = s2u(size);
	if (usize == 0) {
		/* size_t overflow. */
		return (NULL);
	}

	return (huge_palloc(tsd, arena, usize, chunksize, zero));
}

void *
huge_palloc(tsd_t *tsd, arena_t *arena, size_t usize, size_t alignment,
    bool zero)
{
	void *ret;
	size_t csize;
	extent_node_t *node;
	bool is_zeroed;

	/* Allocate one or more contiguous chunks for this request. */

	csize = CHUNK_CEILING(usize);
	assert(csize >= usize);

	/* Allocate an extent node with which to track the chunk. */
	node = ipalloct(tsd, CACHELINE_CEILING(sizeof(extent_node_t)),
	    CACHELINE, false, tsd != NULL, NULL);
	if (node == NULL)
		return (NULL);

	/*
	 * Copy zero into is_zeroed and pass the copy to chunk_alloc(), so that
	 * it is possible to make correct junk/zero fill decisions below.
	 */
	is_zeroed = zero;
	arena = arena_choose(tsd, arena);
	if (unlikely(arena == NULL)) {
		base_node_dalloc(node);
		return (NULL);
	}
	ret = arena_chunk_alloc_huge(arena, NULL, csize, alignment, &is_zeroed);
	if (ret == NULL) {
		idalloct(tsd, node, tsd != NULL);
		return (NULL);
	}

	/* Insert node into huge. */
	node->addr = ret;
	node->size = usize;
	node->arena = arena;

	malloc_mutex_lock(&huge_mtx);
	extent_tree_ad_insert(&huge, node);
	malloc_mutex_unlock(&huge_mtx);

	if (config_fill && !zero) {
		if (unlikely(opt_junk))
			memset(ret, 0xa5, usize);
		else if (unlikely(opt_zero) && !is_zeroed)
			memset(ret, 0, usize);
	}

	return (ret);
}

#ifdef JEMALLOC_JET
#undef huge_dalloc_junk
#define	huge_dalloc_junk JEMALLOC_N(huge_dalloc_junk_impl)
#endif
static void
huge_dalloc_junk(void *ptr, size_t usize)
{

	if (config_fill && have_dss && unlikely(opt_junk)) {
		/*
		 * Only bother junk filling if the chunk isn't about to be
		 * unmapped.
		 */
		if (!config_munmap || (have_dss && chunk_in_dss(ptr)))
			memset(ptr, 0x5a, usize);
	}
}
#ifdef JEMALLOC_JET
#undef huge_dalloc_junk
#define	huge_dalloc_junk JEMALLOC_N(huge_dalloc_junk)
huge_dalloc_junk_t *huge_dalloc_junk = JEMALLOC_N(huge_dalloc_junk_impl);
#endif

static bool
huge_ralloc_no_move_expand(void *ptr, size_t oldsize, size_t size, bool zero) {
	size_t usize;
	void *expand_addr;
	size_t expand_size;
	extent_node_t *node, key;
	arena_t *arena;
	bool is_zeroed;
	void *ret;

	usize = s2u(size);
	if (usize == 0) {
		/* size_t overflow. */
		return (true);
	}

	expand_addr = ptr + CHUNK_CEILING(oldsize);
	expand_size = CHUNK_CEILING(usize) - CHUNK_CEILING(oldsize);

	malloc_mutex_lock(&huge_mtx);

	key.addr = ptr;
	node = extent_tree_ad_search(&huge, &key);
	assert(node != NULL);
	assert(node->addr == ptr);

	/* Find the current arena. */
	arena = node->arena;

	malloc_mutex_unlock(&huge_mtx);

	/*
	 * Copy zero into is_zeroed and pass the copy to chunk_alloc(), so that
	 * it is possible to make correct junk/zero fill decisions below.
	 */
	is_zeroed = zero;
	ret = arena_chunk_alloc_huge(arena, expand_addr, expand_size, chunksize,
				     &is_zeroed);
	if (ret == NULL)
		return (true);

	assert(ret == expand_addr);

	malloc_mutex_lock(&huge_mtx);
	/* Update the size of the huge allocation. */
	node->size = usize;
	malloc_mutex_unlock(&huge_mtx);

	if (config_fill && !zero) {
		if (unlikely(opt_junk))
			memset(ptr + oldsize, 0xa5, usize - oldsize);
		else if (unlikely(opt_zero) && !is_zeroed)
			memset(ptr + oldsize, 0, usize - oldsize);
	}
	return (false);
}

bool
huge_ralloc_no_move(void *ptr, size_t oldsize, size_t size, size_t extra,
    bool zero)
{
	size_t usize;

	/* Both allocations must be huge to avoid a move. */
	if (oldsize < chunksize)
		return (true);

	assert(s2u(oldsize) == oldsize);
	usize = s2u(size);
	if (usize == 0) {
		/* size_t overflow. */
		return (true);
	}

	/*
	 * Avoid moving the allocation if the existing chunk size accommodates
	 * the new size.
	 */
	if (CHUNK_CEILING(oldsize) >= CHUNK_CEILING(usize)
	    && CHUNK_CEILING(oldsize) <= CHUNK_CEILING(size+extra)) {
		size_t usize_next;

		/* Increase usize to incorporate extra. */
		while (usize < s2u(size+extra) && (usize_next = s2u(usize+1)) <
		    oldsize)
			usize = usize_next;

		/* Update the size of the huge allocation if it changed. */
		if (oldsize != usize) {
			extent_node_t *node, key;

			malloc_mutex_lock(&huge_mtx);

			key.addr = ptr;
			node = extent_tree_ad_search(&huge, &key);
			assert(node != NULL);
			assert(node->addr == ptr);

			assert(node->size != usize);
			node->size = usize;

			malloc_mutex_unlock(&huge_mtx);

			if (oldsize < usize) {
				if (zero || (config_fill &&
				    unlikely(opt_zero))) {
					memset(ptr + oldsize, 0, usize -
					    oldsize);
				} else if (config_fill && unlikely(opt_junk)) {
					memset(ptr + oldsize, 0xa5, usize -
					    oldsize);
				}
			} else if (config_fill && unlikely(opt_junk) && oldsize
			    > usize)
				memset(ptr + usize, 0x5a, oldsize - usize);
		}
		return (false);
	}

	if (CHUNK_CEILING(oldsize) >= CHUNK_CEILING(size)
	    && CHUNK_CEILING(oldsize) <= CHUNK_CEILING(size+extra)) {
		return (false);
	}

	/* Shrink the allocation in-place. */
	if (CHUNK_CEILING(oldsize) > CHUNK_CEILING(usize)) {
		extent_node_t *node, key;
		void *excess_addr;
		size_t excess_size;

		malloc_mutex_lock(&huge_mtx);

		key.addr = ptr;
		node = extent_tree_ad_search(&huge, &key);
		assert(node != NULL);
		assert(node->addr == ptr);

		/* Update the size of the huge allocation. */
		node->size = usize;

		malloc_mutex_unlock(&huge_mtx);

		excess_addr = node->addr + CHUNK_CEILING(usize);
		excess_size = CHUNK_CEILING(oldsize) - CHUNK_CEILING(usize);

		/* Zap the excess chunks. */
		huge_dalloc_junk(ptr + usize, oldsize - usize);
		arena_chunk_dalloc_huge(node->arena, excess_addr, excess_size);

		return (false);
	}

	/* Attempt to expand the allocation in-place. */
	if (huge_ralloc_no_move_expand(ptr, oldsize, size + extra, zero)) {
		if (extra == 0)
			return (true);

		/* Try again, this time without extra. */
		return (huge_ralloc_no_move_expand(ptr, oldsize, size, zero));
	}
	return (false);
}

void *
huge_ralloc(tsd_t *tsd, arena_t *arena, void *ptr, size_t oldsize, size_t size,
    size_t extra, size_t alignment, bool zero, bool try_tcache_dalloc)
{
	void *ret;
	size_t copysize;

	/* Try to avoid moving the allocation. */
	if (!huge_ralloc_no_move(ptr, oldsize, size, extra, zero))
		return (ptr);

	/*
	 * size and oldsize are different enough that we need to use a
	 * different size class.  In that case, fall back to allocating new
	 * space and copying.
	 */
	if (alignment > chunksize)
		ret = huge_palloc(tsd, arena, size + extra, alignment, zero);
	else
		ret = huge_malloc(tsd, arena, size + extra, zero);

	if (ret == NULL) {
		if (extra == 0)
			return (NULL);
		/* Try again, this time without extra. */
		if (alignment > chunksize)
			ret = huge_palloc(tsd, arena, size, alignment, zero);
		else
			ret = huge_malloc(tsd, arena, size, zero);

		if (ret == NULL)
			return (NULL);
	}

	/*
	 * Copy at most size bytes (not size+extra), since the caller has no
	 * expectation that the extra bytes will be reliably preserved.
	 */
	copysize = (size < oldsize) ? size : oldsize;
	memcpy(ret, ptr, copysize);
	iqalloc(tsd, ptr, try_tcache_dalloc);
	return (ret);
}

void
huge_dalloc(tsd_t *tsd, void *ptr)
{
	extent_node_t *node, key;

	malloc_mutex_lock(&huge_mtx);

	/* Extract from tree of huge allocations. */
	key.addr = ptr;
	node = extent_tree_ad_search(&huge, &key);
	assert(node != NULL);
	assert(node->addr == ptr);
	extent_tree_ad_remove(&huge, node);

	malloc_mutex_unlock(&huge_mtx);

	huge_dalloc_junk(node->addr, node->size);
	arena_chunk_dalloc_huge(node->arena, node->addr,
	    CHUNK_CEILING(node->size));
	idalloct(tsd, node, tsd != NULL);
}

size_t
huge_salloc(const void *ptr)
{
	size_t ret;
	extent_node_t *node, key;

	malloc_mutex_lock(&huge_mtx);

	/* Extract from tree of huge allocations. */
	key.addr = __DECONST(void *, ptr);
	node = extent_tree_ad_search(&huge, &key);
	assert(node != NULL);

	ret = node->size;

	malloc_mutex_unlock(&huge_mtx);

	return (ret);
}

prof_tctx_t *
huge_prof_tctx_get(const void *ptr)
{
	prof_tctx_t *ret;
	extent_node_t *node, key;

	malloc_mutex_lock(&huge_mtx);

	/* Extract from tree of huge allocations. */
	key.addr = __DECONST(void *, ptr);
	node = extent_tree_ad_search(&huge, &key);
	assert(node != NULL);

	ret = node->prof_tctx;

	malloc_mutex_unlock(&huge_mtx);

	return (ret);
}

void
huge_prof_tctx_set(const void *ptr, prof_tctx_t *tctx)
{
	extent_node_t *node, key;

	malloc_mutex_lock(&huge_mtx);

	/* Extract from tree of huge allocations. */
	key.addr = __DECONST(void *, ptr);
	node = extent_tree_ad_search(&huge, &key);
	assert(node != NULL);

	node->prof_tctx = tctx;

	malloc_mutex_unlock(&huge_mtx);
}

bool
huge_boot(void)
{

	/* Initialize chunks data. */
	if (malloc_mutex_init(&huge_mtx))
		return (true);
	extent_tree_ad_new(&huge);

	return (false);
}

void
huge_prefork(void)
{

	malloc_mutex_prefork(&huge_mtx);
}

void
huge_postfork_parent(void)
{

	malloc_mutex_postfork_parent(&huge_mtx);
}

void
huge_postfork_child(void)
{

	malloc_mutex_postfork_child(&huge_mtx);
}
