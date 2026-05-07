#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include <list.h>
#include "filesys/file.h"

/* [Phase 5] Page Type Enum 정의 수정 */
enum vm_type {
    VM_BIN,     /* Binary File (Code/Data) */
    VM_FILE,    /* General File */
    VM_MMAP,    /* Memory Mapped File */
    VM_ANON,    /* Anonymous Page (Swap 등) */
    VM_STACK,   /* Stack Page */
    VM_SWAP     /* Swapped Out Page (Load 시 상태) */
};

/* Supplemental Page Table Entry */
struct spte {
    void *vaddr;                /* Virtual Address */
    enum vm_type type;          /* Page Type */
    bool writable;              /* Writable? */
    bool loaded;                /* Is currently loaded in frame? */

    struct file *file;          /* Backing File */
    off_t ofs;                  /* Offset in file */
    size_t read_bytes;          /* Bytes to read */
    size_t zero_bytes;          /* Bytes to zero */

    size_t swap_slot;           /* Swap Slot Index (if swapped) */

    struct frame *frame;        /* Pointer to Frame (if loaded) */

    struct hash_elem helem;     /* Hash Element */
};

/* Supplemental Page Table Main Structure */
struct supplemental_page_table {
    struct hash pages;
};

/* Functions */
bool spt_init (struct supplemental_page_table *spt);
void spt_destroy (struct supplemental_page_table *spt);
struct spte *spt_find (struct supplemental_page_table *spt, void *vaddr);
bool spt_insert (struct supplemental_page_table *spt, struct spte *spte);
void spt_remove (struct supplemental_page_table *spt, struct spte *spte);
bool vm_try_stack_growth (void *addr, void *esp);
bool vm_load_page (struct spte *spte);

#endif /* vm/page.h */