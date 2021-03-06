#ifndef LIBCRUNCH_H
#define LIBCRUNCH_H

#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/lock.h>
#include <sys/rwlock.h>
#include <sys/mutex.h>

#ifndef NULL
#define NULL 0
#endif

void *__real_malloc(unsigned long size, struct malloc_type *type, int flags);

/* Debug printouts
 * 1 will cause issues if run in any part of the kernel which could cause a loop
 * 2 is only for userspace testing
 * otherwise we have no debug printouts
*/
#if DEBUG_STUBS == 1
  #include <sys/ktr.h>
  #define PRINTD(x) CTR0(KTR_PTRACE, x)
  #define PRINTD1(f, x) CTR1(KTR_PTRACE, f, x)
  #define PRINTD2(f, x1, x2) CTR2(KTR_PTRACE, f, x1, x2)
#elif DEBUG_STUBS == 2
  #include <stdio.h>
  #define PRINTD(x) printf(x "\n")
  #define PRINTD1(f, x) printf(f "\n", x)
  #define PRINTD2(f, x1, x2) printf(f "\n", x1, x2)
#else
  #define PRINTD(x) if(0 && x)
  #define PRINTD1(f, x) if(0 && f && x)
  #define PRINTD2(f, x1, x2) if(0 && f && x1 && x2)
#endif

/* Simply converting the existing debug calls for now, want to avoid calling
 * string operations in kernel */
#define __libcrunch_debug_level 999
#define debug_printf(lvl, fmt, ...) do { \
    if ((lvl) <= __libcrunch_debug_level) { \
      PRINTD(#lvl fmt #__VA_ARGS__); \
    } \
  } while (0)


/* libcrunch_cil_inlines.h */
#ifndef unlikely
#define __libcrunch_defined_unlikely
#define unlikely(cond) (__builtin_expect( (cond), 0 ))
#endif
#ifndef likely
#define __libcrunch_defined_likely
#define likely(cond)   (__builtin_expect( (cond), 1 ))
#endif

/* All the locks */
/* pageindex */
#ifdef _KERNEL
  extern struct mtx pageindex_mutex;
  #define PAGEINDEX_RLOCK \
	if (!mtx_initialized(&pageindex_mutex)) \
		mtx_init(&pageindex_mutex, "pageindex_lock", NULL, MTX_SPIN); \
	mtx_lock_spin(&pageindex_mutex)
  #define PAGEINDEX_WLOCK \
	if (!mtx_initialized(&pageindex_mutex)) \
		mtx_init(&pageindex_mutex, "pageindex_lock", NULL, MTX_SPIN); \
	mtx_lock_spin(&pageindex_mutex)
  #define PAGEINDEX_UNLOCK mtx_unlock_spin(&pageindex_mutex)
#else
  #define PAGEINDEX_RLOCK
  #define PAGEINDEX_WLOCK
  #define PAGEINDEX_UNLOCK
#endif
/* heapindex */
#ifdef _KERNEL
  extern struct mtx heapindex_mutex;
  #define HEAPINDEX_RLOCK \
	if (!mtx_initialized(&heapindex_mutex)) \
		mtx_init(&heapindex_mutex, "heapindex_lock", NULL, MTX_SPIN); \
	mtx_lock_spin(&heapindex_mutex)
  #define HEAPINDEX_WLOCK \
	if (!mtx_initialized(&heapindex_mutex)) \
		mtx_init(&heapindex_mutex, "heapindex_lock", NULL, MTX_SPIN); \
	mtx_lock_spin(&heapindex_mutex)
  #define HEAPINDEX_UNLOCK mtx_unlock_spin(&heapindex_mutex)
#else
  #define HEAPINDEX_RLOCK
  #define HEAPINDEX_WLOCK
  #define HEAPINDEX_UNLOCK
#endif
/* typesindex */
#define TYPESINDEX_RLOCK PAGEINDEX_RLOCK
#define TYPESINDEX_WLOCK PAGEINDEX_WLOCK
#define TYPESINDEX_UNLOCK PAGEINDEX_UNLOCK

extern unsigned long int __libcrunch_failed_liballocs_err;
extern unsigned long int __libcrunch_called_before_init;
extern unsigned long int __libcrunch_splaying_on;
extern unsigned long int __libcrunch_pageindex_entries;
extern unsigned long int __libcrunch_uniqtype_entries;
extern unsigned long int __libcrunch_malloc_entries;
extern unsigned long int __libcrunch_static_entries;

#endif /* LIBCRUNCH_H */
