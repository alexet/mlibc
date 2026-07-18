#include <errno.h>

int __thread __mlibc_errno;

// Only defined (and thus only ever linkable/probeable) when the glibc
// option — the only thing that actually populates these, in
// options/elf/generic/startup.cpp — is enabled. _GNU_SOURCE isn't part of
// this gate: whether these globals *exist* in the library is a property of
// how mlibc itself was built, not of whether some particular translation
// unit that includes <errno.h> happens to request GNU extensions (that's
// what the separate _GNU_SOURCE check in <errno.h>'s declaration is for).
#if __MLIBC_GLIBC_OPTION
char *program_invocation_name = nullptr;
char *program_invocation_short_name = nullptr;
extern char *__progname __attribute__((__weak__, __alias__("program_invocation_short_name")));
extern char *__progname_full __attribute__((__weak__, __alias__("program_invocation_name")));
#endif

int *__errno_location() {
	return &__mlibc_errno;
}
