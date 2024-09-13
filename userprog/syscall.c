#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"
#include <string.h>
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/palloc.h"


/* Process identifier. */
typedef int pid_t;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
bool CheckFileDir(const char *, uint64_t *);

void halt (void);
void exit (int status);
int wait (pid_t pid);
pid_t fork (const char *thread_name, struct intr_frame *f);
int exec (const char *cmd_line);
int write (int fd, const void *buffer, unsigned size);
int read (int fd, void *buffer, unsigned size);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
void close (int fd);
void seek (int fd, unsigned position);
unsigned tell (int fd);

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
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	// TODO: Your implementation goes here.
	thread_current()->is_user = true;
	switch (f->R.rax)
	{
	case SYS_HALT:	//0
		halt();
		break;
	case SYS_EXIT:	//1
		exit(f->R.rdi);
		break;
	case SYS_FORK:	//2
		f->R.rax = fork(f->R.rdi, f);
		break;
	case SYS_EXEC:	//3
		f->R.rax = exec(f->R.rdi);
		break;
	case SYS_WAIT:	//4
		f->R.rax = wait(f->R.rdi);
		break;
	case SYS_CREATE://5
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;
	case SYS_REMOVE://6
		f->R.rax = remove(f->R.rdi);
		break;
	case SYS_OPEN:	//7
		f->R.rax = open(f->R.rdi);
		break;
	case SYS_FILESIZE://8
		f->R.rax = filesize(f->R.rdi);
		break;
	case SYS_READ:	//9
		f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
	 	break;
	case SYS_WRITE:	//10
		f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_SEEK:	//11
		seek(f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL:	//12
		f->R.rax = tell(f->R.rdi);
		break;
	case SYS_CLOSE: //13
		close(f->R.rdi);
		break;

	
	default:
		break;
	}
}



#pragma region Finished
void halt (void)
{
	power_off();
}

void exit (int status)
{
	struct thread* cur = thread_current();
	
	cur->thread_exit_status = status;
	if(cur->parent)
		cur->parent->childrenExitStatus = status;
	thread_exit();
}

pid_t fork (const char *thread_name, struct intr_frame *f)
{
	return process_fork(thread_name, f);
}

int exec (const char *cmd_line)
{
	char *line = palloc_get_page(PAL_ZERO);

	strlcpy(line, cmd_line, PGSIZE);
	int ret = process_exec(line);

	if(ret == -1)
		exit(-1);
		
	return ret;
}

int wait (pid_t pid)
{
	return process_wait(pid);
}

bool create (const char *file, unsigned initial_size)
{
	if(CheckFileDir(file, thread_current()->pml4)) exit(-1);
	return filesys_create(file, initial_size);
}

bool remove (const char *file)
{
	return filesys_remove(file);
}

int open (const char *file)
{
	struct thread* cur = thread_current();

	if(CheckFileDir(file, cur->pml4))	exit(-1);

	struct file *retFile = filesys_open(file);
	if(!retFile) return -1;

	for(int i = 3; i < FDMAXCOUNT; i++)
	{
		if(cur->fds[i] == NULL)
		{
			cur->fds[i] = retFile;
			return i;
		}
	}

	return -1;
}

int filesize (int fd)
{
	return file_length(thread_current()->fds[fd]);
}

int read (int fd, void *buffer, unsigned size)
{
	if(fd < 2 || fd >= FDMAXCOUNT) return -1;

	struct file* file = thread_current()->fds[fd];
	if(!file) return -1;

	return file_read(file, buffer, size);
}

int write (int fd, const void *buffer, unsigned size)
{
	if(fd == 1)
	{
		const char* s = (const char*) buffer;
		printf("%s", s);
		return strlen(s);
	}
	
	if(fd < 2 || fd >= FDMAXCOUNT) exit(-1);

	struct file * curFile = thread_current()->fds[fd];
	if(!curFile) exit(-1);

	return file_write(curFile, buffer, size);
}

void seek (int fd, unsigned position)
{
	struct file* file = thread_current()->fds[fd];
	if(!file) exit(-1);

	file_seek(file, position);
}

unsigned tell (int fd)
{
	struct file* file = thread_current()->fds[fd];
	if(!file) exit(-1);

	return file_tell(file);
}

void close (int fd)
{
	if(fd < 2 || fd >= FDMAXCOUNT) return;
	struct thread* cur = thread_current();

	if(cur->denied_file == cur->fds[fd])
		file_close(cur->fds[fd]);
	cur->fds[fd] = NULL;
}

bool CheckFileDir(const char *file, uint64_t *pml4)
{
	return (!file || is_kernel_vaddr(file) || pml4_get_page(pml4, file) == NULL);
}

#pragma endregion