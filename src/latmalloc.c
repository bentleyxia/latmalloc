#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
// TODO: remove before release

#define lat_calloc calloc
#define lat_free free
#define lat_malloc malloc
#define lat_memalign memalign
#define lat_realloc realloc
#define lat_mallopt mallopt

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

void *lat_malloc(size_t size);
void *lat_calloc(size_t nmemb, size_t size);
void *lat_realloc(void *oldmem, size_t size);
void *lat_memalign(size_t alignment, size_t size);
void lat_free(void *ptr);
int lat_mallopt(int param_number, int value);
void *lat_mmap(size_t size);
void *lat_munmap(void *addr, size_t size);
extern void *mremap(void *__addr, size_t __old_len, size_t __new_len,
                    int __flags, ...);

#define ONLY_MSPACES 1
#define MSPACES 1
// Use custom mmap to have the record of mmapped object
#define MMAP(s) lat_mmap(s)
#define MUNMAP(a, s) lat_munmap(a, s)
#define DIRECT_MMAP(s) lat_mmap(s)
#define DIRECT_MUNMAP(a, s) lat_munmap(a, s)
#define HAVE_MORECORE 0
// Direct MMAP when syscall mmap is needed
#define DEFAULT_MMAP_THRESHOLD ((size_t)0)
#define DEFAULT_GRANULARITY ((size_t)128U * 1024U)
#define MALLOC_ALIGNMENT 64

#include "dlmalloc.c"

#undef MMAP
#undef MUNMAP
#undef DIRECT_MMAP
#undef DIRECT_MUNMAP
#undef DEFAULT_GRANULARITY
#ifdef MAP_POPULATE
#undef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#define TEMP_SIZE                                                              \
  (4 * 1024 * 1024L) // temp mspace capacity before initialization

static int lat_rank;
static int lat_rank_size;
static int lat_mem_size;
int pagesize;

// Forbid custom mmap allocate memory
bool mmap_enabled = false;

static void __lat_init();
static void __param_init();

static intptr_t phy;

int create_mmap_region(int64_t size, bool is_base);
// lat virtual -> system virtual memory addres
static inline void *virt_to_phy(intptr_t virt) { return (void *)(virt + phy); }
// remote the mapped file in /dev/shm
static void __lat_destroy() { unlink("/dev/shm/lattice.IHEPCC"); }

// global metadata of the lat memory allocator
struct lat_segment {
  int8_t flag;    // Flag if initialized
  intptr_t limit; // End address of lattice
  intptr_t brk;   // Next available address of lattice
};

struct mmap_record {
  int fd;
  int64_t size;
};

// TODO: Max mmap record size is 100
struct mmap_record mmap_record[100];

// metadata of mmapped region
static struct lat_segment *lat_segment = NULL;
// dlmalloc metadata for memory allocation
static mspace lat_mspace;
// mmap_record for memory mapping

void *lat_malloc(size_t size) {
  if (unlikely(lat_segment == NULL)) {
    __lat_init();
  }
  return mspace_malloc(lat_mspace, size);
}

void *lat_realloc(void *oldmem, size_t size) {
  if (unlikely(lat_segment == NULL)) {
    __lat_init();
  }
  return mspace_realloc(lat_mspace, oldmem, size);
}

void *lat_calloc(size_t nmemb, size_t size) {
  if (unlikely(lat_segment == NULL)) {
    __lat_init();
  }
  return mspace_calloc(lat_mspace, nmemb, size);
}

void *lat_memalign(size_t alignment, size_t size) {
  if (unlikely(lat_segment == NULL)) {
    __lat_init();
  }
  return mspace_memalign(lat_mspace, alignment, size);
}

void lat_free(void *mem) { return mspace_free(lat_mspace, mem); }

void *lat_mmap(size_t size) {
  void *pointer;
  int fd = create_mmap_region(size, false);
  pointer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pointer == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }
  // not base, so close fd
  close(fd);
  // TODO: need bookkeeping of mmap record
  return pointer;
}

void *lat_munmap(void *pointer, size_t size) {
  munmap(pointer, size);
  return NULL;
}

int lat_mallopt(int param_number, int value) {
  return change_mparam(param_number, value);
}

/**
 * @brief Create a mmap region for shared memory.
 *
 * @param size mmap region size
 * @param is_base check if the region is the base of the shared memory
 * @return int file descriptor of the mmap region
 *
 */
int create_mmap_region(int64_t size, bool is_base) {
  int fd;

  char file_template[24];
  file_template[23] = '\0';

  // If base, create a rendezvous file. Else create randomized temp file.
  if (is_base) {

    strcpy(file_template, "/dev/shm/lattice.IHEPCC");
    fd = open(file_template, O_RDWR, 0600);
    atexit(__lat_destroy);
  } else {
    strcpy(file_template, "/dev/shm/lattice.XXXXXX");
    fd = mkstemp(file_template);

    // close the file descriptor, and if not the base region, unlink the
    // file.(fd is used by mmap, so it is defer closed)
    unlink(file_template);
  }

  // fd = mkstemp(file_template);
  if (fd < 0) {
    fd = open(file_template, O_RDWR | O_CREAT, 0666);

    if (fd < 0) {
      fprintf(stderr, "create_mmap_buffer: mkstemp failed\n");
      return -1;
    }
  }

  if (size) {
    // Increase the size of the file to the desired size. This seems not to
    // be needed for files that are backed by the huge page fs, see also
    // http: //
    // www.mail-archive.com/kvm-devel@lists.sourceforge.net/msg14737.html
    if (ftruncate(fd, (off_t)size) != 0) {
      fprintf(stderr, "create_mmap_buffer: ftruncate failed\n");
      return -1;
    }
  }

  // TODO: Leave traces in the system, if not may be cause unsafety.
  // if (unlink(&file_template[0]) != 0) {
  //   fprintf(stderr, "create_mmap_buffer: unlink failed\n");
  //   return -1;
  // }

  return fd;
}

/**
 * @brief Initialize parameters for lat memory allocator.
 *        - LAT_RANK is stand for the current process used by lattice. (if not
 * MPI, it is 0)
 *        - LAT_RANK_SIZE is the number of processes used by lattice. (if not
 * MPI, it is 1)
 *        - LAT_MEM_SIZE is the total size of memory used by lattice. (should
 * set by environment variable)
 *
 */
static void __param_init() {
  lat_rank = getenv("LAT_RANK") ? atoi(getenv("LAT_RANK")) : 0;
  lat_rank_size = getenv("LAT_RANK_SIZE") ? atoi(getenv("LAT_RANK_SIZE")) : 1;
  lat_mem_size =
      getenv("LAT_MEM_SIZE") ? atoi(getenv("LAT_MEM_SIZE")) : 1073741824;
}

/**
 * @brief Per process called function to initialize lat memory allocator.
 *
 * If memory allocator not initialized, it will create a shared memory region
 * in /dev/shm. Else, it will use the existing shared memory region. At the
 * start of the region, use lat_segment record the meta data of the region,
 * including flag, brk and limit. Because of ASLR and protection mechanics, the
 * virtual address of different processes will randomized. So, all the processes
 * that use lat memory allocator should be rebased via another virtual address
 * that start with 0 here. So the totally memory translation view is
 *
 * lat virtual address -> system virtual address -> physical address.
 *
 * The memory is divided into several segments that stand
 * for different processes.
 *
 */
static void __lat_init() {
  __param_init();
  // temporary space for the mspace for malloc()
  char *tmp_mspace = (char *)alloca(TEMP_SIZE);
  lat_mspace = create_mspace_with_base(tmp_mspace, TEMP_SIZE, 0);

#ifdef USE_MPI
  MPI_Comm_rank(MPI_COMM_WORLD, &lat_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &lat_size);

  if (lat_rank == 0) {
    int fd = create_mmap_region(0);
    if (fd < 0) {
      fprintf(stderr, "create_mmap_buffer: create_mmap_region failed\n");
      exit(1);
    }
    lat_segment = (lat_segment *)mmap(
        NULL, sizeof(lat_segment), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (lat_segment == (void *)MAP_FAILED) {
      fprintf(stderr, "create_mmap_buffer: mmap failed\n");
      exit(1);
    }
    close(fd);
  }
#endif

  // check illegal memory size
  if (!lat_mem_size) {
    fprintf(stderr, "LAT_MEM_SIZE is not set.\n");
    exit(1);
  }
  // different process use same fd to recognize the same memory region,
  int fd = create_mmap_region(lat_mem_size, true);
  if (fd < 0) {
    fprintf(stderr, "create_mmap_buffer: create_mmap_region failed,\n please "
                    "check ${LAT_MEM_SIZE} setting \n");
    exit(1);
  }
  // allocate the memory with lat_mem_size
  lat_segment = (struct lat_segment *)mmap(
      NULL, lat_mem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (lat_segment == (void *)MAP_FAILED) {
    fprintf(stderr, "create_mmap_buffer: mmap failed\n");
    exit(1);
  }

  // offset is the memory that lat_segment comsumes
  pagesize = getpagesize();
  int offset = (sizeof(struct lat_segment) / pagesize + 1) * pagesize;

  // TODO: need to solve remaining memory
  int interval = (lat_mem_size - offset) / lat_rank_size;

  if (!lat_segment->flag) {

    // from now on, lat memory allocater can allocate memory using brk
    lat_segment->brk = (intptr_t)offset;
    lat_segment->limit = (intptr_t)lat_mem_size;
    lat_segment->flag = 1;
  }

  phy = (intptr_t)lat_segment;

  // lat_segment is a region that shared between process, so it can be
  // sync_fetch_and_add to get the next available address between different
  // processes

  // phy base
  intptr_t base = __sync_fetch_and_add(&lat_segment->brk, interval);

  lat_mspace = create_mspace_with_base(virt_to_phy(base), interval, 1);
}
// Buffer for mmap
