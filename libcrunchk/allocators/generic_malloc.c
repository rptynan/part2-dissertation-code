#include <libcrunchk/include/index_tree.h>
#include <libcrunchk/include/pageindex.h>
#include <libcrunchk/include/liballocs.h>
#include <sys/malloc.h>

#ifndef EXTRA_INSERT_SPACE
#define EXTRA_INSERT_SPACE 0
#endif

/* from heap_index.h */
#define INSERT_DESCRIBES_OBJECT(ins) (!((ins)->alloc_site) )
	/* || (char*)((uintptr_t)((unsigned long long)((ins)->alloc_site))) >= MINIMUM_USER_ADDRESS)
	 * Feel like there's no minimum address appropriate for kernel */

/* index_tree wrapper functions, all subject to change */
struct itree_node *heapindex_root = NULL;

int heapindex_compare(const void *a, const void *b) {
	struct insert *aa = (struct insert *) a;
	struct insert *bb = (struct insert *) b;
	if (aa->addr < bb->addr) return -1;  // addr a good choice here?
	if (aa->addr > bb->addr) return +1;  // -> yes because only thing free gets
	return 0;
}

unsigned long heapindex_distance(const void *a, const void *b) {
	struct insert *aa = (struct insert *) a;
	struct insert *bb = (struct insert *) b;
	#define max(x, y) (x > y ? x : y)
	#define min(x, y) (x < y ? x : y)
	return (unsigned long)
		(max(aa->addr, bb->addr) - min(aa->addr, bb->addr));
}

struct insert *heapindex_lookup(const void *addr) {
	const struct insert ins = {.addr = (void *)addr};
	struct itree_node *res = itree_find_closest_under(
		heapindex_root, &ins, heapindex_compare, heapindex_distance
	);
	if (res) return (struct insert *) res->data;
	return NULL;
}

void heapindex_insert(
	void *alloc_site,
	void *addr,
	_Bool alloc_site_is_actually_uniqtype
) {
	struct insert *ins =
		__real_malloc(sizeof(struct insert), M_TEMP, M_WAITOK);
	ins->alloc_site_flag = alloc_site_is_actually_uniqtype;
	ins->alloc_site = (unsigned long) alloc_site;
	ins->addr = addr;
	itree_insert(&heapindex_root, (void *)ins, heapindex_compare);
}

void heapindex_remove(void *addr) {
	struct insert ins = {.addr = addr};
	void *free_me = itree_remove(
		&heapindex_root, (void *)&ins, heapindex_compare
	);
	if (free_me) __real_free(free_me, M_TEMP);
}


/* A client-friendly lookup function that knows about bigallocs.
 * FIXME: this needs to go away! Clients shouldn't have to know about inserts,
 * and not all allocators maintain them. */
struct insert *__liballocs_get_insert(const void *mem)
{
	struct big_allocation *b = __lookup_bigalloc(mem,
		&__generic_malloc_allocator, NULL);
	if (b)
	{
		// TODO see if this is used
		/* assert(b->meta.what == INS_AND_BITS); */
		/* return &b->meta.un.ins_and_bits.ins; */
	}
	return lookup_object_info(mem, NULL, NULL, NULL);
}

liballocs_err_t __generic_heap_get_info(
	void * obj,
	struct big_allocation *maybe_bigalloc,
	struct uniqtype **out_type,
	void **out_base,
	unsigned long *out_size,
	const void **out_site
) {
	++__liballocs_hit_heap_case; // FIXME: needn't be heap -- could be alloca
	/* For heap allocations, we look up the allocation site.
	 * (This also yields an offset within a toplevel object.)
	 * Then we translate the allocation site to a uniqtypes rec location.
	 * (For direct calls in eagerly-loaded code, we can cache this information
	 * within uniqtypes itself. How? Make uniqtypes include a hash table with
	 * initial contents mapping allocsites to uniqtype recs. This hash table
	 * is initialized during load, but can be extended as new allocsites
	 * are discovered, e.g. indirect ones.)
	 */
	struct insert *heap_info = NULL;
	
	/* NOTE: bigallocs already have the size adjusted by the insert. */
	if (maybe_bigalloc)
	{
		/* We already have the metadata. */
		// TODO replacing this with lookup for now, is meta updated anywhere?
		// If so, use here.
		/* heap_info = &maybe_bigalloc->meta.un.ins_and_bits.ins; */
		heap_info = lookup_object_info(obj, NULL, NULL, NULL);
		if (out_base) *out_base = maybe_bigalloc->begin;
		if (out_size) *out_size = (char*) maybe_bigalloc->end - (char*) maybe_bigalloc->begin;
	} 
	else
	{
		size_t alloc_chunksize;
		heap_info = lookup_object_info(obj, out_base, &alloc_chunksize, NULL);
		if (heap_info)
		{
			if (out_size) *out_size = alloc_chunksize - sizeof (struct insert) - EXTRA_INSERT_SPACE;
		}
	}
	
	if (!heap_info)
	{
		++__liballocs_aborted_unindexed_heap;
		return &__liballocs_err_unindexed_heap_object;
	}
	
	return extract_and_output_alloc_site_and_type(
		heap_info, out_type, (void**) out_site
	);
}


struct insert *lookup_object_info(
	const void *mem,
	void **out_object_start,
	size_t *out_object_size,
	void **ignored
) {
	/* There was a lookup cache here */

	struct insert *result = heapindex_lookup(mem);

	/* If we have caching which can tell us these, uncomment */
	/* if (out_object_start) *out_object_start = object_start; */
	/* if (out_object_size) *out_object_size = size; */

	return result;
}



struct allocator __generic_malloc_allocator = {
	.name = "generic malloc",
	.get_info = __generic_heap_get_info,
	.is_cacheable = 1,
	/* .ensure_big = NULL,  // ensure_big, TODO ? */
};



/*
 * malloc wrappers
*/

int __currently_freeing = 0;  // TODO
int __currently_allocating = 0; // TODO mutexes

/* We don't want to do expensive operations during malloc with M_NOWAIT or
 * during frees, so instead we buffer them all using a static buffer and
 * perform them during M_WAITOK mallocs */
#define MALLOC_BUF_SIZE 100000
unsigned long free_buffer_i = 0;
void *free_buffer[MALLOC_BUF_SIZE];
struct malloc_buffer_type {
	void *addr;
	void *caller;
	unsigned long size;
};
unsigned long malloc_buffer_i = 0;
struct malloc_buffer_type malloc_buffer[MALLOC_BUF_SIZE];


/* The hook, __real_malloc has same signature */
#include <libcrunchk/include/index_tree.h>
void *__wrap_malloc(unsigned long size, struct malloc_type *mtp, int flags)
{
	PRINTD1("malloc called, size: %u", size);
	if (!mtp) PRINTD("malloc, no type!");
	else PRINTD1("malloc called, type: %s", mtp->ks_shortdesc);

	/* __currently_allocating++; */
	/* PRINTD1("malloc __currently_allocating = %u", __currently_allocating); */
	void *ret;
	ret = __real_malloc(size, mtp, flags);
	if (ret) {
		void *caller = __builtin_return_address(1);
		if (flags & M_WAITOK) {
			PRINTD("malloc inserting to indices");
			// TODO we're ignoring M_NOWAITs for now because itree can't do a
			// malloc during them, but should add a buffer to do M_NOWAITs
			// later
			// TODO ensure thread-safety
			pageindex_insert(ret, ret + size, &__generic_malloc_allocator);
			heapindex_insert(caller, ret, 0);

			for (int i = 0; i < free_buffer_i; i++) {
				// Might be unnecessary to buffer these as is_a, etc does
				// lookup anyway...
				pageindex_remove(free_buffer[i]);
				heapindex_remove(free_buffer[i]);
			}
			free_buffer_i = 0;
			for (int i = 0; i < malloc_buffer_i; i++) {
				pageindex_insert(
					malloc_buffer[i].addr,
					malloc_buffer[i].addr + malloc_buffer[i].size,
					&__generic_malloc_allocator
				);
				heapindex_insert(
					malloc_buffer[i].caller,
					malloc_buffer[i].addr,
					0
				);
			}
			malloc_buffer_i = 0;
		}
		else {
			PRINTD("malloc buffering index insert");
			if (malloc_buffer_i < MALLOC_BUF_SIZE - 1) {
				malloc_buffer[malloc_buffer_i++].addr = ret;
				malloc_buffer[malloc_buffer_i++].caller = caller;
				malloc_buffer[malloc_buffer_i++].size = size;
			}
		}
	}
	/* __currently_allocating--; */
	PRINTD1("malloc returning: %p", ret);
	return ret;
}

void __wrap_free(void *addr, struct malloc_type *mtp)
{
	PRINTD1("free called, addr: %p", addr);
	__real_free(addr, mtp);
	if (free_buffer_i < MALLOC_BUF_SIZE - 1) free_buffer[free_buffer_i++] = addr;
}
