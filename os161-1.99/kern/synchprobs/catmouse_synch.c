#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>

/* 
 * This simple default synchronization mechanism allows only creature at a time to
 * eat.   The globalCatMouseSem is used as a a lock.   We use a semaphore
 * rather than a lock so that this code will work even before locks are implemented.
 */

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */

/*
 * replace this with declarations of any synchronization and other variables you need here
 */
volatile int numOfCatsEating, numOfMiceEating;
static struct lock **bowlLock;
static struct cv *bowlCV;

/* 
 * The CatMouse simulation will call this function once before any cat or
 * mouse tries to each.
 *
 * You can use it to initialize synchronization and other variables.
 * 
 * parameters: the number of bowls
 */
void
catmouse_sync_init(int bowls)
{
  numOfCatsEating = 0;
  numOfMiceEating = 0;

  bowlCV = kmalloc(sizeof(struct cv));
  bowlCV = cv_create("bowlCV");
  if (bowlCV == NULL) panic("could not create bowl cv");

  bowlLock = kmalloc(sizeof(struct lock) * bowls);
  int i;
  for (i = 0; i < bowls; i++) {
    bowlLock[i] = lock_create("bowlLock");
    if (bowlLock[i] == NULL) {
      panic("could not create bowl lock");
    }
  }

kprintf("---------init--------\n");
  return;
}

/* 
 * The CatMouse simulation will call this function once after all cat
 * and mouse simulations are finished.
 *
 * You can use it to clean up any synchronization and other variables.
 *
 * parameters: the number of bowls
 */
void
catmouse_sync_cleanup(int bowls)
{
  KASSERT(bowlCV != NULL);
  cv_destroy(bowlCV);
  int i;
  for (i = 0; i < bowls; i++) {
    KASSERT(bowlLock[i] != NULL);
    lock_destroy(bowlLock[i]);
  }
  kfree(bowlLock);
kprintf("--------clean--------\n");
}

void
cat_before_eating(unsigned int bowl) 
{
  KASSERT(bowlLock[bowl-1] != NULL && bowlCV != NULL);
  lock_acquire(bowlLock[bowl-1]);
int i = 0;
kprintf("---cat before eating---------bowl %d\n",bowl);
  while (numOfMiceEating != 0) {
i++;
kprintf("cat stuck #%d: numOfMiceEating=%d, bowl=%d\n",i,numOfMiceEating,bowl);
    cv_wait(bowlCV, bowlLock[bowl-1]);
  }
  numOfCatsEating++;
}

void
cat_after_eating(unsigned int bowl) 
{
  KASSERT(bowlLock[bowl-1] != NULL && bowlCV != NULL);
kprintf("---cat after eating---------bowl %d\n",bowl);
  numOfCatsEating--;
  cv_signal(bowlCV, bowlLock[bowl-1]);
  lock_release(bowlLock[bowl-1]);
}

void
mouse_before_eating(unsigned int bowl) 
{
  KASSERT(bowlLock[bowl-1] != NULL && bowlCV != NULL);
  lock_acquire(bowlLock[bowl-1]);
kprintf("---mouse before eating------bowl %d\n",bowl);
int i = 0;
  while (numOfCatsEating != 0) {
i++;
kprintf("mouse stuck #%d: numOfCatsEating=%d, bowl=%d\n",i,numOfCatsEating,bowl);
    cv_wait(bowlCV, bowlLock[bowl-1]);
  }
  numOfMiceEating++;
}

void
mouse_after_eating(unsigned int bowl) 
{
  KASSERT(bowlLock[bowl-1] != NULL && bowlCV != NULL);
kprintf("---mouse after eating------bowl %d\n",bowl);
  numOfMiceEating--;
  cv_signal(bowlCV, bowlLock[bowl-1]);
  lock_release(bowlLock[bowl-1]);
}
