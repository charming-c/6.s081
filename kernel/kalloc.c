// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

struct spinlock kmemlock;

char name[NCPU][7];

void
kinit()
{
  initlock(&kmemlock, "kmem");
  for(int i = 0; i<NCPU; i++) {
    snprintf(name[i], 7, "kmem_%d", i);
    initlock(&kmem[i].lock, name[i]);
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int cpu_id;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  push_off();
  cpu_id = cpuid();
  pop_off();

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem[cpu_id].lock);
  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;
  release(&kmem[cpu_id].lock);
}

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
