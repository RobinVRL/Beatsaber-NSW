/* Writes into executable guest code (mod hooks).
 *
 * Android mods (Scotland2 / flamingo / beatsaber-hook) install inline hooks by
 * mprotect()ing game text RWX and storing to it directly. Horizon never maps
 * memory W+X, so those stores fault. The exception handler calls
 * bs_code_write_fault(): when the faulting instruction is an ordinary store
 * to a code page, the store is performed through a temporary writable alias
 * (svcMapProcessMemory), caches are maintained, and execution resumes after
 * the instruction.
 *
 * bs_exec_alloc() hands out code pages for trampolines. They are mapped as
 * code but left RW (state AliasCode keeps them reprotectable), so mods write
 * them directly; mprotect(PROT_EXEC) later flips them to RX in place. */
#include <switch.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "so_util.h"
#include "util.h"
#include "code_write.h"

#define PAGE 0x1000ull

static void *g_alias;            /* 2 reserved pages for the RW alias */
static atomic_flag g_alias_busy = ATOMIC_FLAG_INIT;
static atomic_ulong g_emulated;
static bool g_lax_code_perms; /* RX stores succeed without faulting */

void bs_code_write_init(void) {
    if (g_alias) return;
    virtmemLock();
    void *a = virtmemFindAslr(2 * PAGE, 0);
    if (a && virtmemAddReservation(a, 2 * PAGE)) g_alias = a;
    virtmemUnlock();
    debugPrintf("[codew] alias window %p\n", g_alias);
}

static bool is_code_page(uintptr_t addr) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) return false;
    if (mi.perm & Perm_W) return false;
    if (!(mi.perm & Perm_R)) return false;
    u32 t = mi.type & 0xff;
    return t == MemType_CodeStatic || t == MemType_CodeMutable ||
           t == MemType_ModuleCodeStatic || t == MemType_ModuleCodeMutable ||
           t == MemType_ModuleCodeMutable || t == MemType_CodeReadOnly ||
           t == MemType_CodeWritable;
}

/* Write through a temporary RW alias, one page at a time. */
static bool alias_write(uintptr_t dst, const void *src, size_t len) {
    if (!g_alias) return false;
    while (atomic_flag_test_and_set(&g_alias_busy)) {}
    bool ok = true;
    const uint8_t *s = src;
    while (len && ok) {
        uintptr_t page = dst & ~(PAGE - 1);
        size_t off = dst - page, n = PAGE - off;
        if (n > len) n = len;
        Result rc = svcMapProcessMemory(g_alias, envGetOwnProcessHandle(), page, PAGE);
        if (R_FAILED(rc)) { ok = false; break; }
        memcpy((uint8_t *)g_alias + off, s, n);
        armDCacheFlush((uint8_t *)g_alias + off, n);
        svcUnmapProcessMemory(g_alias, envGetOwnProcessHandle(), page, PAGE);
        armICacheInvalidate((void *)dst, n);
        /* Emulators (Eden/yuzu JIT) may not honour IC IVAU for code that is
         * already translated; re-applying the executable permission makes
         * the kernel invalidate its instruction cache range. Without this a
         * hook removed after it has run keeps executing the stale
         * "ldr x17,#8; br x17" stub and jumps into the restored bytes. */
        {
            MemoryInfo mi; u32 pi;
            if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, page)) && (mi.perm & Perm_X))
                svcSetProcessMemoryPermission(envGetOwnProcessHandle(), page, PAGE,
                                              mi.perm & (Perm_R | Perm_X));
        }
        dst += n; s += n; len -= n;
    }
    atomic_flag_clear(&g_alias_busy);
    return ok;
}

int bs_code_write(void *dst, const void *src, size_t len) {
    return alias_write((uintptr_t)dst, src, len) ? 0 : -1;
}

static uint64_t *xreg(ThreadExceptionDump *c, unsigned n) {
    if (n < 29) return &c->cpu_gprs[n].x;
    if (n == 29) return &c->fp.x;
    if (n == 30) return &c->lr.x;
    return NULL;
}
static uint64_t rt_val(ThreadExceptionDump *c, unsigned n) {   /* 31 = XZR */
    uint64_t *r = xreg(c, n); return r ? *r : 0;
}
static uint64_t rn_val(ThreadExceptionDump *c, unsigned n) {   /* 31 = SP */
    uint64_t *r = xreg(c, n); return r ? *r : c->sp.x;
}
static void rn_set(ThreadExceptionDump *c, unsigned n, uint64_t v) {
    uint64_t *r = xreg(c, n); if (r) *r = v; else c->sp.x = v;
}
static int64_t sext(uint64_t v, unsigned bits) {
    return (int64_t)(v << (64 - bits)) >> (64 - bits);
}

/* Decode an A64 store. Fills addr/data/len and the base writeback. */
typedef struct { uint64_t addr; uint8_t data[32]; size_t len; int wb; uint64_t wb_val; unsigned rn; } Store;

static void reg_bytes(ThreadExceptionDump *c, int simd, unsigned t, size_t sz, uint8_t *out) {
    if (simd) memcpy(out, &c->fpu_gprs[t], sz);
    else { uint64_t v = rt_val(c, t); memcpy(out, &v, sz); }
}

static bool decode_store(ThreadExceptionDump *c, uint32_t i, Store *s) {
    memset(s, 0, sizeof *s);
    unsigned rt = i & 31, rn = (i >> 5) & 31;
    s->rn = rn;
    int simd = (i >> 26) & 1;
    /* Single register: STR/STRB/STRH/STUR (+SIMD). */
    if ((i & 0x3A000000u) == 0x38000000u) {
        unsigned size = i >> 30, opc = (i >> 22) & 3;
        size_t sz;
        if (!simd) { if (opc != 0) return false; sz = 1u << size; }
        else if (opc == 0) sz = 1u << size;
        else if (opc == 2 && size == 0) sz = 16;
        else return false;
        uint64_t base = rn_val(c, rn);
        if (i & (1u << 24)) {                       /* unsigned offset */
            s->addr = base + ((i >> 10) & 0xfff) * sz;
        } else if ((i & (1u << 21)) == 0) {         /* imm9 forms */
            int64_t imm = sext((i >> 12) & 0x1ff, 9);
            unsigned idx = (i >> 10) & 3;
            if (idx == 1) { s->addr = base; s->wb = 1; s->wb_val = base + imm; }
            else if (idx == 3) { s->addr = base + imm; s->wb = 1; s->wb_val = s->addr; }
            else s->addr = base + imm;              /* STUR / STTR */
        } else if (((i >> 10) & 3) == 2) {          /* register offset */
            unsigned rm = (i >> 16) & 31, opt = (i >> 13) & 7, S = (i >> 12) & 1;
            uint64_t off = rt_val(c, rm);
            if (opt == 2) off = (uint32_t)off;
            else if (opt == 6) off = (uint64_t)(int64_t)(int32_t)off;
            else if (opt != 3 && opt != 7) return false;
            unsigned sh = 0; while ((1u << sh) < sz) ++sh;
            s->addr = base + (S ? off << sh : off);
        } else return false;
        s->len = sz;
        reg_bytes(c, simd, rt, sz, s->data);
        return true;
    }
    /* Pair: STP/STNP (+SIMD). */
    if ((i & 0x3A000000u) == 0x28000000u && !((i >> 22) & 1)) {
        unsigned opc = i >> 30, idx = (i >> 23) & 3, rt2 = (i >> 10) & 31;
        size_t sz;
        if (!simd) { if (opc == 0) sz = 4; else if (opc == 2) sz = 8; else return false; }
        else { if (opc > 2) return false; sz = 4u << opc; }
        int64_t imm = sext((i >> 15) & 0x7f, 7) * (int64_t)sz;
        uint64_t base = rn_val(c, rn);
        if (idx == 1) { s->addr = base; s->wb = 1; s->wb_val = base + imm; }
        else if (idx == 3) { s->addr = base + imm; s->wb = 1; s->wb_val = s->addr; }
        else s->addr = base + imm;
        s->len = 2 * sz;
        reg_bytes(c, simd, rt, sz, s->data);
        reg_bytes(c, simd, rt2, sz, s->data + sz);
        return true;
    }
    return false;
}

bool bs_code_write_fault(ThreadExceptionDump *c) {
    uintptr_t far = c->far.x, pc = c->pc.x;
    if (!g_alias || !is_code_page(far)) return false;
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, pc)) || !(mi.perm & Perm_R)) return false;
    uint32_t insn = *(volatile uint32_t *)pc;
    Store s;
    if (!decode_store(c, insn, &s)) return false;
    /* FAR must lie within the decoded store. */
    if (far < (s.addr & ~(PAGE - 1)) || far >= s.addr + s.len + PAGE) return false;
    if (!alias_write(s.addr, s.data, s.len)) return false;
    if (s.wb) rn_set(c, s.rn, s.wb_val);
    c->pc.x = pc + 4;
    atomic_fetch_add(&g_emulated, 1);
    return true;
}

unsigned long bs_code_write_count(void) { return atomic_load(&g_emulated); }

/* ---- executable allocations ------------------------------------------- */
/* Each allocation: heap staging pages moved to a code-region address with
 * svcMapProcessCodeMemory. Initial permission is RW and the state stays
 * reprotectable, so a later mprotect(PROT_EXEC) can make it RX. */
typedef struct { uintptr_t addr; size_t size; void *src; int rx; } ExecMap;
#define MAX_EXEC 256
static ExecMap g_exec[MAX_EXEC];
static Mutex g_exec_lock;

void *so_exec_map(size_t size);
void *bs_exec_alloc(size_t size) {
    size = (size + PAGE - 1) & ~(PAGE - 1);
    void *dst = so_exec_map(size);
    if (!dst) return NULL;
    void *src = NULL;
    MemoryInfo mi; u32 pi;
    svcQueryMemory(&mi, &pi, (u64)dst);
    mutexLock(&g_exec_lock);
    for (int k = 0; k < MAX_EXEC; ++k)
        if (!g_exec[k].addr) { g_exec[k] = (ExecMap){(uintptr_t)dst, size, src, 0}; break; }
    mutexUnlock(&g_exec_lock);
    static int logged;
    if (logged++ < 8)
        debugPrintf("[codew] exec alloc %p+%#zx type=%#x perm=%u\n", dst, size, mi.type, mi.perm);
    return dst;
}

bool bs_exec_owns(const void *p, size_t len) {
    uintptr_t a = (uintptr_t)p;
    for (int k = 0; k < MAX_EXEC; ++k)
        if (g_exec[k].addr && a >= g_exec[k].addr && a + len <= g_exec[k].addr + g_exec[k].size)
            return true;
    return false;
}

/* mprotect for code pages (module text or exec allocations). */
int bs_code_mprotect(void *addr, size_t len, int prot_exec) {
    uintptr_t a = (uintptr_t)addr;
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return -1;
    u32 t = mi.type & 0xff;
    bool code = t == MemType_CodeStatic || t == MemType_CodeMutable ||
                t == MemType_ModuleCodeMutable || t == MemType_CodeReadOnly ||
                t == MemType_CodeWritable || t == MemType_ModuleCodeStatic;
    if (!code) return 1;                   /* not ours */
    if (prot_exec && (mi.perm & Perm_W) && bs_exec_owns(addr, len)) {
        Result rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), a, len, Perm_Rx);
        armDCacheFlush(addr, len);
        armICacheInvalidate(addr, len);
        if (R_FAILED(rc)) debugPrintf("[codew] RX %p failed 0x%x\n", addr, rc);
    }
    /* Text pages stay RX; writes to them are emulated by the fault handler
     * on hardware. Eden does not fault on such stores and its JIT ignores
     * the guest's cache maintenance, so patched code (hook installs and
     * hook removals) keeps running stale translations. Re-applying the
     * page permission makes the kernel flush the JIT for the range. */
    else if (mi.perm & Perm_X) {
        uintptr_t start = a & ~(PAGE - 1);
        uintptr_t end = (a + len + PAGE - 1) & ~(PAGE - 1);
        armDCacheFlush(addr, len);
        armICacheInvalidate(addr, len);
        Result rc = 0;
        /* Only where code stores don't fault (emulator): there the kernel
         * permission is not enforced for execution, so a brief RW window is
         * harmless. On hardware stores go through bs_code_write_fault. */
        if (g_lax_code_perms) {
            Handle h = envGetOwnProcessHandle();
            rc = svcSetProcessMemoryPermission(h, start, end - start, Perm_Rw);
            if (R_SUCCEEDED(rc))
                rc = svcSetProcessMemoryPermission(h, start, end - start, Perm_Rx);
        }
        static int logged;
        if (R_FAILED(rc) && logged++ < 8)
            debugPrintf("[codew] reprotect %p+%#zx failed 0x%x type=%#x perm=%u attr=%#x region=%#lx+%#lx\n", addr, len, rc, mi.type, mi.perm, mi.attr, (unsigned long)mi.addr, (unsigned long)mi.size);
    }
    return 0;
}

/* Startup self-test: emulated store into an RX code page. */
bool bs_code_write_selftest(void) {
    void *p = bs_exec_alloc(PAGE);
    if (!p) return false;
    bs_code_mprotect(p, PAGE, 1);
    MemoryInfo mi; u32 pi;
    svcQueryMemory(&mi, &pi, (u64)p);
    unsigned long before = bs_code_write_count();
    *(volatile uint32_t *)p = 0xd65f03c0u;   /* ret */
    bool wrote = *(volatile uint32_t *)p == 0xd65f03c0u;
    bool ran = false;
    if (wrote) { ((void (*)(void))p)(); ran = true; }
    g_lax_code_perms = wrote && bs_code_write_count() == before;
    debugPrintf("[codew] selftest lax=%d perm=%u wrote=%d emulated=%lu ran=%d\n", g_lax_code_perms, mi.perm, wrote,
                bs_code_write_count() - before, ran);
    return wrote && ran;
}
