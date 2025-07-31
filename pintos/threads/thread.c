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
#include "filesys/file.h"
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
static struct list sleep_list; // 인터럽트된 스레드 보관할 리스트
static struct list all_list;	// 모든 쓰레드를 추적할 리스트

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

int load_avg;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);
void thread_sleep (int64_t ticks);
void thread_wakeup (int64_t ticks);
bool thread_compare_wakeup_time (struct list_elem *a, struct list_elem *b, void *aux UNUSED);
bool thread_compare_priority (struct list_elem *a, struct list_elem *b, void *aux UNUSED);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

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
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&all_list);   // 모든 쓰레드 리스트 초기화
	list_init (&sleep_list); // 리스트 초기화
	list_init (&destruction_req);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	list_push_back (&all_list, &initial_thread->all_elem);
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
	load_avg = INT_TO_FIXED(0);
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
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

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
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
		intr_yield_on_return ();		// 인터럽트 복귀시 스케줄링 실행
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
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

	struct thread *cur = thread_current();
	t->parent = cur;
	list_push_back(&cur->child_list, &t->child_elem);

	t->fdt = palloc_get_multiple(PAL_ZERO, 3);
	if (t->fdt == NULL)
		return TID_ERROR;

	t->fd_idx = 2;

	t->fdt[0] = STDIN_; 				// stdin 초기화
	t->fdt[1] = STDOUT_;				// stdout 초기화

	t->stdin_count = 1;
	t->stdout_count = 1;

	t->exit_status = 0;
	t->waited = false;

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
	if (thread_current()->priority > t->priority){
		// 큐에 추가만
		thread_unblock (t);
	}
	else {
		// 새로운 스레드 우선순위가 높다면 실행
		// 큐에 추가 후 현재 스레드를 준비 상태로
		thread_unblock (t);
		thread_yield();
	}

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	// list_push_back (&ready_list, &t->elem);
	list_insert_ordered(&ready_list, &t->elem, thread_compare_priority, NULL); // 우선 순위를 기준으로 삽입되도록 변경
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
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
	struct thread *cur = thread_current();
	list_remove(&cur->all_elem);
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
// 현재 스레드를 대기 큐에 넣어주는 함수
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		// list_push_back (&ready_list, &curr->elem);
		list_insert_ordered(&ready_list, &curr->elem, thread_compare_priority, NULL); // 우선 순위를 기준으로 삽입되도록 변경
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

// thread를 대기 상태로 만들어 주는 함수
void
thread_sleep (int64_t ticks) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	old_level = intr_disable ();
	curr->wakeup_time = timer_ticks() + ticks; 	// 일어날 시간

	list_insert_ordered (&sleep_list, &curr->elem, thread_compare_wakeup_time, NULL);

	thread_block();								// Block
	intr_set_level(old_level); 					// 인터럽트 복원
}

// 현재 스레드를 깨우는 함수
void
thread_wakeup (int64_t ticks) {
	while(!list_empty(&sleep_list)){
		// list_entry (리스트 next ptr, offsetof(struct, member.next), 타입)
		struct thread *t = list_entry(list_front(&sleep_list), struct thread, elem);
		if (t->wakeup_time > ticks)
            break;

        list_pop_front(&sleep_list);
		if(thread_mlfqs){
			mlfqs_calc_priority(t);
		}
        thread_unblock(t);
	}
}

bool
thread_compare_wakeup_time (struct list_elem *a, struct list_elem *b, void *aux UNUSED) {
	struct thread *thread_a = list_entry(a, struct thread, elem);
	struct thread *thread_b = list_entry(b, struct thread, elem);
	return thread_a->wakeup_time < thread_b->wakeup_time;
}

bool
thread_compare_priority (struct list_elem *a, struct list_elem *b, void *aux UNUSED) {
	struct thread *thread_a = list_entry(a, struct thread, elem);
	struct thread *thread_b = list_entry(b, struct thread, elem);
	return thread_a->priority > thread_b->priority;
}

bool
thread_compare_donate_priority (struct list_elem *a, struct list_elem *b, void *aux UNUSED)
{
	struct thread *thread_a = list_entry(a, struct thread, donation_elem);
	struct thread *thread_b = list_entry(b, struct thread, donation_elem);
	return thread_a->priority > thread_b->priority;
}

/*
동작과정 정리
Pintos 기본적으로 RR방식이며 이걸 priority기반으로 동작함
현재 쓰레드가 락을 생성, 작업 중 타임을 모두 소진하면 다시 준비 큐로 되돌아감
그 뒤에 새로운 쓰레드가 실행상태로 진입하게 되며 락 획득을 요청하게 됨
만약 이때 처리하고 싶은 영역에 대해 락을 다른 쓰레드가 가지고 있다면
자신보다 우선순위가 낮은 상태 따라서 낮은 우선순위와 락을 가지고 있는 쓰레드를 먼저 처리하기위해
우선순위를 기부할 필요가 생김

아래 donate 코드는 현재 생성된 쓰레드가 락을 요청하면서 실행됨
즉, donation리스트 기준으로 가장 뒤에 삽입된 쓰레드
현재 쓰레드가 락을 가진 애들을 보면서 앞으로 진행하면 최종적으로 처음 락을 가진 쓰레드를 만나고
우선순위를 기부하면서 종료됨
*/

// 락 획득 요청시 실행
void donate(){
	struct thread *cur = thread_current();
	struct thread *holder = cur->wait_on_lock->holder;

	// holder들을 보면서 우선순위 기부
	while (holder != NULL && holder->priority < cur->priority) {
        holder->priority = cur->priority;
        if (holder->wait_on_lock == NULL) break;
        holder = holder->wait_on_lock->holder;
    }
}

void recalc_priority(){
	struct thread *cur = thread_current();

	cur->priority = cur->original_priority;

	if (!list_empty (&cur->donations)) {
    list_sort (&cur->donations, thread_compare_donate_priority, 0);

    struct thread *prev = list_entry (list_front (&cur->donations), struct thread, donation_elem);
    if (prev->priority > cur->priority)
		cur->priority = prev->priority;
	}
}


void
remove_lock(struct lock *lock){
	/*
	현재 쓰레드를 종료하면 우선순위와 증여자 목록 삭제
	*/
	struct list_elem *e, *next;
	struct thread *cur = thread_current();
	
    for (e = list_begin(&cur->donations); e != list_end(&cur->donations); e = list_next(e)){
		struct thread *t = list_entry(e, struct thread, donation_elem);
		if (t->wait_on_lock == lock){
			list_remove(&t->donation_elem);
    }
}
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {
	// 우선순위가 바뀌면 더 높은 우선순위의 스레드가 실행되도록 한다.
	struct thread *cur = thread_current ();
	
	if (thread_mlfqs) return;

	cur->original_priority = new_priority;

		// 우선순위 재계산
		recalc_priority();

		// 준비 큐가 비어있지 않다면
		if (!list_empty(&ready_list)){
			int fisrt_priority = list_entry(list_front (&ready_list), struct thread, elem)->priority; // 준비중인 스레드중 가장 높은 우선순위 가져옴

			if (fisrt_priority > new_priority){
				thread_yield();
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
thread_set_nice (int nice) {
	/* TODO: Your implementation goes here */
	enum intr_level old_level = intr_disable ();
	thread_current ()->nice = nice;
	mlfqs_calc_priority (thread_current ());
	// thread_yield();
	thread_check_preemption();
	intr_set_level (old_level);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	enum intr_level old_level = intr_disable ();
	int nice = thread_current ()-> nice;
	intr_set_level (old_level);
	return nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	enum intr_level old_level = intr_disable ();
	int load_avg_value = FIXED_TO_INT_ROUND (FIXED_MUL_INT (load_avg, 100));
	intr_set_level (old_level);
	return load_avg_value;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	enum intr_level old_level = intr_disable ();
	int recent_cpu= FIXED_TO_INT_ROUND (FIXED_MUL_INT (thread_current ()->recent_cpu, 100));
	intr_set_level (old_level);
	return recent_cpu;
}


void
mlfqs_calc_recent_cpu(struct thread *t){
	if (t == idle_thread) return ;
    t->recent_cpu = FIXED_ADD_INT (FIXED_MUL (FIXED_DIV (FIXED_MUL_INT (load_avg, 2), FIXED_ADD_INT (FIXED_MUL_INT (load_avg, 2), 1)), t->recent_cpu), t->nice);
}

void
mlfqs_calc_load_avg(){
	int ready_threads = list_size(&ready_list);
    if (thread_current() != idle_thread) ready_threads++;
	load_avg = FIXED_ADD (
        FIXED_MUL (FIXED_DIV (INT_TO_FIXED (59), INT_TO_FIXED (60)), load_avg),
        FIXED_MUL_INT (FIXED_DIV (INT_TO_FIXED (1), INT_TO_FIXED (60)), ready_threads)
    );
}

void
mlfqs_calc_priority(struct thread *t){
	if (t == idle_thread) return ;

	int priority = PRI_MAX - FIXED_TO_INT(FIXED_DIV_INT(t->recent_cpu, 4)) - (t->nice * 2);
	
	// 범위 제한
	if (priority > PRI_MAX) priority = PRI_MAX;
    if (priority < PRI_MIN) priority = PRI_MIN;
	
	t->priority = priority;
}

void
mlfqs_increase_recent_cpu(){
	struct thread *cur = thread_current();
	if (cur != idle_thread) cur->recent_cpu += F;
}

void
mlfqs_recalc_recent_cpu(){
	struct list_elem *e;
	// ready_list의 모든 스레드
    for (e = list_begin(&ready_list); e != list_end(&ready_list); e = list_next(e)) {
        struct thread *t = list_entry(e, struct thread, elem);
        mlfqs_calc_recent_cpu(t);
    }
    
    // sleep_list의 모든 스레드
    for (e = list_begin(&sleep_list); e != list_end(&sleep_list); e = list_next(e)) {
        struct thread *t = list_entry(e, struct thread, elem);
        mlfqs_calc_recent_cpu(t);
    }
    
    // 현재 실행 중인 스레드
    struct thread *cur = thread_current();
    mlfqs_calc_recent_cpu(cur);
}

void
mlfqs_recalc_priority(){
		struct list_elem *e;
	// ready_list의 모든 스레드
    for (e = list_begin(&ready_list); e != list_end(&ready_list); e = list_next(e)) {
        struct thread *t = list_entry(e, struct thread, elem);
        mlfqs_calc_priority(t);
    }
    
    // 현재 실행 중인 스레드
    struct thread *cur = thread_current();
    mlfqs_calc_priority(cur);

	list_sort(&ready_list, thread_compare_priority, NULL);
	intr_yield_on_return();
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
	t->original_priority = priority; 	// 기존 우선순위 저장용(불변)
	t->magic = THREAD_MAGIC;
	list_init(&t->donations); 			// 리스트 초기화
	t->wait_on_lock = NULL; 			// 초기화, 대기중인 락 없음
	t->nice = 0;
	t->recent_cpu = 0;
	if (thread_mlfqs){
		mlfqs_calc_priority(t);
	}

	t->parent = NULL;					// 현재 부모는 NULL
	list_init(&t->child_list);			// 자식 리스트 초기화

	sema_init(&t->wait_sema, 0);		// 부모 대기
	sema_init(&t->exit_sema, 0);		// 자식 대기
	sema_init(&t->fork_sema, 0);


	list_push_back(&all_list, &t->all_elem);
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
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf; 	// 현재 실행중인 스레드의 프레임을 가져온다.
	uint64_t tf = (uint64_t) &th->tf;						// 받아온 스레드 프레임을 가져온다.
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


void 
thread_check_preemption (void)
{
    if (!intr_context() && // 인터럽트 컨텍스트 확인 추가
        !list_empty (&ready_list) &&
        thread_current ()->priority <
        list_entry (list_front (&ready_list), struct thread, elem)->priority)
        thread_yield ();
}

/* 모든 리스트를 돌면서 자식 프로세스를 찾는 함수 */
struct thread *
get_thread_tid(tid_t tid){
	struct list_elem *e;
	struct thread *found = NULL;

	enum intr_level old_level = intr_disable(); // 인터럽트를 비활성화해서 일관성을 보장

	for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)){
		struct thread *t = list_entry(e, struct thread, all_elem);
		if (t->tid == tid){
			found = t;
			break;
		}
	}
	intr_set_level(old_level);
	return found;
}