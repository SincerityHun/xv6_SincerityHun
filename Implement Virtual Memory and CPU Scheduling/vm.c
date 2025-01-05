#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"
#include "spinlock.h" //SPINLOCK 사용
#include "fs.h"
#include "sleeplock.h"
#include "file.h"     //File Offset 설정용

extern char data[]; // defined by kernel.ld
pde_t *kpgdir;      // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X | STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X | STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if (*pde & PTE_P)
  {
    pgtab = (pte_t *)P2V(PTE_ADDR(*pde));
  }
  else
  {
    if (!alloc || (pgtab = (pte_t *)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char *)PGROUNDDOWN((uint)va);
  last = (char *)PGROUNDDOWN(((uint)va) + size - 1);
  for (;;)
  {
    if ((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if (*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap
{
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
    {(void *)KERNBASE, 0, EXTMEM, PTE_W},            // I/O space
    {(void *)KERNLINK, V2P(KERNLINK), V2P(data), 0}, // kern text+rodata
    {(void *)data, V2P(data), PHYSTOP, PTE_W},       // kern data+memory
    {(void *)DEVSPACE, DEVSPACE, 0, PTE_W},          // more devices
};

// Set up kernel part of a page table.
pde_t *
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if ((pgdir = (pde_t *)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void *)DEVSPACE)
    panic("PHYSTOP too high");
  for (k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if (mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                 (uint)k->phys_start, k->perm) < 0)
    {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void switchkvm(void)
{
  // V2P -> Virtual to Physical
  lcr3(V2P(kpgdir)); // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void switchuvm(struct proc *p)
{
  // 1. p라는 프로세스가 없거나, kernel stack이 없거나, page directory가 없으면 panic
  /*
    kernel stack이란?
    - process가 kernel mode에 들어갈 때 사용함 (sys call, int, kernel routines 등)
    - syscall 이나 intr가 발생했을 때 CPU는 user mode -> kernel mode로 전환
    - 이때 user stack에서 kernel stack으로 실행이 옮겨짐
  */
  if (p == 0)
    panic("switchuvm: no process");
  if (p->kstack == 0)
    panic("switchuvm: no kstack");
  if (p->pgdir == 0)
    panic("switchuvm: no pgdir");

  // 2. Intr 멈춰~
  pushcli();

  // 3. Task State Segment Setup -> CPU에 Intr 발생 시 현재 프로세스의 커널 스택을 사용하도록
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts) - 1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort)0xFFFF;
  ltr(SEG_TSS << 3);

  // 4. Page Directory로 바꿔라
  lcr3(V2P(p->pgdir)); // switch to process's address space
  // 5. 다시 intr 재가동
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if (sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W | PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if ((uint)addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walkpgdir(pgdir, addr + i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if (readi(ip, P2V(pa), offset + i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if (newsz >= KERNBASE)
    return 0;
  if (newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for (; a < newsz; a += PGSIZE)
  {
    mem = kalloc();
    if (mem == 0)
    {
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pgdir, (char *)a, PGSIZE, V2P(mem), PTE_W | PTE_U) < 0)
    {
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if (newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for (; a < oldsz; a += PGSIZE)
  {
    pte = walkpgdir(pgdir, (char *)a, 0);
    if (!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if ((*pte & PTE_P) != 0)
    {
      pa = PTE_ADDR(*pte);
      if (pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void freevm(pde_t *pgdir)
{
  uint i;

  if (pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for (i = 0; i < NPDENTRIES; i++)
  {
    if (pgdir[i] & PTE_P)
    {
      char *v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char *)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if (pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t *
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if ((d = setupkvm()) == 0)
    return 0;
  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walkpgdir(pgdir, (void *)i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if (!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char *)P2V(pa), PGSIZE);
    if (mappages(d, (void *)i, PGSIZE, V2P(mem), flags) < 0)
    {
      kfree(mem);
      goto bad;
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

// PAGEBREAK!
//  Map user virtual address to kernel address.
char *
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if ((*pte & PTE_P) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  return (char *)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char *)p;
  while (len > 0)
  {
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char *)va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if (n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

// PAGEBREAK!
//  Blank page.
// PAGEBREAK!
//  Blank page.
// PAGEBREAK!
//  Blank page.
// PA4
enum memstate
{
  UNUSING,
  USING
};
struct mmap_area
{
  struct file *f;      // 해당 파일
  uint addr;           // Address
  int length;          // 크기
  int offset;          // 파일 offset
  int prot;            // PROT_READ , PROT_READ|PROT_WRITE
  int flags;           // MAP_POPULATE, 0, MAP_POPULATE|MAP_ANONYMOUS
  struct proc *p;      // 해당 프로세스
  enum memstate state; // 현재 이 페이지 사용하고 있는지
};
struct
{
  struct spinlock lock; // page lock
  struct mmap_area maps[64];
} mtable;
struct mmap_area *mallock()
{
  struct mmap_area *map;
  acquire(&mtable.lock);
  for (map = mtable.maps; map < &mtable.maps[64]; map++)
    if (map->state == 0)
    {
      release(&mtable.lock);
      return map;
    }
  release(&mtable.lock);
  return 0;
}
int compare_prot(struct mmap_area *mmap)
{
  // 같으면 1, 다르면 0
  int file_flags = 0;
  if (mmap->f->readable)
    file_flags |= 1;
  if (mmap->f->writable)
    file_flags |= 2;
  return mmap->prot <= file_flags;
}
int map_populate(struct mmap_area *mmap)
{
  // Page Entry Mapping 되면 1, 안되면 0
  char *page;
  int pte_w = (mmap->prot & PROT_WRITE) ? PTE_W : 0;
  for (int i = 0; i < mmap->length / PGSIZE; i++)
  {
    //Page Allocation
    page = kalloc();
    // Clean
    memset(page, 0, PGSIZE);
    // Fileread
    fileread(mmap->f, page, PGSIZE);
    // OFFSET Init
    mmap->f->off = mmap->offset;
    // 현재 프로세스의 페이지 디렉토리에 새로 할당된 페이지 매핑
    mappages(myproc()->pgdir, (void *)mmap->addr + PGSIZE * i, PGSIZE, V2P(page), pte_w | PTE_U);
  }
  return 1;
}
int map_populate_annonymous(struct mmap_area *mmap)
{
  // Page Entry Mapping 되면 1, 안되면 0
  // Page Entry Mapping 되면 1, 안되면 0
  char *page;
  int pte_w = (mmap->prot & PROT_WRITE) ? PTE_W : 0;
  for (int i = 0; i < mmap->length / PGSIZE; i++)
  {
    // Page Allocation
    page = kalloc();
    // Clean
    memset(page, 0, PGSIZE);
    // 현재 프로세스의 페이지 디렉토리에 새로 할당된 페이지 매핑
    mappages(myproc()->pgdir, (void *)mmap->addr + PGSIZE * i, PGSIZE, V2P(page), pte_w | PTE_U);
  }
  return 1;
}
// // 해당 주소와 프로세스에 대한 매핑 영역을 찾는 별도의 함수
// struct mmap_area *find_mmap_area(uint addr)
// {
//   for (struct mmap_area *mmap = mtable.maps; mmap < &mtable.maps[64]; mmap++)
//   {
//     if (mmap->addr <= addr && mmap->addr + mmap->length > addr && mmap->p == myproc() && mmap->state ==USING)
//       return mmap;
//   }
//   return 0; // 찾지 못했으면 NULL 반환
// }
// //페이지 매핑
// int handle_page_mapping(struct mmap_area *mmap, uint addr, char *page)
// {
//   int prot = (mmap->prot & PROT_WRITE) ? PTE_W : 0;
//   memset(page, 0, PGSIZE); // 2. 새 페이지 0으로 초기화
//   //3. File Mapping 이였던 경우 파일을 읽어왔어야함.
//   if (mmap->f != 0)
//   {
//     fileread(mmap->f, page, PGSIZE); // 파일에서 읽어오기
//     mmap->f->off = mmap->offset;     // 파일 오프셋 설정
//   }
//   // 매핑 수행
//   return mappages(myproc()->pgdir, (void *)addr, PGSIZE, V2P(page), prot | PTE_U);
// }
// int page_fault_handler(uint addr, int io)
// {
//   addr = PGROUNDDOWN(addr);
//   acquire(&mtable.lock);                         // 잠금 획득
//   struct mmap_area *mmap = find_mmap_area(addr); // 매핑 영역 검색
//   if (!mmap || mmap->prot < io)
//   {
//     release(&mtable.lock); // 잠금 해제 및 에러 처리
//     return -1;
//   }

//   char *page = kalloc(); // 1. 새 페이지 할당
//   if (!page)
//   {
//     release(&mtable.lock); // 페이지 할당 실패 시 잠금 해제 및 에러 처리
//     return -1;
//   }

//   // 페이지 매핑 처리
//   int result = handle_page_mapping(mmap, addr, page);
//   release(&mtable.lock);       // 잠금 해제
//   return result == 0 ? 1 : -1; // 성공하면 1, 실패하면 0 반환
// }
int page_fault_handler(uint addr, int io)
{
  addr = PGROUNDDOWN(addr);
  struct mmap_area *mmap;
  char *page;
  int prot;
  acquire(&mtable.lock);
  for (mmap = mtable.maps; mmap < &mtable.maps[64]; mmap++)
  {
    if (mmap->addr <= addr && mmap->addr + mmap->length > addr && mmap->p == myproc() && mmap->state)
    {
      if (mmap->prot < io)
      {
        release(&mtable.lock);
        return -1;
      }
      if (mmap->f != 0) // File Mapping
      {
        prot = (mmap->prot > 1) ? 2 : 0;
        // 1. Allocate New Physical page
        page = kalloc();
        
        memset(page, 0, PGSIZE);
        release(&mtable.lock);
        // 3. Read File
        fileread(mmap->f, page, PGSIZE);
        mmap->f->off = mmap->offset;
        acquire(&mtable.lock);
        // 4, Page Mapping
        mappages(myproc()->pgdir, (void *)addr, PGSIZE, V2P(page), prot | PTE_U);
        release(&mtable.lock);
        return 1;
      }
      else // Anonymous Mapping
      {
        prot = (mmap->prot > 1) ? 2 : 0;
        // 1. Allocate New Physical page
        page = kalloc();
        // 2. Fill new page with 0
        memset(page, 0, PGSIZE);
        // 3. Page Mapping
        mappages(myproc()->pgdir, (void *)addr, PGSIZE, V2P(page), prot | PTE_U);
        release(&mtable.lock);
        return 1;
      }
    }
  }
  release(&mtable.lock);
  return -1;
}
uint mmap(uint addr, int length, int prot, int flags, int fd, int offset)
{
  // 1. Validate
  if (addr % PGSIZE != 0)
    return 0;
  if (length < 0 || length % PGSIZE != 0)
    return 0;
  if (prot != PROT_READ && prot != (PROT_READ | PROT_WRITE))
    return 0;
  if (flags & MAP_ANONYMOUS)
  {
    if (fd != -1 || offset != 0)
      return 0;
  }
  else
  {
    if (fd < 0) // Ensure valid file descriptor for file mapping
      return 0;
  }

  // 2. Check mmap_area array
  struct mmap_area *mmap;
  acquire(&mtable.lock);
  for (mmap = mtable.maps; mmap < &mtable.maps[64]; mmap++)
  {
    if (mmap->addr <= (addr + MMAPBASE) && mmap->addr + mmap->length > (addr + MMAPBASE) && mmap->state == USING && mmap->p == myproc())
    {
      // 할당을 원하는 공간을 이미 현재 이 프로세스가 쓰고 있다면 0을 반환한다.
      release(&mtable.lock);
      return 0;
    }
  }
  release(&mtable.lock);

  // 3.Init mmap_area
  mmap = mallock();
  mmap->f = 0; // 일단 파일 0으로 초기화
  mmap->addr = addr + MMAPBASE;
  mmap->length = length;
  mmap->prot = prot;
  mmap->flags = flags;
  mmap->p = myproc();
  mmap->state = USING;

  // 4. Worked
  if (flags & MAP_POPULATE)
  {
    if (flags & MAP_ANONYMOUS) // MAP_ANONYMOUS도 있는 경우
    // mmap(0,8192, PROT_READ, MAP_POPULATE | MAP_ANONYMOUS, -1, 0)
    {
      if(!map_populate_annonymous(mmap))
      {
        mmap->state = UNUSING;
        return 0;
      }
    }
    else // MAP_POPULATE만 있는 경우
    // mmap(0,8192, PROT_READ, MAP_POPULATE, fd, 4096)
    {
      // 1. INIT MMAP with FILE
      mmap->offset = offset;
      mmap->f = mmap->p->ofile[fd];
      mmap->f->off = offset;
      if (!compare_prot(mmap))
      {
        mmap->state = UNUSING;
        return 0;
      }
      if(!map_populate(mmap))
      {
        mmap->state = UNUSING;
        return 0;
      }
    }
  }
  else // MAP_POPULATE이 없는 경우
  // mmap(0,8192,PROT_READ, 0, fd, 4096)
  {
    mmap->f = mmap->p->ofile[fd];
    mmap->offset = offset;
    mmap->f->off = offset;
    if (!compare_prot(mmap))
    {
      mmap->state = UNUSING;
      return 0;
    }
  }

  return mmap->addr;
}

int munmap(uint addr)
{
  struct mmap_area *map;
  pte_t *pte = 0;
  // mmap_area 순회
  acquire(&mtable.lock);
  for (map = mtable.maps; map < &mtable.maps[64]; map++)
  {
    // 주어진 주소가 있다면?
    if (map->addr <= (addr+MMAPBASE) && map->addr + map->length >= (addr+MMAPBASE) && map->state == USING && map->p == myproc())
    {
      //mmap_area 순회
      for (int i = 0; i < map->length / PGSIZE; i++)
      {
        //Virtual Address와 매핑된 페이지 테이블 엔트리
        pte = walkpgdir(myproc()->pgdir, (void *)map->addr + i * PGSIZE, 0);
        //존재한다면 메모리에서 제거
        if (pte && (*pte & PTE_P))
        {
          // 존재한다면? 1로 채우고 freelist에 다시 넣고 Page Table Entry 비활성화
          memset(P2V(PTE_ADDR(*pte)), 1, PGSIZE);
          kfree(P2V(PTE_ADDR(*pte)));
          *pte = *pte & 0x0;
        }
      }
      map->f = 0;
      map->addr = 0;
      map->length = 0;
      map->offset = 0;
      map->prot = 0;
      map->flags = 0;
      map->p = 0;
      map->state = UNUSING;
      release(&mtable.lock);
      return 1;
    }
  }
  release(&mtable.lock);
  return -1;
}

int freemem()
{
  return freemems;
}
int map_fork(struct proc *proc)
{
  struct mmap_area *map;
  struct mmap_area *temp_map;
  pte_t *pte;
  acquire(&mtable.lock);

  // Iterate
  for (map = mtable.maps; map < &mtable.maps[64]; map++)
  {
    if (map->state==USING && map->p == myproc())
    {
      // 새 매핑을 위한 공간을 할당
      temp_map = mallock();

      // 매핑된 영역의 속성을 복사
      *temp_map = *map;
      temp_map->p = proc;  // 새 프로세스를 위한 소유권 설정
      temp_map->state = USING; // 활성 상태로 설정
      if (map->f)
      {
        // 파일이 있는 경우, 파일 오프셋을 설정
        temp_map->f->off = temp_map->offset;
      }

      // 매핑된 페이지를 새 프로세스의 주소 공간에 복사
      for (int j = 0; j < map->length / PGSIZE; j++)
      {
        // 기존 페이지 테이블 엔트리를 탐색
        pte = walkpgdir(myproc()->pgdir, (void *)map->addr + PGSIZE * j, 1);
        if (pte && (*pte & PTE_P))
        {
          // 페이지를 할당하고, 0으로 초기화
          char *page = kalloc();
          memset(page, 0, PGSIZE);
          int pte_w = temp_map->prot >= 2 ? 2 : 0;

          if (map->f)
          {
            // 파일에서 페이지 내용을 읽기
            fileread(map->f, page, PGSIZE); // 파일 읽기 작업
            map->f->off += PGSIZE;          // 파일 오프셋을 증가
          }

          // 새 프로세스의 페이지 테이블에 페이지를 매핑
          mappages(proc->pgdir, (void *)temp_map->addr + PGSIZE * j, PGSIZE, V2P(page), pte_w| PTE_U);
        }
      }
    }
  }

  // mtable의 잠금을 해제
  release(&mtable.lock);
  return 1; // 성공적으로 매핑이 완료
}
