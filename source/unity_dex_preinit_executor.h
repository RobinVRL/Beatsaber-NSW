/* Adapter from the executable original Quest DEX interpreter to the pinned
 * Unity Java preinit contract. No global JNI state or field publication.
 */
#ifndef BS_UNITY_DEX_PREINIT_EXECUTOR_H
#define BS_UNITY_DEX_PREINIT_EXECUTOR_H

#include "unity_dex_vm.h"
#include "unity_java_preinit.h"

typedef struct BsDexPreinitBridge {
    BsDexHost host;               /* callbacks owned by a real Java host */
    BsDexVM *vm;                 /* original DEX interpreter, owned here */
    BsDexStop last_stop;         /* precise last original guest instruction */
    BsDexValue saved_state;      /* observed Bundle or verified null */
    uint32_t max_instructions;   /* 0 uses conservative 50,000-instruction cap */
    void *activity;              /* actual observed UnityPlayerGameActivity */
} BsDexPreinitBridge;

/* Caller supplies actual context callbacks. A new bridge has no Java VM
 * execution until execute_to_checkpoint is invoked. All pointers must remain
 * valid until bs_dex_preinit_bridge_destroy. */
void bs_dex_preinit_bridge_init(BsDexPreinitBridge *bridge,
                                const BsDexHost *host,
                                BsDexValue saved_state,
                                uint32_t max_instructions);
BsUnityJavaPreinitExecutor bs_dex_preinit_bridge_executor(
    BsDexPreinitBridge *bridge);
void bs_dex_preinit_bridge_destroy(BsDexPreinitBridge *bridge);
BsDexStop bs_dex_preinit_bridge_last_stop(const BsDexPreinitBridge *bridge);

/* Read a live, platform-backed observation from the original checkpoint. */
int bs_dex_preinit_readback(BsDexVM *vm, const BsDexHost *host, void *activity,
                            BsUnityJavaPreinitObservation *out);

#endif
