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

#include "bionic_mmap.h"
#include "util.h"

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
} BsHeapMap;
static BsMmapWindow g_window;
static BsHeapMap g_heapmaps[BS_FALLBACK_MAPS];
static Mutex g_mmap_lock;
static size_t g_committed_pages;
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
    return g_window.ready && n && a<b+total && a+n>b;
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

static void window_initialize_locked(void) {
    if (g_window.initialized) return;
    g_window.initialized=true;

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
        if (memory.type==MemType_Unmapped) {
            u64 aligned=(memory.addr+(BS_OC_ALIGN-1)) &
                        ~(u64)(BS_OC_ALIGN-1);
            u64 bound=memory.addr+memory.size;
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

    virtmemLock();
    VirtmemReservation *rv=virtmemAddReservation((void *)best_start,best_size);
    virtmemUnlock();
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
    debugPrintf("[mmap] overcommit window reserved %zu MiB at %p\n",
                best_size>>20,(void *)best_start);
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

/* Horizon tracks every svcMapMemory alias as kernel memory blocks, and the
 * kernel's block pool is finite (Eden: ResourceExhausted 0xce01 after ~10k
 * single-page aliases). Contiguous runs of uncommitted window pages are
 * therefore backed by contiguous pool pages from the bump cursor and mapped
 * with ONE call; recycled single pages are used only once it is exhausted. */
static bool map_window_run_locked(size_t page,uint32_t src_page,size_t pages,
                                  size_t first,size_t count) {
    void *dst=g_window.base+page*BS_PAGE_SIZE;
    void *src=g_window.pool+(size_t)src_page*BS_PAGE_SIZE;
    Result rc=svcMapMemory(dst,src,pages*BS_PAGE_SIZE);
    if (R_FAILED(rc)) {
        debugPrintf("[mmap] svcMapMemory failed rc=0x%x dst=%p src=%p "
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
    extern char *fake_heap_end;
    char *brk=sbrk(0);
    size_t tail=(brk!=(char *)-1 && fake_heap_end>brk) ? (size_t)(fake_heap_end-brk) : 0;
    size_t free_bytes=tail+(size_t)mallinfo().fordblks;
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
        Result rc=svcUnmapMemory(dst,src,run*BS_PAGE_SIZE);
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
            debugPrintf("[mmap] svcUnmapMemory failed rc=0x%x\n",rc);
            all_unmapped=false;
        }
        i+=run;
    }
    if (!all_unmapped) errno=ENOMEM;
    return all_unmapped;
}
static void *reserve_window_locked(size_t length) {
    size_t need=page_align(length)/BS_PAGE_SIZE;
    if (!need||need>g_window.page_count) return NULL;
    const size_t step=length>=((size_t)64<<20) ?
                      BS_OC_ALIGN/BS_PAGE_SIZE : 1;
    for(size_t i=0;i+need<=g_window.page_count;i+=step) {
        size_t j=0;
        while(j<need&&!g_window.used[i+j])++j;
        if(j==need) {
            memset(g_window.used+i,1,need);
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
        g_heapmaps[i]=(BsHeapMap){p,reserved};
        tracked=true;
        break;
    }
    mutexUnlock(&g_mmap_lock);
    if(!tracked) { free(p);errno=ENOMEM;return (void *)-1; }
    return p;
}

static void *bs_mmap_impl(void *hint,size_t length,int prot,int flags,int fd,int64_t offset);
void *bs_mmap(void *hint,size_t length,int prot,int flags,
              int fd,int64_t offset) {
    void *r=bs_mmap_impl(hint,length,prot,flags,fd,offset);
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
            mutexUnlock(&g_mmap_lock);
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
        return 0;
    }
    for(size_t i=0;i<BS_FALLBACK_MAPS;++i)
        if(g_heapmaps[i].address==address) {
            if(page_align(length)!=g_heapmaps[i].length) {
                mutexUnlock(&g_mmap_lock);
                errno=ENOTSUP;return -1;
            }
            g_heapmaps[i]=(BsHeapMap){0};
            mutexUnlock(&g_mmap_lock);
            free(address);
            return 0;
        }
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
            mutexUnlock(&g_mmap_lock);
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
