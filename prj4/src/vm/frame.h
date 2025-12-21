#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/palloc.h"
#include "threads/synch.h"
#include <list.h>
#include "vm/page.h"

/* Physical Frame 구조체 */
struct frame {
    void *kpage;               // 커널 가상 주소 (물리 메모리)
    struct spte *spte;         // 이 프레임을 점유 중인 페이지 테이블 엔트리
    struct thread *owner;      // 프레임 소유 스레드
    struct list_elem elem;     // frame_table 리스트 연결용
    bool pinned;               // [Phase 5 Fix] Eviction 방지 플래그
    bool is_dying;             // [Phase 5 Fix] 종료 중 플래그
};

void frame_init (void);
void *frame_alloc (enum palloc_flags flags, struct spte *spte);
void frame_free (void *kpage);
void evict_frame (void);
void vm_frame_unpin (void *kpage); // [Phase 5 Fix] 프로토타입 추가

#endif /* vm/frame.h */