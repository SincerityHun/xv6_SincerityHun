// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld
int freemems;      // Current number of free memory page

struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock; // 사용할거면 이거 써
  int use_lock;         // lock을 사용할 거야?
  struct run *freelist;
} kmem;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void kinit1(void *vstart, void *vend)
{
  // 1. kernel memory에 spinlock 초기화
  initlock(&kmem.lock, "kmem");
  // 2. 아직 Multiprocessing 전, spinlock을 사용하지 않는다.
  kmem.use_lock = 0;
  // 3. 주어진 시작(vstart)과 끝(vend) 주소 사이의 virtual memory를 free list에 추가
  freerange(vstart, vend);
}

void kinit2(void *vstart, void *vend)
{
  // 1. 주어진 시작(vstart)과 끝(vend) 주소 사이의 virtual memory를 free list에 추가
  freerange(vstart, vend);
  // 2. 이제 spinlock을 활성화
  kmem.use_lock = 1;
}

void freerange(void *vstart, void *vend)
{
  char *p;
  p = (char *)PGROUNDUP((uint)vstart);
  for (; p + PGSIZE <= (char *)vend; p += PGSIZE)
    kfree(p);
}
// PAGEBREAK: 21
//  Free the page of physical memory pointed at by v,
//  which normally should have been returned by a
//  call to kalloc().  (The exception is when
//  initializing the allocator; see kinit above.)
void kfree(char *v)
{
  struct run *r;

  if ((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);

  if (kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run *)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  freemems++; // Free page number Increase
  if (kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char *
kalloc(void)
{
  struct run *r;

  if (kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  freemems--; // Free page number decrease
  if (kmem.use_lock)
    release(&kmem.lock);
  return (char *)r;
}
