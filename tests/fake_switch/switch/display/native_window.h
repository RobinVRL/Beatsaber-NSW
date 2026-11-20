/* Only for JNI host tests built with -D__SWITCH__ and -Itests/fake_switch.
 * Models the libnx validation boundary so test code can exercise its
 * positive/negative paths without claiming a physical Switch surface. */
#ifndef BS_JNI_TEST_FAKE_NWINDOW_H
#define BS_JNI_TEST_FAKE_NWINDOW_H
#include <stdbool.h>
#include <stdint.h>
typedef struct NWindow { uint32_t live_token; } NWindow;
static inline bool nwindowIsValid(NWindow *window) {
    return window && window->live_token == UINT32_C(0x53464e57);
}
#endif
