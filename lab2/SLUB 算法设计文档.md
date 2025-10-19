# SLUB 算法设计文档

## 一、概述

在第二个拓展练习Challenge中，我们实现了一个基于 **SLUB **算法的动态内存分配器，用于内核层面的小块内存管理。
相较于传统的 `first_fit`、`best_fit` 等页分配算法，SLUB 提供了更细粒度的对象级分配，减少内存碎片并提升分配性能。

系统通过统一的 `pmm_manager` 接口将 SLUB 集成进物理内存管理框架中，使得内核可以使用 `kmalloc/kfree` 与 `kmem_cache_*` 系列接口进行对象级内存管理。

## 二、核心数据结构

### `struct kmem_cache`

管理特定大小对象的缓存池，里面管理多个`slab`，不同状态的`slab`用`full`、`partial`、`empty`描述，并用链表管理：

```c
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
```

- `slabs_full`：已满的 slab 链表。
- `slabs_partial`：部分使用的 slab。
- `slabs_empty`：完全空闲的 slab。
- `objsize`：每个对象的大小。
- `ctor/dtor`：对象构造与析构函数。

### `struct slab`

slab 是一页物理内存（通常 4KB），一页内按照特定的对象大小分配:

```c
struct slab {
    uint32_t magic;
    list_entry_t slab_link;
    struct kmem_cache *cache;
    struct Page *s_page;
    void *free_list;
    size_t inuse;
    size_t num_objs;
};
```

### `struct big_alloc_hdr`

用于标识页级大块分配：

```c
struct big_alloc_hdr {
    uint32_t magic;
    uint32_t pages;
};
```

## 三、核心接口

### `kmem_cache_create()`

```c
struct kmem_cache *kmem_cache_create(
    const char *name, size_t objsize, size_t align,
    void (*ctor)(void *), void (*dtor)(void *));
```

- 创建一个新的缓存池（cache），用于管理相同大小的对象。
- 其中的`name`指的是这个cache的名字，易于管理；
- `objsize`指的就是每个对象的大小（以字节为单位）；
- 后面的参数描述了对齐要求、对象的构造和析构函数。

```c
list_init(&cache->slabs_full);
list_init(&cache->slabs_partial);
list_init(&cache->slabs_empty);
list_add_before(&kmem_caches, &cache->kmem_cache_link);
```

- 前面三行用来初始化三种状态slab的链表，作用于cache内部；
- 最后一行将当前的cache加入全局cache链表中，作用于全局。

### `slab_init`

```c
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
```

- 计算这一页能放多少个对象；

- 用一个单链表把所有空闲对象连起来；

- `free_list` 指向第一个空闲对象。

- 这样操作，分配时只需要：

  ```c
  obj = s->free_list;
  s->free_list = *(void **)obj;
  ```

  就能在 **O(1)** 时间取出一个对象。

### `kmem_cache_alloc`

从指定缓存中分配一个对象：

```c
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
```

它的逻辑是：

1. 找到可以用的 slab（从“部分空闲（`partial`）”和“全部空闲（`empty`）”的slab列表中寻找）。

2. 如果没有可用的slab，就分配一页新的slab，也照例进行slab的初始化（`slab_init(cache, s, page)`）。

3. 找到了可用的slab后，从 slab 的 `free_list` 取一个空闲对象。

4. 分配完之后，如果该slab变满了，移入`full`链表

   ```c
   if (s->inuse == s->num_objs) 
   { 
       list_del(&s->slab_link); 
       list_add(&cache->slabs_full, &s->slab_link); 
   }
   ```

### `kmem_cache_free`

把一个通过 `kmem_cache_alloc()` 分配的对象释放回它所属的 slab。

```c
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
```

 调用`obj_to_slab`函数，来推算出对象所在的slab。

```c
*(void **)obj = s->free_list;
s->free_list = obj;
s->inuse--;
```

接着，把对象重新挂回`free_list`。挂回之后，slab的状态可能改变，需要进行更新：

```c
if (s->inuse == 0) {
    // slab 完全空闲，移入 slabs_empty
    list_del(&s->slab_link);
    list_add(&cache->slabs_empty, &s->slab_link);
} else if (s->inuse == s->num_objs - 1) {
    // slab 从 full 状态回到部分使用状态
    list_del(&s->slab_link);
    list_add(&cache->slabs_partial, &s->slab_link);
}
```

### `init_kmalloc_caches`

这个函数在系统启动阶段，为 `kmalloc()` 预建一组固定大小的缓存池。每个缓存池对应一种常用对象大小（例如 8B、16B、32B...）。

```c
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
```

遍历`sz`从`KMALLOC_MIN_SIZE`(8字节)加起。然后调用`kmem_cache_create`函数分配缓冲池。

对应的缓冲池名称是`kmalloc-8`（管理8B大小）、`kmalloc-16`（管理16B大小）......

## 四、测试

### `slub_check`

```c
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
```

测试分为三部分：

1. 基本的 `kmem_cache` 功能测试
2. 小内存分配（`kmalloc`）测试
3. 大内存分配（页级分配）测试
