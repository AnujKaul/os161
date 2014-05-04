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
/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

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
	}
	lock_release(lk_core_map);
	panic("could not allocate a page.....!\n");
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
		free_upages(cur_page->pa);
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
