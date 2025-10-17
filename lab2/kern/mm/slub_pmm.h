#ifndef __KERN_MM_SLUB_PMM_H__
#define __KERN_MM_SLUB_PMM_H__

#include <pmm.h>

// SLUB分配器接口
void slub_init(void);
void* slub_alloc(size_t size);
void slub_free(void* ptr);
void slub_check(void);

// 获取SLUB分配器的统计信息
size_t slub_allocated_memory(void);
size_t slub_total_slabs(void);

#endif /* !__KERN_MM_SLUB_PMM_H__ */