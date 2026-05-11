#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "vm/page.h"
#include "vm/frame.h"

/* [Phase 5] 외부 락 참조 */
extern struct lock filesys_lock;

#define MAX_ARGC 64

/* A record belongs to its parent until wait() consumes it or the parent
   exits.  An orphaned live child owns the record until it exits. */
struct child_rec
  {
    tid_t parent;
    tid_t child;
    int exit_code;
    bool dead;
    bool waited;
    struct semaphore done;
    struct list_elem elem;
  };

struct exec_sync
  {
    struct semaphore done;
    bool success;
    char *cmdline;
    struct child_rec *child;
  };

static struct list child_table;
static struct lock child_lock;
static bool ct_inited;

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

static void
ct_init_once (void)
{
  enum intr_level old_level = intr_disable ();
  if (!ct_inited)
    {
      list_init (&child_table);
      lock_init (&child_lock);
      ct_inited = true;
    }
  intr_set_level (old_level);
}

/* The caller holds child_lock. */
static struct child_rec *
ct_find (tid_t parent, tid_t child)
{
  struct list_elem *e;
  for (e = list_begin (&child_table); e != list_end (&child_table);
       e = list_next (e))
    {
      struct child_rec *r = list_entry (e, struct child_rec, elem);
      if (r->parent == parent && r->child == child)
        return r;
    }
  return NULL;
}

static void
ct_set_exit (tid_t child, int exit_code)
{
  struct list_elem *e;
  lock_acquire (&child_lock);
  for (e = list_begin (&child_table); e != list_end (&child_table);
       e = list_next (e))
    {
      struct child_rec *r = list_entry (e, struct child_rec, elem);
      if (r->child == child)
        {
          if (r->parent == TID_ERROR)
            {
              list_remove (&r->elem);
              free (r);
            }
          else
            {
              r->exit_code = exit_code;
              r->dead = true;
              sema_up (&r->done);
            }
          break;
        }
    }
  lock_release (&child_lock);
}

static void
ct_orphan_children (tid_t parent)
{
  struct list_elem *e;
  lock_acquire (&child_lock);
  e = list_begin (&child_table);
  while (e != list_end (&child_table))
    {
      struct child_rec *r = list_entry (e, struct child_rec, elem);
      e = list_next (e);
      if (r->parent != parent)
        continue;
      if (r->dead)
        {
          list_remove (&r->elem);
          free (r);
        }
      else
        r->parent = TID_ERROR;
    }
  lock_release (&child_lock);
}

/* Creates a child and waits until loading succeeds or fails. */
tid_t
process_execute (const char *file_name)
{
  char *page;
  struct exec_sync *xs;
  struct child_rec *child;
  char prog[NAME_MAX + 1];
  char *save_ptr, *prog_name;
  tid_t tid;
  bool success;

  if (strlen (file_name) >= PGSIZE)
    return TID_ERROR;
  strlcpy (prog, file_name, sizeof prog);
  prog_name = strtok_r (prog, " \t", &save_ptr);
  if (prog_name == NULL)
    return TID_ERROR;

  ct_init_once ();
  page = palloc_get_page (0);
  if (page == NULL)
    return TID_ERROR;
  strlcpy (page, file_name, PGSIZE);
  xs = malloc (sizeof *xs);
  child = malloc (sizeof *child);
  if (xs == NULL || child == NULL)
    {
      free (xs);
      free (child);
      palloc_free_page (page);
      return TID_ERROR;
    }
  child->parent = thread_current ()->tid;
  child->child = TID_ERROR;
  child->exit_code = -1;
  child->dead = child->waited = false;
  sema_init (&child->done, 0);
  sema_init (&xs->done, 0);
  xs->success = false;
  xs->cmdline = page;
  xs->child = child;

  tid = thread_create (prog_name, PRI_DEFAULT, start_process, xs);
  if (tid == TID_ERROR)
    {
      palloc_free_page (page);
      free (child);
      free (xs);
      return TID_ERROR;
    }

  sema_down (&xs->done);
  success = xs->success;
  free (xs);
  if (!success)
    {
      process_wait (tid);
      return TID_ERROR;
    }
  return tid;
}

static void
start_process (void *aux)
{
  struct exec_sync *xs = aux;
  struct child_rec *child = xs->child;
  struct intr_frame if_;
  char *prog, *save_ptr, *prog_name;
  bool success = false;

  /* Publish status before loading: even a child that exits immediately
     must remain observable by its parent. */
  child->child = thread_current ()->tid;
  lock_acquire (&child_lock);
  list_push_back (&child_table, &child->elem);
  lock_release (&child_lock);
  thread_current ()->cmdline = xs->cmdline;
  prog = palloc_get_page (0);
  if (prog != NULL)
    {
      strlcpy (prog, xs->cmdline, PGSIZE);
      prog_name = strtok_r (prog, " \t", &save_ptr);
      memset (&if_, 0, sizeof if_);
      if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
      if_.cs = SEL_UCSEG;
      if_.eflags = FLAG_IF | FLAG_MBS;
      success = prog_name != NULL && load (prog_name, &if_.eip, &if_.esp);
      palloc_free_page (prog);
    }

  xs->success = success;
  sema_up (&xs->done);
  /* The parent may free xs as soon as it wakes. */
  if (!success)
    thread_exit ();
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits once for a direct child and consumes its exit record. */
int
process_wait (tid_t child_tid)
{
  struct child_rec *r;
  int code;
  ct_init_once ();
  lock_acquire (&child_lock);
  r = ct_find (thread_current ()->tid, child_tid);
  if (r == NULL || r->waited)
    {
      lock_release (&child_lock);
      return -1;
    }
  r->waited = true;
  lock_release (&child_lock);
  sema_down (&r->done);
  lock_acquire (&child_lock);
  code = r->exit_code;
  list_remove (&r->elem);
  lock_release (&child_lock);
  free (r);
  return code;
}

/* Release resources before waking a parent waiting for termination. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  ct_init_once ();
  ct_orphan_children (cur->tid);
  if (cur->pagedir != NULL)
    printf ("%s: exit(%d)\n", cur->name, cur->exit_code);
  if (cur->spt != NULL)
    {
      spt_destroy (cur->spt);
      free (cur->spt);
      cur->spt = NULL;
    }
  lock_acquire (&filesys_lock);
  while (!list_empty (&cur->fdt))
    {
      struct file_desc *d = list_entry (list_pop_front (&cur->fdt),
                                       struct file_desc, elem);
      file_close (d->f);
      free (d);
    }
  file_close (cur->exec_file);
  cur->exec_file = NULL;
  lock_release (&filesys_lock);
  if (cur->cmdline != NULL)
    {
      palloc_free_page (cur->cmdline);
      cur->cmdline = NULL;
    }
  pd = cur->pagedir;
  if (pd != NULL)
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
  ct_set_exit (cur->tid, cur->exit_code);
}

void
process_activate (void)
{
  struct thread *t = thread_current ();
  pagedir_activate (t->pagedir);
  tss_update ();
}

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4. */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread. */
bool
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  t->spt = malloc(sizeof *t->spt);
  if (t->spt == NULL)
    goto done;
  if (!spt_init (t->spt))
    {
      free (t->spt);
      t->spt = NULL;
      goto done;
    }

  /* [Phase 5 Fix] filesys 접근 시 락 보호 (동시성 문제 해결) */
  lock_acquire(&filesys_lock);
  file = filesys_open (file_name);
  if (file == NULL)
    {
      lock_release(&filesys_lock);
      printf ("load: %s: open failed\n", file_name);
      goto done;
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024)
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done_unlock;
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done_unlock;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done_unlock;
      file_ofs += sizeof phdr;
      switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done_unlock;
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else
                {
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done_unlock;
            }
          else
            goto done_unlock;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done_unlock;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  /* [Phase 5] 실행 파일 쓰기 방지 및 보관 */
  file_deny_write(file);
  thread_current()->exec_file = file;

  success = true;

done_unlock:
  lock_release(&filesys_lock); // 작업 완료 후 락 해제

done:
  /* 실패 시에만 여기서 닫음 (성공 시엔 exec_file에 보관) */
  if (file && !success) {
      lock_acquire(&filesys_lock);
      file_close(file);
      lock_release(&filesys_lock);
  }
  return success;
}

static bool install_page (void *upage, void *kpage, bool writable);

static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;
  if (phdr->p_memsz < phdr->p_filesz)
    return false;
  if (phdr->p_memsz == 0)
    return false;
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;
  if (phdr->p_vaddr < PGSIZE)
    return false;

  return true;
}

static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  struct thread *t = thread_current ();
  ASSERT (t->spt != NULL);

  while (read_bytes > 0 || zero_bytes > 0)
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      struct spte *sp = malloc (sizeof *sp);
      if (sp == NULL)
        return false;

      sp->vaddr = upage;
      sp->type = VM_FILE;
      sp->writable = writable;
      sp->loaded = false;

      /* 파일 포인터를 독립적으로 관리하기 위해 reopen */
      struct file *reopened_file = file_reopen(file);
      if (reopened_file == NULL) {
          free(sp);
          return false;
      }
      sp->file = reopened_file;
      sp->ofs = ofs;
      sp->read_bytes = page_read_bytes;
      sp->zero_bytes = page_zero_bytes;
      sp->swap_slot = SIZE_MAX;
      sp->frame = NULL;

      if (!spt_insert (t->spt, sp))
        {
          file_close(sp->file);
          free (sp);
          return false;
        }

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      ofs += page_read_bytes;
    }

  return true;
}

#define STACK_BOTTOM ((uint8_t *)PHYS_BASE - PGSIZE)

static bool push_bytes(void **esp, const void *src, size_t n) {
    uint8_t *new_esp = (uint8_t *)(*esp) - n;
    if (new_esp < STACK_BOTTOM) return false;
    *esp = new_esp;
    if (n == 0) return true;
    if (src) memcpy(*esp, src, n);
    else memset(*esp, 0, n);
    return true;
}

static bool push_u32(void **esp, uint32_t val) {
    return push_bytes(esp, &val, sizeof(uint32_t));
}

static bool
setup_stack (void **esp)
{
  bool success = false;

  /* [Phase 4] VM Stack Entry 생성 */
  struct spte *spte = malloc(sizeof(struct spte));
  if (!spte) return false;

  spte->vaddr = ((uint8_t *) PHYS_BASE) - PGSIZE;
  spte->type = VM_STACK;
  spte->writable = true;
  spte->loaded = true;
  spte->file = NULL;
  spte->read_bytes = 0;
  spte->zero_bytes = PGSIZE;
  spte->swap_slot = SIZE_MAX;
  spte->frame = NULL;

  if (!spt_insert(thread_current()->spt, spte)) {
      free(spte);
      return false;
  }

  /* [Phase 4/5] frame_alloc으로 스택 페이지 할당 */
  void *kpage = frame_alloc (PAL_USER | PAL_ZERO, spte);

  if (kpage != NULL)
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        {
          *esp = PHYS_BASE;

          // ----- Argument Passing -----
          char *fn_copy = palloc_get_page (0);
          if (fn_copy == NULL) return false;
          strlcpy (fn_copy, thread_current()->cmdline, PGSIZE);

          char *argv[MAX_ARGC];
          int argc = 0;
          char *save_ptr = NULL;

          for (char *tok = strtok_r(fn_copy, " \t", &save_ptr);
               tok != NULL;
               tok = strtok_r(NULL, " \t", &save_ptr))
          {
            if (argc >= MAX_ARGC) { palloc_free_page(fn_copy); return false; }
            argv[argc++] = tok;
          }

          void *arg_addr[MAX_ARGC];
          for (int i = argc - 1; i >= 0; --i) {
            size_t len = strlen(argv[i]) + 1;
            if (!push_bytes(esp, argv[i], len)) { palloc_free_page(fn_copy); return false; }
            arg_addr[i] = *esp;
          }

          size_t pad = (uintptr_t)(*esp) % 4;
          if (pad && !push_bytes(esp, NULL, pad)) {
             palloc_free_page(fn_copy);
             return false;
          }

          if (!push_u32(esp, 0)) { palloc_free_page(fn_copy); return false; }

          for (int i = argc - 1; i >= 0; --i) {
            if (!push_u32(esp, (uint32_t)(uintptr_t)arg_addr[i])) { palloc_free_page(fn_copy); return false; }
          }

          uint32_t argv_user = (uint32_t)(uintptr_t)(*esp);
          if (!push_u32(esp, argv_user)) { palloc_free_page(fn_copy); return false; }
          if (!push_u32(esp, (uint32_t)argc)) { palloc_free_page(fn_copy); return false; }
          if (!push_u32(esp, 0)) { palloc_free_page(fn_copy); return false; }

          palloc_free_page(fn_copy);
          pagedir_set_dirty (thread_current ()->pagedir, kpage, false);
          vm_frame_unpin (kpage);
        }
      else
        {
          frame_free (kpage);
        }
    }

  return success;
}

static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}