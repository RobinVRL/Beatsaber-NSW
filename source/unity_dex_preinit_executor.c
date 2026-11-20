#include "unity_dex_preinit_executor.h"
#include <string.h>

/* Unlike a mock callback, this executor invokes the actual original DEX
 * instructions inside BsDexVM. The VM itself validates original whole-file
 * identity, dispatches original DEX method bodies, and stops at the first
 * missing Android service without fabricating return values. */
static int execute_original_to_checkpoint(
    void *user, const uint8_t *dex, size_t size,
    uint32_t oncreate_code_offset, uint32_t constructor_code_offset,
    uint32_t checkpoint_dex_pc, void *activity) {
    BsDexPreinitBridge *bridge = user;
    if (!bridge || bridge->vm || !bridge->host.validate_activity ||
        !activity || oncreate_code_offset != BS_UJP_GAME_ONCREATE_CODE ||
        constructor_code_offset != BS_UJP_UNITY_PLAYER_CTOR_CODE ||
        checkpoint_dex_pc != BS_UJP_UNITY_ACTIVITY_WRITE_PC) return 0;
    bridge->last_stop = (BsDexStop){0};
    bridge->activity = NULL;
    bridge->vm = bs_dex_vm_create(dex, size, &bridge->host);
    if (!bridge->vm) return 0;
    if (!bs_dex_vm_start_activity_oncreate(bridge->vm, activity,
                                           bridge->saved_state)) {
        bridge->last_stop.kind = BS_DEX_STOP_HOST_MISSING;
        bridge->last_stop.detail =
            "actual UnityPlayerGameActivity host identity not established";
        return 0;
    }
    bridge->activity = activity;
    const uint32_t limit = bridge->max_instructions ?
        bridge->max_instructions : 50000u;
    bridge->last_stop = bs_dex_vm_run(bridge->vm, limit);
    return bridge->last_stop.kind == BS_DEX_STOP_CHECKPOINT &&
           bridge->last_stop.code_off == BS_UJP_UNITY_PLAYER_CTOR_CODE &&
           bridge->last_stop.dex_pc == BS_UJP_UNITY_ACTIVITY_WRITE_PC;
}

/* Both the actual DEX program counter AND the interpreted instance/static
 * fields are checked. A VM-local object has no Android host reference: the
 * result must remain blocked until a real class host materializes it. */
int bs_dex_preinit_readback(BsDexVM *vm, const BsDexHost *host, void *activity,
                            BsUnityJavaPreinitObservation *out) {
    if (!vm || !host || !out || !activity || !host->validate_activity) return 0;
    const BsDexStop stop = bs_dex_vm_stop(vm);
    BsDexCheckpointSnapshot s = {0};
    if (!bs_dex_vm_checkpoint_snapshot(vm, &s) ||
        !s.original_activity_instance_validated ||
        !s.verified_activity_branch || !s.unity_class_initialized)
        return 0;
    void *context = bs_dex_host_pointer(vm, s.player_context);
    void *player_activity = bs_dex_host_pointer(vm, s.player_activity);
    void *static_context = bs_dex_host_pointer(vm, s.static_current_context);
    void *static_activity = bs_dex_host_pointer(vm, s.static_current_activity);
    void *player = bs_dex_host_pointer(vm, s.unity_player);
    void *handler = bs_dex_host_pointer(vm, s.handler);
    void *ui_thread = bs_dex_host_pointer(vm, s.ui_thread);
    void *frame_layout = bs_dex_host_pointer(vm, s.frame_layout);
    void *surface_view = bs_dex_host_pointer(vm, s.surface_view);
    if (!player || !handler || !ui_thread || !frame_layout || !surface_view ||
        context != activity || player_activity != activity ||
        static_context != activity || static_activity != activity)
        return 0;
    /* The host validate_activity callback must attest that an existing real
     * UnityPlayerGameActivity was constructed with its original <clinit>,
     * including System.loadLibrary("game"). This is NOT inferred from a
     * symbolic class handle or JNI RegisterNatives success. */
    if (host->validate_activity(host->user, activity) != 1)
        return 0;
    *out = (BsUnityJavaPreinitObservation){
        .last_code_offset = stop.code_off,
        .last_dex_pc = stop.dex_pc,
        .unity_player = player,
        .unity_player_context = context,
        .unity_player_activity = activity,
        .static_current_context = static_context,
        .static_current_activity = static_activity,
        .handler = handler,
        .ui_thread = ui_thread,
        .frame_layout = frame_layout,
        .surface_view = surface_view,
        .game_class_initialized = 1,
        .unity_class_initialized = 1,
        .game_native_library_loaded = 1,
        .original_activity_instanceof = 1,
        .java_exception_pending = 0
    };
    return 1;
}

static int readback_original_runtime(void *user,
                                      BsUnityJavaPreinitObservation *out) {
    BsDexPreinitBridge *bridge = user;
    return bridge && bs_dex_preinit_readback(bridge->vm, &bridge->host,
                                             bridge->activity, out);
}

void bs_dex_preinit_bridge_init(BsDexPreinitBridge *bridge,
                                const BsDexHost *host,
                                BsDexValue saved_state,
                                uint32_t max_instructions) {
    if (!bridge) return;
    memset(bridge, 0, sizeof *bridge);
    if (host) bridge->host = *host;
    bridge->saved_state = saved_state;
    bridge->max_instructions = max_instructions;
}

BsUnityJavaPreinitExecutor bs_dex_preinit_bridge_executor(
    BsDexPreinitBridge *bridge) {
    BsUnityJavaPreinitExecutor executor = {0};
    if (!bridge) return executor;
    executor.user = bridge;
    executor.execute_to_checkpoint = execute_original_to_checkpoint;
    executor.readback = readback_original_runtime;
    return executor;
}

void bs_dex_preinit_bridge_destroy(BsDexPreinitBridge *bridge) {
    if (!bridge) return;
    bs_dex_vm_destroy(bridge->vm);
    bridge->vm = NULL;
    bridge->activity = NULL;
    bridge->last_stop = (BsDexStop){0};
}

BsDexStop bs_dex_preinit_bridge_last_stop(const BsDexPreinitBridge *bridge) {
    if (bridge) return bridge->last_stop;
    BsDexStop unavailable = {0};
    unavailable.kind = BS_DEX_STOP_INVALID;
    unavailable.detail = "DEX preinit executor not initialized";
    return unavailable;
}
