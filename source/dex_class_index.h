/* Read-only class_def index of the ORIGINAL Quest classes.dex.
 *
 * Records every class DEFINED by the APK (internal name, e.g.
 * "com/unity3d/player/UnityPlayer") with its declared superclass. This is
 * the data ART's PathClassLoader consults to decide whether an app class
 * exists; it never executes bytecode or class initializers.
 */
#ifndef BS_DEX_CLASS_INDEX_H
#define BS_DEX_CLASS_INDEX_H

#include <stddef.h>
#include <stdint.h>

/* Build the index from a verified DEX image. The image may be freed after
 * this returns. Returns the number of indexed classes, 0 on failure. */
size_t bs_dex_class_index_build(const uint8_t *dex, size_t size);
void bs_dex_class_index_reset(void);
size_t bs_dex_class_index_count(void);

/* 1 if `internal_name` is defined by the APK DEX. When `super_out` is not
 * NULL the declared superclass internal name is written ("" if none). */
int bs_dex_class_index_lookup(const char *internal_name,
                              char *super_out, size_t super_cap);

#endif
