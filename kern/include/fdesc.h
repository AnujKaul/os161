/* File operations header
    - This file will be used to define file table data structure
*/
#define MAX_LENGTH 32

#include <threadlist.h>
#include <synch.h>
#include <vnode.h>
#include <types.h>

struct fdesc{
	char name[MAX_LENGTH];
	off_t offset;
	int flag;
	int ref_count;
	struct lock *lk;
	struct vnode *vn;
};


