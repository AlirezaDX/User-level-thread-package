#include <assert.h>
#include <stdlib.h>
#include <ucontext.h>
#include "thread.h"
#include "interrupt.h"
#include <stdbool.h>

//#define A_MEGABYTE  1048576

#define MY_DEBUG_FLAG
#ifdef MY_DEBUG_FLAG 
#endif /* MY_DEBUG_FLAG */

/* This is the wait queue structure */
struct wait_queue {
	
};

/* This is the thread control block */
typedef struct thread {

	int status;
	Tid nextInQueue;

	ucontext_t myContext;
	volatile int isContextSet;
	void* stackAllocPointer;

	bool isKilled;
} myThread;

enum //ContextSet status
{
	//CS_UK = 0,
	CS_NOTSET = 1,
	CS_SET = 2,
};
enum //thread status
{
	T_ACTIVE = 0,
	T_READY = 1,
	T_SLEEP = 2,
	T_DEAD = 3,
	T_UNUSED = -1
};

typedef struct toFreeQueue{
	void* toFreePtr;
	struct toFreeQueue* next;
} ToFreeQueue;

typedef struct thread_box{
	Tid runningHeadId;
	Tid runningTailId;
	myThread threads[THREAD_MAX_THREADS];
	
	ToFreeQueue* tFQ;
} GlobalThreadStruct;

GlobalThreadStruct _tBox;

void
TFQ_push(void* pointer){
	/* If TFQ is empty */
	if(_tBox.tFQ == NULL){
		_tBox.tFQ = (ToFreeQueue*) malloc(sizeof(ToFreeQueue));
		assert(_tBox.tFQ);
		_tBox.tFQ->toFreePtr = pointer;
		_tBox.tFQ->next = NULL;
		return;
	}
	/* Else */
	ToFreeQueue* curr = _tBox.tFQ;
	/* Find the last item and put it in curr */
	while(curr->next !=NULL){
		curr = curr->next;
	}

	/* Add the pointer to the end */
	curr->next = (ToFreeQueue*) malloc(sizeof(ToFreeQueue));
	assert(curr->next);
	curr->next->toFreePtr = pointer;
	curr->next->next = NULL;
}
void
TFQ_free(){
	if(_tBox.tFQ == NULL)
		return;
	ToFreeQueue* prev = _tBox.tFQ;
	ToFreeQueue* curr = _tBox.tFQ;
	free(curr->toFreePtr);
	while(curr->next !=NULL){
		curr = curr->next;
		free(curr->toFreePtr);
		free(prev);
		prev = curr;
	}
	free(curr);
	_tBox.tFQ = NULL;
}

static inline myThread* GTS_actHeadThread(){
	return &_tBox.threads[_tBox.runningHeadId];
}

/* A function that adds newTid to tail
*  Caution: _tBox.threads[newTid].nextInQueue = -1; */
void
GTS_pushActive(Tid newTail){
	_tBox.threads[_tBox.runningTailId].nextInQueue = newTail;
	_tBox.runningTailId = newTail;
	_tBox.threads[newTail].nextInQueue = -1;
	_tBox.threads[newTail].status = T_READY;
}


void
thread_init(void)
{
	int i;
	for(i=0; i<THREAD_MAX_THREADS; i++){

		_tBox.threads[i].status = T_UNUSED;
		_tBox.threads[i].nextInQueue = -1;
		_tBox.threads[i].isContextSet = CS_SET;
		_tBox.threads[i].stackAllocPointer = NULL;
		_tBox.threads[i].isKilled = false;
	}

	_tBox.threads[0].status = T_ACTIVE;
	_tBox.runningHeadId = 0;
	_tBox.runningTailId = 0;
	_tBox.tFQ = NULL;
}

Tid
thread_id()
{
	return _tBox.runningHeadId;
}

void
thread_stub(void (*thread_main)(void *), void *arg)
{

	thread_main(arg); // call thread_main() function with arg
	thread_exit();
}


Tid
thread_create(void (*fn) (void *), void *parg)
{
	/* Finding an empty spot for the new thread */
	int id;
	for(id=0; id<THREAD_MAX_THREADS; id++){
		if( _tBox.threads[id].status == T_UNUSED )
			break;
	}
	
	/* If it never broke (No space for next thread) */
	if(id == THREAD_MAX_THREADS)
		return THREAD_NOMORE;
	
	_tBox.threads[id].stackAllocPointer = malloc(THREAD_MIN_STACK);
	if(_tBox.threads[id].stackAllocPointer == NULL)
	{
		return THREAD_NOMEMORY;
	}

	/* Place stack pointer on top of the stack */
	void* newSP = _tBox.threads[id].stackAllocPointer + THREAD_MIN_STACK;

	/* The BP which will be pushed when the thread_stub function is called will be pushed to sp-8 which must be alligned to 16 bytes */
	newSP = newSP - (long)(newSP - 8)%16 ;

	int err;
	err = getcontext(&_tBox.threads[id].myContext);
	assert(!err);
	_tBox.threads[id].myContext.uc_mcontext.gregs[REG_RIP] = (long)&thread_stub;
	_tBox.threads[id].myContext.uc_mcontext.gregs[REG_RDI] = (long)fn;
	_tBox.threads[id].myContext.uc_mcontext.gregs[REG_RSI] = (long)parg;
	_tBox.threads[id].myContext.uc_mcontext.gregs[REG_RSP] = (long)newSP;

	//_tBox.threads[id].status = T_READY;
	GTS_pushActive(id);
	return id;

	return THREAD_FAILED;
}

/* Returns previous head ID
*  Caution: There must be more than 2 threads in active Queue to call this
*  Caution: The previous head will be (untouched and) lost if not captured from return
*  Caution: want_tid must be in the active queue already (error1)
 */
Tid 
replaceHead(Tid want_tid)
{
	Tid prevHead = _tBox.runningHeadId;
	_tBox.runningHeadId = _tBox.threads[_tBox.runningHeadId].nextInQueue;//Throw out the original head
	/* If the new head is what we wanted to replace with */
	if(_tBox.runningHeadId == want_tid){
		_tBox.threads[want_tid].status = T_ACTIVE;
		return prevHead;
	}
	
	#ifdef MY_DEBUG_FLAG
		bool foundInTheLoop = false;
	#endif /* MY_DEBUG_FLAG */

	Tid curr = _tBox.runningHeadId;
	/* Go through the ready queue to find the thread before want_tid */
	while(_tBox.threads[curr].nextInQueue != -1){
		if(_tBox.threads[curr].nextInQueue == want_tid)
		{
			/* Since the want_tid is going to be going to head it's previous item should point to it's next item */
			_tBox.threads[curr].nextInQueue = _tBox.threads[want_tid].nextInQueue;
			/* If want_tid was the tail therefore the one before it is the new tail */
			if(want_tid == _tBox.runningTailId){
				_tBox.runningTailId = curr;
			}

			#ifdef MY_DEBUG_FLAG
				foundInTheLoop = true;
			#endif /* MY_DEBUG_FLAG */

			break;
		}
		curr = _tBox.threads[curr].nextInQueue;
	}
	
	#ifdef MY_DEBUG_FLAG
		if(!foundInTheLoop)
		{
			fprintf(stderr, "Error1 in replaceHead.\n");
			exit(0);
		}
	#endif /* MY_DEBUG_FLAG */

	/* Now that we fixed how the removal of want_id affects the queue we put it at the head */
	_tBox.threads[want_tid].nextInQueue = _tBox.runningHeadId;
	_tBox.threads[want_tid].status = T_ACTIVE;
	_tBox.runningHeadId = want_tid;
	
	return prevHead;
}

void
setContextTo(Tid tid){
	if(_tBox.threads[tid].isKilled)
	{
		_tBox.threads[tid].myContext.uc_mcontext.gregs[REG_RIP] = (long)&thread_exit;
	}
	/* Else */
	int err = setcontext(&_tBox.threads[tid].myContext);
	assert(!err);
}

void
changeContext(Tid from, Tid to){
	int err;
	_tBox.threads[from].isContextSet = CS_NOTSET;
	err = getcontext(&_tBox.threads[from].myContext);
	assert(!err);
	
	/* If it's the first time we are coming here */
	if(_tBox.threads[from].isContextSet == CS_NOTSET)
	{
		_tBox.threads[from].isContextSet = CS_SET;
		setContextTo(to);
	}
}

Tid
thread_yield(Tid want_tid)
{

	TFQ_free();

	if(want_tid == _tBox.runningHeadId)
		return want_tid;

	if( want_tid >= THREAD_MAX_THREADS )
		return THREAD_INVALID;
	
	if(want_tid < 0){
		
		if( want_tid == THREAD_ANY ){
			if(_tBox.runningHeadId == _tBox.runningTailId)
			{
				return THREAD_NONE;
			}
			
			#ifdef MY_DEBUG_FLAG
				/* Now that head and tail are not the same then next in queue must not be -1  */
				if(_tBox.threads[_tBox.runningHeadId].nextInQueue == -1)
				{
					fprintf(stderr, "Error1 in Yield.\n");
					exit(0);
				}
			#endif /* MY_DEBUG_FLAG */
				
			return thread_yield(_tBox.threads[_tBox.runningHeadId].nextInQueue);
		}
		
		if( want_tid == THREAD_SELF ){
			#ifdef MY_DEBUG_FLAG 
				changeContext(_tBox.runningHeadId, _tBox.runningHeadId);
			#endif /* MY_DEBUG_FLAG */
			return _tBox.runningHeadId;
		}
		
		return THREAD_INVALID;
	}	
	//Else
	if( _tBox.threads[want_tid].status == T_READY )
	{
		#ifdef MY_DEBUG_FLAG
		/* want_tid wasn't head and it was ready but the ready queue has only the head in it */
			if(_tBox.runningHeadId == _tBox.runningTailId)
			{
				fprintf(stderr, "Error2 in Yield.\n");
				exit(0);
			}
		#endif /* MY_DEBUG_FLAG */

		/* Caution: Replace before push. 
		*  The functions can change the fields in the inputted thread. */
		Tid prevHead = replaceHead(want_tid);
		GTS_pushActive(prevHead);
		
		//Now lets change the thread:
		changeContext(prevHead, want_tid);

		return want_tid;
	}
	else // want_id was not ready
		return THREAD_INVALID;

}

void
thread_exit()
{
	if(_tBox.runningHeadId == _tBox.runningTailId){
		exit(0);
	}

	if(_tBox.threads[_tBox.runningHeadId].nextInQueue == -1){
		fprintf(stderr, "Error2 in Exit.\n");
		exit(0);
	}

	Tid prevHead = replaceHead(_tBox.threads[_tBox.runningHeadId].nextInQueue);
	
	_tBox.threads[prevHead].status = T_UNUSED;
	_tBox.threads[prevHead].nextInQueue = -1;
	_tBox.threads[prevHead].isContextSet = CS_SET;
	TFQ_push(_tBox.threads[prevHead].stackAllocPointer);
	_tBox.threads[prevHead].stackAllocPointer = NULL;/////////////
	_tBox.threads[prevHead].isKilled = false;

	setContextTo(_tBox.runningHeadId);
}

Tid
thread_kill(Tid tid)
{
	if(tid<0 || _tBox.runningHeadId == tid || tid>=THREAD_MAX_THREADS || _tBox.threads[tid].status == T_UNUSED){
		return THREAD_INVALID;
	}

	_tBox.threads[tid].isKilled = true;
	return tid;
}

/******************************************************************/

/* make sure to fill the wait_queue structure defined above */
struct wait_queue *
wait_queue_create()
{
	struct wait_queue *wq;

	wq = malloc(sizeof(struct wait_queue));
	assert(wq);

	TBD();

	return wq;
}

void
wait_queue_destroy(struct wait_queue *wq)
{
	TBD();
	free(wq);
}

Tid
thread_sleep(struct wait_queue *queue)
{
	TBD();
	return THREAD_FAILED;
}

/* when the 'all' parameter is 1, wakeup all threads waiting in the queue.
 * returns whether a thread was woken up on not. */
int
thread_wakeup(struct wait_queue *queue, int all)
{
	TBD();
	return 0;
}

/* suspend current thread until Thread tid exits */
Tid
thread_wait(Tid tid)
{
	TBD();
	return 0;
}

struct lock {
	/* ... Fill this in ... */
};

struct lock *
lock_create()
{
	struct lock *lock;

	lock = malloc(sizeof(struct lock));
	assert(lock);

	TBD();

	return lock;
}

void
lock_destroy(struct lock *lock)
{
	assert(lock != NULL);

	TBD();

	free(lock);
}

void
lock_acquire(struct lock *lock)
{
	assert(lock != NULL);

	TBD();
}

void
lock_release(struct lock *lock)
{
	assert(lock != NULL);

	TBD();
}

struct cv {
	/* ... Fill this in ... */
};

struct cv *
cv_create()
{
	struct cv *cv;

	cv = malloc(sizeof(struct cv));
	assert(cv);

	TBD();

	return cv;
}

void
cv_destroy(struct cv *cv)
{
	assert(cv != NULL);

	TBD();

	free(cv);
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
	assert(cv != NULL);
	assert(lock != NULL);

	TBD();
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
	assert(cv != NULL);
	assert(lock != NULL);

	TBD();
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	assert(cv != NULL);
	assert(lock != NULL);

	TBD();
}
