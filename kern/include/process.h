#include <thread.h>
#include <types.h>
#include <synch.h>

#define MAX_PROCESSES 256

struct process
{
	pid_t parentId;
	struct thread *myThread;	
	struct lock* lk;
	struct cv* exitcv;
	int code;
	bool exitStatus; 
};

int process_initialize(void);
int process_start(struct thread *);
void setParentChildRelation(struct thread *);



 
