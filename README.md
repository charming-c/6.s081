# net lab 实验手册
主要是实现两个函数，直接按照所给的 hint 实现就好了。
## 1. e1000_transmit 函数
实现如下:
```c
int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //
  // printf("%s\n", "e1000_transmit");
  acquire(&e1000_lock);
  uint32 tx_ring_tail = regs[E1000_TDT];
  struct tx_desc *tx = &tx_ring[tx_ring_tail];
  if(tx->status != E1000_TXD_STAT_DD) {
    release(&e1000_lock);
    return -1;
  }
  if(tx_mbufs[tx_ring_tail]) mbuffree(tx_mbufs[tx_ring_tail]);
  tx->addr = (uint64)m->head;
  tx->length = m->len;
  tx_mbufs[tx_ring_tail] = m;
  tx->cmd = 9;
  regs[E1000_TDT] = (regs[E1000_TDT] + 1) % TX_RING_SIZE;
  release(&e1000_lock);
  return 0;
}
```
需要注意的是，这里在实现数据传输的时候，应该加锁保护一下环形缓冲区的读写，因为很可能多个进程同时并行的在 cpu 进行数据的发送，这个时候由于缓冲区的共享，需要加锁保护地写。
## 2. e1000_recv 函数
实现如下：
```c
static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //
  // printf("%s\n", "e1000_recv");
  int rx_ring_tail = regs[E1000_RDT];
  rx_ring_tail = (rx_ring_tail + 1) % RX_RING_SIZE;
  while(rx_ring[rx_ring_tail].status & E1000_RXD_STAT_DD) {
    struct mbuf *mb = rx_mbufs[rx_ring_tail];
    mb->len = rx_ring[rx_ring_tail].length;
    net_rx(mb);
    mb = mbufalloc(0);
    rx_mbufs[rx_ring_tail] = mb;
    rx_ring[rx_ring_tail].addr = (uint64)mb->head;
    rx_ring[rx_ring_tail].status = 0;
    rx_ring_tail = (rx_ring_tail + 1) % RX_RING_SIZE;
    if(regs[E1000_RDT] == rx_ring_tail) break;
  }
  
  regs[E1000_RDT] = rx_ring_tail - 1;

}
```
这里不加锁是，在网卡收到一个数据包后，会引起 PLIC 发起一个硬件中断，显然，xv6 一次只会处理同一个设备的一个中断，不会有缓冲区的读写冲突，也就不用加锁。注意如果数据超过缓冲区，我们就终止循环不读了。