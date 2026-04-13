#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

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
      lock_acquire (&filesys_lock);
      success = prog_name != NULL && load (prog_name, &if_.eip, &if_.esp);
      lock_release (&filesys_lock);
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

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
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

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
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

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
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

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL)
    {
      printf ("load: %s: open failed\n", file_name);
      goto done;
  }
  // [추가] 실행 중인 파일은 수정할 수 없도록 막는다.
  //file_deny_write(file);

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
      goto done;
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
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
          goto done;
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
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  // [phase5] 로드 성공 확정 → 실행 파일 핸들 보관 + 쓰기 금지
  thread_current()->exec_file = file;
  file_deny_write(file);
  //

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */

  //[phase5-edit] 성공이면 exec_file로 보관 중이니 닫지 않는다.
  if (file && !success)
    file_close(file);
  return success;
}
/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

//[add] - 지역 헬퍼 2개
// 스택 바닥(한 페이지) 경계 계산용
#define STACK_BOTTOM ((uint8_t *)PHYS_BASE - PGSIZE)

// 바이트 n개 push (언더플로우 가드 포함)
static bool push_bytes(void **esp, const void *src, size_t n) {
    uint8_t *new_esp = (uint8_t *)(*esp) - n;
    if (new_esp < STACK_BOTTOM) return false;
    *esp = new_esp;
    if (n == 0) return true;
    if (src) memcpy(*esp, src, n);
    else memset(*esp, 0, n);
    return true;
}

// 32비트 값 하나 push (예: NULL, argc, fake RA 등)
static bool push_u32(void **esp, uint32_t val) {
    return push_bytes(esp, &val, sizeof(uint32_t));
}
//[add] end


/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */

// ---------- setup_stack 완성본 ----------
static bool
setup_stack (void **esp)
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL)
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        {
          *esp = PHYS_BASE;

          // ----- Argument Passing 시작 -----
          // 1) cmdline 복사
          char *fn_copy = palloc_get_page (0);
          if (fn_copy == NULL) return false;
          strlcpy (fn_copy, thread_current()->cmdline, PGSIZE);

          // 2) 토큰화 (공백/탭)
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

          // 인자 없을 수도 있음(args-none) → 괜찮음. (argc==0)

          // 3) 문자열 역순으로 push 하면서 시작주소 저장
          void *arg_addr[MAX_ARGC];
          for (int i = argc - 1; i >= 0; --i) {
            size_t len = strlen(argv[i]) + 1; // '\0' 포함
            if (!push_bytes(esp, argv[i], len)) { palloc_free_page(fn_copy); return false; }
            arg_addr[i] = *esp; // 이 문자열의 시작주소
          }

          // 4) 4바이트 정렬(워드 얼라인)
          size_t pad = (uintptr_t)(*esp) % 4;
          if (pad && !push_bytes(esp, NULL, pad)) {
             palloc_free_page(fn_copy);
             return false;
            }

          // 5) NULL sentinel
          if (!push_u32(esp, 0)) { palloc_free_page(fn_copy); return false; }

          // 6) argv 포인터들(역순으로 push했으니 역순으로 주소 push)
          for (int i = argc - 1; i >= 0; --i) {
            if (!push_u32(esp, (uint32_t)(uintptr_t)arg_addr[i])) { palloc_free_page(fn_copy); return false; }
          }

          // 7) argv의 주소, argc, fake return address(0)
          uint32_t argv_user = (uint32_t)(uintptr_t)(*esp);
          if (!push_u32(esp, argv_user)) { palloc_free_page(fn_copy); return false; }   // argv
          if (!push_u32(esp, (uint32_t)argc)) { palloc_free_page(fn_copy); return false; } // argc
          if (!push_u32(esp, 0)) { palloc_free_page(fn_copy); return false; }              // fake RA

          // 8) 디버깅용 덤프 (필요 없으면 주석)

          /*
          #ifdef AP_DEBUG
          printf("[AP] stack dump begin\n");
          hex_dump((uintptr_t)*esp, *esp, 128, true);
          printf("[AP] stack dump end\n");
          #endif
          */

          palloc_free_page(fn_copy);
          // ----- Argument Passing 끝 -----
        }
      else
        palloc_free_page (kpage);
    }
  return success;
}
// -------------------------------------------

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
