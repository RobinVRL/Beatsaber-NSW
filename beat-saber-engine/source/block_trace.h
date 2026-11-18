#ifndef BS_BLOCK_TRACE_H
#define BS_BLOCK_TRACE_H

/* Lock-free record of which thread is currently blocked in which bridged
 * primitive, on which object, and from which guest call site. Intended for
 * the host scheduler watchdog, which must diagnose deadlocks without taking
 * any lock that could participate in them. Diagnostic only. */

#include <stddef.h>
#include <stdint.h>

enum {
    BS_BLOCK_NONE = 0,
    BS_BLOCK_MUTEX,
    BS_BLOCK_COND_WAIT,
    BS_BLOCK_COND_TIMEDWAIT,
    BS_BLOCK_SEM_WAIT,
    BS_BLOCK_LOOPER_POLL,
    BS_BLOCK_PIPE_READ,
    BS_BLOCK_PIPE_WRITE,
    BS_BLOCK_POLL,
    BS_BLOCK_FUTEX,
    BS_BLOCK_JOIN,
};

typedef struct BsBlockInfo {
    uint64_t thread_id;
    int op;
    uintptr_t object;
    uintptr_t caller;
    uint64_t since_tick;
} BsBlockInfo;

/* Enter returns a cookie for leave (record index + 1, or 0 if full). */
unsigned bs_block_enter(int op, const void *object, const void *caller);
void bs_block_leave(unsigned cookie);
size_t bs_block_snapshot(BsBlockInfo *out, size_t capacity);
const char *bs_block_op_name(int op);

/* Last acquirer of a bridged guest mutex (hashed by object; may be stale or
 * overwritten by a colliding object, which is fine for diagnostics). */
void bs_block_note_owner(const void *object, const void *caller);
void bs_block_clear_owner(const void *object);
int bs_block_lookup_owner(uintptr_t object, uint64_t *thread_id,
                          uintptr_t *caller);

#endif
