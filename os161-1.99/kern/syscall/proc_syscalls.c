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
#include <machine/trapframe.h>
#include <synch.h>
#include <proc.h>
#include <wchan.h>

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

int sys_fork(struct trapframe *tf, pid_t *retval) {
  struct proc *child = proc_create_runprogram("child process");
  if (child == NULL) {
    return ENPROC;
  }

  int err = as_copy(curproc_getas(), &child->p_addrspace);
  if (err) {
    proc_destroy(child);
    return err;
  }

  child->parentPid = curproc->pid;

  struct trapframe *childTF = kmalloc(sizeof(struct trapframe));
  if (childTF == NULL) {
    proc_destroy(child);
    return ENOMEM;
  }

  *childTF = *tf;

  err = thread_fork("child process thread", child, enter_forked_process, childTF, 0);
  if (err) {
    proc_destroy(child);
    kfree(childTF);
    childTF = NULL;
    return err;
  }

  *retval = child->pid;
  return 0;
}

void enter_forked_process(void * tf, unsigned long data2) {
  (void) data2;

  struct trapframe childTF = *((struct trapframe *) tf);
  kfree(tf);

  childTF.tf_v0 = 0;
  childTF.tf_a3 = 0;
  childTF.tf_epc += 4;

  as_activate();

  mips_usermode(&childTF);
}

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

  if (curproc->parentPid != -1 && getProc(curproc->parentPid) != NULL) {
    wchan_lock(getProc(curproc->parentPid)->procWchan);
    wchan_sleep(getProc(curproc->parentPid)->procWchan);
  }
  setProcToNull(curproc->pid);
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
  if (getProc(pid)->parentPid != curproc->pid) {
    return(ECHILD);
  }
  if (getProc(pid) == NULL) {
    return(ESRCH);
  }
  if (status == NULL) {
    return(EFAULT);
  }

  P(getProc(pid)->procSem);
  V(getProc(pid)->procSem); // in case waitpid gets called more than once after child process exited

  exitstatus = _MKWAIT_EXIT(getProc(pid)->exitCode);

  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

