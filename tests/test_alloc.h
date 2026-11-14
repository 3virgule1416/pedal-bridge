/*
 * Interception de malloc & cie pour compter les allocations pendant run().
 * À inclure dans UN SEUL fichier par exécutable, lié avec -rdynamic pour que
 * le .so chargé par dlopen utilise aussi ces fonctions.
 */
#ifndef PB_TEST_ALLOC_H
#define PB_TEST_ALLOC_H

#include <errno.h>
#include <stddef.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);
extern void  __libc_free(void *);

static volatile int  g_in_rt;
static volatile long g_rt_allocs;

void *malloc(size_t n)            { if (g_in_rt) g_rt_allocs++; return __libc_malloc(n); }
void *calloc(size_t a, size_t b)  { if (g_in_rt) g_rt_allocs++; return __libc_calloc(a, b); }
void *realloc(void *p, size_t n)  { if (g_in_rt) g_rt_allocs++; return __libc_realloc(p, n); }
void  free(void *p)               { if (g_in_rt) g_rt_allocs++; __libc_free(p); }
void *memalign(size_t a, size_t n){ if (g_in_rt) g_rt_allocs++; return __libc_memalign(a, n); }
void *aligned_alloc(size_t a, size_t n) { return memalign(a, n); }
int posix_memalign(void **p, size_t a, size_t n)
{ *p = memalign(a, n); return *p ? 0 : ENOMEM; }

#endif
