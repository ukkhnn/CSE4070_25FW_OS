#include "vm/swap.h"
#include <debug.h>
#include <stdint.h>
#include <stdbool.h>
#include "devices/block.h"
#include "lib/kernel/bitmap.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

struct block *swap_block;
struct bitmap *swap_map;
struct lock swap_lock;

/* 한 페이지(4KB)를 저장하는 데 필요한 섹터 수 (4KB / 512B = 8) */
static const size_t SECTORS_PER_PAGE = PGSIZE / BLOCK_SECTOR_SIZE;

void
swap_init (void)
{
  lock_init (&swap_lock);
  swap_block = block_get_role (BLOCK_SWAP);

  if (swap_block == NULL) {
    swap_map = NULL;
    return;
  }

  /* Swap Disk의 크기에 맞춰 비트맵 생성 */
  size_t swap_size = block_size (swap_block) / SECTORS_PER_PAGE;
  swap_map = bitmap_create (swap_size);
  if (swap_map == NULL) return;

  bitmap_set_all (swap_map, false); // 모든 슬롯을 false(free)로 초기화
}

size_t
swap_out (const void *kpage)
{
  if (!swap_block || !swap_map) return SIZE_MAX;

  lock_acquire (&swap_lock);

  /* 빈 슬롯(0)을 찾아 1로 뒤집고 인덱스 반환 */
  size_t slot = bitmap_scan_and_flip (swap_map, 0, 1, false);

  if (slot == BITMAP_ERROR) {
    lock_release (&swap_lock);
    return SIZE_MAX;
  }

  /* 해당 슬롯의 섹터들에 페이지 내용 쓰기 */
  for (size_t i = 0; i < SECTORS_PER_PAGE; i++) {
    block_write (swap_block,
                 slot * SECTORS_PER_PAGE + i,
                 kpage + (i * BLOCK_SECTOR_SIZE));
  }

  lock_release (&swap_lock);
  return slot;
}

bool
swap_in (size_t slot, void *kpage)
{
  if (!swap_block || !swap_map) return false;

  lock_acquire (&swap_lock);

  if (slot >= bitmap_size (swap_map) || !bitmap_test (swap_map, slot)) {
    lock_release (&swap_lock);
    return false; // 이미 비어있는 슬롯 접근 시 에러
  }

  /* 디스크에서 메모리로 읽어오기 */
  for (size_t i = 0; i < SECTORS_PER_PAGE; i++) {
    block_read (swap_block,
                slot * SECTORS_PER_PAGE + i,
                kpage + (i * BLOCK_SECTOR_SIZE));
  }

  /* Keep the slot until the caller installs the page successfully. */

  lock_release (&swap_lock);
  return true;
}

void
swap_free (size_t slot)
{
  if (!swap_map) return;

  lock_acquire (&swap_lock);
  if (slot < bitmap_size (swap_map) && bitmap_test (swap_map, slot)) {
    bitmap_reset (swap_map, slot);
  }
  lock_release (&swap_lock);
}