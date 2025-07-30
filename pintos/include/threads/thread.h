#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "synch.h"
#ifdef VM
#include "vm/vm.h"
#endif

#define MAX_FD 1 << 9
#define STDIN_  1
#define STDOUT_ 2


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* nice */
#define NICE_MIN -20
#define NICE_DEFAULT 0
#define NICE_MAX 20

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */
	int original_priority;
	int64_t wakeup_time;				// 일어날 시간 추가
	struct list donations;				// 도네이션 리스트
	struct list_elem donation_elem;
	struct lock *wait_on_lock;			// 대기중인 락
	int nice;
	int recent_cpu;

	struct thread *parent;				// 부모 쓰레드 포인터
	struct list child_list;				// 자식 쓰레드 리스트
	struct list_elem child_elem;		// 자식 쓰레드 관리를 위한 list_elem

    /* 프로세스 종료 동기화를 위한 세마포어 */
    struct semaphore wait_sema;      	// 부모가 자식 종료를 기다릴 때 사용 (초기값: 0)
    struct semaphore exit_sema;      	// 자식의 완전한 종료를 보장하기 위해 사용 (초기값: 0)
	struct semaphore fork_sema;

	struct intr_frame parent_if;

	struct file **fdt;
	int fd_idx;

	struct file *running;
	
	int stdin_count;
	int stdout_count;

	int exit_status;					// 프로세스 종료 상태 (비정상 -1)
	bool waited;						// 이미 wait()이 호출되었는지 확인

	struct list_elem all_elem;

	/* Shared between thread.c and synch.c. */
	struct list_elem elem;              /* List element. */

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

bool thread_compare_priority (struct list_elem *a, struct list_elem *b, void *aux UNUSED);
bool thread_compare_donate_priority (struct list_elem *a, struct list_elem *b, void *aux UNUSED);
void donate();
void recalc_priority();
void remove_lock(struct lock *lock);
void thread_check_preemption();
struct thread * get_thread_tid(tid_t tid);


// 매크로 함수 정의
/*
소수점 앞에 17비트, 소수점 뒤에 14비트, 부호 비트가 1개가 있기 때문에 이를 17.14 고정 소수점 숫자 표현이라고 함.
17.14 형식의 숫자는 최대 (2^31 - 1)/(2^14) ≈ 131,071.999의 값을 나타냄
*/
#define FIXED_POINT_SHIFT 14
#define F (1 << FIXED_POINT_SHIFT)

/* Convert n to fixed point */
#define INT_TO_FIXED(n) ((n) * (F))

/* Convert x to integer (rounding toward zero) */
#define FIXED_TO_INT(x) ((x) / (F))

/* Convert x to integer (rounding to nearest) */
#define FIXED_TO_INT_ROUND(x) ((x) >= 0 ? ((x) + (F) / 2) / (F) : ((x) - (F) / 2) / (F))

/* Add x and y */
#define FIXED_ADD(x, y) ((x) + (y))

/* Subtract y from x */
#define FIXED_SUB(x, y) ((x) - (y))

/* Add x and n */
#define FIXED_ADD_INT(x, n) ((x) + (n) * (F))

/* Subtract n from x */
#define FIXED_SUB_INT(x, n) ((x) - (n) * (F))

/* Multiply x by y */
#define FIXED_MUL(x, y) (((int64_t)(x)) * (y) / (F))

/* Multiply x by n */
#define FIXED_MUL_INT(x, n) ((x) * (n))

/* Divide x by y */
#define FIXED_DIV(x, y) (((int64_t)(x)) * (F) / (y))

/* Divide x by n */
#define FIXED_DIV_INT(x, n) ((x) / (n))


#endif /* threads/thread.h */

/*
1. 고정 소수점으로 변환
정수 3을 고정 소수점으로 변환하기: 3 * 16384 = 49152
실수 2.5을 고정 소수점으로 변환하기: 2.5 * 16384 = 40960

2. 고정 소수점 덧셈
1.5와 2.25를 더하기:
1.5를 고정 소수점으로 변환: 1.5 * 16384 = 24576
2.25를 고정 소수점으로 변환: 2.25 * 16384 = 36864
덧셈: 24576 + 36864 = 61440
고정 소수점 결과를 실수로 변환: 61440 / 16384 = 3.75

3. 고정 소수점 곱셈
1.5와 2.0을 곱하기:
1.5를 고정 소수점으로 변환: 24576
2.0을 고정 소수점으로 변환: 2.0 * 16384 = 32768
곱셈: ((int64_t) 24576) * 32768 / 16384 = 49152
고정 소수점 결과를 실수로 변환: 49152 / 16384 = 3.0

4. 고정 소수점 나눗셈
3.0을 2.0으로 나누기:
3.0을 고정 소수점으로 변환: 3.0 * 16384 = 49152
2.0을 고정 소수점으로 변환: 32768
나눗셈: ((int64_t) 49152) * 16384 / 32768 = 24576
고정 소수점 결과를 실수로 변환: 24576 / 16384 = 1.5
*/