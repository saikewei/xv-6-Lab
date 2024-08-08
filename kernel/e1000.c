#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++)
  {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64)tx_ring;
  if (sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;

  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++)
  {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64)rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64)rx_ring;
  if (sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA + 1] = 0x5634 | (1 << 31);
  // multicast table
  for (int i = 0; i < 4096 / 32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |                 // enable
                     E1000_TCTL_PSP |                // pad short packets
                     (0x10 << E1000_TCTL_CT_SHIFT) | // collision stuff
                     (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8 << 10) | (6 << 20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN |      // enable receiver
                     E1000_RCTL_BAM |     // enable broadcast
                     E1000_RCTL_SZ_2048 | // 2048-byte rx buffers
                     E1000_RCTL_SECRC;    // strip CRC

  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0;       // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0;       // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int e1000_transmit(struct mbuf *m)
{
  // 获取e1000锁，确保对发送环形缓冲区的访问是安全的
  acquire(&e1000_lock);

  // 获取当前TDT（发送描述符尾部指针）的值，即网卡硬件当前处理到的发送描述符索引
  uint64 tdt = regs[E1000_TDT];
  // 计算发送环中的当前索引
  uint64 index = tdt % TX_RING_SIZE;
  // 获取当前索引处的发送描述符
  struct tx_desc send_desc = tx_ring[index];

  // 检查发送描述符是否已经被硬件处理（通过判断状态位是否包含DD标志）
  if (!(send_desc.status & E1000_TXD_STAT_DD))
  {
    // 如果发送描述符尚未处理，说明环形缓冲区满，释放锁并返回-1表示失败
    release(&e1000_lock);
    return -1;
  }

  // 如果之前存在缓冲区，则释放它
  if (tx_mbufs[index] != 0)
  {
    mbuffree(tx_mbufs[index]);
  }

  // 将新的缓冲区指针存储到对应位置
  tx_mbufs[index] = m;
  // 将缓冲区的地址写入描述符
  tx_ring[index].addr = (uint64)tx_mbufs[index]->head;
  // 将缓冲区的长度写入描述符
  tx_ring[index].length = (uint16)tx_mbufs[index]->len;
  // 设置描述符的命令位，指示这是一个要发送的完整数据包，并要求发送后更新状态
  tx_ring[index].cmd = E1000_TXD_CMD_RS | E1000_TXD_CMD_EOP;
  // 清除描述符的状态位，准备发送
  tx_ring[index].status = 0;

  // 更新TDT寄存器，使网卡处理下一个发送描述符
  tdt = (tdt + 1) % TX_RING_SIZE;
  regs[E1000_TDT] = tdt;
  // 确保所有内存操作在同步点前完成
  __sync_synchronize();

  // 释放e1000锁
  release(&e1000_lock);

  // 返回0表示发送操作已成功提交
  return 0;
}

static void e1000_recv(void)
{
  // 获取当前RDT（接收描述符尾部指针）的值，即网卡硬件当前处理到的接收描述符索引
  uint64 rdt = regs[E1000_RDT];
  // 计算接收环中下一个描述符的索引
  uint64 index = (rdt + 1) % RX_RING_SIZE;

  // 检查当前接收描述符的状态位是否包含DD标志，即是否有数据包到达
  if (!(rx_ring[index].status & E1000_RXD_STAT_DD))
  {
    // 如果没有数据包到达，直接返回
    return;
  }

  // 使用while循环处理所有可用的数据包
  while (rx_ring[index].status & E1000_RXD_STAT_DD)
  {
    // 获取当前索引处的mbuf指针
    struct mbuf *buf = rx_mbufs[index];
    // 将数据包长度写入mbuf并调整指针
    mbufput(buf, rx_ring[index].length);
    // 为接收环形缓冲区分配新的mbuf
    rx_mbufs[index] = mbufalloc(0);
    // 如果分配失败，则触发panic（此处不在代码中实现）
    // 更新接收描述符的地址字段，以便接收新数据包
    rx_ring[index].addr = (uint64)rx_mbufs[index]->head;
    // 清除描述符的状态位，准备接收下一个数据包
    rx_ring[index].status = 0;
    // 更新RDT寄存器，指示网卡可以使用这个描述符
    rdt = index;
    regs[E1000_RDT] = rdt;
    // 确保所有内存操作在同步点前完成
    __sync_synchronize();

    // 调用网络层处理函数，将数据包传递到更高层处理
    net_rx(buf);
    // 计算下一个描述符的索引
    index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  }
}

void e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
