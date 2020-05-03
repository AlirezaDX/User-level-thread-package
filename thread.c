#include <assert.h>
#include <stdlib.h>
#include <ucontext.h>
#include "thread.h"
#include "interrupt.h"
#include <stdbool.h>

//#define MY_DEBUG_FLAG
#ifdef MY_DEBUG_FLAG 
#endif /* MY_DEBUG_FLAG */

/* This is the FIFO queue structure */
struct wait_queue {
	Tid threads[THREAD_MAX_THREADS]; //Pay attention 1 extra space means tail != head ever
	int headIdx; //Where to read from next
	int tailIdx; //Where to store in next
};

/* Push the tid in FIFO order
 * Will not check for not NULL
 * THREAD_INVALID:    the queue is full technically impossible */
void
WQ_push(struct wait_queue *queue, Tid in){
	queue->threads[queue->tailIdx] = in;
	queue->tailIdx++;
	queue->tailIdx = queue->tailIdx % THREAD_MAX_THREADS;
	
	#ifdef MY_DEBUG_FLAG
		if(queue->headIdx == queue->tailIdx)
		{
			fprintf(stderr, "Error1 WQ_push.\n");
			exit(0);
		}
	#endif /* MY_DEBUG_FLAG */

	return;
}


/* Pops the tid in FIFO order
 * Will not check for not NULL
 * THREAD_NONE:    no more threads */
Tid
WQ_pop(struct wait_queue *queue){

	if(queue->headIdx == queue->tailIdx )
		return THREAD_NONE;
	// Else
	int ans = queue->threads[queue->headIdx];
	queue->headIdx++;
	queue->headIdx = queue->headIdx % THREAD_MAX_THREADS;
	
	return ans;
}

/* This is the thread control block */
typedef struct thread {

	int status;
	Tid nextInQueue;
	bool isKilled;

	ucontext_t myContext;
	volatile int isContextSet;
	void* stackAllocPointer;
	
	//Wait queue I belong to
	struct wait_queue * myWaitQueue; //Default NULL
	//Wait queue of threads waiting on my exit
	struct wait_queue * exitWaitQueue; //Default NULL
} myThread;
/* ContextSet status */
enum 
{
	//CS_UK = 0,
	CS_NOTSET = 1,
	CS_SET = 2,
};
/* Thread status */
enum 
{
	T_ACTIVE = 0,
	T_READY = 1,
	T_SLEEP = 2,
	T_DEAD = 3,
	T_UNUSED = -1
};

/* Linked list structure */
typedef struct toFreeQueue{
	void* toFreePtr;
	struct toFreeQueue* next;
} ToFreeQueue;

/* Active and running queue holding all the threads */
typedef struct thread_box{
	Tid runningHeadId;
	Tid runningTailId;
	myThread threads[THREAD_MAX_THREADS];
	
	ToFreeQueue* tFQ;
} GlobalThreadStruct;

GlobalThreadStruct _tBox;

/* Creates and adds to the end of the TFQ */
void
GTS_TFQ_push(void* pointer){
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
/* Frees all the items in the linked list
*  Including the pointer keys they hold */
void
GTS_TFQ_free(){
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

/* Reset the values of a thread stored in GTS */
void
GTS_threadReset(Tid tid){
	_tBox.threads[tid].status = T_UNUSED;
	_tBox.threads[tid].nextInQueue = -1;
	_tBox.threads[tid].isKilled = false;

	_tBox.threads[tid].isContextSet = CS_SET;
	_tBox.threads[tid].stackAllocPointer = NULL;

	_tBox.threads[tid].myWaitQueue = NULL;
	_tBox.threads[tid].exitWaitQueue = NULL;
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

/* Returns previous head ID
*  Caution: There must be more than 2 threads in active Queue to call this
*  Caution: The previous head will be (untouched and) lost if not captured from return
*  Caution: want_tid must be in the active queue already (error1)
 */
Tid 
GTS_replaceHead(Tid want_tid)
{
	Tid prevHead = _tBox.runningHeadId;
	_tBox.runningHeadId = _tBox.threads[_tBox.runningHeadId].nextInQueue;//Throw out the original head
	/* If the new head is what we wanted to replace with */
	if(_tBox.runningHeadId == want_tid){
		_tBox.threads[want_tid].status = T_ACTIVE;
		return prevHead;
	}
	
	#ifdef MY_DEBUG_FLAG
		/* The thread requested to replace the head is not in
		*  in the chain of our active threads */
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
		/* The thread requested to replace the head is not in
		*  in the chain of our active threads */
		if(!foundInTheLoop)
		{
			fprintf(stderr, "Error1 in GTS_replaceHead.\n");
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
thread_init(void)
{
	int enabled = interrupts_set(0);
    /* critical section */
	int i;
	for(i=0; i<THREAD_MAX_THREADS; i++){
		GTS_threadReset(i);
	}

	_tBox.threads[0].status = T_ACTIVE;
	_tBox.runningHeadId = 0;
	_tBox.runningTailId = 0;
	_tBox.tFQ = NULL;
	/* end critical section */
	interrupts_set(enabled);
}

Tid
thread_id()
{
	return _tBox.runningHeadId;
}

void
thread_stub(void (*thread_main)(void *), void *arg)
{
	/* When the thread was created the interupts were disabled */
	interrupts_set(1); 
	thread_main(arg); // Call thread_main() function with arg
	thread_exit();
}


Tid
thread_create(void (*fn) (void *), void *parg)
{
	int enabled = interrupts_set(0);
    /* critical section */
    
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

	/* end critical section */
	interrupts_set(enabled);
	return id;

	return THREAD_FAILED;
}

/* Must at this point tid be the head in the queue*/
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
	/* If we are coming back to our context */
	GTS_TFQ_free();
}

static Tid
thread_yield_helper(Tid want_tid)
{

	if( want_tid == _tBox.runningHeadId )
		return want_tid;

	if( want_tid >= THREAD_MAX_THREADS )
		return THREAD_INVALID;
	
	if( want_tid < 0 ){
		
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
				
			return thread_yield_helper(_tBox.threads[_tBox.runningHeadId].nextInQueue);
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
		Tid prevHead = GTS_replaceHead(want_tid);
		GTS_pushActive(prevHead);
		
		//Now lets change the thread:
		changeContext(prevHead, want_tid);

		return want_tid;
	}
	else // want_id was not ready
		return THREAD_INVALID;
}

Tid
thread_yield(Tid want_tid){
	int enabled = interrupts_set(0);
	/* critical section */
	int ans = thread_yield_helper(want_tid);
	/* end critical section */
	interrupts_set(enabled);
	return ans;
}

void
thread_exit()
{
	/*int enabled = */interrupts_set(0);
    /* critical section */
	Tid prevHead = _tBox.runningHeadId;
	/* Wake up any thread which is waiting*/
	if( _tBox.threads[prevHead].exitWaitQueue != NULL ){
		thread_wakeup/*_helper*/(_tBox.threads[prevHead].exitWaitQueue, 1);
		wait_queue_destroy/*_helper*/(_tBox.threads[prevHead].exitWaitQueue);
	}
	/* If no threads are available in the ready queue
	* Must be done after waking up all the threads waiting my exit*/
	if(_tBox.runningHeadId == _tBox.runningTailId){
		exit(0);
	}

	#ifdef MY_DEBUG_FLAG
		/* Just checked and head and tail were not equal */
		if(_tBox.threads[_tBox.runningHeadId].nextInQueue == -1){
			fprintf(stderr, "Error2 in Exit.\n");
			exit(0);
		}
	#endif /* MY_DEBUG_FLAG */
	

	prevHead = GTS_replaceHead(_tBox.threads[_tBox.runningHeadId].nextInQueue);
	GTS_TFQ_push(_tBox.threads[prevHead].stackAllocPointer);
	/* Must be done in the end*/
	GTS_threadReset(prevHead);

	/* end critical section */
	//interrupts_set(enabled); // Let interupts be off this thread is exited
	setContextTo(_tBox.runningHeadId);
}

Tid
thread_kill(Tid tid)
{
	int enabled = interrupts_set(0);
    /* critical section */
	if(tid<0 || _tBox.runningHeadId == tid || tid>=THREAD_MAX_THREADS || _tBox.threads[tid].status == T_UNUSED){
		return THREAD_INVALID;
	}

	_tBox.threads[tid].isKilled = true;
	/* end critical section */
	interrupts_set(enabled);
	return tid;
}

/******************************************************************/
/******************************************************************/
struct wait_queue *
wait_queue_create()
{
	int enabled = interrupts_set(0);
	
	struct wait_queue *wq;
	wq = malloc(sizeof(struct wait_queue));
	assert(wq);

	wq->headIdx = 0;
	wq->tailIdx = 0;
	
	interrupts_set(enabled);
	return wq;
}

inline static bool
wait_queue_isEmpty(struct wait_queue *wq){
	return wq->headIdx == wq->tailIdx;
}

void
wait_queue_destroy(struct wait_queue *wq)
{
	int enabled = interrupts_set(0);
	
	#ifdef MY_DEBUG_FLAG
		/* Wait queue is not empty and we are deleting it. 
		*  The threads in it will be forever lost. */
		if(!wait_queue_isEmpty(wq))
		{
			fprintf(stderr, "Error1 in wait Q destroy.\n");
			exit(0);
		}
	#endif /* MY_DEBUG_FLAG */

	free(wq);
	interrupts_set(enabled);
}

static Tid
thread_sleep_helper(struct wait_queue *queue)
{
	if(queue == NULL){
		return THREAD_INVALID;
	}
	
	Tid nextThread = _tBox.threads[_tBox.runningHeadId].nextInQueue;
	if(nextThread == -1){
		return THREAD_NONE;
	}

	Tid prevHead = GTS_replaceHead(nextThread);
	
	_tBox.threads[prevHead].status = T_SLEEP;
	_tBox.threads[prevHead].nextInQueue = -1;
	_tBox.threads[prevHead].myWaitQueue = queue;
	WQ_push(queue, prevHead);

	//Now lets change the thread:
	changeContext(prevHead, nextThread);

	return nextThread;
}

Tid
thread_sleep(struct wait_queue *queue)
{
	int enabled = interrupts_set(0);
	/* critical section */
	int ans = thread_sleep_helper(queue);
	/* end critical section */
	interrupts_set(enabled);
	return ans;
}

/* when the 'all' parameter is 1, wakeup all threads waiting in the queue.
 * returns whether a thread was woken up on not. */
static int
thread_wakeup_helper(struct wait_queue *queue, int all)
{
	if(queue == NULL){
		return 0;
	}

	int i = 0;
	Tid popped = WQ_pop(queue);
	if( popped < 0 ) //THREAD_NONE
		return i;//0
	{
		GTS_pushActive(popped);
		_tBox.threads[popped].myWaitQueue = NULL;
	}
	i++;

	while(all){
		popped = WQ_pop(queue);
		if( popped < 0 ) //THREAD_NONE
			break;
		{
			GTS_pushActive(popped);
			_tBox.threads[popped].myWaitQueue = NULL;
		}
		i++;
	}
	return i;
}

int
thread_wakeup(struct wait_queue *queue, int all)
{
	int enabled = interrupts_set(0);
	/*critical section */
	int ans = thread_wakeup_helper(queue, all);
	/* end critical section */
	interrupts_set(enabled);
	return ans;
}

static Tid
thread_wait_helper(Tid tid)
{
	if(_tBox.runningHeadId == tid || tid < 0 || tid >= THREAD_MAX_THREADS || _tBox.threads[tid].status == T_UNUSED)
		return THREAD_INVALID;
	
	/* Check if the waiting queue has been allocated */
	struct wait_queue * dest = _tBox.threads[tid].exitWaitQueue;
	if(dest == NULL){
		_tBox.threads[tid].exitWaitQueue = wait_queue_create();
		dest = _tBox.threads[tid].exitWaitQueue;
	}
	
	Tid ret = thread_sleep_helper(dest);
	/* If that thread is sleeping and there is no other active thread to run!!! */
	if( 0 > ret){
		fprintf(stderr, "Error1 thread_wait_helper.\n");
		exit(0);
	}
	
	return tid;
}

/* suspend current thread until Thread tid exits */
Tid
thread_wait(Tid tid)
{
	int enabled = interrupts_set(0);
	Tid ans = thread_wait_helper(tid);
	interrupts_set(enabled);
	return ans;
}

struct lock {
	struct wait_queue * lkWaitQueue; // Default not NULL
	Tid lockHolder; // Default: -1 = none
};

struct lock *
lock_create()
{
	struct lock *lock;
	lock = malloc(sizeof(struct lock));
	assert(lock);

	lock->lkWaitQueue = wait_queue_create();
	lock->lockHolder = -1; 

	return lock;
}

void
lock_destroy(struct lock *lock)
{
	int enabled = interrupts_set(0);
	
	assert(lock != NULL);

	wait_queue_destroy(lock->lkWaitQueue);
	free(lock);

	interrupts_set(enabled);
}

void
lock_acquire(struct lock *lock)
{
	int enabled = interrupts_set(0);
	/* If no one is currently holding the lock */
	if(lock->lockHolder == -1){
		lock->lockHolder = _tBox.runningHeadId;
	}
	else{
		#ifdef MY_DEBUG_FLAG
			Tid ret = 
		#endif /* MY_DEBUG_FLAG */
		thread_sleep_helper(lock->lkWaitQueue);

		#ifdef MY_DEBUG_FLAG
		 	/* No other thread is active. Shouldn't happen */
			if( 0 > ret){
				fprintf(stderr, "Error1 lock_acquire.\n");
				exit(0);
			}
		#endif /* MY_DEBUG_FLAG */
			
		/* Take hold of the lock */
		lock->lockHolder = _tBox.runningHeadId;
	}

	interrupts_set(enabled);
}

void
lock_release(struct lock *lock)
{
	int enabled = interrupts_set(0);

	assert(lock != NULL);
	
	int count = thread_wakeup_helper(lock->lkWaitQueue, 0);
	if(count == 1){
		/* We don't do anything. the lock holder should not become -1
		*  because some random thread will acquire the lock if they come in
		*  before the one woken up gets to run */
	}
	else{ //count == 0
		#ifdef MY_DEBUG_FLAG
			/* Wake up must succeed and return either 1 or 0 */
			if( count != 0 ){
				fprintf(stderr, "Error1 lock_release.\n");
				exit(0);
			}
		#endif /* MY_DEBUG_FLAG */
		lock->lockHolder = -1;
	}
	
	interrupts_set(enabled);
}

struct cv {
	struct wait_queue * cvWaitQueue;
};

struct cv *
cv_create()
{
	int enabled = interrupts_set(0);
	
	struct cv *cv;
	cv = malloc(sizeof(struct cv));
	assert(cv);

	cv->cvWaitQueue = wait_queue_create();

	interrupts_set(enabled);
	return cv;
}

void
cv_destroy(struct cv *cv)
{
	int enabled = interrupts_set(0);
	assert(cv != NULL);
	
	wait_queue_destroy(cv->cvWaitQueue);
	free(cv);

	interrupts_set(enabled);
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
	int enabled = interrupts_set(0);
	assert(cv != NULL);
	assert(lock != NULL);
	
	#ifdef MY_DEBUG_FLAG
		/* The thread runnung is not the one who owns the lock!!! */
		if(lock->lockHolder != _tBox.runningHeadId){
			fprintf(stderr, "Error1 in cv_wait.\n");
			exit(0);
		}
	#endif /* MY_DEBUG_FLAG */

	lock_release(lock);

	#ifdef MY_DEBUG_FLAG
		Tid ret = 
	#endif /* MY_DEBUG_FLAG */
	thread_sleep_helper(cv->cvWaitQueue);

	#ifdef MY_DEBUG_FLAG
		/* Bad use of function. No other thread is active! No ops */
		if( 0 > ret){
			fprintf(stderr, "Warning 1 cv_wait: No ops.\n");
			// exit(0);
		}	
	#endif /* MY_DEBUG_FLAG */
	
	/* Now that we have awaken */
	lock_acquire(lock);

	interrupts_set(enabled);
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
	int enabled = interrupts_set(0);
	assert(cv != NULL);
	assert(lock != NULL);

	thread_wakeup_helper(cv->cvWaitQueue, 0);

	interrupts_set(enabled);
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	int enabled = interrupts_set(0);
	assert(cv != NULL);
	assert(lock != NULL);

	thread_wakeup_helper(cv->cvWaitQueue, 1);

	interrupts_set(enabled);
}
