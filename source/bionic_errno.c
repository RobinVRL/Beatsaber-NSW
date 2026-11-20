/* Bionic (Linux AArch64) errno ABI for original Quest guest code.
 * SPDX-License-Identifier: MIT
 */
#include "bionic_errno.h"

#include <errno.h>
#include <stddef.h>

/* {host newlib value, Linux asm-generic value}. 1..34 are identical in both
 * numberings and are omitted. Literal newlib numbers are used because many
 * names are hidden behind __LINUX_ERRNO_EXTENSIONS__ in newlib's header;
 * the common ones are cross-checked below. */
static const struct { int host, linux_value; } k_errno_map[] = {
    {45, 35} /* EDEADLK */, {91, 36} /* ENAMETOOLONG */, {46, 37} /* ENOLCK */,
    {88, 38} /* ENOSYS */, {90, 39} /* ENOTEMPTY */, {92, 40} /* ELOOP */,
    {35, 42} /* ENOMSG */, {36, 43} /* EIDRM */, {37, 44} /* ECHRNG */,
    {38, 45} /* EL2NSYNC */, {39, 46} /* EL3HLT */, {40, 47} /* EL3RST */,
    {41, 48} /* ELNRNG */, {42, 49} /* EUNATCH */, {43, 50} /* ENOCSI */,
    {44, 51} /* EL2HLT */, {50, 52} /* EBADE */, {51, 53} /* EBADR */,
    {52, 54} /* EXFULL */, {53, 55} /* ENOANO */, {54, 56} /* EBADRQC */,
    {55, 57} /* EBADSLT */, {56, 35} /* EDEADLOCK */, {57, 59} /* EBFONT */,
    {60, 60} /* ENOSTR */, {61, 61} /* ENODATA */, {62, 62} /* ETIME */,
    {63, 63} /* ENOSR */, {64, 64} /* ENONET */, {65, 65} /* ENOPKG */,
    {66, 66} /* EREMOTE */, {67, 67} /* ENOLINK */, {68, 68} /* EADV */,
    {69, 69} /* ESRMNT */, {70, 70} /* ECOMM */, {71, 71} /* EPROTO */,
    {74, 72} /* EMULTIHOP */, {76, 73} /* EDOTDOT */, {77, 74} /* EBADMSG */,
    {139, 75} /* EOVERFLOW */, {80, 76} /* ENOTUNIQ */, {81, 77} /* EBADFD */,
    {82, 78} /* EREMCHG */, {83, 79} /* ELIBACC */, {84, 80} /* ELIBBAD */,
    {85, 81} /* ELIBSCN */, {86, 82} /* ELIBMAX */, {87, 83} /* ELIBEXEC */,
    {138, 84} /* EILSEQ */, {143, 86} /* ESTRPIPE */, {131, 87} /* EUSERS */,
    {108, 88} /* ENOTSOCK */, {121, 89} /* EDESTADDRREQ */,
    {122, 90} /* EMSGSIZE */, {107, 91} /* EPROTOTYPE */,
    {109, 92} /* ENOPROTOOPT */, {123, 93} /* EPROTONOSUPPORT */,
    {124, 94} /* ESOCKTNOSUPPORT */, {95, 95} /* EOPNOTSUPP */,
    {134, 95} /* ENOTSUP */, {96, 96} /* EPFNOSUPPORT */,
    {106, 97} /* EAFNOSUPPORT */, {112, 98} /* EADDRINUSE */,
    {125, 99} /* EADDRNOTAVAIL */, {115, 100} /* ENETDOWN */,
    {114, 101} /* ENETUNREACH */, {126, 102} /* ENETRESET */,
    {113, 103} /* ECONNABORTED */, {104, 104} /* ECONNRESET */,
    {105, 105} /* ENOBUFS */, {127, 106} /* EISCONN */,
    {128, 107} /* ENOTCONN */, {110, 108} /* ESHUTDOWN */,
    {129, 109} /* ETOOMANYREFS */, {116, 110} /* ETIMEDOUT */,
    {111, 111} /* ECONNREFUSED */, {117, 112} /* EHOSTDOWN */,
    {118, 113} /* EHOSTUNREACH */, {120, 114} /* EALREADY */,
    {119, 115} /* EINPROGRESS */, {133, 116} /* ESTALE */,
    {132, 122} /* EDQUOT */, {135, 123} /* ENOMEDIUM */,
    {140, 125} /* ECANCELED */, {142, 130} /* EOWNERDEAD */,
    {141, 131} /* ENOTRECOVERABLE */,
    /* newlib-only codes without a Linux equivalent. */
    {79, 22} /* EFTYPE */, {89, 2} /* ENMFILE */, {75, 22} /* ELBIN */,
    {130, 11} /* EPROCLIM */,
};

#ifdef __SWITCH__
_Static_assert(EDEADLK == 45 && ENOSYS == 88 && ETIMEDOUT == 116 &&
               ENOTSUP == 134 && EOVERFLOW == 139 && EINPROGRESS == 119 &&
               ECONNREFUSED == 111 && EOPNOTSUPP == 95,
               "newlib errno numbering");
#endif
int bs_errno_to_bionic(int value) {
    if (value <= 34) return value;
    for (size_t i = 0; i < sizeof k_errno_map / sizeof k_errno_map[0]; ++i)
        if (k_errno_map[i].host == value) return k_errno_map[i].linux_value;
    return value;
}

int bs_errno_from_bionic(int value) {
    if (value <= 34) return value;
    for (size_t i = 0; i < sizeof k_errno_map / sizeof k_errno_map[0]; ++i)
        if (k_errno_map[i].linux_value == value) return k_errno_map[i].host;
    return value;
}

/* Host TLS is -mtp=soft (libnx ThreadVars), so it is unaffected by the
 * guest owning TPIDR_EL0. A host function reports failure by setting the
 * newlib errno; the next guest __errno() translates it and then clears the
 * host value, so a later, identical host error is still observed and a
 * guest "errno = 0" survives successful host calls. */
static __thread int t_guest_errno;

int *bs_guest_errno_pointer(void) {
    int host = errno;
    if (host) {
        t_guest_errno = bs_errno_to_bionic(host);
        errno = 0;
    }
    return &t_guest_errno;
}
