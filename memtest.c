#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdint.h>
#include <time.h>
#include "umalloc.h"

#define PGSIZE 4096
// 测试程序里同时管理的最大内存块数量
#define MAX_ALLOCS 200  // 在 xv6 里面用户栈的大小为 1 页，所以要注意不能创建过多变量导致栈溢出

// ============================= 辅助函数 =============================
// 简单的伪随机数生成器
static unsigned long int next = 1;
int rand(void) {
    next = next * 1103515245 + 12345;
    return (unsigned int)(next / 65536) % 32768;
}
void srand(unsigned int seed) {
    next = seed;
}

// 验证内存写入和读取
void check_data_integrity(char *ptr, int size, char pattern) {
    for(int i = 0; i < size; i++) {
        ptr[i] = pattern;  // 验证写入
    }
    for(int i = 0; i < size; i++) {
        if(ptr[i] != pattern) {  // 验证读取
            printf("ERROR: DATA CORRUPTION at %p! Expected %d, got %d\n", &ptr[i], pattern, ptr[i]);
            exit(1);
        }
    }
}

// 检查两个内存块是否重叠
int check_overlap(void *p1, int s1, void *p2, int s2) {
    uintptr_t start1 = (uintptr_t)p1;
    uintptr_t end1 = start1 + s1;
    uintptr_t start2 = (uintptr_t)p2;
    uintptr_t end2 = start2 + s2;

    if (start1 < end2 && start2 < end1) {
        return 1; // 重叠
    }
    return 0; // 不重叠
}

// 获取当前纳秒数
uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000L + ts.tv_nsec;
}


// ============================= 测试用例 =============================
void test_basic_correctness() {
    printf("\n[Test 1] 基础正确性与重叠检测...\n");
    void *ptrs[50];
    int sizes[50];
    int count = 50;

    // 连续申请 50 块内存
    for(int i = 0; i < count; i++) {
        sizes[i] = (rand() % 128) + 8; // 8到136字节
        ptrs[i] = umalloc(sizes[i]);
        if(ptrs[i] == 0) {
            printf("ERROR: Malloc failed at index %d\n", i);
            exit(1);
        }
        // 写入数据验证
        check_data_integrity((char*)ptrs[i], sizes[i], (char)i);
    }

    // 检查重叠 (O(N^2))
    for(int i = 0; i < count; i++) {
        for(int j = i + 1; j < count; j++) {
            if(check_overlap(ptrs[i], sizes[i], ptrs[j], sizes[j])) {
                printf("ERROR: OVERLAP DETECTED between %p (size %d) and %p (size %d)\n", 
                      ptrs[i], sizes[i], ptrs[j], sizes[j]);
                exit(1);
            }
        }
    }

    // 显示内存状态
    fragmentation_stats();
    
    // 释放
    for(int i = 0; i < count; i++) {
        ufree(ptrs[i]);
    }
    printf("成功: 基础读写与重叠检查通过。\n");
}

void test_coalescing() {
    printf("\n[Test 2] 内存合并 (Coalescing) 逻辑测试...\n");
    fragmentation_stats(); // 初始状态

    // 1. 分配三个连续块 A, B, C
    void *a = umalloc(128);
    void *b = umalloc(520);
    void *c = umalloc(300);
    
    printf("Allocated: A=%p, B=%p, C=%p\n", a, b, c);

    // 2. 释放 A 和 B，留下 C 作为中间空洞
    ufree(a);
    ufree(b);
    printf("Freed A and B. C is still holding the middle.\n");
    fragmentation_stats();

    // 3. 尝试分配一个 600 字节的大块，由于之前已经释放了 A 和 B，所以这个时候新申请的内存应该直接从 A 开始
    void *d = umalloc(600); 
    printf("Allocated D = %p\n", d);

    if (d == a) {
        printf("成功: 成功重用合并后的起始地址 (验证通过)\n");
    } else {
        printf("警告: 未重用起始地址\n");
    }
    ufree(c);
    ufree(d);
}

void test_stress_random() {
    printf("\n[Test 3] 随机压力测试 (模拟真实负载)...\n");
    void *ptrs[MAX_ALLOCS];  // 保存第 i 个槽位当前 malloc 返回的指针
    int sizes[MAX_ALLOCS];  // 保存对应分配的大小
    int allocated[MAX_ALLOCS]; // 当前槽位是否已分配，0: free, 1: allocated
    
    for(int i=0; i<MAX_ALLOCS; i++) allocated[i] = 0;  // 初始化状态

    int ops = 2000; // 总共执行 2000 次堆操作
    
    for(int i = 0; i < ops; i++) {
        int idx = rand() % MAX_ALLOCS;
        
        if (allocated[idx]) {
            // 已分配，则释放
            ufree(ptrs[idx]);
            allocated[idx] = 0;
        } else {
            // 未分配，则分配
            int size = (rand() % 256) + 1; // 1-256 字节
            ptrs[idx] = umalloc(size);
            if (ptrs[idx]) {
                allocated[idx] = 1;
                sizes[idx] = size;
                // 写入一个模式字节，稍后检查是否被其他分配踩踏
                memset(ptrs[idx], (char)(idx & 0xFF), size);
            }
        }
    }

    // 检查内存状态
    fragmentation_stats();

    // 最终清理并检查数据完整性
    for(int i = 0; i < MAX_ALLOCS; i++) {
        if(allocated[i]) {
            // 检查之前写入的数据是否还在，验证没有发生堆溢出/踩踏
            char expected = (char)(i & 0xFF);
            char *p = (char*)ptrs[i];
            for(int k=0; k<sizes[i]; k++) {
                if(p[k] != expected) {
                    printf("ERROR: DATA CORRUPTION in Stress Test at idx %d!\n", i);
                    exit(1);
                }
            }
            ufree(ptrs[i]);
        }
    }
    printf("成功: 随机压力测试通过 (2000 ops)。\n");
    fragmentation_stats();
}

void test_performance_benchmark() {
    printf("\n[Test 4] 性能基准测试 (Time & Fragmentation)...\n");
    
    #define BENCH_COUNT 100  // 测试次数
    static void *temp[BENCH_COUNT];  // 保存分配的指针
    int size = 25;  // 每次分配的大小
    int runs = 10;  // 重复测试的次数
    
    // 1. 速度测试
    uint64_t start_time = get_time_ns();

    for(int run = 0; run < runs; run++) {
        for(int i = 0; i < BENCH_COUNT; i++) {
            temp[i] = umalloc(size); 
        }
        for(int i = 0; i < BENCH_COUNT; i++) {
            ufree(temp[i]);
        }
    }

    uint64_t end_time = get_time_ns();
    uint64_t total_time = end_time - start_time;
    
    printf("  >> [速度测试] 完成 %d 次分配/释放\n", BENCH_COUNT * runs);
    printf("  >> 总耗时: %lu ns | 平均每轮: %lu ns\n", total_time, total_time / runs);

    // 2. 内存碎片测试
    printf("\n  >> 正在制造内存碎片...\n");
    
    // 重新记录周期（仅针对碎片化场景的逻辑）
    uint64_t frag_start = get_time_ns();

    // 分配不同尺寸的块 (8B - 512B)
    for(int i = 0; i < BENCH_COUNT; i++) {
        int var_size = (i % 64 + 1) * 8 + 1;  // +1 是为了凸显外部碎片
        temp[i] = umalloc(var_size);
    }

    // 制造空洞，释放所有奇数序号的块
    // 此时内存中会出现 [已用]-[空闲]-[已用]-[空闲] 的交替布局
    for(int i = 1; i < BENCH_COUNT; i += 2) {
        if(temp[i]) {
            ufree(temp[i]);
            temp[i] = 0; // 置空防止野指针
        }
    }

    // 打印中间状态，观察外部碎片百分比
    printf("  >> [中间状态] 释放一半块后的碎片统计:\n");
    fragmentation_stats();

    // 填补空洞，重新申请相同尺寸的块
    for(int i = 1; i < BENCH_COUNT; i += 2) {
        int var_size = (i % 64 + 1) * 8;
        temp[i] = umalloc(var_size);
    }

    uint64_t frag_end = get_time_ns();
    printf("  >> [碎片测试] 变长分配混合场景耗时: %lu ns\n", (unsigned long)(frag_end - frag_start));

    // 3. 内存回收测试
    for(int i = 0; i < BENCH_COUNT; i++) {
        if(temp[i]) {
            ufree(temp[i]);
            temp[i] = 0;
        }
    }

    printf("  >> [最终状态] 全部释放后的内存统计 (理想应为 Used:0, 1 block):\n");
    fragmentation_stats();
}

void test_visualization() {
    printf("\n[Test 5] 内存可视化测试...\n");
    
    // 分配一些内存
    // Tip: sizeof(struct mem_block) = 48, 以及需要 8 字节对齐
    void *p1 = umalloc(100);  // 148 + 4 = 152
    void *p2 = umalloc(200);  // 248
    void *p3 = umalloc(50);  // 98 + 6 = 104
    // 总共 152 + 248 + 104 = 504 字节
    
    // 详细可视化
    visualize_memory();
    
    // 释放一些制造碎片
    ufree(p2);
    visualize_memory();
    
    // 再分配
    void *p4 = umalloc(300);
    void *p5 = umalloc(450);
    visualize_memory();
    
    // 清理
    ufree(p1);
    ufree(p3);
    ufree(p4);
    ufree(p5);
}

void* thread_worker(void* arg) {
    int id = *(int*)arg;
    for (int j = 0; j < 100; j++) {
        int size = (j % 64) + 16;
        void *p = umalloc(size); // 这里的 malloc 是你自己实现的
        if (p) {
            memset(p, id, size);
            if (j % 2) ufree(p);
        }
    }
    return NULL;
}

void test_concurrent_threads() {
    printf("\n[Test 6] 并发 Pthread malloc/free 测试...\n");
    int n = 4;
    pthread_t threads[n];
    int tids[n];

    for (int i = 0; i < n; i++) {
        tids[i] = i + 1;
        if (pthread_create(&threads[i], NULL, thread_worker, &tids[i]) != 0) {
            perror("pthread_create failed");
            exit(1);
        }
    }

    for (int i = 0; i < n; i++) {
        pthread_join(threads[i], NULL);
    }

    fragmentation_stats();
    printf("并发线程测试完成。\n");
}


int main(void) {
    printf("=== Starting Advanced Malloc Tests ===\n");
    srand(100); // 固定随机种子保证可复现

    // test_basic_correctness();
    // test_coalescing();
    // test_stress_random();
    test_performance_benchmark();
    // test_visualization();
    // test_concurrent_threads();

    printf("\n=== All Tests Passed Successfully ===\n");
    exit(0);
}