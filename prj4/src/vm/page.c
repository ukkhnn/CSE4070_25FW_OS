#include "vm/page.h"
#include "vm/swap.h"
#include <debug.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "filesys/file.h"
#include "threads/thread.h"
#include "vm/frame.h"
#include "userprog/pagedir.h"
#include "threads/synch.h"

extern struct lock filesys_lock;

static unsigned
spte_hash (const struct hash_elem *e, void *aux UNUSED)
{
  const struct spte *p = hash_entry (e, struct spte, helem);
  return hash_bytes (&p->vaddr, sizeof p->vaddr);
}

static bool
spte_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
  const struct spte *pa = hash_entry (a, struct spte, helem);
  const struct spte *pb = hash_entry (b, struct spte, helem);
  return pa->vaddr < pb->vaddr;
}

static void
spte_destroy_action (struct hash_elem *e, void *aux UNUSED)
{
  struct spte *p = hash_entry (e, struct spte, helem);
  void *kpage = frame_pin (p);
  if (kpage != NULL && p->type == VM_MMAP
      && (pagedir_is_dirty (thread_current ()->pagedir, p->vaddr)
          || pagedir_is_dirty (thread_current ()->pagedir, kpage)))
    {
      bool held = lock_held_by_current_thread (&filesys_lock);
      if (!held)
        lock_acquire (&filesys_lock);
      file_write_at (p->file, kpage, p->read_bytes, p->ofs);
      if (!held)
        lock_release (&filesys_lock);
    }
  frame_discard (p);
  if (p->swap_slot != SIZE_MAX)
    swap_free (p->swap_slot);
  if (p->file != NULL)
    {
      bool held = lock_held_by_current_thread (&filesys_lock);
      if (!held)
        lock_acquire (&filesys_lock);
      file_close (p->file);
      if (!held)
        lock_release (&filesys_lock);
    }
  free (p);
}

bool
spt_init (struct supplemental_page_table *spt)
{
  ASSERT (spt != NULL);
  return hash_init (&spt->pages, spte_hash, spte_less, NULL);
}

void
spt_destroy (struct supplemental_page_table *spt)
{
  ASSERT (spt != NULL);
  hash_destroy (&spt->pages, spte_destroy_action);
}

struct spte *
spt_find (struct supplemental_page_table *spt, void *vaddr)
{
  ASSERT (spt != NULL);
  if (vaddr == NULL)
    return NULL;
  void *va = pg_round_down (vaddr);
  struct spte temp;
  temp.vaddr = va;
  struct hash_elem *e = hash_find (&spt->pages, &temp.helem);
  return e ? hash_entry (e, struct spte, helem) : NULL;
}

bool
spt_insert (struct supplemental_page_table *spt, struct spte *spte)
{
  ASSERT (spt != NULL);
  ASSERT (spte != NULL);
  ASSERT (pg_ofs (spte->vaddr) == 0);
  return hash_insert (&spt->pages, &spte->helem) == NULL;
}

void
spt_remove (struct supplemental_page_table *spt, struct spte *spte)
{
  ASSERT (spt != NULL);
  ASSERT (spte != NULL);
  hash_delete (&spt->pages, &spte->helem);
}

bool
vm_try_stack_growth (void *addr, void *esp)
{
  if (addr == NULL || is_kernel_vaddr (addr))
    return false;
  uintptr_t stack_limit = (uintptr_t) PHYS_BASE - (1 << 23);
  if ((uintptr_t) addr < stack_limit)
    return false;
  if ((uintptr_t) addr < (uintptr_t) esp - 32)
    return false;

  struct spte *spte = malloc (sizeof (struct spte));
  if (spte == NULL)
    return false;

  spte->vaddr = pg_round_down (addr);
  spte->type = VM_STACK;
  spte->writable = true;
  spte->loaded = false;
  spte->frame = NULL;
  spte->file = NULL;
  spte->read_bytes = 0;
  spte->zero_bytes = PGSIZE;
  spte->swap_slot = SIZE_MAX;

  if (!spt_insert (thread_current ()->spt, spte))
    {
      free (spte);
      return false;
    }
  return true;
}

bool
vm_load_page (struct spte *spte)
{
  void *resident = frame_pin (spte);
  if (resident != NULL)
    {
      vm_frame_unpin (resident);
      return true;
    }
  size_t old_slot = spte->swap_slot;

  void *kpage = frame_alloc (PAL_USER, spte);
  if (kpage == NULL)
    return false;

  bool success = false;

  switch (spte->type)
    {
    case VM_SWAP:
      if (spte->swap_slot == SIZE_MAX)
        {
          success = false;
          break;
        }
      if (swap_in (spte->swap_slot, kpage))
        {
          success = true;
        }
      break;

    case VM_BIN:
    case VM_FILE:
    case VM_MMAP:
      {
        if (spte->file)
          {
            bool held = lock_held_by_current_thread (&filesys_lock);
            if (!held)
              lock_acquire (&filesys_lock);

            int read_bytes = file_read_at (spte->file, kpage, spte->read_bytes,
                                           spte->ofs);

            if (!held)
              lock_release (&filesys_lock);

            if (read_bytes != (int) spte->read_bytes)
              {
                success = false;
              }
            else
              {
                memset (kpage + spte->read_bytes, 0, spte->zero_bytes);
                success = true;
              }
          }
        break;
      }

    case VM_STACK:
    case VM_ANON:
      memset (kpage, 0, PGSIZE);
      success = true;
      break;
    }

  if (success)
    {
      if (pagedir_set_page (thread_current ()->pagedir, spte->vaddr,
                            kpage, spte->writable))
        {
          if (old_slot != SIZE_MAX)
            {
              swap_free (old_slot);
              spte->swap_slot = SIZE_MAX;
            }
          spte->loaded = true;
          pagedir_set_dirty (thread_current ()->pagedir, kpage, false);
          vm_frame_unpin (kpage);
          return true;
        }
    }

  frame_free (kpage);
  return false;
}
