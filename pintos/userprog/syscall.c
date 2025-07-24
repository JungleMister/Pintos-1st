#include "userprog/syscall.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "threads/palloc.h"
#include "intrinsic.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081			/* Segment selector msr */
#define MSR_LSTAR 0xc0000082		/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
							((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			  FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}
static void check_address(const void *addr)
{
	if (addr == NULL || !is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL)
	{
		thread_current()->exit_status = -1;
        printf("%s: exit(-1)\n", thread_name());
        thread_exit();
	}
}

static struct file_descriptor *
find_fd(int fd)
{
	struct list_elem *e;
	struct list *fd_list = &thread_current()->fd_list;
	for (e = list_begin(fd_list); e != list_end(fd_list); e = list_next(e))
	{
		struct file_descriptor *fdesc = list_entry(e, struct file_descriptor, elem);
		if (fdesc->fd == fd)
		{
			return fdesc;
		}
	}
	return NULL;
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{
	// TODO: Your implementation goes here.
	uint64_t syscall_num = f->R.rax;
	switch (syscall_num)
	{
	case SYS_WRITE:
	{
		int fd = f->R.rdi;
		const char *buffer = (const char *)f->R.rsi;
		unsigned size = f->R.rdx;

		for (unsigned i = 0; i < size; i++)
			check_address(buffer + i);

		if (fd == 1)
		{
			putbuf(buffer, size);
			f->R.rax = size;
		}
		else
		{
			struct file_descriptor *fdesc = find_fd(fd);
			if (fdesc == NULL || fdesc->file == NULL)
			{
				f->R.rax = -1;
				break;
			}

			off_t bytes_written = file_write(fdesc->file, buffer, size);
			f->R.rax = bytes_written;
		}
		break;
	}

	case SYS_EXIT:
	{
		int status = f->R.rdi;
		thread_current()->exit_status = status;
		printf("%s: exit(%d)\n", thread_name(), status);
		thread_exit();
		break;
	}

	case SYS_CREATE:
	{
		const char *file = (const char *)f->R.rdi;
		unsigned initial_size = f->R.rsi;

		if (file == NULL)
		{
			thread_current()->exit_status = -1;
			printf("%s: exit(-1)\n", thread_name());
			thread_exit();
		}

		// 유저 주소 접근 권한 검사
		check_address(file);

		f->R.rax = filesys_create(file, initial_size);
		break;
	}

	case SYS_OPEN:
	{
		const char *file = (const char *)f->R.rdi;
		check_address(file); // 주소 유효성 확인

		struct file *opened_file = filesys_open(file);
		if (opened_file == NULL)
		{
			f->R.rax = -1;
			break;
		}

		// 파일 디스크립터 할당
		struct file_descriptor *fdesc = malloc(sizeof(struct file_descriptor));
		if (fdesc == NULL)
		{
			file_close(opened_file);
			f->R.rax = -1;
			break;
		}

		// 파일 디스크립터 초기화
		fdesc->file = opened_file;
		fdesc->fd = thread_current()->next_fd++;

		// 스레드의 파일 디스크립터 리스트에 추가
		list_push_back(&thread_current()->fd_list, &fdesc->elem);

		f->R.rax = fdesc->fd; // 파일 디스크립터 반환
		break;
	}

	case SYS_CLOSE:
	{
		int fd = f->R.rdi;

		// stdin/stdout 닫기 방지
		if (fd <= 1)
		{
			f->R.rax = -1;
			break;
		}

		struct file_descriptor *fdesc = find_fd(fd);
		if (fdesc == NULL)
		{
			f->R.rax = -1;
			break;
		}

		// 리스트에서 먼저 제거
		list_remove(&fdesc->elem);

		// 파일이 유효한 경우에만 닫기
		if (fdesc->file != NULL)
		{
			file_close(fdesc->file);
			fdesc->file = NULL;
		}

		// 디스크립터 메모리 해제
		free(fdesc);
		f->R.rax = 0; // 성공
		break;
	}

	case SYS_FILESIZE:
	{
		int fd = f->R.rdi;
		struct file_descriptor *fdesc = find_fd(fd);
		if (fdesc == NULL || fdesc->file == NULL)
		{
			f->R.rax = -1;
		}
		else
		{
			f->R.rax = file_length(fdesc->file);
		}
		break;
	}

	case SYS_READ:
	{
		int fd = f->R.rdi;
		void *buffer = (void *)f->R.rsi;
		unsigned size = f->R.rdx;

		// 버퍼 주소 유효성 검사
		if (buffer == NULL)
		{
			thread_current()->exit_status = -1;
			printf("%s: exit(-1)\n", thread_name());
			thread_exit();
		}

		// 버퍼 범위 전체에 대해 주소 유효성 검사
		for (unsigned i = 0; i < size; i += PGSIZE)
			check_address((char *)buffer + i);
		if (size > 0)
			check_address((char *)buffer + size - 1);

		if (fd == 0) // stdin
		{
			// 표준 입력에서 읽기 (키보드 입력)
			char *char_buffer = (char *)buffer;
			unsigned bytes_read = 0;
			for (unsigned i = 0; i < size; i++)
			{
				char c = input_getc();
				char_buffer[i] = c;
				bytes_read++;
			}
			f->R.rax = bytes_read;
		}
		else
		{
			// 파일에서 읽기
			struct file_descriptor *fdesc = find_fd(fd);
			if (fdesc == NULL || fdesc->file == NULL)
			{
				f->R.rax = -1;
				break;
			}

			off_t bytes_read = file_read(fdesc->file, buffer, size);
			f->R.rax = bytes_read;
		}
		break;
	}

	case SYS_REMOVE:
	{
		const char *file = (const char *)f->R.rdi;

		// 주소 유효성 검사
		check_address(file);

		// NULL 포인터 방어
		if (file == NULL)
		{
			thread_current()->exit_status = -1;
			printf("%s: exit(-1)\n", thread_name());
			thread_exit();
		}

		// 파일 삭제 시도
		f->R.rax = filesys_remove(file); // 성공 시 true, 실패 시 false 반환
		break;
	}

	case SYS_SEEK:
	{
		int fd = f->R.rdi;
		unsigned position = f->R.rsi;

		struct file_descriptor *fdesc = find_fd(fd);
		if (fdesc == NULL || fdesc->file == NULL)
		{
			// 유효하지 않은 fd는 무시 (반환값이 없으므로 종료만)
			break;
		}

		file_seek(fdesc->file, position);
		break;
	}

	case SYS_TELL:
	{
		int fd = f->R.rdi;

		struct file_descriptor *fdesc = find_fd(fd);
		if (fdesc == NULL || fdesc->file == NULL)
		{
			f->R.rax = -1;
			break;
		}

		f->R.rax = file_tell(fdesc->file);
		break;
	}

	case SYS_FORK:
	{
		const char *name = (const char *)f->R.rdi;
		check_address(name); // 포인터 유효성 검사

		f->R.rax = process_fork(name, f); // intr_frame 전달!
		break;
	}

	case SYS_EXEC:
	{
		const char *cmd_line = (const char *)f->R.rdi;
		check_address(cmd_line); // 유저 주소 검증
		f->R.rax = process_exec((void *)cmd_line);
		NOT_REACHED();
	}

	case SYS_WAIT:
	{
		tid_t tid = f->R.rdi;
		f->R.rax = process_wait(tid);
		break;
	}

		/*
		
		case SYS_EXEC:
		case SYS_WAIT:
		*/

	default:
	
		  
	}
}
