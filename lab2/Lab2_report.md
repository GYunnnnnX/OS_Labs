# 操作系统课程ucore Lab2

## 练习一：理解first-fit 连续物理内存分配算法

在first-fit算法中，内存分配器维护着一个空闲块列表（`free list`）。当接收到内存分配请求时，它会沿着该列表查找第一个足够大的空闲块来满足请求。如果找到的空闲块明显大于所需大小，则通常会将其拆分，一部分分配给请求方，剩余部分重新加入`free list`中。释放时把释放块按地址顺序插回链表并尝试与前后块合并。

### `default_init(void)`

这个函数的功能很简单，主要作用是初始化管理结构。它的实现代码只有两行：`list_init(&free_list)`初始化空闲链表头；`nr_free = 0`将空闲页计数置 0,为后续的内存映射初始化和分配/释放做准备。

### `default_init_memmap(struct Page *base, size_t n)`

这个函数的功能是：在内核启动时，对一段连续的物理页范围进行“加入空闲链表管理”（**注册**）操作。调用链是 `kern_init -> pmm_init -> page_init -> init_memmap -> pmm_manager->init_memmap`。

该函数具体的操作是：

1. 对该区间（`[base,base+n)`）内每一页 `p`：

   - `assert(PageReserved(p));`：确保之前被标记为 reserved。
   - `p->flags = p->property = 0; set_page_ref(p, 0);`：清除标志和引用计数。

2. 在区间起始页 `base` 上设置 `base->property = n;`，即设置了空闲块的长度为`n`。 并标记 `SetPageProperty(base)`，把它作为一个空闲块头。

3. `nr_free += n;` 更新全局空闲页计数，增加`n`。

4. 将 `base` 的 `page_link` 插入到 `free_list`，**按物理地址顺序**插入（遍历 free_list 找到第一个比 `base` 大的节点之前插入，保证链表按地址递增）。它的具体实现逻辑是：

   - if循环首先判断检查 `free_list` 是否为空（没有任何空闲块节点，只剩下哨兵）。如果为空，下面的分支会直接把 `base` 作为第一个元素加入链表。

   - 如果`free_list`不为空，需要将 `base` 插入到合适的位置以**保持按地址升序（从低地址到高地址）**。

   - 首先，定义一个游标变量`le`,从`free_list`的哨兵节点开始向后面遍历。遍历链表中的每一个真实节点：

     - `list_next(le)` 把 `le` 移到下一个节点（第一次循环时是第一个真实元素），
     - 当再次回到 `&free_list`（循环一圈）时停止。

     这是写法用来遍历**循环双向循环**链表。

   - 遍历到某个节点时，把当前链表节点 `le` 转换成对应的 `struct Page *`（因为链表存放的是每个 `Page` 的 `page_link` 字段）。

   - 如果`base < page`，表示要插入的 `base`（释放回来的块）位于当前 `page` 节点之前（地址更小）。

     调用`list_add_before(le,&(base->page_link))`进行插入操作，把 `base->page_link` 插入到当前节点 `le`（即 `page`）之前。插入后，`base` 位于 `page` 前面，保持链表地址顺序不变。插入后，即可调用`break`退出循环。

   - 如果`base` 并没有小于当前 `page`（即 `base` 地址更大），并且当前节点 `le` 的下一个节点是哨兵 `&free_list`（说明当前节点是链表中的最后一个真实元素），那么说明 `base` 的地址比链表中所有现有节点都要大，需要把 `base` 插到链表末尾（即最后一个元素之后）。此时，调用`list_add(le, &(base->page_link))`把 `base->page_link` 插入到 `le` 的后面（即链表末尾位置）。由于链表是循环的，插入到 `le` 之后后，`base` 的 next 会指向 `&free_list`，prev 指向 `le`，保持循环结构。

这个函数最终实现了把一段未被占用的页区**注册**为一个空闲块（并按地址有序地放入空闲链表）的操作，供后续分配使用。

### `default_alloc_pages(size_t n)`

这个函数的功能是按 first-fit 策略分配连续 `n` 页，返回首页 `struct Page *`（失败返回 `NULL`）。

1. 首先判断 `if (n > nr_free)`快速判断，需求的大小是不是超出了总空闲的块大小。

2. 然后依然使用写法：`while ((le = list_next(le)) != &free_list)`来从 `free_list` 头开始遍历：对每个链表元素得到对应 `struct Page *p`（通过 `le2page`），检查 `p->property >= n`。

3. 找到第一个满足的 `p`（first-fit），记录该 `page = p`。

   - 检查确保`page != NULL`。
   - 从链表中 `list_del(&(page->page_link));` 删除该块头。

   - 若 `page->property > n`（块大于需要），则**拆分**：在 `page + n` 处创建新的块头 `p2`，设置 `p2->property = page_property - n; SetPageProperty(p2);` 并使用`list_add`函数它插回到链表中（插入位置为原来被删除节点的前驱 `prev`，保持地址顺序）。
   - `nr_free -= n;` 更新空闲页计数。
   - `ClearPageProperty(page);` 将返回的 `page` 的 `property` 标志清除（表明这是已分配的块）。

   返回 `page`。

4. 若未找到合适块则返回 `NULL`。

这个函数最终实现了分配`n`的大小的空间的功能，根据first-fit算法，找到第一个满足需求空间的块，然后将它多余的部分拆分出来，并维护空闲链表。

### `default_free_pages(struct Page *base, size_t n)`

这个函数的功能是释放 `base` 开始的 `n` 连续页，并把它们合并回空闲链表（同时尝试与相邻空闲块合并以减少碎片）。

1. 先对这 `n` 页逐页清理：`p->flags = 0; set_page_ref(p, 0);`（确保不是 reserved、引用为0）。

2. 在 `base` 上设置 `base->property = n; SetPageProperty(base); nr_free += n;`，把释放块当作一个空闲块头。

3. 将 `base` 插入 `free_list`（按地址有序插入，与 `init_memmap` 插入逻辑相同）。

4. 试图与前一个空闲块合并：

   - 取 `le = list_prev(&(base->page_link));` 若不是链表头，则 `p = le2page(le,page_link)`；

   - 若 `p + p->property == base`（前块紧邻），则把两者合并：`p->property += base->property`更新合并块的大小，` ClearPageProperty(base); list_del(&(base->page_link)); base = p;`删除原来的空闲块的信息，从空表中删除，并更新 base 指向新的合并后块。

5. 试图与下一个空闲块合并：

   - `le = list_next(&(base->page_link));` 若不是链表头，则 `p = le2page(le,page_link)`；

   - 若 `base + base->property == p`（后块紧邻），则 `base->property += p->property; ClearPageProperty(p); list_del(&(p->page_link));`（与前向合并的逻辑相同，只不过从表中删除的是`p`的信息）。

6. 前后合并都尝试过之后，就可以退出函数了。

这个函数最终实现了恢复释放页面为可重分配资源，并通过与邻块合并来尽量减少外部碎片。它包含了块的初始化、插入操作和合并操作。

### 算法可改进的地方：

#### **按大小分级的空闲链表（segregated lists）**

维护多个按区间大小划分的 free lists（例如按 1 页，2 页，3–8 页，9–64 页，>64 页），根据请求大小直接选择对应的链表，不必遍历整个空闲链表：分配时只在对应大小段中查找，能显著减少搜索时间。

#### **位图或树结构（例如二叉树 / 红黑树）**

按空闲块大小索引（如 `std::map<size, list>`、size-ordered tree），采用特殊数据结构来进行查找，可以实现近 O(log n) 查找合适块，加速查找效率。

#### Best-Fit 算法

Best-Fit 算法在空闲块中找到 **最小的、能满足请求的块**，能更好的利用小的剩余空间，但实现更慢；折中做法是对拆分后剩余块小于阈值则不拆分，避免产生过多小碎片。

#### **Buddy 系统**

内存块按 **2 的幂次**划分（1、2、4、8 页…），每个空闲块都有一个“伙伴”块。分配时选择最小可容纳请求的幂次块。按 2 的幂次管理块，天生支持按大小快速查找，合并和拆分快速，适合物理页分配场景（尤其内核）。





## 练习二：实现 Best-Fit 连续物理内存分配算法



## 扩展练习Challenge：buddy system（伙伴系统）分配算法



## 扩展练习Challenge：任意大小的内存单元slub分配算法



## 扩展练习Challenge：硬件的可用物理内存范围的获取方法

