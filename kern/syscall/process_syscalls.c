#include <kern/unistd.h> 
#include <kern/errno.h>
#include <types.h>
#include <lib.h>
#include <array.h>
#include <vfs.h>
#include <vnode.h>
#include <syscall.h>
#include <thread.h>
#include <current.h>
#include <fdesc.h>
#include <kern/fcntl.h>
#include <uio.h>
#include <kern/iovec.h>
#include <copyinout.h>
#include <synch.h>
#include <process.h>


static struct process *process_table[MAX_PROCESSES];

int process_start(struct thread * thread)
{
	int i;
	int id;
	struct process *process;
	process = kmalloc(sizeof(*process));
	if(process == NULL){
		return -1;	
	}

	for (i=1;i<256;i++)
	{
		if(process_table[i] == NULL)
		{
			id = i;
			break;
		}
	}
	process->parentId = 0;
	process->code = 0;
	process->exitStatus=0;
	process->lk = lock_create("exitlock");
	if(process->lk == NULL)
	{	
		kfree(process);
		return -1;
	}	
	process->exitcv = cv_create("exitcv");
	if(process->exitcv == NULL)
	{
		kfree(process);
		return -1;
	}
	
		
	process->myThread = thread;
	process_table[i] = process;
	thread->processId = i;		
	return 0;

}


int sys__getpid(int * ret)
{
	if(curthread->processId != 0)
	{
		*ret = curthread->processId;
		return 0;
	}
	
	return -1;
}

int sys__waitpid(pid_t pid, int * status, int option, int * ret)
{
	//int result;
	//int returnVal;
	int i;
	//void kstatus;
	
	//result = copyin((const_userptr_t)status),kstatus,4);
	//if(result != 0)
	//{
	//	*ret = EFAULT;
	//	return -1;
	//}
	if(option == 0)
	{
		*ret = ESRCH;
		return -1;
	}
	
	for(i=1;i<256;i++)
	{
		if(process_table[pid] == NULL)
		{
			*ret = ESRCH;
			return -1;
		}
	}

	if(process_table[pid]->parentId != curthread->processId)
	{
		*ret = EFAULT;
		return -1;
	}
	
	
	lock_acquire(process_table[pid]->lk);
	if(process_table[pid]->exitStatus == 0)
	{
		cv_wait(process_table[pid]->exitcv,process_table[pid]->lk);	
	}
	*ret = pid;
	*status = process_table[pid]->exitStatus;
		
	cv_destroy(process_table[pid]->exitcv);
	lock_destroy(process_table[pid]->lk);
	
	kfree(process_table[pid]);
	
	return 0;

}
