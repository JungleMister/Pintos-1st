#include "userprog/syscall.h"
#include "lib/user/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

#include "threads/init.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/palloc.h"


void syscall_entry (void);
void syscall_handler (struct intr_frame *);

// 시스템 콜 선언
void check_address(const char *addr);
pid_t sys_fork (const char *thread_name, struct intr_frame *if_);
int dup2(int oldfd, int newfd);
int add_file(struct file *file);

struct lock filesys_lock;

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init(&filesys_lock);
}
struct file *process_get_file (int fd){
	if (fd < 0 || fd >= MAX_FD)
		return NULL;
	struct file *f = thread_current()->fdt[fd];
	return f;
}
/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	// TODO: Your implementation goes here.
	switch (f->R.rax)
	{
	// 시스템 콜 번호는 rax 레지스터에 저장된다.
	case SYS_HALT:
		halt();
		break;

	case SYS_EXIT:
		// rdi에는 종료 상태가 저장됨
		exit(f->R.rdi);
		break;

	case SYS_FORK:
		/* code */
		char *thread_name = f->R.rdi;
		f->R.rax = sys_fork(thread_name, f);
		break;

	case SYS_EXEC:
		/* code */
		f->R.rax = exec(f->R.rdi);
		break;

	case SYS_WAIT:
		/* code */
		f->R.rax = wait(f->R.rdi);
		break;

	case SYS_CREATE:
		/* code */
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;

	case SYS_REMOVE:
		/* code */
		f->R.rax = remove(f->R.rdi);
		break;

	case SYS_OPEN:
		/* code */
		f->R.rax = open(f->R.rdi);
		break;

	case SYS_FILESIZE:
		/* code */
		f->R.rax = filesize(f->R.rdi);
		break;

	case SYS_READ:
		/* code */
		f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
		break;

	case SYS_WRITE:
		/* code */
		f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;

	case SYS_SEEK:
		/* code */
		seek(f->R.rdi, f->R.rsi);
		break;

	case SYS_TELL:
		/* code */
		f->R.rax = tell(f->R.rdi);
		break;

	case SYS_CLOSE:
		/* code */
		close(f->R.rdi);
		break;

	/* Project 3 */
	case SYS_MMAP:
		/* code */
		break;

	case SYS_MUNMAP:
		/* code */
		break;

	/* Project 4 */
	case SYS_CHDIR:
		/* code */
		break;

	case SYS_MKDIR:
		/* code */
		break;

	case SYS_READDIR:
		/* code */
		break;

	case SYS_ISDIR:
		/* code */
		break;

	case SYS_INUMBER:
		/* code */
		break;

	case SYS_SYMLINK:
		/* code */
		break;

	/* Extra for Project 2 */
	case SYS_DUP2:
		/* code */
		f->R.rax = dup2(f->R.rdi, f->R.rsi);
		break;

	case SYS_MOUNT:
		/* code */
		break;

	case SYS_UMOUNT:
		/* code */
		break;
	
	default:
		thread_exit ();
		break;
	}
}

void check_address(const char *addr)
{
	// 할당할 때만 확인하고 나머지는 page fault로 확인
    if (addr == NULL || !is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL)
        exit(-1);
}

int add_file(struct file *file){
	struct thread *cur = thread_current();

	for (int fd = 0; fd < MAX_FD; fd++){
		if (cur->fdt[fd] == NULL){
			cur->fdt[fd] = file;
			cur->fd_idx = fd;
			return cur->fd_idx;
		}
	}

	cur->fd_idx = MAX_FD;	// 응 테이블 가득 차면 오류로 처리할거여~
	return -1; 				// 테이블 가득 참
}

// OS 종료
void halt(){
	power_off();
}

// 현재 프로세스를 종료
void exit(int status){
    struct thread *cur = thread_current();	
    cur->exit_status = status;
	printf("%s: exit(%d)\n", cur->name, status);
    thread_exit();
}

tid_t sys_fork (const char *thread_name, struct intr_frame *if_){
	struct thread *cur = thread_current();
	
	tid_t child_tid = process_fork (thread_name, if_);
	
	// 자식 스레드가 생성되지 않았다면 TID_ERROR 반환
	if (child_tid == TID_ERROR)
		return TID_ERROR;	

	return child_tid;
}

int write (int fd, const void *buffer, unsigned length) {
	check_address(buffer);
	
	// write에서 STDIN을 할 필요 없음
	if (fd <= 0 || buffer == NULL || fd >= MAX_FD)
		return -1;

	struct thread *cur = thread_current();
	struct file *file = cur->fdt[fd];
	unsigned written = 0;

	// 파일이 없을 때, 표준 입력일 때
	if (file == NULL || file == STDIN_){
		return -1;
	}
	else if (file == STDOUT_){
		if (cur->stdout_count == 0){
			NOT_REACHED();
			cur->fdt[fd] = NULL;
			written = -1;
		}
		else{
			if (length <= 512){
				putbuf(buffer, length);
				return length;
			}
			else{
				const char *buf_ptr = buffer;

				while (written < length){
					unsigned remain = length - written;
					unsigned write_size = (remain > 512) ? 512 : remain;
					putbuf(buf_ptr + written, write_size); 
					written += write_size;
				}
			}
		}
	}
	else{
		lock_acquire(&filesys_lock);
		written = file_write(file, buffer, length);
		lock_release(&filesys_lock);
	}
	return written;
}

bool create(const char *file, unsigned initial_size){
	check_address(file);
	
	return filesys_create(file, initial_size);
}

bool remove(const char *file) {
	check_address(file);

	return filesys_remove(file);
}

int exec (const char *file){
	check_address(file);

	char *f_name = palloc_get_page(PAL_ASSERT | PAL_ZERO);
	if (f_name == NULL)
        exit(-1);   

	strlcpy(f_name, file, PGSIZE);

	if (process_exec(f_name) == -1)
		exit(-1);

	NOT_REACHED();
	return 0;
}

int open (const char *file){
	check_address(file);
	
	lock_acquire(&filesys_lock);
	struct file *f = filesys_open(file);

	if (f == NULL){
		return -1;
	}
	// thread fd에 등록
	int fd = add_file(f);
	
	if (fd == -1) {
		file_close(f);
	}
	lock_release(&filesys_lock);

	return fd;
}

int filesize(int fd){
	struct thread *cur = thread_current();
	return file_length(cur->fdt[fd]);
}

int read (int fd, void *buffer, unsigned length){
	check_address(buffer);

	if (fd < 0 || fd >= MAX_FD){
		return -1;
	}
	
	struct thread *cur = thread_current();
	struct file *file = cur->fdt[fd];
	unsigned bytes_read = 0;

	// 파일이 없을 때, 출력일 때
	if (file == NULL || file == STDOUT_){
		return -1;
	}

	if (file == STDIN_) {
		if (cur->stdin_count == 0){
			NOT_REACHED();
			cur->fdt[fd] = NULL;
			bytes_read = -1;
		}
		else{
			// 표준 입력에서 키보드 입력 읽기
			uint8_t *buf = (uint8_t *)buffer;

			for (int i = 0; i < length; i++) {
				char key = input_getc();  // 키 입력까지 대기
				buf[i] = key;
				bytes_read++;

				if (key == '\0') break;   // 널 문자로 종료
				}
			return bytes_read;
		}
	}
	else{
		lock_acquire(&filesys_lock);
		bytes_read = file_read(file, buffer, length);
		lock_release(&filesys_lock);
	}

	return bytes_read;
}

void seek(int fd, unsigned position){
	lock_acquire(&filesys_lock);			// 변경이 될 가능성이 존재 써주는게 좋긴하다. 단, 안써도 테스트는 통과

	if (fd < 0 || fd >= MAX_FD)
		return;
	struct thread *cur = thread_current();
	struct file *file = cur->fdt[fd];
	
	// 1, 2는 표준입출력
	if (file > 2)
		file_seek(cur->fdt[fd], position);

	lock_release(&filesys_lock);
}

unsigned tell (int fd){
	lock_acquire(&filesys_lock);			// 변경이 될 가능성이 존재 써주는게 좋긴하다. 단, 안써도 테스트는 통과
	if (fd < 0 || fd >= MAX_FD)
		return;
	struct file *file = thread_current()->fdt[fd];

	if (file < 2)
		return;
	
	int pos = file_tell(file);
	lock_release(&filesys_lock);

	return pos;
}

void close (int fd){
	if (fd < 0 || fd >= MAX_FD)
		return;
	
	struct thread *cur = thread_current();
	struct file *file = cur->fdt[fd];

	if(file == NULL)
		return;

	if (file == STDIN_){
		cur->stdin_count--;
	}
	else if (file == STDOUT_){
		cur->stdout_count--;
	}
	else if (get_ref_cnt(file) == 1){
		file_close(file);
	}
	else {
		dec_ref_cnt(file);
	}
	
	cur->fdt[fd] = NULL;
}

int wait (tid_t tid){
	return process_wait(tid);
}

int dup2(int oldfd, int newfd){	
	if (oldfd < 0 || newfd < 0 || oldfd >= MAX_FD || newfd >= MAX_FD)
		return -1;
	
	if (oldfd == newfd)
			return newfd;
		
	struct thread *cur = thread_current();
	struct file *oldfile = cur->fdt[oldfd];

	if (oldfile == NULL){
		return -1;
	}

	if (oldfile == STDIN_ && cur->stdin_count != 0){
		cur->stdin_count++;
	}
	else if (oldfile == STDOUT_ && cur->stdout_count != 0){
		cur->stdout_count++;
	}
	else if (oldfile > STDOUT_){
		inc_ref_cnt(oldfile);
	}

	if (cur->fdt[newfd] != NULL)
		close(newfd);
	
	// dup2는 얕은 복사다 ㅋ 알아버렸다
    cur->fdt[newfd] = oldfile;  // 동일한 객체 공유
	
	return newfd;
}
