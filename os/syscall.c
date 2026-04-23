#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

#define BIG_STRIDE 1000000

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd);
uint64 sys_munmap(uint64 start, uint64 len);

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
	struct proc *p = curr_proc();
    char name[200];
    if (copyinstr(p->pagetable, name, va, 200) < 0)
        return -1;

    struct inode *ip = namei(name);
    if (ip == NULL) {
        return -1; // File not found
    }

    struct proc *np = allocproc();
    if (np == 0) {
        iput(ip); // Release inode if we can't allocate a process
        return -1;
    }

    // 2. bin_loader now expects the inode pointer, not an integer ID
    if (bin_loader(ip, np) < 0) {
        // Handle loading error (optional but recommended)
        iput(ip);
        return -1; 
    }

    // 3. Very Important: Release the inode reference after loading
    // bin_loader reads the data; iput decrements the reference count
    iput(ip);

    // Continue with Project 3 logic (Stride Scheduling initialization)
    np->parent = p;
    np->priority = 16;     // Default priority from Project 3
    np->stride = 0;       // Initial stride
    np->pass = BIG_STRIDE / np->priority;
    np->state = RUNNABLE;

	// struct file *stdin_stdout = filealloc();
	// stdin_stdout->type = FD_STDIO;
	// stdin_stdout->readable = 1;
	// stdin_stdout->writable = 1;

	// np->files[0] = idup(stdin_stdout);
	// np->files[1] = idup(stdin_stdout);
	// np->files[2] = idup(stdin_stdout);

    
    add_task(np);

    return (uint64)np->pid;
}

uint64 sys_set_priority(long long prio)
{
	// TODO: your job is to complete the sys call
	struct proc *p = curr_proc();

    if (prio < 2)
        return -1;

    p->priority = (uint64)prio;
    p->pass = BIG_STRIDE / p->priority;
    
    // According to Stride Scheduling, we usually don't reset 
    // stride to 0 here to prevent priority-change exploits, 
    // but the PDF requirement for "initial" stride is 0.
    
    return (uint64)prio; // Return the new priority on success
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

// retrieve metadata about an open file
// verify that nlink increased after linkat
int sys_fstat(int fd,uint64 stat){
	//TODO: your job is to complete the syscall
	struct proc *p = curr_proc();
    
    // Check if the file descriptor is valid
    if (fd < 0 || fd >= NFILE || p->files[fd] == NULL) {
        return -1;
    }

    struct file *f = p->files[fd];
    
    // Ensure the file is an inode (not a pipe or device if your OS differentiates)
    // If your project uses f->type, usually FD_INODE = 1 or 2
    if (f->ip == NULL) return -1;

	// populate the Stat struct with the file's metadata from the inode
    Stat st;
    st.dev = 0; // Drive number is always 0 for this lab
    st.ino = f->ip->inum;
    
    // Map internal types to the specific mode bits required by the test
    // T_DIR = 1, T_FILE = 2 (from your fs.h)
    st.mode = (f->ip->type == T_DIR) ? 0x040000 : 0x100000;
    st.nlink = f->ip->nlink;

    // Copy the struct from kernel memory to user memory
    if (copyout(p->pagetable, stat, (char *)&st, sizeof(Stat)) < 0) {
        return -1;
    }
        
    return 0;
}

// creates a hard link to a file
// not copying the file; you are creating a new name for the same inode.
int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags){
	//TODO: your job is to complete the syscall
	char name_old[MAXPATH], name_new[MAXPATH];
    struct proc *p = curr_proc();

    // Copy path strings from user space in to kernel using provided addresses
    if (copyinstr(p->pagetable, name_old, oldpath, MAXPATH) < 0) return -1;
    if (copyinstr(p->pagetable, name_new, newpath, MAXPATH) < 0) return -1;

    // Find the existing file's inode
    struct inode *ip = namei(name_old);
    if (ip == NULL) return -1;

	ivalid(ip);

    // Check if the new name already exists (Possible error mentioned in PDF)
    struct inode *check_ip = namei(name_new);
    if (check_ip != NULL) {
        iput(check_ip);
        iput(ip);
        return -1;
    }

    // In this lab, we use the root directory for all links
    struct inode *dp = root_dir(); 
    
	// Add a new entry in the target directory using dirlink.
    // Create the directory entry for the new name pointing to the old inode
    if (dirlink(dp, name_new, ip->inum) < 0) {
        iput(ip);
        iput(dp);
        return -1;
    }

    // Increment ip->nlink and call iupdate(ip) to save the change to disk
    ip->nlink++; 
    iupdate(ip); 
    
	// release the directory and inode references using iput to prevent memory leaks
    iput(ip);
    iput(dp);
    return 0;
}

// Unlink a file path to a file
int sys_unlinkat(int dirfd, uint64 name, uint64 flags){
	//TODO: your job is to complete the syscall
	struct inode *ip, *dp;
    char name_buf[DIRSIZ], path_buf[MAXPATH];
    struct proc *p = curr_proc();

    if(copyinstr(p->pagetable, path_buf, name, MAXPATH) < 0) return -1;

    // 1. Get the parent directory
    if((dp = nameiparent(path_buf, name_buf)) == 0) return -1;
    ivalid(dp);

    // Safeguard A: Don't allow unlinking "." or ".."
    if(strncmp(name_buf, ".", DIRSIZ) == 0 || strncmp(name_buf, "..", DIRSIZ) == 0){
        iunlockput(dp);
        return -1;
    }

	// find he inode and its offset in the parent directory
    uint off;
    if((ip = dirlookup(dp, name_buf, &off)) == 0){
        iunlockput(dp);
        return -1;
    }
    ivalid(ip);

    // Safeguard B: PROTECT THE ROOT and DIRECTORIES
    // Standard unlink must only work on files.
    if(ip->inum == 1 || ip->type == T_DIR){
        iunlockput(ip);
        iunlockput(dp);
        return -1; // Return error instead of deleting
    }

    // 2. Remove the directory entry
    struct dirent de;
    memset(&de, 0, sizeof(de));
    if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) panic("unlink: writei");

    iupdate(dp);
    iunlockput(dp);

    // 3. Decrement link count and sync to disk
    ip->nlink--;
    iupdate(ip);
    
    // This now safely triggers deletion ONLY for regular files
	// If that was the last name for the file and no process has it open, the blocks are freed.
    iunlockput(ip); 

    return 0;

}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_mmap:  // 222
        ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
        break;
    case SYS_munmap:  // 215
        ret = sys_munmap(args[0], args[1]);
        break;
	case SYS_setpriority: // 140
		ret = sys_set_priority(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd) {
    // 1. Basic validation
    if (len == 0) return 0; // Return directly if length is 0 
    if (len > 1024 * 1024 * 1024) return -1; // Upper limit 1GiB 
    
    // port bit 0:R, 1:W, 2:X. Other bits must be 0 
    // Also, unreadable/non-writable/non-executable memory is meaningless
    if ((port & ~0x7) != 0 || (port & 0x7) == 0) return -1;
    
    // Address must be page aligned for mappages
    if (start % PGSIZE != 0) return -1;

    struct proc *p = curr_proc();
    uint64 end = PGROUNDUP(start + len);

    // 2. Check if the virtual range is already mapped 
    for (uint64 va = start; va < end; va += PGSIZE) {
        if (walkaddr(p->pagetable, va) != 0) {
            return -1; // A page already mapped exists
        }
    }

    // 3. Define PTE flags
    int pte_flags = PTE_U; // Always set User bit
    if (port & 1) pte_flags |= PTE_R;
    if (port & 2) pte_flags |= PTE_W;
    if (port & 4) pte_flags |= PTE_X;

    // 4. Allocate physical memory and map 
    for (uint64 va = start; va < end; va += PGSIZE) {
        char *mem = kalloc();
        if (mem == 0) {
            // Insufficient physical memory 
            // Roll back previous mappings in this loop
            uvmunmap(p->pagetable, start, (va - start) / PGSIZE, 1);
            return -1;
        }
        memset(mem, 0, PGSIZE); // Zero out anonymous memory
        if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, pte_flags) != 0) {
            kfree(mem);
            uvmunmap(p->pagetable, start, (va - start) / PGSIZE, 1);
            return -1;
        }
    }

    return 0; // Success [cite: 108]
}

uint64 sys_munmap(uint64 start, uint64 len) {
    if (len == 0) return 0;
    if (start % PGSIZE != 0) return -1;

    struct proc *p = curr_proc();
    uint64 end = PGROUNDUP(start + len);

    // 1. Check if the range is fully mapped 
    for (uint64 va = start; va < end; va += PGSIZE) {
        if (walkaddr(p->pagetable, va) == 0) {
            return -1; // Unmapped virtual memory exists in range
        }
    }

    // 2. Perform unmapping and free physical pages
    uvmunmap(p->pagetable, start, (end - start) / PGSIZE, 1);
    
    return 0; // Success 
}