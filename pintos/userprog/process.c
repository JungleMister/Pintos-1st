#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(void *);

/* General process initializer for initd and other process. */
static void
process_init(void)
{
	struct thread *current = thread_current();
	list_init(&current->fd_list);
	current->next_fd = 2;
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */

struct wrap
{
	struct thread *thread;
	char *file_name;
};

tid_t process_create_initd(const char *file_name)
{
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page(0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy(fn_copy, file_name, PGSIZE);

	char *save_ptr;
	char *parsed_name = strtok_r(fn_copy, " ", &save_ptr);
	strlcpy(fn_copy, file_name, PGSIZE);

	/* Create a new thread to execute FILE_NAME. */
	struct wrap w = {.file_name = fn_copy, .thread = thread_current()};
	tid = thread_create(parsed_name, PRI_DEFAULT, initd, &w);
	sema_down(&thread_current()->fork_sema);
	if (tid == TID_ERROR)
		palloc_free_page(fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd(void *wrap)
{
#ifdef VM
	supplemental_page_table_init(&thread_current()->spt);
#endif
	process_init();
	struct wrap *w = (struct wrap *)wrap;
	struct thread *parent = w->thread;
	char *f_name = w->file_name;

	char *temp;
	char *save_ptr;
	strlcpy(thread_current()->name, f_name, sizeof thread_current()->name);
	temp = palloc_get_page(0);
	if (temp != NULL)
	{
		strlcpy(temp, f_name, PGSIZE);
		char *exec_name = strtok_r(temp, " ", &save_ptr);
		if (exec_name != NULL)
			strlcpy(thread_current()->name, exec_name, sizeof thread_current()->name);
		palloc_free_page(temp);
	}

	thread_current()->parent = parent;
	list_push_back(&parent->child_list, &thread_current()->child_elem);
	sema_up(&parent->fork_sema);

	if (process_exec(f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED();
}

/* 인자 전달용 구조체 */
struct fork_args
{
	struct intr_frame if_; // 레지스터 정보
	struct thread *parent; // 부모 스레드
};

static struct thread *get_child_by_tid(tid_t tid)
{
	struct list *child_list = &thread_current()->child_list;
	struct list_elem *e;

	for (e = list_begin(child_list); e != list_end(child_list); e = list_next(e))
	{
		struct thread *child = list_entry(e, struct thread, child_elem);
		if (child->tid == tid)
			return child;
	}
	return NULL;
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t process_fork(const char *name, struct intr_frame *if_)
{
	struct fork_args *args = palloc_get_page(PAL_ZERO);
	if (args == NULL)
		return TID_ERROR;

	// 부모 레지스터 상태 복사
	memcpy(&args->if_, if_, sizeof(struct intr_frame));
	args->parent = thread_current();

	// 자식 생성
	tid_t tid = thread_create(name, PRI_DEFAULT, __do_fork, args);
	if (tid == TID_ERROR)
	{
		palloc_free_page(args);
		return TID_ERROR;
	}
	sema_down(&thread_current()->fork_sema); // 자식이 초기화 완료할 때까지 대기

	// 자식이 초기화 완료할 때까지 대기
	struct thread *child = get_child_by_tid(tid);
	if (child == NULL || !child->fork_success)
	{
		palloc_free_page(args);
		return TID_ERROR;
	}
	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte(uint64_t *pte, void *va, void *aux)
{
	struct thread *current = thread_current();
	struct thread *parent = (struct thread *)aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kernel_vaddr(va))
		return true;
	/* 2. Resolve VA from the parent's page map level 4. */
	// parent_page = pml4_get_page(parent->pml4, va);
	parent_page = pml4_get_page(parent->pml4, va);
	if (parent_page == NULL)
		return true;

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (newpage == NULL)
		return false;
	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = (*pte & PTE_W) != 0;

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page(current->pml4, va, newpage, writable))
	{
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork(void *aux)
{
	struct fork_args *args = (struct fork_args *)aux;
	struct intr_frame if_ = args->if_; // 부모의 레지스터 상태 복사
	struct thread *parent = args->parent;
	struct thread *current = thread_current();

	bool success = false; // 최종 성공 여부

	// 부모 설정
	current->parent = parent;

	// 주소 공간 복사
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto cleanup;

	process_activate(current);

#ifdef VM
	supplemental_page_table_init(&current->spt);
	if (!supplemental_page_table_copy(&current->spt, &parent->spt))
		goto cleanup;
#else
	if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
		goto cleanup;
#endif

	// 파일 디스크립터 복사
	list_init(&current->fd_list);
	current->next_fd = 2;

	struct list_elem *e;
	for (e = list_begin(&parent->fd_list); e != list_end(&parent->fd_list); e = list_next(e))
	{
		struct file_descriptor *p_fdesc = list_entry(e, struct file_descriptor, elem);
		struct file_descriptor *c_fdesc = malloc(sizeof(struct file_descriptor));
		if (c_fdesc == NULL)
			goto cleanup;

		c_fdesc->fd = p_fdesc->fd;
		c_fdesc->file = file_duplicate(p_fdesc->file);
		list_push_back(&current->fd_list, &c_fdesc->elem);
		current->next_fd = p_fdesc->fd >= current->next_fd ? p_fdesc->fd + 1 : current->next_fd;
	}

	// 부모 자식 연결
	list_push_back(&parent->child_list, &current->child_elem);

	// 성공 설정 및 세마포어 알림
	current->parent = parent;
	current->fork_success = true;
	sema_up(&parent->fork_sema);
	palloc_free_page(args);

	// 자식은 0 반환
	if_.R.rax = 0;

	do_iret(&if_); // 유저 모드 진입 (절대 리턴 안 됨)

	NOT_REACHED(); // 여기에 도달하면 오류

cleanup:
	// 실패 처리
	current->fork_success = false;
	current->killed_by_exception = true;
	sema_up(&current->fork_sema);
	palloc_free_page(args);
	thread_exit(); // 실패 시 정리 후 종료
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void *f_name)
{
	struct thread *curr = thread_current();
	char *file_name = f_name;
	bool success;

	// 인자 파싱을 위한 복사본 생성
	char *cmd_copy = palloc_get_page(PAL_ZERO);
	if (cmd_copy == NULL)
		return -1;
	strlcpy(cmd_copy, file_name, PGSIZE);

	// 인자 파싱
	char *argv_local[32];
	int argc_local = 0;
	char *token, *save_ptr;

	token = strtok_r(cmd_copy, " ", &save_ptr);
	if (token == NULL)
	{
		palloc_free_page(cmd_copy);
		return -1;
	}

	// 첫 번째 토큰이 실제 실행 파일명
	char *actual_file_name = token;
	argv_local[argc_local++] = token;

	while ((token = strtok_r(NULL, " ", &save_ptr)) != NULL)
		argv_local[argc_local++] = token;

	// 인자 복사용 페이지
	char **argv_page = palloc_get_page(PAL_ZERO);
	if (argv_page == NULL)
	{
		palloc_free_page(cmd_copy);
		return -1;
	}

	for (int i = 0; i < argc_local; i++)
		argv_page[i] = argv_local[i];

	curr->argc = argc_local;
	curr->argv = argv_page;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup();

	/* And then load the binary */
	success = load(actual_file_name, &_if);

	/* 메모리 정리 - load 완료 후에 정리 */
	palloc_free_page(cmd_copy);
	palloc_free_page(argv_page);

	/* If load failed, quit. */
	if (!success)
	{
		// 로드 실패 시 스레드 종료 처리 - 직접 exit 호출
		curr->exit_status = -1;
		printf("%s: exit(-1)\n", thread_name());
		thread_exit(); // 스레드 종료하므로 여기서 끝
		NOT_REACHED();
	}

	/* Start switched process. */
	do_iret(&_if);
	NOT_REACHED();
}

static struct thread *find_child(tid_t child_tid)
{
	struct list *children = &thread_current()->child_list;
	struct list_elem *e;

	for (e = list_begin(children); e != list_end(children); e = list_next(e))
	{
		struct thread *child = list_entry(e, struct thread, child_elem);
		if (child->tid == child_tid)
		{
			return child;
		}
	}
	return NULL;
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int process_wait(tid_t child_tid)
{

	struct thread *current = thread_current();
	struct thread *child = NULL;
	struct list_elem *e;

	// 자식 리스트에서 해당 tid 찾기
	for (e = list_begin(&current->child_list);
		 e != list_end(&current->child_list);
		 e = list_next(e))
	{
		struct thread *t = list_entry(e, struct thread, child_elem);
		if (t->tid == child_tid)
		{
			child = t;
			break;
		}
	}

	if (child == NULL)
		return -1; // 자식이 아니거나 이미 기다렸음

	// 자식이 종료될 때까지 대기
	sema_down(&child->wait_sema);

	// 종료 상태 가져오기
	int exit_status;
	if (child->killed_by_exception)
		exit_status = -1;
	else
		exit_status = child->exit_status;

	// 자식을 리스트에서 제거 (중복 wait 방지)
	list_remove(&child->child_elem);

	sema_up(&child->free_sema);

	return exit_status;
}

void close_all_files(void)
{
	struct thread *curr = thread_current();
	struct list *fd_list = &curr->fd_list;

	// fd_list가 제대로 초기화되었는지 확인
	if (fd_list == NULL || list_empty(fd_list))
		return;

	struct list_elem *e = list_begin(fd_list);

	while (e != list_end(fd_list))
	{
		struct list_elem *next = list_next(e); // 다음 요소 저장
		struct file_descriptor *fdesc = list_entry(e, struct file_descriptor, elem);

		// 리스트에서 제거
		list_remove(e);

		// 실행 중인 파일(running_file)은 여기서 닫지 않음
		if (fdesc != NULL && fdesc->file != NULL)
		{
			if (fdesc->file != curr->running_file)
			{
				file_close(fdesc->file);
			}
			// 실행 파일이라도 file 필드는 NULL로 둠
			fdesc->file = NULL;
		}

		if (fdesc != NULL)
		{
			free(fdesc);
		}
		e = next;
	}
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{

	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

	struct thread *cur = thread_current();

	// 파일 및 리소스 정리
	close_all_files();
	process_cleanup();

	/* 실행 중인 파일 write 허용 + 닫기 */
	if (cur->running_file != NULL)
	{

		file_close(cur->running_file);
		cur->running_file = NULL;
	}

	// 부모에게 종료 알림
	if (cur->parent != NULL)
	{
		sema_up(&cur->wait_sema);
		sema_down(&cur->free_sema);
	}

	// 자식들은 고아 프로세스가 되도록 처리
	struct list_elem *e;
	for (e = list_begin(&cur->child_list);
		 e != list_end(&cur->child_list);
		 e = list_next(e))
	{
		struct thread *child = list_entry(e, struct thread, child_elem);
		child->parent = NULL; // 부모 연결 끊기
	}
}

/* Free the current process's resources. */
static void
process_cleanup(void)
{
	struct thread *curr = thread_current();

#ifdef VM
	supplemental_page_table_kill(&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL)
	{
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate(NULL);
		pml4_destroy(pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread *next)
{
	/* Activate thread's page tables. */
	pml4_activate(next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0			/* Ignore. */
#define PT_LOAD 1			/* Loadable segment. */
#define PT_DYNAMIC 2		/* Dynamic linking info. */
#define PT_INTERP 3			/* Name of dynamic loader. */
#define PT_NOTE 4			/* Auxiliary info. */
#define PT_SHLIB 5			/* Reserved. */
#define PT_PHDR 6			/* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr
{
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
						 uint32_t read_bytes, uint32_t zero_bytes,
						 bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load(const char *file_name, struct intr_frame *if_)
{
	struct thread *t = thread_current();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create();
	if (t->pml4 == NULL)
		goto done;
	process_activate(thread_current());

	/* Open executable file. */
	file = filesys_open(file_name);
	if (file == NULL)
	{
		printf("load: %s: open failed\n", file_name);
		goto done;
	}

	t->running_file = file;
	file_deny_write(file);

	/* Read and verify executable header. */
	if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 0x3E // amd64
		|| ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024)
	{
		printf("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++)
	{
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length(file))
			goto done;
		file_seek(file, file_ofs);

		if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type)
		{
		case PT_NULL:
		case PT_NOTE:
		case PT_PHDR:
		case PT_STACK:
		default:
			/* Ignore this segment. */
			break;
		case PT_DYNAMIC:
		case PT_INTERP:
		case PT_SHLIB:
			goto done;
		case PT_LOAD:
			if (validate_segment(&phdr, file))
			{
				bool writable = (phdr.p_flags & PF_W) != 0;
				uint64_t file_page = phdr.p_offset & ~PGMASK;
				uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
				uint64_t page_offset = phdr.p_vaddr & PGMASK;
				uint32_t read_bytes, zero_bytes;
				if (phdr.p_filesz > 0)
				{
					/* Normal segment.
					 * Read initial part from disk and zero the rest. */
					read_bytes = page_offset + phdr.p_filesz;
					zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
				}
				else
				{
					/* Entirely zero.
					 * Don't read anything from disk. */
					read_bytes = 0;
					zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
				}
				if (!load_segment(file, file_page, (void *)mem_page,
								  read_bytes, zero_bytes, writable))
					goto done;
			}
			else
				goto done;
			break;
		}
	}

	/* Set up stack. */
	if (!setup_stack(if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	{
#define MAX_ARGC 32
		struct thread *t = thread_current();
		char *arg_ptrs[MAX_ARGC];
		uint64_t rsp = if_->rsp;

		// 문자열 복사 (역순 저장)
		for (int i = t->argc - 1; i >= 0; i--)
		{
			int len = strlen(t->argv[i]) + 1;
			rsp -= len;
			memcpy((void *)rsp, t->argv[i], len);
			arg_ptrs[i] = (char *)rsp;
		}

		// 8바이트 정렬
		rsp = rsp & ~0x7;

		// NULL sentinel
		rsp -= sizeof(char *);
		*(char **)rsp = NULL;

		// argv[i] 포인터 push (역순)
		for (int i = t->argc - 1; i >= 0; i--)
		{
			rsp -= sizeof(char *);
			*(char **)rsp = arg_ptrs[i];
		}

		char **argv_addr = (char **)rsp;

		// fake return address
		rsp -= sizeof(void *);
		*(void **)rsp = 0;

		// 레지스터 설정
		if_->rsp = rsp;
		if_->R.rdi = t->argc;
		if_->R.rsi = (uint64_t)argv_addr;

		// argument parsing 구현 후
	}

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	// file_close(file);
	// 파일 디스크립터에 file을 등록 시켜야함.
	struct file_descriptor *fdesc = malloc(sizeof(struct file_descriptor));
	if (fdesc == NULL)
		goto done;

	fdesc->file = file;
	fdesc->fd = t->next_fd++;
	list_push_back(&t->fd_list, &fdesc->elem);
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Phdr *phdr, struct file *file)
{
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t)file_length(file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr((void *)phdr->p_vaddr))
		return false;
	if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	file_seek(file, ofs);
	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page(PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
		{
			palloc_free_page(kpage);
			return false;
		}
		memset(kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page(upage, kpage, writable))
		{
			printf("fail\n");
			palloc_free_page(kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack(struct intr_frame *if_)
{
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (kpage != NULL)
	{
		success = install_page(((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page(kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page(void *upage, void *kpage, bool writable)
{
	struct thread *t = thread_current();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment(struct page *page, void *aux)
{
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer(VM_ANON, upage,
											writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack(struct intr_frame *if_)
{
	bool success = false;
	void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
