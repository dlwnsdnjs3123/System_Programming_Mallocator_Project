/*
 * mm.c - 분리 가용 리스트를 사용하는 동적 메모리 할당기
 *
 * 일반 블록은 크기와 할당 여부를 저장하는 4바이트 헤더와 푸터를 가진다.
 * 가용 블록의 payload에는 이전/다음 가용 블록 포인터를 저장하고,
 * 블록 크기에 따라 여러 가용 리스트로 나누어 관리한다. 같은 크기의 작은
 * 요청이 충분히 반복되면 같은 크기의 객체를 하나의 pool에 모아서 할당한다.
 * pool이 비면 일반 가용 리스트로 돌려보낸다. 블록을 할당할 때 남는 공간이
 * 충분하면 분할하고, 인접한 가용 블록은 바로 병합한다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

team_t team = {
    "",
    "Portfolio Maintainer",
    "",
};

/* 기본 상수와 매크로 */
#define WSIZE       4           /* 헤더와 푸터의 크기 */
#define DSIZE       8           /* 더블 워드 크기 */
#define CHUNKSIZE  (1 << 12)     /* 기본 heap 확장 크기 */
#define LISTLIMIT  4             /* 분리 가용 리스트 개수 */
#define ENDPLACE_LIMIT 64         /* 뒤쪽에 배치할 최대 payload 크기 */
#define REALLOC_BUFFER 256        /* realloc 확장 시 미리 확보할 공간 */
#define REALLOC_SPLIT  512        /* realloc 축소 시 분할 기준 */
#define POOL_TRIGGER   64         /* pool 사용을 시작할 반복 횟수 */
#define POOL_PAYLOAD   4096       /* 작은 객체 pool 하나의 크기 */
#define POOL_MAX       128        /* pool에서 처리할 최대 객체 크기 */

#define ALIGN(size) (((size) + (DSIZE - 1)) & ~0x7)
#define MINBLOCK ALIGN(DSIZE + 2 * sizeof(void *))
#define PACK(size, alloc) ((size) | (alloc))

#define GET(p) (*(unsigned int *)(p))
#define PUT(p, val) (*(unsigned int *)(p) = (val))
#define GET_SIZE(p) (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)

#define HDRP(bp) ((char *)(bp) - WSIZE)
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)))
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE((char *)(bp) - DSIZE))

#define PRED(bp) (*(void **)(bp))
#define SUCC(bp) (*(void **)((char *)(bp) + sizeof(void *)))

static char *heap_listp;
static void **seg_listp;
static void *pool_list;
static size_t watch_size1;
static size_t watch_size2;
static unsigned int watch_count1;
static unsigned int watch_count2;

static void *extend_heap(size_t words);
static void *regular_malloc(size_t size);
static void *pool_malloc(size_t size);
static void *find_pool(const void *ptr);
static void *coalesce(void *bp);
static void *find_fit(size_t asize);
static void *place(void *bp, size_t asize);
static void insert_free(void *bp);
static void remove_free(void *bp);
static size_t adjust_block_size(size_t size);
static int place_at_end(size_t asize);
static int list_index(size_t size);
static void **list_head(int index);
static int in_heap(const void *p);
static int in_free_list(const void *target);

#define POOL_SIZE_OFF   sizeof(void *)
#define POOL_FREE_OFF   ALIGN(POOL_SIZE_OFF + sizeof(unsigned int))
#define POOL_LIVE_OFF   (POOL_FREE_OFF + sizeof(void *))
#define POOL_CAP_OFF    (POOL_LIVE_OFF + sizeof(unsigned int))
#define POOL_HEADER     ALIGN(POOL_CAP_OFF + sizeof(unsigned int))

#define POOL_NEXT(pool) (*(void **)(pool))
#define POOL_SIZE(pool) (*(unsigned int *)((char *)(pool) + POOL_SIZE_OFF))
#define POOL_FREE(pool) (*(void **)((char *)(pool) + POOL_FREE_OFF))
#define POOL_LIVE(pool) (*(unsigned int *)((char *)(pool) + POOL_LIVE_OFF))
#define POOL_CAP(pool)  (*(unsigned int *)((char *)(pool) + POOL_CAP_OFF))
#define POOL_FIRST(pool) ((char *)(pool) + POOL_HEADER)

/*
 * mm_init - 빈 heap의 프롤로그, 에필로그와 가용 리스트를 초기화한다.
 */
int mm_init(void)
{
    char *start;
    size_t list_size = LISTLIMIT * sizeof(void *);
    size_t table_size = list_size;
    int i;

    if ((start = mem_sbrk((int)(table_size + 4 * WSIZE))) == (void *)-1)
        return -1;

    seg_listp = (void **)start;
    for (i = 0; i < LISTLIMIT; i++)
        *list_head(i) = NULL;

    pool_list = NULL;
    watch_size1 = watch_size2 = 0;
    watch_count1 = watch_count2 = 0;

    heap_listp = start + table_size;
    PUT(heap_listp, 0);                            /* 정렬을 위한 padding */
    PUT(heap_listp + WSIZE, PACK(DSIZE, 1));       /* 프롤로그 헤더 */
    PUT(heap_listp + 2 * WSIZE, PACK(DSIZE, 1));   /* 프롤로그 푸터 */
    PUT(heap_listp + 3 * WSIZE, PACK(0, 1));       /* 에필로그 헤더 */
    heap_listp += 2 * WSIZE;

    return 0;
}

/*
 * extend_heap - mem_sbrk로 heap을 늘리고 새 가용 블록을 만든다.
 */
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;
    if ((bp = mem_sbrk((int)size)) == (void *)-1)
        return NULL;

    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));

    return coalesce(bp);
}

/*
 * mm_malloc - 맞는 가용 블록을 찾고, 없으면 heap을 확장한다.
 */
void *mm_malloc(size_t size)
{
    size_t object_size;
    unsigned int count = 0;

    if (size == 0)
        return NULL;

    object_size = ALIGN(size);
    if (object_size <= POOL_MAX) {
        if (watch_size1 == object_size || watch_count1 == 0) {
            watch_size1 = object_size;
            count = ++watch_count1;
        } else if (watch_size2 == object_size || watch_count2 == 0) {
            watch_size2 = object_size;
            count = ++watch_count2;
        }
        if (count > POOL_TRIGGER)
            return pool_malloc(size);
    }

    return regular_malloc(size);
}

/* boundary tag를 사용하는 일반 블록을 할당한다. */
static void *regular_malloc(size_t size)
{
    size_t asize;
    size_t extendsize;
    char *bp;

    asize = adjust_block_size(size);

    if ((bp = find_fit(asize)) != NULL) {
        return place(bp, asize);
    }

    extendsize = asize <= 2048 ? CHUNKSIZE : asize;
    if ((bp = extend_heap(extendsize / WSIZE)) == NULL)
        return NULL;
    return place(bp, asize);
}

/* 반복되는 작은 객체는 같은 크기의 pool에서 할당한다. */
static void *pool_malloc(size_t size)
{
    size_t object_size = ALIGN(size);
    void *pool;
    void *object;
    char *cursor;
    unsigned int i;
    unsigned int capacity;
    size_t pool_bytes;

    for (pool = pool_list; pool != NULL; pool = POOL_NEXT(pool)) {
        if (POOL_SIZE(pool) == object_size && POOL_FREE(pool) != NULL)
            break;
    }

    if (pool == NULL) {
        capacity = (POOL_PAYLOAD - POOL_HEADER) / object_size;
        pool_bytes = POOL_HEADER + capacity * object_size;
        pool = regular_malloc(pool_bytes);
        if (pool == NULL)
            return NULL;

        POOL_NEXT(pool) = pool_list;
        POOL_SIZE(pool) = (unsigned int)object_size;
        POOL_CAP(pool) = capacity;
        POOL_LIVE(pool) = 0;
        POOL_FREE(pool) = POOL_FIRST(pool);
        cursor = POOL_FIRST(pool);
        for (i = 0; i + 1 < POOL_CAP(pool); i++, cursor += object_size)
            *(void **)cursor = cursor + object_size;
        *(void **)cursor = NULL;
        pool_list = pool;
    }

    object = POOL_FREE(pool);
    POOL_FREE(pool) = *(void **)object;
    POOL_LIVE(pool)++;
    return object;
}

static void *find_pool(const void *ptr)
{
    void *pool;
    char *first;
    char *end;
    size_t object_size;
    for (pool = pool_list; pool != NULL; pool = POOL_NEXT(pool)) {
        object_size = POOL_SIZE(pool);
        first = POOL_FIRST(pool);
        end = first + POOL_CAP(pool) * object_size;
        if ((char *)ptr >= first && (char *)ptr < end &&
            ((char *)ptr - first) % object_size == 0)
            return pool;
    }
    return NULL;
}

/*
 * mm_free - 블록을 가용 상태로 바꾸고 인접한 가용 블록과 병합한다.
 */
void mm_free(void *ptr)
{
    size_t size;
    size_t payload_size;
    void *pool;
    void *prev_pool;
    void *scan_pool;

    if (ptr == NULL)
        return;

    pool = find_pool(ptr);
    if (pool != NULL) {
        payload_size = POOL_SIZE(pool);
        if (watch_size1 == payload_size && watch_count1 > 0)
            watch_count1--;
        else if (watch_size2 == payload_size && watch_count2 > 0)
            watch_count2--;
        *(void **)ptr = POOL_FREE(pool);
        POOL_FREE(pool) = ptr;
        POOL_LIVE(pool)--;
        if (POOL_LIVE(pool) != 0)
            return;

        prev_pool = NULL;
        for (scan_pool = pool_list; scan_pool != pool;
             scan_pool = POOL_NEXT(scan_pool))
            prev_pool = scan_pool;
        if (prev_pool == NULL)
            pool_list = POOL_NEXT(pool);
        else
            POOL_NEXT(prev_pool) = POOL_NEXT(pool);
        ptr = pool;
    }

    size = GET_SIZE(HDRP(ptr));
    payload_size = size - DSIZE;
    if (watch_size1 == payload_size && watch_count1 > 0)
        watch_count1--;
    else if (watch_size2 == payload_size && watch_count2 > 0)
        watch_count2--;
    PUT(HDRP(ptr), PACK(size, 0));
    PUT(FTRP(ptr), PACK(size, 0));
    coalesce(ptr);
}

/*
 * coalesce - 새로 반환된 블록을 앞뒤의 가용 블록과 병합한다.
 */
static void *coalesce(void *bp)
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));
    void *prev_bp;
    void *next_bp;

    if (prev_alloc && next_alloc) {
        insert_free(bp);
        return bp;
    } else if (prev_alloc && !next_alloc) {
        next_bp = NEXT_BLKP(bp);
        remove_free(next_bp);
        size += GET_SIZE(HDRP(next_bp));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
    } else if (!prev_alloc && next_alloc) {
        prev_bp = PREV_BLKP(bp);
        remove_free(prev_bp);
        size += GET_SIZE(HDRP(prev_bp));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(prev_bp), PACK(size, 0));
        bp = prev_bp;
    } else {
        prev_bp = PREV_BLKP(bp);
        next_bp = NEXT_BLKP(bp);
        remove_free(prev_bp);
        remove_free(next_bp);
        size += GET_SIZE(HDRP(prev_bp)) + GET_SIZE(HDRP(next_bp));
        PUT(HDRP(prev_bp), PACK(size, 0));
        PUT(FTRP(next_bp), PACK(size, 0));
        bp = prev_bp;
    }

    insert_free(bp);
    return bp;
}

/*
 * find_fit - 크기 class별 가용 블록을 best-fit 방식으로 탐색한다.
 */
static void *find_fit(size_t asize)
{
    void *bp;
    void *best_bp;
    size_t block_size;
    size_t best_size;
    int index;

    for (index = list_index(asize); index < LISTLIMIT; index++) {
        best_bp = NULL;
        best_size = (size_t)-1;
        for (bp = *list_head(index); bp != NULL; bp = SUCC(bp)) {
            block_size = GET_SIZE(HDRP(bp));
            if (asize <= block_size && block_size < best_size) {
                best_bp = bp;
                best_size = block_size;
                if (block_size == asize)
                    return bp;
            }
        }
        if (best_bp != NULL)
            return best_bp;
    }
    return NULL;
}

/*
 * place - 가용 블록에 요청을 배치하고 남는 공간이 충분하면 분할한다.
 */
static void *place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));
    size_t remainder = csize - asize;
    void *allocated_bp = bp;

    remove_free(bp);
    if (remainder >= MINBLOCK && place_at_end(asize)) {
        PUT(HDRP(bp), PACK(remainder, 0));
        PUT(FTRP(bp), PACK(remainder, 0));
        allocated_bp = NEXT_BLKP(bp);
        PUT(HDRP(allocated_bp), PACK(asize, 1));
        PUT(FTRP(allocated_bp), PACK(asize, 1));
        insert_free(bp);
        return allocated_bp;
    }

    if (remainder >= MINBLOCK) {
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
        bp = NEXT_BLKP(bp);
        PUT(HDRP(bp), PACK(remainder, 0));
        PUT(FTRP(bp), PACK(remainder, 0));
        insert_free(bp);
    } else {
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
    return allocated_bp;
}

/*
 * insert_free - 가용 블록을 해당 크기 class 리스트의 앞에 삽입한다.
 */
static void insert_free(void *bp)
{
    void **head = list_head(list_index(GET_SIZE(HDRP(bp))));

    PRED(bp) = NULL;
    SUCC(bp) = *head;
    if (*head != NULL)
        PRED(*head) = bp;
    *head = bp;
}

/*
 * remove_free - 가용 블록을 연결 리스트에서 제거한다.
 */
static void remove_free(void *bp)
{
    void **head = list_head(list_index(GET_SIZE(HDRP(bp))));

    if (PRED(bp) != NULL)
        SUCC(PRED(bp)) = SUCC(bp);
    else
        *head = SUCC(bp);

    if (SUCC(bp) != NULL)
        PRED(SUCC(bp)) = PRED(bp);
}

static size_t adjust_block_size(size_t size)
{
    size_t asize;

    asize = ALIGN(size + DSIZE);

    return asize < MINBLOCK ? MINBLOCK : asize;
}

static int place_at_end(size_t asize)
{
    size_t payload = asize - DSIZE;

    return payload <= ENDPLACE_LIMIT && (payload & (payload - 1)) == 0;
}

/*
 * list_index - 블록 크기에 맞는 가용 리스트 번호를 구한다.
 */
static int list_index(size_t size)
{
    if (size <= 128)
        return 0;
    if (size <= 512)
        return 1;
    if (size <= CHUNKSIZE)
        return 2;
    return 3;
}

static void **list_head(int index)
{
    return (void **)((char *)seg_listp + index * sizeof(void *));
}

/*
 * mm_check - heap 블록과 명시적 가용 리스트의 일관성을 검사한다.
 * 실제 할당 과정에서는 호출하지 않고 디버깅할 때만 사용한다.
 */
int mm_check(void)
{
    void *bp;
    int index;
    int heap_free_count = 0;
    int list_free_count = 0;

    if (heap_listp == NULL)
        return 0;

    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
        if ((uintptr_t)bp % DSIZE != 0) {
            fprintf(stderr, "heap check: misaligned block %p\n", bp);
            return 0;
        }
        if (GET(HDRP(bp)) != GET(FTRP(bp))) {
            fprintf(stderr, "heap check: header/footer mismatch at %p\n", bp);
            return 0;
        }

        if (!GET_ALLOC(HDRP(bp))) {
            heap_free_count++;
            if (!GET_ALLOC(HDRP(NEXT_BLKP(bp)))) {
                fprintf(stderr, "heap check: adjacent free blocks at %p\n", bp);
                return 0;
            }
            if (!in_free_list(bp)) {
                fprintf(stderr, "heap check: free block missing from list %p\n", bp);
                return 0;
            }
        }
    }

    for (index = 0; index < LISTLIMIT; index++) {
        for (bp = *list_head(index); bp != NULL; bp = SUCC(bp)) {
            if (!in_heap(bp) || GET_ALLOC(HDRP(bp))) {
                fprintf(stderr, "heap check: invalid free-list block %p\n", bp);
                return 0;
            }
            if (list_index(GET_SIZE(HDRP(bp))) != index) {
                fprintf(stderr, "heap check: block in wrong size class %p\n", bp);
                return 0;
            }
            if (PRED(bp) != NULL && SUCC(PRED(bp)) != bp) {
                fprintf(stderr, "heap check: broken predecessor link %p\n", bp);
                return 0;
            }
            if (SUCC(bp) != NULL && PRED(SUCC(bp)) != bp) {
                fprintf(stderr, "heap check: broken successor link %p\n", bp);
                return 0;
            }
            list_free_count++;
            if (list_free_count > heap_free_count) {
                fprintf(stderr, "heap check: cycle or duplicate free block\n");
                return 0;
            }
        }
    }

    if (heap_free_count != list_free_count) {
        fprintf(stderr, "heap check: free-block counts differ\n");
        return 0;
    }
    return 1;
}

static int in_heap(const void *p)
{
    return (const char *)p >= (const char *)mem_heap_lo() &&
           (const char *)p <= (const char *)mem_heap_hi();
}

static int in_free_list(const void *target)
{
    void *bp;
    int index;

    for (index = 0; index < LISTLIMIT; index++) {
        for (bp = *list_head(index); bp != NULL; bp = SUCC(bp)) {
            if (bp == target)
                return 1;
        }
    }
    return 0;
}

/*
 * mm_realloc - 가능하면 인접한 공간을 사용해서 기존 위치에서 크기를 바꾼다.
 */
void *mm_realloc(void *ptr, size_t size)
{
    void *newptr;
    void *prev_bp;
    void *next_bp;
    void *split_bp;
    size_t asize;
    size_t target_size;
    size_t oldsize;
    size_t prevsize;
    size_t nextsize;
    size_t combined_size;
    size_t remainder;
    size_t extend_size;
    size_t old_payload_size;
    size_t copy_size;
    void *pool;

    if (ptr == NULL)
        return mm_malloc(size);

    if (size == 0) {
        mm_free(ptr);
        return NULL;
    }

    pool = find_pool(ptr);
    if (pool != NULL) {
        old_payload_size = POOL_SIZE(pool);
        if (size <= old_payload_size)
            return ptr;
        newptr = mm_malloc(size);
        if (newptr == NULL)
            return NULL;
        memcpy(newptr, ptr, old_payload_size);
        mm_free(ptr);
        return newptr;
    }

    asize = adjust_block_size(size);
    oldsize = GET_SIZE(HDRP(ptr));

    /* 크기가 줄어드는 경우에는 기존 주소를 그대로 사용할 수 있다. */
    if (asize <= oldsize) {
        remainder = oldsize - asize;
        if (remainder >= REALLOC_SPLIT) {
            PUT(HDRP(ptr), PACK(asize, 1));
            PUT(FTRP(ptr), PACK(asize, 1));
            split_bp = NEXT_BLKP(ptr);
            PUT(HDRP(split_bp), PACK(remainder, 0));
            PUT(FTRP(split_bp), PACK(remainder, 0));
            coalesce(split_bp);
        }
        return ptr;
    }

    target_size = ALIGN(asize + REALLOC_BUFFER);

    next_bp = NEXT_BLKP(ptr);
    nextsize = GET_SIZE(HDRP(next_bp));

    /* 다음 블록이 가용 상태이고 공간이 충분하면 현재 블록과 합친다. */
    if (!GET_ALLOC(HDRP(next_bp)) && oldsize + nextsize >= asize) {
        remove_free(next_bp);
        combined_size = oldsize + nextsize;
        if (target_size > combined_size)
            target_size = combined_size;
        remainder = combined_size - target_size;

        if (remainder >= MINBLOCK) {
            PUT(HDRP(ptr), PACK(target_size, 1));
            PUT(FTRP(ptr), PACK(target_size, 1));
            split_bp = NEXT_BLKP(ptr);
            PUT(HDRP(split_bp), PACK(remainder, 0));
            PUT(FTRP(split_bp), PACK(remainder, 0));
            insert_free(split_bp);
        } else {
            PUT(HDRP(ptr), PACK(combined_size, 1));
            PUT(FTRP(ptr), PACK(combined_size, 1));
        }
        return ptr;
    }

    /* 다음 가용 블록이 heap 끝에 있으면 부족한 만큼만 확장한다. */
    if (!GET_ALLOC(HDRP(next_bp)) &&
        GET_SIZE(HDRP(NEXT_BLKP(next_bp))) == 0) {
        combined_size = oldsize + nextsize;
        extend_size = target_size - combined_size;
        if (mem_sbrk((int)extend_size) != (void *)-1) {
            remove_free(next_bp);
            PUT(HDRP(ptr), PACK(target_size, 1));
            PUT(FTRP(ptr), PACK(target_size, 1));
            PUT(HDRP(NEXT_BLKP(ptr)), PACK(0, 1));
            return ptr;
        }
    }

    /* 현재 블록 바로 뒤가 에필로그이면 heap을 직접 확장한다. */
    if (nextsize == 0) {
        extend_size = target_size - oldsize;
        if (mem_sbrk((int)extend_size) != (void *)-1) {
            PUT(HDRP(ptr), PACK(target_size, 1));
            PUT(FTRP(ptr), PACK(target_size, 1));
            PUT(HDRP(NEXT_BLKP(ptr)), PACK(0, 1));
            return ptr;
        }
    }

    /* 뒤쪽 확장이 불가능하면 앞쪽의 가용 블록도 함께 사용한다. */
    prev_bp = PREV_BLKP(ptr);
    if (!GET_ALLOC(HDRP(prev_bp))) {
        prevsize = GET_SIZE(HDRP(prev_bp));
        combined_size = prevsize + oldsize;
        if (!GET_ALLOC(HDRP(next_bp)))
            combined_size += nextsize;

        if (combined_size >= asize) {
            remove_free(prev_bp);
            if (!GET_ALLOC(HDRP(next_bp)))
                remove_free(next_bp);

            old_payload_size = oldsize - DSIZE;
            copy_size = size < old_payload_size ? size : old_payload_size;
            memmove(prev_bp, ptr, copy_size);

            if (target_size > combined_size)
                target_size = combined_size;
            remainder = combined_size - target_size;
            if (remainder >= MINBLOCK) {
                PUT(HDRP(prev_bp), PACK(target_size, 1));
                PUT(FTRP(prev_bp), PACK(target_size, 1));
                split_bp = NEXT_BLKP(prev_bp);
                PUT(HDRP(split_bp), PACK(remainder, 0));
                PUT(FTRP(split_bp), PACK(remainder, 0));
                insert_free(split_bp);
            } else {
                PUT(HDRP(prev_bp), PACK(combined_size, 1));
                PUT(FTRP(prev_bp), PACK(combined_size, 1));
            }
            return prev_bp;
        }
    }

    newptr = mm_malloc(size + REALLOC_BUFFER);
    if (newptr == NULL)
        return NULL;

    old_payload_size = oldsize - DSIZE;
    copy_size = size < old_payload_size ? size : old_payload_size;
    memcpy(newptr, ptr, copy_size);
    mm_free(ptr);
    return newptr;
}
