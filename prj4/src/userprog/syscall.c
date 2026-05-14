#include "userprog/syscall.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "lib/kernel/console.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "vm/frame.h"
#include "vm/page.h"

struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);
static void sys_exit (int status) NO_RETURN;

static struct file_desc *
fd_lookup (int fd)
{
  if (fd < 2)
    return NULL;
  struct list *files = &thread_current ()->fdt;
  for (struct list_elem *e = list_begin (files); e != list_end (files);
       e = list_next (e))
    {
      struct file_desc *d = list_entry (e, struct file_desc, elem);
      if (d->fd == fd)
        return d;
    }
  return NULL;
}

static void
sys_exit (int status)
{
  thread_current ()->exit_code = status;
  thread_exit ();
  NOT_REACHED ();
}

static bool
validate_range (const void *addr, size_t size)
{
  uintptr_t start = (uintptr_t) addr;
  if (size != 0 && (start == 0 || start >= (uintptr_t) PHYS_BASE
                   || size > (uintptr_t) PHYS_BASE - start))
    return false;
  return true;
}

static struct spte *
user_page (const void *addr, bool write)
{
  struct thread *t = thread_current ();
  if (!validate_range (addr, 1) || t->spt == NULL)
    return NULL;
  struct spte *page = spt_find (t->spt, (void *) addr);
  if (page == NULL && vm_try_stack_growth ((void *) addr, t->tf.esp))
    page = spt_find (t->spt, (void *) addr);
  if (page == NULL || (write && !page->writable))
    return NULL;
  return page;
}

/* Check the entire destination before reading a file or consuming input.
   SPT entries stay owned by this process even if their frames are evicted. */
static bool
validate_destination (void *buffer, size_t size)
{
  while (size != 0)
    {
      if (user_page (buffer, true) == NULL)
        return false;
      size_t chunk = PGSIZE - pg_ofs (buffer);
      if (chunk > size) chunk = size;
      buffer = (uint8_t *) buffer + chunk;
      size -= chunk;
    }
  return true;
}

/* User buffers are copied while pinned and outside filesys_lock.  A file
   operation therefore cannot fault while holding a lock needed by paging. */
static void *
pin_user_page (const void *addr, bool write)
{
  struct spte *page = user_page (addr, write);
  if (page == NULL)
    return NULL;
  for (;;)
    {
      void *kpage = frame_pin (page);
      if (kpage != NULL)
        return kpage;
      if (!vm_load_page (page))
        return NULL;
    }
}

static bool
copy_user (void *kernel, void *user, size_t size, bool to_user)
{
  if (!validate_range (user, size))
    return false;
  while (size != 0)
    {
      size_t chunk = PGSIZE - pg_ofs (user);
      if (chunk > size) chunk = size;
      void *kpage = pin_user_page (user, to_user);
      if (kpage == NULL) return false;
      void *mapped = (uint8_t *) kpage + pg_ofs (user);
      if (to_user)
        {
          memcpy (mapped, kernel, chunk);
          pagedir_set_dirty (thread_current ()->pagedir, user, true);
        }
      else
        memcpy (kernel, mapped, chunk);
      pagedir_set_accessed (thread_current ()->pagedir, user, true);
      vm_frame_unpin (kpage);
      user = (uint8_t *) user + chunk;
      kernel = (uint8_t *) kernel + chunk;
      size -= chunk;
    }
  return true;
}

static char *
copy_string (const char *user)
{
  char *copy = palloc_get_page (0);
  if (copy == NULL)
    return NULL;
  for (size_t i = 0; i < PGSIZE; i++)
    {
      if (!copy_user (copy + i, (void *) (user + i), 1, false))
        {
          palloc_free_page (copy);
          sys_exit (-1);
        }
      if (copy[i] == '\0')
        return copy;
    }
  palloc_free_page (copy);
  sys_exit (-1);
}

static int
argument (const void *esp, unsigned index)
{
  int value;
  if (!copy_user (&value, (uint8_t *) esp + sizeof value * index,
                  sizeof value, false))
    sys_exit (-1);
  return value;
}

static int
sys_open (const char *user)
{
  char *name = copy_string (user);
  if (name == NULL)
    return -1;
  lock_acquire (&filesys_lock);
  struct file *file = name[0] != '\0' ? filesys_open (name) : NULL;
  lock_release (&filesys_lock);
  palloc_free_page (name);
  if (file == NULL)
    return -1;
  struct thread *t = thread_current ();
  struct file_desc *d = malloc (sizeof *d);
  if (d == NULL || t->next_fd == INT_MAX)
    {
      lock_acquire (&filesys_lock);
      file_close (file);
      lock_release (&filesys_lock);
      free (d);
      return -1;
    }
  d->fd = t->next_fd++;
  d->f = file;
  list_push_back (&t->fdt, &d->elem);
  return d->fd;
}

static void
sys_close (int fd)
{
  struct file_desc *d = fd_lookup (fd);
  if (d == NULL)
    return;
  lock_acquire (&filesys_lock);
  file_close (d->f);
  lock_release (&filesys_lock);
  list_remove (&d->elem);
  free (d);
}

static int
sys_io (int fd, void *buffer, unsigned size, bool write)
{
  if (!validate_range (buffer, size)) sys_exit (-1);
  struct file_desc *d = fd_lookup (fd);
  if ((write && fd == 0) || (!write && fd == 1)
      || (fd > 1 && d == NULL) || fd < 0)
    return -1;
  if (size == 0)
    return 0;
  if (!write && !validate_destination (buffer, size))
    sys_exit (-1);
  uint8_t *copy = palloc_get_page (0);
  if (copy == NULL)
    return -1;
  unsigned done = 0;
  while (done < size)
    {
      unsigned chunk = size - done;
      if (chunk > PGSIZE) chunk = PGSIZE;
      int count;
      if (write && !copy_user (copy, (uint8_t *) buffer + done, chunk, false))
        {
          palloc_free_page (copy);
          sys_exit (-1);
        }
      if (fd == 1)
        {
          putbuf ((char *) copy, chunk);
          count = chunk;
        }
      else if (fd == 0)
        {
          for (unsigned i = 0; i < chunk; i++) copy[i] = input_getc ();
          count = chunk;
        }
      else
        {
          lock_acquire (&filesys_lock);
          count = write ? file_write (d->f, copy, chunk)
                        : file_read (d->f, copy, chunk);
          lock_release (&filesys_lock);
        }
      if (count <= 0) break;
      if (!write && !copy_user (copy, (uint8_t *) buffer + done, count, true))
        {
          palloc_free_page (copy);
          sys_exit (-1);
        }
      done += count;
      if ((unsigned) count < chunk) break;
    }
  palloc_free_page (copy);
  return done;
}

int
sys_fibonacci (int n)
{
  if (n < 0) return -1;
  if (n == 0) return 0;
  uint32_t a = 0, b = 1;
  for (int i = 2; i <= n; i++)
    {
      uint32_t next = a + b;
      a = b;
      b = next;
    }
  return (int) b;
}

int
sys_max_of_four_int (int a, int b, int c, int d)
{
  int value = a;
  if (b > value) value = b;
  if (c > value) value = c;
  if (d > value) value = d;
  return value;
}

void
syscall_init (void)
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f)
{
  thread_current ()->tf = *f;
  int no = argument (f->esp, 0);
  /* Fetch in explicit statements so argument access order is predictable. */
  int a, b, c;
  switch (no)
    {
    case SYS_HALT:
      shutdown_power_off ();
      NOT_REACHED ();
    case SYS_EXIT:
      sys_exit (argument (f->esp, 1));
    case SYS_EXEC:
      {
        const char *user = (void *) argument (f->esp, 1);
        char *command = copy_string (user);
        f->eax = command != NULL ? process_execute (command) : -1;
        if (command != NULL) palloc_free_page (command);
        break;
      }
    case SYS_WAIT:
      f->eax = process_wait (argument (f->esp, 1));
      break;
    case SYS_OPEN:
      f->eax = sys_open ((void *) argument (f->esp, 1));
      break;
    case SYS_CREATE:
    case SYS_REMOVE:
      {
        const char *user = (void *) argument (f->esp, 1);
        unsigned size = no == SYS_CREATE ? (unsigned) argument (f->esp, 2) : 0;
        char *name = copy_string (user);
        bool success = false;
        if (name != NULL)
          {
            lock_acquire (&filesys_lock);
            success = name[0] != '\0' && size <= INT_MAX
                      && (no == SYS_CREATE ? filesys_create (name, size)
                                           : filesys_remove (name));
            lock_release (&filesys_lock);
            palloc_free_page (name);
          }
        f->eax = success;
        break;
      }
    case SYS_READ:
    case SYS_WRITE:
      a = argument (f->esp, 1);
      b = argument (f->esp, 2);
      c = argument (f->esp, 3);
      f->eax = sys_io (a, (void *) b, (unsigned) c, no == SYS_WRITE);
      break;
    case SYS_CLOSE:
      sys_close (argument (f->esp, 1));
      break;
    case SYS_FILESIZE:
    case SYS_SEEK:
    case SYS_TELL:
      {
        struct file_desc *d = fd_lookup (argument (f->esp, 1));
        unsigned position = no == SYS_SEEK ? (unsigned) argument (f->esp, 2) : 0;
        f->eax = -1;
        if (d != NULL && position <= INT_MAX)
          {
            lock_acquire (&filesys_lock);
            if (no == SYS_FILESIZE) f->eax = file_length (d->f);
            else if (no == SYS_TELL) f->eax = file_tell (d->f);
            else file_seek (d->f, position);
            lock_release (&filesys_lock);
          }
        break;
      }
    case SYS_MMAP:
      {
        struct file_desc *d = fd_lookup (argument (f->esp, 1));
        void *addr = (void *) argument (f->esp, 2);
        f->eax = vm_mmap (d != NULL ? d->f : NULL, addr);
        break;
      }
    case SYS_MUNMAP:
      vm_munmap (argument (f->esp, 1));
      break;
    case SYS_FIBONACCI:
      f->eax = sys_fibonacci (argument (f->esp, 1));
      break;
    case SYS_MAX_OF_FOUR_INT:
      a = argument (f->esp, 1);
      b = argument (f->esp, 2);
      c = argument (f->esp, 3);
      f->eax = sys_max_of_four_int (a, b, c, argument (f->esp, 4));
      break;
    default:
      sys_exit (-1);
    }
}
