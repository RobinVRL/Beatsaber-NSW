/* Minimal libnx surface for compiling the REAL bionic_mmap.c on Linux.
 * These mocks only provide platform mechanics; bs_madvise is not replaced.
 * No stub may report a successful unimplemented virtual-memory operation.
 */
#ifndef BS_TEST_MADVISE_SWITCH_STUB_H
#define BS_TEST_MADVISE_SWITCH_STUB_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int Result;
typedef struct Mutex { int unused; } Mutex;
typedef struct VirtmemReservation { int unused; } VirtmemReservation;
typedef struct MemoryInfo { u64 addr, size; int type; } MemoryInfo;

enum {
    InfoType_StackRegionAddress = 1,
    InfoType_StackRegionSize = 2,
    MemType_Unmapped = 0,
    CUR_PROCESS_HANDLE = 0,
};

#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)

static inline void mutexLock(Mutex *mutex) { (void)mutex; }
static inline void mutexUnlock(Mutex *mutex) { (void)mutex; }
static inline void virtmemLock(void) {}
static inline void virtmemUnlock(void) {}

/* The fallback heap-map tests do not initialize the Horizon stack window.
 * Separately, the committed-window case seeds REAL bionic_mmap.c ownership
 * metadata directly, avoiding a fake 512 MiB Horizon virtual reservation.
 */
static inline Result svcGetInfo(u64 *out, int type, int process, u64 subid) {
    (void)out; (void)type; (void)process; (void)subid;
    return -1;
}
static inline Result svcQueryMemory(MemoryInfo *out, u32 *page_info, u64 addr) {
    (void)out; (void)page_info; (void)addr;
    return -1;
}
static inline VirtmemReservation *virtmemAddReservation(void *base, size_t size) {
    (void)base; (void)size;
    return NULL;
}
static inline Result svcMapMemory(void *dst, void *src, size_t size) {
    (void)dst; (void)src; (void)size;
    return -1;
}
static inline Result svcUnmapMemory(void *dst, void *src, size_t size) {
    (void)dst; (void)src; (void)size;
    return -1;
}
#endif
