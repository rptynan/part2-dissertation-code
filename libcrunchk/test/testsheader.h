/* Due to some funky linking of the kernel malloc in the libcrunchk code and
 * the userspace malloc, this definition is included in all the tests (which
 * are userspace) so they don't get confused. The exception is tests internal
 * to libcrunchk, e.g. testing the index_tree.
*/
#if IS_INTERNAL_TEST == 1
void *malloc(unsigned long size, struct malloc_type *type, int flags);
#else
void *malloc(unsigned long size);
#endif
