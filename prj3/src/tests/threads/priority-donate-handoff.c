/* A lock's remaining waiters must donate to each new owner.  A
   previously awakened waiter must donate again if another thread
   acquires the lock before it gets to run. */

#include "tests/threads/tests.h"
#include "threads/synch.h"
#include "threads/thread.h"

struct handoff
  {
    struct lock lock;
    struct semaphore done;
  };

static thread_func lower_waiter;
static thread_func higher_waiter;
static void queue_waiters (struct handoff *);
static void wait_for_completion (struct handoff *);

void
test_priority_donate_handoff (void)
{
  struct handoff handoff;

  ASSERT (!thread_mlfqs);
  ASSERT (thread_get_priority () == PRI_DEFAULT);
  lock_init (&handoff.lock);
  sema_init (&handoff.done, 0);

  queue_waiters (&handoff);
  lock_release (&handoff.lock);
  wait_for_completion (&handoff);

  /* Keep running after releasing the lock, so the awakened waiter
     has to retry after our successful try-acquire. */
  queue_waiters (&handoff);
  thread_set_priority (PRI_MAX);
  lock_release (&handoff.lock);
  ASSERT (lock_try_acquire (&handoff.lock));
  thread_set_priority (PRI_MIN);
  msg ("Reacquiring holder should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 2, thread_get_priority ());
  lock_release (&handoff.lock);
  wait_for_completion (&handoff);
  thread_set_priority (PRI_DEFAULT);
}

static void
queue_waiters (struct handoff *handoff)
{
  lock_acquire (&handoff->lock);
  thread_create ("lower", PRI_DEFAULT + 1, lower_waiter, handoff);
  thread_create ("higher", PRI_DEFAULT + 2, higher_waiter, handoff);
}

static void
wait_for_completion (struct handoff *handoff)
{
  sema_down (&handoff->done);
  sema_down (&handoff->done);
}

static void
lower_waiter (void *aux)
{
  struct handoff *handoff = aux;

  lock_acquire (&handoff->lock);
  lock_release (&handoff->lock);
  sema_up (&handoff->done);
}

static void
higher_waiter (void *aux)
{
  struct handoff *handoff = aux;

  lock_acquire (&handoff->lock);
  thread_set_priority (PRI_DEFAULT - 1);
  msg ("New holder should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 1, thread_get_priority ());
  lock_release (&handoff->lock);
  sema_up (&handoff->done);
}
