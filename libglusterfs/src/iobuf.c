/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#include "glusterfs/iobuf.h"
#include "glusterfs/statedump.h"
#include <stdio.h>
#include "glusterfs/libglusterfs-messages.h"

/*
  TODO: implement destroy margins and prefetching of arenas
*/

#define NUM_IOBUFS 64
#define DEFAULT_PAGE_SIZE (128 * GF_UNIT_KB)
/* Make sure this array is sorted based on pagesize */
static const uint32_t gf_iobuf_init_config[IOBUF_ARENA_MAX_INDEX] = {
    64,
    128,
    256,
    512,
    2 * GF_UNIT_KB,
    8 * GF_UNIT_KB,
    32 * GF_UNIT_KB,
    DEFAULT_PAGE_SIZE,
    256 * GF_UNIT_KB,
};

static int32_t
gf_iobuf_get_arena_index(const uint32_t page_size)
{
    int32_t i;

    for (i = 0; i < IOBUF_ARENA_MAX_INDEX; i++) {
        if (page_size <= gf_iobuf_init_config[i])
            return i;
    }

    return -1;
}

static int32_t
gf_iobuf_get_pagesize(const uint32_t page_size, uint32_t *index)
{
    uint32_t i;
    uint32_t size;

    for (i = 0; i < IOBUF_ARENA_MAX_INDEX; i++) {
        size = gf_iobuf_init_config[i];
        if (page_size <= size) {
            if (index != NULL)
                *index = i;
            return size;
        }
    }

    return -1;
}

static gf_boolean_t
__iobuf_arena_init_iobufs(struct iobuf_arena *iobuf_arena)
{
    struct iobuf *iobuf = NULL;
    int offset = 0;
    int i;

    iobuf_arena->iobufs = GF_MALLOC(sizeof(*iobuf) * NUM_IOBUFS,
                                    gf_common_mt_iobuf);
    if (caa_unlikely(!iobuf_arena->iobufs))
        return _gf_false;

    iobuf = iobuf_arena->iobufs;
    for (i = 0; i < NUM_IOBUFS; i++) {
        GF_ATOMIC_INIT(iobuf->ref, 0);
        iobuf->iobuf_arena = iobuf_arena;

        iobuf->free_ptr = NULL;
        iobuf->ptr = iobuf_arena->mem_base + offset;
        LOCK_INIT(&iobuf->lock);

        offset += iobuf_arena->page_size;
        iobuf++;
    }

    return _gf_true;
}

static void
__iobuf_arena_destroy_iobufs(struct iobuf_arena *iobuf_arena)
{
    int iobuf_cnt;
    struct iobuf *iobuf = NULL;
    int i;

    iobuf = iobuf_arena->iobufs;
    if (!iobuf) {
        gf_msg_callingfn(THIS->name, GF_LOG_ERROR, 0, LG_MSG_IOBUFS_NOT_FOUND,
                         "iobufs not found");
        return;
    }

    for (i = 0; i < NUM_IOBUFS; i++) {
        GF_ASSERT(GF_ATOMIC_GET(iobuf->ref) == 0);

        LOCK_DESTROY(&iobuf->lock);
        iobuf++;
    }

    GF_FREE(iobuf_arena->iobufs);
}

static void
__iobuf_arena_destroy(struct iobuf_arena *iobuf_arena)
{
    __iobuf_arena_destroy_iobufs(iobuf_arena);

    if (iobuf_arena->mem_base && iobuf_arena->mem_base != MAP_FAILED)
        munmap(iobuf_arena->mem_base, iobuf_arena->arena_size);

    GF_FREE(iobuf_arena);
}

static struct iobuf_arena *
__iobuf_arena_alloc(struct iobuf_pool *iobuf_pool, const uint32_t rounded_size)
{
    struct iobuf_arena *iobuf_arena = NULL;

    iobuf_arena = GF_MALLOC(sizeof(*iobuf_arena), gf_common_mt_iobuf_arena);
    if (caa_unlikely(!iobuf_arena))
        goto out;

    INIT_LIST_HEAD(&iobuf_arena->list);
    iobuf_arena->page_size = rounded_size;
    iobuf_arena->arena_size = rounded_size * NUM_IOBUFS;
    iobuf_arena->iobuf_pool = iobuf_pool;
    iobuf_arena->mem_base = mmap(NULL, iobuf_arena->arena_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (iobuf_arena->mem_base == MAP_FAILED) {
        gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_MAPPING_FAILED, NULL);
        goto err;
    }
    iobuf_arena->lower_slots = ~0UL;

    iobuf_arena->alloc_cnt = 0;

    if (!__iobuf_arena_init_iobufs(iobuf_arena)) {
        gf_smsg(THIS->name, GF_LOG_ERROR, 0, LG_MSG_INIT_IOBUF_FAILED, NULL);
        goto err;
    }

    iobuf_pool->arena_cnt++;

    return iobuf_arena;

err:
    __iobuf_arena_destroy(iobuf_arena);

out:
    return NULL;
}

static struct iobuf_arena *
__iobuf_pool_add_arena(struct iobuf_pool *iobuf_pool,
                       const uint32_t rounded_size, const uint32_t index)
{
    struct iobuf_arena *iobuf_arena = NULL;

    iobuf_arena = __iobuf_arena_alloc(iobuf_pool, rounded_size);
    if (!iobuf_arena) {
        gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_ARENA_NOT_FOUND, NULL);
        return NULL;
    }
    list_add(&iobuf_arena->list, &iobuf_pool->arenas[index]);

    return iobuf_arena;
}

/* This function destroys all the iobufs and the iobuf_pool */
void
iobuf_pool_destroy(struct iobuf_pool *iobuf_pool)
{
    struct iobuf_arena *iobuf_arena = NULL;
    struct iobuf_arena *tmp = NULL;
    int i = 0;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf_pool, out);

    pthread_mutex_lock(&iobuf_pool->mutex);
    {
        for (i = 0; i < IOBUF_ARENA_MAX_INDEX; i++) {
            list_for_each_entry_safe(iobuf_arena, tmp, &iobuf_pool->arenas[i],
                                     list)
            {
                list_del_init(&iobuf_arena->list);
                __iobuf_arena_destroy(iobuf_arena);
            }
        }
    }
    pthread_mutex_unlock(&iobuf_pool->mutex);

    pthread_mutex_destroy(&iobuf_pool->mutex);

    GF_FREE(iobuf_pool);

out:
    return;
}

struct iobuf_pool *
iobuf_pool_new(void)
{
    struct iobuf_pool *iobuf_pool = NULL;
    int i;
    uint32_t page_size;
    int32_t rounded_size;

    iobuf_pool = GF_CALLOC(sizeof(*iobuf_pool), 1, gf_common_mt_iobuf_pool);
    if (caa_unlikely(!iobuf_pool))
        goto out;
    pthread_mutex_init(&iobuf_pool->mutex, NULL);
    for (i = 0; i < IOBUF_ARENA_MAX_INDEX; i++) {
        INIT_LIST_HEAD(&iobuf_pool->arenas[i]);
    }

    iobuf_pool->default_page_size = DEFAULT_PAGE_SIZE;

    /* No locking required here
     * as no one else can use this pool yet
     */
    for (i = 0; i < IOBUF_ARENA_MAX_INDEX; i++) {
        page_size = gf_iobuf_init_config[i];
        rounded_size = gf_iobuf_get_pagesize(page_size, NULL);

        __iobuf_pool_add_arena(iobuf_pool, rounded_size, i);
    }

out:

    return iobuf_pool;
}

/* Always called under the iobuf_pool mutex lock */
static struct iobuf_arena *
__iobuf_select_arena(struct iobuf_pool *iobuf_pool, const uint32_t page_size,
                     const uint32_t index)
{
    struct iobuf_arena *iobuf_arena = NULL;
    int32_t rounded_size;

    /* look for unused iobuf from the head-most arena */
    list_for_each_entry(iobuf_arena, &iobuf_pool->arenas[index], list)
    {
        if (iobuf_arena->lower_slots) {
            return iobuf_arena;
            break;
        }
    }

    /* all arenas were full, find the right count to add */
    rounded_size = gf_iobuf_get_pagesize(page_size, NULL);
    iobuf_arena = __iobuf_pool_add_arena(iobuf_pool, rounded_size, index);

    return iobuf_arena;
}

/* Always called under the iobuf_pool mutex lock */
static struct iobuf *
__iobuf_get(struct iobuf_pool *iobuf_pool, const uint32_t page_size,
            const uint32_t index)
{
    struct iobuf *iobuf = NULL;
    struct iobuf_arena *iobuf_arena = NULL;
    int32_t slot_index;

    /* most eligible arena for picking an iobuf */
    iobuf_arena = __iobuf_select_arena(iobuf_pool, page_size, index);
    if (caa_unlikely(!iobuf_arena))
        return NULL;

    slot_index = gf_bits_index(iobuf_arena->lower_slots);
    iobuf = &(iobuf_arena->iobufs[slot_index]);
    iobuf->slot_index = slot_index;
    iobuf_arena->lower_slots &= (~(1 << slot_index));

    /* no resetting requied for this element */
    iobuf_arena->alloc_cnt++;

    return iobuf;
}

static struct iobuf *
iobuf_get_from_stdalloc(const size_t page_size)
{
    struct iobuf *iobuf = NULL;

    iobuf = GF_MALLOC(sizeof(*iobuf), gf_common_mt_iobuf);
    if (caa_unlikely(!iobuf))
        goto out;

    /* Hold a ref because you are allocating and using it */
    GF_ATOMIC_INIT(iobuf->ref, 1);
    iobuf->slot_index = -1;  // means stdalloc
    /* 4096 is the alignment */
    iobuf->free_ptr = GF_MALLOC(((page_size + GF_IOBUF_ALIGN_SIZE) - 1),
                                gf_common_mt_char);
    if (caa_unlikely(!iobuf->free_ptr))
        goto out;

    iobuf->ptr = GF_ALIGN_BUF(iobuf->free_ptr, GF_IOBUF_ALIGN_SIZE);
    LOCK_INIT(&iobuf->lock);

    return iobuf;
out:
    if (iobuf) {
        GF_FREE(iobuf->free_ptr);
        GF_FREE(iobuf);
        iobuf = NULL;
    }

    return iobuf;
}

struct iobuf *
iobuf_get2(struct iobuf_pool *iobuf_pool, size_t page_size)
{
    struct iobuf *iobuf = NULL;
    int32_t rounded_size;
    uint32_t index = 0;

    if (caa_unlikely(page_size == 0)) {
        page_size = iobuf_pool->default_page_size;
    }

    rounded_size = gf_iobuf_get_pagesize(page_size, &index);
    if (caa_unlikely(rounded_size < 0)) {
        /* make sure to provide the requested buffer with standard
           memory allocations */
        iobuf = iobuf_get_from_stdalloc(page_size);

        gf_msg_debug("iobuf", 0,
                     "request for iobuf of size %zu "
                     "is serviced using standard calloc() (%p) as it "
                     "exceeds the maximum available buffer size",
                     page_size, iobuf);

        iobuf_pool->request_misses++;
        goto out;
    }

    pthread_mutex_lock(&iobuf_pool->mutex);
    {
        iobuf = __iobuf_get(iobuf_pool, page_size, index);
    }
    pthread_mutex_unlock(&iobuf_pool->mutex);
    if (caa_unlikely(!iobuf)) {
        gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_IOBUF_NOT_FOUND, NULL);
        goto out;
    }

    GF_ATOMIC_INC(iobuf->ref);

out:
    return iobuf;
}

struct iobuf *
iobuf_get_page_aligned(struct iobuf_pool *iobuf_pool, size_t page_size,
                       size_t align_size)
{
    size_t req_size;
    struct iobuf *iobuf = NULL;

    if (caa_unlikely(page_size == 0)) {
        req_size = iobuf_pool->default_page_size;
    } else
        req_size = page_size;

    iobuf = iobuf_get2(iobuf_pool, req_size + align_size);
    if (caa_unlikely(!iobuf))
        return NULL;
    /* If std allocation was used, then free_ptr will be non-NULL. In this
     * case, we do not want to modify the original free_ptr.
     * On the other hand, if the buf was gotten through the available
     * arenas, then we use iobuf->free_ptr to store the original
     * pointer to the offset into the mmap'd block of memory and in turn
     * reuse iobuf->ptr to hold the page-aligned address. And finally, in
     * iobuf_put(), we copy iobuf->free_ptr into iobuf->ptr - back to where
     * it was originally when __iobuf_get() returned this iobuf.
     */
    if (!iobuf->free_ptr)
        iobuf->free_ptr = iobuf->ptr;
    iobuf->ptr = GF_ALIGN_BUF(iobuf->ptr, align_size);

    return iobuf;
}

struct iobuf *
iobuf_get(struct iobuf_pool *iobuf_pool)
{
    struct iobuf *iobuf = NULL;
    int32_t index;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf_pool, out);

    index = gf_iobuf_get_arena_index(iobuf_pool->default_page_size);
    if (caa_unlikely(index < 0)) {
        gf_smsg("iobuf", GF_LOG_ERROR, 0, LG_MSG_PAGE_SIZE_EXCEEDED,
                "page_size=%zu", iobuf_pool->default_page_size, NULL);
        return NULL;
    }

    pthread_mutex_lock(&iobuf_pool->mutex);
    {
        iobuf = __iobuf_get(iobuf_pool, iobuf_pool->default_page_size, index);
    }
    pthread_mutex_unlock(&iobuf_pool->mutex);
    if (caa_unlikely(!iobuf)) {
        gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_IOBUF_NOT_FOUND, NULL);
        goto out;
    }

    GF_ATOMIC_INC(iobuf->ref);

out:
    return iobuf;
}

void
iobuf_put(struct iobuf *iobuf)
{
    struct iobuf_arena *iobuf_arena = NULL;
    struct iobuf_pool *iobuf_pool = NULL;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);

    if (caa_unlikely(iobuf->slot_index < 0)) {
        gf_msg_debug("iobuf", 0,
                     "freeing the iobuf (%p) "
                     "allocated with standard calloc()",
                     iobuf);

        /* free up properly without bothering about lists and all */
        LOCK_DESTROY(&iobuf->lock);
        GF_FREE(iobuf->free_ptr);
        GF_FREE(iobuf);
        return;
    }

    iobuf_arena = iobuf->iobuf_arena;
    if (caa_unlikely(!iobuf_arena)) {
        gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_ARENA_NOT_FOUND, NULL);
        return;
    }

    iobuf_pool = iobuf_arena->iobuf_pool;
    if (caa_unlikely(!iobuf_pool)) {
        gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_POOL_NOT_FOUND, "iobuf",
                NULL);
        return;
    }

    if (iobuf->free_ptr) {
        iobuf->ptr = iobuf->free_ptr;
        iobuf->free_ptr = NULL;
    }

    pthread_mutex_lock(&iobuf_pool->mutex);
    {
        iobuf_arena->lower_slots |= (1 << iobuf->slot_index);
    }
    pthread_mutex_unlock(&iobuf_pool->mutex);

out:
    return;
}

void
iobuf_unref(struct iobuf *iobuf)
{
    int ref;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);

    ref = GF_ATOMIC_DEC(iobuf->ref);

    if (!ref)
        iobuf_put(iobuf);

out:
    return;
}

struct iobuf *
iobuf_ref(struct iobuf *iobuf)
{
    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);
    GF_ATOMIC_INC(iobuf->ref);

out:
    return iobuf;
}

struct iobref *
iobref_new()
{
    struct iobref *iobref = NULL;

    iobref = GF_MALLOC(sizeof(*iobref), gf_common_mt_iobref);
    if (!iobref)
        return NULL;

    iobref->iobrefs = GF_CALLOC(sizeof(*iobref->iobrefs), 16,
                                gf_common_mt_iobrefs);
    if (!iobref->iobrefs) {
        GF_FREE(iobref);
        return NULL;
    }

    LOCK_INIT(&iobref->lock);

    GF_ATOMIC_INIT(iobref->ref, 1);
    iobref->allocated = 16;
    iobref->used = 0;
    return iobref;
}

struct iobref *
iobref_ref(struct iobref *iobref)
{
    GF_VALIDATE_OR_GOTO("iobuf", iobref, out);
    GF_ATOMIC_INC(iobref->ref);

out:
    return iobref;
}

static void
iobref_destroy(struct iobref *iobref)
{
    int i;
    struct iobuf *iobuf = NULL;

    for (i = 0; i < iobref->allocated; i++) {
        iobuf = iobref->iobrefs[i];

        iobref->iobrefs[i] = NULL;
        if (iobuf)
            iobuf_unref(iobuf);
    }

    GF_FREE(iobref->iobrefs);
    GF_FREE(iobref);

out:
    return;
}

void
iobref_unref(struct iobref *iobref)
{
    int ref;

    GF_VALIDATE_OR_GOTO("iobuf", iobref, out);
    ref = GF_ATOMIC_DEC(iobref->ref);

    if (!ref)
        iobref_destroy(iobref);

out:
    return;
}

void
iobref_clear(struct iobref *iobref)
{
    int i = 0;

    GF_VALIDATE_OR_GOTO("iobuf", iobref, out);

    for (; i < iobref->allocated; i++) {
        if (iobref->iobrefs[i] != NULL) {
            iobuf_unref(iobref->iobrefs[i]);
        } else {
            /** iobuf's are attached serially */
            break;
        }
    }

    iobref_unref(iobref);

out:
    return;
}

static void
__iobref_grow(struct iobref *iobref)
{
    void *newptr = NULL;
    int i;

    newptr = GF_REALLOC(iobref->iobrefs,
                        iobref->allocated * 2 * (sizeof(*iobref->iobrefs)));
    if (newptr) {
        iobref->iobrefs = newptr;
        iobref->allocated *= 2;

        for (i = iobref->used; i < iobref->allocated; i++)
            iobref->iobrefs[i] = NULL;
    }
}

static int
__iobref_add(struct iobref *iobref, struct iobuf *iobuf)
{
    int i;

    if (iobref->used == iobref->allocated) {
        __iobref_grow(iobref);

        if (iobref->used == iobref->allocated) {
            return -ENOMEM;
        }
    }

    for (i = 0; i < iobref->allocated; i++) {
        if (iobref->iobrefs[i] == NULL) {
            iobref->iobrefs[i] = iobuf_ref(iobuf);
            iobref->used++;
            break;
        }
    }

    return 0;
}

int
iobref_add(struct iobref *iobref, struct iobuf *iobuf)
{
    int ret;

    GF_VALIDATE_OR_GOTO("iobuf", iobref, out);
    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);

    LOCK(&iobref->lock);
    {
        ret = __iobref_add(iobref, iobuf);
    }
    UNLOCK(&iobref->lock);

    return ret;
out:
    return -EINVAL;
}

int
iobref_merge(struct iobref *to, struct iobref *from)
{
    int i = 0;
    int ret = 0;
    struct iobuf *iobuf = NULL;

    GF_VALIDATE_OR_GOTO("iobuf", to, out);
    GF_VALIDATE_OR_GOTO("iobuf", from, out);

    LOCK(&from->lock);
    {
        for (i = 0; i < from->allocated; i++) {
            iobuf = from->iobrefs[i];

            if (!iobuf)
                break;

            ret = iobref_add(to, iobuf);

            if (ret < 0)
                break;
        }
    }
    UNLOCK(&from->lock);

out:
    return ret;
}

size_t
iobuf_size(struct iobuf *iobuf)
{
    size_t size = 0;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);

    if (!iobuf->iobuf_arena) {
        gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_ARENA_NOT_FOUND, NULL);
        goto out;
    }

    if (!iobuf->iobuf_arena->iobuf_pool) {
        gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_POOL_NOT_FOUND, NULL);
        goto out;
    }

    size = iobuf->iobuf_arena->page_size;
out:
    return size;
}

size_t
iobref_size(struct iobref *iobref)
{
    size_t size = 0;
    int i;

    GF_VALIDATE_OR_GOTO("iobuf", iobref, out);

    LOCK(&iobref->lock);
    {
        for (i = 0; i < iobref->allocated; i++) {
            if (iobref->iobrefs[i])
                size += iobuf_size(iobref->iobrefs[i]);
        }
    }
    UNLOCK(&iobref->lock);

out:
    return size;
}

void
iobuf_info_dump(struct iobuf *iobuf, const char *key_prefix)
{
    char key[GF_DUMP_MAX_BUF_LEN];
    struct iobuf my_iobuf;
    int ret;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);

    ret = TRY_LOCK(&iobuf->lock);
    if (ret) {
        return;
    }
    memcpy(&my_iobuf, iobuf, sizeof(my_iobuf));
    UNLOCK(&iobuf->lock);

    gf_proc_dump_build_key(key, key_prefix, "ref");
    gf_proc_dump_write(key, "%" GF_PRI_ATOMIC, GF_ATOMIC_GET(my_iobuf.ref));
    gf_proc_dump_build_key(key, key_prefix, "ptr");
    gf_proc_dump_write(key, "%p", my_iobuf.ptr);

out:
    return;
}

static void
iobuf_arena_info_dump(struct iobuf_arena *iobuf_arena, const char *key_prefix)
{
    char key[GF_DUMP_MAX_BUF_LEN];
    int i = 1;
    struct iobuf *trav;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf_arena, out);

    gf_proc_dump_build_key(key, key_prefix, "mem_base");
    gf_proc_dump_write(key, "%p", iobuf_arena->mem_base);
    gf_proc_dump_build_key(key, key_prefix, "alloc_cnt");
    gf_proc_dump_write(key, "%" PRIu64, iobuf_arena->alloc_cnt);
    gf_proc_dump_build_key(key, key_prefix, "page_size");
    gf_proc_dump_write(key, "%u", iobuf_arena->page_size);

out:
    return;
}

void
iobuf_stats_dump(struct iobuf_pool *iobuf_pool)
{
    char msg[1024];
    struct iobuf_arena *trav = NULL;
    int i = 1;
    int j = 0;
    int ret = -1;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf_pool, out);

    ret = pthread_mutex_trylock(&iobuf_pool->mutex);

    if (ret) {
        return;
    }
    gf_proc_dump_add_section("iobuf.global");
    gf_proc_dump_write("iobuf_pool", "%p", iobuf_pool);
    gf_proc_dump_write("iobuf_pool.default_page_size", "%u",
                       iobuf_pool->default_page_size);
    gf_proc_dump_write("iobuf_pool.arena_cnt", "%u", iobuf_pool->arena_cnt);
    gf_proc_dump_write("iobuf_pool.request_misses", "%" PRId64,
                       iobuf_pool->request_misses);

    for (j = 0; j < IOBUF_ARENA_MAX_INDEX; j++) {
        list_for_each_entry(trav, &iobuf_pool->arenas[j], list)
        {
            snprintf(msg, sizeof(msg), "arena.%d", i);
            gf_proc_dump_add_section("%s", msg);
            iobuf_arena_info_dump(trav, msg);
            i++;
        }
    }

    pthread_mutex_unlock(&iobuf_pool->mutex);

out:
    return;
}

void
iobuf_to_iovec(struct iobuf *iob, struct iovec *iov)
{
    GF_VALIDATE_OR_GOTO("iobuf", iob, out);
    GF_VALIDATE_OR_GOTO("iobuf", iov, out);

    iov->iov_base = iobuf_ptr(iob);
    iov->iov_len = iobuf_pagesize(iob);

out:
    return;
}

int
iobuf_copy(struct iobuf_pool *iobuf_pool, const struct iovec *iovec_src,
           int iovcnt, struct iobref **iobref, struct iobuf **iobuf,
           struct iovec *iov_dst)
{
    size_t size = -1;
    int ret = 0;

    size = iov_length(iovec_src, iovcnt);

    *iobuf = iobuf_get2(iobuf_pool, size);
    if (!(*iobuf)) {
        ret = -1;
        errno = ENOMEM;
        goto out;
    }

    *iobref = iobref_new();
    if (!(*iobref)) {
        iobuf_unref(*iobuf);
        errno = ENOMEM;
        ret = -1;
        goto out;
    }

    ret = iobref_add(*iobref, *iobuf);
    if (ret) {
        iobuf_unref(*iobuf);
        iobref_unref(*iobref);
        errno = ENOMEM;
        ret = -1;
        goto out;
    }

    iov_unload(iobuf_ptr(*iobuf), iovec_src, iovcnt);

    iov_dst->iov_base = iobuf_ptr(*iobuf);
    iov_dst->iov_len = size;

out:
    return ret;
}
