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

/* struct thread의 `magic` 멤버에 대한 무작위 값.
   스택 오버플로우를 감지하는 데 사용됩니다. 자세한 내용은
   thread.h 상단의 큰 주석을 참조하세요. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드용 무작위 값
   이 값을 수정하지 마세요. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태에 있는 프로세스 목록.
   즉, 실행 준비가 되어 있지만 실제로는 실행 중이지 않은 프로세스들. */
static struct list ready_list;
static struct list sleep_list;
static struct list all_list;

/* Idle 스레드. */
static struct thread *idle_thread;

/* 초기 스레드, `init.c:main()`을 실행 중인 스레드. */
static struct thread *initial_thread;

/* `allocate_tid()`에서 사용하는 잠금. */
static struct lock tid_lock;

/* 스레드 파괴 요청들 */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

#define f (1 << 14)  // 2^14, 17.14 고정 소수점에서 사용

/* fixed-point를 정수로 변환 */
#define fp_to_int_round(x) (x >= 0 ? (x + f / 2) / f : (x - f / 2) / f)

/* 정수를 fixed-point로 변환 */
#define int_to_fp(n) (n * f)

/* fixed-point 곱하기 정수 */
#define mul_fp_int(x, n) (x * n)

/* fixed-point 나누기 정수 */
#define div_fp_int(x, n) (x / n)

/* fixed-point 곱하기 fixed-point */
#define mul_fp(x, y) (((int64_t) x) * y / f)

/* fixed-point 나누기 fixed-point */
#define div_fp(x, y) (((int64_t) x) * f / y)

/* fixed-point 더하기 정수*/
#define add_fp_int(x, n) (x + n * f)

/* fixed-point 더하기 fixed-point*/
#define add_fp(x, y) (x + y)

/* fixed-point 빼기 정수*/
#define minus_fp_int(x, n) (x - n * f)

/* fixed-point 빼기 fixed-point*/
#define minus_fp(x, y) (x - y)

int load_avg = 0 * f;
/* false (기본값)인 경우, 라운드로빈 스케줄러를 사용합니다.
   true인 경우, 다단계 피드백 큐 스케줄러(mlfqs)를 사용합니다.
   커널 명령줄 옵션 "-o mlfqs"로 제어됩니다. */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* T가 유효한 스레드를 가리키는 것으로 보일 경우 true를 반환합니다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 현재 실행 중인 스레드를 반환합니다.
 * CPU의 스택 포인터 `rsp`를 읽고, 이를 페이지의 시작으로 내림합니다.
 * `struct thread`는 항상 페이지의 시작에 위치하고 스택 포인터는 페이지의 중간 어딘가에 위치하므로,
 * 이를 통해 현재 스레드를 찾습니다. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// 스레드 시작을 위한 전역 기술자 테이블(Global Descriptor Table, GDT).
// GDT는 `thread_init` 이후에 설정될 것이므로, 먼저 임시 GDT를 설정해야 합니다.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* 현재 실행 중인 코드를 스레드로 변환하여 스레딩 시스템을 초기화합니다.
   일반적으로 이 작업은 불가능하지만, loader.S가 스택의 하단을 페이지 경계에 맞추어 배치했기 때문에
   이 경우에만 가능합니다.

   또한 실행 대기열(run queue)과 tid 잠금을 초기화합니다.

   이 함수를 호출한 후, `thread_create()`를 사용하여 스레드를 생성하기 전에 페이지 할당기를 초기화해야 합니다.

   이 함수가 완료될 때까지 `thread_current()`를 호출하는 것은 안전하지 않습니다. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널을 위한 임시 GDT를 다시 로드합니다.
 	 * 이 GDT는 사용자 컨텍스트를 포함하지 않습니다.
 	 * 커널은 `gdt_init()`에서 사용자 컨텍스트를 포함하는 GDT를 다시 구축할 것입니다. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&sleep_list);
	list_init (&all_list);
	list_init (&destruction_req);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* 인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작합니다.
 * 또한 idle 스레드를 생성합니다. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* 타이머 인터럽트 핸들러에 의해 매 타이머 틱마다 호출됩니다.
 * 따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행됩니다. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* 주어진 초기 PRIORITY를 가진 NAME이라는 이름의 새로운 커널 스레드를 생성하고,
   FUNCTION을 실행하며 AUX를 인수로 전달합니다. 새 스레드는 준비 큐에 추가됩니다.
   새 스레드의 스레드 식별자를 반환하며, 생성에 실패하면 TID_ERROR를 반환합니다.

   `thread_start()`가 호출된 경우, 새 스레드는 `thread_create()`가 반환되기 전에 스케줄링될 수 있습니다.
   심지어 `thread_create()`가 반환되기 전에 종료될 수도 있습니다. 반대로, 원래의 스레드는
   새 스레드가 스케줄링되기 전까지 얼마든지 실행될 수 있습니다. 순서를 보장해야 하는 경우에는
   세마포어나 다른 형태의 동기화를 사용하세요.

   제공된 코드는 새 스레드의 `priority` 멤버를 PRIORITY로 설정하지만, 실제 우선순위 스케줄링은 구현되어 있지 않습니다.
   우선순위 스케줄링은 문제 1-3의 목표입니다. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	#ifdef USERPROG
	//부모자식 관계 세팅
	struct thread* parent = thread_current();
	t->parent = parent;

	if(parent != idle_thread && name != "idle")
	{
		for(int i = 0; i < FDMAXCOUNT; i++)
		{
			if(parent->childTids[i] == -1)
			{
				//printf("[%s]=>[%s] PARENTSET[%d]  %d\n", t->name, parent->name,  i, tid);
				parent->childTids[i] = tid;
				break;
			}
		}
	}
	#endif

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t);

	thread_preempt ();

	return tid;
}

static bool
sleep_ful (const struct list_elem *a_, const struct list_elem *b_,
            void *aux UNUSED) 
{
  const struct thread *a = list_entry (a_, struct thread, elem);
  const struct thread *b = list_entry (b_, struct thread, elem);
  
  return a->wakeup < b->wakeup;
}

void
thread_sleep (int64_t ticks) {
	enum intr_level old_level;
	struct thread *cur;

	old_level = intr_disable ();
	cur = thread_current ();
	ASSERT (cur != idle_thread)
	cur->wakeup = ticks;
	list_insert_ordered (&sleep_list, &cur->elem, sleep_ful, NULL);
	thread_block ();
	intr_set_level (old_level);
}

void
thread_awake (int64_t ticks) {
	while (!list_empty (&sleep_list)) {
		struct list_elem *cur = list_begin(&sleep_list);
		struct thread *cur_thread = list_entry (cur, struct thread, elem);
		if (cur_thread->wakeup == ticks) {
			list_remove (cur);
			thread_unblock (cur_thread);
		}
		else {
			return;
		}
	}
}

/* 현재 스레드를 잠재웁니다. `thread_unblock()`에 의해 깨어날 때까지 다시 스케줄되지 않습니다.
   이 함수는 인터럽트가 꺼진 상태에서 호출해야 합니다.
   보통 `synch.h`의 동기화 원시 동작을 사용하는 것이 더 나은 방법입니다. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

static bool
priority_ful (const struct list_elem *a_, const struct list_elem *b_,
            void *aux UNUSED) 
{
  const struct thread *a = list_entry (a_, struct thread, elem);
  const struct thread *b = list_entry (b_, struct thread, elem);
  
  return a->priority > b->priority;
}

/* 차단된 스레드 T를 실행 준비 상태로 전환합니다.
   T가 차단 상태가 아닌 경우, 이는 오류입니다.
   (실행 중인 스레드를 준비 상태로 만들려면 `thread_yield()`를 사용하세요.)

   이 함수는 실행 중인 스레드를 선점하지 않습니다. 이는 중요할 수 있습니다: 호출자가 직접 인터럽트를 비활성화했을 경우,
   스레드를 원자적으로 차단 해제하고 다른 데이터를 업데이트할 수 있을 것으로 기대할 수 있습니다. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	list_insert_ordered(&ready_list, &t->elem, priority_ful, NULL);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* 현재 실행 중인 스레드를 반환합니다.
   이는 running_thread()에 몇 가지 기본적인 검사를 추가한 것입니다.
   자세한 내용은 thread.h 파일 상단의 큰 주석을 참조하세요. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();

	#ifdef USERPROG
	struct thread* cur = thread_current();
	struct thread* parent = cur->parent;
	if(parent != NULL && cur != idle_thread && parent != idle_thread)	
	{
		for(int i = 0; i < FDMAXCOUNT; i++)
		{
			if(parent->childTids[i] == cur->tid)
			{
				parent->childTids[i] = -1;
				break;
			}
		}
	}
	#endif

	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU를 양보합니다. 현재 스레드는 잠자기 상태가 아니며,
   스케줄러의 결정에 따라 즉시 다시 예약될 수 있습니다. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered(&ready_list, &curr->elem, priority_ful, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

void
thread_preempt (void) {
	struct thread *curr = thread_current ();

	if (!list_empty(&ready_list) && list_entry(list_begin(&ready_list), struct thread, elem)->priority > curr->priority) {
		if(intr_context()) {
			intr_yield_on_return();
		}
		else {
			thread_yield ();
		}
	}
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {
	struct thread *curr = thread_current ();
	struct list *list = &curr->lock_list;
	struct list_elem *e = list_begin(list);

	// lock_list가 비어있다면 curr->priority를 new_priority로 바꿔준다.
	if (list_empty(&curr->lock_list)) {
		curr->priority = new_priority;
	}
	// 그게 아니라면 lock_list 탐색
	else {
		for (;;) {
			struct lock *l = list_entry (e, struct lock, lock_elem);
			// lock의 old_priority중 new_priority보다 작은건 -1로 초기화
			// new_priority보다 커지는 순간 old_priority = new_priority로 초기화
			if (l->old_priority > new_priority) {
				l->old_priority = new_priority;
				break;
			}
			l->old_priority = -1;
			// 끝까지 탐색했다면 탈출
			if (e->next == list_end(list)) {
				curr->priority = new_priority;
				break;
			}
			e = e->next;
		}
	}
	thread_preempt ();
}

void
calculate_priority (void) {
	struct list_elem *e;
	for (e = list_begin(&all_list); e != list_end(&all_list); e = e->next) {
		struct thread *t = list_entry (e, struct thread, allelem);
		if (t == idle_thread) {
			continue;
		}
		int new_priority = PRI_MAX - fp_to_int_round(t->recent_cpu / 4) - (t->nice * 2);
		if (new_priority > PRI_MAX) {
			new_priority = PRI_MAX;
		}
		else if (new_priority < PRI_MIN) {
			new_priority = PRI_MIN;
		}
		t->priority = new_priority;	
	}
}

void
calculate_tick_recent_cpu (struct thread *t) {
	if (t == idle_thread) {
		return;
	}
	t->recent_cpu = add_fp_int(t->recent_cpu, 1);
}

void
calculate_recent_cpu_load_avg (void) {
	int ready_threads = list_size(&ready_list);
	// thread_current가 idle_thread가 아니라면 실행중인 스레드까지 카운트
	if (thread_current () != idle_thread) {
		ready_threads = ready_threads + 1;
	}
	// load_avg = (59/60) * load_avg + (1/60) * ready_threads;
	load_avg = add_fp(mul_fp(div_fp_int(int_to_fp(59),60), load_avg),div_fp_int(int_to_fp(ready_threads),60));

	struct list_elem *e;
	for (e = list_begin(&all_list); e != list_end(&all_list); e = e->next) {
		struct thread *t = list_entry (e, struct thread, allelem);
		if (t != idle_thread) {
			// t->recent_cpu = (2 * load_avg)/(2 * load_avg + 1) * t->recent_cpu + t->nice;
			t->recent_cpu = add_fp_int(mul_fp(div_fp(mul_fp_int(load_avg, 2), add_fp_int(mul_fp_int(load_avg, 2), 1)), t->recent_cpu), t->nice);
		}
	}
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	enum intr_level old_level;
	old_level = intr_disable ();
	struct thread *t = thread_current ();
	t->nice = nice;
	t->priority = PRI_MAX - fp_to_int_round(t->recent_cpu / 4) - (nice * 2);
	thread_preempt ();
	intr_set_level (old_level);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	enum intr_level old_level;
	old_level = intr_disable ();
	int temp = thread_current ()->nice;
	intr_set_level (old_level);
	return temp;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	enum intr_level old_level;
	old_level = intr_disable ();
	int temp = fp_to_int_round(mul_fp_int(load_avg, 100));
	intr_set_level (old_level);
	return temp;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	enum intr_level old_level;
	old_level = intr_disable ();
	int temp = fp_to_int_round(mul_fp_int(thread_current ()->recent_cpu, 100));
	intr_set_level (old_level);
	return temp;
}

/* idle 스레드. 다른 스레드가 실행 준비가 되어 있지 않을 때 실행됩니다.

   idle 스레드는 처음에 `thread_start()`에 의해 준비 리스트에 추가됩니다.
   초기에는 한 번 스케줄링되며, 이 시점에서 `idle_thread`를 초기화하고, `thread_start()`가 계속 진행할 수 있도록
   전달된 세마포어를 "up"한 후 즉시 차단됩니다. 그 이후로, idle 스레드는 준비 리스트에 나타나지 않습니다.
   준비 리스트가 비어 있을 때 `next_thread_to_run()`에서 특별한 경우로 반환됩니다. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();
		
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
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	if (name == "main") {
		t->nice = 0;
		t->recent_cpu = 0 * f;
	}
	else {
		struct thread *curr = thread_current ();
		t->nice = curr->nice;
		t->recent_cpu = curr->recent_cpu;
	}
	list_push_back(&all_list, &t->allelem);
	list_init (&t->lock_list);
	
	// project2 added.
#ifdef USERPROG
	t->childrenExitStatus = -1;
	t->parent = NULL;
	t->waitingThread = -1;
	for(int i = 0; i < FDMAXCOUNT; i++)
	{
		//printf("[%s]child 배열 초기화...\n", t->name);
		t->childTids[i] = -1;
	}
	// // 표준 입출력 fd 등록
	t->fds[0] = NULL; // STDIN_FILENO;
	t->fds[1] = NULL; // STDOUT_FILENO;
	// fd2 : STDERR_FILENO 이지만 구현은 안되어있음.
	t->fds[2] = NULL;
	for(int i = 3; i < FDMAXCOUNT; i++)
	{
		t->fds[i] = NULL;
	}


	t->thread_exit_status = 0;
	t->is_user = false;
	t->denied_file = NULL;
#endif


	t->waiting_lock = NULL;
	t->magic = THREAD_MAGIC;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
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
			: : "g" ((uint64_t) tf) : "memory");
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
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
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
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		list_remove(&victim->allelem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

bool thread_has_child(tid_t tid)
{
	struct thread* curThread = thread_current();
	
	for(int i = 0; i < FDMAXCOUNT; i++)
	{
		//printf("[%s] thread_has_child[%d],  %d:%d\n", curThread->name, i, curThread->childTids[i],tid);
		if(curThread->childTids[i] == tid)
			return true;
	}

	return false;
}

bool thread_check_destroy(tid_t tid)
{
	struct list_elem* cur = list_begin(&destruction_req);

	while (cur != list_end(&destruction_req))
	{
		if(list_entry(cur, struct thread, elem)->tid == tid)	return true;
		cur = list_next(cur);
	}

	return false;
}


struct thread* get_thread_by_tid(tid_t tid)
{
	struct list_elem* cur = list_begin(&all_list);
	//printf("get_thread_by_tid\n");
	while (cur != list_end(&all_list))
	{
		struct thread* curThread = list_entry(cur, struct thread, allelem);
		//printf("Looking for Thread.. %d:%d\n", curThread->tid, tid);
		if(curThread->tid == tid)
		{
			//printf("Thread Found! %d\n", curThread->tid);
			return curThread;
		}	
		cur = list_next(cur);
	}

	return NULL;
}