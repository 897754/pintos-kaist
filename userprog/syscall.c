#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
//	준용 추가
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "userprog/process.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include <string.h>

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

//	준용 추가

/* Reads a byte at user virtual address UADDR.
 * UADDR must be below KERN_BASE.
 * Returns the byte value if successful, -1 if a segfault
 * occurred. */
static int64_t
get_user(const uint8_t *uaddr)
{
	int64_t result;
	__asm __volatile(
		"movabsq $done_get, %0\n"
		"movzbq %1, %0\n"
		"done_get:\n"
		: "=&a"(result) : "m"(*uaddr));
	return result;
}

/* Writes BYTE to user address UDST.
 * UDST must be below KERN_BASE.
 * Returns true if successful, false if a segfault occurred. */
static bool
put_user(uint8_t *udst, uint8_t byte)
{
	int64_t error_code;
	__asm __volatile(
		"movabsq $done_put, %0\n"
		"movb %b2, %1\n"
		"done_put:\n"
		: "=&a"(error_code), "=m"(*udst) : "q"(byte));
	return error_code != -1;
}

void isLegalAddr(void *ptr)
{
	struct thread *th = thread_current();
	if (is_kernel_vaddr(ptr) || ptr == NULL || pml4_get_page(th->pml4, ptr) == NULL)
	{
		exit(-1);
	}
}

void exit(int status)
{
	thread_current()->exitStatus = status;
	thread_exit();
}

tid_t fork(const char *thread_name, struct intr_frame *frame)
{
	tid_t returnPid = process_fork(thread_name, frame);
	enum intr_level old_intr = intr_disable();
	thread_block();
	intr_set_level(old_intr);
	if (frame->R.rax == TID_ERROR) {
		return TID_ERROR;
	}
	return returnPid;
}

bool create(const char *file, unsigned initial_size)
{
	isLegalAddr(file);
	return filesys_create(file, (off_t)initial_size);
}

bool remove(const char *file)
{
	isLegalAddr(file);
	return filesys_remove(file);
}

bool isFileOpened(int fd)
{
	if (0 <= fd && fd < FD_MAX)
	{
		if (thread_current()->descriptors[fd] != NULL)
		{
			return true;
		}
	}
	return false;
}

int open(const char *file)
{
	isLegalAddr(file);
	struct thread *th = thread_current();
	if (th->nextDescriptor >= FD_MAX)
	{
		return -1;
	}
	else
	{
		int desc = th->nextDescriptor;
		th->descriptors[desc] = filesys_open(file);
		if (th->descriptors[desc] == NULL)
		{
			return -1;
		}
		else
		{
			while (th->descriptors[th->nextDescriptor] != NULL)
			{
				th->nextDescriptor++;
			}
			return desc;
		}
	}
}

int filesize(int fd)
{
	if (isFileOpened(fd))
	{
		return file_length(thread_current()->descriptors[fd]);
	}
	else
	{
		exit(-1);
	}
}

int read(int fd, void *buffer, unsigned size)
{
	isLegalAddr(buffer);
	if (isFileOpened(fd))
	{
		struct file *target = thread_current()->descriptors[fd];
		off_t returnValue = file_read(target, buffer, (off_t)size);
		return returnValue;
	}
	else
	{
		exit(-1);
	}
}

int write(int fd, const void *buffer, unsigned size)
{
	isLegalAddr(buffer);
	if (fd == STDOUT_FILENO)
	{
		putbuf(buffer, size);
		return size;
	}
	else if (isFileOpened(fd))
	{
		struct file *target = thread_current()->descriptors[fd];
		off_t returnValue = file_write(target, buffer, (off_t)size);
		return returnValue;
	}
	else
	{
		exit(-1);
	}
}

void seek(int fd, unsigned position)
{
	if (isFileOpened(fd))
	{
		file_seek(thread_current()->descriptors[fd], (off_t)position);
	}
	else
	{
		exit(-1);
	}
}

unsigned tell(int fd)
{
	if (isFileOpened(fd))
	{
		return thread_current()->descriptors[fd];
	}
	else
	{
		exit(-1);
	}
}

void close(int fd)
{
	if (isFileOpened(fd))
	{
		file_close(thread_current()->descriptors[fd]);
		thread_current()->descriptors[fd] = NULL;
		thread_current()->nextDescriptor = fd;
	}
	else
	{
		exit(-1);
	}
}

int exec(const char *cmd_line)
{
	isLegalAddr(cmd_line);
	char *cmdCopy = palloc_get_page(0);

	file_close(thread_current()->execFile);
	strlcpy(cmdCopy, cmd_line, PGSIZE);
	if (process_exec(cmdCopy))
	{
		exit(-1);
	}
	return -1;
}

tid_t wait(tid_t pid)
{
	return process_wait(pid);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f)
{
	// TODO: Your implementation goes here.
	//	systemcall 번호 - rax
	//	인자 - rdi, rsi, rdx, r10, r8, r9
	switch (f->R.rax)
	{
	case SYS_HALT:
		power_off();
		break;
	case SYS_EXIT:
		exit(f->R.rdi);
		break;
	case SYS_FORK:
		f->R.rax = fork(f->R.rdi, f);
		break;
	case SYS_EXEC:
		f->R.rax = exec(f->R.rdi);
		break;
	case SYS_WAIT:
		f->R.rax = wait(f->R.rdi);
		break;
	case SYS_CREATE:
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;
	case SYS_REMOVE:
		f->R.rax = remove(f->R.rdi);
		break;
	case SYS_OPEN:
		f->R.rax = open(f->R.rdi);
		break;
	case SYS_FILESIZE:
		f->R.rax = filesize(f->R.rdi);
		break;
	case SYS_READ:
		f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_WRITE:
		f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_SEEK:
		seek(f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL:
		f->R.rax = tell(f->R.rdi);
		break;
	case SYS_CLOSE:
		close(f->R.rdi);
		break;
	}
}
