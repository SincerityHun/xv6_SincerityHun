#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[]; // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void tvinit(void)
{
  int i;

  for (i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE << 3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE << 3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void idtinit(void)
{
  lidt(idt, sizeof(idt));
}

// PAGEBREAK: 41
void trap(struct trapframe *tf)
{
  int err;
  // SYSCALL 또는 INTR 또는 Exception이 발생하면 CPU에 의해 호출된다.
  // 1.SYSCALL 이 불릴 경우에
  if (tf->trapno == T_SYSCALL)
  {
    // 현재 프로세스가 이미 죽었다면? SYSCALL 못 부른다.
    if (myproc()->killed)
      exit();
    // 현재 프로세스의 trapframe을 INTR 혹은 EXCEPTION이 불렸을 경우에 있던 프레임을 그대로 가져온다.
    myproc()->tf = tf;
    // syscall 수행
    syscall();
    // syscall 수행 후 죽었는지 확인
    if (myproc()->killed)
      exit();

    return;
  }

  // 2. INTR이나 EXCEPTION이 불릴 경우에
  switch (tf->trapno)
  {
    // TIMER INTR
  case T_IRQ0 + IRQ_TIMER:
    if (cpuid() == 0)
    {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
    // DISK INTR
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;

    // IDE1 INTR (가짜 인터럽트))
  case T_IRQ0 + IRQ_IDE + 1:
    // Bochs generates spurious IDE1 interrupts.
    break;

    // KEYBOARD INTR
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;

    // UART INTR
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;

  case T_PGFLT:
    err = tf->err & 2 ? 2 : 1;
    if(page_fault_handler(rcr2(),err) != -1)
      break;
    // 예기치 못한 INTR
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  // PAGEBREAK: 13
  default:
    if (myproc() == 0 || (tf->cs & 3) == 0)
    {
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if (myproc() && myproc()->state == RUNNING && tf->trapno == T_IRQ0 + IRQ_TIMER)
  {
    /*
    1. 전에 timetick을 쓴만큼 aruntime update - 1tick추가
    2. 흐른 시간만큼 업데이트
    3. 현재 가중치로 vruntime 계산
    */
    // 1. 실제 시간 계산
    myproc()->aruntime += 1000;
    // 2. 얼만큼 흘렀는지 계산(aruntime_prev - aruntime)
    uint temp = myproc()->aruntime - myproc()->aruntime_prev;
    // 3. 현재 nice값 기반 가중치로 vruntime 계산
    myproc()->vruntime_low += 1024000 / myproc()->weight;
    if ((uint)(myproc()->vruntime_low / 1000000000))
    {
      myproc()->vruntime_low %= 1000000000;
      myproc()->vruntime_high++;
    }
    // 4. 이번에 쓴 밀리틱이랑 처음에 스케줄될때 정해졌던 timeslice 비교
    if (temp >= myproc()->timeslice)
    {
      // timeslice가 만약에 작으면? 양보 -> aruntime_prev 업데이트
      myproc()->aruntime_prev = myproc()->aruntime;
      yield();
    }
  }

  // Check if the process has been killed since we yielded
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();
}
