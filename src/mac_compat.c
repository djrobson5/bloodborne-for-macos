/* macOS host pieces (mac_compat.h): address space reservation, the runtime's heap below
 * 1 TiB, the guest TCB's pthread key, thread ids and fault-free reads. */
#define BB_MAC_COMPAT_IMPLEMENTATION
#include "mac_compat.h"
#ifdef __APPLE__
#include <errno.h>
#include <pthread.h>
#include <os/lock.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

/* ---- Address space ---- */
#define RESERVE_STEP (UINT64_C(1) << 30)
static int reserve_range(uint64_t start, uint64_t end) {
    int holes=0;
    for (uint64_t at=start; at<end; at+=RESERVE_STEP) {
        const uint64_t size=end-at<RESERVE_STEP ? end-at : RESERVE_STEP;
        /* No MAP_FIXED: a piece something else already holds is left to it, and reported. */
        void *p=mmap((void *)at,size,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
        if (p==(void *)at) continue;
        if (p!=MAP_FAILED) munmap(p,size);
        if (holes++<4) fprintf(stderr,"Runtime: cannot reserve %#llx+%#llx (taken)\n",(unsigned long long)at,(unsigned long long)size);
    }
    return holes;
}
static void heap_init(void);
static int reserved;
int runtime_mac_reserve(void) {
    if (reserved) return 0;
    reserved=1;
    int holes=reserve_range(BB_MAC_LOW_MIN,BB_MAC_LOW_END)+reserve_range(BB_MAC_HEAP_MIN,BB_MAC_USER_MAX);
    heap_init();
    return holes ? -1 : 0;
}
/* Before main and before the GPU driver and SDL allocate: their allocations go bottom-up from
 * 4 GiB and would otherwise grow into the guest's ranges. */
__attribute__((constructor)) static void reserve_at_start(void) { runtime_mac_reserve(); }

/* ---- Heap in [BB_MAC_HEAP_MIN, BB_MAC_HEAP_END) ----
 * Power-of-two blocks with a 16-byte header in front of each pointer; freed blocks go on their
 * size class's list. Blocks are never returned to other sizes; large free ones give their pages
 * back. The runtime allocates little and mostly long-lived objects (threads, locks, handles). */
#define HEAP_BASE BB_MAC_HEAP_MIN
#define HEAP_MAGIC UINT32_C(0x68776f6c)
#define MIN_CLASS 5  /* 32-byte blocks */
#define MAX_CLASS 32 /* 4 GiB blocks */
typedef struct { uint32_t magic, cls; uintptr_t block; } Header;
_Static_assert(sizeof(Header)==16,"heap header");
typedef struct FreeBlock { struct FreeBlock *next; } FreeBlock;
static FreeBlock *free_lists[MAX_CLASS+1];
static os_unfair_lock class_locks[MAX_CLASS+1];
static os_unfair_lock bump_lock=OS_UNFAIR_LOCK_INIT;
static uintptr_t bump=HEAP_BASE;
static int heap_ready;

static void heap_init(void) {
    void *p=mmap((void *)HEAP_BASE,BB_MAC_HEAP_END-HEAP_BASE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0);
    if (p!=(void *)HEAP_BASE) { perror("Runtime: low heap"); return; }
    for (unsigned c=0;c<=MAX_CLASS;++c) class_locks[c]=OS_UNFAIR_LOCK_INIT;
    heap_ready=1;
}
static int owned(const void *p) { return (uintptr_t)p>=HEAP_BASE && (uintptr_t)p<BB_MAC_HEAP_END; }
static unsigned class_of(size_t bytes) {
    unsigned c=MIN_CLASS;
    while (c<=MAX_CLASS && (UINT64_C(1)<<c)<bytes) ++c;
    return c;
}
static uintptr_t take_block(unsigned c) {
    os_unfair_lock_lock(&class_locks[c]);
    FreeBlock *b=free_lists[c];
    if (b) free_lists[c]=b->next;
    os_unfair_lock_unlock(&class_locks[c]);
    if (b) return (uintptr_t)b;
    const uint64_t size=UINT64_C(1)<<c, align=size<4096 ? 16 : 4096;
    os_unfair_lock_lock(&bump_lock);
    uintptr_t at=(bump+align-1)&~(uintptr_t)(align-1);
    if (at+size>BB_MAC_HEAP_END) at=0;
    else bump=at+size;
    os_unfair_lock_unlock(&bump_lock);
    return at;
}
/* A block for bytes at alignment (a power of two, at least 16). */
static void *heap_alloc(size_t bytes, size_t alignment) {
    if (!heap_ready && (runtime_mac_reserve(), !heap_ready)) return NULL;
    if (bytes>(UINT64_C(1)<<MAX_CLASS)) { errno=ENOMEM; return NULL; }
    const size_t need=bytes+sizeof(Header)+(alignment>16 ? alignment : 0);
    const unsigned c=class_of(need ? need : 1);
    if (c>MAX_CLASS) { errno=ENOMEM; return NULL; }
    const uintptr_t block=take_block(c);
    if (!block) { errno=ENOMEM; return NULL; }
    const uintptr_t user=(block+sizeof(Header)+alignment-1)&~(uintptr_t)(alignment-1);
    Header *h=(Header *)(user-sizeof(Header));
    h->magic=HEAP_MAGIC; h->cls=c; h->block=block;
    return (void *)user;
}
static const Header *header_of(const void *p) {
    const Header *h=(const Header *)((uintptr_t)p-sizeof(Header));
    if (h->magic!=HEAP_MAGIC || h->cls<MIN_CLASS || h->cls>MAX_CLASS) {
        fprintf(stderr,"Runtime: low heap: bad pointer %p\n",p);
        abort();
    }
    return h;
}
static size_t usable(const void *p) {
    const Header *h=header_of(p);
    return (size_t)(h->block+(UINT64_C(1)<<h->cls)-(uintptr_t)p);
}

void *runtime_low_malloc(size_t size) { return heap_alloc(size,16); }
void *runtime_low_calloc(size_t count, size_t size) {
    if (size && count>SIZE_MAX/size) { errno=ENOMEM; return NULL; }
    void *p=heap_alloc(count*size,16);
    if (p) memset(p,0,count*size); /* reused blocks are not zero */
    return p;
}
void *runtime_low_aligned_alloc(size_t alignment, size_t size) {
    if (!alignment || (alignment&(alignment-1))) { errno=EINVAL; return NULL; }
    return heap_alloc(size,alignment<16 ? 16 : alignment);
}
void runtime_low_free(void *p) {
    if (!p) return;
    if (!owned(p)) { free(p); return; } /* the host heap's (strdup, getline, realpath) */
    const Header *h=header_of(p);
    const unsigned c=h->cls;
    FreeBlock *b=(FreeBlock *)h->block;
    ((Header *)h)->magic=0;
    if (c>=20) madvise((void *)(b+1),(UINT64_C(1)<<c)-4096,MADV_FREE); /* 1 MiB and up: pages back */
    os_unfair_lock_lock(&class_locks[c]);
    b->next=free_lists[c]; free_lists[c]=b;
    os_unfair_lock_unlock(&class_locks[c]);
}
void *runtime_low_realloc(void *p, size_t size) {
    if (!p) return runtime_low_malloc(size);
    if (!owned(p)) return realloc(p,size);
    const size_t have=usable(p);
    if (size<=have) return p;
    void *next=runtime_low_malloc(size);
    if (!next) return NULL;
    memcpy(next,p,have);
    runtime_low_free(p);
    return next;
}

/* ---- Threads ---- */
static pthread_key_t tcb_key;
static pthread_once_t tcb_once=PTHREAD_ONCE_INIT;
static void make_key(void) {
    if (pthread_key_create(&tcb_key,NULL)) { perror("STOP: pthread_key_create"); exit(21); }
}
uint32_t runtime_mac_tls_key(void) {
    pthread_once(&tcb_once,make_key);
    return (uint32_t)tcb_key;
}
void runtime_mac_set_tcb(void *tcb) {
    pthread_once(&tcb_once,make_key);
    pthread_setspecific(tcb_key,tcb);
}
uint64_t runtime_mac_thread_id(void) {
    uint64_t id=0;
    pthread_threadid_np(NULL,&id);
    return id;
}
size_t runtime_mac_safe_read(void *out, uintptr_t address, size_t size) {
    mach_vm_size_t got=0;
    if (mach_vm_read_overwrite(mach_task_self(),address,size,(mach_vm_address_t)(uintptr_t)out,&got)!=KERN_SUCCESS) return 0;
    return (size_t)got;
}
#endif
