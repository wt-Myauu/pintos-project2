#include "threads/palloc.h"
#include <bitmap.h>
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "threads/loader.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "lib/kernel/list.h"

/* Buddy 설정 */
#define BUDDY_MAX_ORDER 32

struct free_block {
  struct list_elem elem;
};

/* A memory pool. */
struct pool {
  struct lock lock;        /* Mutual exclusion. */
  struct bitmap *used_map; /* Bitmap of used pages. */
  uint8_t *base;           /* Base of pool. */

  /* Next-Fit */
  size_t next_fit_cursor;

  /* Buddy */
  size_t buddy_max_order;
  struct list buddy_free[BUDDY_MAX_ORDER + 1];
};

/* Two pools: one for kernel data, one for user pages. */
static struct pool kernel_pool, user_pool;

static void init_pool (struct pool *, void *base, size_t page_cnt, const char *name);
static bool page_from_pool (const struct pool *, void *page);

/* Current allocation mode. */
static enum palloc_mode palloc_mode = PAL_FIRST_FIT;

/* ---------- Buddy helpers ---------- */

static size_t
floor_log2 (size_t x)
{
  size_t r = 0;
  while (x > 1) { x >>= 1; r++; }
  return r;
}

static size_t
ceil_log2 (size_t x)
{
  size_t p = 1, r = 0;
  while (p < x) { p <<= 1; r++; }
  return r;
}

static size_t
pow2 (size_t order)
{
  return (size_t) 1 << order;
}

static struct free_block *
idx_to_block (struct pool *pool, size_t idx)
{
  return (struct free_block *) (pool->base + idx * PGSIZE);
}

static bool
free_block_less (const struct list_elem *a,
                 const struct list_elem *b,
                 void *aux)
{
  (void) aux;
  /* list_elem 주소 = 블록 시작주소에 들어있는 elem의 주소 → 정렬 기준으로 OK */
  return a < b;
}

static void
buddy_lists_reset (struct pool *pool)
{
  size_t n = bitmap_size (pool->used_map);
  pool->buddy_max_order = floor_log2 (n);
  if (pool->buddy_max_order > BUDDY_MAX_ORDER)
    pool->buddy_max_order = BUDDY_MAX_ORDER;

  for (size_t o = 0; o <= pool->buddy_max_order; o++)
    list_init (&pool->buddy_free[o]);
}

/* [start, end) (전부 free) 구간을 buddy 블록으로 쪼개서 free list에 넣기 */
static void
buddy_add_range (struct pool *pool, size_t start, size_t end)
{
  while (start < end)
    {
      size_t o = pool->buddy_max_order;
      while (o > 0)
        {
          size_t bs = pow2 (o);
          if ((start & (bs - 1)) == 0 && start + bs <= end)
            break;
          o--;
        }

      struct free_block *b = idx_to_block (pool, start);
      list_insert_ordered (&pool->buddy_free[o], &b->elem, free_block_less, NULL);
      start += pow2 (o);
    }
}

static void
buddy_rebuild (struct pool *pool)
{
  buddy_lists_reset (pool);

  size_t n = bitmap_size (pool->used_map);
  for (size_t i = 0; i < n; )
    {
      if (bitmap_test (pool->used_map, i)) { i++; continue; }

      size_t start = i;
      while (i < n && !bitmap_test (pool->used_map, i))
        i++;

      buddy_add_range (pool, start, i);
    }
}

static size_t
buddy_alloc (struct pool *pool, size_t page_cnt)
{
  size_t n = bitmap_size (pool->used_map);
  size_t need_order = ceil_log2 (page_cnt);
  size_t need_pages = pow2 (need_order);

  if (page_cnt == 0 || need_pages > n || need_order > pool->buddy_max_order)
    return BITMAP_ERROR;

  // 핵심: order>=need_order 중에서 "가장 작은 idx" 블록을 고른다.
  size_t best_order = SIZE_MAX;
  size_t best_idx = BITMAP_ERROR;

  for (size_t o = need_order; o <= pool->buddy_max_order; o++)
    {
      if (list_empty (&pool->buddy_free[o]))
        continue;

      struct list_elem *e = list_front (&pool->buddy_free[o]);
      struct free_block *blk = list_entry (e, struct free_block, elem);
      size_t idx = ((uint8_t *) blk - pool->base) / PGSIZE;

      if (best_idx == BITMAP_ERROR || idx < best_idx)
        {
          best_idx = idx;
          best_order = o;
        }
    }

  if (best_idx == BITMAP_ERROR)
    return BITMAP_ERROR;

  // 선택된 order에서 가장 앞(=lowest idx) 블록 꺼내기
  struct list_elem *e = list_pop_front (&pool->buddy_free[best_order]);
  struct free_block *blk = list_entry (e, struct free_block, elem);
  size_t idx = ((uint8_t *) blk - pool->base) / PGSIZE;

  // 필요한 order까지 split (오른쪽 buddy를 free list로) 
  size_t o = best_order;
  while (o > need_order)
    {
      o--;
      size_t half = pow2 (o);
      size_t buddy_idx = idx + half;

      struct free_block *buddy = idx_to_block (pool, buddy_idx);
      list_insert_ordered (&pool->buddy_free[o], &buddy->elem,
                           free_block_less, NULL);
    }

  bitmap_set_multiple (pool->used_map, idx, need_pages, true);
  return idx;
}


static struct list_elem *
buddy_find_elem (struct pool *pool, size_t idx, size_t order)
{
  struct free_block *b = idx_to_block (pool, idx);
  for (struct list_elem *e = list_begin (&pool->buddy_free[order]);
       e != list_end (&pool->buddy_free[order]);
       e = list_next (e))
    {
      if (e == &b->elem)
        return e;
    }
  return NULL;
}

static void
buddy_free (struct pool *pool, size_t idx, size_t page_cnt)
{
  size_t n = bitmap_size (pool->used_map);
  size_t order = ceil_log2 (page_cnt);
  size_t pages = pow2 (order);

  ASSERT (idx + pages <= n);
  ASSERT (bitmap_all (pool->used_map, idx, pages));

  bitmap_set_multiple (pool->used_map, idx, pages, false);

  while (order < pool->buddy_max_order)
    {
      size_t buddy_idx = idx ^ pow2 (order);
      if (buddy_idx + pages > n)
        break;

      struct list_elem *be = buddy_find_elem (pool, buddy_idx, order);
      if (be == NULL)
        break;

      list_remove (be);
      idx = idx < buddy_idx ? idx : buddy_idx;
      order++;
      pages <<= 1;
    }

  struct free_block *blk = idx_to_block (pool, idx);
  list_insert_ordered (&pool->buddy_free[order], &blk->elem, free_block_less, NULL);
}


static size_t
best_fit_scan_and_mark (struct pool *pool, size_t cnt)
{
  struct bitmap *bm = pool->used_map;
  size_t n = bitmap_size (bm);

  size_t best_start = BITMAP_ERROR;
  size_t best_len = SIZE_MAX;

  if (cnt == 0 || cnt > n)
    return BITMAP_ERROR;

  for (size_t i = 0; i < n; )
    {
      if (bitmap_test (bm, i)) { i++; continue; }

      size_t start = i;
      while (i < n && !bitmap_test (bm, i))
        i++;
      size_t len = i - start;

      if (len >= cnt && len < best_len)
        {
          best_len = len;
          best_start = start;
          if (best_len == cnt)
            break;
        }
    }

  if (best_start != BITMAP_ERROR)
    bitmap_set_multiple (bm, best_start, cnt, true);

  return best_start;
}

/* ---------- Public API ---------- */

void
palloc_set_mode (enum palloc_mode mode)
{
  palloc_mode = mode;

  if (mode == PAL_NEXT_FIT)
    {
      if (kernel_pool.used_map) kernel_pool.next_fit_cursor = 0;
      if (user_pool.used_map)   user_pool.next_fit_cursor = 0;
    }

  if (mode == PAL_BUDDY)
    {
      if (kernel_pool.used_map)
        {
          lock_acquire (&kernel_pool.lock);
          buddy_rebuild (&kernel_pool);
          lock_release (&kernel_pool.lock);
        }
      if (user_pool.used_map)
        {
          lock_acquire (&user_pool.lock);
          buddy_rebuild (&user_pool);
          lock_release (&user_pool.lock);
        }
    }
}

void
palloc_init (size_t user_page_limit)
{
  uint8_t *free_start = ptov (1024 * 1024);
  uint8_t *free_end = ptov (init_ram_pages * PGSIZE);
  size_t free_pages = (free_end - free_start) / PGSIZE;
  size_t user_pages = free_pages / 2;
  size_t kernel_pages;

  if (user_pages > user_page_limit)
    user_pages = user_page_limit;
  kernel_pages = free_pages - user_pages;

  init_pool (&kernel_pool, free_start, kernel_pages, "kernel pool");
  init_pool (&user_pool, free_start + kernel_pages * PGSIZE, user_pages, "user pool");
}

void *
palloc_get_multiple (enum palloc_flags flags, size_t page_cnt)
{
  struct pool *pool = flags & PAL_USER ? &user_pool : &kernel_pool;
  void *pages;
  size_t page_idx;

  if (page_cnt == 0)
    return NULL;

  lock_acquire (&pool->lock);

  switch (palloc_mode)
    {
      case PAL_FIRST_FIT:
        page_idx = bitmap_scan_and_flip (pool->used_map, 0, page_cnt, false);
        break;

      case PAL_NEXT_FIT:
        {
          size_t n = bitmap_size (pool->used_map);

          page_idx = bitmap_scan_and_flip (pool->used_map, pool->next_fit_cursor,
                                           page_cnt, false);
          if (page_idx == BITMAP_ERROR && pool->next_fit_cursor != 0)
            page_idx = bitmap_scan_and_flip (pool->used_map, 0, page_cnt, false);

          if (page_idx != BITMAP_ERROR)
            {
              pool->next_fit_cursor = page_idx + page_cnt;
              if (pool->next_fit_cursor >= n)
                pool->next_fit_cursor = 0;
            }
          break;
        }

      case PAL_BEST_FIT:
        page_idx = best_fit_scan_and_mark (pool, page_cnt);
        break;

      case PAL_BUDDY:
        page_idx = buddy_alloc (pool, page_cnt);
        break;

      default:
        page_idx = bitmap_scan_and_flip (pool->used_map, 0, page_cnt, false);
        break;
    }

  lock_release (&pool->lock);

  pages = (page_idx != BITMAP_ERROR) ? (pool->base + PGSIZE * page_idx) : NULL;

  if (pages != NULL)
    {
      if (flags & PAL_ZERO)
        memset (pages, 0, PGSIZE * page_cnt);
    }
  else
    {
      if (flags & PAL_ASSERT)
        PANIC ("palloc_get: out of pages");
    }

  return pages;
}

void *
palloc_get_page (enum palloc_flags flags)
{
  return palloc_get_multiple (flags, 1);
}

void
palloc_free_multiple (void *pages, size_t page_cnt)
{
  struct pool *pool;
  size_t page_idx;

  ASSERT (pg_ofs (pages) == 0);
  if (pages == NULL || page_cnt == 0)
    return;

  if (page_from_pool (&kernel_pool, pages))
    pool = &kernel_pool;
  else if (page_from_pool (&user_pool, pages))
    pool = &user_pool;
  else
    NOT_REACHED ();

  page_idx = pg_no (pages) - pg_no (pool->base);

#ifndef NDEBUG
  memset (pages, 0xcc, PGSIZE * page_cnt);
#endif

  lock_acquire (&pool->lock);

  if (palloc_mode == PAL_BUDDY)
    buddy_free (pool, page_idx, page_cnt);
  else
    {
      ASSERT (bitmap_all (pool->used_map, page_idx, page_cnt));
      bitmap_set_multiple (pool->used_map, page_idx, page_cnt, false);
    }

  lock_release (&pool->lock);
}

void
palloc_free_page (void *page)
{
  palloc_free_multiple (page, 1);
}

size_t
palloc_get_page_index (void *page)
{
  struct pool *pool;

  if (page_from_pool (&kernel_pool, page))
    pool = &kernel_pool;
  else if (page_from_pool (&user_pool, page))
    pool = &user_pool;
  else
    return BITMAP_ERROR;

  return pg_no (page) - pg_no (pool->base);
}

static void
init_pool (struct pool *p, void *base, size_t page_cnt, const char *name)
{
  size_t bm_pages = DIV_ROUND_UP (bitmap_buf_size (page_cnt), PGSIZE);
  if (bm_pages > page_cnt)
    PANIC ("Not enough memory in %s for bitmap.", name);
  page_cnt -= bm_pages;

  printf ("%zu pages available in %s.\n", page_cnt, name);

  lock_init (&p->lock);
  p->used_map = bitmap_create_in_buf (page_cnt, base, bm_pages * PGSIZE);
  p->base = (uint8_t *) base + bm_pages * PGSIZE;
  p->next_fit_cursor = 0;

  /* bitmap 내용이 쓰레기일 수도 있으니 명시적으로 free로 초기화 */
  bitmap_set_all (p->used_map, false);

  /* buddy 리스트도 미리 구성(모드 전환 시 재구성도 함) */
  buddy_rebuild (p);
}

static bool
page_from_pool (const struct pool *pool, void *page)
{
  size_t page_no = pg_no (page);
  size_t start_page = pg_no (pool->base);
  size_t end_page = start_page + bitmap_size (pool->used_map);

  return page_no >= start_page && page_no < end_page;
}
