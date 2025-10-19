# Buddy System设计文档

## 扩展练习Challenge：buddy system（伙伴系统）分配算法

### 关键数据结构：

```c++
typedef struct {
    list_entry_t free_list;         // 自由链表的头节点
    unsigned int nr_free;           // 该阶空闲块的数量
} free_area_t;
```

```c++
#define MAX_ORDER  4      // 最大阶数
static free_area_t free_area[MAX_ORDER + 1]; // 每个阶的自由区
```

```c++
struct Page {
    int ref;                        // page frame's reference counter
    uint64_t flags;                 // array of flags that describe the status of the page frame
    unsigned int property;          // the num of free block, used in first fit pm manager
    list_entry_t page_link;         // free list link
};
```

值得一提的是，在buddy system中，我们把Page结构中的property属性设置为**页块的阶数**，而不是页的数量。通过这三类数据结构，我们实现了一个**伪二叉树**， 

- **free_area_t 数组作为树的层级索引**

- **Page 结构作为树的节点**

并且可以直接访问特定大小的自由链表，减少搜索时间。

### 头文件引入与全局定义

```C++
#include <pmm.h>
#include <list.h>
#include <string.h>
#include <stdio.h>
#include <buddy_system_pmm.h>

#define MAX_ORDER  4      // 最大阶数

static free_area_t free_area[MAX_ORDER + 1]; // 每个阶的自由区
```

首先引入一些头文件，并在此设置我们的buddy system算法所定义的块的最大阶数`MAX_ORDER`为4，并定义一个链表数组`free_area`用于索引0~4阶大小的的空闲块。`MAX_ORDER`设置得太小可能导致大页请求无法分配，设置过大会增加分块次数，影响性能，所以我们要看实际使用的最大页数的值来设置`MAX_ORDER`，尽可能刚好覆盖最大页数，可以通过动态调整实现。

### 功能函数

```C++
// 获取总空闲页数
static size_t get_nr_free(void) 
```

```C++
// 计算阶数：找到能容纳n页的最小2的幂
static unsigned int get_order(size_t n)
```

```C++
// 检查页面是否在自由链表中
static int page_in_free_list(struct Page *page) 
```

```C++
// 检查是否是伙伴关系
static int is_buddy(struct Page *page, unsigned int order) 
```

```C++
// 找到伙伴页
static struct Page *get_buddy(struct Page *page) 
```

```C++
// 对外接口：获取总空闲页数
static size_t buddy_nr_free_pages(void) {
    return get_nr_free();
}
```

我们先来定义一些功能函数，这些功能函数的作用已经全部在注释中写出，用于辅助buddy system的搭建。

接下来将实现伙伴系统的核心功能：初始化、内存分配和内存释放。

### **初始化阶段**

```C++
函数 buddy_init():
    对于 i 从 0 到 MAX_ORDER:
        初始化 free_area[i].free_list 为空链表
        设置 free_area[i].nr_free = 0
```

- 这个函数初始化伙伴系统的自由链表数组。
- 对每个阶数i（从0到`MAX_ORDER`），初始化该阶的自由链表并将该阶的空闲块数量置为0，后续可直接通过**O(1)访问**特定大小的自由链表。
- 这是伙伴系统开始管理内存前的必要步骤，确保所有自由链表为空，后续可以正确添加空闲块。

```C++
函数 buddy_init_memmap(base, n):
    断言 n > 0
    
    // 初始化所有页面状态
    对于 p 从 base 到 base + n - 1:
        p.flags = 0
        p.ref = 0
        ClearPageProperty(p)
    
    remaining = n           // 剩余待初始化的页数
    current = base          // 当前处理位置
    
    循环 当 remaining > 0:
        // 找到当前能分配的最大2的幂块
        order = get_order(remaining)
        block_size = 1 << order
        
        // 如果剩余内存不够完整块，降低阶数
        循环 当 block_size > remaining 且 order > 0:
            order = order - 1
            block_size = 1 << order
        
        // 初始化这个内存块
        current.property = order
        SetPageProperty(current)                    // 标记为头页
        list_add(free_area[order].free_list, current.page_link)
        free_area[order].nr_free = free_area[order].nr_free + 1
        
        // 移动到下一个位置
        current = current + block_size
        remaining = remaining - block_size
```

- 这个函数用于初始化一段连续的物理内存，将其划分为多个2的幂次大小的块，并加入到对应阶数的自由链表中，**核心思想：优先创建大块，减少初始内存碎片**。
- 首先，初始化这段内存中每个页面的标志位、引用计数，并清除Property标志（表示不是空闲块头页）。
- 然后，循环处理剩余内存，每次尽可能分配最大的2的幂次大小的块：
  - 使用`get_order(remaining)`计算当前剩余内存能容纳的最大阶数，即最大的k使得2^k <= remaining。
  - 如果计算出的块大小大于剩余内存，则逐步降低阶数，直到块大小不超过剩余内存。
  - 设置当前块的头页的property为阶数，设置Property标志，并将该头页加入到对应阶的自由链表中。
  - 更新当前指针和剩余内存大小。
- 这样，一段连续的内存就被划分成了多个不同大小的块，每个块都是2的幂次大小，并且被加入到对应的自由链表中。

### **分配流程**

```C++
函数 buddy_alloc_pages(n):
    断言 n > 0
    如果 n > 总空闲页数:
        返回 NULL
    
    order = get_order(n)            // 计算所需最小阶数
    current_order = order
    
    // 向上寻找可用的块
    循环 当 current_order <= MAX_ORDER:
        如果 free_area[current_order].free_list 不为空:
            跳出循环
        current_order = current_order + 1
    
    如果 current_order > MAX_ORDER:
        返回 NULL
    
    // 取出可用块
    page = 从 free_area[current_order].free_list 取出第一个块
    free_area[current_order].nr_free = free_area[current_order].nr_free - 1
    
    // 分割大块直到合适大小
    循环 当 current_order > order:
        current_order = current_order - 1
        
        // 创建伙伴块
        buddy = page + (1 << current_order)
        buddy.property = current_order
        SetPageProperty(buddy)
        
        // 将伙伴加入自由链表
        list_add(free_area[current_order].free_list, buddy.page_link)
        free_area[current_order].nr_free = free_area[current_order].nr_free + 1
        
        // 更新当前块属性
        page.property = current_order
    
    // 标记为已分配
    ClearPageProperty(page)
    
    返回 page
```

- 这个函数用于分配至少n个连续页面。
- 首先检查是否有足够的空闲页面，如果没有则返回NULL。
- 计算所需阶数order，即最小的k使得2^k >= n。
- 从order开始向上扫描自由链表，找到第一个非空的自由链表，该链表中的块大小至少为2^current_order。
- 如果找不到可用的块，返回NULL。
- 从找到的自由链表中取出第一个块，这个块的大小是2^current_order。
- 如果当前块的大小大于所需（即current_order > order），则进行分割：
  - 将块分割成两个大小为2^(current_order-1)的伙伴块。
  - 将其中一个伙伴块（即buddy）设置为空闲，并加入到current_order-1阶的自由链表中。
  - 另一个伙伴块（即当前块）继续用于分配，并更新其阶数为current_order-1。
  - 重复分割直到当前块的大小正好为2^order。
- 清除分配块的Property标志，表示它已被分配。
- 返回分配块的地址。

### **释放流程**

```C++
函数 buddy_free_pages(base, n):
    断言 n > 0
    
    order = get_order(n)
    page = base
    
    // 设置释放块的基本属性
    page.property = order
    SetPageProperty(page)
    current_order = order
    
    // 尝试合并伙伴块
    循环 当 current_order < MAX_ORDER:
        buddy = get_buddy(page)     // 计算伙伴地址
        
        // 检查伙伴是否可合并
        如果 buddy 不存在 或 
           buddy.property != current_order 或
           不是 PageProperty(buddy) 或
           buddy 不在自由链表中:
            跳出循环
        
        // 移除伙伴块
        从 free_area[current_order].free_list 移除 buddy
        free_area[current_order].nr_free = free_area[current_order].nr_free - 1
        ClearPageProperty(buddy)
        
        // 确定合并后的头页（取地址较小的）
        如果 page > buddy:
            page = buddy
        
        // 提升阶数
        current_order = current_order + 1
        page.property = current_order
    
    // 将最终块加入自由链表
    list_add(free_area[current_order].free_list, page.page_link)
    free_area[current_order].nr_free = free_area[current_order].nr_free + 1
```

- 这个函数用于释放之前分配的连续页面。
- 计算释放块的阶数order。
- 设置释放块的头页属性：阶数为order，并设置Property标志，表示这是一个空闲块的头页。
- 然后尝试合并伙伴块：
  - 循环从当前阶开始，直到最大阶，每次循环尝试合并伙伴。
  - 通过`get_buddy(p)`计算当前块的伙伴块。
  - 检查伙伴块是否存在、是否是同一阶、是否是空闲的（通过`is_buddy`函数，该函数检查伙伴块是否在自由链表中且属性正确）。
  - 如果伙伴块满足合并条件，则将伙伴块从当前阶的自由链表中移除，并清除伙伴块的Property标志。
  - 合并两个块，形成一个大一阶的块，并更新当前阶数`current_order`和合并后的块的头页属性。
  - 合并后，继续尝试与伙伴合并，直到无法合并为止。
- 将最终合并后的块加入到对应阶的自由链表中，并增加该阶的空闲块数量。

