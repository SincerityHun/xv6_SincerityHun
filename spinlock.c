// Mutual exclusion spin locks.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"

void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
// Holding a lock for a long time may cause
// other CPUs to waste time spinning to acquire it.
void
acquire(struct spinlock *lk)
{
  pushcli(); // disable interrupts to avoid deadlock.
  if(holding(lk))
    panic("acquire");

  // The xchg is atomic.
  while(xchg(&lk->locked, 1) != 0)
    ;

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen after the lock is acquired.
  __sync_synchronize();

  // Record info about lock acquisition for debugging.
  lk->cpu = mycpu();
  getcallerpcs(&lk, lk->pcs);
}

// Release the lock.
void
release(struct spinlock *lk)
{
  if(!holding(lk))
    panic("release");

  lk->pcs[0] = 0;
  lk->cpu = 0;

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other cores before the lock is released.
  // Both the C compiler and the hardware may re-order loads and
  // stores; __sync_synchronize() tells them both not to.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code can't use a C assignment, since it might
  // not be atomic. A real OS would use C atomics here.
  asm volatile("movl $0, %0" : "+m" (lk->locked) : );

  popcli();
}

// Record the current call stack in pcs[] by following the %ebp chain.
void
getcallerpcs(void *v, uint pcs[])
{
  uint *ebp;
  int i;

  ebp = (uint*)v - 2;
  for(i = 0; i < 10; i++){
    if(ebp == 0 || ebp < (uint*)KERNBASE || ebp == (uint*)0xffffffff)
      break;
    pcs[i] = ebp[1];     // saved %eip
    ebp = (uint*)ebp[0]; // saved %ebp
  }
  for(; i < 10; i++)
    pcs[i] = 0;
}

// Check whether this cpu is holding the lock.
int
holding(struct spinlock *lock)
{
  int r;
  pushcli();
  r = lock->locked && lock->cpu == mycpu();
  popcli();
  return r;
}


// Pushcli/popcli are like cli/sti except that they are matched:
// it takes two popcli to undo two pushcli.  Also, if interrupts
// are off, then pushcli, popcli leaves them off.

void
pushcli(void)
{
  int eflags;
  // 1. 현재 레지스터 상태 저장
  eflags = readeflags();
  // 2. 인터럽트 비활성화
  cli();
  // 3. 지금 처음으로 pushcli가 호출된거야? 처음이면 ncli가 0이겠지? 중첩이 안되었을 거니까?
  if(mycpu()->ncli == 0)
    mycpu()->intena = eflags & FL_IF; // Interrupt Flag 정보를 intena에 저장함으로써 기존에 인터럽트 정보를 저장해둔다.(복원용)
  // 4. puchli의 깊이 저장
  mycpu()->ncli += 1;
}

void
popcli(void)
{
  //1. IF 플래그가 활성화되어 있어? 활성화 되어 있으면 하드웨어 인터럽트를 받을 수 있는건데 그러면 시스템 패닉!
  if(readeflags()&FL_IF)
    panic("popcli - interruptible");
  //2. 1감소시킨 ncli값이 0보다 작다는건 pushcli가 popcli보다 덜 되었다는건데 이럼 시스템 패닉!
  if(--mycpu()->ncli < 0)
    panic("popcli");
  //3. ncli가 0인지 확인 -> 모든 cli 다 치웠어? -> intena가 0이 아니라 1인지 확인 -> 기존에 인터럽트 활성화 시켰어? -> 인터럽트 활성화
  if(mycpu()->ncli == 0 && mycpu()->intena)
    sti();
}

