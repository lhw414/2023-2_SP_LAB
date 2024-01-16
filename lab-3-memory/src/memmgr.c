//--------------------------------------------------------------------------------------------------
// System Programming                       Memory Lab                                    Fall 2023
//
/// @file
/// @brief dynamic memory manager
/// @author Hyunwoo LEE
/// @studid 2020-12907
//--------------------------------------------------------------------------------------------------


// Dynamic memory manager
// ======================
// This module implements a custom dynamic memory manager.
//
// Heap organization:
// ------------------
// The data segment for the heap is provided by the dataseg module. A 'word' in the heap is
// eight bytes.
//
// Implicit free list:
// -------------------
// - minimal block size: 32 bytes (header +footer + 2 data words)
// - h,f: header/footer of free block
// - H,F: header/footer of allocated block
//
// - state after initialization
//
//         initial sentinel half-block                  end sentinel half-block
//                   |                                             |
//   ds_heap_start   |   heap_start                         heap_end       ds_heap_brk
//               |   |   |                                         |       |
//               v   v   v                                         v       v
//               +---+---+-----------------------------------------+---+---+
//               |???| F | h :                                 : f | H |???|
//               +---+---+-----------------------------------------+---+---+
//                       ^                                         ^
//                       |                                         |
//               32-byte aligned                           32-byte aligned
//
// - allocation policies: first, next, best fit
// - block splitting: always at 32-byte boundaries
// - immediate coalescing upon free
//

#define _GNU_SOURCE

#include <assert.h>
#include <error.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dataseg.h"
#include "memmgr.h"


/// @name global variables
/// @{
static void *ds_heap_start = NULL;                     ///< physical start of data segment
static void *ds_heap_brk   = NULL;                     ///< physical end of data segment
static void *heap_start    = NULL;                     ///< logical start of heap
static void *heap_end      = NULL;                     ///< logical end of heap
static int  PAGESIZE       = 0;                        ///< memory system page size
static void *(*get_free_block)(size_t) = NULL;         ///< get free block for selected allocation policy
static void *next_block    = NULL;                     ///< next block used by next-fit policy
static size_t CHUNKSIZE    = 1<<10;                    ///< minimal data segment allocation unit (adjust to tune performance)
static size_t SHRINKTHLD   = 1<<10;                    ///< threshold to shrink heap (implementation optional; adjust to tune performance)
static int  mm_initialized = 0;                        ///< initialized flag (yes: 1, otherwise 0)
static int  mm_loglevel    = 0;                        ///< log level (0: off; 1: info; 2: verbose)
/// @}

/// @name Macro definitions
/// @{
#define MAX(a, b)          ((a) > (b) ? (a) : (b))     ///< MAX function

#define TYPE               unsigned long               ///< word type of heap (=8 bytes)
#define TYPE_SIZE          sizeof(TYPE)                ///< size of word type

#define ALLOC              1                           ///< block allocated flag
#define FREE               0                           ///< block free flag
#define STATUS_MASK        ((TYPE)(0x7))               ///< mask to retrieve flagsfrom header/footer
#define SIZE_MASK          (~STATUS_MASK)              ///< mask to retrieve size from header/footer

#define BS                 32                          ///< minimal block size. Must be a power of 2
#define BS_MASK            (~(BS-1))                   ///< alignment mask

#define WORD(p)            ((TYPE)(p))                 ///< convert pointer to TYPE
#define PTR(w)             ((void*)(w))                ///< convert TYPE to void*

#define PREV_PTR(p)        ((p)-TYPE_SIZE)             ///< get pointer to word preceeding p
#define NEXT_PTR(p)        ((p)+TYPE_SIZE)             ///< get pointer to word succeeding p
#define HDR2FTR(p)         ((p)+GET_SIZE(p)-TYPE_SIZE) ///< get footer for given header
#define FTR2HDR(p)         ((p)-GET_SIZE(p)+TYPE_SIZE) ///< get header for given footer

#define PACK(size,status)  ((size) | (status))         ///< pack size & status into boundary tag
#define SIZE(v)            (v & SIZE_MASK)             ///< extract size from boundary tag
#define STATUS(v)          (v & STATUS_MASK)           ///< extract status from boundary tag

#define PUT(p, v)          (*(TYPE*)(p) = (TYPE)(v))   ///< write word v to *p
#define GET(p)             (*(TYPE*)(p))               ///< read word at *p
#define GET_SIZE(p)        (SIZE(GET(p)))              ///< extract size from header/footer
#define GET_STATUS(p)      (STATUS(GET(p)))            ///< extract status from header/footer


//
#define MIN(a, b)                       ((a) > (b) ? (b) : (a))                 ///< MIN FUNCTION
#define ROUND_UP(w)                     (((w)+BS-1)/BS*BS)                      ///< round up data
#define NEXT_BLK(p)                     ((p) + GET_SIZE(p))                     ///< find next block from header
#define NEXT_BLK_FROM_PAYLOAD(p)        ((p) + GET_SIZE(PREV_PTR(p)))           ///< find next block from payload
//
/// @}


/// @name Logging facilities
/// @{

/// @brief print a log message if level <= mm_loglevel. The variadic argument is a printf format
///        string followed by its parametrs
#ifdef DEBUG
  #define LOG(level, ...) mm_log(level, __VA_ARGS__)

/// @brief print a log message. Do not call directly; use LOG() instead
/// @param level log level of message.
/// @param ... variadic parameters for vprintf function (format string with optional parameters)
static void mm_log(int level, ...)
{
  if (level > mm_loglevel) return;

  va_list va;
  va_start(va, level);
  const char *fmt = va_arg(va, const char*);

  if (fmt != NULL) vfprintf(stdout, fmt, va);

  va_end(va);

  fprintf(stdout, "\n");
}

#else
  #define LOG(level, ...)
#endif

/// @}


/// @name Program termination facilities
/// @{

/// @brief print error message and terminate process. The variadic argument is a printf format
///        string followed by its parameters
#define PANIC(...) mm_panic(__func__, __VA_ARGS__)

/// @brief print error message and terminate process. Do not call directly, Use PANIC() instead.
/// @param func function name
/// @param ... variadic parameters for vprintf function (format string with optional parameters)
static void mm_panic(const char *func, ...)
{
  va_list va;
  va_start(va, func);
  const char *fmt = va_arg(va, const char*);

  fprintf(stderr, "PANIC in %s%s", func, fmt ? ": " : ".");
  if (fmt != NULL) vfprintf(stderr, fmt, va);

  va_end(va);

  fprintf(stderr, "\n");

  exit(EXIT_FAILURE);
}
/// @}


static void* ff_get_free_block(size_t);
static void* nf_get_free_block(size_t);
static void* bf_get_free_block(size_t);

void mm_init(AllocationPolicy ap)
{
  LOG(1, "mm_init()");
  //
  // set allocation policy
  //
  char *apstr;
  switch (ap) {
    case ap_FirstFit: get_free_block = ff_get_free_block; apstr = "first fit"; break;
    case ap_NextFit:  get_free_block = nf_get_free_block; apstr = "next fit";  break;
    case ap_BestFit:  get_free_block = bf_get_free_block; apstr = "best fit";  break;
    default: PANIC("Invalid allocation policy.");
  }
  LOG(2, "  allocation policy       %s\n", apstr);

  //
  // retrieve heap status and perform a few initial sanity checks
  //
  ds_heap_stat(&ds_heap_start, &ds_heap_brk, NULL);
  PAGESIZE = ds_getpagesize();

  LOG(2, "  ds_heap_start:          %p\n"
         "  ds_heap_brk:            %p\n"
         "  PAGESIZE:               %d\n",
         ds_heap_start, ds_heap_brk, PAGESIZE);

  if (ds_heap_start == NULL) PANIC("Data segment not initialized.");
  if (ds_heap_start != ds_heap_brk) PANIC("Heap not clean.");
  if (PAGESIZE == 0) PANIC("Reported pagesize == 0.");

  //
  // initialize heap
  //

  // SBRK as chuncksize
  ds_sbrk(CHUNKSIZE);
  ds_heap_stat(&ds_heap_start, &ds_heap_brk, NULL);
  // Get pagesize
  PAGESIZE = ds_getpagesize();
  // Store heap start pointer and end pointer
  heap_start = PTR((WORD(ds_heap_start) / BS + 1) * BS);
  heap_end = PTR(WORD(ds_heap_brk - TYPE_SIZE) / BS * BS);
  LOG(2, "After allocate heap       \n"
        "  ds_heap_start:          %p\n"
        "  ds_heap_brk:            %p\n"
        "  PAGESIZE:               %d\n",
        "  heap_start:             %p\n",
        "  heap_end:               %p\n",
        ds_heap_start, ds_heap_brk, PAGESIZE, heap_start, heap_end);
  // pre heap block (Initial sentinel half block)
  PUT(PREV_PTR(heap_start), PACK(0, ALLOC));
  
  // first free heap block (make header and footer for free block)
  size_t initial_free_block_size = WORD(heap_end) - WORD(heap_start);
  PUT(heap_start, PACK(initial_free_block_size, FREE));
  PUT(PREV_PTR(heap_end), PACK(initial_free_block_size, FREE));
  // post heap block (end sentinel half block)
  PUT(heap_end, PACK(0, ALLOC));

  //
  // heap is initialized
  //
  mm_initialized = 1;
}


void* mm_malloc(size_t size)
{
  LOG(1, "mm_malloc(0x%lx)", size);

  assert(mm_initialized);

  // If size is zero, return null
  if (size == 0) {
    return NULL;
  }
  // Round up size as blocksize
  size_t blocksize = ROUND_UP(TYPE_SIZE + size + TYPE_SIZE);
  // Get free block pointer
  void* free_block = get_free_block(blocksize);

  // When there's no free block, expand heap
  if (free_block == NULL) { 
    void *old_heap_end = heap_end;
    void *new_heap_end;

    // Decide how much to expand the heap by
    size_t expand_size = MAX(CHUNKSIZE, blocksize);

    // Expand heap by expand_size
    if (ds_sbrk(expand_size) == NULL) {
      return NULL; // Expansion failed
    }
    // Stroe ds heap start pointer and brk pointer
    ds_heap_stat(&ds_heap_start, &ds_heap_brk, NULL);
    // Get page size
    PAGESIZE = ds_getpagesize();
    new_heap_end = PTR(WORD(ds_heap_brk - TYPE_SIZE) / BS * BS);

    // Update heap_end
    heap_end = new_heap_end;

    // Initialize the newly allocated block
    size_t expanded_block_size = WORD(heap_end) - WORD(old_heap_end);

    // Check if old_heap_end is adjacent to a free block
    void *prev_block = PREV_PTR(old_heap_end); // Get the footer of the previous block
    if (GET_STATUS(prev_block) == FREE) { // If adjacent block is free, merge it
      size_t prev_block_size = GET_SIZE(prev_block);
      old_heap_end = PTR(WORD(old_heap_end) - prev_block_size);
      expanded_block_size += prev_block_size;
    }
    // Store header and footer info
    PUT(old_heap_end, PACK(expanded_block_size, FREE));
    PUT(HDR2FTR(old_heap_end), PACK(expanded_block_size, FREE));

    // Update the post heap block (end sentinel)
    PUT(heap_end, PACK(0, ALLOC));

    free_block = old_heap_end;
  }

  // Get free block sizse
  size_t free_block_size = GET_SIZE(free_block);

  // If free block size is bigger than block block size + 4 * type size
  if (free_block_size >= blocksize + 4 * TYPE_SIZE) {
    // Split block
    PUT(free_block, PACK(blocksize, ALLOC));
    PUT(HDR2FTR(free_block), PACK(blocksize, ALLOC));
    void *remainder = NEXT_BLK(free_block);
    PUT(remainder, PACK(free_block_size - blocksize, FREE));
    PUT(HDR2FTR(remainder), PACK(free_block_size - blocksize, FREE));
  } else { // It is better, merge small free block into allocate block
    PUT(free_block, PACK(free_block_size, ALLOC));
    PUT(HDR2FTR(free_block), PACK(free_block_size, ALLOC));
  }
  // Return payload pointer
  return (free_block + TYPE_SIZE);
}

void* mm_calloc(size_t nmemb, size_t size)
{
  LOG(1, "mm_calloc(0x%lx, 0x%lx)", nmemb, size);

  assert(mm_initialized);

  //
  // calloc is simply malloc() followed by memset()
  //
  void *payload = mm_malloc(nmemb * size);

  if (payload != NULL) memset(payload, 0, nmemb * size);

  return payload;
}

void* mm_realloc(void *ptr, size_t size)
{
  LOG(1, "mm_realloc(%p, 0x%lx)", ptr, size);

  assert(mm_initialized);

  // If prt is null, mm_malloc
  if (ptr == NULL) {
    return mm_malloc(size);
  }

  // If size is zero, mm_free
  if (size == 0) {
    mm_free(ptr);
    return NULL;
  }

  // Get original size
  size_t old_size = GET_SIZE(PREV_PTR(ptr));

  // Caculate new size
  size_t new_size = ROUND_UP(TYPE_SIZE + size + TYPE_SIZE);

  // if new size equals to old size, return ptr
  if (new_size == old_size) {
    return ptr;
  }

  // If new size is smaller than old size, downsize allocate blocks
  if (new_size < old_size) {
    // Free old size - new size
    PUT(HDR2FTR(PREV_PTR(ptr)), PACK(old_size - new_size, FREE));
    PUT(FTR2HDR(HDR2FTR(PREV_PTR(ptr))), PACK(old_size - new_size, FREE));
    // Coalesce if next block is free
    if (GET_STATUS(NEXT_PTR(HDR2FTR(PREV_PTR(ptr)))) == FREE) {
      size_t new_free_size = GET_SIZE(HDR2FTR(PREV_PTR(ptr))) + GET_SIZE(NEXT_PTR(HDR2FTR(PREV_PTR(ptr))));
      // Coalesce two free blocks
      PUT(HDR2FTR(NEXT_PTR(HDR2FTR(PREV_PTR(ptr)))), PACK(new_free_size, FREE));
      PUT(FTR2HDR(HDR2FTR(PREV_PTR(ptr))), PACK(new_free_size, FREE));
    } 
    // Allocate new size
    PUT(PREV_PTR(ptr), PACK(new_size, ALLOC));
    PUT(HDR2FTR(PREV_PTR(ptr)), PACK(new_size, ALLOC));
    return ptr;
  }

  // Check next block that possibly merging to origin block
  void *next_block = NEXT_BLK_FROM_PAYLOAD(ptr);
  size_t next_size = GET_SIZE(next_block);
  // Check next block that possibly merging to origin block
  if (GET_STATUS(next_block) == FREE && old_size + next_size >= new_size) {
    // Merge origin block to next block
    PUT(HDR2FTR(NEXT_BLK(PREV_PTR(ptr))), PACK(old_size + next_size - new_size, FREE));
    PUT(FTR2HDR(HDR2FTR(NEXT_BLK(PREV_PTR(ptr)))), PACK(old_size + next_size - new_size, FREE));
    // Set header and footer
    PUT(PREV_PTR(ptr), PACK(new_size, ALLOC));
    PUT(HDR2FTR(PREV_PTR(ptr)), PACK(new_size, ALLOC));
    return ptr;
  }

  // Allocate new block
  void *new_ptr = mm_malloc(size);
  if (new_ptr == NULL) {
    return NULL;
  }

  // Copy data from older one
  size_t copy_size = old_size - TYPE_SIZE;
  memcpy(new_ptr, ptr, copy_size);

  // Free original block
  mm_free(ptr);
  // Return new payload pointer
  return new_ptr;
}

void mm_free(void *ptr)
{
  LOG(1, "mm_free(%p)", ptr);

  assert(mm_initialized);
  // Get head pointer
  void* head_ptr = PREV_PTR(ptr);

  // If ptr is null, return
  if (ptr == NULL) {
    return;
  }

  // If already free, return
  if (GET_STATUS(head_ptr) == FREE) {
    return;
  }

  // Retrieve the size of the block to be freed
  size_t size = (size_t) GET_SIZE(head_ptr);

  // Mark the block as free
  PUT(head_ptr, PACK(size, FREE));
  PUT(HDR2FTR(head_ptr), PACK(size, FREE));

  // If the previous block is free, coalesce
  if (GET_STATUS(PREV_PTR(head_ptr)) == FREE) {
    size += GET_SIZE(PREV_PTR(head_ptr));
    PUT(FTR2HDR(PREV_PTR(head_ptr)), PACK(size, FREE));
    PUT(HDR2FTR(head_ptr), PACK(size, FREE));
    // Set new header pointer
    head_ptr = FTR2HDR(PREV_PTR(head_ptr));
  }

  // If the next block is free, coalesce
  if (GET_STATUS(NEXT_BLK(head_ptr)) == FREE) {
    size += GET_SIZE(NEXT_BLK(head_ptr));
    PUT(head_ptr, PACK(size, FREE));
    PUT(HDR2FTR(head_ptr), PACK(size, FREE));
  }

  // Check if this is the last block in the heap and size > CHUNKSIZE
  if (NEXT_BLK(head_ptr) == heap_end && size >= SHRINKTHLD) {
    // Perform heap shrink
    ds_sbrk(-size);
    ds_heap_stat(&ds_heap_start, &ds_heap_brk, NULL);
    PAGESIZE = ds_getpagesize();
    // Update heap_end if you maintain it
    heap_end -= size;
    // Update end sentinel half-block
    PUT(heap_end, PACK(0, ALLOC));
  }
}

/// @name block allocation policites
/// @{

/// @brief find and return a free block of at least @a size bytes (first fit)
/// @param size size of block (including header & footer tags), in bytes
/// @retval void* pointer to header of large enough free block
/// @retval NULL if no free block of the requested size is avilable
static void* ff_get_free_block(size_t size)
{
  LOG(1, "ff_get_free_block(1x%lx (%lu))", size, size);

  assert(mm_initialized);

  void *current_block = heap_start;
  // Until heap end, find free block
  while(current_block < heap_end) { 
    if (!GET_STATUS(current_block) && GET_SIZE(current_block) >= size) { // If there's a free block, return its pointer
      return current_block;
    }
    // If not free block, travel next block
    current_block += GET_SIZE(current_block);
  }

  return NULL;
}

/// @brief find and return a free block of at least @a size bytes (next fit)
/// @param size size of block (including header & footer tags), in bytes
/// @retval void* pointer to header of large enough free block
/// @retval NULL if no free block of the requested size is avilable
static void* nf_get_free_block(size_t size)
{
  LOG(1, "nf_get_free_block(0x%x (%lu))", size, size);

  assert(mm_initialized);
  // If next block is null or over heap end, set next block as heap start
  if (next_block == NULL || next_block >= heap_end) {
    next_block = heap_start;
  }
  // Set initial next block as next block
  void *initial_next_block = next_block;
  // Until traveling 1 cycle, find free block
  for(;;) {
    // If there's free block wihch size is bigger than request one, return it's pointer
    if (!GET_STATUS(next_block) && GET_SIZE(next_block) >= size) {
      return next_block;
    }
    // If next block is allocate or small size, travel next block
    next_block += GET_SIZE(next_block);
    // If next block is over heap end, set as heap start
    if (next_block >= heap_end) {
      next_block = heap_start;
    }
    // If travel 1 cycle, break
    if (next_block == initial_next_block) {
      break;
    }
  }
  // If there's no free block, return null
  return NULL;
}

/// @brief find and return a free block of at least @a size bytes (best fit)
/// @param size size of block (including header & footer tags), in bytes
/// @retval void* pointer to header of large enough free block
/// @retval NULL if no free block of the requested size is avilable
static void* bf_get_free_block(size_t size)
{
  LOG(1, "bf_get_free_block(0x%lx (%lu))", size, size);

  assert(mm_initialized);
  // Memory for pointer of best fit block
  void *best_fit_block = NULL;
  // Store smallest diff
  size_t smallest_diff = SIZE_MAX;
  // Start at heap start point
  void *current_block = heap_start;
  // Until heap end, travel blocks
  while(current_block < heap_end) {
    if (!GET_STATUS(current_block)) { // If current block is free block, check its size
      size_t current_size = GET_SIZE(current_block);
      if (current_size >= size) { // Check it has enough size.
        size_t diff = current_size - size; // Check diff
        if (diff < smallest_diff) {// If current diff is smaller than smallest diff, update best fit block and smallest diff
          best_fit_block = current_block;
          smallest_diff = diff;
          if (smallest_diff == 0) { // If smallest diff is zero, return its pointer
            break;
          }
        }
      }
    }
    current_block += GET_SIZE(current_block);
  }
  // return best fit block pointer
  return best_fit_block;
}

/// @}

void mm_setloglevel(int level)
{
  mm_loglevel = level;
}


void mm_check(void)
{
  assert(mm_initialized);

  void *p;
  char *apstr;
  if (get_free_block == ff_get_free_block) apstr = "first fit";
  else if (get_free_block == nf_get_free_block) apstr = "next fit";
  else if (get_free_block == bf_get_free_block) apstr = "best fit";
  else apstr = "invalid";

  printf("----------------------------------------- mm_check ----------------------------------------------\n");
  printf("  ds_heap_start:          %p\n", ds_heap_start);
  printf("  ds_heap_brk:            %p\n", ds_heap_brk);
  printf("  heap_start:             %p\n", heap_start);
  printf("  heap_end:               %p\n", heap_end);
  printf("  allocation policy:      %s\n", apstr);
  printf("  next_block:             %p\n", next_block);

  printf("\n");
  p = PREV_PTR(heap_start);
  printf("  initial sentinel:       %p: size: %6lx (%7ld), status: %s\n",
         p, GET_SIZE(p), GET_SIZE(p), GET_STATUS(p) == ALLOC ? "allocated" : "free");
  p = heap_end;
  printf("  end sentinel:           %p: size: %6lx (%7ld), status: %s\n",
         p, GET_SIZE(p), GET_SIZE(p), GET_STATUS(p) == ALLOC ? "allocated" : "free");
  printf("\n");
  printf("  blocks:\n");

  printf("    %-14s  %8s  %10s  %10s  %8s  %s\n", "address", "offset", "size (hex)", "size (dec)", "payload", "status");

  long errors = 0;
  p = heap_start;
  while (p < heap_end) {
    char *ofs_str, *size_str;

    TYPE hdr = GET(p);
    TYPE size = SIZE(hdr);
    TYPE status = STATUS(hdr);

    if (asprintf(&ofs_str, "0x%lx", p-heap_start) < 0) ofs_str = NULL;
    if (asprintf(&size_str, "0x%lx", size) < 0) size_str = NULL;
    printf("    %p  %8s  %10s  %10ld  %8ld  %s\n",
           p, ofs_str, size_str, size, size-2*TYPE_SIZE, status == ALLOC ? "allocated" : "free");

    free(ofs_str);
    free(size_str);

    void *fp = p + size - TYPE_SIZE;
    TYPE ftr = GET(fp);
    TYPE fsize = SIZE(ftr);
    TYPE fstatus = STATUS(ftr);

    if ((size != fsize) || (status != fstatus)) {
      errors++;
      printf("    --> ERROR: footer at %p with different properties: size: %lx, status: %lx\n", 
             fp, fsize, fstatus);
      mm_panic("mm_check");
    }

    p = p + size;
    if (size == 0) {
      printf("    WARNING: size 0 detected, aborting traversal.\n");
      break;
    }
  }

  printf("\n");
  if ((p == heap_end) && (errors == 0)) printf("  Block structure coherent.\n");
  printf("-------------------------------------------------------------------------------------------------\n");
}
