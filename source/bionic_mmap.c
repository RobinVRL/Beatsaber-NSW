/* Native Bionic memory mapping for original Quest Unity/IL2CPP on Horizon.
 * Derived from the MIT mmap arena and stack-region techniques in
 * ChanseyIsTheBest/pvz_fusion_en_nx, adapted for Unity 6000.3 and with
 * explicit failures when a requested mapping cannot be represented safely.
 *
 * This layer is not yet verified on physical Switch hardware; do not start
 * the game until memory-map, TLS and JNI integration tests pass.
 */
#include <switch.h>
#include <malloc.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/iosupport.h>

#if defined(__SWITCH2__)
#include <switch2/memory/heap.h>
#endif
#include "bionic_mmap.h"
#include "util.h"
#include "so_util.h"

#define BS_PAGE_SIZE 0x1000u
#define BS_MAP_ANONYMOUS 0x20
#define BS_MAP_FIXED 0x10
#define BS_MAP_SHARED 0x01
#define BS_MAP_PRIVATE 0x02
#define BS_PROT_NONE 0
#define BS_PROT_READ 1
#define BS_PROT_WRITE 2
#define BS_PROT_EXEC 4
#define BS_MADV_DONTNEED 4
#define BS_MADV_NORMAL 0
#define BS_OC_WINDOW_BYTES ((size_t)16384 * 1024 * 1024)
#define BS_OC_CHUNK_BYTES ((size_t)32 * 1024 * 1024)
#define BS_OC_HEAP_RESERVE ((size_t)96 * 1024 * 1024)
#define BS_OC_MIN_BYTES ((size_t)512 * 1024 * 1024)
#define BS_OC_ALIGN ((size_t)256 * 1024 * 1024)
#define BS_STACK_HEADROOM ((u64)128 * 1024 * 1024)
#define BS_NO_PAGE 0xffffffffu
#define BS_FALLBACK_MAPS 2048

typedef struct BsMmapWindow {
    uint8_t *base;
    size_t page_count;
    uint8_t *used;
    uint32_t *source;
    uint8_t *pool;
    uint32_t *pool_next;
    size_t pool_pages;
    size_t pool_cursor;
    size_t chunk_start;
    size_t chunk_end;
    size_t owned_pages;
    uint32_t free_head;
    VirtmemReservation *reservation;
    bool ready;
    bool initialized;
} BsMmapWindow;
typedef struct BsHeapMap {
    void *address;
    size_t length;
    bool reserve;   /* heap-backed PROT_NONE reservation (Switch 2) */
    void *raw;      /* reserve: pointer to free() (address is aligned in it) */
} BsHeapMap;
static BsMmapWindow g_window;
static BsHeapMap g_heapmaps[BS_FALLBACK_MAPS];
static Mutex g_mmap_lock;
static size_t g_committed_pages;
static size_t g_window_high;
size_t bs_mmap_committed_mib(void) { return g_committed_pages*BS_PAGE_SIZE>>20; }
size_t bs_mmap_pool_mib(void) { return g_window.owned_pages*BS_PAGE_SIZE>>20; }

static size_t page_align(size_t count) {
    if (count > SIZE_MAX - (BS_PAGE_SIZE-1)) return 0;
    return (count+(BS_PAGE_SIZE-1)) & ~(size_t)(BS_PAGE_SIZE-1);
}

/* Window pages are svcMapMemory aliases. Filesystem IPC cannot transfer
 * into/out of them (reads silently return zeroes), so file I/O whose buffer
 * lies in the window is bounced through heap memory at the devoptab level.
 * This covers read/pread/fread/write for every sdmc descriptor. */
static bool window_contains(const void *p,size_t n) {
    uintptr_t a=(uintptr_t)p,b=(uintptr_t)g_window.base;
    size_t total=g_window.page_count*BS_PAGE_SIZE;
    if (g_window.ready && n && a<b+total && a+n>b) return true;
    /* Switch 2: module data pages are svcMapMemory aliases as well. */
    uintptr_t lo=0,hi=0;
    return n && so_arena_range(&lo,&hi) && a<hi && a+n>lo;
}
#define BS_IO_BOUNCE (256u<<10)
static devoptab_t g_bounce_tab;
static const devoptab_t *g_bounce_orig;
static ssize_t bounce_read_r(struct _reent *r,void *fd,char *ptr,size_t len) {
    if (!window_contains(ptr,len)) return g_bounce_orig->read_r(r,fd,ptr,len);
    size_t chunk=len<BS_IO_BOUNCE?len:BS_IO_BOUNCE;
    char *tmp=malloc(chunk);
    if (!tmp) { r->_errno=ENOMEM;return -1; }
    size_t done=0;
    ssize_t result=0;
    while (done<len) {
        size_t want=len-done<chunk?len-done:chunk;
        ssize_t got=g_bounce_orig->read_r(r,fd,tmp,want);
        if (got<0) { result=done?0:-1;break; }
        memcpy(ptr+done,tmp,(size_t)got);
        done+=(size_t)got;
        if ((size_t)got<want) break;
    }
    free(tmp);
    return result<0?-1:(ssize_t)done;
}
static ssize_t bounce_write_r(struct _reent *r,void *fd,const char *ptr,size_t len) {
    if (!window_contains(ptr,len)) return g_bounce_orig->write_r(r,fd,ptr,len);
    size_t chunk=len<BS_IO_BOUNCE?len:BS_IO_BOUNCE;
    char *tmp=malloc(chunk);
    if (!tmp) { r->_errno=ENOMEM;return -1; }
    size_t done=0;
    ssize_t result=0;
    while (done<len) {
        size_t want=len-done<chunk?len-done:chunk;
        memcpy(tmp,ptr+done,want);
        ssize_t put=g_bounce_orig->write_r(r,fd,tmp,want);
        if (put<0) { result=done?0:-1;break; }
        done+=(size_t)put;
        if ((size_t)put<want) break;
    }
    free(tmp);
    return result<0?-1:(ssize_t)done;
}
static void install_io_bounce(void) {
    if (g_bounce_orig) return;
    int index=FindDevice("sdmc:");
    if (index<0 || !devoptab_list[index] || !devoptab_list[index]->read_r ||
        !devoptab_list[index]->write_r) {
        debugPrintf("[mmap] sdmc devoptab not found: window file I/O unbounced\n");
        return;
    }
    g_bounce_orig=devoptab_list[index];
    g_bounce_tab=*g_bounce_orig;
    g_bounce_tab.read_r=bounce_read_r;
    g_bounce_tab.write_r=bounce_write_r;
    devoptab_list[index]=&g_bounce_tab;
}

/* Window backing mode. Unity 6 reserves one 256 MiB-aligned region per
 * allocator (18+ regions, 4.5+ GiB of address space, <1 GiB committed), which
 * cannot fit the 2 GiB stack region that svcMapMemory is limited to on real
 * hardware. Code mode instead aliases pool pages with svcMapProcessCodeMemory
 * (+ RW permission) into the ASLR code region, exactly like module data
 * segments, so the full 16 GiB window fits on Switch 1. */
static bool g_code_mode;
static Handle window_process(void) {
    Handle h=envGetOwnProcessHandle();
    return h==INVALID_HANDLE ? CUR_PROCESS_HANDLE : h;
}
static Result window_map(void *dst,void *src,size_t size) {
    if (!g_code_mode) return svcMapMemory(dst,src,size);
    Handle h=window_process();
    Result rc=svcMapProcessCodeMemory(h,(u64)dst,(u64)src,size);
    if (R_FAILED(rc)) return rc;
    rc=svcSetProcessMemoryPermission(h,(u64)dst,size,Perm_Rw);
    if (R_FAILED(rc)) svcUnmapProcessCodeMemory(h,(u64)dst,(u64)src,size);
    return rc;
}
static Result window_unmap(void *dst,void *src,size_t size) {
    if (!g_code_mode) return svcUnmapMemory(dst,src,size);
    return svcUnmapProcessCodeMemory(window_process(),(u64)dst,(u64)src,size);
}
/* Reserve an aligned 16 GiB code-region window and prove one page can be
 * aliased RW into it (and released) on this kernel. */
static VirtmemReservation *code_window_reserve(u64 *base_out) {
    uintptr_t lo=0,hi=0;
    if (so_arena_range(&lo,&hi)) return NULL; /* kaiHEN: code SVCs unavailable */
    virtmemLock();
    void *found=virtmemFindCodeMemory(BS_OC_WINDOW_BYTES+BS_OC_ALIGN,0);
    u64 base=found ? ((u64)found+BS_OC_ALIGN-1)&~(u64)(BS_OC_ALIGN-1) : 0;
    VirtmemReservation *rv=base ? virtmemAddReservation((void *)base,BS_OC_WINDOW_BYTES) : NULL;
    virtmemUnlock();
    if (!rv) return NULL;
    void *page=memalign(BS_PAGE_SIZE,BS_PAGE_SIZE);
    Result rc=page ? 0 : MAKERESULT(Module_Libnx,LibnxError_OutOfMemory);
    if (page) {
        g_code_mode=true;
        rc=window_map((void *)base,page,BS_PAGE_SIZE);
        if (R_SUCCEEDED(rc)) {
            volatile uint32_t *probe=(volatile uint32_t *)base;
            *probe=0x5a5a5a5a;
            rc=*probe==0x5a5a5a5a ? 0 : MAKERESULT(Module_Libnx,LibnxError_BadInput);
            Result un=window_unmap((void *)base,page,BS_PAGE_SIZE);
            if (R_SUCCEEDED(rc)) rc=un;
        }
        g_code_mode=R_SUCCEEDED(rc);
        free(page);
    }
    if (R_FAILED(rc)) {
        debugPrintf("[mmap] code-region window unavailable (0x%x): using stack region\n",rc);
        virtmemLock(); virtmemRemoveReservation(rv); virtmemUnlock();
        return NULL;
    }
    *base_out=base;
    return rv;
}

static void window_initialize_locked(void) {
    if (g_window.initialized) return;
    g_window.initialized=true;

    u64 code_base=0;
    VirtmemReservation *code_rv=code_window_reserve(&code_base);

    u64 start=0, length=0;
    if (R_FAILED(svcGetInfo(&start,InfoType_StackRegionAddress,
                           CUR_PROCESS_HANDLE,0)) ||
        R_FAILED(svcGetInfo(&length,InfoType_StackRegionSize,
                           CUR_PROCESS_HANDLE,0))) return;

    debugPrintf("[mmap] stack region %#llx size %llu MiB\n",
                (unsigned long long)start, (unsigned long long)(length>>20));
    u64 finish=start+length;
    u64 best_start=0;
    size_t best_size=0;
    for (u64 cursor=start;cursor<finish;) {
        MemoryInfo memory={0};
        u32 page_info=0;
        if (R_FAILED(svcQueryMemory(&memory,&page_info,cursor)) ||
            memory.addr+memory.size<=cursor) break;
        if (memory.type!=MemType_Unmapped && memory.size>=((u64)16<<20))
            debugPrintf("[mmap]   stack-region block %#llx +%llu MiB type %u\n",
                        (unsigned long long)memory.addr,
                        (unsigned long long)(memory.size>>20),(unsigned)memory.type);
        if (memory.type==MemType_Unmapped) {
            u64 lo=memory.addr,bound=memory.addr+memory.size;
            /* Real hardware (Switch 1 and 2) refuses svcMapMemory outside
             * the stack region with 0xdc01; Eden doesn't check. Leave room
             * at the top for libnx thread stacks, which map there too. */
            if (lo<start) lo=start;
            if (bound>finish) bound=finish;
            if (bound==finish && bound-lo>BS_STACK_HEADROOM+BS_OC_MIN_BYTES)
                bound-=BS_STACK_HEADROOM;
            if (bound<=lo) { cursor=memory.addr+memory.size; continue; }
            /* The Switch 2 module arena is reserved but partly unmapped:
             * keep the larger free side of it. */
            uintptr_t arena_lo=0,arena_hi=0;
            if (so_arena_range(&arena_lo,&arena_hi) && lo<arena_hi && bound>arena_lo) {
                u64 below=arena_lo>lo ? arena_lo-lo : 0;
                u64 above=bound>arena_hi ? bound-arena_hi : 0;
                if (above>=below) lo=arena_hi; else bound=arena_lo;
            }
            u64 aligned=(lo+(BS_OC_ALIGN-1)) &
                        ~(u64)(BS_OC_ALIGN-1);
            if (aligned<bound && bound-aligned>=BS_OC_MIN_BYTES) {
                size_t avail=(size_t)(bound-aligned);
                avail &= ~(BS_OC_ALIGN-1);
                if (avail>BS_OC_WINDOW_BYTES) avail=BS_OC_WINDOW_BYTES;
                if (avail>best_size) {
                    best_start=aligned;best_size=avail;
                }
            }
        }
        cursor=memory.addr+memory.size;
    }
    if (code_rv) { best_start=code_base; best_size=BS_OC_WINDOW_BYTES; }
    uintptr_t pre_base=0; size_t pre_size=0; void *pre_rv=NULL;
    if (!code_rv && so_window_reservation(&pre_base,&pre_size,&pre_rv)) {
        /* Already reserved at module-arena time, before threads could
         * fragment it; the scan above is ignored. */
        if (pre_size>BS_OC_WINDOW_BYTES) pre_size=BS_OC_WINDOW_BYTES;
        best_start=pre_base; best_size=pre_size; code_rv=pre_rv;
    }
    if (!best_size) {
        debugPrintf("[mmap] no free 512MB+ stack window: using heap maps\n");
        return;
    }

    size_t pages=best_size/BS_PAGE_SIZE;
    uint8_t *used=calloc(pages,1);
    uint32_t *source=malloc(pages*sizeof(*source));
    /* Physical backing grows on demand: window pages alias page-aligned
     * chunks memalign'ed from newlib's heap, so Unity's mappings and newlib
     * malloc (guest malloc, Mesa/nvmap GPU buffers) share one budget instead
     * of a fixed split. Pool page indices span the whole newlib heap. This
     * adapts to the host: ~3.2 GiB on Switch 1 titles, ~10 GiB inside a
     * Switch 2 title under kaiHEN. */
    extern char *fake_heap_start, *fake_heap_end;
    uintptr_t heap_lo=((uintptr_t)fake_heap_start+BS_PAGE_SIZE-1)&~(uintptr_t)(BS_PAGE_SIZE-1);
    uintptr_t heap_hi=(uintptr_t)fake_heap_end&~(uintptr_t)(BS_PAGE_SIZE-1);
#if defined(__SWITCH2__)
    /* libounce's mimalloc owns the heap; newlib's fake_heap_* are unset.
     * Span the kernel heap region instead: every heap chunk lies in it. */
    {
        u64 hr=0,hs=0;
        if (R_SUCCEEDED(svcGetInfo(&hr,InfoType_HeapRegionAddress,CUR_PROCESS_HANDLE,0)) &&
            R_SUCCEEDED(svcGetInfo(&hs,InfoType_HeapRegionSize,CUR_PROCESS_HANDLE,0)) && hs) {
            heap_lo=(uintptr_t)hr;
            heap_hi=(uintptr_t)(hr+hs)&~(uintptr_t)(BS_PAGE_SIZE-1);
        }
    }
#endif
    uint8_t *pool=heap_hi>heap_lo ? (uint8_t *)heap_lo : NULL;
    size_t pool_pages=pool ? (heap_hi-heap_lo)/BS_PAGE_SIZE : 0;
    u64 total_mem=0;
    svcGetInfo(&total_mem,InfoType_TotalMemorySize,CUR_PROCESS_HANDLE,0);
    debugPrintf("[mmap] process memory total=%llu MiB, newlib heap %zu MiB at %p: "
                "on-demand pool (chunk %zu MiB, malloc reserve %zu MiB)\n",
                (unsigned long long)(total_mem>>20),(pool_pages*BS_PAGE_SIZE)>>20,pool,
                BS_OC_CHUNK_BYTES>>20,BS_OC_HEAP_RESERVE>>20);
    uint32_t *pool_next=pool ? malloc(pool_pages*sizeof(*pool_next)) : NULL;
    if (!used||!source||!pool||!pool_next) {
        free(used);free(source);free(pool_next);
        debugPrintf("[mmap] no physical commit pool: using heap maps\n");
        return;
    }
    for(size_t i=0;i<pages;++i)source[i]=BS_NO_PAGE;

    VirtmemReservation *rv=code_rv;
    if (!rv) {
        virtmemLock();
        rv=virtmemAddReservation((void *)best_start,best_size);
        virtmemUnlock();
    }
    if (!rv) {
        free(used);free(source);free(pool_next);
        debugPrintf("[mmap] virtual window reservation failed\n");
        return;
    }

    g_window=(BsMmapWindow){
        .base=(uint8_t *)best_start,
        .page_count=pages,
        .used=used,
        .source=source,
        .pool=pool,
        .pool_next=pool_next,
        .pool_pages=pool_pages,
        .free_head=BS_NO_PAGE,
        .reservation=rv,
        .ready=true,
        .initialized=true
    };
    debugPrintf("[mmap] overcommit window reserved %zu MiB at %p (%s)\n",
                best_size>>20,(void *)best_start,
                g_code_mode ? "code region, svcMapProcessCodeMemory" : "stack region, svcMapMemory");
    install_io_bounce();
}

static bool range_in_window(void *address,size_t length,size_t *first,size_t *count) {
    uintptr_t p=(uintptr_t)address;
    uintptr_t base=(uintptr_t)g_window.base;
    size_t total=g_window.page_count*BS_PAGE_SIZE;
    if (!g_window.ready||p<base||p>=base+total||
        length>base+total-p) return false;
    *first=(p-base)/BS_PAGE_SIZE;
    *count=page_align(length)/BS_PAGE_SIZE;
    if (*first+*count>g_window.page_count)return false;
    return true;
}

/* Diagnostic: per-256 MiB slot reserved/committed MiB. */
void bs_mmap_log_regions(void) {
    mutexLock(&g_mmap_lock);
    const size_t per=BS_OC_ALIGN/BS_PAGE_SIZE;
    char line[1024]; size_t n=0; unsigned slots=0;
    for (size_t s0=0;g_window.ready && s0<g_window.page_count;s0+=per) {
        size_t used=0,committed=0;
        for (size_t k=s0;k<s0+per && k<g_window.page_count;++k) {
            used+=g_window.used[k]!=0;
            committed+=g_window.source[k]!=BS_NO_PAGE;
        }
        if (!used) continue;
        ++slots;
        if (n<sizeof(line)-32)
            n+=snprintf(line+n,sizeof(line)-n," %zu:%zu/%zu",s0/per,
                        used*BS_PAGE_SIZE>>20,committed*BS_PAGE_SIZE>>20);
    }
    line[n]=0;
    mutexUnlock(&g_mmap_lock);
    debugPrintf("[mmap] regions(slot:reserved/committed MiB) %u slots, %zu MiB committed:%s\n",
                slots,g_committed_pages*BS_PAGE_SIZE>>20,line);
}

/* Horizon tracks every svcMapMemory alias as kernel memory blocks, and the
 * kernel's block pool is finite (Eden: ResourceExhausted 0xce01 after ~10k
 * single-page aliases). Contiguous runs of uncommitted window pages are
 * therefore backed by contiguous pool pages from the bump cursor and mapped
 * with ONE call; recycled single pages are used only once it is exhausted. */
static bool map_window_run_locked(size_t page,uint32_t src_page,size_t pages,
                                  size_t first,size_t count) {
    void *dst=g_window.base+page*BS_PAGE_SIZE;
    void *src=g_window.pool+(size_t)src_page*BS_PAGE_SIZE;
    Result rc=window_map(dst,src,pages*BS_PAGE_SIZE);
    if (R_FAILED(rc)) {
        debugPrintf("[mmap] window map failed rc=0x%x dst=%p src=%p "
                    "pages=%zu request=[%zu,+%zu) pool_cursor=%zu\n",
                    rc,dst,src,pages,first,count,g_window.pool_cursor);
        errno=ENOMEM;
        return false;
    }
    for (size_t k=0;k<pages;++k)
        g_window.source[page+k]=src_page+(uint32_t)k;
    g_committed_pages+=pages;
    memset(dst,0,pages*BS_PAGE_SIZE);
    return true;
}

/* Take another chunk of newlib heap as backing. Fails when that would leave
 * newlib less than BS_OC_HEAP_RESERVE free (Mesa/nvmap and guest malloc). */
static bool grow_pool_locked(size_t want_pages) {
    size_t want=want_pages*BS_PAGE_SIZE;
    size_t bytes=want>BS_OC_CHUNK_BYTES ? want : BS_OC_CHUNK_BYTES;
#if defined(__SWITCH2__)
    size_t free_bytes=(size_t)heapGetFreeSize();   /* mimalloc heap */
#else
    extern char *fake_heap_end;
    char *brk=sbrk(0);
    size_t tail=(brk!=(char *)-1 && fake_heap_end>brk) ? (size_t)(fake_heap_end-brk) : 0;
    size_t free_bytes=tail+(size_t)mallinfo().fordblks;
#endif
    if (free_bytes<want+BS_OC_HEAP_RESERVE) return false;
    if (free_bytes<bytes+BS_OC_HEAP_RESERVE) bytes=want;
    uint8_t *p=memalign(BS_PAGE_SIZE,bytes);
    if (!p && bytes>want) p=memalign(BS_PAGE_SIZE,bytes=want);
    if (!p) return false;
    if (p<g_window.pool ||
        (size_t)(p-g_window.pool)/BS_PAGE_SIZE+bytes/BS_PAGE_SIZE>g_window.pool_pages) {
        debugPrintf("[mmap] chunk %p outside newlib heap\n",(void *)p);
        free(p);
        return false;
    }
    size_t idx=(size_t)(p-g_window.pool)/BS_PAGE_SIZE, n=bytes/BS_PAGE_SIZE;
    g_window.owned_pages+=n;
    if (idx==g_window.chunk_end) {
        g_window.chunk_end+=n;          /* adjacent: extend the bump region */
        return true;
    }
    /* Retire the rest of the old bump region to the free list. */
    for (size_t k=g_window.chunk_end;k>g_window.pool_cursor;--k) {
        g_window.pool_next[k-1]=g_window.free_head;
        g_window.free_head=(uint32_t)(k-1);
    }
    g_window.chunk_start=g_window.pool_cursor=idx;
    g_window.chunk_end=idx+n;
    return true;
}

static bool commit_window_pages_locked(size_t first,size_t count) {
    for (size_t i=first;i<first+count;) {
        if (g_window.source[i]!=BS_NO_PAGE) { ++i; continue; }
        size_t run=1;
        while (i+run<first+count && g_window.source[i+run]==BS_NO_PAGE)
            ++run;
        size_t bump=g_window.chunk_end-g_window.pool_cursor;
        if (bump) {
            size_t pages=run<bump ? run : bump;
            uint32_t src_page=(uint32_t)g_window.pool_cursor;
            if (!map_window_run_locked(i,src_page,pages,first,count))
                return false;
            g_window.pool_cursor+=pages;
            i+=pages;
            continue;
        }
        if (g_window.free_head==BS_NO_PAGE) {
            if (grow_pool_locked(run)) continue;
            errno=ENOMEM;
            debugPrintf("[mmap] backing exhausted: %zu MiB owned, %zu MiB committed\n",
                        g_window.owned_pages*BS_PAGE_SIZE>>20,
                        g_committed_pages*BS_PAGE_SIZE>>20);
            return false;
        }
        /* Pop an ascending contiguous run off the free list: one alias. */
        uint32_t src_page=g_window.free_head;
        size_t pages=1;
        uint32_t next=g_window.pool_next[src_page];
        while (pages<run && next==src_page+(uint32_t)pages) {
            next=g_window.pool_next[next];
            ++pages;
        }
        uint32_t old_head=g_window.free_head;
        g_window.free_head=next;
        if (!map_window_run_locked(i,src_page,pages,first,count)) {
            g_window.free_head=old_head;
            return false;
        }
        i+=pages;
    }
    return true;
}
static bool decommit_window_pages_locked(size_t first,size_t count) {
    bool all_unmapped=true;
    for(size_t i=first;i<first+count;) {
        uint32_t src_page=g_window.source[i];
        if (src_page==BS_NO_PAGE) { ++i; continue; }
        /* Unmap the longest run whose pool pages are also contiguous. */
        size_t run=1;
        while (i+run<first+count &&
               g_window.source[i+run]==src_page+(uint32_t)run)
            ++run;
        void *dst=g_window.base+i*BS_PAGE_SIZE;
        void *src=g_window.pool+(size_t)src_page*BS_PAGE_SIZE;
        Result rc=window_unmap(dst,src,run*BS_PAGE_SIZE);
        if (R_SUCCEEDED(rc)) {
            for (size_t k=0;k<run;++k) g_window.source[i+k]=BS_NO_PAGE;
            g_committed_pages-=run;
            if ((size_t)src_page+run==g_window.pool_cursor &&
                src_page>=g_window.chunk_start) {
                /* Top of the bump region: return it for contiguous reuse. */
                g_window.pool_cursor=src_page;
            } else {
                /* Push in reverse so pops yield an ascending run. */
                for (size_t k=run;k>0;--k) {
                    uint32_t page=src_page+(uint32_t)(k-1);
                    g_window.pool_next[page]=g_window.free_head;
                    g_window.free_head=page;
                }
            }
        } else {
            debugPrintf("[mmap] window unmap failed rc=0x%x\n",rc);
            all_unmapped=false;
        }
        i+=run;
    }
    if (!all_unmapped) errno=ENOMEM;
    return all_unmapped;
}
/* libunity reserves 2*A-1page and immediately unmaps everything outside one
 * A-aligned A-sized block (the usual alignment trim). Window reservations of
 * >=64 MiB are already 256 MiB aligned, so grant only A and record the tail
 * as a phantom whose munmap is a no-op: this halves the address space each
 * Unity heap region costs in the ~1.3 GiB stack-region window. */
#define BS_PHANTOMS 64
static struct { uintptr_t lo,hi; } g_phantoms[BS_PHANTOMS];
static bool phantom_release_locked(void *address,size_t length) {
    uintptr_t a=(uintptr_t)address,b=a+page_align(length);
    for (size_t i=0;i<BS_PHANTOMS;++i)
        if (g_phantoms[i].hi && a>=g_phantoms[i].lo && b<=g_phantoms[i].hi) {
            /* A real reservation landed in the tail before it was trimmed:
             * the phantom is stale and this munmap is for the real one. */
            uintptr_t base=(uintptr_t)g_window.base;
            for (uintptr_t q=a;q<b;q+=BS_PAGE_SIZE)
                if (q>=base && q<base+g_window.page_count*BS_PAGE_SIZE &&
                    g_window.used[(q-base)/BS_PAGE_SIZE]) {
                    g_phantoms[i].hi=0;
                    return false;
                }
            if (a==g_phantoms[i].lo && b==g_phantoms[i].hi) g_phantoms[i].hi=0;
            else if (a==g_phantoms[i].lo) g_phantoms[i].lo=b;
            else if (b==g_phantoms[i].hi) g_phantoms[i].hi=a;
            return true;
        }
    return false;
}
static size_t trim_alignment_length(size_t length) {
    size_t a=(page_align(length)+BS_PAGE_SIZE)/2;
    if (a>=((size_t)64<<20) && a<=BS_OC_ALIGN && !(a&(a-1)) &&
        page_align(length)==2*a-BS_PAGE_SIZE) return a;
    return 0;
}
static void *reserve_exact_locked(size_t length);
static void *reserve_window_locked(size_t length) {
    size_t trimmed=trim_alignment_length(length);
    if (trimmed) {
        for (size_t i=0;i<BS_PHANTOMS;++i) if (!g_phantoms[i].hi) {
            uint8_t *p=reserve_exact_locked(trimmed);
            if (!p) break;
            g_phantoms[i].lo=(uintptr_t)p+trimmed;
            g_phantoms[i].hi=(uintptr_t)p+page_align(length);
            return p;
        }
    }
    return reserve_exact_locked(length);
}
static void *reserve_exact_locked(size_t length) {
    size_t need=page_align(length)/BS_PAGE_SIZE;
    if (!need||need>g_window.page_count) return NULL;
    const size_t step=length>=((size_t)64<<20) ?
                      BS_OC_ALIGN/BS_PAGE_SIZE : 1;
    for(size_t i=0;i+need<=g_window.page_count;i+=step) {
        size_t j=0;
        while(j<need&&!g_window.used[i+j])++j;
        if(j==need) {
            memset(g_window.used+i,1,need);
            if (i+need>g_window_high) {
                if ((i+need)*BS_PAGE_SIZE>>28 != g_window_high*BS_PAGE_SIZE>>28)
                    debugPrintf("[mmap] window high-water %zu/%zu MiB\n",
                                (i+need)*BS_PAGE_SIZE>>20,
                                g_window.page_count*BS_PAGE_SIZE>>20);
                g_window_high=i+need;
            }
            return g_window.base+i*BS_PAGE_SIZE;
        }
        if(step==1) i+=j;
    }
    return NULL;
}

static void *heap_map(size_t length,int flags,int fd,int64_t offset) {
    size_t reserved=page_align(length);
    if (!reserved) { errno=ENOMEM;return (void *)-1; }
    void *p=memalign(BS_PAGE_SIZE,reserved);
    if (!p) { errno=ENOMEM;return (void *)-1; }
    memset(p,0,reserved);
    if (!(flags&BS_MAP_ANONYMOUS)) {
        if (fd<0 || offset<0) { free(p);errno=EBADF;return (void *)-1; }
        /* devkitA64's newlib provides lseek/read but no pread symbol.
         * Restore the shared descriptor's offset after reading the map.
         * Serialize this bridge's positional operations with other mmap calls.
         */
        mutexLock(&g_mmap_lock);
        off_t old_position=lseek(fd,0,SEEK_CUR);
        if(old_position==(off_t)-1 || lseek(fd,(off_t)offset,SEEK_SET)==(off_t)-1) {
            int saved=errno;
            mutexUnlock(&g_mmap_lock);
            free(p);errno=saved;return (void *)-1;
        }
        size_t copied=0;
        int read_error=0;
        while(copied<length) {
            if ((uint64_t)offset + copied > INT64_MAX) {
                read_error=EOVERFLOW;break;
            }
            ssize_t n=read(fd,(uint8_t *)p+copied,length-copied);
            if(n<0) {
                read_error=errno;break;
            }
            if(!n)break;
            copied+=(size_t)n;
        }
        if(lseek(fd,old_position,SEEK_SET)==(off_t)-1 && !read_error)
            read_error=errno;
        mutexUnlock(&g_mmap_lock);
        if(read_error){free(p);errno=read_error;return (void *)-1;}
    }
    mutexLock(&g_mmap_lock);
    bool tracked=false;
    for(size_t i=0;i<BS_FALLBACK_MAPS;++i)if(!g_heapmaps[i].address) {
        g_heapmaps[i]=(BsHeapMap){p,reserved,false,NULL};
        tracked=true;
        break;
    }
    mutexUnlock(&g_mmap_lock);
    if(!tracked) { free(p);errno=ENOMEM;return (void *)-1; }
    return p;
}

/* Heap-backed PROT_NONE reservation (Switch 2 overflow). Always readable
 * and writable; commit = zero, decommit = no-op. Alignment-trim requests
 * (2*A-1page) get an A-aligned A-sized block plus a phantom tail. */
static BsHeapMap *heapmap_containing_locked(void *address,size_t length) {
    for(size_t i=0;i<BS_FALLBACK_MAPS;++i)
        if(g_heapmaps[i].address &&
           (uintptr_t)address>=(uintptr_t)g_heapmaps[i].address &&
           (uintptr_t)address-(uintptr_t)g_heapmaps[i].address<g_heapmaps[i].length &&
           length<=g_heapmaps[i].length-((uintptr_t)address-(uintptr_t)g_heapmaps[i].address))
            return &g_heapmaps[i];
    return NULL;
}
#if defined(__SWITCH2__)
/* 256 MiB-aligned blocks come from 4-slot slabs: mimalloc refuses huge
 * alignments (256 MiB failed with 8.5 GiB free), so one slab over-allocates
 * by one block and the padding is paid once per 4 blocks, not per block. */
#define BS_SLAB_SLOTS 4
#define BS_SLABS 16
#define BS_SLAB_SLOT ((void *)1)   /* BsHeapMap.raw marker: slab-owned */
static struct { uint8_t *base; bool used[BS_SLAB_SLOTS]; } g_slabs[BS_SLABS];
static void *slab_take_locked(void) {
    for (size_t s=0;s<BS_SLABS;++s)
        for (size_t k=0;g_slabs[s].base && k<BS_SLAB_SLOTS;++k)
            if (!g_slabs[s].used[k]) {
                g_slabs[s].used[k]=true;
                return g_slabs[s].base+k*BS_OC_ALIGN;
            }
    for (size_t s=0;s<BS_SLABS;++s) if (!g_slabs[s].base) {
        void *raw=memalign(BS_PAGE_SIZE,(BS_SLAB_SLOTS+1)*BS_OC_ALIGN);
        if (!raw) return NULL;
        g_slabs[s].base=(uint8_t *)(((uintptr_t)raw+BS_OC_ALIGN-1)&~(uintptr_t)(BS_OC_ALIGN-1));
        g_slabs[s].used[0]=true;
        debugPrintf("[mmap] heap slab %zu: %u x 256 MiB at %p\n",s,BS_SLAB_SLOTS,
                    (void *)g_slabs[s].base);
        return g_slabs[s].base;
    }
    return NULL;
}
static void slab_release_locked(void *p) {
    for (size_t s=0;s<BS_SLABS;++s)
        if (g_slabs[s].base && (uint8_t *)p>=g_slabs[s].base &&
            (uint8_t *)p<g_slabs[s].base+BS_SLAB_SLOTS*BS_OC_ALIGN)
            g_slabs[s].used[((uint8_t *)p-g_slabs[s].base)/BS_OC_ALIGN]=false;
}
static void *heap_reserve(size_t length) {
    size_t trimmed=trim_alignment_length(length);
    size_t size=trimmed?trimmed:page_align(length);
    size_t align=trimmed?trimmed:BS_PAGE_SIZE;
    void *raw=NULL, *p=NULL;
    if (size==BS_OC_ALIGN && align==BS_OC_ALIGN) {
        mutexLock(&g_mmap_lock);
        p=slab_take_locked();
        mutexUnlock(&g_mmap_lock);
        raw=p?BS_SLAB_SLOT:NULL;
    } else {
        raw=memalign(BS_PAGE_SIZE,size+align);
        p=raw ? (void *)(((uintptr_t)raw+align-1)&~(uintptr_t)(align-1)) : NULL;
    }
    if(!p) {
        debugPrintf("[mmap] heap reserve %zu MiB failed (free %llu MiB)\n",size>>20,
                    (unsigned long long)(heapGetFreeSize()>>20));
        return NULL;
    }
    memset(p,0,size);
    mutexLock(&g_mmap_lock);
    bool tracked=false;
    for(size_t i=0;i<BS_FALLBACK_MAPS;++i) if(!g_heapmaps[i].address) {
        g_heapmaps[i]=(BsHeapMap){p,size,true,raw};
        tracked=true;
        break;
    }
    if(tracked && trimmed)
        for(size_t i=0;i<BS_PHANTOMS;++i) if(!g_phantoms[i].hi) {
            g_phantoms[i].lo=(uintptr_t)p+trimmed;
            g_phantoms[i].hi=(uintptr_t)p+page_align(length);
            break;
        }
    mutexUnlock(&g_mmap_lock);
    if(!tracked) {
        if (raw==BS_SLAB_SLOT) { mutexLock(&g_mmap_lock); slab_release_locked(p); mutexUnlock(&g_mmap_lock); }
        else free(raw);
        return NULL;
    }
    debugPrintf("[mmap] heap-backed reserve %zu MiB at %p (free %llu MiB)\n",size>>20,p,
                (unsigned long long)(heapGetFreeSize()>>20));
    return p;
}
#endif
static void *bs_mmap_impl(void *hint,size_t length,int prot,int flags,int fd,int64_t offset);
void *bs_mmap(void *hint,size_t length,int prot,int flags,
              int fd,int64_t offset) {
    void *r=bs_mmap_impl(hint,length,prot,flags,fd,offset);
    if (prot==BS_PROT_NONE && (flags&BS_MAP_ANONYMOUS) && length>=((size_t)64<<20)) {
        void *ra=__builtin_return_address(0);
        so_module *m=so_find_module_by_addr(ra);
        debugPrintf("[mmap] reserve %zu MiB -> %p from %s+%#lx\n",length>>20,r,
                    m?m->name:"?",(unsigned long)((uintptr_t)ra-(m?(uintptr_t)m->load_virtbase:0)));
        if (r==(void *)-1) bs_mmap_log_regions();
    }
    if(r==(void *)-1) debugPrintf("[mmap] fd=%d len=%zu prot=%d flags=%#x off=%lld -> %p errno=%d\n",fd,length,prot,flags,(long long)offset,r,r==(void *)-1?errno:0);
    return r;
}
static void *bs_mmap_impl(void *hint,size_t length,int prot,int flags,
              int fd,int64_t offset) {
    if(!length){errno=EINVAL;return (void *)-1;}
    if ((prot & ~(BS_PROT_READ|BS_PROT_WRITE|BS_PROT_EXEC)) ||
        ((flags & (BS_MAP_SHARED|BS_MAP_PRIVATE)) != BS_MAP_SHARED &&
         (flags & (BS_MAP_SHARED|BS_MAP_PRIVATE)) != BS_MAP_PRIVATE)) {
        errno=EINVAL;return (void *)-1;
    }
    /* Heap-backed mappings cannot enforce execute or inaccessible pages. */
    if (prot & BS_PROT_EXEC) { errno=ENOTSUP;return (void *)-1; }
    if (!(flags & BS_MAP_ANONYMOUS) && (flags & BS_MAP_SHARED) &&
        (prot & BS_PROT_WRITE)) {
        /* Writable shared file maps would need write-back. Read-only shared
         * maps (APK/asset readers) are equivalent to a private copy. */
        debugPrintf("[mmap] writable MAP_SHARED fd=%d len=%zu unsupported\n",
                    fd, length);
        errno=ENOTSUP;return (void *)-1;
    }
    if (!(flags & BS_MAP_ANONYMOUS) &&
        (offset<0 || ((uint64_t)offset & (BS_PAGE_SIZE-1)))) {
        errno=EINVAL;return (void *)-1;
    }
    if(flags&BS_MAP_FIXED) {
        if(!hint || ((uintptr_t)hint&(BS_PAGE_SIZE-1))) {
            errno=EINVAL;return (void *)-1;
        }
        if (!(flags & BS_MAP_ANONYMOUS)) {
            errno=ENOTSUP;return (void *)-1;
        }
        size_t first=0,count=0;
        mutexLock(&g_mmap_lock);
        bool owned=range_in_window(hint,length,&first,&count);
        for (size_t i=0; owned && i<count; ++i)
            if (!g_window.used[first+i]) owned=false;
        if(!owned) {
            BsHeapMap *hm=heapmap_containing_locked(hint,page_align(length));
            mutexUnlock(&g_mmap_lock);
            if (hm && hm->reserve) {
                /* Heap reservation: always backed; commit = fresh zeroes. */
                if (prot!=BS_PROT_NONE) memset(hint,0,page_align(length));
                return hint;
            }
            errno=EINVAL;
            return (void *)-1;
        }
        if(prot==BS_PROT_NONE) {
            if (!decommit_window_pages_locked(first,count)) {
                mutexUnlock(&g_mmap_lock);
                return (void *)-1;
            }
        } else if(!commit_window_pages_locked(first,count)) {
            mutexUnlock(&g_mmap_lock);
            return (void *)-1;
        } else {
            /* MAP_FIXED replaces previous contents even when already mapped. */
            memset(hint,0,page_align(length));
        }
        mutexUnlock(&g_mmap_lock);
        return hint;
    }
    if ((flags&BS_MAP_ANONYMOUS) && prot==BS_PROT_NONE) {
        mutexLock(&g_mmap_lock);
        window_initialize_locked();
        void *reserved=g_window.ready ? reserve_window_locked(length):NULL;
        mutexUnlock(&g_mmap_lock);
        if(reserved)return reserved;
#if defined(__SWITCH2__)
        /* The 2 GiB stack region (the only svcMapMemory target) cannot hold
         * Unity 6's 256 MiB-per-allocator reservations. Switch 2 titles get
         * ~9 GiB, so back the overflow with real heap memory instead. */
        reserved=heap_reserve(length);
        if(reserved)return reserved;
#endif
        errno=ENOMEM;
        return (void *)-1;
    }
    return heap_map(length,flags,fd,offset);
}

int bs_munmap(void *address,size_t length) {
    if(!address||!length||((uintptr_t)address&(BS_PAGE_SIZE-1))) {
        errno=EINVAL;return -1;
    }
    mutexLock(&g_mmap_lock);
    if (phantom_release_locked(address,length)) {
        mutexUnlock(&g_mmap_lock);
        return 0;
    }
    size_t first=0,count=0;
    if(range_in_window(address,length,&first,&count)) {
        for (size_t i=0;i<count;++i)
            if (!g_window.used[first+i]) {
                mutexUnlock(&g_mmap_lock);
                errno=EINVAL;return -1;
            }
        if (!decommit_window_pages_locked(first,count)) {
            mutexUnlock(&g_mmap_lock);
            return -1;
        }
        memset(g_window.used+first,0,count);
        mutexUnlock(&g_mmap_lock);
        if (length>=((size_t)64<<20))
            debugPrintf("[mmap] release %zu MiB at %p\n",length>>20,address);
        return 0;
    }
    for(size_t i=0;i<BS_FALLBACK_MAPS;++i)
        if(g_heapmaps[i].address==address) {
            if(page_align(length)!=g_heapmaps[i].length &&
               !(g_heapmaps[i].reserve && page_align(length)>g_heapmaps[i].length)) {
                mutexUnlock(&g_mmap_lock);
                errno=ENOTSUP;return -1;
            }
            void *to_free=g_heapmaps[i].raw ? g_heapmaps[i].raw : address;
            g_heapmaps[i]=(BsHeapMap){0};
#if defined(__SWITCH2__)
            if (to_free==BS_SLAB_SLOT) {
                slab_release_locked(address);
                mutexUnlock(&g_mmap_lock);
                return 0;
            }
#endif
            mutexUnlock(&g_mmap_lock);
            free(to_free);
            return 0;
        }
    /* Partial release of a heap reservation: memory stays until the whole
     * block goes (heap blocks can't be split). */
    BsHeapMap *hm=heapmap_containing_locked(address,page_align(length));
    if (hm && hm->reserve) { mutexUnlock(&g_mmap_lock); return 0; }
    mutexUnlock(&g_mmap_lock);
    errno=EINVAL;
    return -1;
}

int bs_mprotect(void *address,size_t length,int prot) {
    if(!address||!length||((uintptr_t)address&(BS_PAGE_SIZE-1)) ||
       (prot & ~(BS_PROT_READ|BS_PROT_WRITE|BS_PROT_EXEC))) {
        errno=EINVAL;return -1;
    }
    mutexLock(&g_mmap_lock);
    size_t first=0,count=0;
    if(range_in_window(address,length,&first,&count)) {
        if(prot&BS_PROT_EXEC) {
            mutexUnlock(&g_mmap_lock);
            errno=ENOTSUP;return -1;
        }
        for (size_t i=0;i<count;++i)
            if (!g_window.used[first+i]) {
                mutexUnlock(&g_mmap_lock);
                errno=ENOMEM;return -1;
            }
        /* Decommitting PROT_NONE destroys data, whereas Linux mprotect must
         * preserve it. Only permit PROT_NONE for untouched reservations. */
        if (prot==BS_PROT_NONE) {
            for (size_t i=0;i<count;++i)
                if (g_window.source[first+i]!=BS_NO_PAGE) {
                    mutexUnlock(&g_mmap_lock);
                    errno=ENOTSUP;return -1;
                }
            mutexUnlock(&g_mmap_lock);
            return 0;
        }
        bool okay=commit_window_pages_locked(first,count);
        mutexUnlock(&g_mmap_lock);
        return okay?0:-1;
    }
    for(size_t i=0;i<BS_FALLBACK_MAPS;++i)
        if(g_heapmaps[i].address &&
           (uintptr_t)address>=(uintptr_t)g_heapmaps[i].address &&
           (uintptr_t)address-(uintptr_t)g_heapmaps[i].address <
             g_heapmaps[i].length &&
           length <= g_heapmaps[i].length-
             ((uintptr_t)address-(uintptr_t)g_heapmaps[i].address)) {
            bool is_reserve=g_heapmaps[i].reserve;
            mutexUnlock(&g_mmap_lock);
            if (is_reserve && !(prot&BS_PROT_EXEC)) return 0;
            /* The heap pages are already readable and writable. Do not
             * claim to enforce PROT_NONE or executable permissions.
             */
            if(prot&BS_PROT_EXEC || prot==BS_PROT_NONE) {
                errno=ENOTSUP;return -1;
            }
            return 0;
        }
    mutexUnlock(&g_mmap_lock);
    errno=ENOMEM;
    return -1;
}

int bs_madvise(void *address,size_t length,int advice) {
    if(advice!=BS_MADV_DONTNEED && advice!=BS_MADV_NORMAL) {
        errno=EINVAL;return -1;
    }
    if (!length) return 0;
    if (!address || ((uintptr_t)address&(BS_PAGE_SIZE-1))) {
        errno=EINVAL;return -1;
    }
    mutexLock(&g_mmap_lock);
    size_t first=0,count=0;
    if(range_in_window(address,length,&first,&count)) {
        for (size_t i=0;i<count;++i)
            if (!g_window.used[first+i]) {
                mutexUnlock(&g_mmap_lock);
                errno=ENOMEM;return -1;
            }
        /* MADV_NORMAL restores the default paging advice, not page contents.
         * Our owned window has only this normal policy, so it needs no
         * kernel transition and must NOT discard any existing page data. */
        if (advice==BS_MADV_DONTNEED)
            for(size_t i=first;i<first+count;++i)
                if(g_window.source[i]!=BS_NO_PAGE)
                    memset(g_window.base+i*BS_PAGE_SIZE,0,BS_PAGE_SIZE);
        mutexUnlock(&g_mmap_lock);
        return 0;
    }
    for(size_t i=0;i<BS_FALLBACK_MAPS;++i)
        if(g_heapmaps[i].address &&
           (uintptr_t)address>=(uintptr_t)g_heapmaps[i].address &&
           (uintptr_t)address-(uintptr_t)g_heapmaps[i].address <
             g_heapmaps[i].length &&
           length <= g_heapmaps[i].length-
             ((uintptr_t)address-(uintptr_t)g_heapmaps[i].address)) {
            if(advice==BS_MADV_DONTNEED) memset(address,0,length);
            mutexUnlock(&g_mmap_lock);
            return 0;
        }
    mutexUnlock(&g_mmap_lock);
    errno=ENOMEM;
    return -1;
}
