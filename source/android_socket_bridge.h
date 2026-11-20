/* Android Linux sockaddr/socket flags -> Horizon libnx BSD ABI.
 * Never passes a Linux sockaddr or option number straight to libnx.
 * SPDX-License-Identifier: MIT */
#ifndef BS_ANDROID_SOCKET_BRIDGE_H
#define BS_ANDROID_SOCKET_BRIDGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
struct sockaddr;

bool bs_android_network_init(void);
bool bs_android_network_ready(void);
bool bs_android_socket_abi_selftest(void);
void bs_android_network_exit(void);
int bs_android_socket(int domain, int type, int protocol);
int bs_android_connect(int fd, const void *address, unsigned length);
int bs_android_bind(int fd, const void *address, unsigned length);
int bs_android_listen(int fd, int backlog);
int bs_android_accept(int fd, void *address, unsigned *length);
int bs_android_getsockname(int fd, void *address, unsigned *length);
int bs_android_getpeername(int fd, void *address, unsigned *length);
int bs_android_shutdown(int fd, int how);
ssize_t bs_android_send(int fd, const void *buffer, size_t length, int flags);
ssize_t bs_android_recv(int fd, void *buffer, size_t length, int flags);
ssize_t bs_android_sendto(int fd, const void *buffer, size_t length,
                         int flags, const void *address, unsigned addrlen);
ssize_t bs_android_recvfrom(int fd, void *buffer, size_t length,
                           int flags, void *address, unsigned *addrlen);
int bs_android_setsockopt(int fd, int level, int option,
                         const void *data, unsigned length);
int bs_android_getsockopt(int fd, int level, int option,
                         void *data, unsigned *length);
int bs_android_inet_pton(int family, const char *text, void *address);
const char *bs_android_inet_ntop(int family, const void *address,
                                char *text, unsigned length);
uint32_t bs_android_inet_addr(const char *text);

/* Bionic ARM64 struct addrinfo has ai_addr BEFORE ai_canonname. libnx's
 * BSD netdb.h orders those fields the opposite way. Callers must use this
 * guest layout: never pass it directly to Horizon getaddrinfo/freeaddrinfo.
 */
typedef struct BsAndroidAddrInfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    uint32_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct BsAndroidAddrInfo *ai_next;
} BsAndroidAddrInfo;

int bs_android_getaddrinfo(const char *node, const char *service,
                           const BsAndroidAddrInfo *hints,
                           BsAndroidAddrInfo **result);
void bs_android_freeaddrinfo(BsAndroidAddrInfo *result);
int bs_android_getnameinfo(const void *guest_address, unsigned length,
                           char *host, unsigned host_length,
                           char *service, unsigned service_length, int flags);
const char *bs_android_gai_strerror(int error);

/* Pure Linux<->BSD conversions for tests; never do network I/O. */
int bs_android_sockaddr_guest_to_native(const void *address, unsigned length,
                                       void *out, size_t capacity,
                                       unsigned *out_length);
int bs_android_sockaddr_native_to_guest(const void *address, unsigned length,
                                       void *out, size_t capacity,
                                       unsigned *out_length);
int bs_android_socket_flags_guest_to_native(int flags);
int bs_android_socket_family_guest_to_native(int family);
#endif
