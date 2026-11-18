/* Bounded host-only regression. The callbacks here are deliberately test
 * doubles; they never execute Java/DEX or claim a live Activity. */
#define _CRT_SECURE_NO_WARNINGS 1
#include "../source/unity_java_preinit.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_execute_count;
static int g_readback_count;

static int check_host(void *user, BsUnityJavaPreinitRequirement requirement,
                      const void *value) {
    (void)user;
    if (requirement < BS_UJP_VM_ATTACHED) return value != NULL;
    return value == NULL || requirement == BS_UJP_HANDLER_CONSTRUCTOR ||
           requirement == BS_UJP_FRAMELAYOUT_FACTORY ||
           requirement == BS_UJP_SURFACEVIEW_FACTORY;
}

static int test_invoke(void *user, const uint8_t *dex, size_t size,
                       uint32_t oncreate, uint32_t constructor,
                       uint32_t dex_pc, void *activity) {
    assert(user && dex && size == BS_UNITY_JAVA_DEX_SIZE);
    assert(oncreate == BS_UJP_GAME_ONCREATE_CODE);
    assert(constructor == BS_UJP_UNITY_PLAYER_CTOR_CODE);
    assert(dex_pc == BS_UJP_UNITY_ACTIVITY_WRITE_PC);
    assert(activity == user);
    ++g_execute_count;
    return 1;
}

static int test_readback(void *user, BsUnityJavaPreinitObservation *out) {
    assert(user && out);
    ++g_readback_count;
    /* A forged/stale PC must fail, even when a callback claimed it executed. */
    out->last_code_offset = BS_UJP_UNITY_PLAYER_CTOR_CODE;
    out->last_dex_pc = BS_UJP_UNITY_CONTEXT_WRITE_PC;
    out->static_current_context = user;
    out->static_current_activity = user;
    out->original_activity_instanceof = 1;
    return 1;
}

static uint8_t *read_file(const char *name, size_t *size) {
    FILE *input = fopen(name, "rb");
    assert(input);
    assert(fseek(input, 0, SEEK_END) == 0);
    const long length = ftell(input);
    assert(length > 0);
    assert(fseek(input, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)length);
    assert(bytes);
    assert(fread(bytes, 1, (size_t)length, input) == (size_t)length);
    assert(fclose(input) == 0);
    *size = (size_t)length;
    return bytes;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    size_t size = 0;
    uint8_t *dex = read_file(argv[1], &size);
    assert(size == BS_UNITY_JAVA_DEX_SIZE);

    BsUnityJavaPreinitReport audit = {0};
    assert(bs_unity_java_preinit_verify_dex(dex, size, &audit) == 1);
    assert(audit.blockers == 0 && audit.dex_crc32 == BS_UNITY_JAVA_DEX_CRC32);
    assert(audit.expected_code_anchors >= 16);
    assert(audit.verified_code_anchors == audit.expected_code_anchors);

    /* Every code anchor must reflect ORIGINAL DEX, not merely a declared
     * method name or a symbolic JNI class. */
    dex[BS_UJP_UNITY_PLAYER_CTOR_CODE + 16 +
        BS_UJP_UNITY_ACTIVITY_WRITE_PC * 2] ^= 0x01;
    BsUnityJavaPreinitReport corrupt = {0};
    assert(bs_unity_java_preinit_verify_dex(dex, size, &corrupt) == 0);
    assert(corrupt.blockers & BS_UJP_BLOCK_DEX);
    dex[BS_UJP_UNITY_PLAYER_CTOR_CODE + 16 +
        BS_UJP_UNITY_ACTIVITY_WRITE_PC * 2] ^= 0x01;
    assert(bs_unity_java_preinit_verify_dex(dex, size - 1, &corrupt) == 0);

    BsUnityJavaPreinitReport missing = {0};
    assert(bs_unity_java_preinit_check_host(NULL, &missing) == 0);
    assert(missing.blockers & (UINT64_C(1) << BS_UJP_ACTIVITY));
    assert(missing.blockers & (UINT64_C(1) << BS_UJP_RESOURCES));
    assert(missing.blockers & (UINT64_C(1) << BS_UJP_HANDLER_CONSTRUCTOR));
    assert(missing.blockers & (UINT64_C(1) << BS_UJP_NATIVE_LIBRARY_LOADER));
    assert(missing.blockers & BS_UJP_BLOCK_CONTEXT_IDENTITY);

    /* These pointers represent the test fixtures only. In real integration,
     * the callback MUST establish actual Java object and service identity. */
    int fake_objects[14] = {0};
    BsUnityJavaPreinitHost host = {
        .activity = &fake_objects[0], .context = &fake_objects[0],
        .resources = &fake_objects[1], .configuration = &fake_objects[2],
        .asset_manager = &fake_objects[3],
        .application_info = &fake_objects[4],
        .app_metadata = &fake_objects[5],
        .native_library_dir = "/original/native/lib/",
        .intent = &fake_objects[6], .package_manager = &fake_objects[7],
        .class_loader = &fake_objects[8], .window = &fake_objects[9],
        .native_surface = &fake_objects[10], .ui_thread = &fake_objects[11],
        .verify = check_host
    };
    BsUnityJavaPreinitReport host_report = {0};
    assert(bs_unity_java_preinit_check_host(&host, &host_report) == 1);
    host.context = &fake_objects[12];
    host_report = (BsUnityJavaPreinitReport){0};
    assert(bs_unity_java_preinit_check_host(&host, &host_report) == 0);
    assert(host_report.blockers & BS_UJP_BLOCK_CONTEXT_IDENTITY);
    host.context = host.activity;
    host.verify = NULL;
    host_report = (BsUnityJavaPreinitReport){0};
    assert(bs_unity_java_preinit_check_host(&host, &host_report) == 0);
    assert(host_report.blockers & (UINT64_C(1) << BS_UJP_NATIVE_LIBRARY_LOADER));
    host.verify = check_host;

    BsUnityJavaPreinitObservation observation = {0};
    BsUnityJavaPreinitReport run_report = {0};
    /* Even complete-looking host values cannot imply a successful Java
     * assignment when the original DEX executor does not exist. */
    assert(bs_unity_java_preinit_run(dex, size, &host, NULL,
                                     &observation, &run_report) == 0);
    assert(run_report.blockers & BS_UJP_BLOCK_EXECUTOR);
    assert(!run_report.reached_context_write &&
           !run_report.reached_activity_write);
    assert(g_execute_count == 0);

    const BsUnityJavaPreinitExecutor false_executor = {
        .user = host.activity,
        .execute_to_checkpoint = test_invoke,
        .readback = test_readback
    };
    assert(bs_unity_java_preinit_run(dex, size, &host, &false_executor,
                                     &observation, &run_report) == 0);
    assert(run_report.blockers & BS_UJP_BLOCK_CHECKPOINT);
    assert(g_execute_count == 1 && g_readback_count == 1);
    assert(!run_report.reached_activity_write);
    assert(!observation.static_current_activity);

    /* Missing JNI/Thread services block execution before invoking any guest
     * callback, even when a test executor is supplied. */
    host.ui_thread = NULL;
    assert(bs_unity_java_preinit_run(dex, size, &host, &false_executor,
                                     &observation, &run_report) == 0);
    assert(run_report.blockers & (UINT64_C(1) << BS_UJP_UI_THREAD));
    assert(g_execute_count == 1);
    free(dex);
    puts("PASS: original DEX identity/16 opcodes, explicit Java host services, "
         "absent executor, forged checkpoint and strict fail-closed behavior");
    return 0;
}
