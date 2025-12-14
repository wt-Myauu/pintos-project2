#include "tests/threads/tests.h"
#include "threads/palloc.h"
#include "lib/debug.h"

void
test_buddy (void)
{
  palloc_set_mode (PAL_BUDDY);
  enum palloc_flags F = PAL_USER | PAL_ASSERT;

  void *A = palloc_get_multiple (F, 4);
  size_t a = palloc_get_page_index (A);
  msg ("Allocated A at index %zu", a);

  void *B = palloc_get_multiple (F, 4);
  size_t b = palloc_get_page_index (B);
  msg ("Allocated B at index %zu", b);

  // ck가 0, 4를 기대하니 검증 
  ASSERT (a == 0);
  ASSERT (b == 4);

  palloc_free_multiple (A, 4);
  palloc_free_multiple (B, 4);
}
