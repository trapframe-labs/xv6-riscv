#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}


// Make a direct-map page table for the kernel.
pagetable_t
kvmmake_per_process(struct proc* p)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // map kernel stacks
  proc_mapstacks_per_process(kpgtbl, p);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Initialize the one kernel_pagetable
pagetable_t 
kvminit_per_process(struct proc* p)
{
  pagetable_t kpagetable = kvmmake_per_process(p);
  return kpagetable;
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;   
    if((*pte & PTE_V) == 0)  // has physical page been allocated?
      continue;
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, pagetable_t kpagetable_per_proc, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    // if already mapped, remove previous mapping before remapping same address
    pte_t *pte = walk(kpagetable_per_proc, a, 0);
    if(pte!=0) {
      if(*pte & PTE_V) {
        uvmunmap(kpagetable_per_proc, a, 1, 0);
      }
    }
    if(mappages(kpagetable_per_proc, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  struct proc* p = myproc();
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
    uvmunmap(p->kpagetable_per_proc, PGROUNDUP(newsz), npages, 0);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Recursively free page-table pages.
// All leaf mappings should be ignored.
void
freewalk_per_process(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    // leaf mappins are ignored when we don't check permissions
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk_per_process((pagetable_t)child);
      pagetable[i] = 0;
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, pagetable_t kpagetable_per_proc, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   // page table entry hasn't been allocated
    if((*pte & PTE_V) == 0)
      continue;   // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
    // if already mapped, remove previous mapping before remapping same address
    if(mappages(kpagetable_per_proc, i, PGSIZE, (uint64)mem, (flags & (~PTE_U))) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);         // get the start address of current page
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);   // covert virtual -> physical address
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
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

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  struct proc* p = myproc();
  
  // cannot copy anything from the kernel
  if(srcva >= KERNBASE)
    return -1;

  char* src = (char*)srcva;
  for(int i=0; i<len; ++i) {
    if((uint64)src > (p->sz))
      return -1;
    *dst++ = *src++;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  struct proc* p = myproc();

  // cannot copy anything from the kernel
  if(srcva >= KERNBASE)
    return -1;
  
  char* s = (char*)srcva;
  char* d = (char*)dst;
  
  for(int i=0; i<max; ++i) {
    if((uint64)s >= p->sz)
      return -1;
    *d = *s;
    if(*d == '\0')
      return 0;
    ++d; ++s;
  }
  return -1;
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}

/*
  Quesiton 1: Define a function called vmprint(). It should take a pagetable_t argument, and print
  that pagetable in the format described below. Insert if(p->pid==1) vmprint(p->pagetable)
  in exec.c just before the return argc, to print the first process's page table. You
  receive full credit for this assignment if you pass the pte printout test of make grade.

  page table 0x0000000087f6e000
  ..0: pte 0x0000000021fda801 pa 0x0000000087f6a000
  .. ..0: pte 0x0000000021fda401 pa 0x0000000087f69000
  .. .. ..0: pte 0x0000000021fdac1f pa 0x0000000087f6b000
  .. .. ..1: pte 0x0000000021fda00f pa 0x0000000087f68000
  .. .. ..2: pte 0x0000000021fd9c1f pa 0x0000000087f67000
  ..255: pte 0x0000000021fdb401 pa 0x0000000087f6d000
  .. ..511: pte 0x0000000021fdb001 pa 0x0000000087f6c000
  .. .. ..510: pte 0x0000000021fdd807 pa 0x0000000087f76000
  .. .. ..511: pte 0x0000000020001c0b pa 0x0000000080007000

  The first line displays the argument to vmprint. After that there is a line for each PTE,
  including PTEs that refer to page-table pages deeper in the tree. Each PTE line is indented
  by a number of " .." that indicates its depth in the tree. Each PTE line shows the PTE
  index in its page-table page, the pte bits, and the physical address extracted from the
  PTE. Don't print PTEs that are not valid. In the above example, the top-level page-table
  page has mappings for entries 0 and 255. The next level down for entry 0 has only index 0
  mapped, and the bottom-level for that index 0 has entries 0, 1, and 2 mapped.

  Your code might emit different physical addresses than those shown above.
  The number of entries and the virtual addresses should be the same.

  Some hints:
  * You can put vmprint() in kernel/vm.c.
  * Use the macros at the end of the file kernel/riscv.h.
  * The function freewalk may be inspirational.
  * Define the prototype for vmprint in kernel/defs.h so that you can call it from exec.c.
  * Use %p in your printf calls to print out full 64-bit hex PTEs and addresses as shown in the example.

  Explain the output of vmprint in terms of Fig 3-4 from the text. What does page 0 contain? What is in
  page 2? When running in user mode, could the process read/write the memory mapped by page 1?
*/

void vmprint_levels(pagetable_t pagetable, int level, int depth)
{
  const int max_ptes = 512;
  for (int i = 0; i < max_ptes; ++i)
  {
    pte_t pte = pagetable[i];
    if (pte & PTE_V)
    {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      for (int i = 0; i < depth; ++i)
      {
        printf(".. ");
      }
      printf("..%d: pte %p pa %p\n", i, (void*)pte, (void*)child);
      if ((pte & (PTE_R | PTE_W | PTE_X)) == 0)
        vmprint_levels((pagetable_t)child, level - 1, depth + 1);
    }
  }
  return;
}

void vmprint(pagetable_t pagetable)
{
  uint level = 2, depth = 0;
  printf("page table %p\n", pagetable);
  vmprint_levels(pagetable, level, depth);
  printf("\n");
}

/*
  q) Explain the output of vmprint in terms of Fig 3-4 from the text.
  a) It is a multi-level page table structure. Prints out all valid PTEs.
  It contains all the sections like text, data, stack, guard page, trampoline etc.

  q) What does page 0 contain?
  a) Considering leaf page 0. It is a page at level 0. It contains the text section
  of the program as per the textbook.

  q) What is in page 2?
  a) This most likely contains more program data (BSS / uninitialized data). The ELF
  header shows that the data segment memsz extends beyond one page, so the next
  page after page 1 will still belong to the program's data region rather than the heap.

  q) When running in user mode, could the process read/write the memory mapped by page 1?
  a) Yes, we should be able to read/write to page 1. Page 1 as per the textbook is data section,
  which is not executable, but should be read/write.

Question 2: A kernel page table per process (hard)
Xv6 has a single kernel page table that's used whenever it executes in the kernel. The kernel page table is a direct mapping to physical addresses, 
so that kernel virtual address x maps to physical address x. Xv6 also has a separate page table for each process's user address space, containing only 
mappings for that process's user memory, starting at virtual address zero. Because the kernel page table doesn't contain these mappings, user addresses 
are not valid in the kernel. Thus, when the kernel needs to use a user pointer passed in a system call (e.g., the buffer pointer passed to write()), 
the kernel must first translate the pointer to a physical address. The goal of this section and the next is to allow the kernel to directly dereference 
user pointers.

Your first job is to modify the kernel so that every process uses its own copy of the kernel page table when executing in the kernel. Modify struct 
proc to maintain a kernel page table for each process, and modify the scheduler to switch kernel page tables when switching processes. For this step, 
each per-process kernel page table should be identical to the existing global kernel page table. You pass this part of the lab if usertests runs correctly.
Read the book chapter and code mentioned at the start of this assignment; it will be easier to modify the virtual memory code correctly with an 
understanding of how it works. Bugs in page table setup can cause traps due to missing mappings, can cause loads and stores to affect unexpected pages of
physical memory, and can cause execution of instructions from incorrect pages of memory.

Some hints:

* Add a field to struct proc for the process's kernel page table.
* A reasonable way to produce a kernel page table for a new process is to implement a modified version of kvminit that makes a new page table instead of 
modifying kernel_pagetable. You'll want to call this function from allocproc.
* Make sure that each process's kernel page table has a mapping for that process's kernel stack. In unmodified xv6, all the kernel stacks are set up in 
procinit. You will need to move some or all of this functionality to allocproc.
* Modify scheduler() to load the process's kernel page table into the core's satp register (see kvminithart for inspiration). Don't forget to call 
sfence_vma() after calling w_satp().
scheduler() should use kernel_pagetable when no process is running.
* Free a process's kernel page table in freeproc.
* You'll need a way to free a page table without also freeing the leaf physical memory pages.
* vmprint may come in handy to debug page tables.
* It's OK to modify xv6 functions or add new functions; you'll probably need to do this in at least kernel/vm.c and kernel/proc.c. (But, don't modify 
kernel/vmcopyin.c, kernel/stats.c, user/usertests.c, and user/stats.c.)
* A missing page table mapping will likely cause the kernel to encounter a page fault. It will print an error that includes sepc=0x00000000XXXXXXXX. 
You can find out where the fault occurred by searching for XXXXXXXX in kernel/kernel.asm.

Question 3: Simplify copyin/copyinstr (hard)
* The kernel's copyin function reads memory pointed to by user pointers. It does this by translating them to physical addresses, which the kernel can 
directly dereference. It performs this translation by walking the process page-table in software. Your job in this part of the lab is to add user 
mappings to each process's kernel page table (created in the previous section) that allow copyin (and the related string function copyinstr) to 
directly dereference user pointers.
* Replace the body of copyin in kernel/vm.c with a call to copyin_new (defined in kernel/vmcopyin.c); do the same for copyinstr and copyinstr_new. Add
 mappings for user addresses to each process's kernel page table so that copyin_new and copyinstr_new work. You pass this assignment if usertests runs
  correctly and all the make grade tests pass.
* This scheme relies on the user virtual address range not overlapping the range of virtual addresses that the kernel uses for its own instructions and
 data. Xv6 uses virtual addresses that start at zero for user address spaces, and luckily the kernel's memory starts at higher addresses. However, this
  scheme does limit the maximum size of a user process to be less than the kernel's lowest virtual address. After the kernel has booted, that address 
  is 0xC000000 in xv6, the address of the PLIC registers; see kvminit() in kernel/vm.c, kernel/memlayout.h, and Figure 3-4 in the text. You'll need to 
  modify xv6 to prevent user processes from growing larger than the PLIC address.

Some hints:

* Replace copyin() with a call to copyin_new first, and make it work, before moving on to copyinstr.
* At each point where the kernel changes a process's user mappings, change the process's kernel page table in the same way. Such points include fork(),
 exec(), and sbrk().
* Don't forget that to include the first process's user page table in its kernel page table in userinit.
* What permissions do the PTEs for user addresses need in a process's kernel page table? (A page with PTE_U set cannot be accessed in kernel mode.)
* Don't forget about the above-mentioned PLIC limit.
* Linux uses a technique similar to what you have implemented. Until a few years ago many kernels used the same per-process page table in both user and 
kernel space, with mappings for both user and kernel addresses, to avoid having to switch page tables when switching between user and kernel space. 
However, that setup allowed side-channel attacks such as Meltdown and Spectre.
*/
