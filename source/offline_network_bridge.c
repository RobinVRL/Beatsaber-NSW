/* Original Quest socket APIs must have the correct AArch64 signatures even
 * when the owner explicitly chooses an offline-only port. Every operation
 * requiring a network socket fails with ENETDOWN; DNS resolution fails with
 * EAI_FAIL and leaves no allocated addrinfo. No Horizon socket is opened and
 * no remote or local network activity is attempted. MIT license. */
#include "offline_network_bridge.h"
#include <errno.h>

static int offline_errno(void) { errno = ENETDOWN; return -1; }

int bs_offline_socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    return offline_errno();
}
int bs_offline_socketpair(int domain, int type, int protocol, int sv[2]) {
    (void)domain; (void)type; (void)protocol;
    if (sv) { sv[0] = -1; sv[1] = -1; }
    return offline_errno();
}
int bs_offline_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)fd; (void)addr; (void)len; return offline_errno();
}
int bs_offline_bind(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)fd; (void)addr; (void)len; return offline_errno();
}
int bs_offline_listen(int fd, int backlog) {
    (void)fd; (void)backlog; return offline_errno();
}
int bs_offline_accept(int fd, struct sockaddr *addr, socklen_t *len) {
    (void)fd; (void)addr; (void)len; return offline_errno();
}
int bs_offline_shutdown(int fd, int how) {
    (void)fd; (void)how; return offline_errno();
}
int bs_offline_getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
    (void)fd; (void)addr; (void)len; return offline_errno();
}
int bs_offline_getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
    (void)fd; (void)addr; (void)len; return offline_errno();
}
int bs_offline_getsockopt(int fd, int level, int option, void *value,
                          socklen_t *len) {
    (void)fd; (void)level; (void)option; (void)value; (void)len;
    return offline_errno();
}
int bs_offline_setsockopt(int fd, int level, int option, const void *value,
                          socklen_t len) {
    (void)fd; (void)level; (void)option; (void)value; (void)len;
    return offline_errno();
}
ssize_t bs_offline_send(int fd, const void *data, size_t length, int flags) {
    (void)fd; (void)data; (void)length; (void)flags;
    return offline_errno();
}
ssize_t bs_offline_recv(int fd, void *data, size_t length, int flags) {
    (void)fd; (void)data; (void)length; (void)flags;
    return offline_errno();
}
ssize_t bs_offline_sendto(int fd, const void *data, size_t length, int flags,
                          const struct sockaddr *addr, socklen_t len) {
    (void)fd; (void)data; (void)length; (void)flags; (void)addr; (void)len;
    return offline_errno();
}
ssize_t bs_offline_recvfrom(int fd, void *data, size_t length, int flags,
                            struct sockaddr *addr, socklen_t *len) {
    (void)fd; (void)data; (void)length; (void)flags; (void)addr; (void)len;
    return offline_errno();
}
ssize_t bs_offline_sendmsg(int fd, const struct msghdr *message, int flags) {
    (void)fd; (void)message; (void)flags; return offline_errno();
}
ssize_t bs_offline_recvmsg(int fd, struct msghdr *message, int flags) {
    (void)fd; (void)message; (void)flags; return offline_errno();
}
int bs_offline_getaddrinfo(const char *node, const char *service,
                           const struct addrinfo *hints, struct addrinfo **out) {
    (void)node; (void)service; (void)hints;
    if (out) *out = NULL;
    return EAI_FAIL;
}
int bs_offline_getnameinfo(const struct sockaddr *addr, socklen_t length,
                           char *host, socklen_t host_length,
                           char *service, socklen_t service_length, int flags) {
    (void)addr; (void)length; (void)host; (void)host_length;
    (void)service; (void)service_length; (void)flags;
    return EAI_FAIL;
}
void bs_offline_freeaddrinfo(struct addrinfo *info) {
    /* This offline resolver only ever returns NULL, so there is no owned
     * addrinfo to release. Do not free an arbitrary foreign runtime object. */
    (void)info;
}
