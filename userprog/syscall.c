#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

#include <string.h>
#include "threads/palloc.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include <console.h>
#include "devices/input.h"
#include "userprog/process.h"
#include "threads/synch.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void set_code_and_exit(int exit_code);
bool check_valid_mem(void* ptr);
bool check_valid_fd(int fd);

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

struct lock lock;

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
	
	lock_init(&lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	thread_current()->is_user = true;
	
	switch (f->R.rax) {
	case SYS_HALT:                   /* Halt the operating system. */ 
	{
		power_off();
		break;
	}
	case SYS_EXIT:                   /* Terminate this process. */
	{	
		set_code_and_exit(f->R.rdi);
		break;
	}
	case SYS_FORK:                   /* Clone current process. */
	{
		char *thread_name = f->R.rdi;
		if (!check_valid_mem(thread_name)) {
			f->R.rax = -1;
			break;
		}

		f->R.rax = process_fork(thread_name, f);
		
		break;
	}
	case SYS_EXEC:                   /* Switch current process. */
	{
		char *file = f->R.rdi; 
		if (!check_valid_mem(file)) break;
		
		char *fn_copy = palloc_get_page (0);
		if (fn_copy == NULL)
			return TID_ERROR;
		strlcpy (fn_copy, file, PGSIZE);
		
		if (process_exec(fn_copy) < 0)
		{
			palloc_free_page(fn_copy);
			f->R.rax = -1;
			set_code_and_exit(-1);
		}
		break;
	}
	case SYS_WAIT:                   /* Wait for a child process to die. */
	{
		tid_t pid = f->R.rdi;

		f->R.rax = process_wait(pid);
		// thread_current()->children[pid] = NULL;

		break;
	}
	case SYS_CREATE:                 /* Create a file. */
	{
		char *file = f->R.rdi; 
		unsigned initial_size = f->R.rsi;

		if (check_valid_mem(file))
			f->R.rax = filesys_create(file, initial_size);
		
		else
			set_code_and_exit(-1);
		
		break;
	}
	case SYS_REMOVE:                 /* Delete a file. */
	{
		char *file_name = f->R.rdi;
		if (check_valid_mem(file_name)){
			f->R.rax = filesys_remove(file_name);
		}
		else f->R.rax = false;

		break;
	}
	case SYS_OPEN: {				/* Open a file. */
		char *file_name = f->R.rdi;
		if (!check_valid_mem(file_name))
			set_code_and_exit(-1);

		struct file* open_file = filesys_open(file_name);
		
		if (open_file == NULL)
			f->R.rax = -1;
		else
		{
			// int i = 0;
			// while(thread_current()->fd_table[i] && i < 64) i ++;
			// thread_current()->fd_table[i] = open_file;
			
			int i = 0;
			while(thread_current()->fd_table[i] && i < 64) i++;
			if (thread_current()->fd_table[i] == NULL)
				thread_current()->fd_table[i] = open_file;
			else{
				file_close(open_file);
			} 

			f->R.rax = i;
		}
		break;
	}                   
	case SYS_FILESIZE:               /* Obtain a file's size. */
	{
		int fd = f->R.rdi;
		if (check_valid_fd(fd))
		{
			struct file* open_file = thread_current()->fd_table[fd]; 
			f->R.rax = file_length(open_file);
		}
	
		break;
	}

	case SYS_READ:                   /* Read from a file. */
	{
		int fd = f->R.rdi;
		void *buffer = f->R.rsi;
		unsigned size = f->R.rdx;

		if (!check_valid_fd(fd) || !check_valid_mem(buffer)){
			set_code_and_exit(-1);
		} 

		lock_acquire(&lock);

		if (fd == 0) 
			f->R.rax = input_getc();

		else
		{
			struct file* read_file 
					= (struct file*)(thread_current()->fd_table[fd]);
			f->R.rax = file_read(read_file, buffer, size);
		}
		lock_release(&lock);	
		break;
	}
	case SYS_WRITE: {                /* Write to a file. */
		int fd = f->R.rdi;
		void *buffer = f->R.rsi;
		unsigned size = f->R.rdx;
		
		if (!check_valid_mem(buffer) || !check_valid_fd(fd)){
			set_code_and_exit(-1);
		} 

		lock_acquire(&lock);
		
		if (fd == 1) 
		{
			putbuf(buffer, size);
			f->R.rax = size;
		}
		else
		{
			// ???????? 왜 palloc 하면 lg가 fail 나지??????
			// char *contents = palloc_get_page (0);
			// if (contents == NULL)
			// 	return TID_ERROR;
			// strlcpy (contents, (char *)buffer, PGSIZE);
			
			struct file* write_file 
					= (struct file*)(thread_current()->fd_table[fd]);
			f->R.rax = file_write(write_file, buffer, size);
			
			// palloc_free_page (contents);	
		}
		lock_release(&lock);	
		break;
	}
	case SYS_SEEK:                   /* Change position in a file. */
	{
		int fd = f->R.rdi;
		unsigned position = f->R.rsi;

		if (check_valid_fd(fd))
			file_seek(thread_current()->fd_table[fd], position);
		break;
	}
	case SYS_TELL:                   /* Report current position in a file. */
	{
		int fd = f->R.rdi;
		if (check_valid_fd(fd))
			f->R.rax = file_tell(thread_current()->fd_table[fd]);
		break;
	}
	case SYS_CLOSE:                  /* Close a file. */
	{
		int fd = f->R.rdi;

		if (!check_valid_fd(fd)) break;
		
		struct file* close_file 
				= thread_current()->fd_table[fd];
		
		file_close(close_file);
		thread_current()->fd_table[fd] = NULL;
	
		break;
	}

	default:
	{
		set_code_and_exit(-1);
    	break;
	}

	// /* Project 3 and optionally project 4. */
	// SYS_MMAP,                   /* Map a file into memory. */
	// SYS_MUNMAP,                 /* Remove a memory mapping. */

	// /* Project 4 only. */
	// SYS_CHDIR,                  /* Change the current directory. */
	// SYS_MKDIR,                  /* Create a directory. */
	// SYS_READDIR,                /* Reads a directory entry. */
	// SYS_ISDIR,                  /* Tests if a fd represents a directory. */
	// SYS_INUMBER,                /* Returns the inode number for a fd. */
	// SYS_SYMLINK,                /* Returns the inode number for a fd. */

	// /* Extra for Project 2 */
	// SYS_DUP2,                   /* Duplicate the file descriptor */

	// SYS_MOUNT,
	// SYS_UMOUNT,
	}
}

bool 
check_valid_mem(void* ptr){
	if (ptr == NULL || !is_user_vaddr (ptr) || pml4_get_page(thread_current()->pml4, ptr) == NULL)
		return false;
	return true;
}

void
set_code_and_exit(int exit_code){
	thread_current()->exit_code = exit_code;
	thread_exit();
}

bool
check_valid_fd(int fd){
	for (int i = 0; i < 64; i++)
	{
		if (fd == i && thread_current()->fd_table[i] != NULL)
			return true;
	}
	return false;
}