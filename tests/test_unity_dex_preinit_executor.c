/* Executable original-Dalvik bridge integration test. The host's Activity
 * token and verify callback are TEST FIXTURES; there is no actual Android
 * Activity. This test proves the original DEX interpreter stops before
 * synthetic static assignments or fabricated checkpoint completion. */
#define _CRT_SECURE_NO_WARNINGS 1
#include "../source/unity_dex_preinit_executor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int activity_token;
static int other_host_values[16];

static int validate_fixture_activity(void *user, void *activity) {
    (void)user;
    return activity == &activity_token;
}

static int test_only_verify(void *user, BsUnityJavaPreinitRequirement which,
                            const void *value) {
    (void)user;
    (void)which;
    (void)value;
    return 1;
}

static uint8_t *read_original_dex(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    assert(f && fseek(f, 0, SEEK_END) == 0);
    long count = ftell(f);
    assert(count == BS_UNITY_JAVA_DEX_SIZE);
    assert(fseek(f, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)count);
    assert(bytes && fread(bytes, 1, (size_t)count, f) == (size_t)count);
    assert(fclose(f) == 0);
    *size = (size_t)count;
    return bytes;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    size_t size = 0;
    uint8_t *dex = read_original_dex(argv[1], &size);
    BsDexHost dex_host = {.validate_activity = validate_fixture_activity};
    BsDexPreinitBridge bridge;
    bs_dex_preinit_bridge_init(&bridge, &dex_host, bs_dex_null(), 1000);
    BsUnityJavaPreinitExecutor executor =
        bs_dex_preinit_bridge_executor(&bridge);
    assert(executor.execute_to_checkpoint && executor.readback);

    /* The preinit gate can check every host prerequisite, but a mocked
     * verifier is NOT sufficient for an original DEX execution checkpoint. */
    BsUnityJavaPreinitHost host = {
        .context = &activity_token,
        .activity = &activity_token,
        .resources = &other_host_values[0],
        .configuration = &other_host_values[1],
        .asset_manager = &other_host_values[2],
        .application_info = &other_host_values[3],
        .app_metadata = &other_host_values[4],
        .native_library_dir = "/measured/native/lib",
        .intent = &other_host_values[5],
        .package_manager = &other_host_values[6],
        .class_loader = &other_host_values[7],
        .window = &other_host_values[8],
        .native_surface = &other_host_values[9],
        .ui_thread = &other_host_values[10],
        .verify = test_only_verify
    };
    BsUnityJavaPreinitObservation observation = {0};
    BsUnityJavaPreinitReport report = {0};
    assert(bs_unity_java_preinit_run(dex, size, &host, &executor,
                                     &observation, &report) == 0);
    assert(report.blockers & BS_UJP_BLOCK_CHECKPOINT);
    assert(report.verified_code_anchors == report.expected_code_anchors);
    assert(!report.reached_context_write && !report.reached_activity_write);
    BsDexStop stop = bs_dex_preinit_bridge_last_stop(&bridge);
    assert(stop.kind == BS_DEX_STOP_HOST_MISSING);
    assert(stop.steps == 9 && stop.depth == 7);
    assert(stop.code_off == 0x22e9d8 && stop.dex_pc == 2);
    assert(stop.opcode == 0x70);
    assert(stop.host_class &&
           !strcmp(stop.host_class, "Landroid/view/SurfaceView;"));
    assert(stop.host_method && !strcmp(stop.host_method, "<init>"));
    assert(executor.readback(executor.user, &observation) == 0);
    assert(bs_dex_preinit_readback(bridge.vm, &dex_host, &activity_token,
                                  &observation) == 0);
    assert(!observation.static_current_context &&
           !observation.static_current_activity);
    printf("PASS: actual original DEX interpreter adapted to Java preinit "
           "gate; %u guest instructions, stopped at %s.%s code=0x%x PC=0x%x\n",
           stop.steps, stop.host_class, stop.host_method,
           stop.code_off, stop.dex_pc);
    bs_dex_preinit_bridge_destroy(&bridge);
    assert(bs_dex_preinit_bridge_last_stop(&bridge).kind ==
           BS_DEX_STOP_INVALID);
    free(dex);
    return 0;
}
