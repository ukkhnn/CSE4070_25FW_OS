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

struct mapping
  {
    int id;
    void *addr;
    size_t pages;
    struct list_elem elem;
  };

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
  list_init (&spt->mappings);
  spt->next_mapid = 0;
  return hash_init (&spt->pages, spte_hash, spte_less, NULL);
}

void
spt_destroy (struct supplemental_page_table *spt)
{
  ASSERT (spt != NULL);
  while (!list_empty (&spt->mappings))
    vm_munmap (list_entry (list_front (&spt->mappings), struct mapping, elem)->id);
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

/* Each mapped page owns a reopened file handle.  Closing the original
   descriptor, removing its name, and unmapping are independent operations. */
int
vm_mmap (struct file *file, void *addr)
{
  struct thread *t = thread_current ();
  struct supplemental_page_table *spt = t->spt;
  if (file == NULL || spt == NULL || addr == NULL || pg_ofs (addr) != 0
      || !is_user_vaddr (addr))
    return -1;

  lock_acquire (&filesys_lock);
  off_t length = file_length (file);
  lock_release (&filesys_lock);
  if (length <= 0 || (size_t) length > (uintptr_t) PHYS_BASE - (uintptr_t) addr)
    return -1;
  size_t pages = ((size_t) length + PGSIZE - 1) / PGSIZE;
  for (size_t i = 0; i < pages; i++)
    if (spt_find (spt, (uint8_t *) addr + i * PGSIZE) != NULL)
      return -1;

  struct mapping *mapping = malloc (sizeof *mapping);
  if (mapping == NULL)
    return -1;
  mapping->id = spt->next_mapid++;
  mapping->addr = addr;
  mapping->pages = 0;
  list_push_back (&spt->mappings, &mapping->elem);

  for (size_t i = 0; i < pages; i++)
    {
      struct spte *page = calloc (1, sizeof *page);
      if (page == NULL)
        goto fail;
      page->vaddr = (uint8_t *) addr + i * PGSIZE;
      page->type = VM_MMAP;
      page->writable = true;
      page->ofs = i * PGSIZE;
      size_t remaining = length - page->ofs;
      page->read_bytes = remaining < PGSIZE ? remaining : PGSIZE;
      page->zero_bytes = PGSIZE - page->read_bytes;
      page->swap_slot = SIZE_MAX;
      lock_acquire (&filesys_lock);
      page->file = file_reopen (file);
      lock_release (&filesys_lock);
      if (page->file == NULL)
        {
          free (page);
          goto fail;
        }
      if (!spt_insert (spt, page))
        {
          spte_destroy_action (&page->helem, NULL);
          goto fail;
        }
      mapping->pages++;
    }
  return mapping->id;

fail:
  vm_munmap (mapping->id);
  return -1;
}

void
vm_munmap (int mapid)
{
  struct supplemental_page_table *spt = thread_current ()->spt;
  if (spt == NULL)
    return;
  for (struct list_elem *e = list_begin (&spt->mappings);
       e != list_end (&spt->mappings); e = list_next (e))
    {
      struct mapping *m = list_entry (e, struct mapping, elem);
      if (m->id != mapid)
        continue;
      for (size_t i = 0; i < m->pages; i++)
        {
          struct spte *page = spt_find (spt, (uint8_t *) m->addr + i * PGSIZE);
          if (page != NULL)
            {
              spt_remove (spt, page);
              spte_destroy_action (&page->helem, NULL);
            }
        }
      list_remove (&m->elem);
      free (m);
      return;
    }
}
