# Lab 6: COW lab

fork copy on write（写时复制）：为了防止在 fork 时，从父进程到子进程之间页表复制的大量时间开销，并且当子进程在 fork 之后执行 exec，则页表复制的内容全部都是无用功，所以引出写时复制的机制：

- fork 时，将父进程和子进程的页表映射到同一物理地址，防止父子进程写入时弄脏数据，将 pte 置为不可写，同时利用 pte 的 RSW 位打上 COW 页标记。
- 当父子进程需要写入 cow 页时，由于 pte 的不可写，触发 page fault，进入 trap。
- trap 后，将对应的 cow 复制进新的页，并将原先的虚拟地址映射进新的页。
- 由于 cow 页可能不止一个 pte 条目引用，所以采用引用计数机制，对于所有物理页，alloc 计数 +1，free 时计数 -1，当 free 后计数为零时触发回收。同时注意 fork 时，由于页表条目映射到同一个物理地址，所以需要计数 +1。
- 由于 CPU 多核并行，所以在对计数统计时，**加锁修改**。
- 在内核对用户空间虚拟地址写入时，不会进入 trap，触发 pagefault，所以需要在这时，一样复制映射新的页。
- **注意防止内存泄漏**。

## 实验实现：

### 1. trap.c

添加 pagefault 处理:

```c
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok
  } else if(r_scause() == 15 || r_scause() == 13) {
    /**
     * 1. 检查是不是 cow page
     * 2. 申请新物理页
     * 3. 拷贝物理页内容
     * 4. 替换页表条目，映射到新的页表（可写权限）
     * 5. 删除旧物理页
     */
    struct proc *p = myproc();
    uint64 va = r_stval();
    uint64 n_pa = (uint64)kalloc();
    pte_t *pte = walk(p->pagetable, va, 0);
    uint64 pa = walkaddr(p->pagetable, va);

    // printf("va: %p sz: %p\n", va, n_pa);
    if(n_pa == 0 || va > p->sz || ((*pte & PTE_RSW) != PTE_RSW) || pa == 0) {
      if(n_pa != 0) kfree((void *)n_pa);
      p->killed = 1;
    }

    else {
      
      // if(pa == 0) panic("usertrap(): va has no pa");
      memset((void *)n_pa, 0, PGSIZE);
      memmove((char *)n_pa, (char*)pa, PGSIZE);
      uvmunmap(p->pagetable, PGROUNDDOWN(va), 1, 1);
      if(mappages(p->pagetable, PGROUNDDOWN(va), PGSIZE, n_pa, PTE_U | PTE_W | PTE_R | PTE_X) != 0) {
        kfree((void *)n_pa);
        p->killed = 1;
      }
      // printf("trap:\n");
      // vmprint(p->pagetable);
    }
    
  }
  else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}
```

### 2. fork

在 fork 时，在 uvmcopy 时，不再 alloc 新的物理页，而是修改父进程 pte flags，并且父子映射到原来同一个物理地址：

```c
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  // char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    // 查找 pte 时，将 父进程的 pte 设置成不可写
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);

    *pte &= (~PTE_W);
    *pte |= PTE_RSW;

    // 子进程 pte 不可写，且使用相同的物理内存
    flags = PTE_FLAGS(*pte);
    // if((mem = kalloc()) == 0)
    //   goto err;
    // memmove(mem, (char*)pa, PGSIZE);
    vmcount((void *)pa, 1);

    if(mappages(new, i, PGSIZE, (uint64)pa, flags) != 0){
      // kfree(mem);
      vmcount((void *)pa, -1);
      goto err;
    }
    // pgcounterprint();
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}
```

### 3. 引用计数机制

**初始化锁**

```c
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
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&kmem.counter_lock, "pg_counter");
  freerange(end, (void*)PHYSTOP);
}
```

**alloc 和 free 的计数改变：**

```c
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
```

**为 fork 时修改物理页计数添加外部接口：**

```c
void
vmcount(void *pa, int n) {
  int sum = ((PHYSTOP - KERNBASE) / PGSIZE);
  int num = ((uint64)pa - KERNBASE) / PGSIZE;

  if(num < 0 || num > sum) panic("vmcount");
  acquire(&kmem.counter_lock);
  pg_counter[num] += n;
  release(&kmem.counter_lock);

}
```

### 4. 内核写用户空间

主要是 copyout 将内核数据写入用户空间，这里添加和 trap 中一样的机制：

```c
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0 = 0;
  uint64 n_pa = 0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);

    if(pa0 == 0)
    return -1;
    /**如果是 cow page
     * 1. 申请一个新的 page
     * 2. 将内容复制过去
     * 3. unmap 旧页，map 新页 */ 
    pte_t *pte = walk(pagetable, va0, 0);
    if(*pte & PTE_RSW) {
      n_pa = (uint64)kalloc();
      if(n_pa == 0) panic("no memory");
      memset((void *)n_pa, 0, PGSIZE);
      memmove((void *)n_pa, (void *)pa0, PGSIZE);
      uvmunmap(pagetable, va0, 1, 1);
      if(mappages(pagetable, va0, PGSIZE, n_pa, PTE_U | PTE_W | PTE_R | PTE_X) != 0){
        kfree((void *) n_pa);
        panic("copyout map err");
      } 
      pa0 = n_pa;
    }
   
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}
```

