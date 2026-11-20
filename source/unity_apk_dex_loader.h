/* Read the ORIGINAL, unmodified Quest APK classes.dex from its ZIP archive.
 * A verified DEX image is data for an actual Java interpreter, not evidence
 * that any Java class or Android Activity has been executed. */
#ifndef BS_UNITY_APK_DEX_LOADER_H
#define BS_UNITY_APK_DEX_LOADER_H

#include <stddef.h>
#include <stdint.h>

typedef struct BsUnityOriginalDexImage {
    uint8_t *data;
    size_t size;
} BsUnityOriginalDexImage;

/* Returns 1 only for the pinned Quest 1.46 classes.dex inside an ordinary
 * non-encrypted ZIP32 APK. Rejects duplicates and deflated/untrusted content.
 * No guest bytecode is run and no Android state is published. */
int bs_unity_apk_load_original_dex(const char *apk_path,
                                   BsUnityOriginalDexImage *out);
void bs_unity_apk_free_original_dex(BsUnityOriginalDexImage *image);

#endif
