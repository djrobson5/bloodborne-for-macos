/* macOS host (x86-64 under Rosetta 2): the pieces Linux provides that the runtime uses and
 * macOS does not. Implementations in mac_compat.c.
 *
 * Address space on Apple Silicon: [64 GiB, 448 GiB) is the kernel's GPU carveout and cannot be
 * mapped. The default malloc zone hands out 0x7f... addresses, far above the 1 TiB the guest's
 * 40-bit pointer fields can hold: the runtime's allocations come from a heap of its own at the
 * carveout's end instead (runtime_low_malloc), below bit 39 (a 40-bit field read as signed keeps
 * them positive), and guest memory starts after it (runtime_memory.c). */
#ifndef BB_MAC_COMPAT_H
#define BB_MAC_COMPAT_H
#ifdef __APPLE__
/* System headers first: the macros below rename calls they declare. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc/malloc.h>
#include <sys/stat.h>

/* Guest-visible address ranges (runtime_memory.c reserves them at startup). */
#define BB_MAC_LOW_MIN UINT64_C(0x0800000000)  /* host-owned guest-visible: image, stacks */
#define BB_MAC_LOW_END UINT64_C(0x1000000000)  /* the GPU carveout starts here */
#define BB_MAC_HEAP_MIN UINT64_C(0x7000000000) /* and ends here: the runtime's heap */
#define BB_MAC_HEAP_END UINT64_C(0x7400000000)
#define BB_MAC_USER_MIN BB_MAC_HEAP_END        /* guest memory */
#define BB_MAC_USER_MAX UINT64_C(0xfc00000000)

/* Reserves the guest-visible ranges (no access) so the host cannot place anything there. */
int runtime_mac_reserve(void);
/* The runtime's heap below 1 TiB. free and realloc take host heap pointers as well. */
void *runtime_low_malloc(size_t size);
void *runtime_low_calloc(size_t count, size_t size);
void *runtime_low_realloc(void *pointer, size_t size);
void *runtime_low_aligned_alloc(size_t alignment, size_t size);
void runtime_low_free(void *pointer);
#ifndef BB_MAC_COMPAT_IMPLEMENTATION
/* Function-like: __attribute__((malloc)) in system headers stays as it is. */
#define malloc(size) runtime_low_malloc(size)
#define calloc(count, size) runtime_low_calloc(count, size)
#define realloc(pointer, size) runtime_low_realloc(pointer, size)
#define aligned_alloc(alignment, size) runtime_low_aligned_alloc(alignment, size)
#define free(pointer) runtime_low_free(pointer)
#endif
/* struct stat's timestamps (Linux st_atim...). */
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec

/* Guest TCB in a pthread key: GS is the thread's TSD array on macOS, so the loader points the
 * guest's `mov rax, fs:[0]` (rewritten to gs:[0]) at gs:[key * 8]. */
uint32_t runtime_mac_tls_key(void);
void runtime_mac_set_tcb(void *tcb);
/* Kernel thread id (Linux gettid). */
uint64_t runtime_mac_thread_id(void);
/* Reads that cannot fault (Linux process_vm_readv on our own process): bytes read. */
size_t runtime_mac_safe_read(void *out, uintptr_t address, size_t size);

/* Register access in a signal handler's context (ucontext_t). */
#define BB_UC_RIP(uc) ((uc)->uc_mcontext->__ss.__rip)
#define BB_UC_RSP(uc) ((uc)->uc_mcontext->__ss.__rsp)
#define BB_UC_RBP(uc) ((uc)->uc_mcontext->__ss.__rbp)
#define BB_UC_RDI(uc) ((uc)->uc_mcontext->__ss.__rdi)
#define BB_UC_ERR(uc) ((uc)->uc_mcontext->__es.__err)
#endif
#endif
