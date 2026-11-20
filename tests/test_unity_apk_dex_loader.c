#include "unity_apk_dex_loader.h"
#include "unity_java_preinit.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    BsUnityOriginalDexImage image = {0};
    assert(!bs_unity_apk_load_original_dex(NULL, &image));
    assert(!bs_unity_apk_load_original_dex("not-an-actual-apk.zip", &image));
    assert(bs_unity_apk_load_original_dex(argv[1], &image));
    assert(image.data && image.size == BS_UNITY_JAVA_DEX_SIZE);
    uint8_t *owned_bytes = image.data;
    assert(!bs_unity_apk_load_original_dex(argv[1], &image));
    assert(image.data == owned_bytes && image.size == BS_UNITY_JAVA_DEX_SIZE);
    BsUnityJavaPreinitReport report = {0};
    assert(bs_unity_java_preinit_verify_dex(image.data, image.size, &report));
    assert(report.verified_code_anchors == report.expected_code_anchors);
    assert(report.dex_crc32 == BS_UNITY_JAVA_DEX_CRC32);
    image.data[0] ^= 1;
    assert(!bs_unity_java_preinit_verify_dex(image.data, image.size, &report));
    bs_unity_apk_free_original_dex(&image);
    assert(!image.data && !image.size);
    bs_unity_apk_free_original_dex(&image);
    puts("PASS: original Quest ZIP classes.dex extraction, full DEX identity, tamper rejection");
    return 0;
}
