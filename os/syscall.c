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

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDIN)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
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
	debugf("fork!\n");
	return fork();
}

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	copyinstr(p->pagetable, name, va, 200);
	debugf("sys_exec %s\n", name);
	return exec(name);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

// creates a new child process and loads a target program directly into it.
// Unlike fork, it doesn't need to copy the parent's memory space and replace the current process's memory with a new program
uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
	struct proc *p = curr_proc();
    char name[200];
	// copy name from user space to kernel space
    if (copyinstr(p->pagetable, name, va, 200) < 0)
        return -1;

    int id = get_id_by_name(name);
    if (id < 0) return -1; // Program not found

	// reserve a slot in the process pool
    struct proc *np = allocproc();
    if (np == 0) return -1; // Out of processes

    // Load the new program directly into the new process
    loader(id, np); 

    np->parent = p;

	// Sets the state to RUNNABLE and adds it to the task manager.
    np->state = RUNNABLE;
    add_task(np);

    return (uint64)np->pid; // Parent returns child PID
}

// allows a process to change its own priority which directly affects its scheduling "pass" value.
uint64 sys_set_priority(long long prio){
	// TODO: your job is to complete the sys call
	struct proc *p = curr_proc();

    if (prio < 2)
        return -1;


	// higher priority results in a smaller pass, meaning the stride increases more slowly, allowing the process to be selected by the scheduler more frequently.
    p->priority = (uint64)prio;
    p->pass = BIG_STRIDE / p->priority;
    
    return (uint64)prio; // Return the new priority on success
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
		ret = sys_exec(args[0]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
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