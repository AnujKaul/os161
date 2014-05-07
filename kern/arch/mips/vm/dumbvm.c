/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <thread.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <page.h>
#include <synch.h>
#include <uio.h>
#include <stat.h>
#include <kern/iovec.h>
#include <vfs.h>
#include <vnode.h>
#include <kern/fcntl.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12


#define ONDISK 'd'
#define INMEMORY 'r'


/*
 * Wrap rma_stealmem in a spinlock.


 */
//declare the coremap. Will be kept static

static struct page * coremap;

static struct lock * lk_core_map;
static paddr_t firstaddr, lastaddr, freeaddr;
static unsigned long no_of_pages;
static int is_vm_bootstrapped = 0;

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

// declaration for page swapping - Anuj Kaul zindabaad - Iska code zindabaad tha, zindabaad hai aur zindabaad rahega(sunny deol style)
struct vnode* swapfile;
static unsigned long no_of_swap_slots;
static unsigned int swap_index = 0;


void
vm_bootstrap(void)
{
	unsigned long i;
	unsigned long no_of_kern_pages;	
	ram_getsize(&firstaddr,&lastaddr);
	
	no_of_pages = (lastaddr-firstaddr)/PAGE_SIZE;

	coremap = (struct page *)PADDR_TO_KVADDR(firstaddr);
	freeaddr = firstaddr + (no_of_pages * sizeof(struct page));	
	// find number of kernel pages to be marked fixed 
	no_of_kern_pages = (freeaddr - firstaddr)/PAGE_SIZE + 1;
	for(i=0;i<no_of_pages;i++)
	{
		
		coremap[i].as = NULL;
		coremap[i].va = 0;
		coremap[i].timestamp = 0;
		coremap[i].num_pages = 0;
		if(i < no_of_kern_pages) //all coremap entry pages are marked as fixed. So no swapping for these pages. we dont want trouble
		coremap[i].cur_state = FIXED;
		else
		coremap[i].cur_state = FREE;
	}
	lk_core_map = lock_create("coremap_lock");
	is_vm_bootstrapped = 1;
	/* Do nothing. */
}

/*Kaul saab ki jai*/
/********************************** PAGE SWAP ************************************************/

void pageswap(){

		struct stat tpage;
		char * console = kstrdup("lhd0raw:");
		int result = vfs_open(console, O_RDWR,0664, &swapfile);
		if(result){
			panic("Virtual Memory Error: Swap space could not be created!!! \n");
		}
		
		VOP_STAT(swapfile,&tpage);
		no_of_swap_slots = tpage.st_size/PAGE_SIZE;
}

paddr_t seek_victim(int seltype){

	//... page was not available request for swap, using the algorithm
	int index;
	struct pagetable *pgtable;

	int flag = 0;


	if(seltype == 0){ 	// select at random

		while(1){

			index = random()%no_of_pages;
			if(coremap[index].cur_state==DIRTY){

				coremap[index].as = curthread->t_addrspace;
				pgtable = coremap[index].as->table;
				do{
					if(pgtable->pa == firstaddr + index * PAGE_SIZE && pgtable->swap_status != ONDISK){
						pgtable->swap_status = ONDISK;
						pgtable->indx_swapfile = swap_index;
						flag = 1;
						break;
					}
					pgtable = pgtable->next;

				}while(pgtable->next != NULL);
			}
			if(flag == 1){
				break;
			}

		}

	}
	else if(seltype == 1){ 	// paging algorithm
		// likh liya tumne algorithm - DS kar lo pehle
	}

	return(firstaddr + (index * PAGE_SIZE));
}

void write_page(unsigned int swpindx, paddr_t swap_page){

	struct iovec io_swap;
	struct uio uio_swap;
	uio_kinit(&io_swap, &uio_swap,(void*)PADDR_TO_KVADDR(swap_page), PAGE_SIZE, swpindx * PAGE_SIZE, UIO_WRITE);


	int reserror = VOP_WRITE(swapfile,&uio_swap);
	if(reserror){
		panic("VM ERROR : Swapping out Failed");
	}
	tlb_invalidate(swap_page);
}

void tlb_invalidate(paddr_t paddr)
{
	uint32_t ehi,elo,i;
	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if ((elo & PAGE_FRAME) == (paddr &  PAGE_FRAME))	{
			tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
		}
	}
}

void tlb_invalidate_all()
{
	int i;
	for (i=0; i<NUM_TLB; i++) {
		{
			tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
		}
	}
}
void handle_pagefault(vaddr_t virt_addr) {
	paddr_t ppage_tbw = alloc_upages(1);
	struct pagetable *head;
	struct pagetable *current;

	head = curthread->t_addrspace->table;
	current = head;

	do{
		if(current->va == virt_addr){
			if(current->swap_status == ONDISK){
				//current->indx_swapfile;
				read_page(current->indx_swapfile,ppage_tbw);
				current->swap_status=INMEMORY;
				current->pa = ppage_tbw;
			}
			break;
		}
		current = current->next;
	}while(current->next != NULL);


}


void read_page(unsigned int frmflindx, paddr_t swap_to_mem){
	//if(swap_to_mem >= firstaddr){
	//	panic("VM ERROR : Again messing in the kernel address space!!");
	//}

	struct iovec io_swap;
	struct uio uio_swap;
	uio_kinit(&io_swap, &uio_swap, (void*)PADDR_TO_KVADDR(swap_to_mem), PAGE_SIZE, frmflindx * PAGE_SIZE, UIO_READ);
	int result=VOP_READ(swapfile, &uio_swap);

	if(result) {
		panic("VM ERROR: Swapping in Failed!");
	}

}

/****************************************************************************************************/


/* Addrspace functions*/
/*******************************************************************************************************************/
struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

	as->regions = NULL;
	as->table = NULL;	
	as->as_stackvbase = 0;
	as->as_stackvtop = 0;

	return as;
}

void
as_destroy(struct addrspace *as)
{
	struct region * cur_region;
	
	struct pagetable * cur_page;

	while (as->regions != NULL)
	{
		cur_region = as->regions;
		as->regions = as->regions->next;
		kfree(cur_region);
	}

	while(as->table != NULL)
	{
		cur_page = as->table;
		if(cur_page->pa == 0)
		{
			KASSERT(cur_page->pa != 0);
		}
		if(cur_page->swap_status != ONDISK)
		{
			free_upages(cur_page->pa);
		}
		as->table = as->table->next;		
		kfree(cur_page);
	}
	
	kfree(as);
}

void
as_activate(struct addrspace * as)
{
	int i, spl;

	(void)as;

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 
	struct region * insert_region;
	struct region * tmp_region;
	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;
	
	
	insert_region = (struct region *)kmalloc(sizeof(struct region));
	insert_region->va = vaddr;	
	insert_region->no_of_pages = npages;
	insert_region->next = NULL;
	if(as->regions == NULL)
	{
		as->regions = insert_region;
	}
	else
	{
		tmp_region = as->regions;
		while(tmp_region->next != NULL)
		{
			tmp_region = tmp_region->next;
		}
		tmp_region->next = insert_region;
	}
	as->heap_start = vaddr + npages * PAGE_SIZE;
 	as->heap_end = vaddr + npages * PAGE_SIZE;	
	as->heap_pages = 0;
	return 0;
}

/*static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}*/

/*
	this is where i gets a little tricky. we create the address space for the process. We allocate user pages for all the defined
	regions in as_define region. as user alloc accepts only one page at a time , we allocate one page at a time
*/
int
as_prepare_load(struct addrspace *as)
{
	(void)as;
	/*struct region * tmp_region;
	struct pagetable * page_entry;
	struct pagetable * tmp_page;
	tmp_region = as->regions;
	
	size_t count = 0;
	size_t i;*/

	/*while(tmp_region != NULL)
	{	
		// get the region
		vaddr_t region_base;
		region_base = tmp_region->va;
		count = tmp_region->no_of_pages;	
		tmp_page = as->table;
		if(tmp_page != NULL)
		{
			while(tmp_page->next != NULL)
			{
				tmp_page = tmp_page->next;
			}
		}
		// find the number of pages in the region and start allocationg it. alloc will bzero for us :)
		for(i=0;i<count;i++)
		{

		 	page_entry = (struct pagetable *)kmalloc(sizeof(struct pagetable));
			if(page_entry == NULL)
			{
				panic("could not allocate kernel memory\n");
			}
			//virtual address being computed according to the base address for the region.
			page_entry->va = region_base + i * PAGE_SIZE;			
			page_entry->pa = alloc_upages(1);
			page_entry->next = NULL;
			if(tmp_page == NULL)
			{
				as->table = page_entry;
				tmp_page = as->table;
			}
			else
			{
				tmp_page->next = page_entry;
				tmp_page = tmp_page->next;			
			}
		}
		tmp_region = tmp_region->next;
	}*/
	
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{

	as->as_stackvbase = USERSTACK;
	as->as_stackvtop = USERSTACK;
	*stackptr = USERSTACK;
	return 0;
}

/*
	copy the entire thing into a new address space. we will create new pages for the addressspace, copy the contents from the old ones
	we do it for everythis, the stackbase, regions. page table, and heap start and end
*/
int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	//int spl;
	
	struct addrspace *new;
	struct region * temp_region;
	struct region * old_region;
	struct region * new_region;
	struct pagetable * temp_table;
	struct pagetable * old_table;
	struct pagetable * new_table;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}
	old_region = old->regions;
	// copy all the regions. badly written code. 	
	while(old_region != NULL)
	{
		new_region = (struct region *)kmalloc(sizeof(struct region));
		new_region->va = old_region->va;
		new_region->no_of_pages = old_region->no_of_pages;
		new_region->next = NULL;
		if(new->regions == NULL)
		{	
			new->regions = new_region;
		}
		else
		{
			temp_region = new->regions;
			while(temp_region->next != NULL)
			{
				temp_region = temp_region->next;
			}
			temp_region->next = new_region;
		}
		old_region = old_region->next;
		
	}
	// copy the page table. equally bad code as above.
	old_table = old->table;
	while(old_table != NULL)
	{
		new_table = (struct pagetable *)kmalloc(sizeof(struct pagetable));
		new_table->va = old_table->va;
		new_table->pa = alloc_upages(1);
		new_table->next = NULL;
		memmove((void *)PADDR_TO_KVADDR(new_table->pa),(const void *)PADDR_TO_KVADDR(old_table->pa),PAGE_SIZE);
		if(new->table == NULL)
		{	
			new->table = new_table;
		}
		else
		{
			temp_table = new->table;
			while(temp_table->next != NULL)
			{
				temp_table = temp_table->next;
			}
			temp_table->next = new_table;
		}
		old_table = old_table->next;

 	}
	new->heap_start = old->heap_start;
	new->heap_end = old->heap_end;
	new->as_stackvbase = old->as_stackvbase;
	new->as_stackvtop = old->as_stackvtop;	
	*ret = new;
	return 0;
}


/******************************************************************************************************************************/

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);
	
	spinlock_release(&stealmem_lock);
	return addr;
}


//Here we allocate user level pages. We only allocate one page at a time. The logic will get easy later on. 
paddr_t alloc_upages(int npages)
{
	unsigned long i;
	int isPageAvailable = 1;
	lock_acquire(lk_core_map);
	if(npages == 1)
	{
		// Just scan through the entire list of coremap entries. Find a free page and allocate it. easy enough. Lets see if it works
		// Pt to be noted. We return the physical address. So that we can store in the page table :)
		for(i=0;i<no_of_pages;i++)
		{
			if(coremap[i].cur_state == FREE)
			{
				// mark as dirty.
				coremap[i].cur_state = DIRTY;
				coremap[i].va = PADDR_TO_KVADDR(firstaddr + i * PAGE_SIZE);
				coremap[i].as = curthread->t_addrspace;
				// bzero all allocated pages
				bzero((void*)(PADDR_TO_KVADDR(firstaddr + i * PAGE_SIZE)),PAGE_SIZE);			
				//offset to the real physical page
					lock_release(lk_core_map);
				return firstaddr + i * PAGE_SIZE;
			}
			else{
				isPageAvailable = 0;
			}
			
		}
		if(isPageAvailable == 0){ // i.e. no free physical memory blocks time to free them has come
			paddr_t returnPhyPage = seek_victim(0);
			write_page(swap_index,returnPhyPage);				
			swap_index++;
			bzero((void*)(PADDR_TO_KVADDR(returnPhyPage)),PAGE_SIZE);			
			lock_release(lk_core_map);
			return returnPhyPage;
		}
	}
	lock_release(lk_core_map);
	panic("could not allocate a page.....!\n");
	return 0;
}

// As we allocated only one user level page at a time. We only free one page in this case too. Another hope this works
void free_upages(paddr_t pa)
{
	
	unsigned long i;
	
	for(i=0;i<no_of_pages;i++)
	{
		if(firstaddr + i * PAGE_SIZE == pa)
		{
			// set the state to free so others can use it.
			coremap[i].cur_state = FREE;
			coremap[i].va = 0;
			coremap[i].as = NULL;
			//not necessary but should do it
			//bzero((void*)(PADDR_TO_KVADDR(firstaddr + i * PAGE_SIZE)),PAGE_SIZE);		
			break;
		}
	}
	if(i>=no_of_pages)
	{
		panic("could not free a page\n");
	}
}

/* Allocate/free some  kernel-space virtual pages */
// this is badly written. But it works(i guess). Just find n contineous free spaces and allocate the space. This is why we store the num_pages in the page array. Will later help to free all the allocated memory
vaddr_t 
alloc_kpages(int npages)
{
	unsigned long i;
	unsigned long j;
	struct pagetable * pgtable;
	int count = 0;
	paddr_t pa;
	if(is_vm_bootstrapped == 0)
	{
		pa = getppages(npages);
		if (pa==0) {
			return 0;
		}
		return PADDR_TO_KVADDR(pa);
	}
	else
	{
		lock_acquire(lk_core_map);
		for(i=0;i<no_of_pages;i++)
		{
			if(coremap[i].cur_state == FREE)
			{
				count++;
				if(count == npages)
				{
					for(j=i;j> i-npages; j--)
					{
						coremap[j].cur_state = FIXED;
						bzero((void*)(PADDR_TO_KVADDR(firstaddr + j * PAGE_SIZE)),PAGE_SIZE);		
					}
					coremap[(i - npages) + 1].num_pages = count;
					lock_release(lk_core_map);
					return PADDR_TO_KVADDR(firstaddr + (((i - npages) + 1) * PAGE_SIZE));
					
				}
			}
			else
			{
				// start re-counting is case no contineous free space found
				count = 0;
			}
		}
		
		if(npages == 1)
		{
			while(1)
			{
				int index = random()%no_of_pages;
				if(coremap[index].cur_state == DIRTY)
				{
					coremap[index].cur_state = FIXED;
					//bzero((void*)(PADDR_TO_KVADDR(firstaddr + j * PAGE_SIZE)),PAGE_SIZE);
					pgtable = coremap[index].as->table;
					do{
						if(pgtable->pa == firstaddr + index * PAGE_SIZE){
						pgtable->swap_status = ONDISK;
						pgtable->indx_swapfile = swap_index;
						write_page(swap_index,pgtable->pa);				
						swap_index++;
						coremap[index].as = NULL;
						coremap[index].num_pages = 1;
						bzero((void*)(PADDR_TO_KVADDR(firstaddr + index * PAGE_SIZE)),PAGE_SIZE);	
						break;
					}
						pgtable = pgtable->next;
					}while(pgtable->next != NULL);
						
						
				}
				else
				{
					continue;
				}
				
				lock_release(lk_core_map);
				return PADDR_TO_KVADDR(firstaddr + index * PAGE_SIZE);
					
			}
		}
	}
	lock_release(lk_core_map);
	
	panic("could not allocate a kernel page.....!\n");
	return 0;
}

void 
free_kpages(vaddr_t addr)
{
	/* nothing - leak the memory. */
	// we will now free the momory. n contineous allocations should be freed
	unsigned long i;
	int j;
	for(i=0;i<no_of_pages;i++)
	{
		if(PADDR_TO_KVADDR(firstaddr + i * PAGE_SIZE) == addr)
		{
			for(j=0;j<coremap[i].num_pages;j++)	
			{
				//bzero((void *)PADDR_TO_KVADDR(firstaddr + (i +j) * PAGE_SIZE), PAGE_SIZE);
				coremap[i+j].num_pages = 0;
				coremap[i+j].cur_state = FREE;
			}
			break;
		}	
	}

}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{

	//vaddr_t vbase;
	//, stackbase, stacktop;
	struct pagetable * tmp_page;
	struct addrspace * as;
	struct pagetable * page_entry;
	struct region * tmp_region;
	paddr_t paddr;
	int pg_count;
	int i;
	uint32_t ehi, elo;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);
	if(swapfile == NULL)
	{
		pageswap();
	}
	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	as = curthread->t_addrspace;
	if(as == NULL)
		return EFAULT;

	tmp_page = as->table;
	while(tmp_page != NULL)
	{
		if((tmp_page->va <= faultaddress) && ((tmp_page->va + PAGE_SIZE) > faultaddress))
		{
			if(tmp_page->swap_status == ONDISK)
			{
				handle_pagefault(tmp_page->va);
			}
			paddr = faultaddress + tmp_page->pa - tmp_page->va;		

			ehi = faultaddress;
			elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
			DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
			tlb_random(ehi, elo);

			return 0;

		}
		tmp_page = tmp_page->next;

	}
	tmp_region = as->regions;
	while(tmp_region !=NULL)
	{
		if(faultaddress >= tmp_region->va && faultaddress < tmp_region->va + PAGE_SIZE * tmp_region->no_of_pages)
		{
			i = (faultaddress - tmp_region->va) / PAGE_SIZE;
			page_entry = (struct pagetable *)kmalloc(sizeof(struct pagetable));
			if(page_entry == NULL)
			{
				panic("could not allocate kernel memory\n");
			}
			page_entry->va = tmp_region->va + i * PAGE_SIZE;
			page_entry->pa = alloc_upages(1);
			page_entry->swap_status = INMEMORY;
			page_entry->next = NULL;
			tmp_page = as->table;
			if(tmp_page == NULL)
			{
				as->table = page_entry;
			}
			else
			{
				while(tmp_page->next != NULL)
				{
					tmp_page = tmp_page->next;
				}			
				tmp_page->next = page_entry;
			}
			paddr = faultaddress + page_entry->pa - page_entry->va;		

			ehi = faultaddress;
			elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
			DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
			tlb_random(ehi, elo);
			return 0;
		}
		tmp_region = tmp_region->next;
	}
	

	/* on demand paging for the stack. We only allocate stack pages if the fault address is within 1 page size.
	Just allocate a physical page and store the fault address in the tlb. Simple it seems. Sould work
	This code segment will only be called when the entry is not present in the page table*/
	if((faultaddress < as->as_stackvbase) && (faultaddress >= (as->as_stackvbase - PAGE_SIZE))) 
	{
		//curthread->t_addrspace->as_stackvbase = curthread->t_addrspace->as_stackvbase - PAGE_SIZE; 		
				
		tmp_page = as->table;
		
		while(tmp_page->next != NULL)
		{
			tmp_page = tmp_page->next;
		}
		
		page_entry = (struct pagetable *)kmalloc(sizeof(struct pagetable));		
		
		if(page_entry == NULL)
		{
			panic("could not allocate kernel memory\n");
		}
		page_entry->va = as->as_stackvbase - PAGE_SIZE;
		page_entry->pa = alloc_upages(1);
		page_entry->swap_status = INMEMORY;
		page_entry->next = NULL;
		tmp_page->next = page_entry;

		as->as_stackvbase = as->as_stackvbase - PAGE_SIZE;

		paddr = faultaddress + page_entry->pa - page_entry->va;		

		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_random(ehi, elo);

 
		return 0;

	}
	/*
	Now we will handle extending the heap. If the current address is in the pages, the code above will handle it itself. But if its not,
	it will coe own to this segment where we will allocate a page according to the requirement
	*/
	if(faultaddress >= as->heap_start && faultaddress <= as->heap_end)
	{
		pg_count = (faultaddress - as->heap_start + as->heap_pages * PAGE_SIZE)/PAGE_SIZE + 1;	
		
		tmp_page = as->table;
		
		while(tmp_page->next != NULL)
		{
			tmp_page = tmp_page->next;
		}

		for(i = 0;i<pg_count;i++)
		{
			page_entry = (struct pagetable *)kmalloc(sizeof(struct pagetable));		
		
			if(page_entry == NULL)
			{
				panic("could not allocate kernel memory\n");
			}
			page_entry->va = as->heap_start + (as->heap_pages + i) * PAGE_SIZE;
			page_entry->pa = alloc_upages(1);
			page_entry->next = NULL;
			page_entry->swap_status = INMEMORY;
			tmp_page->next = page_entry;
			tmp_page = tmp_page->next;

			//as->as_stackvbase = as->as_stackvbase - PAGE_SIZE;
			if((page_entry->va <= faultaddress) && ((page_entry->va + PAGE_SIZE) > faultaddress))
			{
				paddr = faultaddress + page_entry->pa - page_entry->va;		

				ehi = faultaddress;
				elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
				DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
				
			}
		}
		as->heap_pages += pg_count;
		tlb_random(ehi, elo);
		return 0;
	
		
	}
	return EFAULT;
}



