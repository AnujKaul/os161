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
#include <process.h>
#include <fdesc.h>
#include <kern/fcntl.h>
#include <uio.h>
#include <kern/iovec.h>
#include <copyinout.h>
#include <synch.h>
#include <mips/trapframe.h>
#include <addrspace.h>
#include <kern/wait.h>
#include <spl.h>

void child_fork_start(void *, unsigned long);

void (*child_fork)(void *, unsigned long) = &child_fork_start;

static struct process *process_table[256];


void setParentChildRelation(struct thread * childThread)
{

	process_table[childThread->processId]->parentId = curthread->processId;

}

int process_initialize(void)
{ 
	int i;
	for(i=0;i<256;i++)
	{
		process_table[i] = NULL;
	}
	

	return 0;

}

int process_start(struct thread * thread)
{
	int i;
	int id;
	//int intVal;
	struct process *process;
	process = (struct process *)kmalloc(sizeof(struct process));
		
	if(process == NULL){
		return -1;	
	}
	
	for (i=2;i<256;i++)
	{
		if(process_table[i] == NULL)
		{
			id = i;
			break;
		}
	}
	//intVal = splhigh();	
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
	process_table[id] = process;
	thread->processId = id;
	//splx(intVal);
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
	if(option != 0)
	{
		kprintf("waitpid1\n");
		
		return EINVAL;
	}
	
	if(process_table[pid] == NULL)
	{
		kprintf("waitpid2\n");

		return ESRCH;
	}
	
	if(process_table[pid]->parentId != curthread->processId)
	{
		kprintf("waitpid3\n");

		return EFAULT;
	}
	
	
	lock_acquire(process_table[pid]->lk);
	if(process_table[pid]->exitStatus == 0)
	{
		cv_wait(process_table[pid]->exitcv,process_table[pid]->lk);	
	}
	*ret = pid;
	*status = process_table[pid]->code;
	lock_release(process_table[pid]->lk);
	cv_destroy(process_table[pid]->exitcv);
	lock_destroy(process_table[pid]->lk);
	
	kfree(process_table[pid]);

	return 0;

 }


void child_fork_start(void * data1, unsigned long data2)
{
	
	//kprintf("********************************i am here*********************************\n");
	struct trapframe tf;	
	struct addrspace * child_as = (struct addrspace *)data2;
	struct trapframe * child_tf = (struct trapframe *)data1;
	
	child_tf->tf_a3 = 0;	
	child_tf->tf_v0 = 0;
	
	child_tf->tf_epc += 4;

	curthread->t_addrspace = child_as;	
	as_activate(curthread->t_addrspace);	
	
	tf = *child_tf;	
	mips_usermode(&tf);
	
	
}

int sys__fork(struct trapframe * tf,int * ret)
{
	struct trapframe * child_tf;
	struct addrspace * child_as;	
	int result;

	int intVal;
 	
	child_tf = (struct trapframe *)kmalloc(sizeof(struct trapframe));
	struct thread * child;
	//child_as = (struct addrspace *)malloc(sizeof(struct addrspace));
	
		
	*child_tf = *tf;
	result = as_copy(curthread->t_addrspace,&child_as);
	if(result != 0)
	{
		//kprintf("**********i reached here again***************\n");
		kfree(child_tf);

		return ENOMEM;
	}	
	
	
	intVal = splhigh();	
	result = thread_fork("child_thread",child_fork, (struct trapframe *)child_tf, (unsigned long)child_as,&child);
	if(result !=0)
	{
		splx(intVal);
		kfree(child_tf);

		return ENOMEM;
	}
		
 	
	
	//kprintf("***********%d***********\n",child->processId);
	//kprintf("***********%d***********\n",curthread->processId);
	
	*ret = child->processId;
	//kprintf("%d\n",*ret);
	splx(intVal);

	return 0;
	

}
 

void sys___exit(int exitcode)
{
	int intVal;
	struct process *child, *parent;
	child = process_table[curthread->processId];
	intVal = splhigh();
	if (child->parentId >= 2)
	{
		parent = process_table[child->parentId];
	}
	if (parent == NULL)
	{
		//cv_destroy(child->exitcv);
		//lock_destroy(child->lk);
		kfree(process_table[curthread->processId]);	
	}
	else
	{
		if(parent->exitStatus == 0)
		{
			child->code = _MKWAIT_EXIT(exitcode);
			child->exitStatus = 1;
			cv_signal(child->exitcv,child->lk);
		}
		else
		{
			//cv_destroy(child->exitcv);
			//lock_destroy(child->lk);
			kfree(process_table[curthread->processId]);	

		}
	}
	splx(intVal);
	thread_exit();

}

int sys__execv(const char *program, char **args, int *returnval){

    int i = 0;
    int stringbytes=0;
    size_t actualsize;
    int membytes[256];
    int numofargs = 0;
    int stacksize = 0;
    char ** kbuf;

    char* loadfile;
    int reserror = 0;

    struct vnode *ve;
    vaddr_t enterexecutable;
    vaddr_t stckptr;
    
    /*Get hold of the file name and validate that stupid thing!!!*/
    if(program == NULL){
        return ENOENT;
    }

    loadfile = (char *)kmalloc(256); // limiting file size to 256 characters.
    
    reserror = copyinstr((const_userptr_t)program, loadfile, 256, &actualsize);
	
    if(reserror != 0){

        return (*returnval = reserror);
    }

    if(loadfile == NULL){
        return EINVAL;
    }
    if(strlen(loadfile) == 0){
        return EINVAL;

    }

	
    /*Stop messing with the file name now!*/


    /*Phreak out now ... time to play with the arguments
     *
     *
     */
    if(args != NULL){
	i=0;
	
        while(args[i + 1] != NULL)
        {
                if(((strlen(args[i+1])+1)%4 ) == 0){
                    membytes[i]  =  ((strlen(args[i + 1])+1)/4 ) ;        //gather info for aligned string space
                }
                else{
                    membytes[i]  =  ((strlen(args[i + 1])+1)/4 ) +1 ;
                }

                i++;
		
        }
        numofargs = i;

        
        for(i=0; i<numofargs; i++){
        
                stringbytes += membytes[i];
        }

        stacksize = (stringbytes+numofargs+1);
        


        kbuf = (char **)kmalloc(sizeof(char *) * numofargs);

	for(i = 0;i < numofargs;i++)
        {
    
	    // hoping this sets padded character strings
	    kbuf[i] = (char *)kmalloc(sizeof(char) * membytes[i] * 4);
            reserror = copyinstr((const_userptr_t)args[i + 1], (char *)kbuf[i], (sizeof(char ) * membytes[i] * 4), &actualsize);
            if(reserror !=0)
            {
                kfree(kbuf);
                kfree(loadfile);
                return reserror;
            }
        }
    }

    /*Work with an intermediate user stack we design .... FS i hope this works :-x */

    /*vaddr_t intermediatestack[stacksize];
    if(args != NULL){
        //int intrmdtstckptr = 0;
        //vaddr_t intermediatestack[numofargs + stringbytes];
        int locinstack = numofargs + 1;
        for( i = 0 ; i < numofargs ; i++ ){

            reserror = copyoutstr((const char *)kbuf[i], (userptr_t)intermediatestack[locinstack], (sizeof(char)*membytes[i]*4), &actualsize);
            if(reserror != 0)
            {
                kfree(kbuf);
                kfree(loadfile);
                return reserror;
            }
            intermediatestack[i] = locinstack;
            locinstack = locinstack + membytes[i];
        }
        intermediatestack[numofargs] = (vaddr_t)NULL;
    }
*/
    /*user stack intermediate representation done...*/
    /*OK I hope this works.... update working status <BETA>*/

    /* try and open the program name given to load in execv */
        reserror = vfs_open(loadfile, O_RDONLY,0,&ve);
        if(reserror)
        {
            kfree(loadfile);
            return reserror;
        }

    /*
     * Lets start exploiting dumbVM
     * and its functionalities...
     */
    /*destroy the current address space of the thread*/
        if(curthread->t_addrspace)
        {
            as_destroy(curthread->t_addrspace);
            curthread->t_addrspace = NULL;
        }

        if(curthread->t_addrspace != NULL){
            kprintf("dude destroyer failed!!");
            return -1;
        }


        /* Create new virtual address space */
        curthread->t_addrspace = as_create();
        if(curthread->t_addrspace == NULL)
        {
            vfs_close(ve);
            kfree(loadfile);
            return reserror;
        }

        /* Activate the address space */
        as_activate(curthread->t_addrspace);

        /* Load the ELF file*/
        reserror = load_elf(ve, &enterexecutable);
        if (reserror)
        {
            vfs_close(ve);
            kfree(loadfile);
            return reserror;
        }
        /* file close... */
        vfs_close(ve);

        /* Define the user stack in the address space */
        reserror = as_define_stack(curthread->t_addrspace, &stckptr);
        if (reserror) {
            kfree(loadfile);
            return reserror;
        }

	if(args != NULL){

	    vaddr_t intermediateAddrVal[numofargs + 1];
	    intermediateAddrVal[numofargs] = 0;
            //copying the strings...into the user stack!!
	    for( i = numofargs - 1  ; i >= 0 ; i-- ){
	    	stckptr = stckptr - (sizeof(vaddr_t) * membytes[i]);
		reserror = copyout((char *)kbuf[i], (userptr_t)stckptr, (sizeof(vaddr_t) * membytes[i]));
		if(reserror)
		{
		    kfree(kbuf);	
		    kfree(loadfile);
		    return reserror;
		}		
		intermediateAddrVal[i] = stckptr;
	     }
	    kfree(kbuf);
	    //copying addresses ... into user stack!! 
            for( i = numofargs ; i >= 0 ; i-- ){
	    	stckptr = stckptr - sizeof(vaddr_t);
		reserror = copyout(&intermediateAddrVal[i], (userptr_t)stckptr, sizeof(vaddr_t));
		if(reserror)
		{
		    kfree(loadfile);
		    return reserror;
		}		
	     }

	    /*vaddr_t stckptrold = stckptr;
            for (i = 0 ; i < numofargs ; i++){
               	//memcpy((userptr_t)stckptr,  (vaddr_t *)(stckptrold + (kbuf[i] * sizeof(vaddr_t))), sizeof(vaddr_t)); 
		//stckptr = (vaddr_t)(stckptrold + (intermediatestack[i] * sizeof(vaddr_t)));
		stckptr++;
            }
	    stckptr = stckptrold;

	    */
            kfree(loadfile);
        }


        /* Warp to user mode */
        enter_new_process(numofargs, (userptr_t)stckptr, stckptr, enterexecutable);

        /* enter_new_process does not return. */
        panic("enter_new_process returned\n");
        return EINVAL;

}
