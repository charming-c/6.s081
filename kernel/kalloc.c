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

int pg_counter[(PHYSTOP - KERNBASE) / PGSIZE];

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct spinlock counter_lock;
  struct run *freelist;
} kmem;

void
pgcounterprint() {
   for(int i = 0; i<(PHYSTOP - KERNBASE) / PGSIZE; i++)
    printf("%p: %d\n", KERNBASE + i * PGSIZE, pg_counter[i]);
}

void
vmcount(void *pa, int n) {
  int sum = ((PHYSTOP - KERNBASE) / PGSIZE);
  int num = ((uint64)pa - KERNBASE) / PGSIZE;

  if(num < 0 || num > sum) panic("vmcount");
  acquire(&kmem.counter_lock);
  pg_counter[num] += n;
  release(&kmem.counter_lock);

}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&kmem.counter_lock, "pg_counter");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  // for(int i = 0; i<(PHYSTOP - KERNBASE) / PGSIZE; i++)
  //   pg_counter[i] = 1;
  // // pgcounterprint();
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

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem.counter_lock);
  int num = ((uint64)pa - KERNBASE) / PGSIZE;
  pg_counter[num] = pg_counter[num] > 0 ? pg_counter[num] - 1 : 0;
  release(&kmem.counter_lock);
  if(pg_counter[num] < 0) panic("kfree pg counter < 0");
  if(pg_counter[num] != 0) return;

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  acquire(&kmem.counter_lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r) {
    memset((char*)r, 5, PGSIZE); // fill with junk
    int num = ((uint64)r - KERNBASE) / PGSIZE;
    pg_counter[num]++;
  }
  release(&kmem.counter_lock);
  return (void*)r;
}
