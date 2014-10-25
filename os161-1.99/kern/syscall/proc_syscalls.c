#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  curproc->exitCode = exitcode;

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  V(curproc->procSem); // allow waitpid call to return

  // delay process destruction until waitpid cannot be called,
  // aka when parent process is NULL
  if (processes[curproc->parentPid] != NULL) {
    wchan_lock(processes[curproc->parentPid]->procWchan);
    wchan_sleep(processes[curproc->parentPid]->procWchan);
  }
  processes[curproc->pid] = NULL;
  // wake up all children processes from their delayed destruction
  wchan_wakeall(curproc->procWchan);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  *retval = curproc->pid;
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid, userptr_t status, int options, pid_t *retval)
{
  int exitstatus;
  int result;

  if (options != 0) {
    return(EINVAL);
  }
  if (processes[pid]->parentPid != curproc->pid) {
    return(ECHILD);
  }
  if (processes[pid] == NULL) {
    return(ESRCH);
  }
  if (status == NULL) {
    return(EFAULT);
  }

  P(processes[pid]->procSem);
  V(processes[pid]->procSem); // in case waitpid gets called more than once after child process exited

  exitstatus = _MKWAIT_EXIT(processes[pid]->exitCode);

  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

