#define _DEFAULT_SOURCE
#include "umalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>


// ==================== 数据结构 ====================
struct {
  pthread_mutex_t lock;  // 用户空间锁
  struct mem_block *globallist;  // 全局链表头指针，低地址到高地址排序
  size_t used_memory;
  size_t total_memory;
} mem = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0};


// ==================== 常量工具函数 ====================
// 工具宏
#define ALIGN(size) (((size) + 7) & ~7)  // 8字节对齐
#define BLOCK_SIZE(size) (ALIGN(size + sizeof(struct mem_block)))  // 包括元数据的内存块大小
#define GET_BLOCK(ptr) ((struct mem_block*)((char*)(ptr) - sizeof(struct mem_block)))
#define PAYLOAD_SIZE(block) ((block)->size - sizeof(struct mem_block))  // 内存块有效载荷大小

// 快速适配分配
#define QUICK_LIST_COUNT 10  // 这里设定以 32 字节为基准，每级翻倍
struct mem_block *quick_lists[QUICK_LIST_COUNT];

// 初始化所有快速链表为空
void init_quick_lists() {
  for (size_t i = 0; i < QUICK_LIST_COUNT; i++) quick_lists[i] = NULL;
}

// 根据大小选择快速链表索引
int quick_list_index(size_t size) {
  int index = 0;
  while (size > 32 && index < QUICK_LIST_COUNT - 1) {
    size >>= 1; 
    index++;  
  }
  return index;
}

// 将块从快速链表中摘除
void remove_from_quick_list(struct mem_block *block) {
  if (!block) return;
  int index = quick_list_index(block->size);
  if (block->next) block->next->prev = block->prev;
  if (block->prev) block->prev->next = block->next;
  if (quick_lists[index] == block) quick_lists[index] = block->next;
  block->prev = NULL;
  block->next = NULL;
}

// 扩展堆函数
struct mem_block* extend_heap(size_t min_size) {
  size_t pgsize = sysconf(_SC_PAGESIZE);  // 获取系统页大小
  size_t extend_size = (min_size < pgsize) ? pgsize : ((min_size + pgsize - 1) & ~(pgsize - 1));  // 向上取整到页边界
  
  void *new_mem = sbrk(extend_size);  // 向内核申请内存
  if (new_mem == (void*)-1) return NULL;  // 内存不足

  // 初始化新内存块
  struct mem_block *new_block = (struct mem_block*)new_mem;
  new_block->is_free = 1;
  new_block->size = extend_size;
  new_block->applyed_size = 0;
  new_block->prev = NULL;
  new_block->next = NULL;
  new_block->prev_global = NULL;
  new_block->next_global = NULL;
  
  // 拓展来的地址是高地址，添加到全局链表的尾部
  struct mem_block *curr = mem.globallist;
  if (!curr) {
    mem.globallist = new_block;
  } else {
    while (curr->next_global) curr = curr->next_global;
    curr->next_global = new_block;
    new_block->prev_global = curr;
  }

  // 添加到快速链表
  int index = quick_list_index(new_block->size);
  new_block->next = quick_lists[index];
  if (quick_lists[index]) quick_lists[index]->prev = new_block;
  quick_lists[index] = new_block;

  printf("extend_heap: added %zu bytes at %p\n", extend_size, new_mem);
  return new_block;
}


// ==================== 初始化 =====================
void
mem_init(size_t heap_size) {
  pthread_mutex_lock(&mem.lock);  // 获取锁
  // 如果已经初始化过了，直接解锁退出
  if (mem.total_memory > 0) {
    pthread_mutex_unlock(&mem.lock);
    return;
  }
  
// 向内核申请内存
    void *heap_start = sbrk(heap_size);
    if (heap_start == (void*)-1) {
        pthread_mutex_unlock(&mem.lock); // 失败解锁
        perror("mem_init: sbrk failed");
        exit(1);
    }

  // 初始化第一个内存块
  struct mem_block *first_block = (struct mem_block*)heap_start;
  first_block->size = heap_size;
  first_block->applyed_size = 0;
  first_block->is_free = 1;
  first_block->next_global = NULL;
  first_block->prev_global = NULL;

  mem.globallist = first_block;
  mem.total_memory = heap_size;
  mem.used_memory = 0;

  // 初始化快速适配链表
  init_quick_lists();
  int index = quick_list_index(first_block->size);
  first_block->prev = NULL;
  first_block->next = NULL;
  quick_lists[index] = first_block;

  // 释放锁
  pthread_mutex_unlock(&mem.lock);
}


// ==================== 快速适配分配 ===================
void*
umalloc_quick_fit(size_t nbytes) { 
  if (nbytes <= 0) return NULL;

  pthread_mutex_lock(&mem.lock);  // 获取锁
  size_t required_size = BLOCK_SIZE(nbytes);  // 计算所需内存块大小
  int index = quick_list_index(required_size);
  struct mem_block *block = NULL;

  // 现在快速链表中查找
  for (size_t i = index; i < QUICK_LIST_COUNT; i++) {
    block = quick_lists[i];  // 获取桶i的链表头
    while (block) {
      if (block->is_free && block->size >= required_size) {  
        // 说明找到了合适的块
        goto found;
      }
      block = block->next;
    }
  }

  // 快速链表没找到，扩展堆
  block = extend_heap(required_size);  // 扩展堆
  if (!block) {  // 说明内存不足
    pthread_mutex_unlock(&mem.lock);
    return NULL;
  }
found:
  remove_from_quick_list(block);  // 从快速链表中摘除
  block->is_free = 0;  // 标记为已分配
  block->applyed_size = nbytes;

  // 分割块
  if (block->size > required_size + sizeof(struct mem_block) + 8) {
    struct mem_block *new_block = (struct mem_block*)((char*)block + required_size);
    new_block->size = block->size - required_size;
    new_block->applyed_size = 0;
    new_block->is_free = 1;

    // 插入剩余块到快速链表
    int new_index = quick_list_index(new_block->size);
    new_block->next = quick_lists[new_index];  // 将新块插入到快速链表中作为头指针
    new_block->prev = NULL;
    if (quick_lists[new_index]) quick_lists[new_index]->prev = new_block;
    quick_lists[new_index] = new_block;

    // 修改全局链表
    new_block->prev_global = block;
    new_block->next_global = block->next_global;
    if (block->next_global) block->next_global->prev_global = new_block;
    block->next_global = new_block;

    block->size = required_size;
  }

  mem.used_memory += block->size;

  pthread_mutex_unlock(&mem.lock); // 替换锁
  return (void*)((char*)block + sizeof(struct mem_block));  // 返回用户可用的内存地址
}


// ==================== 最佳适应分配===================
void*
umalloc_best_fit(size_t nbytes) {
  if (nbytes <= 0) return NULL;

  pthread_mutex_lock(&mem.lock);  // 获取锁
  size_t required_size = BLOCK_SIZE(nbytes);  // 计算所需内存块大小
  struct mem_block *best = NULL;
  struct mem_block *curr = mem.globallist;

  // 寻找最佳适配块
  while (curr) {
    if (curr->is_free && curr->size >= required_size) {
      if (best == NULL || best->size > curr->size) {
        best = curr;
      }
    }
    curr = curr->next_global;
  }

  // 没有找到最合适的块
  if (!best) {
    best = extend_heap(required_size);  // 扩展堆
    if (!best) {  // 说明内存不足
      pthread_mutex_unlock(&mem.lock);
      return NULL;
    }
  }

  // 分割块 (剩余空间足够大)：剩余空间 > 元数据大小 + 最小用户块
  best->is_free = 0;
  best->applyed_size = nbytes;  // 记录用户申请的大小
  remove_from_quick_list(best);  // 从快速链表中摘除
  if (best->size > required_size + sizeof(struct mem_block) + 8) {
    // 设置并计算新块
    struct mem_block *new_block = (struct mem_block*)((char*)best + required_size);  // 在C语言中，指针加减法是以指向类型的大小为单位
    new_block->size = best->size - required_size;
    new_block->applyed_size = 0;
    new_block->is_free = 1;

    // 更新全局链表
    new_block->next_global = best->next_global;
    new_block->prev_global = best;
    if (best->next_global) best->next_global->prev_global = new_block;
    best->next_global  = new_block;
    best->size = required_size;

    // 新块添加到快速链表
    int new_index = quick_list_index(new_block->size);
    new_block->next = quick_lists[new_index];
    new_block->prev = NULL;
    if (quick_lists[new_index]) quick_lists[new_index]->prev = new_block;
    quick_lists[new_index] = new_block;
  }

  mem.used_memory += best->size;
  pthread_mutex_unlock(&mem.lock);  // 释放锁

  return (void*)((char*)best + sizeof(struct mem_block));  // 返回用户可用的内存地址, 藏内部管理信息（元数据）
}


// ==================== 内存释放 ===================
void
ufree(void *pa)
{
  if (pa == 0) return;

  pthread_mutex_lock(&mem.lock);

  struct mem_block *block = GET_BLOCK(pa);

  // 安全检查
  if (block->is_free) {
      pthread_mutex_unlock(&mem.lock);
      return;
  }

  block->is_free = 1;  // 标记为空闲
  remove_from_quick_list(block);  // 先移除自己
  mem.used_memory -= block->size;
  block->applyed_size = 0;  // 重置申请的大小

  // 合并前一个空间块
  if (block->prev_global && block->prev_global->is_free) {
    remove_from_quick_list(block->prev_global);  // 从快速链表中移除前一个块
    block->prev_global->size += block->size;
    block->prev_global->next_global = block->next_global;
    if (block->next_global) block->next_global->prev_global = block->prev_global;
    block = block->prev_global;
  }

  // 合并后一个空间块
  if (block->next_global && block->next_global->is_free) {
    remove_from_quick_list(block->next_global);  // 从快速链表中移除后一个块
    struct mem_block *next_next = block->next_global->next_global;
    block->size += block->next_global->size; 
    block->next_global = next_next;
    if (next_next) next_next->prev_global = block;
  }

  // 更新快速链表
  int index = quick_list_index(block->size);
  block->next = quick_lists[index];
  if (quick_lists[index]) quick_lists[index]->prev = block;
  quick_lists[index] = block;
  block->prev = 0;

  pthread_mutex_unlock(&mem.lock);
}


// =================== 统计 ==================
// 碎片统计
void 
fragmentation_stats() {
  pthread_mutex_lock(&mem.lock); // 替换锁

  size_t total_free = 0;  // 记录总空闲内存
  size_t largest_free = 0;  // 记录最大空闲块大小
  size_t sum_unapplyed_size = 0;  // 记录申请的总大小
  size_t block_count = 0;  // 记录空闲块数量

  struct mem_block *curr = mem.globallist;
  while (curr) {
    if (curr->is_free) {
      total_free += curr->size; 
      if (curr->size > largest_free) {  // 记录外部碎片
        largest_free = curr->size;
      }
      block_count++;
    } else if (curr->is_free == 0 && curr->applyed_size > 0) {  // 记录内部碎片
      sum_unapplyed_size += (curr->size - curr->applyed_size - sizeof(struct mem_block));  // 计算未使用的有效载荷总和 (不包括元数据)
    }
    curr = curr->next_global;
  }

  size_t external_frag = 0;  // 记录外部碎片
  if (total_free > 0) {
    // 外部碎片率 = (1 - 最大连续空闲块大小 / 总空闲内存) × 100%
    external_frag = (total_free - largest_free) * 10000 / total_free;  // 计算外部碎片百分比，这里保留两位小数
  }

  size_t internal_frag = 0;  // 记录内部碎片，这里没有将元数据也算入内部碎片
  if (sum_unapplyed_size > 0) {
    // 内部碎片率 = 未使用的有效载荷总和 / 已分配的块总大小 × 100%
    internal_frag = sum_unapplyed_size * 10000 / mem.used_memory;  // 计算内部碎片百分比，这里保留两位小数
  }

  printf("Memory Stats:\n");
  printf("  Total: %zu bytes\n", mem.total_memory);
  printf("  Used: %zu bytes\n", mem.used_memory);
  printf("  Free: %zu bytes in %zu blocks\n", total_free, block_count);
  printf("  Largest free block: %zu bytes\n", largest_free);
  printf("  External: %zu.%02zu%%\n", external_frag / 100, external_frag % 100);
  printf("  Internal: %zu.%02zu%%\n", internal_frag / 100, internal_frag % 100);

  pthread_mutex_unlock(&mem.lock);
}


// =================== 内存可视化 ==================
void
visualize_memory() {
  pthread_mutex_lock(&mem.lock);

  struct mem_block *curr = mem.globallist;
  int total_blocks = 0;  // 内存总块数
  int free_blocks = 0;  // 空闲块数
  int used_blocks = 0;  // 已用块数
  size_t total_free = 0;  // 空闲内存总大小
  size_t total_used = 0;  // 已用内存总大小

  // 统计内存块信息
  while (curr) {
    total_blocks++;
    if (curr->is_free) {
      free_blocks++;  // 空闲块
      total_free += curr->size;
    } else {
      used_blocks++;  // 已用块
      total_used += curr->size;
    }
    curr = curr->next_global;
  }

  int util = 0;  // 内存利用率
  if (mem.total_memory > 0) {
    util = (int)(total_used * 100 / mem.total_memory);
  }

  curr = mem.globallist;

  // 打印内存布局
  printf("\n+------------------------------------------------------------+\n");
  printf("|                    MEMORY LAYOUT                           |\n");
  printf("+------------------------------------------------------------+\n");
  printf("| Total: %zu  Used: %zu  Free: %zu |\n",
        mem.total_memory, total_used, total_free);
  printf("| Blocks: %d (Used: %d Free: %d) Util: %d%% |\n",
         total_blocks, used_blocks, free_blocks, util);
  printf("+------------------------------------------------------------+\n");

  if (curr) {
    printf("| Addr Range: %p - %p |\n",
           curr,
           (void *)((char *)curr + mem.total_memory));
  }

  printf("+------------------------------------------------------------+\n");
  printf("| Address        | Memory State                              |\n");
  printf("+------------------------------------------------------------+\n");

  const size_t ROW_SIZE = 256;  // 每一行代表 256 字节的内存空间
  const size_t CHAR_SCALE = 8;  // 每个字符代表 8 字节的内存空间
  size_t rows = mem.total_memory / ROW_SIZE;  // 计算需要多少行
  uintptr_t base = (uintptr_t)mem.globallist;  // 基地址

  for (size_t row = 0; row < rows; row++) {
    uintptr_t row_addr = base + row * ROW_SIZE;  // 计算当前行的基地址
    printf("| %p | ", (void *)row_addr);

    // 逐字符绘制该行
    for (size_t i = 0; i < ROW_SIZE / CHAR_SCALE; i++) {
      uintptr_t addr = row_addr + i * CHAR_SCALE;  // 计算当前字符的基地址
      struct mem_block *b = mem.globallist;
      char c = ' ';

      while (b) {
        uintptr_t bs = (uintptr_t)b;
        uintptr_t be = bs + b->size;
        // 找到当前字符所在的内存块，并根据当前内存块的状态绘制字符
        if (addr >= bs && addr < be) {
          c = b->is_free ? '.' : '#';
          break;
        }
        b = b->next_global;
      }
      printf("%c", c);
    }
    printf(" |\n");
  }

  printf("+------------------------------------------------------------+\n");
  printf("| Legend: # = Used (%zu B)   . = Free (%zu B)                    |\n", CHAR_SCALE, CHAR_SCALE);
  printf("+------------------------------------------------------------+\n");

  pthread_mutex_unlock(&mem.lock);
}


// =================== 统一 malloc 接口 ==================
void*
umalloc(size_t nbytes)
{
  // 首次调用时初始化内存管理器
  if (mem.total_memory == 0) {
    mem_init(4096);
  }

  // return umalloc_best_fit(nbytes);  // 使用最佳适应分配
  return umalloc_quick_fit(nbytes);  // 使用快速适配分配
}
