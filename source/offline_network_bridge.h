/* Offline network boundary for the original Quest runtime. MIT license. */
#ifndef BS_OFFLINE_NETWORK_BRIDGE_H
#define BS_OFFLINE_NETWORK_BRIDGE_H
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

int bs_offline_socket(int domain, int type, int protocol);
int bs_offline_socketpair(int domain, int type, int protocol, int sv[2]);
int bs_offline_connect(int fd, const struct sockaddr *addr, socklen_t len);
int bs_offline_bind(int fd, const struct sockaddr *addr, socklen_t len);
int bs_offline_listen(int fd, int backlog);
int bs_offline_accept(int fd, struct sockaddr *addr, socklen_t *len);
int bs_offline_shutdown(int fd, int how);
int bs_offline_getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int bs_offline_getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int bs_offline_getsockopt(int fd, int level, int option, void *value,
                          socklen_t *len);
int bs_offline_setsockopt(int fd, int level, int option, const void *value,
                          socklen_t len);
ssize_t bs_offline_send(int fd, const void *data, size_t length, int flags);
ssize_t bs_offline_recv(int fd, void *data, size_t length, int flags);
ssize_t bs_offline_sendto(int fd, const void *data, size_t length, int flags,
                          const struct sockaddr *addr, socklen_t len);
ssize_t bs_offline_recvfrom(int fd, void *data, size_t length, int flags,
                            struct sockaddr *addr, socklen_t *len);
ssize_t bs_offline_sendmsg(int fd, const struct msghdr *message, int flags);
ssize_t bs_offline_recvmsg(int fd, struct msghdr *message, int flags);
int bs_offline_getaddrinfo(const char *node, const char *service,
                           const struct addrinfo *hints, struct addrinfo **out);
int bs_offline_getnameinfo(const struct sockaddr *addr, socklen_t length,
                           char *host, socklen_t host_length,
                           char *service, socklen_t service_length, int flags);
void bs_offline_freeaddrinfo(struct addrinfo *info);

#endif
