#include <slub_pmm.h>
#include <default_pmm.h>
#include <string.h>
#include <assert.h>

// ================== 基础配置 ==================
#define KMALLOC_MIN_SIZE   8
#define KMALLOC_MAX_SIZE   (PGSIZE / 2)
#define KMALLOC_CACHES_NUM 10

static list_entry_t kmem_caches;
static struct kmem_cache *kmalloc_caches[KMALLOC_CACHES_NUM];
static int kmalloc_caches_inited = 0;   // 延迟初始化标志

extern const struct pmm_manager default_pmm_manager;
#define page_alloc(n)       default_pmm_manager.alloc_pages(n)
#define page_free(p, n)     default_pmm_manager.free_pages(p, n)
#define page_nr_free()      default_pmm_manager.nr_free_pages()

extern uint64_t va_pa_offset;

// ================== 地址换算 ==================
static inline void *page2kva(struct Page *p) {
    return (void *)(page2pa(p) + va_pa_offset);
}
static inline struct Page *kva2page(void *kva) {
    return pa2page(PADDR(kva));
}

// ================== slab 辅助 ==================
static inline void *slab_idx_to_obj(struct slab *s, size_t idx) {
    uintptr_t base = (uintptr_t)page2kva(s->s_page);
    return (void *)(base + sizeof(struct slab) + s->cache->objsize * idx);
}
static inline struct slab *obj_to_slab(void *obj) {
    struct Page *page = kva2page(obj);
    return (struct slab *)page2kva(page);
}

static void slab_init(struct kmem_cache *cache, struct slab *s, struct Page *page) {
    s->magic  = SLAB_HDR_MAGIC;
    s->cache  = cache;
    s->s_page = page;
    s->inuse  = 0;
    s->num_objs = (PGSIZE - sizeof(struct slab)) / cache->objsize;
    assert(s->num_objs > 0);
    s->free_list = NULL;
    for (size_t i = 0; i < s->num_objs; i++) {
        void *obj = slab_idx_to_obj(s, i);
        *(void **)obj = s->free_list;
        s->free_list = obj;
    }
}

// ================== 初始化与 cache 创建 ==================
static inline void *kcache_alloc_meta(void) {
    struct Page *p = page_alloc(1);
    return p ? page2kva(p) : NULL;
}
static inline void kcache_free_meta(void *meta) {
    if (!meta) return;
    page_free(kva2page(meta), 1);
}

void slub_init(void) {
    list_init(&kmem_caches);
}

struct kmem_cache *kmem_cache_create(
    const char *name, size_t objsize, size_t align,
    void (*ctor)(void *), void (*dtor)(void *)) {

    if (objsize < sizeof(void *)) objsize = sizeof(void *);
    if (align > 0 && (objsize % align) != 0)
        objsize = (objsize / align + 1) * align;

    struct kmem_cache *cache = (struct kmem_cache *)kcache_alloc_meta();
    if (!cache) return NULL;

    memset(cache, 0, sizeof(*cache));
    cache->name = name;
    cache->objsize = objsize;
    cache->ctor = ctor;
    cache->dtor = dtor;
    list_init(&cache->slabs_full);
    list_init(&cache->slabs_partial);
    list_init(&cache->slabs_empty);

    list_add_before(&kmem_caches, &cache->kmem_cache_link);
    cprintf("kmem_cache_create: %s, objsize=%u\n", name, (unsigned int)objsize);
    return cache;
}

void kmem_cache_destroy(struct kmem_cache *cache) {
    list_entry_t *le, *next;
    le = list_next(&cache->slabs_full);
    while (le != &cache->slabs_full) {
        next = list_next(le);
        struct slab *s = container_of(le, struct slab, slab_link);
        list_del(le);
        page_free(s->s_page, 1);
        le = next;
    }
    le = list_next(&cache->slabs_partial);
    while (le != &cache->slabs_partial) {
        next = list_next(le);
        struct slab *s = container_of(le, struct slab, slab_link);
        list_del(le);
        page_free(s->s_page, 1);
        le = next;
    }
    le = list_next(&cache->slabs_empty);
    while (le != &cache->slabs_empty) {
        next = list_next(le);
        struct slab *s = container_of(le, struct slab, slab_link);
        list_del(le);
        page_free(s->s_page, 1);
        le = next;
    }
    list_del(&cache->kmem_cache_link);
    kcache_free_meta(cache);
}

// ================== 分配/释放 ==================
void *kmem_cache_alloc(struct kmem_cache *cache) {
    struct slab *s = NULL;
    if (!list_empty(&cache->slabs_partial))
        s = container_of(list_next(&cache->slabs_partial), struct slab, slab_link);
    else if (!list_empty(&cache->slabs_empty)) {
        s = container_of(list_next(&cache->slabs_empty), struct slab, slab_link);
        list_del(&s->slab_link);
        list_add(&cache->slabs_partial, &s->slab_link);
    } else {
        struct Page *page = page_alloc(1);
        if (!page) return NULL;
        s = (struct slab *)page2kva(page);
        slab_init(cache, s, page);
        list_add(&cache->slabs_partial, &s->slab_link);
    }

    assert(s && s->magic == SLAB_HDR_MAGIC && s->free_list);
    void *obj = s->free_list;
    s->free_list = *(void **)obj;
    s->inuse++;

    if (s->inuse == s->num_objs) {
        list_del(&s->slab_link);
        list_add(&cache->slabs_full, &s->slab_link);
    }

    if (cache->ctor) cache->ctor(obj);
    return obj;
}

void kmem_cache_free(struct kmem_cache *cache, void *obj) {
    if (!obj) return;
    if (cache->dtor) cache->dtor(obj);
    struct slab *s = obj_to_slab(obj);
    assert(s && s->magic == SLAB_HDR_MAGIC && s->cache == cache);
    *(void **)obj = s->free_list;
    s->free_list = obj;
    s->inuse--;

    if (s->inuse == 0) {
        list_del(&s->slab_link);
        list_add(&cache->slabs_empty, &s->slab_link);
    } else if (s->inuse == s->num_objs - 1) {
        list_del(&s->slab_link);
        list_add(&cache->slabs_partial, &s->slab_link);
    }
}

// ================== kmalloc/kfree ==================
static char kmalloc_names[KMALLOC_CACHES_NUM][24];

static struct kmem_cache *find_kmalloc_cache(size_t size) {
    for (int i = 0; i < KMALLOC_CACHES_NUM; i++)
        if (kmalloc_caches[i] && kmalloc_caches[i]->objsize >= size)
            return kmalloc_caches[i];
    return NULL;
}

static void init_kmalloc_caches(void) {
    size_t sz = KMALLOC_MIN_SIZE;
    for (int i = 0; i < KMALLOC_CACHES_NUM; i++) {
        snprintf(kmalloc_names[i], sizeof(kmalloc_names[i]), "kmalloc-%u", (unsigned)sz);
        kmalloc_caches[i] = kmem_cache_create(kmalloc_names[i], sz, 0, NULL, NULL);
        assert(kmalloc_caches[i] != NULL);
        if (sz < KMALLOC_MAX_SIZE) sz <<= 1; else break;
    }
    kmalloc_caches_inited = 1;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    if (!kmalloc_caches_inited)
        init_kmalloc_caches();

    if (size > KMALLOC_MAX_SIZE) {
        size_t pages = (size + sizeof(struct big_alloc_hdr) + PGSIZE - 1) / PGSIZE;
        struct Page *page = page_alloc(pages);
        if (!page) return NULL;
        struct big_alloc_hdr *hdr = (struct big_alloc_hdr *)page2kva(page);
        hdr->magic = BIG_HDR_MAGIC;
        hdr->pages = (uint32_t)pages;
        return (void *)(hdr + 1);
    }
    struct kmem_cache *cache = find_kmalloc_cache(size);
    if (!cache) return NULL;
    return kmem_cache_alloc(cache);
}

void kfree(void *obj) {
    if (!obj) return;
    struct Page *page = kva2page(obj);
    void *base = page2kva(page);
    struct big_alloc_hdr *bhdr = (struct big_alloc_hdr *)base;
    if (bhdr->magic == BIG_HDR_MAGIC) {
        page_free(page, bhdr->pages);
        return;
    }
    struct slab *s = (struct slab *)base;
    assert(s->magic == SLAB_HDR_MAGIC && s->cache);
    kmem_cache_free(s->cache, obj);
}

// ================== PMM接口 ==================
static void slub_pmm_init(void) {
    default_pmm_manager.init();
    slub_init();  // 不提前建 kmalloc caches
}
static void slub_pmm_init_memmap(struct Page *base, size_t n) {
    default_pmm_manager.init_memmap(base, n);
}
static struct Page *slub_pmm_alloc_pages(size_t n) {
    return default_pmm_manager.alloc_pages(n);
}
static void slub_pmm_free_pages(struct Page *base, size_t n) {
    default_pmm_manager.free_pages(base, n);
}
static size_t slub_pmm_nr_free_pages(void) {
    return default_pmm_manager.nr_free_pages();
}

// ================== 自检 ==================
static void slub_check(void) {
    cprintf("\n===== SLUB allocator self-check =====\n");
    struct kmem_cache *c32 = kmem_cache_create("test32", 32, 0, NULL, NULL);
    assert(c32);
    void *a = kmem_cache_alloc(c32);
    void *b = kmem_cache_alloc(c32);
    assert(a && b && a != b);
    kmem_cache_free(c32, a);
    kmem_cache_free(c32, b);
    kmem_cache_destroy(c32);
    cprintf("kmem_cache basic test passed.\n");

    void *s1 = kmalloc(16);
    void *s2 = kmalloc(64);
    void *s3 = kmalloc(16);
    assert(s1 && s2 && s3);
    kfree(s1); kfree(s2); kfree(s3);
    cprintf("kmalloc small test passed.\n");

    size_t big = PGSIZE * 3 + 123;
    void *lg = kmalloc(big);
    assert(lg);
    kfree(lg);
    cprintf("kmalloc big test passed.\n");
    cprintf("SLUB allocator check finished successfully!\n");
}

static void slub_pmm_check(void) {
    slub_check();
    default_pmm_manager.check();
}

const struct pmm_manager slub_pmm_manager = {
    .name = "slub_pmm_manager",
    .init = slub_pmm_init,
    .init_memmap = slub_pmm_init_memmap,
    .alloc_pages = slub_pmm_alloc_pages,
    .free_pages = slub_pmm_free_pages,
    .nr_free_pages = slub_pmm_nr_free_pages,
    .check = slub_pmm_check,
};
