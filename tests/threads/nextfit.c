#include "tests/threads/tests.h"
#include "threads/palloc.h"
#include "lib/debug.h"

void
test_nextfit (void)
{
  palloc_set_mode (PAL_NEXT_FIT);

  enum palloc_flags F = PAL_USER | PAL_ASSERT;

  void *A = palloc_get_page (F);
  size_t a = palloc_get_page_index (A);

  void *B = palloc_get_page (F);
  size_t b = palloc_get_page_index (B);

  // A를 free해도 Next-Fit이면 cursor가 B 다음을 가리키므로,
  //   다시 A(앞쪽 hole)로 돌아가지 않고 다음 위치를 줘야 함. 
  palloc_free_page (A);

  void *C = palloc_get_page (F);
  size_t c = palloc_get_page_index (C);


  ASSERT (b == a + 1);
  ASSERT (c == b + 1);

  //free
  palloc_free_page (B);
  palloc_free_page (C);
}
