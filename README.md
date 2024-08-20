# Traps lab

trap 机制，是由 ecall，sret，mret 这样的指令控制的，硬件 CPU 只会使用几个关键寄存器，其他的所有内容由软件（OS）完成。

## 一、xv6 trap 机制

首先简单介绍一下关于 trap 中所需要使用的汇编和 c 函数：

### 1. Trampoline 

由于 Trampoline 同时映射到 user 和 kernel 的 pagetable 中，所以在切换 satp 后仍旧是可以执行的。其中主要由两个函数：

**uservec：**

```assembly
.globl uservec
uservec:    
	#
        # trap.c sets stvec to point here, so
        # traps from user space start here,
        # in supervisor mode, but with a
        # user page table.
        #
        # sscratch points to where the process's p->trapframe is
        # mapped into user space, at TRAPFRAME.
        #
        
	# swap a0 and sscratch
        # so that a0 is TRAPFRAME
        csrrw a0, sscratch, a0

        # save the user registers in TRAPFRAME
        sd ra, 40(a0)
        sd sp, 48(a0)
        sd gp, 56(a0)
        sd tp, 64(a0)
        sd t0, 72(a0)
        sd t1, 80(a0)
        sd t2, 88(a0)
        sd s0, 96(a0)
        sd s1, 104(a0)
        sd a1, 120(a0)
        sd a2, 128(a0)
        sd a3, 136(a0)
        sd a4, 144(a0)
        sd a5, 152(a0)
        sd a6, 160(a0)
        sd a7, 168(a0)
        sd s2, 176(a0)
        sd s3, 184(a0)
        sd s4, 192(a0)
        sd s5, 200(a0)
        sd s6, 208(a0)
        sd s7, 216(a0)
        sd s8, 224(a0)
        sd s9, 232(a0)
        sd s10, 240(a0)
        sd s11, 248(a0)
        sd t3, 256(a0)
        sd t4, 264(a0)
        sd t5, 272(a0)
        sd t6, 280(a0)

	# save the user a0 in p->trapframe->a0
        csrr t0, sscratch
        sd t0, 112(a0)

        # restore kernel stack pointer from p->trapframe->kernel_sp
        ld sp, 8(a0)

        # make tp hold the current hartid, from p->trapframe->kernel_hartid
        ld tp, 32(a0)

        # load the address of usertrap(), p->trapframe->kernel_trap
        ld t0, 16(a0)

        # restore kernel page table from p->trapframe->kernel_satp
        ld t1, 0(a0)
        csrw satp, t1
        sfence.vma zero, zero

        # a0 is no longer valid, since the kernel page
        # table does not specially map p->tf.

        # jump to usertrap(), which does not return
        jr t0
```

uservec 首先将 a0 和 sscratch 中的寄存器值交换，由于在调用 uservec 之前，sscratch 应该写入的是 Trapframe 的值（物理地址），所以交换后，就可以把各个寄存器的值写入 trapframe，保存用户态的寄存器状态，然后进入内核态执行。

注意，trapframe 也不是全部都是用户空间内容，还包括 kernel_hartid、kernel_trap 以及 kernel_satp，在 uservec 的最后，会将这些值也写入对应的寄存器，尤其是写完 satp 之后，页表一切换，就是进入内核空间了。

uservec 最后会跳转到 p->trapframe->kernel_trap 执行，一般是 usertrap 的地址。

**userret：**

```assembly
.globl userret
userret:
        # userret(TRAPFRAME, pagetable)
        # switch from kernel to user.
        # usertrapret() calls here.
        # a0: TRAPFRAME, in user page table.
        # a1: user page table, for satp.

        # switch to the user page table.
        csrw satp, a1
        sfence.vma zero, zero

        # put the saved user a0 in sscratch, so we
        # can swap it with our a0 (TRAPFRAME) in the last step.
        ld t0, 112(a0)
        csrw sscratch, t0

        # restore all but a0 from TRAPFRAME
        ld ra, 40(a0)
        ld sp, 48(a0)
        ld gp, 56(a0)
        ld tp, 64(a0)
        ld t0, 72(a0)
        ld t1, 80(a0)
        ld t2, 88(a0)
        ld s0, 96(a0)
        ld s1, 104(a0)
        ld a1, 120(a0)
        ld a2, 128(a0)
        ld a3, 136(a0)
        ld a4, 144(a0)
        ld a5, 152(a0)
        ld a6, 160(a0)
        ld a7, 168(a0)
        ld s2, 176(a0)
        ld s3, 184(a0)
        ld s4, 192(a0)
        ld s5, 200(a0)
        ld s6, 208(a0)
        ld s7, 216(a0)
        ld s8, 224(a0)
        ld s9, 232(a0)
        ld s10, 240(a0)
        ld s11, 248(a0)
        ld t3, 256(a0)
        ld t4, 264(a0)
        ld t5, 272(a0)
        ld t6, 280(a0)

	# restore user a0, and save TRAPFRAME in sscratch
        csrrw a0, sscratch, a0
        
        # return to user mode and user pc.
        # usertrapret() set up sstatus and sepc.
        sret
```

userret 会需要依靠 a0 和 a1 两个参数的值，所以一般会传入两个参数。a0 为一个 trapframe，a1 应该是用户页表。由于 userret 首先切换页表到用户页表，所以 trapframe 应该是用户空间的地址，然后 userret 将之前 uservec 时保存在 trapframe 中的所有寄存器的值恢复到相应的寄存器中，然后在 uservec 中 sscratch 设置为 a0，所以最后交换 sscratch 和 a0 后，a0 又变成 a0，sscratch 变为 trapframe。

### 2. trap.c

trap.c 是在内核中执行的，主要在内核态处理 trap。

**usertrap：**

```c
//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
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
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2) {
    // 查看当前运行进程是否设置 alarm，并且判断是否 handler
    if(p->tick > 0) {
      // printf("一次 tick!\n");
      p->total_tick += 1;
      int t_tick = p->total_tick;
      if(t_tick == p->tick && p->isalarmrunning == 0) {
        p->isalarmrunning = 1;
        copy_trapframe(p->tick_trapframe, p->trapframe);
        p->trapframe->epc = p->alarm_handler;
        p->total_tick = p->total_tick % p->tick;

      }
      usertrapret();
    }
    // 放弃 CPU
   else yield();
  }

  usertrapret();
}

```

xv6 在这里处理 trap，是否是系统调用、硬件中断、计时器中断。这里不详细介绍，主要介绍寄存器部分。usertrap 会修改 stvec，因为这个时候已经是内核态了，如果触发 trap，应该执行 kernelvec，同时保存 sepc 的值，防止由于因为计时器中断，内核 yeild 让出 CPU 给其他的进程的内核线程执行，并且这时可能会范湖到用户空间。

**usertrapret:**

```c
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

```

在这里恢复 stvec 为 uservec，并且设置 trapframe 的 kernel_satp、kernel_sp、kernel_trap（在这里开始设置为 usertrap）、kernel_hartid。最终设置 sepc。将 TRAPFRAME 和 satp（内核页表）传入 userret，并且调用执行。

## 二、trap 流程

我认为，梳理 trap 的流程可以不从 ecall 开始，因为在进入用户态之前，其实内核就为进入 trap 准备好了各种前置条件，为了便于梳理这样的前置条件，以便于理解 uservec、usertrap 以及 ret 中的种种设置，我们就从 内核开始一个进程 --> sret 进入用户态执行 --> 开启一个 ecall --> sret 这样的过程来梳理。根据上面的几个函数，我们需要重点关注的几个对象就是 sscratch、trapframe 页、Trampoline 页、satp。

### 1. 第一步：开启一个用户进程

为了和上一个实验联系上，我们把开启第一个用户进程当做示例。

**main 中初始化 trap：**

首先在设置好虚拟内存后，为每一个进程的 kstack 都映射到虚拟地址空间的高处。然后调用 trapinit 和 trapinithart ，将 stvec 设置为 kernelvec（因为这个时候在内核态，所以 trap 进入 kernelvec）。

**userinit 函数**

内核在 userinit 函数设置第一个用户进程。

- 调用 allocproc 函数，创建程序的页表，以及 trapframe，其中 p->trapframe 是使用的物理地址，可以在内核态下直接读写（因为内核态绝大部分地址都是直接映射的，基本等同于用 kalloc 拿到的 pa 都可以在这里读写）。同时，这里设置 context 的 ra 为 forkret，sp 为 内核栈地址。
- 将 initcode 的二进制代码写入用户页表，并且映射到用户页表的低地址（从 0 开始）。
- 写入 p->trapframe->epc = 0，p->trapframe->stack = PGSIZE

执行完 userinit 以后，第一个用户进程的内存模型就出来了。

> 这里需要注意的是，每一次创建用户页表时， Trampoline 都会被加载在最高地址处，并且内容都一样，是 trampoline 的二进制代码。还会将 p->trapframe 映射到 trampoline 的下面。所以对于不同的进程，虽然 p->trapframe 的物理地址不同，但是虚拟地址是一样的，这也是页表设计的一个巧妙之处）。

**在 scheduler 中调度执行 第一个进程**

通过 swtch 函数交换 内核 和 p 的上下文，然后此时 sp 成为了 userinit 中设置的 p 的内核栈地址，ra 是 forkret，在执行完 swtch 后，pc 跳转到 ra，执行 forkret。

**forkret 函数**

- forkret 函数直接调用 usertrapret 函数。

- 这之后，stvec 修改为 uservec，保存了 内核页表、内核栈、usertrap 写入 kernel_trap，hartid，sepc 为 在 userinit 中设置的 0，然后传入 TRAPFRAME 和 satp（用户页表）。执行 userret。

- userret 把 trapframe 的 stack 恢复（其实主要是这个 和 stepc，不然用户空间不知道怎么开始执行），其他的寄存器由于还没有执行过用户态代码，所以完全不需要关心。在这之后，sscratch 就是 TRAPFRAME 了，然后跳转到用户态执行。

stvec：uservec	sscratch：TRAPFRAME

### 2. 用户程序开启 trap

- 调用 ecall，此时 sscratch 是 TRAPFRAME，交换 a0 和 sscratch，保存所有的通用寄存器的值。加载用户进程的内核栈地址到 sp，设置 tp，切换内核页表，然后进入 p->kernel_trap，也就是 usertrap。
- usertrap 会修改 stvec 为 kenerlvec，保存 sepc 到 trapframe
- usertrapret，修改 stvec 为 uservec，保存了 内核页表、内核栈、usertrap 写入 kernel_trap，hartid，sepc 设置为之前 usertrap 保存在 trapframe 的值，传入 TRAPFRAME 和 satp 用户页表。
- userret 切换用户页表，恢复之前在 trapframe 保存的值，将 sscratch 保存为 TRAPFRAME。

stvec：uservec	sscratch：TRAPFRAME

## 三、Traps lab

### 1. Backtrace

```c
void
backtrace2(uint64 *fp, uint64 stack_base)
{
  if((uint64)fp >= stack_base) return;
  printf("%p\n", *(fp - 1));
  fp = (uint64 *)*(fp - 2);
  backtrace2(fp, stack_base);
}

void
backtrace()
{
  // s0 写入的是 栈帧地址！！！
  // 这里用指针表示地址
  uint64 *fp = (uint64 *)r_fp();
  // 计算栈底
  uint64 stackbase = PGROUNDUP((uint64)fp);
  printf("backtrace:\n");

  // for(;;) {
  //   if((uint64)fp >= stackbase) break;

  //   // fp 是一个栈帧的开始的地址
  //   // 那么 ret 为 fp - 1 (这里为什么 -1，因为一个 uint64 长度刚好是 8 bytes)
  //   uint64 *ret = fp - 1;
  //   // 注意这里是 ret 的栈地址，真正的返回地址应该是 ret 地址处的内容
  //   printf("%p\n", *ret);
  //   // 这些就知道上一层栈的开始位置存储在 fp - 2
  //   // 下面把 fp - 2 的内容拿出来，然后转化为地址
  //   uint64 back = *(fp - 2);
  //   fp = (uint64 *) back;
  // }
  backtrace2(fp, stackbase);
}
```

## 2. Alarm

这里放主要代码：

```c
uint64
sys_sigalarm(void)
{
  int tick;
  uint64 handler;
  struct proc *p = myproc();

  if(argint(0, &tick) < 0 || argaddr(1, &handler) < 0)
    return -1;

  p->tick = tick;
  p->total_tick = 0;
  p->alarm_handler = handler;
  return 0;
}

uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();
  copy_trapframe(p->trapframe, p->tick_trapframe);
  p->isalarmrunning = 0;
  usertrapret();
  return 0;
}


int
copy_trapframe(struct trapframe *dst, struct trapframe *src)
{
  memmove((void *)dst, (void *)src, sizeof(struct trapframe));
  return 0;
}


  if(which_dev == 2) {
    // 查看当前运行进程是否设置 alarm，并且判断是否 handler
    if(p->tick > 0) {
      // printf("一次 tick!\n");
      p->total_tick += 1;
      int t_tick = p->total_tick;
      if(t_tick == p->tick && p->isalarmrunning == 0) {
        p->isalarmrunning = 1;
        copy_trapframe(p->tick_trapframe, p->trapframe);
        p->trapframe->epc = p->alarm_handler;
        p->total_tick = p->total_tick % p->tick;

      }
      usertrapret();
    }
    // 放弃 CPU
   else yield();
  }
```



### 



