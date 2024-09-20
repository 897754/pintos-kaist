#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

//	sleep 상태의 쓰레드를 담을 리스트
struct list sleep_list;

//	recentCpu 는 현재 존재하는 모든 쓰레드들에 대해 재계산 되어야 하므로,
//	그냥 싹다 담을 리스트 필요
struct list allList;

//	유사 비트맵
size_t mlfqBits = 0;

//	멀티 큐를 저장할 배열
struct list arrayOfqueue[64];

//	시스템 부하 평균
int loadAvg;
//	ready 상태의 쓰레드들의 수
int readyThreads;
//	17.14 포맷의 부동 소수점 연산을 위한 F
#define F (1 << 14)
//	정수와 실수 간 변환
#define ItoF(INT) (INT * F)
#define FtoI(FLOAT) ((FLOAT >= 0) ? ((FLOAT + F / 2) / F) : ((FLOAT - F / 2) / F))
//	실수 포함 사칙연산
//	(실수 정수의 나눗셈, 곱셈 / 실수끼리의 덧셈, 뺄셈은 걍 하면 됨)
#define PLUSFnI(x, n) (x + n * F)
#define MINUSInF(x, n) (x - n * F)
#define MULTFnF(x, y) (int)(((int64_t)x) * y / F)
#define DIVIDEFnF(x, y) (int)(((int64_t)x) * F / y)

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4		  /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);
static bool sortByPriority(struct list_elem *a, struct list_elem *b, void *aux);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */

void thread_init(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof(gdt) - 1,
		.address = (uint64_t)gdt};
	lgdt(&gdt_ds);

	/* Init the globla thread context */
	lock_init(&tid_lock);
	list_init(&destruction_req);

	//	자고 있는 쓰레드들을 담을 리스트 생성
	list_init(&sleep_list);

	if (thread_mlfqs)
	{
		//	모든 쓰레드들을 추적할 리스트 생성
		list_init(&allList);
		//	leveled queue 들을 저장할 배열 초기화
		initArray(arrayOfqueue);
		//	부하평균 초기화
		loadAvg = 0;
		//	준비 혹은 실행에 있는 쓰레드 수 초기화
		readyThreads = 0;
	}
	else
	{
		list_init(&ready_list);
	}
	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid();
}

//	준용 추가
//	멀티 큐 초기화 하는 친구
void initArray(struct list *array)
{
	for (int i = 0; i < 64; i++)
	{
		list_init(&array[i]);
	}
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void)
{
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init(&idle_started, 0);

	thread_create("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
//	timer_interrupt 에서 유일하게 호출된다. 즉,
//	외부 인터럽트 (timer) context 에서 실행된다. 즉, 핀토스의 스케쥴링에 있어
//	핵심적인 동작이라는 뜻
void thread_tick(void)
{
	struct thread *t = thread_current();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	//	잠든 쓰레드들이 일어나야 되는 지 체크
	checkSleepingThreads();

	//	매 틱마다 현재 실행중인 쓰레드의 rc를 1씩 높여줘야 함
	if (thread_mlfqs && thread_current() != idle_thread)
	{
		thread_current()->recentCpu = PLUSFnI(thread_current()->recentCpu, 1);
	}
	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
	{
		if (thread_mlfqs)
		{
			caculateAllPriority();
		}
		intr_yield_on_return();
	}
}

//	준용 추가
void caculateLoadAvg(void)
{
	loadAvg = MULTFnF(DIVIDEFnF(ItoF(59), ItoF(60)), loadAvg) + (DIVIDEFnF(ItoF(1), ItoF(60))) * readyThreads;
	return;
}

void caculateAllPriority(void)
{
	thread_current()->priority = caculatePriority(thread_current());
	int i = PRI_MAX;
	while (i >= 0)
	{
		if (((size_t)1 << i) & mlfqBits)
		{
			struct list_elem *node = list_begin(&arrayOfqueue[i]);
			while (node != list_end(&arrayOfqueue[i]))
			{
				struct thread *th = list_entry(node, struct thread, elem);
				th->priority = caculatePriority(th);
				node = node->next;
			}
		}
		i--;
	}
}

int caculatePriority(struct thread *th)
{
	int newPriority = FtoI(ItoF(PRI_MAX) - (th->recentCpu / 4) - ItoF(th->nice * 2));
	if (newPriority > PRI_MAX)
	{
		newPriority = PRI_MAX;
	}
	else if (newPriority < PRI_MIN)
	{
		newPriority = PRI_MIN;
	}
	return newPriority;
}

void caculateRecentCpu(void)
{
	struct list_elem *node = list_begin(&allList);
	while (node != list_end(&allList))
	{
		struct thread *th = list_entry(node, struct thread, allElem);
		int oldRC = th->recentCpu;
		th->recentCpu = PLUSFnI(MULTFnF(DIVIDEFnF(2 * loadAvg, PLUSFnI(2 * loadAvg, 1)), oldRC), th->nice);
		node = node->next;
	}
}

void pushInReadyqueue(struct thread *th)
{
	list_push_back(&arrayOfqueue[th->priority], &th->elem);
	mlfqBits = mlfqBits | ((size_t)1 << th->priority);
}

static bool sortBywakeUp(struct list_elem *a, struct list_elem *b, void *aux)
{
	int wA = list_entry(a, struct thread, elem)->wakeUpTime;
	int wB = list_entry(b, struct thread, elem)->wakeUpTime;

	return wA < wB;
}

void sleepThread(struct thread *th)
{
	enum intr_level old_level = intr_disable();
	list_insert_ordered(&sleep_list, &th->elem, sortBywakeUp, NULL);
	thread_block();
	intr_set_level(old_level);
}

void checkSleepingThreads(void)
{
	enum intr_level old_level = intr_disable();

	int nowTime = timer_ticks();
	while (!list_empty(&sleep_list))
	{
		if (list_entry(list_begin(&sleep_list), struct thread, elem)->wakeUpTime <= nowTime)
		{
			thread_unblock(list_entry(list_pop_front(&sleep_list), struct thread, elem));
		}
		else
		{
			break;
		}
	}
	intr_set_level(old_level);
}

//	여기 까지

/* Prints thread statistics. */
void thread_print_stats(void)
{
	printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
		   idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char *name, int priority,
					thread_func *function, void *aux)
{
	struct thread *t;
	tid_t tid;

	ASSERT(function != NULL);

	/* Allocate thread. */
	t = palloc_get_page(PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread(t, name, priority);
	tid = t->tid = allocate_tid();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t)kernel_thread;
	t->tf.R.rdi = (uint64_t)function;
	t->tf.R.rsi = (uint64_t)aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock(t);
	preempt();

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void)
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);

	//	준용 추가
	if (thread_mlfqs && thread_current() != idle_thread)
	{
		readyThreads--;
	}

	thread_current()->status = THREAD_BLOCKED;
	schedule();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread *t)
{
	enum intr_level old_level;

	ASSERT(is_thread(t));

	old_level = intr_disable();
	ASSERT(t->status == THREAD_BLOCKED);
	if (thread_mlfqs)
	{
		pushInReadyqueue(t);
	}
	else
	{
		list_insert_ordered(&ready_list, &t->elem, sortByPriority, NULL);
	}
	//	준용 추가
	if (thread_mlfqs && t != idle_thread)
	{
		readyThreads++;
	}
	t->status = THREAD_READY;

	intr_set_level(old_level);
}

//	준용 추가
void preempt(void)
{
	if (thread_mlfqs && thread_current() != idle_thread)
	{
		int i = PRI_MAX;
		while (i >= 0)
		{
			if ((size_t)(1 << i) & mlfqBits)
			{
				if (i > thread_current()->priority)
				{
					thread_yield();
				}
				else
				{
					return;
				}
			}
			i--;
		}
	}
	else
	{
		if (thread_current() != idle_thread && !list_empty(&ready_list) && list_entry(list_begin(&ready_list), struct thread, elem)->priority > thread_current()->priority)
		{
			thread_yield();
		}
	}
}

/* Returns the name of the running thread. */
const char *
thread_name(void)
{
	return thread_current()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current(void)
{
	struct thread *t = running_thread();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void)
{
	return thread_current()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void)
{
	ASSERT(!intr_context());
	struct thread *curr = thread_current();

#ifdef USERPROG
	process_exit();
#endif
	//	좀비프로세스 처리
	while (!list_empty(&curr->childs))
	{
		struct thread *ch = list_entry(list_pop_front(&curr->childs), struct thread, pgElem);
		ch->parent = NULL;
		if (ch->status == THREAD_DYING) {
			palloc_free_page(ch);
		}
	}

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable();
	do_schedule(THREAD_DYING);
	NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void)
{
	struct thread *curr = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable();
	if (curr != idle_thread)
	{
		//	current thread 가 idle thread 가 아니라면 (실제 동작을 갖는 쓰레드라면)
		//	ready list 에 push
		if (thread_mlfqs)
		{
			pushInReadyqueue(curr);
		}
		else
		{
			list_insert_ordered(&ready_list, &curr->elem, sortByPriority, NULL);
		}
	}
	do_schedule(THREAD_READY);
	intr_set_level(old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority)
{
	if (!(thread_current()->priority > thread_current()->originalPriority))
	{
		thread_current()->priority = new_priority;
	}
	thread_current()->originalPriority = new_priority;
	preempt();
}

/* Returns the current thread's priority. */
int thread_get_priority(void)
{
	return thread_current()->priority;
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice)
{
	/* TODO: Your implementation goes here */
	//	준용 추가
	thread_current()->nice = nice;
	thread_current()->priority = caculatePriority(thread_current());
	preempt();
	return;
}

/* Returns the current thread's nice value. */
int thread_get_nice(void)
{
	/* TODO: Your implementation goes here */
	//	준용 추가
	return thread_current()->nice;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void)
{
	/* TODO: Your implementation goes here */
	return FtoI(loadAvg * 100);
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void)
{
	/* TODO: Your implementation goes here */
	return FtoI(thread_current()->recentCpu * 100);
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current();
	sema_up(idle_started);

	for (;;)
	{
		/* Let someone else run. */
		intr_disable();
		thread_block();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL);

	intr_enable(); /* The scheduler runs with interrupts off. */
	function(aux); /* Execute the thread function. */
	thread_exit(); /* If function() returns, kill the thread. */
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread(struct thread *t, const char *name, int priority)
{
	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);
	t->priority = priority;
	//	준용 추가, 0번 1번은 std in / out
	t->nextDescriptor = 2;
	//	for wait
	t->wakeUpParent = false;
	t->exitStatus = KERN_EXIT;

	//	너 init 이니?
	if (is_thread(running_thread()))
	{
		//	아니요
		t->parent = thread_current();
		list_push_back(&t->parent->childs, &t->pgElem);
	} else {
		//	네
		t->parent = NULL;
	}
	list_init(&t->childs);
	
	for (int i = t->nextDescriptor; i < FD_MAX; i++)
	{
		t->descriptors[i] = NULL;
	}

	//	준용 추가
	if (!thread_mlfqs)
	{
		t->originalPriority = priority;
		t->lock = NULL;
		list_init(&t->holdLocks);
	}
	else
	{
		//	얘네도
		t->nice = 0;
		t->recentCpu = 0;
		list_push_back(&allList, &t->allElem);
	}
	t->magic = THREAD_MAGIC;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run(void)
{
	if (thread_mlfqs)
	{
		int i = PRI_MAX;
		while (i >= 0)
		{
			if ((((size_t)1 << i) & mlfqBits))
			{
				struct thread *nextTh = list_entry(list_pop_front(&arrayOfqueue[i]), struct thread, elem);
				if (list_empty(&arrayOfqueue[i]))
				{
					mlfqBits = mlfqBits & ~((size_t)1 << i);
				}
				return nextTh;
			}
			i--;
		}
		return idle_thread;
	}
	else
	{
		if (list_empty(&ready_list))
			return idle_thread;
		else
			//	여기서 ready_list 에서 뺌
			return list_entry(list_pop_front(&ready_list), struct thread, elem);
	}
}

/* Use iretq to launch the thread */
void do_iret(struct intr_frame *tf)
{
	__asm __volatile(
		"movq %0, %%rsp\n"
		"movq 0(%%rsp),%%r15\n"
		"movq 8(%%rsp),%%r14\n"
		"movq 16(%%rsp),%%r13\n"
		"movq 24(%%rsp),%%r12\n"
		"movq 32(%%rsp),%%r11\n"
		"movq 40(%%rsp),%%r10\n"
		"movq 48(%%rsp),%%r9\n"
		"movq 56(%%rsp),%%r8\n"
		"movq 64(%%rsp),%%rsi\n"
		"movq 72(%%rsp),%%rdi\n"
		"movq 80(%%rsp),%%rbp\n"
		"movq 88(%%rsp),%%rdx\n"
		"movq 96(%%rsp),%%rcx\n"
		"movq 104(%%rsp),%%rbx\n"
		"movq 112(%%rsp),%%rax\n"
		"addq $120,%%rsp\n"
		"movw 8(%%rsp),%%ds\n"
		"movw (%%rsp),%%es\n"
		"addq $32, %%rsp\n"
		"iretq"
		: : "g"((uint64_t)tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch(struct thread *th)
{
	uint64_t tf_cur = (uint64_t)&running_thread()->tf;
	uint64_t tf = (uint64_t)&th->tf;
	ASSERT(intr_get_level() == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile(
		/* Store registers that will be used. */
		"push %%rax\n"
		"push %%rbx\n"
		"push %%rcx\n"
		/* Fetch input once */
		"movq %0, %%rax\n"
		"movq %1, %%rcx\n"
		"movq %%r15, 0(%%rax)\n"
		"movq %%r14, 8(%%rax)\n"
		"movq %%r13, 16(%%rax)\n"
		"movq %%r12, 24(%%rax)\n"
		"movq %%r11, 32(%%rax)\n"
		"movq %%r10, 40(%%rax)\n"
		"movq %%r9, 48(%%rax)\n"
		"movq %%r8, 56(%%rax)\n"
		"movq %%rsi, 64(%%rax)\n"
		"movq %%rdi, 72(%%rax)\n"
		"movq %%rbp, 80(%%rax)\n"
		"movq %%rdx, 88(%%rax)\n"
		"pop %%rbx\n" // Saved rcx
		"movq %%rbx, 96(%%rax)\n"
		"pop %%rbx\n" // Saved rbx
		"movq %%rbx, 104(%%rax)\n"
		"pop %%rbx\n" // Saved rax
		"movq %%rbx, 112(%%rax)\n"
		"addq $120, %%rax\n"
		"movw %%es, (%%rax)\n"
		"movw %%ds, 8(%%rax)\n"
		"addq $32, %%rax\n"
		"call __next\n" // read the current rip.
		"__next:\n"
		"pop %%rbx\n"
		"addq $(out_iret -  __next), %%rbx\n"
		"movq %%rbx, 0(%%rax)\n" // rip
		"movw %%cs, 8(%%rax)\n"	 // cs
		"pushfq\n"
		"popq %%rbx\n"
		"mov %%rbx, 16(%%rax)\n" // eflags
		"mov %%rsp, 24(%%rax)\n" // rsp
		"movw %%ss, 32(%%rax)\n"
		"mov %%rcx, %%rdi\n"
		"call do_iret\n"
		"out_iret:\n"
		: : "g"(tf_cur), "g"(tf) : "memory");
}

static bool sortByPriority(struct list_elem *a, struct list_elem *b, void *aux)
{
	int priorityA = list_entry(a, struct thread, elem)->priority;
	int priorityB = list_entry(b, struct thread, elem)->priority;

	return priorityA > priorityB;
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status)
{
	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(thread_current()->status == THREAD_RUNNING);
	while (!list_empty(&destruction_req))
	{
		struct thread *victim =
			list_entry(list_pop_front(&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current()->status = status;

	schedule();
}

static void
schedule(void)
{
	struct thread *curr = running_thread();
	struct thread *next = next_thread_to_run();

	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(curr->status != THREAD_RUNNING);
	ASSERT(is_thread(next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate(next);
#endif

	if (curr != next)
	{
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{
			ASSERT(curr != next);
			//	준용 추가
			if (thread_mlfqs && curr != idle_thread)
			{
				readyThreads--;
				list_remove(&curr->allElem);
			}
			// 준용 변경 - 부모가 없으면 그냥 종료 (init thread 거나, 부모가 먼저 종료된 경우)
			if (curr->parent == NULL) {
				list_push_back(&destruction_req, &curr->elem);
			}
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch(next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire(&tid_lock);
	tid = next_tid++;
	lock_release(&tid_lock);

	return tid;
}
