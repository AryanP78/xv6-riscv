#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// setpriority(int prio, int deadline)
// Set the calling process's DAPRA priority class and deadline.
// prio: 1=BG, 2=INT, 3=RT
// deadline: absolute tick deadline (use uptime() + N)
uint64
sys_setpriority(void)
{
  int prio, deadline;
  argint(0, &prio);
  argint(1, &deadline);
  if (prio < 1 || prio > 3)
    return -1;
  struct proc *p = myproc();
  p->priority = prio;
  p->deadline = deadline;
  return 0;
}

// getstats(uint64 *faults, uint64 *evictions)
// Copy page replacement statistics to user-space pointers.
uint64
sys_getstats(void)
{
  uint64 uptr_faults, uptr_evictions;
  argaddr(0, &uptr_faults);
  argaddr(1, &uptr_evictions);
  uint64 f, e;
  pagerep_get_stats(&f, &e);
  struct proc *p = myproc();
  if (copyout(p->pagetable, uptr_faults,    (char*)&f, sizeof(f)) < 0) return -1;
  if (copyout(p->pagetable, uptr_evictions, (char*)&e, sizeof(e)) < 0) return -1;
  return 0;
}

// setpolicy(int policy)
// 0=FIFO, 1=LRU, 2=LFU, 3=DAPRA
uint64
sys_setpolicy(void)
{
  int policy;
  argint(0, &policy);
  if (policy < 0 || policy > 3)
    return -1;
  pagerep_set_policy(policy);
  pagerep_reset_stats();
  return 0;
}
