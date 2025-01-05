// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"
#include "proc.h"


void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

struct page pages[PHYSTOP / PGSIZE]; // 전체 Physical Page
struct page *page_lru_head;          // head
int num_free_pages;
int num_lru_pages;
struct spinlock lru_lock; // lru 접근 락
char bitmap[BITMAP_SIZE]; //bitmap 공간

/*
고려사항
1. kalloc을 통해서 새로운 페이지가 할당되는 경우
-> 이떄 이미 페이지가 꽉차있다면
-> Clock algorithm을 통해서 페이지 Swap out 하고 할당

2. process가 종료해서 dealloc하는 경우
-> swap space에 있는 페이지도 같이 지워줘야돼
-> 이때 swap space에 있는 페이지를 지울 때는 secondary memory에 write 해줘야 함

3. copyuvm을 통해 user virtual memory을 복사하는 경우
-> Swap space에 있는 페이지도 복사해서 해당 swap space에 넣어줘야 한다.

4. swap out된 페이지에 접근하려는 경우 (Page Fault handling)
-> Swap in 다만 꽉 차 있다면 다시 또 Swap out 을 수행 후 swap in 해야함.

5. swap space를 physical page로 관리해야함.
*/
void set_bit(int index){
  //for swap out
  bitmap[index / 8] |= (1 << (index % 8));
}
void clear_bit(int index){
  //for swap in
  bitmap[index / 8] &= ~(1 << (index % 8));
}
int check_bit(int index){
  return bitmap[index / 8] & (1 << (index % 8));
}
int find_free_swap_index(void){
  for (int i = 0; i < BITMAP_SIZE;i++)
  {
    if(!check_bit(i)){
      return i;
    }
  }
  return -1; 
}

void append_lru(pde_t *pgdir, uint va, uint pa)
{
  // 1. page 선택
  struct page *newPage = &pages[pa / PGSIZE];

  // 2. 락 걸고 할당
  acquire(&lru_lock);
  newPage->vaddr = (char *)va;
  newPage->pgdir = pgdir;
  if (num_lru_pages == 1) // 현재 한장
  {
    page_lru_head = newPage;
    newPage->prev = newPage;
    newPage->next = newPage;
  }
  else
  { // 아니라면?
    newPage->prev = page_lru_head->prev;
    newPage->next = page_lru_head;
    page_lru_head->prev->next = newPage;
    page_lru_head->prev = newPage;
  }
  num_lru_pages++;
  // 3. 할당 해제
  release(&lru_lock);
  return;
}
void pop_lru(uint pa)
{
  // 1. page 선택
  struct page *deletePage = &pages[pa / PGSIZE];

  // 2. 락걸고 삭제
  acquire(&lru_lock);
  if (deletePage == page_lru_head)
    page_lru_head = 0; // 초기화
  else
  {
    // 이어주고
    deletePage->prev->next = deletePage->next;
    deletePage->next->prev = deletePage->prev;
    // 삭제
    deletePage->next = 0;
    deletePage->prev = 0;
    deletePage->vaddr = 0;
    deletePage->pgdir = 0;
  }
  // 3. 해제
  num_lru_pages--;
  release(&lru_lock);
  return;
}
int reclaim()
{
  cprintf("130\n");
  // 현재 빈 페이지가 없어서 clock algorithm에 따른 Swap out을 수행해 페이지를 만들어야 되는 상황
  acquire(&lru_lock);
  pte_t *targetPte; 
  // 1. swap out 할 페이지가 없는 경우
  if(num_lru_pages == 0)
  {
    cprintf("Out of Memory\n");
    release(&lru_lock);
    return 0;
  }
  while(1)
  {
    //Page entry 찾기
    targetPte = walkpgdir(page_lru_head->pgdir, (char*)PGROUNDDOWN((uint)(void *)page_lru_head->vaddr), 0);
    //Reference bit 구하기
    int reference_bit = (*targetPte) & PTE_A;
    if(reference_bit) //최근 참조된적 있는 경우
    {
      *targetPte &= ~PTE_A; //비트 0으로 만들기
      page_lru_head = page_lru_head->next; // 그다음 lru list 조회
    }
    else{ //최근 참조된적 없는 경우
      //LRU 관리 -> targetPage만 뽑기
      struct page *targetPage;
      targetPage = page_lru_head;
      page_lru_head = targetPage->next;
      page_lru_head->prev = targetPage->prev;
      targetPage->prev->next = page_lru_head;
      if(page_lru_head == targetPage){
        page_lru_head = 0;
      }

      //swap space에 victim page 올리기
      int swap_index = find_free_swap_index();
      if(swap_index<0){
        cprintf("Swap space full\n");
        release(&lru_lock);
        return 0;
      }

      //Victim page을 swap space에 쓰기
      char *vaddr = targetPage->vaddr;
      swapwrite(vaddr, swap_index);
      //swap bitmap 관리
      set_bit(swap_index);

      //Update the PTE and PTE_P clear
      uint flags = PTE_FLAGS(*targetPte);
      *targetPte &= ~0xFFFFF000;
      *targetPte = (swap_index << 12) | flags;
      *targetPte &= ~PTE_P;

      //flush the TLB
      // lcr3(V2P(myproc()->pgdir));
      flush();

      //Physical page free
      release(&kmem.lock);
      kfree(P2V(PTE_ADDR(*targetPte)));
      break;
    }
  }
  release(&lru_lock);
  return 1;
}
/*
targetPte = (uint*)pgdir2pte(targetPage->pgdir, targetPage->vaddr);
*targetPte &= ~0xFFFFF000;
*targetPte |= (uint)temp<<12;
*targetPte &= (~PTE_P);
flush_TLB();
temp++;
release(&lru_lock);
release(&kmem.lock);
kfree(targetMem);
*/

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
  memset(bitmap, 0, BITMAP_SIZE); // bitmap 초기화 해두기
}

void kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
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

try_again:
  if (kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if (!r && reclaim())
    goto try_again;
  if (r)
    kmem.freelist = r->next;
  if (kmem.use_lock)
    release(&kmem.lock);
  return (char *)r;
}
