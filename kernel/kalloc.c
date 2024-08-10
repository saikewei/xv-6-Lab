// 物理内存分配器，主要用于用户进程、内核栈、页表页以及管道缓冲区。
// 该分配器分配的是完整的 4096 字节大小的页面。

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

// 函数声明：将物理内存从 pa_start 到 pa_end 标记为可用。
void freerange(void *pa_start, void *pa_end);

extern char end[]; // 指向内核结束后的第一个地址。
// 这个地址由链接脚本 kernel.ld 定义。

// 运行队列的结构体，每个节点表示一块空闲的物理内存。
struct run
{
  struct run *next; // 指向下一块空闲内存的指针。
};

// kmem 是一个包含 NCPU 个元素的数组，每个元素都对应一个 CPU 的物理内存管理结构。
// 其中 lock 是自旋锁，用于保护空闲链表 freelist。
struct
{
  struct spinlock lock; // 自旋锁，用于多核环境下的同步。
  struct run *freelist; // 空闲链表，指向空闲内存块。
} kmem[NCPU];           // 定义 NCPU 个这样的结构体数组。

// 初始化内存分配器
void kinit()
{
  char lockname[16]; // 用于存储锁的名字。
  for (int i = 0; i < NCPU; i++)
  {
    // 为每个 CPU 初始化一个自旋锁，并给它们命名。
    snprintf(lockname, sizeof(lockname), "kmem_CPU%d", i);
    initlock(&kmem[i].lock, lockname);
  }
  // 将从内核结束位置（end）到物理内存顶（PHYSTOP）之间的内存标记为空闲。
  freerange(end, (void *)PHYSTOP);
}

// 将指定范围内的物理内存标记为可用，并加入到空闲链表中。
void freerange(void *pa_start, void *pa_end)
{
  char *p;
  // 将起始地址向上对齐到最近的页面边界。
  p = (char *)PGROUNDUP((uint64)pa_start);
  // 遍历每一个 4096 字节的页面，并将其标记为空闲。
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p); // 释放并加入空闲链表。
}

// 释放由 pa 指向的物理内存页面。
// 该页面通常由 kalloc() 分配（初始化时的例外，如 kinit 中所见）。
void kfree(void *pa)
{
  struct run *r; // 临时指针，用于指向将要释放的内存块。
  int cpu_id;    // 存储当前 CPU 的 ID。

  // 如果 pa 不是页面对齐的，或地址超出合法范围，则触发 panic。
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // 将内存填充为 1，以捕捉潜在的悬空引用（dangling references）。
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa; // 将 pa 转换为 run 结构体类型。

  push_off();       // 关闭中断，防止在获取 CPU ID 时发生上下文切换。
  cpu_id = cpuid(); // 获取当前 CPU 的 ID。
  pop_off();        // 恢复中断。

  acquire(&kmem[cpu_id].lock);     // 获取当前 CPU 对应的自旋锁。
  r->next = kmem[cpu_id].freelist; // 将释放的内存块插入空闲链表的头部。
  kmem[cpu_id].freelist = r;       // 更新空闲链表的头指针。
  release(&kmem[cpu_id].lock);     // 释放自旋锁。
}

// 分配一个 4096 字节的物理内存页面。
// 返回一个内核可以使用的指针。如果无法分配内存，则返回 0。
void *kalloc(void)
{
  struct run *r; // 临时指针，用于指向将要分配的内存块。
  int cpu_id;    // 存储当前 CPU 的 ID。

  push_off();       // 关闭中断，防止在获取 CPU ID 时发生上下文切换。
  cpu_id = cpuid(); // 获取当前 CPU 的 ID。
  pop_off();        // 恢复中断。

  acquire(&kmem[cpu_id].lock); // 获取当前 CPU 对应的自旋锁。
  r = kmem[cpu_id].freelist;   // 从空闲链表中取出第一个空闲块。
  if (r)
  {
    kmem[cpu_id].freelist = r->next; // 更新空闲链表的头指针。
  }
  else
  {
    // 如果当前 CPU 的空闲链表为空，则尝试从其他 CPU 的空闲链表中窃取空闲块。
    for (int i = 0; i < NCPU; i++)
    {
      if (i == cpu_id)
        continue;             // 跳过当前 CPU 自己。
      acquire(&kmem[i].lock); // 获取其他 CPU 对应的自旋锁。
      r = kmem[i].freelist;   // 尝试从其他 CPU 的空闲链表中获取空闲块。
      if (r)
        kmem[i].freelist = r->next; // 更新空闲链表的头指针。
      release(&kmem[i].lock);       // 释放自旋锁。
      if (r)
        break; // 如果成功获取到空闲块，则跳出循环。
    }
  }
  release(&kmem[cpu_id].lock); // 释放当前 CPU 的自旋锁。

  if (r)
    memset((char *)r, 5, PGSIZE); // 将分配的内存块填充为 5，用于捕捉潜在的错误。
  return (void *)r;               // 返回分配的内存块指针。如果分配失败，则返回 0。
}
