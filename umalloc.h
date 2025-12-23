#ifndef _UMALLOC_H_
#define _UMALLOC_H_

#include <stddef.h>
#include <stdint.h>

struct mem_block {
  size_t size;
  int is_free;
  size_t applyed_size;
  
  // 用于快速适配桶的双向链表 (指针大小都是 8 字节)
  struct mem_block *prev;  // 在quick_lists中的前驱
  struct mem_block *next;  // 在quick_lists中的后继
  
  // 用于全局地址排序的双向链表
  struct mem_block *prev_global;  // 在globallist中的前驱
  struct mem_block *next_global;  // 在globallist中的后继
};

// 接口声明
void mem_init(size_t heap_size);
void* umalloc(size_t nbytes);
void ufree(void *ptr);
void fragmentation_stats(void);
void visualize_memory(void);

#endif