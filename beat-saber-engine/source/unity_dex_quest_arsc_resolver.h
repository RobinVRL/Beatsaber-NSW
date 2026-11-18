/* Bounded Android binary resources.arsc reader for the ORIGINAL Quest APK.
 *
 * All outputs derive from verified binary chunks, typed Res_value records and
 * a caller-supplied, independently observed configuration. This native C
 * resolver does not create android.content.res.Resources/AssetManager Java
 * objects, interpret themes or grant Activity.attach readiness.
 */
#ifndef BS_UNITY_DEX_QUEST_ARSC_RESOLVER_H
#define BS_UNITY_DEX_QUEST_ARSC_RESOLVER_H

#include <stddef.h>
#include <stdint.h>

#define BS_QUEST_ARSC_MAX_BYTES (1024u * 1024u)
#define BS_QUEST_ARSC_MAX_STRING_BYTES 4096u

typedef struct BsQuestArscConfiguration {
    /* ISO/BCP47 language[-REGION] when independently observed from Horizon.
     * Empty means unknown; this is not permission to assume English. */
    char language_tag[16];
    uint16_t density_dpi; /* 0 means unknown, never guessed from NWindow px */
    uint16_t sdk_version; /* 0 means unknown, not assumed from manifest */
    uint8_t orientation;  /* Android 1=portrait, 2=landscape; 0=unknown */
} BsQuestArscConfiguration;

typedef struct BsQuestArscResolver {
    uint8_t *bytes;       /* caller-owned copy of validated table */
    size_t size;
    size_t package_offset;
    size_t global_pool_offset;
    size_t type_pool_offset;
    size_t key_pool_offset;
    uint32_t crc32;
    uint32_t package_id;
    uint32_t type_id_offset;
} BsQuestArscResolver;

/* Deep-copy and validate an arbitrary bounded table; useful for both
 * resource loaders and malformed-data regressions. This does not authenticate
 * the original APK by itself. Failed parsing leaves out entirely empty. */
int bs_quest_arsc_parse(const uint8_t *bytes, size_t size,
                        BsQuestArscResolver *out);

/* Authenticate the pinned original uncompressed resources.arsc entry from
 * the same staged Quest APK, then fully validate its binary chunks. */
int bs_quest_arsc_open_apk(const char *original_apk_path,
                            BsQuestArscResolver *out);
void bs_quest_arsc_release(BsQuestArscResolver *resolver);

/* Optional known configuration attributes. Set observed language only after
 * reading a genuine OS source, never a guessed application default. Accepts
 * BCP47 language or language-REGION; script qualifiers are unsupported. */
int bs_quest_arsc_set_observed_language(
    BsQuestArscConfiguration *config, const char *language_tag);

/* Resolved values only. Returns 0 if a type is incompatible, missing,
 * complex, points to an unsupported reference, has ambiguous configs, or
 * requires unavailable qualifiers (locale, density, orientation, etc).
 * A reference to another package is rejected. No Java object is created.
 * String outputs are caller-owned UTF-8 and require sufficient capacity. */
int bs_quest_arsc_get_string(const BsQuestArscResolver *resolver,
                             uint32_t resource_id,
                             const BsQuestArscConfiguration *observed,
                             char *utf8, size_t capacity);
int bs_quest_arsc_get_boolean(const BsQuestArscResolver *resolver,
                              uint32_t resource_id,
                              const BsQuestArscConfiguration *observed,
                              int *value);
int bs_quest_arsc_get_integer(const BsQuestArscResolver *resolver,
                              uint32_t resource_id,
                              const BsQuestArscConfiguration *observed,
                              int32_t *value);

/* Type names are read from the actual UTF-16 package string pool; entry
 * names from its UTF-8 key pool. No hardcoded resource name list. */
int bs_quest_arsc_get_name(const BsQuestArscResolver *resolver,
                           uint32_t resource_id,
                           char *type_name, size_t type_capacity,
                           char *entry_name, size_t entry_capacity);

#endif
