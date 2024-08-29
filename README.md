# Lock lab 实验手册
主要分成两个部分：
1. 为每个 CPU 分配一个 allocator，而不是持有一个物理内存的大锁，每次申请和释放都只有一个 CPU 使用
2. 为磁盘缓存设置多个 buckets，防止对整个缓存的读写都需要使用一个大锁，导致 CPU 等待

其实就是细化锁的粒度，将共享的数据分组，从而减少竞争后等待的概率。
## 1. kalloc 实验
这里为每个 CPU 都分配了一个 freelist，每个 CPU 都只会在自己的 freelist 中申请和释放物理内存，当把内存不够时，从其他的 CPU freelist 中偷内存。
代码实现：
```c
void *
steal()
{
  int cpu_id;
  int num = 64;
  struct run *r;
  struct run *h = 0;

  push_off();
  cpu_id = cpuid();
  pop_off();

  acquire(&kmemlock);
  for(int i = 0; i<NCPU; i++) {
    if(i == cpu_id) continue;
    acquire(&kmem[i].lock);
    r = kmem[i].freelist;
    if(r) {
      h = r;
      while(r->next && num > 0) {
        r = r->next;
        num--;
      }
      kmem[i].freelist = r->next;
      r->next = 0;
      release(&kmem[i].lock);
      break;
    }
    else release(&kmem[i].lock);
  }
  release(&kmemlock);

  acquire(&kmem[cpu_id].lock);
  return (void *)h;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  int cpu_id;

  push_off();
  cpu_id = cpuid();
  pop_off();

  /** 这里的加锁策略是：
   * 1. 先对当前 cpu 的 freelist 加锁（其他 cpu 在 steal 的时候也会 acquire 的）
   * 2. 如果当前 cpu 的 freelist 已经空了，内存已满，释放 freelist 的锁（防止其他 cpu steal 时死锁）
   * 3. 去其他的 cpu freelist steal
   * 4. steal 时申请 kmem 大锁，因为当一个 cpu 在 steal 的时候，其他的 cpu 不能 steal 
   * 5. steal 回来以后，再重新 acquire freelist */ 
  acquire(&kmem[cpu_id].lock);
  r = kmem[cpu_id].freelist;
  if(r)
    kmem[cpu_id].freelist = r->next;
  else {
    release(&kmem[cpu_id].lock);
    kmem[cpu_id].freelist = steal();
    r = kmem[cpu_id].freelist;
  }

  if(r)
    kmem[cpu_id].freelist = r->next;
  release(&kmem[cpu_id].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
```
这里保存了整个物理内存的大锁，由于当前 CPU 的 freelist 满的时候，先释放 freelist 的锁，然后进入整个物理内存，对所有的 freelist 偷内存页，然后在返回之前再获取当前 CPU freelist 的锁，通过这个方式，只会有一个 CPU 会进入 steal 状态，并且在不阻碍其他 CPU 使用 freelist 的情况下，遍历了所有的 freelist。
## 2. bcache 实验
这里没有想到很好的处理办法，总觉得怎么都有竞争，然后去参考了其他人的博客，这里是[链接](https://blog.miigon.net/posts/s081-lab8-locks/)。代码就不贴了。