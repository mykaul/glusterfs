/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#ifndef _IOBUF_H_
#define _IOBUF_H_

#include <stddef.h>  // for size_t
#include <sys/mman.h>
#include "glusterfs/atomic.h"   // for gf_atomic_t
#include <sys/uio.h>            // for struct iovec
#include "glusterfs/locking.h"  // for gf_lock_t
#include "glusterfs/list.h"

#define IOBUF_ARENA_MAX_INDEX 5

/* Lets try to define the new anonymous mapping
 * flag, in case the system is still using the
 * now deprecated MAP_ANON flag.
 *
 * Also, this should ideally be in a centralized/common
 * header which can be used by other source files also.
 */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define GF_ALIGN_BUF(ptr, bound)                                               \
    ((void *)((unsigned long)(ptr + bound - 1) & (unsigned long)(~(bound - 1))))

#define GF_IOBUF_ALIGN_SIZE 4096

/* one allocatable unit for the consumers of the IOBUF API */
/* each unit hosts @page_size bytes of memory */
struct iobuf;

/* one region of memory mapped from the operating system */
/* each region MMAPs @arena_size bytes of memory */
/* each arena hosts @arena_size / @page_size IOBUFs */
struct iobuf_arena;

/* expandable and contractable pool of memory, internally broken into arenas */
struct iobuf_pool;

struct iobuf {
    gf_atomic_t ref; /* 0 == passive, >0 == active */
    void *free_ptr;  /* in case of stdalloc, this is the
                        one to be freed */
    void *ptr;       /* usable memory region by the consumer */
    int64_t slot_index;
    struct iobuf_arena *iobuf_arena;

    gf_lock_t lock; /* for ->ptr and ->ref */
};

struct iobuf_arena {
    union {
        struct list_head list;
        struct {
            struct iobuf_arena *next;
            struct iobuf_arena *prev;
        };
    };

    uint64_t lower_slots;
    uint64_t higher_slots;
    struct iobuf_pool *iobuf_pool;
    struct iobuf *iobufs; /* allocated iobufs list */
    uint32_t page_size;   /* size of all iobufs in this arena */
    /* this is equal to rounded_size * num_iobufs.
       (rounded_size comes with gf_iobuf_get_pagesize().) */
    uint32_t arena_size;

    uint64_t alloc_cnt; /* total allocs in this pool */
    void *mem_base;
};

struct iobuf_pool {
    pthread_mutex_t mutex;
    uint32_t default_page_size; /* default size of iobuf */
    uint32_t arena_cnt;
    struct list_head arenas[IOBUF_ARENA_MAX_INDEX];
    /* array of arenas. Each element of the array is a list of arenas
       holding iobufs of particular page_size */
    //uint32_t gf_iobuf_init_config[IOBUF_ARENA_MAX_INDEX];
};

struct iobuf_pool *
iobuf_pool_new(void);
void
iobuf_pool_destroy(struct iobuf_pool *iobuf_pool);
struct iobuf *
iobuf_get(struct iobuf_pool *iobuf_pool);
void
iobuf_unref(struct iobuf *iobuf);
struct iobuf *
iobuf_ref(struct iobuf *iobuf);
void
iobuf_to_iovec(struct iobuf *iob, struct iovec *iov);

#define iobuf_ptr(iob) ((iob)->ptr)
#define iobuf_pagesize(iob)                                                    \
    (iob->slot_index >= 0 ? iob->iobuf_arena->page_size : -(iob->slot_index))

struct iobref {
    gf_lock_t lock;
    gf_atomic_t ref;
    struct iobuf **iobrefs;
    int allocated;
    int used;
};

struct iobref *
iobref_new(void);
struct iobref *
iobref_ref(struct iobref *iobref);
void
iobref_unref(struct iobref *iobref);
int
iobref_add(struct iobref *iobref, struct iobuf *iobuf);
int
iobref_merge(struct iobref *to, struct iobref *from);
void
iobref_clear(struct iobref *iobref);

size_t
iobuf_size(struct iobuf *iobuf);
size_t
iobref_size(struct iobref *iobref);
void
iobuf_stats_dump(struct iobuf_pool *iobuf_pool, const char *name);

struct iobuf *
iobuf_get2(struct iobuf_pool *iobuf_pool, size_t page_size);

struct iobuf *
iobuf_get_page_aligned(struct iobuf_pool *iobuf_pool, size_t page_size,
                       size_t align_size);

int
iobuf_copy(struct iobuf_pool *iobuf_pool, const struct iovec *iovec_src,
           int iovcnt, struct iobref **iobref, struct iobuf **iobuf,
           struct iovec *iov_dst);

#endif /* !_IOBUF_H_ */
