/* Bionic (Linux AArch64) errno ABI for original Quest guest code.
 * SPDX-License-Identifier: MIT
 *
 * Host code runs on newlib, whose errno numbering diverges from Linux above
 * EDEADLK (e.g. newlib ETIMEDOUT=116 vs Linux 110). Every error number that
 * crosses into guest code, either as a pthread-style return value or through
 * the guest's errno, must use the Linux numbering the original ELF was built
 * against.
 */
#ifndef BS_BIONIC_ERRNO_H
#define BS_BIONIC_ERRNO_H

/* Translate a newlib error number to the Linux/Bionic number. Values that
 * are already identical (1..34 and others) pass through; 0 stays 0. */
int bs_errno_to_bionic(int newlib_errno);
/* Inverse mapping, for guest-supplied error numbers read by host code. */
int bs_errno_from_bionic(int bionic_errno);

/* Guest __errno(): a per-thread Bionic-numbered errno. Any host errno set
 * since the previous call is translated into it first. */
int *bs_guest_errno_pointer(void);

#endif
