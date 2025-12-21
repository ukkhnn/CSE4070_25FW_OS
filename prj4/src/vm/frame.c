#include "vm/frame.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "filesys/file.h"
#include "vm/swap.h"
#include "threads/synch.h"

static struct list frame_table;
static struct lock frame_lock;
static struct list_elem *clock_hand;

extern struct lock filesys_lock;

/* 내부 함수 선언 */
static struct frame *pick_victim (void);
static void evict_victim (struct frame *victim);


void
frame_init (void) {
    list_init (&frame_table);
    lock_init (&frame_lock);
    clock_hand = NULL;
}

/* * [Phase 5] Frame Allocation Strategy
 * 1. palloc 시도
 * 2. 실패 시 Victim 선정 (Lock 보유) -> Pinned 설정 -> Lock 해제
 * 3. Eviction (I/O, Lock 미보유)
 * 4. palloc 재시도
 * 5. 성공 시 Frame 구조체 생성 및 Pinned=true 설정 (로딩 중 보호)
 */
void *
frame_alloc (enum palloc_flags flags, struct spte *spte) {
    if ((flags & PAL_USER) == 0) return palloc_get_page (flags);

    void *kpage = palloc_get_page (flags);

    /* 메모리가 부족하면 Eviction 수행 */
    while (kpage == NULL) {
        struct frame *victim = pick_victim(); // Victim 선정 (Lock 잡았다가 핀 꽂고 풂)

        if (victim == NULL) {
            // 모든 페이지가 Pinned 상태라면 잠시 대기 후 재시도 (Busy wait 방지)
             thread_yield();
             // 극단적인 경우 여기서 PANIC할 수도 있지만, yield로 버텼다가 다시 시도
        } else {
            evict_victim(victim); // I/O 수행 (Lock 없음)
        }

        kpage = palloc_get_page (flags); // 다시 할당 시도
    }

    struct frame *f = malloc (sizeof (struct frame));
    if (f == NULL) {
        palloc_free_page (kpage);
        return NULL;
    }

    f->kpage = kpage;
    f->spte = spte;
    f->owner = thread_current ();
    f->pinned = true; // [중요] 할당 직후 로딩 중이므로 Pinned 설정
    f->is_dying = false; // [Init]
    spte->frame = f;

    lock_acquire (&frame_lock);
    list_push_back (&frame_table, &f->elem);
    lock_release (&frame_lock);

    return kpage;
}

/* [Phase 5 Fix] Race Condition 방지: Pinned 상태면 죽음 표시만 하고 리턴 */
void
frame_free (void *kpage) {
    lock_acquire (&frame_lock);
    struct list_elem *e;
    for (e = list_begin (&frame_table); e != list_end (&frame_table); e = list_next (e)) {
        struct frame *f = list_entry (e, struct frame, elem);
        if (f->kpage == kpage) {
            if (f->pinned) {
                // 현재 I/O 중이므로 구조체를 해제하지 않고 죽음 표시만 함
                f->is_dying = true;
                f->owner = NULL; // 소유권 포기
            } else {
                // 안전하게 해제
                if (clock_hand == e) {
                    clock_hand = list_next (clock_hand);
                    if (clock_hand == list_end (&frame_table))
                        clock_hand = list_begin (&frame_table);
                }
                list_remove (e);
                free (f);
            }
            break;
        }
    }
    lock_release (&frame_lock);
    palloc_free_page (kpage);
}

void
vm_frame_unpin (void *kpage) {
    lock_acquire (&frame_lock);
    struct list_elem *e = list_begin(&frame_table);
    while (e != list_end(&frame_table)) {
        struct frame *f = list_entry(e, struct frame, elem);
        if (f->kpage == kpage) {
            if (f->is_dying) {
                // 로딩 중에 프로세스가 죽었으면 뒤늦게 청소
                e = list_remove(e);
                free(f);
                // palloc_free_page는 frame_free에서 이미 수행됨
                break;
            } else {
                f->pinned = false;
                break;
            }
        }
        e = list_next(e);
    }
    lock_release (&frame_lock);
}

/* Victim을 선정하는 함수 (Frame Lock을 잡고 실행됨) */
static struct frame *
pick_victim (void) {
    lock_acquire (&frame_lock);
    struct list_elem *e = clock_hand;
    if (e == NULL || e == list_end (&frame_table)) e = list_begin (&frame_table);

    // Frame Table을 최대 2바퀴 돌면서 Victim을 찾음
    for (int i = 0; i < list_size(&frame_table) * 2; i++) {
        struct frame *f = list_entry (e, struct frame, elem);

        // 다음 검색 위치 미리 저장
        e = list_next (e);
        if (e == list_end (&frame_table)) e = list_begin (&frame_table);
        clock_hand = e;

        // [Pinned Check] 고정된 프레임은 건너뜀
        if (f->pinned || f->is_dying) continue;

        if (pagedir_is_accessed (f->owner->pagedir, f->spte->vaddr)) {
            pagedir_set_accessed (f->owner->pagedir, f->spte->vaddr, false);
        } else {
            // Victim 선정!
            f->pinned = true; // 처리 중 다른 스레드가 못 건드리게 고정
            lock_release (&frame_lock);
            return f;
        }
    }

    // Victim을 못 찾음 (모두 Pinned or Accessed)
    lock_release (&frame_lock);
    return NULL;
}

static void
evict_victim (struct frame *f) {
    if (f->is_dying || f->owner == NULL) {
        lock_acquire(&frame_lock);
        list_remove(&f->elem);
        lock_release(&frame_lock);
        free(f);
        palloc_free_page(f->kpage);
        return;
    }

    struct spte *spte = f->spte;
    bool is_dirty = pagedir_is_dirty(f->owner->pagedir, spte->vaddr);

    /* [Phase 5 Fix] VM_SWAP, VM_ANON 타입도 스왑 대상으로 처리 */
    if (spte->type == VM_STACK || spte->type == VM_SWAP || spte->type == VM_ANON) {
        size_t swap_idx = swap_out(f->kpage);
        if (swap_idx != SIZE_MAX) {
            spte->swap_slot = swap_idx;
            spte->type = VM_SWAP;
        } else {
            // Swap Full -> Eviction 취소 (Panic 방지)
            f->pinned = false;
            return;
        }
    }
    else if (spte->type == VM_FILE) {
        /* VM_FILE (Binary)은 Dirty일 때만 Swap(또는 Write-back) 필요
           Pintos에서 Binary는 보통 Read-Only지만,
           수정 가능한 Data Segment라면 Swap으로 보냄 (Write-back 구현 복잡도 회피)
        */
        if (is_dirty) {
            size_t swap_idx = swap_out(f->kpage);
            if (swap_idx != SIZE_MAX) {
                spte->swap_slot = swap_idx;
                spte->type = VM_SWAP;
            } else {
                f->pinned = false;
                return;
            }
        }
        // Dirty가 아니면 그냥 버림 (다시 로드하면 됨)
    }
    else if (spte->type == VM_MMAP) {
        if (is_dirty) {
            bool lock_held = lock_held_by_current_thread (&filesys_lock);
            if (!lock_held) lock_acquire (&filesys_lock);

            file_write_at(spte->file, f->kpage, spte->read_bytes, spte->ofs);

            if (!lock_held) lock_release (&filesys_lock);
        }
    }

    lock_acquire(&frame_lock);
    list_remove(&f->elem);
    lock_release(&frame_lock);

    if (!f->is_dying && f->owner) {
        pagedir_clear_page (f->owner->pagedir, spte->vaddr);
    }
    spte->loaded = false;
    spte->frame = NULL;

    palloc_free_page (f->kpage);
    free (f);
}