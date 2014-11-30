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
#include <kern/fcntl.h>
#include <vfs.h>
#include <limits.h>

int sys_execv(userptr_t progname, userptr_t args)
{
	struct addrspace *as, *old_as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result, i, argc = 0;

  if (progname == NULL) return ENOENT;
  if (args == NULL) return EFAULT;

  // count argc
  char *temp = NULL;
  while (true) {
    result = copyin(args+argc*sizeof(char*), &temp, sizeof(char*));
    if (result) return result;
    if (temp == NULL) break;
    argc++;
  }

  // get args into kernel on heap
  char **kargs = kmalloc(sizeof(char*)*argc);
  unsigned int *kargs_len = kmalloc(sizeof(int)*argc);
  if (kargs == NULL) return ENOMEM;
  for (i = 0; i < argc; i++) {
    result = copyin(args+i*sizeof(char*), &temp, sizeof(char*));
    if (result) return result;
    kargs[i] = kmalloc(1024);
    result = copyinstr((const_userptr_t)temp, kargs[i], 1024, &kargs_len[i]);
    if (result) return result;
  }

  // get progname into kernal on heap
  char *name = kmalloc(PATH_MAX);
  result = copyinstr((const_userptr_t) progname, name, PATH_MAX, NULL);
  if (result || name == NULL) return result;

	/* Open the file. */
	result = vfs_open(name, O_RDONLY, 0, &v);
	if (result) {
    kfree(name);
		return result;
	}
  kfree(name);

  // destroy old address space
  as_deactivate();
  old_as = curproc_setas(NULL);
  as_destroy(old_as);
  KASSERT(curproc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as ==NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}
	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

  // copyout args as strings
  char **stack_locations = kmalloc(sizeof(char*)*argc);
  for (i = argc-1; i >= 0; i--) {
    stackptr -= kargs_len[i];
    stack_locations[i] = (char*) stackptr;
    result = copyoutstr(kargs[i], (userptr_t)stackptr, kargs_len[i], NULL);
    if (result) return result;
  }

  // has to be divisible by 4, 8's fine?
  while (stackptr%8 != 0) stackptr--;

  // copyout args as array
  stackptr -= sizeof(char*); // NULL at the end of array
  for (i = argc-1; i >= 0; i--) {
    stackptr -= sizeof(char*);
    result = copyout(&stack_locations[i], (userptr_t)stackptr, sizeof(char*));
    if (result) return result;
  }

  // argv that points to the first element of array on stack
  char **argv = (char**) stackptr;

  // kfree args and args_len
  for (i = 0; i < argc; i++) {
    kfree(kargs[i]);
    kargs[i] = NULL;
  }
  kfree(kargs);
  kargs = NULL;
  kfree(kargs_len);
  kargs_len = NULL;
  kfree(stack_locations);
  stack_locations = NULL;

	/* Warp to user mode. */
	enter_new_process(argc, (userptr_t) argv, stackptr, entrypoint);

	/* enter_new_process does not return. */
  return EINVAL;
}

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
  curproc->exitCode = _MKWAIT_EXIT(exitcode);

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

int
sys_getpid(pid_t *retval)
{
  *retval = curproc->pid;
  return(0);
}

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

  exitstatus = getProc(pid)->exitCode;

  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

