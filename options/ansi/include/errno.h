#ifndef _ERRNO_H
#define _ERRNO_H

#include <abi-bits/errno.h>
#include <mlibc-config.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __MLIBC_ABI_ONLY

/* Some programs define their own errno as an "extern int" if it is not a macro. */
#define errno __mlibc_errno
extern __thread int __mlibc_errno;

int *__errno_location(void);

/* Linux extensions. */
/* Gated on __MLIBC_GLIBC_OPTION, not just _GNU_SOURCE: these are only ever
 * populated (options/elf/generic/startup.cpp's set_startup_data) when the
 * glibc option is enabled. Declaring them whenever a caller merely requests
 * GNU extensions (_GNU_SOURCE, which e.g. this repo's whole toolchain
 * defines unconditionally for unrelated reasons) let compile-time-only
 * "is this symbol declared" probes — such as Mesa's meson.build check for
 * HAVE_PROGRAM_INVOCATION_NAME — report the extension as available on
 * targets that don't have the glibc option, even though the globals are
 * left permanently nullptr on those targets. That's what actually needs
 * guarding: not the implementation (which is correct wherever the
 * declaration is honest about being available), but this declaration
 * overpromising when __MLIBC_GLIBC_OPTION is off. */
#if defined(_GNU_SOURCE) && __MLIBC_GLIBC_OPTION

extern char *program_invocation_name;
extern char *program_invocation_short_name;
extern char *__progname;
extern char *__progname_full;

#endif

#endif /* !__MLIBC_ABI_ONLY */

#ifdef __cplusplus
}
#endif

#endif /* _ERRNO_H */
