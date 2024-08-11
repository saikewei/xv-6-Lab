#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

extern uint ticks; // 声明一个全局变量，用于表示系统的时钟滴答数

// 定义一个缓存系统结构体
struct
{
  struct spinlock lock[NBUCKET]; // 定义每个桶对应一个自旋锁，以防止并发访问冲突
  struct buf buf[NBUF];          // 定义一个缓冲区数组，存储实际的缓存块
  struct spinlock bcache_lock;   // 用于全局缓存的自旋锁

  // 缓冲区的链表，通过prev/next指针进行连接。
  // 链表按最近使用的顺序排序。
  // head.next指向最近使用的缓冲区，head.prev指向最久未使用的缓冲区。
  struct buf head[NBUCKET]; // 定义每个桶的头部节点，用于管理缓冲区链表
} bcache;                   // 定义全局缓存系统实例

// 初始化缓存系统
void binit(void)
{
  struct buf *b;
  // 初始化全局缓存锁
  initlock(&bcache.bcache_lock, "bcache_lock");

  // 初始化每个桶的自旋锁
  for (int i = 0; i < NBUCKET; ++i)
  {
    initlock(&bcache.lock[i], "bcache_bucket");
  }

  // 初始化每个桶的链表头结点
  for (int i = 0; i < NBUCKET; ++i)
  {
    bcache.head[i].prev = &bcache.head[i]; // 设置每个桶的prev指针指向自己，表示空链表
    bcache.head[i].next = &bcache.head[i]; // 设置每个桶的next指针指向自己，表示空链表
  }

  // 初始化缓冲区链表，将所有缓冲区加入到第一个桶的链表中
  for (b = bcache.buf; b < bcache.buf + NBUF; b++)
  {
    b->next = bcache.head[0].next;     // 将缓冲区插入链表头部
    b->prev = &bcache.head[0];         // 设置前驱指针为链表头
    initsleeplock(&b->lock, "buffer"); // 初始化缓冲区的睡眠锁
    bcache.head[0].next->prev = b;     // 更新原先头部元素的前驱指针
    bcache.head[0].next = b;           // 更新链表头的next指针
  }
}

// 查找或分配缓存块，返回被锁定的缓冲区
static struct buf *
bget(uint dev, uint blockno)
{
  struct buf *b;
  int index = blockno % NBUCKET; // 通过块号计算桶索引

  // 锁定桶对应的自旋锁
  acquire(&bcache.lock[index]);

  // 遍历桶中的缓冲区链表，查找匹配的块
  for (b = bcache.head[index].next; b != &bcache.head[index]; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;                  // 增加引用计数
      release(&bcache.lock[index]); // 释放桶的自旋锁
      acquiresleep(&b->lock);       // 锁定缓冲区的睡眠锁
      return b;                     // 返回匹配的缓冲区
    }
  }
  release(&bcache.lock[index]); // 释放桶的自旋锁

  // 如果没有找到，锁定全局缓存锁，再次查找
  acquire(&bcache.bcache_lock);
  acquire(&bcache.lock[index]);

  // 再次遍历桶中的缓冲区链表，查找匹配的块
  for (b = bcache.head[index].next; b != &bcache.head[index]; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.lock[index]);
      release(&bcache.bcache_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 如果仍未找到，寻找最久未使用的缓存块(LRU算法)
  struct buf *lru_block = 0; // 最久未使用的缓存块指针
  int min_tick = 0;          // 最小的使用时间戳
  for (b = bcache.head[index].next; b != &bcache.head[index]; b = b->next)
  {
    if (b->refcnt == 0 && (lru_block == 0 || b->last_used_tick < min_tick))
    {
      min_tick = b->last_used_tick; // 更新最小时间戳
      lru_block = b;                // 更新最久未使用的缓存块指针
    }
  }

  if (lru_block != 0)
  {
    // 找到可用的LRU缓存块，重新分配并锁定
    lru_block->dev = dev;
    lru_block->blockno = blockno;
    lru_block->refcnt++;
    lru_block->valid = 0; // 标记数据无效，需要重新从磁盘读取

    release(&bcache.lock[index]);
    release(&bcache.bcache_lock);

    acquiresleep(&lru_block->lock); // 锁定LRU缓存块
    return lru_block;
  }

  // 如果该桶中没有可用缓存块，尝试从其他桶中窃取缓存块
  for (int other_index = (index + 1) % NBUCKET; other_index != index; other_index = (other_index + 1) % NBUCKET)
  {
    acquire(&bcache.lock[other_index]);
    for (b = bcache.head[other_index].next; b != &bcache.head[other_index]; b = b->next)
    {
      if (b->refcnt == 0 && (lru_block == 0 || b->last_used_tick < min_tick))
      {
        min_tick = b->last_used_tick;
        lru_block = b;
      }
    }

    // 如果找到LRU缓存块，则从其他桶中获取
    if (lru_block)
    {
      lru_block->dev = dev;
      lru_block->refcnt++;
      lru_block->valid = 0;
      lru_block->blockno = blockno;

      lru_block->next->prev = lru_block->prev;
      lru_block->prev->next = lru_block->next;
      release(&bcache.lock[other_index]);

      lru_block->next = bcache.head[index].next;
      lru_block->prev = &bcache.head[index];
      bcache.head[index].next->prev = lru_block;
      bcache.head[index].next = lru_block;
      release(&bcache.lock[index]);
      release(&bcache.bcache_lock);

      acquiresleep(&lru_block->lock);
      return lru_block;
    }
    release(&bcache.lock[other_index]);
  }

  // 如果所有桶中都没有可用的缓存块，则抛出异常
  release(&bcache.lock[index]);
  release(&bcache.bcache_lock);
  panic("bget: no buffers");
}

// 读取指定块号的缓存块，并返回锁定的缓冲区
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno); // 获取或分配缓存块
  if (!b->valid)          // 如果数据无效，从磁盘读取数据
  {
    virtio_disk_rw(b, 0); // 从磁盘读取数据
    b->valid = 1;         // 标记数据有效
  }
  return b; // 返回缓冲区
}

// 将缓冲区的数据写入磁盘，必须在锁定状态下调用
void bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");    // 检查是否持有睡眠锁
  virtio_disk_rw(b, 1); // 将数据写入磁盘
}

// 释放锁定的缓冲区，并将其移动到最近使用的列表头部
void brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse"); // 检查是否持有睡眠锁

  releasesleep(&b->lock); // 释放缓冲区的睡眠锁

  acquire(&bcache.lock[b->blockno % NBUCKET]);
  b->refcnt--; // 减少引用计数
  if (b->refcnt == 0)
  {
    b->last_used_tick = ticks; // 更新最后使用的时间戳
  }

  release(&bcache.lock[b->blockno % NBUCKET]);
}

// 增加缓冲区的引用计数，表示该块正在使用
void bpin(struct buf *b)
{
  acquire(&bcache.lock[b->blockno % NBUCKET]);
  b->refcnt++;
  release(&bcache.lock[b->blockno % NBUCKET]);
}

// 减少缓冲区的引用计数，表示该块不再被使用
void bunpin(struct buf *b)
{
  acquire(&bcache.lock[b->blockno % NBUCKET]);
  b->refcnt--;
  release(&bcache.lock[b->blockno % NBUCKET]);
}
