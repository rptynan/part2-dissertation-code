#include <libcrunchk/include/liballocs.h>
#include <libcrunchk/include/index_tree.h>
#include <libcrunchk/include/pageindex.h>
#include <libcrunchk/include/allocmeta.h>
#ifdef _KERNEL
  // CTASSERT, for sysctl
  #include <sys/cdefs.h>
  #include <sys/param.h>
  #include <sys/systm.h>
#endif
// sysctl
#include <sys/types.h>
#include <sys/sysctl.h>


/* special uniqtypes */
struct uniqtype *pointer_to___uniqtype__void;
struct uniqtype *pointer_to___uniqtype__signed_char;
struct uniqtype *pointer_to___uniqtype__unsigned_char;
struct uniqtype *pointer_to___uniqtype____PTR_signed_char;
struct uniqtype *pointer_to___uniqtype____PTR___PTR_signed_char;
struct uniqtype *pointer_to___uniqtype__Elf64_auxv_t;
struct uniqtype *pointer_to___uniqtype____ARR0_signed_char;
struct uniqtype *pointer_to___uniqtype__intptr_t;


/* Counters, macro will define the unsigned long to store the value and then
 * define a sysctl MIB under the debug.liballocs parent node */
#ifdef _KERNEL
  static SYSCTL_NODE(
	_debug, OID_AUTO, liballocs, CTLFLAG_RD, 0, "liballocs stats"
  );
  #define LIBALLOCS_COUNTER(name) \
	unsigned long int __liballocs_ ## name = 0; \
	SYSCTL_ULONG( \
		_debug_liballocs, OID_AUTO, name, CTLFLAG_RW, \
		&__liballocs_ ## name, \
		sizeof(__liballocs_ ## name), \
		"__liballocs_" #name \
	)
#else
  /* In the (userspace) test case, when we don't have the kernel stuff, just
   * ignore syctl */
  #define LIBALLOCS_COUNTER(name) \
	unsigned long int __liballocs_ ## name = 0;
#endif

LIBALLOCS_COUNTER(aborted_stack);
LIBALLOCS_COUNTER(aborted_static);
LIBALLOCS_COUNTER(aborted_unknown_storage);
LIBALLOCS_COUNTER(hit_heap_case);
LIBALLOCS_COUNTER(hit_stack_case);
LIBALLOCS_COUNTER(hit_static_case);
LIBALLOCS_COUNTER(aborted_unindexed_heap);
LIBALLOCS_COUNTER(aborted_unrecognised_allocsite);


_Bool __liballocs_is_initialized = 0;

/* built-in errors */
struct liballocs_err __liballocs_err_not_impl
 = {"Not implemented"};
struct liballocs_err __liballocs_err_abort
 = {"Aborted"};
struct liballocs_err __liballocs_err_stack_walk_step_failure
 = { "stack walk reached higher frame" };
struct liballocs_err __liballocs_err_stack_walk_reached_higher_frame 
 = { "stack walk reached higher frame" };
struct liballocs_err __liballocs_err_stack_walk_reached_top_of_stack 
 = { "stack walk reached top-of-stack" };
struct liballocs_err __liballocs_err_unknown_stack_walk_problem 
 = { "unknown stack walk problem" };
struct liballocs_err __liballocs_err_unindexed_heap_object
 = { "unindexed heap object" };
struct liballocs_err __liballocs_err_unrecognised_alloc_site
 = { "unrecognised alloc site" };
struct liballocs_err __liballocs_err_unrecognised_static_object
 = { "unrecognised static object" };
struct liballocs_err __liballocs_err_object_of_unknown_storage
 = { "object of unknown storage" };


/* allocsite -> uniqtype index */
struct uniqtype_index_node {
	const void *site;
	const void *type;
};
int uniqtype_index_compare(const void *a, const void *b) {
	const struct uniqtype_index_node *aa =
		(const struct uniqtype_index_node *) a;
	const struct uniqtype_index_node *bb =
		(const struct uniqtype_index_node *) b;
	if (aa->site < bb->site) return -1;
	if (aa->site > bb->site) return +1;
	return 0;
}
struct itree_node *uniqtype_index = NULL;

/* other special uniqtype for unset types */
struct uniqtype unset__uniqtype__ = {{0}};


extern inline void __liballocs_ensure_init(void)
{
	PRINTD("__liballocs_ensure_init");
	if (unlikely(!__liballocs_is_initialized)) {
		__liballocs_is_initialized = 1;
		__static_allocator_init();
	}
}

extern inline struct liballocs_err *__liballocs_get_alloc_info(
	const void *obj,
	struct allocator **out_allocator,
	const void **out_alloc_start,
	unsigned long *out_alloc_size_bytes,
	struct uniqtype **out_alloc_uniqtype,
	const void **out_alloc_site
) {
	PRINTD1("__liballocs_get_alloc_info: %p", obj);
	/* struct liballocs_err *err = 0; */

	struct big_allocation *containing_bigalloc;
	struct big_allocation *maybe_the_allocation;
	struct allocator *a = __liballocs_leaf_allocator_for(
		obj, &containing_bigalloc, &maybe_the_allocation
	);
	if (__builtin_expect(!a, 0))
	{
		_Bool fixed = __liballocs_notify_unindexed_address(obj);
		if (fixed)
		{
			a = __liballocs_leaf_allocator_for(
				obj, &containing_bigalloc, &maybe_the_allocation
			);
			if (!a) return &__liballocs_err_abort;
		}
		else
		{
			__liballocs_report_wild_address(obj);
			++__liballocs_aborted_unknown_storage;
			return &__liballocs_err_object_of_unknown_storage;
		}
	}
	if (out_allocator) *out_allocator = a;
	return a->get_info(
		(void*) obj,
		maybe_the_allocation,
		out_alloc_uniqtype,
		(void**) out_alloc_start,
		out_alloc_size_bytes,
		out_alloc_site
	);
}

/* inline int */ 
int
__liballocs_walk_subobjects_spanning_rec(
	signed accum_offset, unsigned accum_depth,
	const signed target_offset_within_u,
	struct uniqtype *u, 
	int (*cb)(struct uniqtype *spans, signed span_start_offset, unsigned depth, 
		struct uniqtype *containing, struct uniqtype_rel_info *contained_pos, 
		signed containing_span_start_offset, void *arg),
	void *arg
	)
{
	signed ret = 0;
	/* Calculate the offset to descend to, if any. This is different for 
	 * structs versus arrays. */
	if (UNIQTYPE_IS_ARRAY_TYPE(u))
	{
		struct uniqtype_rel_info *element_contained_in_u = &u->related[0];
		struct uniqtype *element_uniqtype = element_contained_in_u->un.t.ptr;
		signed div = target_offset_within_u / element_uniqtype->pos_maxoff;
		signed mod = target_offset_within_u % element_uniqtype->pos_maxoff;
		if (element_uniqtype->pos_maxoff != 0 && UNIQTYPE_ARRAY_LENGTH(u) > div)
		{
			signed skip_sz = div * element_uniqtype->pos_maxoff;
			__liballocs_private_assert(target_offset_within_u < u->pos_maxoff,
				"offset not within subobject", __FILE__, __LINE__, __func__);
			int ret = cb(element_uniqtype, accum_offset + skip_sz, accum_depth + 1u, 
				u, element_contained_in_u, accum_offset, arg);
			if (ret) return ret;
			// tail-recurse
			else return __liballocs_walk_subobjects_spanning_rec(
				accum_offset + (div * element_uniqtype->pos_maxoff), accum_depth + 1u,
				mod, element_uniqtype, cb, arg);
		} 
		else // element's pos_maxoff == 0 || num_contained <= target_offset / element's pos_maxoff 
		{}
	}
	else // struct/union case
	{
		signed num_contained = UNIQTYPE_COMPOSITE_MEMBER_COUNT(u);
		int lower_ind = 0;
		int upper_ind = num_contained;
		while (lower_ind + 1 < upper_ind) // difference of >= 2
		{
			/* Bisect the interval */
			int bisect_ind = (upper_ind + lower_ind) / 2;
			__liballocs_private_assert(bisect_ind > lower_ind, "progress", __FILE__, __LINE__, __func__);
			if (u->related[bisect_ind].un.memb.off > target_offset_within_u)
			{
				/* Our solution lies in the lower half of the interval */
				upper_ind = bisect_ind;
			} else lower_ind = bisect_ind;
		}

		if (lower_ind + 1 == upper_ind)
		{
			/* We found one offset; we may still have overshot, in the case of a 
			 * stack frame where offset zero might not be used. */
			if (u->related[lower_ind].un.memb.off > target_offset_within_u)
			{
				return 0; // FIXME: cb not called; what should our return value be then?
			}

			/* ... but we might not have found the *lowest* index, in the 
			 * case of a union. Scan backwards so that we have the lowest. 
			 * FIXME: need to account for the element size? Or here are we
			 * ignoring padding anyway? */
			signed offset = u->related[lower_ind].un.memb.off;
			for (;
					lower_ind > 0 && u->related[lower_ind-1].un.memb.off == offset;
					--lower_ind);
			// now we have the lowest lower_ind
			// scan forwards!
			for (int i_ind = lower_ind; i_ind < UNIQTYPE_COMPOSITE_MEMBER_COUNT(u)
					&& u->related[i_ind].un.memb.off == offset;
					++i_ind)
			{
				if (target_offset_within_u < u->pos_maxoff)
				{
					int ret = cb(u->related[i_ind].un.memb.ptr, accum_offset + offset,
							accum_depth + 1u, u, &u->related[i_ind], accum_offset, arg);
					if (ret) return ret;
					else
					{
						ret = __liballocs_walk_subobjects_spanning_rec(
							accum_offset + offset, accum_depth + 1u,
							target_offset_within_u - offset, u->related[i_ind].un.memb.ptr, cb, arg);
						if (ret) return ret;
					}
				}
			}
		}
		else /* lower_ind >= upper_ind */
		{
			// this should mean num_contained == 0
			__liballocs_private_assert(num_contained == 0,
				"no contained objects", __FILE__, __LINE__, __func__);
		}
	}
	
	return ret;
}

extern inline int
/* int */
/* __attribute__((always_inline,gnu_inline)) */
__liballocs_walk_subobjects_spanning(
	const signed target_offset_within_u,
	struct uniqtype *u, 
	int (*cb)(struct uniqtype *spans, signed span_start_offset, unsigned depth,
		struct uniqtype *containing, struct uniqtype_rel_info *contained_pos, 
		signed containing_span_start_offset, void *arg),
	void *arg
	)
{
	return __liballocs_walk_subobjects_spanning_rec(
		/* accum_offset */ 0, /* accum_depth */ 0, 
		target_offset_within_u, u, cb, arg);
}

extern inline _Bool __liballocs_find_matching_subobject(
	signed target_offset_within_uniqtype,
	struct uniqtype *cur_obj_uniqtype,
	struct uniqtype *test_uniqtype,
	struct uniqtype **last_attempted_uniqtype,
	signed *last_uniqtype_offset,
	signed *p_cumulative_offset_searched,
	struct uniqtype **p_cur_containing_uniqtype,
	struct uniqtype_rel_info **p_cur_contained_pos
) {
	PRINTD2(
		"__liballocs_find_matching_subobject, test: %p, alloc: %p",
		test_uniqtype,
		cur_obj_uniqtype
	);
	if (target_offset_within_uniqtype == 0 
		&& (!test_uniqtype || cur_obj_uniqtype == test_uniqtype))
	{
		if (p_cur_containing_uniqtype) *p_cur_containing_uniqtype = NULL;
		if (p_cur_contained_pos) *p_cur_contained_pos = NULL;
		return 1;
	}
	else
	{
		/* We might have *multiple* subobjects spanning the offset.
		 * Test all of them. */
		struct uniqtype *containing_uniqtype = NULL;
		struct uniqtype_rel_info *contained_pos = NULL;
		
		signed sub_target_offset = target_offset_within_uniqtype;
		struct uniqtype *contained_uniqtype = cur_obj_uniqtype;
		
		_Bool success = __liballocs_first_subobject_spanning(
			&sub_target_offset, &contained_uniqtype,
			&containing_uniqtype, &contained_pos);
		// now we have a *new* sub_target_offset and contained_uniqtype
		
		if (!success) return 0;
		
		if (p_cumulative_offset_searched) *p_cumulative_offset_searched += contained_pos->un.memb.off;
		
		if (last_attempted_uniqtype) *last_attempted_uniqtype = contained_uniqtype;
		if (last_uniqtype_offset) *last_uniqtype_offset = sub_target_offset;
		do {
			assert(containing_uniqtype == cur_obj_uniqtype);
			_Bool recursive_test = __liballocs_find_matching_subobject(
					sub_target_offset,
					contained_uniqtype, test_uniqtype,
					last_attempted_uniqtype, last_uniqtype_offset, p_cumulative_offset_searched,
					p_cur_containing_uniqtype,
					p_cur_contained_pos);
			if (__builtin_expect(recursive_test, 1))
			{
				if (p_cur_containing_uniqtype) *p_cur_containing_uniqtype = containing_uniqtype;
				if (p_cur_contained_pos) *p_cur_contained_pos = contained_pos;

				return 1;
			}
			// else look for a later contained subobject at the same offset
			signed subobj_ind = contained_pos - &containing_uniqtype->related[0];
			assert(subobj_ind >= 0);
			assert(subobj_ind == 0 || subobj_ind < UNIQTYPE_COMPOSITE_MEMBER_COUNT(containing_uniqtype));
			if (__builtin_expect(
					UNIQTYPE_COMPOSITE_MEMBER_COUNT(containing_uniqtype) <= subobj_ind + 1
					|| containing_uniqtype->related[subobj_ind + 1].un.memb.off != 
						containing_uniqtype->related[subobj_ind].un.memb.off,
				1))
			{
				// no more subobjects at the same offset, so fail
				return 0;
			} 
			else
			{
				contained_pos = &containing_uniqtype->related[subobj_ind + 1];
				contained_uniqtype = contained_pos->un.memb.ptr;
			}
		} while (1);
		
		assert(0);
	}
	return 0;
}


/* stolen from libkern, to avoid outside calls */
static int __liballocs_strncmp(const char *s1, const char *s2, size_t n)
{
	if (n == 0)
		return (0);
	do {
		if (*s1 != *s2++)
			return (*(const unsigned char *)s1 -
					*(const unsigned char *)(s2 - 1));
		if (*s1++ == '\0')
			break;
	} while (--n != 0);
	return (0);
}

static const char *(__attribute__((pure)) __liballocs_uniqtype_symbol_name)(
	const struct uniqtype *u
) {
	return "__liballocs_uniqtype_symbol_name not implemented";
	// TODO not sure if will work in kernel
	/* if (!u) return NULL; */
	/* Dl_info i = dladdr_with_cache(u); */
	/* if (i.dli_saddr == u) */
	/* { */
	/* 	return i.dli_sname; */
	/* } else return NULL; */
}

const char *(__attribute__((pure)) __liballocs_uniqtype_name)(
	const struct uniqtype *u
) {
	return "(no type)";
	if (!u) return "(no type)";
	const char *symbol_name = __liballocs_uniqtype_symbol_name(u);
	if (symbol_name)
	{
		if (0 == __liballocs_strncmp(symbol_name, "__uniqtype__", sizeof "__uniqtype__" - 1))
		{
			/* Codeless. */
			return symbol_name + sizeof "__uniqtype__" - 1;
		}
		else if (0 == __liballocs_strncmp(symbol_name, "__uniqtype_", sizeof "__uniqtype_" - 1))
		{
			/* With code. */
			return symbol_name + sizeof "__uniqtype_" - 1 + /* code + underscore */ 9;
		}
		return symbol_name;
	}
	return "(unnamed type)";
}


inline _Bool
/* __attribute__((always_inline,gnu_inline)) */
__liballocs_first_subobject_spanning(
	signed *p_target_offset_within_uniqtype,
	struct uniqtype **p_cur_obj_uniqtype,
	struct uniqtype **p_cur_containing_uniqtype,
	struct uniqtype_rel_info **p_cur_contained_pos
) {
	struct uniqtype *cur_obj_uniqtype = *p_cur_obj_uniqtype;
	signed target_offset_within_uniqtype = *p_target_offset_within_uniqtype;
	/* Calculate the offset to descend to, if any. This is different for 
	 * structs versus arrays. */
	if (UNIQTYPE_IS_ARRAY_TYPE(cur_obj_uniqtype))
	{
		signed num_contained = UNIQTYPE_ARRAY_LENGTH(cur_obj_uniqtype);
		struct uniqtype *element_uniqtype = UNIQTYPE_ARRAY_ELEMENT_TYPE(cur_obj_uniqtype);
		if (element_uniqtype->pos_maxoff != 0 && 
				num_contained > target_offset_within_uniqtype / element_uniqtype->pos_maxoff)
		{
			*p_cur_containing_uniqtype = cur_obj_uniqtype;
			*p_cur_contained_pos = &cur_obj_uniqtype->related[0];
			*p_cur_obj_uniqtype = element_uniqtype;
			*p_target_offset_within_uniqtype = target_offset_within_uniqtype % element_uniqtype->pos_maxoff;
			return 1;
		} else return 0;
	}
	else // struct/union case
	{
		signed num_contained = UNIQTYPE_COMPOSITE_MEMBER_COUNT(cur_obj_uniqtype);

		int lower_ind = 0;
		int upper_ind = num_contained;
		while (lower_ind + 1 < upper_ind) // difference of >= 2
		{
			/* Bisect the interval */
			int bisect_ind = (upper_ind + lower_ind) / 2;
			__liballocs_private_assert(bisect_ind > lower_ind, "bisection progress",
				__FILE__, __LINE__, __func__);
			if (cur_obj_uniqtype->related[bisect_ind].un.memb.off > target_offset_within_uniqtype)
			{
				/* Our solution lies in the lower half of the interval */
				upper_ind = bisect_ind;
			} else lower_ind = bisect_ind;
		}

		if (lower_ind + 1 == upper_ind)
		{
			/* We found one offset; we may still have overshot, in the case of a 
			 * stack frame where offset zero might not be used. */
			if (cur_obj_uniqtype->related[lower_ind].un.memb.off > target_offset_within_uniqtype)
			{
				return 0;
			}
			/* We found one offset */
			__liballocs_private_assert(cur_obj_uniqtype->related[lower_ind].un.memb.off <= target_offset_within_uniqtype,
				"offset underapproximates", __FILE__, __LINE__, __func__);

			/* ... but we might not have found the *lowest* index, in the 
			 * case of a union. Scan backwards so that we have the lowest. 
			 * FIXME: need to account for the element size? Or here are we
			 * ignoring padding anyway? */
			while (lower_ind > 0 
				&& cur_obj_uniqtype->related[lower_ind-1].un.memb.off
					 == cur_obj_uniqtype->related[lower_ind].un.memb.off)
			{
				--lower_ind;
			}
			*p_cur_contained_pos = &cur_obj_uniqtype->related[lower_ind];
			*p_cur_containing_uniqtype = cur_obj_uniqtype;
			*p_cur_obj_uniqtype
			 = cur_obj_uniqtype->related[lower_ind].un.memb.ptr;
			/* p_cur_obj_uniqtype now points to the subobject's uniqtype. 
			 * We still have to adjust the offset. */
			*p_target_offset_within_uniqtype
			 = target_offset_within_uniqtype - cur_obj_uniqtype->related[lower_ind].un.memb.off;

			return 1;
		}
		else /* lower_ind >= upper_ind */
		{
			// this should mean num_contained == 0
			__liballocs_private_assert(num_contained == 0,
				"no contained objects", __FILE__, __LINE__, __func__);
			return 0;
		}
	}
 return 1;
}

const char *format_symbolic_address(const void *addr)
{
	return "format_symbolic_address not implemented";
}

void __liballocs_notify_unset_type(
	const void *alloc_site,
	const void *test_uniqtype
) {
	struct uniqtype_index_node *node = __real_malloc(
		sizeof(struct uniqtype_index_node),
		M_TEMP,
		M_WAITOK  // TODO this probably isn't right, as it's called from is_a
	);
	node->site = alloc_site;
	node->type = test_uniqtype;
	__libcrunch_uniqtype_entries++;
	TYPESINDEX_WLOCK;
	itree_insert(&uniqtype_index, node, uniqtype_index_compare);
	TYPESINDEX_UNLOCK;
}

inline
struct uniqtype * __attribute__((gnu_inline))
allocsite_to_uniqtype(const void *allocsite)
{
	PRINTD1("allocsite_to_uniqtype: %p", allocsite);
	const struct uniqtype_index_node to_find = {.site = allocsite};
	TYPESINDEX_RLOCK;
	const struct itree_node *res = itree_find(
		&uniqtype_index, uniqtype_index, &to_find, uniqtype_index_compare
	);
	TYPESINDEX_UNLOCK;

	/* Logic: if we've set this to null before, then a node will be present
	 * with null data. Otherwise pass back our magic uniqtype so libcrunch
	 * knows to notify unset type */
	if (!res) return &unset__uniqtype__;
	if (!res->data) return NULL;
	struct uniqtype *type = (struct uniqtype *)
		((struct uniqtype_index_node *) res->data)->type;
	return type;
}

liballocs_err_t extract_and_output_alloc_site_and_type(
	struct insert *p_ins,
	struct uniqtype **out_type,
	void **out_site
) {
	PRINTD2(
		"extract_and_..: flag %u, site %p",
		p_ins->alloc_site_flag,
		p_ins->alloc_site
	);
	if (!p_ins) {
		++__liballocs_aborted_unindexed_heap;
		return &__liballocs_err_unindexed_heap_object;
	}
	/* void *alloc_site_addr = (void *) ((uintptr_t) p_ins->alloc_site); */

	/* Now we have a uniqtype or an allocsite. For long-lived objects
	 * the uniqtype will have been installed in the heap header already.
	 * This is the expected case.
	 */
	struct uniqtype *alloc_uniqtype;
	if (__builtin_expect(p_ins->alloc_site_flag, 1)) {
		if (out_site) *out_site = NULL;
		/* Clear the low-order bit, which is available as an extra flag
		 * bit. libcrunch uses this to track whether an object is "loose"
		 * or not. Loose objects have approximate type info that might be
		 * "refined" later, typically e.g. from __PTR_void to __PTR_T. */
		alloc_uniqtype = (struct uniqtype *)((uintptr_t)(p_ins->alloc_site) & ~0x1ul);
	}
	else {
		/* Look up the allocsite's uniqtype, and install it in the heap info
		 * (on NDEBUG builds only, because it reduces debuggability a bit). */
		uintptr_t alloc_site_addr = p_ins->alloc_site;
		void *alloc_site = (void*) alloc_site_addr;
		if (out_site) *out_site = alloc_site;
		alloc_uniqtype = allocsite_to_uniqtype(alloc_site/*, p_ins*/);

		/* Remember the unrecog'd alloc sites we see. */
		// TODO, this is caching?
		/* if (!alloc_uniqtype && alloc_site && */
		/* 	!__liballocs_addrlist_contains( */
		/* 		&__liballocs_unrecognised_heap_alloc_sites, */
		/* 		alloc_site */
		/* 	) */
		/* ) { */
		/* 	__liballocs_addrlist_add(&__liballocs_unrecognised_heap_alloc_sites, alloc_site); */
		/* } */
#undef NDEBUG // FIXME realloc wrapper needs a fix for not having the alloc
			  // site if this is used
#ifdef NDEBUG
		// install it for future lookups
		// FIXME: make this atomic using a union
		// Is this in a loose state? NO. We always make it strict.
		// The client might override us by noticing that we return
		// it a dynamically-sized alloc with a uniqtype.
		// This means we're the first query to rewrite the alloc site,
		// and is the client's queue to go poking in the insert.
		if (alloc_uniqtype) {
			// Only doing this if not null because of inserting type from first
			// check in libcrunch
			p_ins->alloc_site_flag = 1;
			p_ins->alloc_site = (uintptr_t) alloc_uniqtype /* | 0x0ul */;
		}
#endif
	}

	// if we didn't get an alloc uniqtype, we abort
	if (!alloc_uniqtype)
	{
		++__liballocs_aborted_unrecognised_allocsite;

		/* We used to do this in clear_alloc_site_metadata in libcrunch...
		 * In cases where heap classification failed, we null out the allocsite
		 * to avoid repeated searching. We only do this for non-debug
		 * builds because it makes debugging a bit harder.
		 * NOTE that we don't want the insert to look like a deep-index
		 * terminator, so we set the flag.
		 */
		if (p_ins)
		{
	#ifdef NDEBUG
			/* TODO this breaks insert type of first check in is_a_internal */
			/* p_ins->alloc_site_flag = 1; */
			/* p_ins->alloc_site = 0; */
	#endif
			// Not sure if this makes sense in kernel
			/* assert(INSERT_DESCRIBES_OBJECT(p_ins)); */
			// The definition of below is that both of alloc_site and
			// alloc_site_flag must be null, which can't be true given the
			// above lines, right?
			/* assert(!INSERT_IS_NULL(p_ins)); */
		}
			
		return &__liballocs_err_unrecognised_alloc_site;;
	}
	// else output it
	if (out_type) *out_type = alloc_uniqtype;
	
	/* return success */
	return NULL;
}
