#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h" // is_user_vaddr()
#include "threads/palloc.h"
#include <string.h>
#include "filesys/file.h"
#include "userprog/pagedir.h"
#include "vm/page.h"   // struct spte, spt_find(), VM_FILE 등

#include "userprog/syscall.h"
#include "threads/synch.h"   // lock_acquire, lock_release를 위해 필수
#include "vm/frame.h"


/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

//[prj4-phase0]
/* VM: Phase 0 placeholder. Phase 2+ will implement real logic. */
static bool handle_mm_fault (void *fault_addr, bool user, bool write, void *esp);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void)
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void)
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f)
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */

  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      thread_exit ();

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel");

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      thread_exit ();
    }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f)
{
  bool not_present;
  bool write;
  bool user;
  void *fault_addr;

  asm ("movl %%cr2, %0" : "=r" (fault_addr));
  intr_enable ();
  page_fault_cnt++;

  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  /* 1. 권한 위반 처리 */
  if (!not_present)
    {
      /* exit(-1) 대신 Pintos 종료 방식 사용 */
      thread_current ()->exit_code = -1;
      thread_exit ();
    }

  /* 2. Kernel Mode 접근 오류 */
  if (user && is_kernel_vaddr (fault_addr))
    {
      thread_current ()->exit_code = -1;
      thread_exit ();
    }

  /* 3. ESP 구하기 */
  /* 주의: thread.h에 tf를 추가해야 컴파일 됨 */
  void *esp = (user) ? f->esp : thread_current ()->tf.esp;

  /* 4. VM Handling */
  struct thread *curr = thread_current ();
  struct spte *spte = spt_find (curr->spt, fault_addr);

  if (spte != NULL)
    {
      if (!handle_mm_fault (fault_addr, user, write, esp))
      {
        thread_current ()->exit_code = -1;
        thread_exit ();
      }
    }
  else
    {
      if (vm_try_stack_growth (fault_addr, esp))
        {
          if (!handle_mm_fault (fault_addr, user, write, esp))
          {
            thread_current ()->exit_code = -1;
            thread_exit ();
          }
        }
      else
        {
          /* Stack Growth 실패 -> Segfault */
          thread_current ()->exit_code = -1;
          thread_exit ();
        }
    }
}


/* handle_mm_fault 함수 전체 교체 */
static bool
handle_mm_fault (void *fault_addr, bool user UNUSED, bool write, void *esp UNUSED)
{
    struct thread *curr = thread_current ();
    void *va = pg_round_down (fault_addr);

    /* 1. 유효한 사용자 주소인지 확인 */
    if (!is_user_vaddr (fault_addr)) return false;

    /* 2. SPTE 찾기 */
    struct spte *spte = spt_find (curr->spt, va);

    if (spte == NULL) return false;

    /* 3. 권한 검사 (Write access to Read-Only page) */
    if (write && !spte->writable) return false;

    /* 4. 이미 로드된 상태라면 (MMU 업데이트 누락 등) 재설정 시도 */
    if (spte->loaded) {
        // 이미 로드된 페이지에 또 fault가 났다면 pagedir 문제일 수 있음.
        // vm_load_page가 이를 처리하도록 넘김.
        return vm_load_page(spte);
    }

    /* 5. [핵심] 모든 로딩 로직은 vm/page.c의 vm_load_page에 위임! */
    // 여기에 lock 관련 코드를 직접 쓰지 마세요. vm_load_page가 다 해줍니다.
    return vm_load_page (spte);
}