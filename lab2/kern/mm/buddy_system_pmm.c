#include <pmm.h>
#include <list.h>
#include <string.h>
#include <stdio.h>
#include <buddy_system_pmm.h>

#define MAX_ORDER  4      // 最大阶数

static free_area_t free_area[MAX_ORDER + 1]; // 每个阶的自由区
  
// 获取总空闲页数
static size_t get_nr_free(void) {
    size_t total = 0;
    for (int i = 0; i <= MAX_ORDER; i++) {
        total += free_area[i].nr_free * (1 << i);
    }
    return total;
}

// 计算阶数：找到能容纳n页的最小2的幂
static unsigned int get_order(size_t n) {
    unsigned int order = 0;
    size_t size = 1;
    
    while (size < n) {
        order++;
        size <<= 1;
        if (order > MAX_ORDER) {
            return MAX_ORDER;
        }
    }
    return order;
}

// 检查页面是否在自由链表中
static int page_in_free_list(struct Page *page) {
    list_entry_t *le = &free_area[page->property].free_list;
    list_entry_t *head = le;
    
    while ((le = list_next(le)) != head) {
        if (le2page(le, page_link) == page) {
            return 1;
        }
    }
    return 0;
}

// 检查是否是伙伴关系
static int is_buddy(struct Page *page, unsigned int order) {
    // 检查页面范围
    if (page2ppn(page) >= npage) {
        return 0;
    }
    // 检查页面属性
    if (!PageProperty(page)) {
        return 0;
    }
    // 检查阶数匹配
    if (page->property != order) {
        return 0;
    }
    // 检查是否在自由链表中
    if (!page_in_free_list(page)) {
        return 0;
    }
    return 1;
}

// 找到伙伴页
static struct Page *get_buddy(struct Page *page) {
    unsigned int order = page->property;
    unsigned long buddy_idx = page2ppn(page) ^ (1 << order);
    
    // 检查伙伴页面是否在有效范围内
    if (buddy_idx >= npage) {
        return NULL;
    }
    
    return &pages[buddy_idx - nbase];
}

static void buddy_init(void) {
    for (int i = 0; i <= MAX_ORDER; i++) {
        list_init(&free_area[i].free_list);
        free_area[i].nr_free = 0;
    }
}

static void buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    
    // 初始化所有页面
    struct Page *p = base;
    for (; p != base + n; p++) {
        p->flags = 0;
        set_page_ref(p, 0);
        ClearPageProperty(p);
    }
    
    // 将内存按最大可能的块大小加入自由链表
    size_t remaining = n;
    struct Page *current = base;
    
    while (remaining > 0) {
        unsigned int order = get_order(remaining);
        unsigned int block_size = 1 << order;
        
        // 如果剩余内存不够一个完整的块，降低阶数
        while (block_size > remaining && order > 0) {
            order--;
            block_size = 1 << order;
        }
        
        // 初始化这个内存块
        current->property = order;
        SetPageProperty(current);
        list_add(&free_area[order].free_list, &(current->page_link));
        free_area[order].nr_free++;
        
        current += block_size;
        remaining -= block_size;
    }
}

static struct Page *buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > get_nr_free()) {
        return NULL;
    }
    
    unsigned int order = get_order(n);
    unsigned int current_order = order;
    
    // 从所需阶数开始向上寻找可用的块
    while (current_order <= MAX_ORDER) {
        if (!list_empty(&free_area[current_order].free_list)) {
            break;
        }
        current_order++;
    }
    
    if (current_order > MAX_ORDER) {
        return NULL;
    }
    
    // 找到可用的块
    list_entry_t *le = list_next(&free_area[current_order].free_list);
    struct Page *page = le2page(le, page_link);
    
    // 从自由链表中移除
    list_del(le);
    free_area[current_order].nr_free--;
    
    // 如果找到的块比需要的大，进行分割
    while (current_order > order) {
        current_order--;
        
        // 分割成两个伙伴块
        struct Page *buddy = page + (1 << current_order);
        buddy->property = current_order;
        SetPageProperty(buddy);
        
        // 将伙伴加入对应阶的自由链表
        list_add(&free_area[current_order].free_list, &(buddy->page_link));
        free_area[current_order].nr_free++;
        
        // 更新当前块的属性
        page->property = current_order;
        SetPageProperty(page);
    }
    
    // 设置分配出去的块
    ClearPageProperty(page);
    
    return page;
}

static void buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    
    unsigned int order = get_order(n);
    struct Page *p = base;
    
    // 设置释放的块
    p->property = order;
    SetPageProperty(p);
    
    unsigned int current_order = order;
    
    // 尝试合并伙伴块
    while (current_order < MAX_ORDER) {
        struct Page *buddy = get_buddy(p);
        
        // 如果伙伴不存在或不是有效的伙伴，停止合并
        if (buddy == NULL || !is_buddy(buddy, current_order)) {
            break;
        }
        
        // 从自由链表中移除伙伴
        list_del(&(buddy->page_link));
        free_area[current_order].nr_free--;
        ClearPageProperty(buddy);
        
        // 确定合并后块的起始地址（取地址较小的那个）
        if (p > buddy) {
            struct Page *tmp = p;
            p = buddy;
            buddy = tmp;
        }
        
        // 合并块
        current_order++;
        p->property = current_order;
        SetPageProperty(p);
    }
    
    // 将合并后的块加入对应阶的自由链表
    list_add(&free_area[current_order].free_list, &(p->page_link));
    free_area[current_order].nr_free++;
}

static size_t buddy_nr_free_pages(void) {
    return get_nr_free();
}

static void buddy_check(void) {
    cprintf("Buddy system check:\n");
    
    // 测试1：验证伙伴合并特性
    cprintf("1. Testing buddy merge...\n");
    struct Page *p0 = alloc_pages(1);
    struct Page *p1 = alloc_pages(1);
    
    // 检查它们是否是伙伴（页号相差1）
    unsigned long idx0 = page2ppn(p0);
    unsigned long idx1 = page2ppn(p1);
    int are_buddies = ((idx0 ^ idx1) == 1);
    
    if (are_buddies) {
        cprintf("  Found buddy pages, testing merge...\n");
        // 记录合并前2页块的数量
        size_t order1_before = free_area[1].nr_free;
        
        free_page(p0);
        free_page(p1);
        
        // 验证合并：应该增加一个2页块
        size_t order1_after = free_area[1].nr_free;
        assert(order1_after == order1_before + 1);
        cprintf("  Buddy merge successful!\n");
    } else {
        cprintf("  Pages are not buddies, skip merge test\n");
        free_page(p0);
        free_page(p1);
    }
    
    // 测试2：验证分割特性
    cprintf("2. Testing block split...\n");
    size_t initial_free = nr_free_pages();
    
    // 分配一个需要分割的大小（比如3页，需要从4页块分割）
    struct Page *p = alloc_pages(3);
    assert(p != NULL);
    
    // 验证确实分配了4页（2^2）
    size_t after_alloc = nr_free_pages();
    assert(initial_free - after_alloc == 4);
    
    free_pages(p, 3);
    
    // 验证完全恢复
    assert(nr_free_pages() == initial_free);
    cprintf("  Block split test passed!\n");
    
    // 测试3：验证阶数计算
    cprintf("3. Testing order calculation...\n");
    assert(get_order(1) == 0);  // 1页 → order0
    assert(get_order(2) == 1);  // 2页 → order1  
    assert(get_order(3) == 2);  // 3页 → order2（需要4页）
    assert(get_order(4) == 2);  // 4页 → order2
    assert(get_order(5) == 3);  // 5页 → order3（需要8页）
    cprintf("  Order calculation correct!\n");
    
    cprintf("buddy_check() succeeded!\n");
}

const struct pmm_manager buddy_system_pmm_manager = {
    .name = "buddy_system_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};