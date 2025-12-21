#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdbool.h>
#include <stddef.h>
#include "threads/synch.h"

/* Forward declarations. */
struct block;
struct bitmap;

/* Global swap state (Phase 0 skeleton). */
extern struct block *swap_block;
extern struct bitmap *swap_map;
extern struct lock swap_lock;

void swap_init (void);

/* Swap-out: write kpage to swap, returns slot index (SIZE_MAX on failure). */
size_t swap_out (const void *kpage);

/* Swap-in: read swap slot into kpage. */
bool swap_in (size_t slot, void *kpage);

/* Free a swap slot. */
void swap_free (size_t slot);

#endif /* VM_SWAP_H */