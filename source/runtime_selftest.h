#ifndef BS_RUNTIME_SELFTEST_H
#define BS_RUNTIME_SELFTEST_H
#include <stdbool.h>

/* Checks the mapped guest's startup-critical Bionic and thread primitives. */
bool bs_runtime_probe_bootstrap_abi(void);

/* Executed only from an explicit on-console diagnostic command.
 * Tests the actual Switch C ABI bridge and local game data without running
 * any original Unity/IL2CPP constructors or rewriting owned APK/OBB files.
 */
int bs_runtime_selftest(void);

#endif
