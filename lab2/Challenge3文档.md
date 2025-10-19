# 硬件的可用物理内存范围的获取方法

## 设备树（DTB）中解析

在`pmm_init()`中，我们需要进行`page_init()`,而操作系统对物理内存的探测，就是在这里进行的。

```c
// detect physical memory space, reserve already used memory,
// then use pmm->init_memmap to create free page list
page_init();
```

我们仔细看`page_init()`的实现过程：

```c
static void page_init(void) {
    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    uint64_t mem_begin = get_memory_base();
    uint64_t mem_size  = get_memory_size();
    if (mem_size == 0) {
        panic("DTB memory info not available");
    }
    uint64_t mem_end   = mem_begin + mem_size;

    cprintf("physcial memory map:\n");
    cprintf("  memory: 0x%016lx, [0x%016lx, 0x%016lx].\n", mem_size, mem_begin,
            mem_end - 1);

    uint64_t maxpa = mem_end;

    if (maxpa > KERNTOP) {
        maxpa = KERNTOP;
    }

    extern char end[];

    npage = maxpa / PGSIZE;
    //kernel在end[]结束, pages是剩下的页的开始
    pages = (struct Page *)ROUNDUP((void *)end, PGSIZE);

    for (size_t i = 0; i < npage - nbase; i++) {
        SetPageReserved(pages + i);
    }

    uintptr_t freemem = PADDR((uintptr_t)pages + sizeof(struct Page) * (npage - nbase));

    mem_begin = ROUNDUP(freemem, PGSIZE);
    mem_end = ROUNDDOWN(mem_end, PGSIZE);
    if (freemem < mem_end) {
        init_memmap(pa2page(mem_begin), (mem_end - mem_begin) / PGSIZE);
    }
}
```

第一行首先定义了物理内存和虚拟内存的线性偏差，它意味着**内核虚拟地址 = 物理地址 + PHYSICAL_MEMORY_OFFSET**（`0xFFFFFFFF40000000`），这些值都在**memlayout.h**中定义。地址map计算的例子比如： `#define KERNBASE 	0xFFFFFFFFC0200000` ,它等于` 0x80200000(物理内存里内核的起始位置, KERN_BEGIN_PADDR) + 0xFFFFFFFF40000000(偏移量, PHYSICAL_MEMORY_OFFSET)`。

下面几行，我们直接调用函数：

```c
uint64_t mem_begin = get_memory_base();
uint64_t mem_size  = get_memory_size();
```

`get_memory_base()`函数和`get_memory_size()`函数都在`dtb.h`中定义：

```c
#ifndef __KERN_DRIVER_DTB_H__
#define __KERN_DRIVER_DTB_H__

#include <defs.h>

// Defined in entry.S
extern uint64_t boot_hartid;
extern uint64_t boot_dtb;

void dtb_init(void);
uint64_t get_memory_base(void);
uint64_t get_memory_size(void);

#endif /* !__KERN_DRIVER_DTB_H__ */
```

所以，物理内存的探测可以通过设备树信息获取。

## 通过 Bootloader 参数传递

Bootloader（OpenSBI） 直接将内存布局作为参数传入内核入口。因为他具有**M-mode**的权限，拥有对所有物理资源的访问权。

## 通过模拟器（QEMU）命令行指定

在 QEMU 启动参数中指定内存大小：

```bash
qemu-system-riscv64 -m 512M ...
```

QEMU 会在设备树或硬件仿真中自动反映为 `0x80000000 ~ 0xA0000000`。

## 通过 BIOS / 固件内存映射（x86 平台）

在 PC 体系上（如 xv6, Linux/x86），可通过 BIOS 提供的 **e820 memory map**。
