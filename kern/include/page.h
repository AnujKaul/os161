#include <thread.h>
#include <types.h>
#include <synch.h>
#include <addrspace.h> 

enum page_state {CLEAN,DIRTY,FREE,FIXED};

struct page
{
	//Address space of the process who holds the page - for kernel pages should be null
	struct addrspace * as;
	//Virtual address - should be set to 0 for kernel pages
	vaddr_t va;
	//State of the page - would be fixed for all kernel pages
	enum page_state cur_state;
	//timestamp - for swapping later on - no use for kernel pages as they will never be swapped out
	uint64_t timestamp;
	//number of contineous allocations
	int num_pages;

};
