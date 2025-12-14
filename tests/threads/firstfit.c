#include "tests/threads/tests.h"
#include "threads/palloc.h"
#include "lib/debug.h"   // ASSERT

void
test_firstfit (void)
{
  palloc_set_mode (PAL_FIRST_FIT);

  /* 메모리 배치(연속 할당):
     a(2) | h2(2) | guard(1) | h3(3) | tail(1)
     그리고 h2, h3를 free 해서 hole(2페이지) + hole(3페이지) 두 개 만든다. */
  void *a     = palloc_get_multiple (PAL_ASSERT, 2);
  void *h2    = palloc_get_multiple (PAL_ASSERT, 2);  // 2-page hole 후보(작음)
  void *guard = palloc_get_multiple (PAL_ASSERT, 1);  // hole 합쳐지는 거 방지
  void *h3    = palloc_get_multiple (PAL_ASSERT, 3);  // 3-page hole 후보(큼)
  void *tail  = palloc_get_multiple (PAL_ASSERT, 1);

  ASSERT (a && h2 && guard && h3 && tail);

  /* hole 2개 만들기 */
  palloc_free_multiple (h2, 2);
  palloc_free_multiple (h3, 3);

  /* (중요) 3페이지 요청:
     첫 번째 hole(h2)은 2페이지라서 못 들어가므로 스킵,
     다음으로 처음 들어가는 hole(h3)을 잡아야 First Fit. */
  void *x = palloc_get_multiple (PAL_ASSERT, 3);
  ASSERT (x == h3);

  /* 다시 free 하고 1페이지 요청:
     이제 가장 앞쪽 hole 시작(h2)로 가야 First Fit. */
  palloc_free_multiple (x, 3);
  void *y = palloc_get_multiple (PAL_ASSERT, 1);
  ASSERT (y == h2);

  /* 정리 */
  palloc_free_multiple (y, 1);
  palloc_free_multiple (a, 2);
  palloc_free_multiple (guard, 1);
  palloc_free_multiple (tail, 1);
}
