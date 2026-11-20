/* Source-backed Android -> Horizon BSD sockets.
 *
 * Android uses Linux sockaddr: uint16_t sa_family at offset 0.
 * libnx uses FreeBSD sockaddr: uint8_t sa_len, uint8_t sa_family.
 * AF_INET6 is Linux 10, libnx 28; Linux socket/options/message flags also
 * differ. Passing either raw data layout would corrupt network addresses.
 *
 * Only IPv4 and IPv6 are enabled; AF_UNIX, SCM_RIGHTS, unresolved network
 * device features and unsupported options FAIL instead of returning success.
 * A live BSD service is a prerequisite for every socket operation.
 * SPDX-License-Identifier: MIT
 */
#include "android_socket_bridge.h"
#include "util.h"
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

_Static_assert(sizeof(struct sockaddr_in) == 16,
               "libnx IPv4 sockaddr size changed");
_Static_assert(sizeof(struct sockaddr_in6) == 28,
               "libnx IPv6 sockaddr size changed");
_Static_assert(offsetof(struct sockaddr_in, sin_port) == 2,
               "libnx IPv4 port offset changed");
_Static_assert(offsetof(struct sockaddr_in6, sin6_port) == 2,
               "libnx IPv6 port offset changed");
_Static_assert(AF_INET == 2 && AF_INET6 == 28,
               "libnx socket family numbers changed");

static bool g_network_ready;

bool bs_android_network_init(void) {
    if (g_network_ready) return true;
    Result result = socketInitializeDefault();
    if (R_FAILED(result)) {
        debugPrintf("[net] Horizon bsd:u init failed 0x%08x; Android network "
                    "APIs return ENOSYS (not online)\n", result);
        return false;
    }
    g_network_ready = true;
    debugPrintf("[net] Horizon bsd:u socket service initialized; Linux "
                "sockaddr/flags conversion enabled\n");
    return true;
}
bool bs_android_network_ready(void) { return g_network_ready; }
bool bs_android_socket_abi_selftest(void) {
    /* Actual AArch64 execution in Eden / on Switch. The test does no network
     * traffic and does not claim that a socket service is available. */
    const uint8_t guest_ipv4[16] = {
        2, 0, 0x1f, 0x90, 127, 0, 0, 1
    };
    uint8_t guest_ipv6[28] = {0};
    guest_ipv6[0] = 10; /* Linux AF_INET6, not BSD AF_INET6=28. */
    guest_ipv6[2] = 0x1f;
    guest_ipv6[3] = 0x90;
    guest_ipv6[23] = 1; /* ::1 */
    uint8_t native[28] = {0};
    uint8_t guest_roundtrip[28] = {0};
    unsigned native_len = 0, guest_len = 0;
    const bool ipv4 =
        bs_android_sockaddr_guest_to_native(
            guest_ipv4, sizeof guest_ipv4, native, sizeof native,
            &native_len) == 0 &&
        native_len == 16 && native[0] == 16 && native[1] == 2 &&
        memcmp(native + 2, guest_ipv4 + 2, 14) == 0 &&
        bs_android_sockaddr_native_to_guest(
            native, native_len, guest_roundtrip, sizeof guest_roundtrip,
            &guest_len) == 0 && guest_len == 16 &&
        memcmp(guest_roundtrip, guest_ipv4, sizeof guest_ipv4) == 0;
    memset(native, 0, sizeof native);
    memset(guest_roundtrip, 0, sizeof guest_roundtrip);
    const bool ipv6 =
        bs_android_sockaddr_guest_to_native(
            guest_ipv6, sizeof guest_ipv6, native, sizeof native,
            &native_len) == 0 &&
        native_len == 28 && native[0] == 28 && native[1] == AF_INET6 &&
        bs_android_sockaddr_native_to_guest(
            native, native_len, guest_roundtrip, sizeof guest_roundtrip,
            &guest_len) == 0 && guest_len == 28 &&
        memcmp(guest_roundtrip, guest_ipv6, sizeof guest_ipv6) == 0;
    const bool flags =
        bs_android_socket_flags_guest_to_native(0x40) == MSG_DONTWAIT &&
        bs_android_socket_flags_guest_to_native(0x100) == MSG_WAITALL &&
        bs_android_socket_flags_guest_to_native(0x4000) == MSG_NOSIGNAL;
    uint8_t tiny[2] = {0};
    const bool truncates =
        bs_android_sockaddr_native_to_guest(
            native, native_len, tiny, sizeof tiny, &guest_len) == 0 &&
        guest_len == 28 && tiny[0] == 10 && tiny[1] == 0;
    const bool result = ipv4 && ipv6 && flags && truncates;
    debugPrintf("[net:abi] original Linux<->Horizon IPv4=%d IPv6=%d "
                "MSG_flags=%d truncated_sockaddr=%d: %s\n",
                (int)ipv4, (int)ipv6, (int)flags, (int)truncates,
                result ? "PASS" : "FAIL");
    return result;
}
void bs_android_network_exit(void) {
    if (!g_network_ready) return;
    socketExit();
    g_network_ready = false;
}
static bool bs_net_require_service(void) {
    if (g_network_ready) return true;
    errno = ENOSYS;
    return false;
}

int bs_android_socket_family_guest_to_native(int family) {
    switch (family) {
        case 0: return AF_UNSPEC;
        case 2: return AF_INET;
        case 10: return AF_INET6;
        default: errno = EAFNOSUPPORT; return -1;
    }
}
static int bs_family_native_to_guest(int family) {
    switch (family) {
        case AF_UNSPEC: return 0;
        case AF_INET: return 2;
        case AF_INET6: return 10;
        default: errno = EAFNOSUPPORT; return -1;
    }
}

int bs_android_sockaddr_guest_to_native(const void *address, unsigned length,
                                       void *out, size_t capacity,
                                       unsigned *out_length) {
    if (!address || !out || !out_length || length < 2) {
        errno = EINVAL;
        return -1;
    }
    uint16_t guest_family;
    memcpy(&guest_family, address, sizeof guest_family);
    int family = bs_android_socket_family_guest_to_native(guest_family);
    if (family < 0) return -1;
    unsigned required = family == AF_INET ? 16 : family == AF_INET6 ? 28 : 0;
    if (!required || length < required || capacity < required) {
        errno = EINVAL;
        return -1;
    }
    memcpy(out, address, required);
    ((uint8_t *)out)[0] = (uint8_t)required;
    ((uint8_t *)out)[1] = (uint8_t)family;
    *out_length = required;
    return 0;
}
int bs_android_sockaddr_native_to_guest(const void *address, unsigned length,
                                       void *out, size_t capacity,
                                       unsigned *out_length) {
    if (!address || !out_length || length < 2) {
        errno = EINVAL;
        return -1;
    }
    const uint8_t native_family = ((const uint8_t *)address)[1];
    int guest_family = bs_family_native_to_guest(native_family);
    if (guest_family < 0) return -1;
    unsigned required = native_family == AF_INET ? 16 :
                        native_family == AF_INET6 ? 28 : 0;
    if (!required || length < required) {
        errno = EINVAL;
        return -1;
    }
    *out_length = required;
    if (!out || !capacity) return 0; /* size query / zero-length buffer */
    /* Linux getpeername/getsockname/accept/recvfrom allow a shorter caller
     * sockaddr buffer. Copy its prefix, but return the ACTUAL size in
     * addrlen; truncation is not a socket failure. */
    const size_t copied = capacity < required ? capacity : required;
    memcpy(out, address, copied);
    uint16_t family16 = (uint16_t)guest_family;
    memcpy(out, &family16, copied < sizeof family16 ? copied : sizeof family16);
    return 0;
}

int bs_android_socket_flags_guest_to_native(int flags) {
    /* Linux and FreeBSD MSG_ values differ. Only verified common options
     * are permitted. NOSIGNAL maps to BSD MSG_NOSIGNAL (no fake signal
     * suppression), and DONTWAIT/WAITALL are intentionally swapped. */
    enum {
        G_MSG_OOB = 1, G_MSG_PEEK = 2, G_MSG_DONTROUTE = 4,
        G_MSG_TRUNC = 0x20, G_MSG_DONTWAIT = 0x40, G_MSG_EOR = 0x80,
        G_MSG_WAITALL = 0x100, G_MSG_NOSIGNAL = 0x4000
    };
    const int allowed = G_MSG_OOB | G_MSG_PEEK | G_MSG_DONTROUTE |
                        G_MSG_TRUNC | G_MSG_DONTWAIT | G_MSG_EOR |
                        G_MSG_WAITALL | G_MSG_NOSIGNAL;
    if (flags & ~allowed) {
        errno = EOPNOTSUPP;
        return -1;
    }
    int native = 0;
    if (flags & G_MSG_OOB) native |= MSG_OOB;
    if (flags & G_MSG_PEEK) native |= MSG_PEEK;
    if (flags & G_MSG_DONTROUTE) native |= MSG_DONTROUTE;
    if (flags & G_MSG_TRUNC) native |= MSG_TRUNC;
    if (flags & G_MSG_DONTWAIT) native |= MSG_DONTWAIT;
    if (flags & G_MSG_EOR) native |= MSG_EOR;
    if (flags & G_MSG_WAITALL) native |= MSG_WAITALL;
    if (flags & G_MSG_NOSIGNAL) native |= MSG_NOSIGNAL;
    return native;
}

int bs_android_socket(int family, int type, int protocol) {
    if (!bs_net_require_service()) return -1;
    int native_family = bs_android_socket_family_guest_to_native(family);
    if (native_family < 0 || native_family == AF_UNSPEC) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    const int linux_flag_mask = 0x800 | 0x80000;
    int base = type & ~linux_flag_mask;
    if (base != SOCK_STREAM && base != SOCK_DGRAM &&
        base != SOCK_RAW && base != SOCK_SEQPACKET) {
        errno = EPROTOTYPE;
        return -1;
    }
    int native_type = base;
    if (type & 0x800) native_type |= SOCK_NONBLOCK;
    if (type & 0x80000) native_type |= SOCK_CLOEXEC;
    return socket(native_family, native_type, protocol);
}

static int bs_net_input_address(const void *address, unsigned length,
                                struct sockaddr_storage *native,
                                socklen_t *native_length) {
    unsigned converted = 0;
    if (bs_android_sockaddr_guest_to_native(
            address, length, native, sizeof *native, &converted))
        return -1;
    *native_length = (socklen_t)converted;
    return 0;
}

int bs_android_connect(int fd, const void *address, unsigned length) {
    if (!bs_net_require_service()) return -1;
    struct sockaddr_storage native;
    socklen_t size;
    return bs_net_input_address(address, length, &native, &size) ? -1 :
        connect(fd, (struct sockaddr *)&native, size);
}
int bs_android_bind(int fd, const void *address, unsigned length) {
    if (!bs_net_require_service()) return -1;
    struct sockaddr_storage native;
    socklen_t size;
    return bs_net_input_address(address, length, &native, &size) ? -1 :
        bind(fd, (struct sockaddr *)&native, size);
}
int bs_android_listen(int fd, int backlog) {
    return bs_net_require_service() ? listen(fd, backlog) : -1;
}
int bs_android_shutdown(int fd, int how) {
    if (!bs_net_require_service()) return -1;
    if (how < 0 || how > 2) { errno = EINVAL; return -1; }
    return shutdown(fd, how);
}

static int bs_net_return_address(int fd, void *address, unsigned *length,
                                 bool peer) {
    if (!bs_net_require_service()) return -1;
    if (!address || !length) { errno = EINVAL; return -1; }
    struct sockaddr_storage native;
    socklen_t native_len = sizeof native;
    int result = peer ?
        getpeername(fd, (struct sockaddr *)&native, &native_len) :
        getsockname(fd, (struct sockaddr *)&native, &native_len);
    if (result) return result;
    unsigned guest_size = 0;
    if (bs_android_sockaddr_native_to_guest(
            &native, native_len, address, *length, &guest_size))
        return -1;
    *length = guest_size;
    return 0;
}
int bs_android_getsockname(int fd, void *address, unsigned *length) {
    return bs_net_return_address(fd, address, length, false);
}
int bs_android_getpeername(int fd, void *address, unsigned *length) {
    return bs_net_return_address(fd, address, length, true);
}
int bs_android_accept(int fd, void *address, unsigned *length) {
    if (!bs_net_require_service()) return -1;
    if ((address && !length) || (!address && length)) {
        errno = EINVAL;
        return -1;
    }
    struct sockaddr_storage native;
    socklen_t native_len = sizeof native;
    int client = accept(fd, address ? (struct sockaddr *)&native : NULL,
                        address ? &native_len : NULL);
    if (client < 0 || !address) return client;
    unsigned guest_size = 0;
    if (bs_android_sockaddr_native_to_guest(
            &native, native_len, address, *length, &guest_size)) {
        close(client);
        return -1;
    }
    *length = guest_size;
    return client;
}
ssize_t bs_android_send(int fd, const void *buffer, size_t length, int flags) {
    if (!bs_net_require_service()) return -1;
    int native_flags = bs_android_socket_flags_guest_to_native(flags);
    return native_flags < 0 ? -1 : send(fd, buffer, length, native_flags);
}
ssize_t bs_android_recv(int fd, void *buffer, size_t length, int flags) {
    if (!bs_net_require_service()) return -1;
    int native_flags = bs_android_socket_flags_guest_to_native(flags);
    return native_flags < 0 ? -1 : recv(fd, buffer, length, native_flags);
}
ssize_t bs_android_sendto(int fd, const void *buffer, size_t length, int flags,
                         const void *address, unsigned addrlen) {
    if (!bs_net_require_service()) return -1;
    int native_flags = bs_android_socket_flags_guest_to_native(flags);
    if (native_flags < 0) return -1;
    if (!address) return send(fd, buffer, length, native_flags);
    struct sockaddr_storage native;
    socklen_t native_len;
    if (bs_net_input_address(address, addrlen, &native, &native_len)) return -1;
    return sendto(fd, buffer, length, native_flags,
                  (struct sockaddr *)&native, native_len);
}
ssize_t bs_android_recvfrom(int fd, void *buffer, size_t length, int flags,
                           void *address, unsigned *addrlen) {
    if (!bs_net_require_service()) return -1;
    if ((address && !addrlen) || (!address && addrlen)) {
        errno = EINVAL;
        return -1;
    }
    int native_flags = bs_android_socket_flags_guest_to_native(flags);
    if (native_flags < 0) return -1;
    if (!address) return recv(fd, buffer, length, native_flags);
    struct sockaddr_storage native;
    socklen_t native_len = sizeof native;
    ssize_t received = recvfrom(fd, buffer, length, native_flags,
                                (struct sockaddr *)&native, &native_len);
    if (received < 0) return received;
    unsigned guest_size = 0;
    if (bs_android_sockaddr_native_to_guest(
            &native, native_len, address, *addrlen, &guest_size))
        return -1;
    *addrlen = guest_size;
    return received;
}

static int bs_net_option(int *level, int *option) {
    /* Linux SOL_SOCKET=1; libnx BSD SOL_SOCKET=0xffff.
     * Do not interpret Linux option 4 (SO_ERROR) as native SO_REUSEADDR. */
    if (*level != 1) {
        if (*level == IPPROTO_TCP || *level == IPPROTO_IP ||
            *level == IPPROTO_IPV6) {
            errno = ENOPROTOOPT; /* Needs protocol-specific ABI translation. */
            return -1;
        }
        errno = ENOPROTOOPT;
        return -1;
    }
    switch (*option) {
        case 2: *option = SO_REUSEADDR; break;
        case 3: *option = SO_TYPE; break;
        case 4: *option = SO_ERROR; break;
        case 6: *option = SO_BROADCAST; break;
        case 7: *option = SO_SNDBUF; break;
        case 8: *option = SO_RCVBUF; break;
        case 9: *option = SO_KEEPALIVE; break;
        case 13: *option = SO_LINGER; break;
        case 20: *option = SO_RCVTIMEO; break;
        case 21: *option = SO_SNDTIMEO; break;
        default: errno = ENOPROTOOPT; return -1;
    }
    *level = SOL_SOCKET;
    return 0;
}
int bs_android_setsockopt(int fd, int level, int option,
                         const void *data, unsigned length) {
    if (!bs_net_require_service()) return -1;
    if (bs_net_option(&level, &option)) return -1;
    return setsockopt(fd, level, option, data, (socklen_t)length);
}
int bs_android_getsockopt(int fd, int level, int option,
                         void *data, unsigned *length) {
    if (!bs_net_require_service()) return -1;
    if (!length) { errno = EINVAL; return -1; }
    if (bs_net_option(&level, &option)) return -1;
    socklen_t native_len = (socklen_t)*length;
    int result = getsockopt(fd, level, option, data, &native_len);
    if (!result) *length = native_len;
    return result;
}
int bs_android_inet_pton(int family, const char *text, void *address) {
    int native_family = bs_android_socket_family_guest_to_native(family);
    if (native_family < 0) return -1;
    return inet_pton(native_family, text, address);
}
const char *bs_android_inet_ntop(int family, const void *address,
                                char *text, unsigned length) {
    int native_family = bs_android_socket_family_guest_to_native(family);
    if (native_family < 0) return NULL;
    return inet_ntop(native_family, address, text, (socklen_t)length);
}

uint32_t bs_android_inet_addr(const char *text) {
    return text ? inet_addr(text) : INADDR_NONE;
}

/* Linux EAI_* values are negative. Horizon uses positive BSD EAI_* values. */
static int bs_resolver_error(int native) {
    switch (native) {
        case 0: return 0;
        case EAI_AGAIN: return -3;
        case EAI_BADFLAGS: return -1;
        case EAI_FAIL: return -4;
        case EAI_FAMILY: return -6;
        case EAI_MEMORY: return -10;
        case EAI_NONAME: return -2;
        case EAI_SERVICE: return -8;
        case EAI_SOCKTYPE: return -7;
        case EAI_SYSTEM: return -11;
        case EAI_OVERFLOW: return -12;
        default: return -4;
    }
}

static void bs_free_addrinfo_nodes(BsAndroidAddrInfo *head) {
    while (head) {
        BsAndroidAddrInfo *next = head->ai_next;
        free(head->ai_addr);
        free(head->ai_canonname);
        free(head);
        head = next;
    }
}

void bs_android_freeaddrinfo(BsAndroidAddrInfo *result) {
    bs_free_addrinfo_nodes(result);
}

int bs_android_getaddrinfo(const char *node, const char *service,
                           const BsAndroidAddrInfo *hints,
                           BsAndroidAddrInfo **result) {
    if (!result) return -1;
    *result = NULL;
    if (!bs_net_require_service()) return -11;
    struct addrinfo native_hints = {0};
    if (hints) {
        /* Linux: PASSIVE=1 CANONNAME=2 NUMERICHOST=4 V4MAPPED=8,
         * ALL=16 ADDRCONFIG=32 NUMERICSERV=0x400. */
        const int allowed = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x400;
        if (hints->ai_flags & ~allowed) return -1;
        native_hints.ai_flags = hints->ai_flags & 0x7;
        if (hints->ai_flags & 0x8) native_hints.ai_flags |= AI_V4MAPPED;
        if (hints->ai_flags & 0x10) native_hints.ai_flags |= AI_ALL;
        if (hints->ai_flags & 0x20) native_hints.ai_flags |= AI_ADDRCONFIG;
        if (hints->ai_flags & 0x400) native_hints.ai_flags |= AI_NUMERICSERV;
        native_hints.ai_family = bs_android_socket_family_guest_to_native(hints->ai_family);
        if (native_hints.ai_family < 0) return -6;
        native_hints.ai_socktype = hints->ai_socktype;
        native_hints.ai_protocol = hints->ai_protocol;
    }
    struct addrinfo *native = NULL;
    int rc = getaddrinfo(node, service, hints ? &native_hints : NULL, &native);
    if (rc) return bs_resolver_error(rc);
    BsAndroidAddrInfo *head = NULL;
    BsAndroidAddrInfo **tail = &head;
    int error = 0;
    for (const struct addrinfo *entry = native; entry; entry = entry->ai_next) {
        int guest_family = bs_family_native_to_guest(entry->ai_family);
        if (guest_family < 0) { error = -6; break; }
        BsAndroidAddrInfo *item = calloc(1, sizeof(*item));
        if (!item) { error = -10; break; }
        item->ai_flags = hints ? hints->ai_flags : 0;
        item->ai_family = guest_family;
        item->ai_socktype = entry->ai_socktype;
        item->ai_protocol = entry->ai_protocol;
        unsigned size = 0;
        if (entry->ai_addr) {
            unsigned native_length = (unsigned)entry->ai_addrlen;
            if (native_length != 16 && native_length != 28) {
                free(item); error = -6; break;
            }
            item->ai_addr = malloc(native_length);
            if (!item->ai_addr ||
                bs_android_sockaddr_native_to_guest(entry->ai_addr,
                    native_length, item->ai_addr, native_length, &size)) {
                free(item->ai_addr); free(item); error = -10; break;
            }
            item->ai_addrlen = size;
        }
        if (entry->ai_canonname) {
            size_t length = strlen(entry->ai_canonname) + 1;
            item->ai_canonname = malloc(length);
            if (!item->ai_canonname) {
                free(item->ai_addr); free(item); error = -10; break;
            }
            memcpy(item->ai_canonname, entry->ai_canonname, length);
        }
        *tail = item;
        tail = &item->ai_next;
    }
    freeaddrinfo(native);
    if (error) { bs_free_addrinfo_nodes(head); return error; }
    *result = head;
    return 0;
}

int bs_android_getnameinfo(const void *guest_address, unsigned length,
                           char *host, unsigned host_length,
                           char *service, unsigned service_length, int flags) {
    if (!bs_net_require_service()) return -11;
    /* Linux and BSD NI_NUMERIC* flags differ: translate, reject unknown. */
    /* Linux: NUMERICHOST=1 NUMERICSERV=2 NOFQDN=4 NAMEREQD=8
     * DGRAM=16; NI_IDN=32 requires a real IDN translator. */
    const int allowed = 1 | 2 | 4 | 8 | 16;
    if (flags & ~allowed) return -1;
    int native_flags = 0;
    if (flags & 1) native_flags |= NI_NUMERICHOST;
    if (flags & 2) native_flags |= NI_NUMERICSERV;
    if (flags & 4) native_flags |= NI_NOFQDN;
    if (flags & 8) native_flags |= NI_NAMEREQD;
    if (flags & 16) native_flags |= NI_DGRAM;
    struct sockaddr_storage native = {0};
    socklen_t native_length = 0;
    if (bs_net_input_address(guest_address, length, &native, &native_length))
        return -6;
    return bs_resolver_error(getnameinfo((struct sockaddr *)&native,
               native_length, host, (socklen_t)host_length,
               service, (socklen_t)service_length, native_flags));
}

const char *bs_android_gai_strerror(int error) {
    switch (error) {
        case 0: return "Success";
        case -1: return "Bad flags";
        case -2: return "Name or service not known";
        case -3: return "Temporary failure in name resolution";
        case -4: return "Non-recoverable failure in name resolution";
        case -6: return "Address family not supported";
        case -7: return "Socket type not supported";
        case -8: return "Service not supported";
        case -10: return "Memory allocation failure";
        case -11: return "System error";
        case -12: return "Result too large";
        default: return "Unknown address resolution error";
    }
}
