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
#include <kern/seek.h>
#include <stat.h> 

#define MAX_FILENAME_SIZE 32

void initialize_file_table(struct thread *);


int sys__open(char * filename,int flags, mode_t mode, int* ret)
{
	int i;
	char kfilename[MAX_FILENAME_SIZE];
	size_t len;
	int result;
	//Copy filename to kernel space	
	result = copyinstr((const_userptr_t)filename, kfilename, MAX_FILENAME_SIZE, &len);
	if(result != 0)
	{
		*ret = EFAULT;
		return -1;
	}
	//Check if file name provided by user is null or not
	if(kfilename == NULL)
	{
		*ret = EFAULT;
		return -1;
	}	
	//initialize STDIN,STDOUT,STDERR - lazy loading
	if(curthread->t_filetable[0] == NULL)
	{
		initialize_file_table(curthread);
	}
	//create fdesc structure
	struct fdesc * file;
	//allocate memory - should be free if and error occurs
	file = (struct fdesc *)kmalloc(sizeof(struct fdesc));
	//call vfs_open to open a vnode
	result = vfs_open(kfilename,flags,mode,&file->vn);
	if(*ret != 0){ 
		*ret = result;
		return -1;
	}
	file->offset = 0;
	file->ref_count++;
	strcpy(file->name,kfilename);
	file->lk = lock_create(kfilename);
	//assign a file descriptor to the file - structure declared within the thread
	for(i=3;i<256;i++)
	{
		if(curthread->t_filetable[i] == NULL)
		{
			curthread->t_filetable[i] = file;
			*ret = i;
			return 0;
		}
	}
	//means that place in the filetable - too many open files
	*ret = EMFILE;
	return -1;
	
}

int sys__close(int fd, int *ret)
{
	
	if(fd<3 || fd>256)
	{
		*ret = EBADF;
		return -1;
	}

	if(curthread->t_filetable[fd] != NULL)
	{
		curthread->t_filetable[fd]->ref_count--;
		if(curthread->t_filetable[fd]->ref_count == 0)
		{
			vfs_close(curthread->t_filetable[fd]->vn);
			kfree(curthread->t_filetable[fd]);
			
		}
		curthread->t_filetable[fd] = NULL;	
		return 0;
		
	}
	*ret = EBADF;
	return -1;
	
}

int sys__write(int fd,void * buff,size_t nbytes, int *ret)
{
	
	void *kbuff;
	kbuff = kmalloc(nbytes);
	int result;	
	result = copyin((const_userptr_t)buff, kbuff, nbytes);
	if(result !=0)
	{
		kfree(kbuff);
		*ret = result;
		return -1;
	}
	
	if(curthread->t_filetable[0] == NULL)
	{
		initialize_file_table(curthread);
	}	
	struct fdesc * file;
	int err;
	if(fd > 256)
	{
		kfree(kbuff);
		*ret = EBADF;
		return -1;
	}
	if(curthread->t_filetable[fd] == NULL)
	{
		kfree(kbuff);
		*ret = EBADF;
		return -1;
	}
	file = curthread->t_filetable[fd];
	struct iovec iov;
	struct uio u;

	iov.iov_ubase = kbuff;
	iov.iov_len = nbytes;
	u.uio_iov =  &iov;
	u.uio_iovcnt = 1;
	u.uio_offset = file->offset;
	u.uio_resid = nbytes;
	u.uio_segflg = UIO_SYSSPACE;
	u.uio_rw = UIO_WRITE;
	u.uio_space = NULL;
	lock_acquire(file->lk);
	if((err = VOP_WRITE(file->vn,&u)) != 0)
	{
		kfree(kbuff);
		*ret = EBADF;
		return -1;
	}
	kfree(kbuff);
	file->offset = u.uio_offset;
	*ret = nbytes - u.uio_resid;
	lock_release(file->lk);
	return 0;
}

int sys__read(int fd,void * buff,size_t nbytes, int *ret)
{
	void * kbuff;
	int result;
	kbuff = kmalloc(nbytes);
	int err;	
	if(curthread->t_filetable[0] == NULL)
	{
		initialize_file_table(curthread);
	}
	struct fdesc * file;
	
	if(fd > 256)
	{
		kfree(kbuff);
		*ret = EBADF;
		return -1;
	}
	if(curthread->t_filetable[fd] == NULL)
	{
		kfree(kbuff);
		*ret = EBADF;
		return -1;
	}
	
	file = curthread->t_filetable[fd];
	struct iovec iov;
	struct uio u;

	iov.iov_ubase = kbuff;
	iov.iov_len = nbytes;
	u.uio_iov =  &iov;
	u.uio_iovcnt = 1;
	u.uio_offset = file->offset;
	u.uio_resid = nbytes;
	u.uio_segflg = UIO_USERSPACE;
	u.uio_rw = UIO_READ;
	u.uio_space = curthread->t_addrspace;
	lock_acquire(file->lk);

	if((err = VOP_READ(file->vn,&u)) != 0)
	{
		kfree(kbuff);
		*ret = err;	
		return err;
	}
	result = copyout(kbuff,(userptr_t)buff,nbytes);
	if(result != 0)
	{
		kfree(kbuff);
		*ret = result;
		return -1;
	}
	kfree(kbuff);
	file->offset = u.uio_offset;
	*ret = nbytes - u.uio_resid;
	lock_release(file->lk);
	return 0;
}

void initialize_file_table(struct thread * thread)
{
	int result;
	char * console = NULL;
	struct vnode * vn;
	console = kstrdup("con:");
	//strcpy(devname,"con:");
	
	result = vfs_open(console,O_WRONLY,0664,&vn);
	kfree(console);
	if(result)
	{
 		panic("Vfs_open:STDIN to filetable: %s\n",strerror(result));
	}
	
	thread->t_filetable[0] = (struct fdesc *)kmalloc(sizeof(struct fdesc));	
	strcpy(thread->t_filetable[0]->name,"STDIN");
	thread->t_filetable[0]->offset = 0;
	thread->t_filetable[0]->flag = 0;
	thread->t_filetable[0]->ref_count = 0;
	thread->t_filetable[0]->lk = lock_create(thread->t_filetable[0]->name);
	thread->t_filetable[0]->vn = vn;

	//STDOUT
	console = kstrdup("con:");
	result = vfs_open(console,O_RDONLY,0664,&vn);
	if(result)
	{
		panic("Vfs_open:STDOUT to filetable: %s\n",strerror(result));
	}
	
	thread->t_filetable[1] = (struct fdesc *)kmalloc(sizeof(struct fdesc));	
	strcpy(thread->t_filetable[1]->name,"STDOUT");
	thread->t_filetable[1]->offset = 0;
	thread->t_filetable[1]->flag = 0;
	thread->t_filetable[1]->ref_count = 0;
	thread->t_filetable[1]->lk = lock_create(thread->t_filetable[1]->name);
	thread->t_filetable[1]->vn = vn;
	
	//STDERR
	console = kstrdup("con:");
	result = vfs_open(console,O_WRONLY,0664,&vn);
	if(result)
	{
		panic("Vfs_open:STDERR to filetable: %s\n",strerror(result));
	}

	thread->t_filetable[2] = (struct fdesc *)kmalloc(sizeof(struct fdesc));	
	strcpy(thread->t_filetable[2]->name,"STDERR");
	thread->t_filetable[2]->offset = 0;
	thread->t_filetable[2]->flag = 0;
	thread->t_filetable[2]->ref_count = 0;
	thread->t_filetable[2]->lk = lock_create(thread->t_filetable[2]->name);
	thread->t_filetable[2]->vn = vn;

}

//lseek....
off_t sys__lseek(int currfiledesc, off_t offsetPos, int whence, int *returnval){
 
    off_t retoffset;
    int errcode;
 
 
    // current file desc for the current thread shouldnt be null
    if( curthread->t_filetable == NULL){
        *returnval = -1;
        return EBADF;
    }
 
 
    //check for illegal positions of the filehandle not be zero
    // such a file cant exist..coz our FT has max 256;
    if(currfiledesc < 0 || currfiledesc > 256){
        *returnval = -1;
        return EBADF;
    }
 
 
    struct fdesc* curFile = curthread->t_filetable[currfiledesc];
 
    struct stat *chkfilend;
    chkfilend = (struct stat *)kmalloc(sizeof(struct stat));
 
    lock_acquire(curthread->t_filetable[currfiledesc]->lk);
 
    switch(whence)  {
 
        case SEEK_CUR:
            //the new position is pos as per manual definition
            //so is set as the retoffset
            retoffset = offsetPos;
            break;
        case SEEK_SET:
            //the new position is the current position plus pos
            retoffset = curFile->offset + offsetPos;
            break;
        case SEEK_END:
            //the new position is the position of end-of-file plus pos
            //call to VOP_STAT gives the status which is a structure having file info
            // check if info is not available then throw error
            if((errcode = VOP_STAT(curFile->vn,chkfilend))){
                return errcode;
            }
            //accessing the file size in bytes and adding new pos to it
            retoffset = chkfilend->st_size + offsetPos;
            break;
        default:
            //invalid whence
            //took me 3 cups of coffee to figure this out!! release lock here.. stupid!
            lock_release(curthread->t_filetable[currfiledesc]->lk);
            *returnval = -1;
            return EINVAL;
            break;
 
    }
//now we have the final offset to be returned
    if(retoffset < 0){
        lock_release(curthread->t_filetable[currfiledesc]->lk);
        *returnval = -1;
        return EINVAL;
    }
 
// now checking for the offset obtained for returning...
    errcode = VOP_TRYSEEK(curFile->vn,retoffset);
 
    //avoiding access to console like objects
        if(errcode == ESPIPE){
            *returnval = -1;
            return ESPIPE;
        }
 
        curFile->offset = retoffset;
        *returnval =  curFile->offset;
        lock_release(curthread->t_filetable[currfiledesc]->lk);
 
    return 0;
}
 
 
 
// __getCWD
int sys___getcwd(char *buf, size_t buflen, int *returnval){
 
    int errcode;
    // ok we have to use vfs_getcwd
    //so we need a uio..
    struct uio cwdu;
    struct iovec cwdiov;
    char * cwdname = (char *)kmalloc(buflen);
    //struct fdesc curFle = curthread->t_filetable;
 
    // now initializing uio object..
    cwdiov.iov_ubase = cwdname;
    cwdiov.iov_len = buflen-1;
    cwdu.uio_iov = &cwdiov;
    cwdu.uio_iovcnt = 1;
    cwdu.uio_offset = 0;
    cwdu.uio_resid = buflen-1;
    cwdu.uio_rw = UIO_SYSSPACE;
    cwdu.uio_segflg = UIO_READ;
    cwdu.uio_space = NULL;
 
    errcode = vfs_getcwd(&cwdu);
 
    if(errcode){
        return errcode;
    }
 
 
    //Note: this call behaves like read - the name stored in buf is not 0-terminated.
    //we need to add termination to file name it should be buflen -1 ^
 
 
    cwdname[(buflen-1)-cwdu.uio_resid] = 0; //set 0 at the position after the input/opt Remaining amt of data
    size_t actualsize;
    copyoutstr((const char *)cwdname,(userptr_t)buf,buflen,&actualsize);
    kfree(cwdname);
    *returnval = buflen - cwdu.uio_resid;
    return 0;
}
 
//dup2
int sys__dup2(int currfd, int dupfd, int* returnval){
    if(currfd < 0 || dupfd < 0 || currfd > 256 || dupfd >= 256){
        *returnval = -1;
        return EBADF;
    }
    lock_acquire(curthread->t_filetable[currfd]->lk);
    struct fdesc * curFile = curthread->t_filetable[currfd];
    struct fdesc * dupFile = curthread->t_filetable[dupfd];
    if(dupFile != NULL){
       if(sys__close(currfd,returnval))
       return EBADF;
    }
 
        dupFile->flag = curFile->flag;
        dupFile->offset = curFile->offset;
        dupFile->vn = curFile->vn;
        *returnval = dupfd;
    lock_release(curthread->t_filetable[currfd]->lk);
    return 0;
}
 
//chdir
int sys__chdir(char *pathname, int *returnval)
{
    char *name = (char*)kmalloc(sizeof(char)*256);
    size_t len;
    copyinstr((userptr_t)pathname, name, 256, &len);
    int errcode = vfs_chdir(name);
    if(errcode)
    {
        *returnval = -1;
        return errcode;
    }
    *returnval = 0;
    kfree(name);
    return 0;
}
