/* Compile with devkitA64 -Werror to verify original Android ARM64 ABI.
 * Native Horizon <netdb.h> uses a different addrinfo pointer ordering.
 */
#include <stddef.h>
#include <stdint.h>
#include "android_socket_bridge.h"
#include "android_audio_bridge.h"

_Static_assert(sizeof(void *) == 8, "Quest needs AArch64 pointer ABI");
_Static_assert(offsetof(BsAndroidAddrInfo, ai_flags) == 0,
               "Bionic ai_flags offset");
_Static_assert(offsetof(BsAndroidAddrInfo, ai_family) == 4,
               "Bionic ai_family offset");
_Static_assert(offsetof(BsAndroidAddrInfo, ai_addrlen) == 16,
               "Bionic ai_addrlen offset");
_Static_assert(offsetof(BsAndroidAddrInfo, ai_addr) == 24,
               "Bionic ai_addr offset");
_Static_assert(offsetof(BsAndroidAddrInfo, ai_canonname) == 32,
               "Bionic ai_canonname offset");
_Static_assert(offsetof(BsAndroidAddrInfo, ai_next) == 40,
               "Bionic ai_next offset");
_Static_assert(sizeof(BsAndroidAddrInfo) == 48,
               "Bionic addrinfo size");

static void check_function_signatures(void) {
    int (*resolve)(const char *, const char *, const BsAndroidAddrInfo *,
                   BsAndroidAddrInfo **) = bs_android_getaddrinfo;
    void (*free_resolution)(BsAndroidAddrInfo *) = bs_android_freeaddrinfo;
    void (*lifecycle)(BsAudioGuestThreadInit, BsAudioGuestThreadExit,
                      void *) = bs_android_audio_set_guest_thread_lifecycle;
    (void)resolve;
    (void)free_resolution;
    (void)lifecycle;
}

int main(void) {
    check_function_signatures();
    return 0;
}
