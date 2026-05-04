#include "vm/frame.h"
#include <debug.h>
#include "threads/thread.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include "vm/swap.h"

static struct list frame_table;
static struct lock frame_lock;
static struct condition frame_changed;
static struct list_elem *clock_hand;

extern struct lock filesys_lock;

static struct frame *find_frame (void *kpage);
static void release_frame (struct frame *f);
static struct frame *pick_victim (void);
static bool evict_victim (struct frame *f);

void
frame_init (void)
{
  list_init (&frame_table);
  lock_init (&frame_lock);
  cond_init (&frame_changed);
  clock_hand = NULL;
}

void *
frame_alloc (enum palloc_flags flags, struct spte *spte)
{
  ASSERT ((flags & PAL_USER) != 0);
  ASSERT (spte != NULL);

  /* Allocation failure must reach the caller instead of spinning forever
     when every frame is pinned or the swap device is full. */
  lock_acquire (&frame_lock);
  size_t attempts = list_size (&frame_table);
  lock_release (&frame_lock);
  void *kpage = palloc_get_page (flags);
  while (kpage == NULL && attempts-- > 0)
    {
      struct frame *victim = pick_victim ();
      if (victim == NULL)
        break;
      evict_victim (victim);
      kpage = palloc_get_page (flags);
    }
  if (kpage == NULL)
    return NULL;

  struct frame *f = malloc (sizeof *f);
  if (f == NULL)
    {
      palloc_free_page (kpage);
      return NULL;
    }
  f->kpage = kpage;
  f->spte = spte;
  f->owner = thread_current ();
  f->pinned = true;
  f->evicting = false;

  lock_acquire (&frame_lock);
  spte->frame = f;
  list_push_back (&frame_table, &f->elem);
  lock_release (&frame_lock);
  return kpage;
}

/* All frame-table helpers below require frame_lock. */
static struct frame *
find_frame (void *kpage)
{
  ASSERT (lock_held_by_current_thread (&frame_lock));
  for (struct list_elem *e = list_begin (&frame_table);
       e != list_end (&frame_table); e = list_next (e))
    {
      struct frame *f = list_entry (e, struct frame, elem);
      if (f->kpage == kpage)
        return f;
    }
  return NULL;
}

static void
release_frame (struct frame *f)
{
  ASSERT (lock_held_by_current_thread (&frame_lock));
  if (clock_hand == &f->elem)
    clock_hand = list_next (&f->elem);
  list_remove (&f->elem);
  if (list_empty (&frame_table))
    clock_hand = NULL;
  if (f->owner->pagedir != NULL)
    pagedir_clear_page (f->owner->pagedir, f->spte->vaddr);
  f->spte->frame = NULL;
  f->spte->loaded = false;
  palloc_free_page (f->kpage);
  free (f);
  cond_broadcast (&frame_changed, &frame_lock);
}

/* Discard a page while keeping its SPT entry alive until any outstanding
   eviction finishes.  Process teardown must use this before freeing SPTE. */
void
frame_discard (struct spte *spte)
{
  lock_acquire (&frame_lock);
  while (spte->frame != NULL && spte->frame->evicting)
    cond_wait (&frame_changed, &frame_lock);
  if (spte->frame != NULL)
    release_frame (spte->frame);
  lock_release (&frame_lock);
}

/* Used to abandon a frame that the current page loader still has pinned. */
void
frame_free (void *kpage)
{
  lock_acquire (&frame_lock);
  struct frame *f = find_frame (kpage);
  if (f != NULL)
    {
      ASSERT (!f->evicting);
      release_frame (f);
    }
  lock_release (&frame_lock);
}

/* Atomically wait for eviction and pin an already resident page. */
void *
frame_pin (struct spte *spte)
{
  lock_acquire (&frame_lock);
  while (spte->frame != NULL && spte->frame->evicting)
    cond_wait (&frame_changed, &frame_lock);
  void *kpage = NULL;
  if (spte->loaded && spte->frame != NULL)
    {
      spte->frame->pinned = true;
      kpage = spte->frame->kpage;
    }
  lock_release (&frame_lock);
  return kpage;
}

void
vm_frame_unpin (void *kpage)
{
  lock_acquire (&frame_lock);
  struct frame *f = find_frame (kpage);
  if (f != NULL)
    {
      ASSERT (!f->evicting);
      f->pinned = false;
      cond_broadcast (&frame_changed, &frame_lock);
    }
  lock_release (&frame_lock);
}

static struct frame *
pick_victim (void)
{
  lock_acquire (&frame_lock);
  size_t scans = list_size (&frame_table) * 2;
  while (scans-- > 0)
    {
      if (clock_hand == NULL || clock_hand == list_end (&frame_table))
        clock_hand = list_begin (&frame_table);
      struct frame *f = list_entry (clock_hand, struct frame, elem);
      clock_hand = list_next (clock_hand);
      if (f->pinned)
        continue;
      if (pagedir_is_accessed (f->owner->pagedir, f->spte->vaddr))
        pagedir_set_accessed (f->owner->pagedir, f->spte->vaddr, false);
      else
        {
          f->pinned = true;
          f->evicting = true;
          /* Prevent the owner from changing the page during backing-store I/O. */
          pagedir_clear_page (f->owner->pagedir, f->spte->vaddr);
          lock_release (&frame_lock);
          return f;
        }
    }
  lock_release (&frame_lock);
  return NULL;
}

static bool
evict_victim (struct frame *f)
{
  struct spte *spte = f->spte;
  bool dirty = pagedir_is_dirty (f->owner->pagedir, spte->vaddr)
               || pagedir_is_dirty (f->owner->pagedir, f->kpage);
  bool success = true;

  switch (spte->type)
    {
    case VM_BIN:
    case VM_FILE:
      if (!dirty)
        break;
      /* A modified executable page becomes private anonymous state. */
      /* fall through */
    case VM_STACK:
    case VM_ANON:
    case VM_SWAP:
      {
        size_t slot = swap_out (f->kpage);
        success = slot != SIZE_MAX;
        if (success)
          {
            spte->swap_slot = slot;
            spte->type = VM_SWAP;
          }
        break;
      }
    case VM_MMAP:
      if (dirty)
        {
          bool held = lock_held_by_current_thread (&filesys_lock);
          if (!held)
            lock_acquire (&filesys_lock);
          success = file_write_at (spte->file, f->kpage, spte->read_bytes,
                                   spte->ofs) == (int) spte->read_bytes;
          if (!held)
            lock_release (&filesys_lock);
        }
      break;
    }

  lock_acquire (&frame_lock);
  if (success)
    release_frame (f);
  else
    {
      /* The page table still exists after clearing its present bit. */
      bool restored = pagedir_set_page (f->owner->pagedir, spte->vaddr,
                                        f->kpage, spte->writable);
      ASSERT (restored);
      pagedir_set_dirty (f->owner->pagedir, spte->vaddr, dirty);
      pagedir_set_accessed (f->owner->pagedir, spte->vaddr, true);
      f->evicting = false;
      f->pinned = false;
      cond_broadcast (&frame_changed, &frame_lock);
    }
  lock_release (&frame_lock);
  return success;
}
