#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

extern uint ticks;

struct
{
  struct spinlock lock[NBUCKET]; // 一个桶一个锁
  struct buf buf[NBUF];
  struct spinlock bcache_lock; // bcache锁

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.

  struct buf head[NBUCKET];
} bcache;

void binit(void)
{
  struct buf *b;
  // 初始化锁
  initlock(&bcache.bcache_lock, "bcache_lock");
  for (int i = 0; i < NBUCKET; ++i)
  {
    initlock(&bcache.lock[i], "bcache_bucket");
  }

  for (int i = 0; i < NBUCKET; ++i)
  {
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }

  for (b = bcache.buf; b < bcache.buf + NBUF; b++)
  {
    b->next = bcache.head[0].next;
    b->prev = &bcache.head[0];
    initsleeplock(&b->lock, "buffer");
    bcache.head[0].next->prev = b;
    bcache.head[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno)
{
  struct buf *b;
  int index = blockno % NBUCKET; // 块号-桶

  acquire(&bcache.lock[index]);

  for (b = bcache.head[index].next; b != &bcache.head[index]; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {

      b->refcnt++;
      release(&bcache.lock[index]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.lock[index]);

  acquire(&bcache.bcache_lock);
  acquire(&bcache.lock[index]);

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

  struct buf *lru_block = 0;
  int min_tick = 0;
  for (b = bcache.head[index].next; b != &bcache.head[index]; b = b->next)
  {
    if (b->refcnt == 0 && (lru_block == 0 || b->last_used_tick < min_tick))
    {
      min_tick = b->last_used_tick;
      lru_block = b;
    }
  }

  if (lru_block != 0)
  {
    lru_block->dev = dev;
    lru_block->blockno = blockno;
    lru_block->refcnt++;
    lru_block->valid = 0;

    release(&bcache.lock[index]);
    release(&bcache.bcache_lock);

    acquiresleep(&lru_block->lock);
    return lru_block;
  }

  // 若该桶没找到，则尝试窃取别的桶的块
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

    // 若其他桶有,则lru_block为最近最久未使用。
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

  release(&bcache.lock[index]);
  release(&bcache.bcache_lock);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid)
  {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock[b->blockno % NBUCKET]);
  b->refcnt--;
  if (b->refcnt == 0)
  {
    b->last_used_tick = ticks;
  }

  release(&bcache.lock[b->blockno % NBUCKET]);
}

void bpin(struct buf *b)
{
  acquire(&bcache.lock[b->blockno % NBUCKET]);
  b->refcnt++;
  release(&bcache.lock[b->blockno % NBUCKET]);
}

void bunpin(struct buf *b)
{
  acquire(&bcache.lock[b->blockno % NBUCKET]);
  b->refcnt--;
  release(&bcache.lock[b->blockno % NBUCKET]);
}
