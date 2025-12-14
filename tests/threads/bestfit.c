#include "tests/threads/tests.h"
#include "threads/palloc.h"
#include "lib/debug.h"

void
test_bestfit (void)
{
  palloc_set_mode (PAL_BEST_FIT);

  enum palloc_flags F = PAL_USER | PAL_ASSERT;

  /* big hole(10)과 small hole(3)을 “big이 앞, small이 뒤”에 만들기 */
  void *big   = palloc_get_multiple (F, 10);
  size_t big_i = palloc_get_page_index (big);

  void *guard = palloc_get_page (F);                /* hole 합쳐지는 거 방지 */

  void *small = palloc_get_multiple (F, 3);
  size_t small_i = palloc_get_page_index (small);

  void *tail  = palloc_get_page (F);

  /* hole 만들기 */
  palloc_free_multiple (big, 10);
  palloc_free_multiple (small, 3);

  /* 요청 3페이지:
     First-Fit이면 big(앞 hole)로 가지만,
     Best-Fit이면 small(3짜리 hole)로 가야 함 */
  void *A = palloc_get_multiple (F, 3);
  size_t a = palloc_get_page_index (A);
  ASSERT (a == small_i);

  /* 요청 5페이지:
     이제 남은 후보는 big(10짜리)뿐이니 big으로 가야 함 */
  void *B = palloc_get_multiple (F, 5);
  size_t b = palloc_get_page_index (B);

  ASSERT (b == big_i);

  /* 정리(필수는 아니지만 깔끔하게) */
  palloc_free_multiple (A, 3);
  palloc_free_multiple (B, 5);
  palloc_free_page (guard);
  palloc_free_page (tail);
}
