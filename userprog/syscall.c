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

/* Process identifier. */
typedef int pid_t;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

void halt (void);
void exit (int status);
pid_t fork (const char *thread_name);
int exec (const char *cmd_line);
int write (int fd, const void *buffer, unsigned size);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
void close (int fd);

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
	//printf ("system call! %d\n", f->R.rax);
	thread_current()->is_user = true;
	switch (f->R.rax)
	{
	case SYS_HALT:	//0
		halt();
		break;
	case SYS_EXIT:	//1
		exit(f->R.rdi);
		break;
	// case SYS_FORK:	//2
	// 	fork(f->R.rdi);
	// 	break;
	// case SYS_EXEC:	//3
	// 	exec(f->R.rdi);
	// 	break;
	case SYS_WAIT:	//4
		f->R.rax = wait(f->R.rdi);
		break;
	case SYS_CREATE://5
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;
	// case SYS_REMOVE://6
	// 	remove(f->R.rdi);
	// 	break;
	case SYS_OPEN:
		f->R.rax = open(f->R.rdi);
		break;
	case SYS_FILESIZE://8
		f->R.rax = filesize(f->R.rdi);
		break;

	// case SYS_READ://9
	// 	f->R.rax = file_read(f->R.rdi, f->R.rsi, f->R.rdx);
	//  	break;

	case SYS_WRITE:	//10
		write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;

	
	default:
		break;
	}
	//thread_exit ();
}

void exit (int status)
{
	struct thread* cur = thread_current();
	
	cur->thread_exit_status = status;
	// printf("%s: exit(%d)", cur->name, status);
	thread_exit();
}

int wait (pid_t pid)
{
	return process_wait(pid);
}

pid_t fork (const char *thread_name)
{
	return process_fork(thread_name, NULL);
}

int write (int fd, const void *buffer, unsigned size)
{
	const char* s = (const char*) buffer;
	if(fd == 1)
	{
		printf("%s", s);
		return strlen(s);
	}

	printf("fd:%d is not supported.", fd);
	halt();
	return size;
}

int exec (const char *cmd_line)
{
	return 0;
}



#pragma region Finished
void halt (void)
{
	power_off();
}

bool create (const char *file, unsigned initial_size)
{
	if(!file || is_kernel_vaddr(file) || pml4_get_page(thread_current()->pml4, file) == NULL) exit(-1);
	return filesys_create(file, initial_size);
}

bool remove (const char *file)
{
	return filesys_remove(file);
}

int filesize (int fd)
{
	return sizeof(thread_current()->fds[fd]);
}

int open (const char *file)
{
	struct thread* cur = thread_current();

	if(!file || is_kernel_vaddr(file) || pml4_get_page(cur->pml4, file) == NULL)	exit(-1);
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

void close (int fd)
{
	struct thread* cur = thread_current();

	cur->fds[fd] = NULL;
}
#pragma endregion