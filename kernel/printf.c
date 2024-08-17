//
// formatted console output -- printf, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

volatile int panicked = 0;

// lock to avoid interleaving concurrent printf's.
static struct {
  struct spinlock lock;
  int locking;
} pr;

static char digits[] = "0123456789abcdef";

static void
printint(int xx, int base, int sign)
{
  char buf[16];
  int i;
  uint x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}

static void
printptr(uint64 x)
{
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the console. only understands %d, %x, %p, %s.
void
printf(char *fmt, ...)
{
  va_list ap;
  int i, c, locking;
  char *s;

  locking = pr.locking;
  if(locking)
    acquire(&pr.lock);

  if (fmt == 0)
    panic("null fmt");

  va_start(ap, fmt);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'x':
      printint(va_arg(ap, int), 16, 1);
      break;
    case 'p':
      printptr(va_arg(ap, uint64));
      break;
    case 's':
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }

  if(locking)
    release(&pr.lock);
}

void
panic(char *s)
{
  pr.locking = 0;
  printf("panic: ");
  printf(s);
  printf("\n");
  panicked = 1; // freeze uart output from other CPUs
  for(;;)
    ;
}

void
printfinit(void)
{
  initlock(&pr.lock, "pr");
  pr.locking = 1;
}

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
