#include <slub_pmm.h>
#include <pmm.h>
#include <list.h>
#include <string.h>
#include <stdio.h>
#define le2slab(le, member)  to_struct((le), slub_slab_t, member)

/* SLUB分配器实现
 * 两层架构:
 * 1. 第一层: 使用buddy system分配整页(4KB)
 * 2. 第二层: 在页内分配任意大小的内存块
 */

// 定义size class: 8, 16, 32, 64, 128, 256, 512, 1024, 2048字节
#define SLUB_MIN_SIZE      8
#define SLUB_MAX_SIZE      2048
#define SLUB_SIZE_CLASSES  9

// 每个size class的大小
static const size_t size_classes[SLUB_SIZE_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048
};

// SLUB对象头(用于空闲对象链表)
typedef struct slub_object {
    struct slub_object *next;
} slub_object_t;

// SLUB slab结构(管理一个页)
typedef struct slub_slab {
    list_entry_t slab_link;      // 链接到kmem_cache的slab链表
    void *page_addr;             // 页的起始地址
    struct Page *page;           // 对应的Page结构
    slub_object_t *freelist;     // 空闲对象链表
    unsigned int inuse;          // 已分配对象数
    unsigned int objects;        // 总对象数
    unsigned int size;           // 对象大小
} slub_slab_t;

// SLUB缓存结构(每个size class一个)
typedef struct kmem_cache {
    size_t size;                 // 对象大小
    size_t objects_per_slab;     // 每个slab的对象数
    list_entry_t partial_slabs;  // 部分使用的slab链表
    list_entry_t full_slabs;     // 完全使用的slab链表
    list_entry_t free_slabs;     // 完全空闲的slab链表
    unsigned int nr_slabs;       // slab总数
    unsigned int nr_objs;        // 对象总数
    unsigned int nr_free;        // 空闲对象数
} kmem_cache_t;

// 每个size class的缓存
static kmem_cache_t kmem_caches[SLUB_SIZE_CLASSES];

// 大对象分配记录(超过2048字节)
#define MAX_LARGE_ALLOCS 64
typedef struct {
    void *addr;
    size_t size;
    size_t pages;
} large_alloc_t;

static large_alloc_t large_allocs[MAX_LARGE_ALLOCS];
static int nr_large_allocs = 0;

// 获取size对应的cache索引
static int get_cache_index(size_t size) {
    for (int i = 0; i < SLUB_SIZE_CLASSES; i++) {
        if (size <= size_classes[i]) {
            return i;
        }
    }
    return -1;
}

// 初始化一个slab
static slub_slab_t* init_slab(kmem_cache_t *cache) {
    // 从buddy system分配一页
    struct Page *page = alloc_page();
    if (page == NULL) {
        return NULL;
    }
    
    // 计算页的虚拟地址
    void *page_addr = (void *)((uintptr_t)pages + 
                               (page - pages) * sizeof(struct Page));
    
    // 在页的开头放置slab管理结构
    slub_slab_t *slab = (slub_slab_t *)page_addr;
    slab->page = page;
    slab->page_addr = page_addr;
    slab->size = cache->size;
    slab->inuse = 0;
    
    // 计算可用空间和对象数量
    size_t available = PGSIZE - sizeof(slub_slab_t);
    slab->objects = available / cache->size;
    cache->objects_per_slab = slab->objects;
    
    // 初始化空闲对象链表
    char *obj_start = (char *)page_addr + sizeof(slub_slab_t);
    slab->freelist = NULL;
    
    for (unsigned int i = 0; i < slab->objects; i++) {
        slub_object_t *obj = (slub_object_t *)(obj_start + i * cache->size);
        obj->next = slab->freelist;
        slab->freelist = obj;
    }
    
    list_add(&cache->free_slabs, &slab->slab_link);
    cache->nr_slabs++;
    cache->nr_objs += slab->objects;
    cache->nr_free += slab->objects;
    
    return slab;
}

// 初始化SLUB分配器
void slub_init(void) {
    cprintf("Initializing SLUB allocator...\n");
    
    for (int i = 0; i < SLUB_SIZE_CLASSES; i++) {
        kmem_cache_t *cache = &kmem_caches[i];
        cache->size = size_classes[i];
        cache->objects_per_slab = 0;
        list_init(&cache->partial_slabs);
        list_init(&cache->full_slabs);
        list_init(&cache->free_slabs);
        cache->nr_slabs = 0;
        cache->nr_objs = 0;
        cache->nr_free = 0;
        
        cprintf("  Size class %d: %d bytes\n", i, (int)cache->size);
    }
    
    // 初始化大对象分配记录
    for (int i = 0; i < MAX_LARGE_ALLOCS; i++) {
        large_allocs[i].addr = NULL;
        large_allocs[i].size = 0;
        large_allocs[i].pages = 0;
    }
    nr_large_allocs = 0;
    
    cprintf("SLUB allocator initialized!\n");
}

// 从slab分配对象
static void* alloc_from_slab(slub_slab_t *slab, kmem_cache_t *cache) {
    if (slab->freelist == NULL) {
        return NULL;
    }
    
    // 从空闲链表取出一个对象
    slub_object_t *obj = slab->freelist;
    slab->freelist = obj->next;
    slab->inuse++;
    cache->nr_free--;
    
    // 如果slab变满,移到full链表
    if (slab->freelist == NULL) {
        list_del(&slab->slab_link);
        list_add(&cache->full_slabs, &slab->slab_link);
    }
    
    return (void *)obj;
}

// SLUB分配
void* slub_alloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    // 对于大对象(>2048字节),直接使用buddy system
    if (size > SLUB_MAX_SIZE) {
        size_t pages_needed = (size + PGSIZE - 1) / PGSIZE;
        struct Page *page = alloc_pages(pages_needed);
        if (page == NULL) {
            return NULL;
        }
        
        // 记录大对象分配
        if (nr_large_allocs < MAX_LARGE_ALLOCS) {
            void *addr = (void *)((uintptr_t)pages + 
                                 (page - pages) * sizeof(struct Page));
            large_allocs[nr_large_allocs].addr = addr;
            large_allocs[nr_large_allocs].size = size;
            large_allocs[nr_large_allocs].pages = pages_needed;
            nr_large_allocs++;
            return addr;
        }
        return NULL;
    }
    
    // 获取对应的cache
    int cache_idx = get_cache_index(size);
    if (cache_idx < 0) {
        return NULL;
    }
    
    kmem_cache_t *cache = &kmem_caches[cache_idx];
    
    // 优先从partial slab分配
    if (!list_empty(&cache->partial_slabs)) {
        list_entry_t *le = list_next(&cache->partial_slabs);
        slub_slab_t *slab = le2slab(le, slab_link);
        void *obj = alloc_from_slab(slab, cache);
        if (obj != NULL) {
            return obj;
        }
    }
    
    // 尝试从free slab分配
    if (!list_empty(&cache->free_slabs)) {
        list_entry_t *le = list_next(&cache->free_slabs);
        slub_slab_t *slab = le2slab(le, slab_link);
        
        // 移到partial链表
        list_del(&slab->slab_link);
        list_add(&cache->partial_slabs, &slab->slab_link);
        
        void *obj = alloc_from_slab(slab, cache);
        if (obj != NULL) {
            return obj;
        }
    }
    
    // 需要分配新slab
    slub_slab_t *new_slab = init_slab(cache);
    if (new_slab == NULL) {
        return NULL;
    }
    
    // 移到partial链表
    list_del(&new_slab->slab_link);
    list_add(&cache->partial_slabs, &new_slab->slab_link);
    
    return alloc_from_slab(new_slab, cache);
}

// 查找对象所属的slab
static slub_slab_t* find_slab(void *ptr, kmem_cache_t **cache_out) {
    for (int i = 0; i < SLUB_SIZE_CLASSES; i++) {
        kmem_cache_t *cache = &kmem_caches[i];
        
        // 检查所有链表
        list_entry_t *lists[] = {
            &cache->partial_slabs,
            &cache->full_slabs,
            &cache->free_slabs
        };
        
        for (int j = 0; j < 3; j++) {
            list_entry_t *head = lists[j];
            list_entry_t *le = list_next(head);
            
            while (le != head) {
                slub_slab_t *slab = le2slab(le, slab_link);
                
                // 检查ptr是否在这个slab的范围内
                if (ptr >= slab->page_addr && 
                    ptr < (void *)((char *)slab->page_addr + PGSIZE)) {
                    if (cache_out) {
                        *cache_out = cache;
                    }
                    return slab;
                }
                
                le = list_next(le);
            }
        }
    }
    
    return NULL;
}

// SLUB释放
void slub_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    
    // 检查是否是大对象
    for (int i = 0; i < nr_large_allocs; i++) {
        if (large_allocs[i].addr == ptr) {
            // 释放大对象
            struct Page *page = pa2page(PADDR((uintptr_t)ptr));
            free_pages(page, large_allocs[i].pages);
            
            // 从记录中删除
            large_allocs[i] = large_allocs[nr_large_allocs - 1];
            nr_large_allocs--;
            return;
        }
    }
    
    // 查找对象所属的slab
    kmem_cache_t *cache;
    slub_slab_t *slab = find_slab(ptr, &cache);
    
    if (slab == NULL) {
        cprintf("Warning: slub_free called with invalid pointer %p\n", ptr);
        return;
    }
    
    // 检查slab当前状态
    int was_full = (slab->freelist == NULL);
    
    // 将对象加入空闲链表
    slub_object_t *obj = (slub_object_t *)ptr;
    obj->next = slab->freelist;
    slab->freelist = obj;
    slab->inuse--;
    cache->nr_free++;
    
    // 如果slab从full变为partial
    if (was_full) {
        list_del(&slab->slab_link);
        list_add(&cache->partial_slabs, &slab->slab_link);
    }
    
    // 如果slab完全空闲
    if (slab->inuse == 0) {
        list_del(&slab->slab_link);
        
        // 如果cache中空闲slab太多(>2个),释放这个slab
        int free_slab_count = 0;
        list_entry_t *le = list_next(&cache->free_slabs);
        while (le != &cache->free_slabs) {
            free_slab_count++;
            le = list_next(le);
        }
        
        if (free_slab_count >= 2) {
            // 释放slab
            cache->nr_slabs--;
            cache->nr_objs -= slab->objects;
            cache->nr_free -= slab->objects;
            free_page(slab->page);
        } else {
            // 保留slab
            list_add(&cache->free_slabs, &slab->slab_link);
        }
    }
}

// 获取已分配内存总量
size_t slub_allocated_memory(void) {
    size_t total = 0;
    
    for (int i = 0; i < SLUB_SIZE_CLASSES; i++) {
        kmem_cache_t *cache = &kmem_caches[i];
        total += (cache->nr_objs - cache->nr_free) * cache->size;
    }
    
    // 加上大对象
    for (int i = 0; i < nr_large_allocs; i++) {
        total += large_allocs[i].size;
    }
    
    return total;
}

// 获取slab总数
size_t slub_total_slabs(void) {
    size_t total = 0;
    for (int i = 0; i < SLUB_SIZE_CLASSES; i++) {
        total += kmem_caches[i].nr_slabs;
    }
    return total;
}

// SLUB测试函数
void slub_check(void) {
    cprintf("\n========== SLUB Allocator Test ==========\n");
    
    // 测试1: 小对象分配和释放
    cprintf("\nTest 1: Small object allocation (8-256 bytes)\n");
    void *ptrs[20];
    size_t sizes[] = {8, 16, 32, 64, 128, 256};
    
    for (int i = 0; i < 20; i++) {
        size_t size = sizes[i % 6];
        ptrs[i] = slub_alloc(size);
        assert(ptrs[i] != NULL);
        
        // 写入数据验证
        memset(ptrs[i], 0xAA, size);
        
        cprintf("  Allocated %d bytes at %p\n", (int)size, ptrs[i]);
    }
    
    cprintf("  Total allocated: %d bytes\n", (int)slub_allocated_memory());
    cprintf("  Total slabs: %d\n", (int)slub_total_slabs());
    
    // 释放一半
    for (int i = 0; i < 10; i++) {
        slub_free(ptrs[i]);
    }
    cprintf("  After freeing 10 objects: %d bytes allocated\n", 
            (int)slub_allocated_memory());
    
    // 释放另一半
    for (int i = 10; i < 20; i++) {
        slub_free(ptrs[i]);
    }
    cprintf("  After freeing all: %d bytes allocated\n", 
            (int)slub_allocated_memory());
    
    // 测试2: 大对象分配
    cprintf("\nTest 2: Large object allocation (>2048 bytes)\n");
    void *large1 = slub_alloc(4096);
    void *large2 = slub_alloc(8192);
    assert(large1 != NULL);
    assert(large2 != NULL);
    
    cprintf("  Allocated 4096 bytes at %p\n", large1);
    cprintf("  Allocated 8192 bytes at %p\n", large2);
    
    memset(large1, 0xBB, 4096);
    memset(large2, 0xCC, 8192);
    
    slub_free(large1);
    slub_free(large2);
    cprintf("  Large objects freed successfully\n");
    
    // 测试3: 混合分配模式
    cprintf("\nTest 3: Mixed allocation pattern\n");
    void *mixed[30];
    
    for (int i = 0; i < 30; i++) {
        size_t size;
        if (i % 3 == 0) size = 32;
        else if (i % 3 == 1) size = 128;
        else size = 512;
        
        mixed[i] = slub_alloc(size);
        assert(mixed[i] != NULL);
    }
    
    cprintf("  Allocated 30 mixed-size objects\n");
    cprintf("  Total allocated: %d bytes\n", (int)slub_allocated_memory());
    
    // 随机释放模式
    for (int i = 0; i < 30; i += 2) {
        slub_free(mixed[i]);
    }
    cprintf("  After freeing every other object: %d bytes\n", 
            (int)slub_allocated_memory());
    
    for (int i = 1; i < 30; i += 2) {
        slub_free(mixed[i]);
    }
    cprintf("  All freed: %d bytes\n", (int)slub_allocated_memory());
    
    // 测试4: 边界情况
    cprintf("\nTest 4: Edge cases\n");
    
    // 测试最小分配
    void *min_obj = slub_alloc(1);
    assert(min_obj != NULL);
    cprintf("  1-byte allocation: %p (using 8-byte class)\n", min_obj);
    slub_free(min_obj);
    
    // 测试NULL释放
    slub_free(NULL);
    cprintf("  NULL free handled correctly\n");
    
    // 测试重复分配和释放
    for (int round = 0; round < 3; round++) {
        void *temp[10];
        for (int i = 0; i < 10; i++) {
            temp[i] = slub_alloc(64);
            assert(temp[i] != NULL);
        }
        for (int i = 0; i < 10; i++) {
            slub_free(temp[i]);
        }
        cprintf("  Round %d: allocate and free 10 objects OK\n", round + 1);
    }
    
    // 测试5: 碎片整理验证
    cprintf("\nTest 5: Fragmentation test\n");
    void *frag[50];
    
    // 分配50个对象
    for (int i = 0; i < 50; i++) {
        frag[i] = slub_alloc(64);
        assert(frag[i] != NULL);
    }
    cprintf("  Allocated 50 objects\n");
    
    // 释放所有奇数索引的对象(制造碎片)
    for (int i = 1; i < 50; i += 2) {
        slub_free(frag[i]);
    }
    cprintf("  Freed 25 objects (creating fragmentation)\n");
    cprintf("  Allocated memory: %d bytes\n", (int)slub_allocated_memory());
    
    // 重新分配25个对象(应该复用空闲空间)
    size_t slabs_before = slub_total_slabs();
    for (int i = 1; i < 50; i += 2) {
        frag[i] = slub_alloc(64);
        assert(frag[i] != NULL);
    }
    size_t slabs_after = slub_total_slabs();
    
    cprintf("  Re-allocated 25 objects\n");
    cprintf("  Slabs before: %d, after: %d (should be same)\n", 
            (int)slabs_before, (int)slabs_after);
    assert(slabs_before == slabs_after);
    
    // 清理
    for (int i = 0; i < 50; i++) {
        slub_free(frag[i]);
    }
    
    // 测试6: 统计信息验证
    cprintf("\nTest 6: Statistics verification\n");
    cprintf("  Final allocated memory: %d bytes\n", 
            (int)slub_allocated_memory());
    cprintf("  Final slab count: %d\n", (int)slub_total_slabs());
    
    // 显示每个size class的状态
    cprintf("\n  Size class statistics:\n");
    for (int i = 0; i < SLUB_SIZE_CLASSES; i++) {
        kmem_cache_t *cache = &kmem_caches[i];
        if (cache->nr_slabs > 0) {
            cprintf("    %4d bytes: %d slabs, %d objects, %d free\n",
                   (int)cache->size, cache->nr_slabs, 
                   cache->nr_objs, cache->nr_free);
        }
    }
    
    cprintf("\n========== SLUB Test PASSED ==========\n\n");
}