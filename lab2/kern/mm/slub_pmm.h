#ifndef __KERN_MM_SLUB_PMM_H__
#define __KERN_MM_SLUB_PMM_H__

#include <pmm.h>
#include <list.h>
#include <defs.h>
#include <stdio.h>
#include <assert.h>
#include <memlayout.h>

// ---- container_of ----

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

// ---------- SLUB 结构 ----------
struct kmem_cache {
    list_entry_t slabs_full;
    list_entry_t slabs_partial;
    list_entry_t slabs_empty;
    size_t objsize;
    size_t num_active_slabs;
    size_t num_free_objs;
    const char *name;
    void (*ctor)(void *);
    void (*dtor)(void *);
    list_entry_t kmem_cache_link;
};

#define SLAB_HDR_MAGIC 0x534C4142u /* 'SLAB' */
struct slab {
    uint32_t magic;
    list_entry_t slab_link;
    struct kmem_cache *cache;
    struct Page *s_page;
    void *free_list;
    size_t inuse;
    size_t num_objs;
};

#define BIG_HDR_MAGIC  0x42494730u /* 'BIG0' */
struct big_alloc_hdr {
    uint32_t magic;
    uint32_t pages;
};

// 全局 SLUB 管理器接口
extern const struct pmm_manager slub_pmm_manager;

// ---------- API ----------
void slub_init(void);
struct kmem_cache *kmem_cache_create(
    const char *name, size_t objsize, size_t align,
    void (*ctor)(void *), void (*dtor)(void *));
void kmem_cache_destroy(struct kmem_cache *cache);
void *kmem_cache_alloc(struct kmem_cache *cache);
void kmem_cache_free(struct kmem_cache *cache, void *obj);
void *kmalloc(size_t size);
void kfree(void *obj);

#endif
