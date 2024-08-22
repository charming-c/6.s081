# lab 5: lazy alloction
在内存分配时，尤其是程序可能会申请大量的内存，然而本身并不会使用，所以，引入 lazy allcation 机制。
- 程序调用 sbrk 时，如果是为了申请内存，则直接增加合理内存的范围，而不进行实际的物理页分配和映射
- 当程序使用未被分配的虚拟内存时，会触发 page fault，此时进入 trap 处理，分配物理页
- 在内核使用用户空间内存时，可能会使用到 sbrk 申请的内存，而未被分配，所以这个时候也需要相同的处理
- 因为 p->sz 可能已经超过页表映射的虚拟内存范围，在进行一些映射，或者页表复制时，可能会导致 pte 为空，需要忽略这种情况
## 实验实现：
### 1. 系统调用 sbrk
只增加 p->sz，而不再申请内存
```c
uint64
sys_sbrk(void)
{
  int addr;
  int n;
  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if (n >= 0)
    myproc()->sz = myproc()->sz + n > MAXVA ? MAXVA : myproc()->sz + n;
  if(n < 0) {
    // 将 sz 控制在 > 0 的范围
    if(addr < n * (-1))
      n = addr * (-1);
    // printf("addr: %d, n: %d\n", addr, n);
    if(growproc(n) < 0)
      return -1;
  }
  // if(growproc(n) < 0)
  //   return -1;
  return addr;
}
```
### 2. trap 处理
在触发 page fault 时，判断虚拟内存是否合理，合理则分配物理页
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
  } else if (r_scause() == 15 || r_scause() == 13) {
    // 拿到一个物理内存地址
    // page fault 的 虚拟内存地址
    void  *pa = kalloc();
    uint64 va = r_stval();
    uint64 sp = p->trapframe->sp;
    sp = PGROUNDDOWN(sp);

    // 如果没有足够的物理内存，直接杀死进程
    if(pa == 0 || PGROUNDUP(va) > p->sz || va < sp) {
      p->killed = 1;
    } else {
        memset(pa, 0, PGSIZE);
        // 将 va 和 pa 进行映射，不成功则杀死进程，同时释放 alloc 的物理内存
        if(mappages(p->pagetable, PGROUNDDOWN(va), PGSIZE, (uint64)pa, PTE_W|PTE_X|PTE_R|PTE_U) != 0) {
          kfree(pa);
          p->killed = 1;
      }
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
### 3. 内核使用 sbrk 空间 && 忽略 pte 为空
```c
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;
  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0) {
    return 0;
  }
  if((*pte & PTE_V) == 0) {
    // printf("pte: %p\n", pte);
    if(va < myproc()->sz) {
      uint64 n_pa = (uint64)kalloc();
      memset((void *)n_pa, 0, PGSIZE);
      if(mappages(pagetable, va, PGSIZE, n_pa, PTE_W|PTE_X|PTE_R|PTE_U) != 0) {
        kfree((void *)n_pa);
        return 0;
      }
    }
    else return 0;
  }
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;
  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) {
      // printf("%p\n", a);
      // panic("uvmunmap: walk");
      continue;
    }
    if((*pte & PTE_V) == 0)
      continue;
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;
    if((*pte & PTE_V) == 0)
      continue;
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;
 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    if(srcva > myproc()->sz) return -1;
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);
    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}
```