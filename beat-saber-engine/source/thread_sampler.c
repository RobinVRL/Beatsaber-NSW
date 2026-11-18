/* Stall diagnostics: sample where every guest thread is executing.
 * SPDX-License-Identifier: MIT
 *
 * Horizon lets a process pause its own threads (svcSetThreadActivity) and
 * read their register context (svcGetThreadContext3). The watchdog uses this
 * to print PC/LR and a short frame-pointer chain for each registered thread,
 * which locates loops and waits inside host code (Mesa, libnx) that the
 * block tracer cannot see. Only lock-free code runs here: no malloc, no
 * debugPrintf, no newlib FILE.
 */
#include <switch.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "thread_sampler.h"
#include "so_util.h"

#define BS_SAMPLER_MAX 96

typedef struct {
    _Atomic uint32_t handle; /* 0 = free slot */
    uint64_t tid;
    _Atomic uintptr_t pthread; /* native pthread_self() of the thread */
} BsSampledThread;

static BsSampledThread g_threads[BS_SAMPLER_MAX];
/* NRO base: start of the executable mapping containing this code. */
static uintptr_t nro_base(void) {
    static uintptr_t base;
    if (!base) {
        MemoryInfo info; u32 page;
        if (R_SUCCEEDED(svcQueryMemory(&info, &page, (uintptr_t)&nro_base)))
            base = info.addr;
    }
    return base;
}

void bs_sampler_register_current(uint64_t tid) {
    Thread *self = threadGetSelf();
    if (!self) return;
    for (size_t i = 0; i < BS_SAMPLER_MAX; ++i) {
        uint32_t expected = 0;
        if (atomic_compare_exchange_strong(&g_threads[i].handle, &expected,
                                           (uint32_t)self->handle)) {
            g_threads[i].tid = tid;
            atomic_store(&g_threads[i].pthread, (uintptr_t)pthread_self());
            return;
        }
    }
}

void bs_sampler_unregister_current(void) {
    Thread *self = threadGetSelf();
    if (!self) return;
    for (size_t i = 0; i < BS_SAMPLER_MAX; ++i)
        if (atomic_load(&g_threads[i].handle) == (uint32_t)self->handle) {
            atomic_store(&g_threads[i].pthread, 0);
            atomic_store(&g_threads[i].handle, 0);
        }
}

uint32_t bs_sampler_handle_for_pthread(uintptr_t thread) {
    if (!thread) return 0;
    for (size_t i = 0; i < BS_SAMPLER_MAX; ++i)
        if (atomic_load(&g_threads[i].pthread) == thread)
            return atomic_load(&g_threads[i].handle);
    return 0;
}

static int describe(char *out, size_t size, uintptr_t addr) {
    so_module *m = so_find_module_by_addr((const void *)addr);
    if (m) {
        const char *name = strrchr(m->name, '/');
        return snprintf(out, size, "%s+0x%lx", name ? name + 1 : m->name,
                        (unsigned long)(addr - (uintptr_t)m->load_virtbase));
    }
    uintptr_t base = nro_base();
    if (addr >= base && addr - base < ((uintptr_t)256 << 20))
        return snprintf(out, size, "nro+0x%lx", (unsigned long)(addr - base));
    return snprintf(out, size, "0x%lx", (unsigned long)addr);
}

static bool readable(uintptr_t addr) {
    MemoryInfo info;
    u32 page;
    if (addr & 7) return false;
    if (R_FAILED(svcQueryMemory(&info, &page, addr))) return false;
    return (info.perm & Perm_R) && info.type != MemType_Unmapped &&
           addr + 16 <= info.addr + info.size;
}

size_t bs_sampler_dump(bs_sampler_sink sink, void *user) {
    size_t sampled = 0;
    Thread *self = threadGetSelf();
    char line[1024];
    for (size_t i = 0; i < BS_SAMPLER_MAX; ++i) {
        Handle h = (Handle)atomic_load(&g_threads[i].handle);
        if (!h || (self && h == self->handle)) continue;
        if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) continue;
        ThreadContext ctx;
        Result rc = svcGetThreadContext3(&ctx, h);
        uintptr_t frames[8] = {0};
        size_t depth = 0;
        if (R_SUCCEEDED(rc)) {
            uintptr_t fp = ctx.fp;
            while (depth < 8 && fp && readable(fp)) {
                const uintptr_t *record = (const uintptr_t *)fp;
                if (!record[1]) break;
                frames[depth++] = record[1];
                if (record[0] <= fp) break;
                fp = record[0];
            }
        }
        svcSetThreadActivity(h, ThreadActivity_Runnable);
        if (R_FAILED(rc)) continue;
        int n = snprintf(line, sizeof line, "[sample] tid=%lu pc=",
                         (unsigned long)g_threads[i].tid);
        n += describe(line + n, sizeof line - n, ctx.pc.x);
        n += snprintf(line + n, sizeof line - n, " lr=");
        n += describe(line + n, sizeof line - n, ctx.lr);
        for (size_t f = 0; f < depth && n < (int)sizeof line - 48; ++f) {
            n += snprintf(line + n, sizeof line - n, " <");
            n += describe(line + n, sizeof line - n, frames[f]);
        }
        if (n < (int)sizeof line - 1) { line[n++] = '\n'; line[n] = 0; }
        sink(user, line, (size_t)n);
        /* Full GPR and stack-top dump, used to recover live object pointers
         * of a spinning guest thread (e.g. Unity job queue owners). */
        n = snprintf(line, sizeof line, "[regs] tid=%lu sp=%#lx",
                     (unsigned long)g_threads[i].tid, (unsigned long)ctx.sp);
        for (int r = 0; r < 29 && n < (int)sizeof line - 24; ++r)
            n += snprintf(line + n, sizeof line - n, " x%d=%#lx", r,
                          (unsigned long)ctx.cpu_gprs[r].x);
        if (n < (int)sizeof line - 1) { line[n++] = '\n'; line[n] = 0; }
        sink(user, line, (size_t)n);
        if (readable(ctx.sp)) {
            n = snprintf(line, sizeof line, "[stack] tid=%lu",
                         (unsigned long)g_threads[i].tid);
            const uintptr_t *s = (const uintptr_t *)ctx.sp;
            for (int w = 0; w < 24 && readable((uintptr_t)(s + w)) &&
                            n < (int)sizeof line - 24; ++w)
                n += snprintf(line + n, sizeof line - n, " %#lx",
                              (unsigned long)s[w]);
            if (n < (int)sizeof line - 1) { line[n++] = '\n'; line[n] = 0; }
            sink(user, line, (size_t)n);
            /* Unity job scheduler: 0x72bc7c saves its caller's x20 (the
             * scheduler) at sp+32 next to return address 0x72bb28. */
            so_module *u = so_find_module_by_name("libunity.so");
            uintptr_t ret = u ? (uintptr_t)u->load_virtbase + 0x72bb28 : 0;
            for (int w = 0; u && w < 256 && readable((uintptr_t)(s + w + 6)); ++w) {
                if (s[w] != ret) continue;
                uintptr_t q = s[w + 4];
                if (!readable(q) || !readable(q + 160)) break;
                const uint32_t *q32 = (const uint32_t *)q;
                n = snprintf(line, sizeof line,
                             "[jobq] tid=%lu sched=%#lx count72=%u active128=%d words:",
                             (unsigned long)g_threads[i].tid, (unsigned long)q,
                             q32[72 / 4], (int)q32[128 / 4]);
                for (int k = 0; k < 40 && n < (int)sizeof line - 16; ++k)
                    n += snprintf(line + n, sizeof line - n, " %x", q32[k]);
                if (n < (int)sizeof line - 1) { line[n++] = '\n'; line[n] = 0; }
                sink(user, line, (size_t)n);
                break;
            }
        }
        ++sampled;
    }
    return sampled;
}
