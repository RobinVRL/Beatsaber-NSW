/* Included via -include in both translation units of the host-only test.
 * Never used in a Switch build; the standard SD path remains unchanged.
 */
#ifndef BS_QUEST_ROOT
#define BS_QUEST_ROOT "../import/quest-engine/APK/lib/arm64-v8a/"
#endif
#ifdef _WIN32
int setenv(const char *name, const char *value, int overwrite);
#endif
