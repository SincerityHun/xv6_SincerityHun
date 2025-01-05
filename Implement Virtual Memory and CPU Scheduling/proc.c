#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

// weights[nice] => 해당 nice값에 대응되는 가중치
int weights[] = {
    /*0*/
    88761,
    71755,
    56483,
    46273,
    36291,
    /*5*/
    29154,
    23254,
    18705,
    14949,
    11916,
    /*10*/
    9548,
    7620,
    6100,
    4904,
    3906,
    /*15*/
    3121,
    2501,
    1991,
    1586,
    1277,
    /*20*/
    1024,
    820,
    655,
    526,
    423,
    /*25*/
    335,
    272,
    215,
    172,
    137,
    /*30*/
    110,
    87,
    70,
    56,
    45,
    /*35*/
    36,
    29,
    23,
    18,
    15,
};
struct
{
  struct spinlock lock;    // Lock Information
  struct proc proc[NPROC]; // 최대 프로세스 개수(NPROC)만큼의 PCB 공간
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int cpuid()
{
  return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu *
mycpu(void)
{
  int apicid, i;

  if (readeflags() & FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i)
  {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc *
myproc(void)
{
  struct cpu *c;
  struct proc *p;
  // 1. 인터럽트 비활성화
  pushcli();
  // 2. CPU 정보 받기
  c = mycpu();
  // 3. 현재 이 CPU에서 돌고 있는 process 받아오기
  p = c->proc;
  // 4. 다시 인터럽트 활성화
  popcli();
  // 5. 현재 cpu에서 돌아가고 있는 process 반환
  return p;
}

// PAGEBREAK: 32
//  Look in the process table for an UNUSED proc.
//  If found, change state to EMBRYO and initialize
//  state required to run in the kernel.
//  Otherwise return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;
  char *sp;

  // 1. ptable lock 걸기
  acquire(&ptable.lock);
  // 2. UNUSED,,비어있는 Process 슬롯
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == UNUSED)
      goto found;

  // 3. UNUSED가 없다면?
  release(&ptable.lock);
  return 0;

found:
  // 1. EMBRYO -> Process 상태 초기화 중
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->nice = 20;
  // PA2
  p->weight = weights[p->nice];
  p->vruntime_high = 0;
  p->vruntime_low = 0;
  p->aruntime = 0;
  p->aruntime_prev = 0;
  p->timeslice = 0;

  // 2. ptable lock 풀기
  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0)
  {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE; // Stack Pointer

  // Leave room for trap frame. -> trap frame 만큼 pointer 이동
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret. -> 반환 주소(trap return) 저장 공간 확보, 즉 이 프로세스로 다시 돌아오려면 여기로 돌아오세요~
  sp -= 4;
  *(uint *)sp = (uint)trapret;

  // Context 저장 공간 확보
  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context); // Context Register 0으로 초기화
  p->context->eip = (uint)forkret;           // context의 Instruction Pointer를 Fork return 함수의 주소로 설정
  //-> "처음으로 이 프로세스가 스케줄링 되어 CPU에서 실행되면 "forkret" 함수부터 실행하세요"

  return p;
}

// PAGEBREAK: 32
//  Set up first user process.
void userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

  initproc = p;
  if ((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0; // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if (n > 0)
  {
    if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  else if (n < 0)
  {
    if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process. -> 메모리 공간 할당
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy process state from proc.
  // 현재 프로세스(부모 프로세스)의 Page Dir을 복사해간다.
  if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0)
  {
    // 실패하면 걍 0으로 돌림
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  // 현재 프로세스(부모)의 정보를 상속 받는 중
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;
  // PA2
  np->vruntime_high = curproc->vruntime_high;
  np->vruntime_low = curproc->vruntime_low;
  np->aruntime = 0;
  np->aruntime_prev = 0;
  np->nice = curproc->nice;
  np->weight = curproc->weight;

  // Clear %eax so that fork returns 0 in the child. -> 자식 프로세스에게는 0을 반환
  np->tf->eax = 0;

  // 부모 프로세스의 열린 파일 디스크립터를 자식 프로세스에 복사
  for (i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  // 부모 프로세스의 이름을 자식 프로세스에 복사
  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  // Ptable에 락걸기
  acquire(&ptable.lock);

  // 현재 프로세스 Runnable 큐에 넣기
  np->state = RUNNABLE;

  release(&ptable.lock);
  map_fork(np);
  // 부모 프로세스에게 자식 프로세스 pid를 반환
  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  // 초기 프로세스면 패닉
  if (curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for (fd = 0; fd < NOFILE; fd++)
  {
    if (curproc->ofile[fd])
    {
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  // 현재 작업중인 디렉토리 해제,  Current Working Directory
  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->parent == curproc)
    {
      // 부모가 자식보다 먼저 죽어버리면 자식을 init에게 줌.
      p->parent = initproc;
      // 근데 그 자식이 zombie라면 initproc을 깨워야함 -> initproc가 얘를 처리하도록 어떻게 처리할까..?
      if (p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void)
{
  /*
  현재 프로세스가 자식 프로세스를 가지고 있을 때 그 자식 중 하나가 종료될 때까지 대기.
  하나라도 종료되면 그 자식 pid 반환.
  없거나 현재 프로세스가 종료 요청을 받으면 -1을 반환한다.
  */
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc)
        continue;
      havekids = 1;
      if (p->state == ZOMBIE)
      {
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock); // DOC: wait-sleep
  }
}
/*
  Compare The Vruntime of Process
  - ptable lock 필수
  - 1이면 p1의 vruntime이 큰 것 (우선순위는 낮음)
  - 0이면 p2의 vruntime이 작은 거 (우선순위는 높음)
  */
int compare_vruntime(struct proc *p1, struct proc *p2)
{
  if (p1->vruntime_high > p2->vruntime_high)
    return 1;
  else if (p1->vruntime_high < p2->vruntime_high)
    return 0;
  else
  {
    if (p1->vruntime_low > p2->vruntime_low)
      return 1;
    else if (p1->vruntime_low < p2->vruntime_low)
      return 0;
  }

  return 0;
}
// PAGEBREAK: 42
//  Per-CPU process scheduler.
//  Each CPU calls scheduler() after setting itself up.
//  Scheduler never returns.  It loops, doing:
//   - choose a process to run
//   - swtch to start running that process
//   - eventually that process transfers control
//       via swtch back to the scheduler.
void scheduler(void)
{
  // Initialization
  struct proc *p;
  struct cpu *c = mycpu();
  struct proc *min_p;
  struct proc *temp_p;
  uint total_weight;
  c->proc = 0;

  for (;;)
  {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock); // Lock the Process table
    total_weight = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      // 1. Runnable 한게 없으면 스킵
      if (p->state != RUNNABLE)
      {
        continue;
      }
      // 2. vruntime이 제일 작은 process 찾기 + 현재 Runnable한 전체 프로세스 가중치 합 구하기.
      min_p = p;
      for (temp_p = ptable.proc; temp_p < &ptable.proc[NPROC]; temp_p++)
      {
        if (temp_p->state == RUNNABLE)
        {
          total_weight += temp_p->weight;
          if (compare_vruntime(min_p, temp_p))
          {
            min_p = temp_p;
          }
        }
      }
      // 3. vruntime이 최소인 프로세스의 timeslice 업데이트 + total weight 초기화
      p = min_p;
      // 2. Runnable 한게 있어? Switch 진행시켜
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p; // Cpu의 실행 process를 이걸로
      p->weight = weights[p->nice];
      switchuvm(p); // CPU가 주어진 프로세스의 가상 메모리 주소 공간을 사용하도록
      p->state = RUNNING;
      p->timeslice = (uint)(10000 * (p->weight / total_weight) + 0.5);
      total_weight = 0;
      swtch(&(c->scheduler), p->context);
      // 3. 끝났어(exit or preempted) -> 다시 스케쥴러에게 컨트롤 줘라
      switchkvm();
      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  // 현재 실행 중인 프로세스의 Context를 스케줄러의 Context로 스위칭
  int intena;
  struct proc *p = myproc();

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (readeflags() & FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;               // 현재 CPU의 INTR 활성화 상태 저장
  swtch(&p->context, mycpu()->scheduler); // 현재 프로세스의 컨텍스트에서 스케줄러의 컨텍스트로 전환
  mycpu()->intena = intena;               // 현재 CPU에 INTR 상태 복원
}

// Give up the CPU for one scheduling round. (Preempted)
void yield(void)
{
  acquire(&ptable.lock); // DOC: yieldlock
  // 현재 가지고 있는 프로세스의 state을 RUNNABLE로 바꾸기(Ready)
  myproc()->state = RUNNABLE;
  // 스케줄러 컨텍스트로 변경
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first)
  {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0; // 이 함수는 프로세스가 처음 생길때만 실행하도록 해야되기 때문에
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if (p == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock)
  {                        // DOC: sleeplock0
    acquire(&ptable.lock); // DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock)
  { // DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// PAGEBREAK!
//  Wake up all processes sleeping on chan.
//  The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;
  // PA2
  uint min_vruntime_low;
  uint min_vruntime_high;
  int flag = 0;
  // Vruntime 제일 값 찾기
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state != RUNNABLE)
      continue;
    if (flag == 0)
    {
      min_vruntime_high = p->vruntime_high;
      min_vruntime_low = p->vruntime_low;
      flag = 1;
    }
    if (min_vruntime_high > p->vruntime_high)
    {
      min_vruntime_high = p->vruntime_high;
      min_vruntime_low = p->vruntime_low;
    }
    else if (min_vruntime_high == p->vruntime_high && min_vruntime_low > p->vruntime_low)
    {
      min_vruntime_low = p->vruntime_low;
    }
  }
  // chan 안에서 Sleeping 중이던거 꺠우는
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == SLEEPING && p->chan == chan)
    {
      p->state = RUNNABLE;
      p->vruntime_high = flag ? min_vruntime_high : 0;
      p->vruntime_low = flag ? (min_vruntime_low - (uint)(1024000 / p->weight)) : 0;
      if (p->vruntime_low < 0)
      {
        if (p->vruntime_high > 0)
        {
          p->vruntime_high--;
          p->vruntime_low += 1000000000;
        }
        else
        {
          p->vruntime_low = 0;
        }
      }
    }
  }
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan)
{
  // 락 걸고 해야해~
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// PAGEBREAK: 36
//  Print a process listing to console.  For debugging.
//  Runs when user types ^P on console.
//  No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [EMBRYO] "embryo",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING)
    {
      getcallerpcs((uint *)p->context->ebp + 2, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int getpname(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      cprintf("%s\n", p->name);
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

int getnice(int pid)
{
  if (pid <= 0)
    return -1;
  struct proc *p;
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      int result = p->nice;
      release(&ptable.lock);
      return result;
    }
  }
  release(&ptable.lock);
  return -1;
}

int setnice(int pid, int value)
{
  if (value < 0 || value > 39 || pid <= 0)
  {
    return -1;
  }

  struct proc *p;
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->nice = value;
      // PA2
      p->weight = weights[value];
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}
int get_digit_count(int num)
{
  int count = 0;
  while (num)
  {
    count++;
    num /= 10;
  }
  return count;
}
void ps(int pid)
{
  if (pid < 0)
    return;
  struct proc *p;
  struct proc *temp[NPROC];
  int count = 0;
  const char *stateNames[] = {"UNUSED", "EMBRYO", "SLEEPING", "RUNNABLE", "RUNNING", "ZOMBIE"};
  acquire(&ptable.lock);
  // PA2
  if (pid)
  {
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->pid == pid && p->state != UNUSED)
      {
        cprintf("%20s%20s%20s%20s%20s%20s%20s%5s%d", "name", "pid", "state", "priority", "runtime/weight", "runtime", "vruntime", "tick", ticks); // Space 12
        cprintf("000\n");
        cprintf("%20s%20d%20s%20d%20d%20d", p->name, p->pid, stateNames[p->state], p->nice, p->aruntime / p->weight, p->aruntime);
        if (p->vruntime_high)
        {
          int zero_count = 9 - get_digit_count(p->vruntime_low); // 9에서 vruntime_low의 자릿수를 뺀 만큼
          cprintf("%d", p->vruntime_high);                       // vruntime_high 출력
          while (zero_count--)                                   // 필요한 만큼의 0들 출력
          {
            cprintf("0");
          }
          cprintf("%d\n", p->vruntime_low); // vruntime_low 출력
        }
        else
        {
          cprintf("%20d\n", p->vruntime_low);
        }

        release(&ptable.lock);
        return;
      }
    }
  }
  else
  {
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->state != UNUSED)
      {
        temp[count++] = p;
      }
    }
    if (count)
    {
      cprintf("%20s%20s%20s%20s%20s%20s%20s%5s%d", "name", "pid", "state", "priority", "runtime/weight", "runtime", "vruntime", "tick", ticks);
      cprintf("000\n");
    }
    for (int i = 0; i < count; i++)
    {
      cprintf("%20s%20d%20s%20d%20d%20d", temp[i]->name, temp[i]->pid, stateNames[temp[i]->state], temp[i]->nice, temp[i]->aruntime / temp[i]->weight, temp[i]->aruntime);
      if (temp[i]->vruntime_high)
      {
        int zero_count = 9 - get_digit_count(temp[i]->vruntime_low); // 9에서 vruntime_low의 자릿수를 뺀 만큼
        cprintf("%d", temp[i]->vruntime_high);                       // vruntime_high 출력
        while (zero_count--)                                         // 필요한 만큼의 0들 출력
        {
          cprintf("0");
        }
        cprintf("%d\n", temp[i]->vruntime_low); // vruntime_low 출력
      }
      else
      {
        cprintf("%20d\n", temp[i]->vruntime_low);
      }
    }

    release(&ptable.lock);
    return;
  }
  release(&ptable.lock);
  return;
}
