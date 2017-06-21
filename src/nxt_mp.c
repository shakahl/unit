
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>


/*
 * A memory pool allocates memory in clusters of specified size and aligned
 * to page_alignment.  A cluster is divided on pages of specified size.  Page
 * size must be a power of 2.  A page can be used entirely or can be divided
 * on chunks of equal size.  Chunk size must be a power of 2.  Non-freeable
 * memory is also allocated from pages.  A cluster can contains a mix of pages
 * with different chunk sizes and non-freeable pages.  Cluster size must be
 * a multiple of page size and may be not a power of 2.  Allocations greater
 * than page are allocated outside clusters.  Start addresses and sizes of
 * the clusters and large allocations are stored in rbtree blocks to find
 * them on free operations.  The rbtree nodes are sorted by start addresses.
 * The rbtree is also used to destroy memory pool.
 */


typedef struct {
    /*
     * Used to link
     *  *) pages with free chunks in pool chunk pages lists,
     *  *) pages with free space for non-freeable allocations,
     *  *) free pages in clusters.
     */
    nxt_queue_link_t     link;

    union {
        /* Chunk bitmap.  There can be no more than 32 chunks in a page. */
        uint32_t         map;

        /* Size of taken non-freeable space. */
        uint32_t         taken;
    } u;

    /*
     * Size of chunks or page shifted by pool->chunk_size_shift.  Zero means
     * that page is free, 0xFF means page with non-freeable allocations.
     */
    uint8_t              size;

    /* Number of free chunks of a chunked page. */
    uint8_t              chunks;

    /*
     * Number of allocation fails due to free space insufficiency
     * in non-freeable page.
     */
    uint8_t              fails;

    /*
     * Page number in page cluster.
     * There can be no more than 256 pages in a cluster.
     */
    uint8_t              number;
} nxt_mp_page_t;


/*
 * Some malloc implementations (e.g. jemalloc) allocates large enough
 * blocks (e.g. greater than 4K) with 4K alignment.  So if a block
 * descriptor will be allocated together with the block it will take
 * excessive 4K memory.  So it is better to allocate the block descriptor
 * apart.
 */

typedef enum {
    /* Block of cluster.  The block is allocated apart of the cluster. */
    NXT_MP_CLUSTER_BLOCK = 0,
    /*
     * Block of large allocation.
     * The block is allocated apart of the allocation.
     */
    NXT_MP_DISCRETE_BLOCK,
    /*
     * Block of large allocation.
     * The block is allocated just after of the allocation.
     */
    NXT_MP_EMBEDDED_BLOCK,
} nxt_mp_block_type_t;


typedef struct {
    NXT_RBTREE_NODE      (node);
    nxt_mp_block_type_t  type:8;

    /* Block size must be less than 4G. */
    uint32_t             size;

    u_char               *start;
    nxt_mp_page_t        pages[];
} nxt_mp_block_t;


struct nxt_mp_s {
    /* rbtree of nxt_mp_block_t. */
    nxt_rbtree_t         blocks;

    uint8_t              chunk_size_shift;
    uint8_t              page_size_shift;
    uint32_t             page_size;
    uint32_t             page_alignment;
    uint32_t             cluster_size;
    uint32_t             retain;

    /* Lists of nxt_mp_page_t. */
    nxt_queue_t          free_pages;
    nxt_queue_t          nget_pages;
    nxt_queue_t          get_pages;
    nxt_queue_t          chunk_pages[0];
};


#define nxt_mp_chunk_get_free(map)                                            \
    (__builtin_ffs(map) - 1)


#define nxt_mp_chunk_is_free(map, chunk)                                      \
    ((map & (1 << chunk)) != 0)


#define nxt_mp_chunk_set_busy(map, chunk)                                     \
    map &= ~(1 << chunk)


#define nxt_mp_chunk_set_free(map, chunk)                                     \
    map |= (1 << chunk)


#define nxt_mp_free_junk(p, size)                                             \
    memset((p), 0x5A, size)


#if !(NXT_DEBUG_MEMORY)
static void *nxt_mp_alloc_small(nxt_mp_t *mp, size_t size);
static void *nxt_mp_get_small(nxt_mp_t *mp, nxt_queue_t *pages, size_t size);
static nxt_mp_page_t *nxt_mp_alloc_page(nxt_mp_t *mp);
static nxt_mp_block_t *nxt_mp_alloc_cluster(nxt_mp_t *mp);
#endif
static void *nxt_mp_alloc_large(nxt_mp_t *mp, size_t alignment, size_t size);
static intptr_t nxt_mp_rbtree_compare(nxt_rbtree_node_t *node1,
    nxt_rbtree_node_t *node2);
static nxt_mp_block_t *nxt_mp_find_block(nxt_rbtree_t *tree, u_char *p);
static const char *nxt_mp_chunk_free(nxt_mp_t *mp, nxt_mp_block_t *cluster,
    u_char *p);


nxt_mp_t *
nxt_mp_create(size_t cluster_size, size_t page_alignment, size_t page_size,
    size_t min_chunk_size)
{
    nxt_mp_t     *mp;
    nxt_uint_t   pages, chunk_size;
    nxt_queue_t  *chunk_pages;

    pages = 0;
    chunk_size = page_size;

    do {
        pages++;
        chunk_size /= 2;
    } while (chunk_size > min_chunk_size);

    mp = nxt_zalloc(sizeof(nxt_mp_t) + pages * sizeof(nxt_queue_t));

    if (nxt_fast_path(mp != NULL)) {
        mp->retain = 1;
        mp->page_size = page_size;
        mp->page_alignment = nxt_max(page_alignment, NXT_MAX_ALIGNMENT);
        mp->cluster_size = cluster_size;

        chunk_pages = mp->chunk_pages;

        do {
            nxt_queue_init(chunk_pages);
            chunk_pages++;
            chunk_size *= 2;
        } while (chunk_size < page_size);

        mp->chunk_size_shift = nxt_lg2(min_chunk_size);
        mp->page_size_shift = nxt_lg2(page_size);

        nxt_rbtree_init(&mp->blocks, nxt_mp_rbtree_compare);

        nxt_queue_init(&mp->free_pages);
        nxt_queue_init(&mp->nget_pages);
        nxt_queue_init(&mp->get_pages);
    }

    return mp;
}


void
nxt_mp_destroy(nxt_mp_t *mp)
{
    void               *p;
    nxt_mp_block_t     *block;
    nxt_rbtree_node_t  *node, *next;

    nxt_debug_alloc("mp destroy");

    next = nxt_rbtree_root(&mp->blocks);

    while (next != nxt_rbtree_sentinel(&mp->blocks)) {

        node = nxt_rbtree_destroy_next(&mp->blocks, &next);
        block = (nxt_mp_block_t *) node;

        p = block->start;

        if (block->type != NXT_MP_EMBEDDED_BLOCK) {
            nxt_free(block);
        }

        nxt_free(p);
    }

    nxt_free(mp);
}


nxt_bool_t
nxt_mp_test_sizes(size_t cluster_size, size_t page_alignment, size_t page_size,
    size_t min_chunk_size)
{
    nxt_bool_t  valid;

    /* Alignment and sizes must be a power of 2. */

    valid = nxt_expect(1, (nxt_is_power_of_two(page_alignment)
                           && nxt_is_power_of_two(page_size)
                           && nxt_is_power_of_two(min_chunk_size)));
    if (!valid) {
        return 0;
    }

    page_alignment = nxt_max(page_alignment, NXT_MAX_ALIGNMENT);

    valid = nxt_expect(1, (page_size >= 64
                           && page_size >= page_alignment
                           && page_size >= min_chunk_size
                           && min_chunk_size * 32 >= page_size
                           && cluster_size >= page_size
                           && cluster_size / page_size <= 256
                           && cluster_size % page_size == 0));
    if (!valid) {
        return 0;
    }

    return 1;
}


nxt_bool_t
nxt_mp_is_empty(nxt_mp_t *mp)
{
    return (nxt_rbtree_is_empty(&mp->blocks)
            && nxt_queue_is_empty(&mp->free_pages));
}


void *
nxt_mp_alloc(nxt_mp_t *mp, size_t size)
{
    nxt_debug_alloc("mp alloc: %uz", size);

#if !(NXT_DEBUG_MEMORY)

    if (size <= mp->page_size) {
        return nxt_mp_alloc_small(mp, size);
    }

#endif

    return nxt_mp_alloc_large(mp, NXT_MAX_ALIGNMENT, size);
}


void *
nxt_mp_zalloc(nxt_mp_t *mp, size_t size)
{
    void  *p;

    p = nxt_mp_alloc(mp, size);

    if (nxt_fast_path(p != NULL)) {
        memset(p, 0, size);
    }

    return p;
}


void *
nxt_mp_align(nxt_mp_t *mp, size_t alignment, size_t size)
{
    nxt_debug_alloc("mp align: @%uz:%uz", alignment, size);

    /* Alignment must be a power of 2. */

    if (nxt_fast_path(nxt_is_power_of_two(alignment))) {

#if !(NXT_DEBUG_MEMORY)

        if (size <= mp->page_size && alignment <= mp->page_alignment) {
            size = nxt_max(size, alignment);

            if (size <= mp->page_size) {
                return nxt_mp_alloc_small(mp, size);
            }
        }

#endif

        return nxt_mp_alloc_large(mp, alignment, size);
    }

    return NULL;
}


void *
nxt_mp_zalign(nxt_mp_t *mp, size_t alignment, size_t size)
{
    void  *p;

    p = nxt_mp_align(mp, alignment, size);

    if (nxt_fast_path(p != NULL)) {
        memset(p, 0, size);
    }

    return p;
}


nxt_inline nxt_uint_t
nxt_mp_chunk_pages_index(nxt_mp_t *mp, size_t size)
{
    nxt_int_t  n, index;

    index = 0;

    if (size > 1) {
        n = nxt_lg2(size - 1) + 1 - mp->chunk_size_shift;

        if (n > 0) {
            index = n;
        }
    }

    return index;
}


#if !(NXT_DEBUG_MEMORY)

nxt_inline u_char *
nxt_mp_page_addr(nxt_mp_t *mp, nxt_mp_page_t *page)
{
    size_t          page_offset;
    nxt_mp_block_t  *block;

    page_offset = page->number * sizeof(nxt_mp_page_t)
                  + offsetof(nxt_mp_block_t, pages);

    block = (nxt_mp_block_t *) ((u_char *) page - page_offset);

    return block->start + (page->number << mp->page_size_shift);
}


static void *
nxt_mp_alloc_small(nxt_mp_t *mp, size_t size)
{
    u_char            *p;
    nxt_uint_t        n, index;
    nxt_queue_t       *chunk_pages;
    nxt_mp_page_t     *page;
    nxt_queue_link_t  *link;

    p = NULL;

    if (size <= mp->page_size / 2) {

        index = nxt_mp_chunk_pages_index(mp, size);
        chunk_pages = &mp->chunk_pages[index];

        if (nxt_fast_path(!nxt_queue_is_empty(chunk_pages))) {

            link = nxt_queue_first(chunk_pages);
            page = nxt_queue_link_data(link, nxt_mp_page_t, link);

            p = nxt_mp_page_addr(mp, page);

            n = nxt_mp_chunk_get_free(page->u.map);
            nxt_mp_chunk_set_busy(page->u.map, n);

            p += ((n << index) << mp->chunk_size_shift);

            page->chunks--;

            if (page->chunks == 0) {
                /*
                 * Remove full page from the pool chunk pages list
                 * of pages with free chunks.
                 */
                nxt_queue_remove(&page->link);
            }

        } else {
            page = nxt_mp_alloc_page(mp);

            if (nxt_fast_path(page != NULL)) {
                page->size = (1 << index);

                n = mp->page_size_shift - (index + mp->chunk_size_shift);
                page->chunks = (1 << n) - 1;

                nxt_queue_insert_head(chunk_pages, &page->link);

                /* Mark the first chunk as busy. */
                page->u.map = 0xFFFFFFFE;

                p = nxt_mp_page_addr(mp, page);
            }
        }

    } else {
        page = nxt_mp_alloc_page(mp);

        if (nxt_fast_path(page != NULL)) {
            page->size = mp->page_size >> mp->chunk_size_shift;

            p = nxt_mp_page_addr(mp, page);
        }
    }

    nxt_debug_alloc("mp chunk:%uz alloc: %p",
                    page->size << mp->chunk_size_shift, p);

    return p;
}


static void *
nxt_mp_get_small(nxt_mp_t *mp, nxt_queue_t *pages, size_t size)
{
    u_char            *p;
    uint32_t          available;
    nxt_mp_page_t     *page;
    nxt_queue_link_t  *link, *next;

    for (link = nxt_queue_first(pages);
         link != nxt_queue_tail(pages);
         link = next)
    {
        next = nxt_queue_next(link);
        page = nxt_queue_link_data(link, nxt_mp_page_t, link);

        available = mp->page_size - page->u.taken;

        if (size <= available) {
            goto found;
        }

        if (available == 0 || page->fails++ > 100) {
            nxt_queue_remove(link);
        }
    }

    page = nxt_mp_alloc_page(mp);

    if (nxt_slow_path(page == NULL)) {
        return page;
    }

    nxt_queue_insert_head(pages, &page->link);

    page->size = 0xFF;

found:

    p = nxt_mp_page_addr(mp, page);

    p += page->u.taken;
    page->u.taken += size;

    nxt_debug_alloc("mp get: %p", p);

    return p;
}


static nxt_mp_page_t *
nxt_mp_alloc_page(nxt_mp_t *mp)
{
    nxt_mp_page_t     *page;
    nxt_mp_block_t    *cluster;
    nxt_queue_link_t  *link;

    if (nxt_queue_is_empty(&mp->free_pages)) {
        cluster = nxt_mp_alloc_cluster(mp);
        if (nxt_slow_path(cluster == NULL)) {
            return NULL;
        }
    }

    link = nxt_queue_first(&mp->free_pages);
    nxt_queue_remove(link);

    page = nxt_queue_link_data(link, nxt_mp_page_t, link);

    return page;
}


static nxt_mp_block_t *
nxt_mp_alloc_cluster(nxt_mp_t *mp)
{
    nxt_uint_t      n;
    nxt_mp_block_t  *cluster;

    n = mp->cluster_size >> mp->page_size_shift;

    cluster = nxt_zalloc(sizeof(nxt_mp_block_t) + n * sizeof(nxt_mp_page_t));

    if (nxt_slow_path(cluster == NULL)) {
        return NULL;
    }

    /* NXT_MP_CLUSTER_BLOCK type is zero. */

    cluster->size = mp->cluster_size;

    cluster->start = nxt_memalign(mp->page_alignment, mp->cluster_size);
    if (nxt_slow_path(cluster->start == NULL)) {
        nxt_free(cluster);
        return NULL;
    }

    n--;
    cluster->pages[n].number = n;
    nxt_queue_insert_head(&mp->free_pages, &cluster->pages[n].link);

    while (n != 0) {
        n--;
        cluster->pages[n].number = n;
        nxt_queue_insert_before(&cluster->pages[n + 1].link,
                                &cluster->pages[n].link);
    }

    nxt_rbtree_insert(&mp->blocks, &cluster->node);

    return cluster;
}

#endif


static void *
nxt_mp_alloc_large(nxt_mp_t *mp, size_t alignment, size_t size)
{
    u_char          *p;
    size_t          aligned_size;
    uint8_t         type;
    nxt_mp_block_t  *block;

    /* Allocation must be less than 4G. */
    if (nxt_slow_path(size >= 0xFFFFFFFF)) {
        return NULL;
    }

    if (nxt_is_power_of_two(size)) {
        block = nxt_malloc(sizeof(nxt_mp_block_t));
        if (nxt_slow_path(block == NULL)) {
            return NULL;
        }

        p = nxt_memalign(alignment, size);
        if (nxt_slow_path(p == NULL)) {
            nxt_free(block);
            return NULL;
        }

        type = NXT_MP_DISCRETE_BLOCK;

    } else {
        aligned_size = nxt_align_size(size, sizeof(uintptr_t));

        p = nxt_memalign(alignment, aligned_size + sizeof(nxt_mp_block_t));
        if (nxt_slow_path(p == NULL)) {
            return NULL;
        }

        block = (nxt_mp_block_t *) (p + aligned_size);
        type = NXT_MP_EMBEDDED_BLOCK;
    }

    block->type = type;
    block->size = size;
    block->start = p;

    nxt_rbtree_insert(&mp->blocks, &block->node);

    return p;
}


static intptr_t
nxt_mp_rbtree_compare(nxt_rbtree_node_t *node1, nxt_rbtree_node_t *node2)
{
    nxt_mp_block_t  *block1, *block2;

    block1 = (nxt_mp_block_t *) node1;
    block2 = (nxt_mp_block_t *) node2;

    return (uintptr_t) block1->start - (uintptr_t) block2->start;
}


void
nxt_mp_free(nxt_mp_t *mp, void *p)
{
    const char      *err;
    nxt_thread_t    *thread;
    nxt_mp_block_t  *block;

    nxt_debug_alloc("mp free %p", p);

    block = nxt_mp_find_block(&mp->blocks, p);

    if (nxt_fast_path(block != NULL)) {

        if (block->type == NXT_MP_CLUSTER_BLOCK) {
            err = nxt_mp_chunk_free(mp, block, p);

            if (nxt_fast_path(err == NULL)) {
                return;
            }

        } else if (nxt_fast_path(p == block->start)) {
            nxt_rbtree_delete(&mp->blocks, &block->node);

            if (block->type == NXT_MP_DISCRETE_BLOCK) {
                nxt_free(block);
            }

            nxt_free(p);

            return;

        } else {
            err = "freed pointer points to middle of block: %p";
        }

    } else {
        err = "freed pointer is out of pool: %p";
    }

    thread = nxt_thread();

    nxt_log(thread->task, NXT_LOG_CRIT, err, p);
}


static nxt_mp_block_t *
nxt_mp_find_block(nxt_rbtree_t *tree, u_char *p)
{
    nxt_mp_block_t     *block;
    nxt_rbtree_node_t  *node, *sentinel;

    node = nxt_rbtree_root(tree);
    sentinel = nxt_rbtree_sentinel(tree);

    while (node != sentinel) {

        block = (nxt_mp_block_t *) node;

        if (p < block->start) {
            node = node->left;

        } else if (p >= block->start + block->size) {
            node = node->right;

        } else {
            return block;
        }
    }

    return NULL;
}


static const char *
nxt_mp_chunk_free(nxt_mp_t *mp, nxt_mp_block_t *cluster, u_char *p)
{
    u_char         *start;
    uintptr_t      offset;
    nxt_uint_t     n, size, chunk;
    nxt_queue_t    *chunk_pages;
    nxt_mp_page_t  *page;

    n = (p - cluster->start) >> mp->page_size_shift;
    start = cluster->start + (n << mp->page_size_shift);

    page = &cluster->pages[n];

    if (nxt_slow_path(page->size == 0)) {
        return "freed pointer points to already free page: %p";
    }

    if (nxt_slow_path(page->size == 0xFF)) {
        return "freed pointer points to non-freeble page: %p";
    }

    size = page->size << mp->chunk_size_shift;

    if (size != mp->page_size) {

        offset = (uintptr_t) (p - start) & (mp->page_size - 1);
        chunk = offset / size;

        if (nxt_slow_path(offset != chunk * size)) {
            return "freed pointer points to wrong chunk: %p";
        }

        if (nxt_slow_path(nxt_mp_chunk_is_free(page->u.map, chunk))) {
            return "freed pointer points to already free chunk: %p";
        }

        nxt_mp_chunk_set_free(page->u.map, chunk);

        if (page->u.map != 0xFFFFFFFF) {
            page->chunks++;

            if (page->chunks == 1) {
                /*
                 * Add the page to the head of pool chunk pages list
                 * of pages with free chunks.
                 */
                n = nxt_mp_chunk_pages_index(mp, size);
                chunk_pages = &mp->chunk_pages[n];

                nxt_queue_insert_head(chunk_pages, &page->link);
            }

            nxt_mp_free_junk(p, size);

            return NULL;

        } else {
            /*
             * All chunks are free, remove the page from pool
             * chunk pages list of pages with free chunks.
             */
            nxt_queue_remove(&page->link);
        }

    } else if (nxt_slow_path(p != start)) {
        return "invalid pointer to chunk: %p";
    }

    /* Add the free page to the pool's free pages tree. */

    page->size = 0;
    nxt_queue_insert_head(&mp->free_pages, &page->link);

    nxt_mp_free_junk(p, size);

    /* Test if all pages in the cluster are free. */

    n = mp->cluster_size >> mp->page_size_shift;
    page = cluster->pages;

    do {
         if (page->size != 0) {
             return NULL;
         }

         page++;
         n--;
    } while (n != 0);

    /* Free cluster. */

    n = mp->cluster_size >> mp->page_size_shift;
    page = cluster->pages;

    do {
         nxt_queue_remove(&page->link);
         page++;
         n--;
    } while (n != 0);

    nxt_rbtree_delete(&mp->blocks, &cluster->node);

    p = cluster->start;

    nxt_free(cluster);
    nxt_free(p);

    return NULL;
}


void *
nxt_mp_retain(nxt_mp_t *mp, size_t size)
{
    void  *p;

    p = nxt_mp_alloc(mp, size);

    if (nxt_fast_path(p != NULL)) {
        mp->retain++;
        nxt_debug_alloc("mp retain: %uD", mp->retain);
    }

    return p;
}


void
nxt_mp_release(nxt_mp_t *mp, void *p)
{
    nxt_mp_free(mp, p);

    mp->retain--;

    nxt_debug_alloc("mp release: %uD", mp->retain);

    if (mp->retain == 0) {
        nxt_mp_destroy(mp);
    }
}


void *
nxt_mp_nget(nxt_mp_t *mp, size_t size)
{
    nxt_debug_alloc("mp nget: %uz", size);

#if !(NXT_DEBUG_MEMORY)

    if (size <= mp->page_size) {
        return nxt_mp_get_small(mp, &mp->nget_pages, size);
    }

#endif

    return nxt_mp_alloc_large(mp, NXT_MAX_ALIGNMENT, size);
}


void *
nxt_mp_get(nxt_mp_t *mp, size_t size)
{
    nxt_debug_alloc("mp get: %uz", size);

#if !(NXT_DEBUG_MEMORY)

    if (size <= mp->page_size) {
        size = nxt_max(size, NXT_MAX_ALIGNMENT);
        return nxt_mp_get_small(mp, &mp->get_pages, size);
    }

#endif

    return nxt_mp_alloc_large(mp, NXT_MAX_ALIGNMENT, size);
}


void *
nxt_mp_zget(nxt_mp_t *mp, size_t size)
{
    void  *p;

    p = nxt_mp_get(mp, size);

    if (nxt_fast_path(p != NULL)) {
        memset(p, 0, size);
    }

    return p;
}
