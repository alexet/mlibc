#include "mlibc/tcb.hpp"
#include <abi-bits/errno.h>
#include <abi-bits/vm-flags.h>
#include <bits/ensure.h>
#include <etos/syscall.hpp>
#include <mlibc/all-sysdeps.hpp>
#include <string.h>

#define STUB()                                                                                     \
	({                                                                                             \
		__ensure(!"STUB function was called");                                                     \
		__builtin_unreachable();                                                                   \
	})

namespace mlibc {

void Sysdeps<LibcPanic>::operator()() {
	sysdep<LibcLog>("!!! mlibc panic !!!");
	sysdep<Exit>(-1);
	__builtin_trap();
}

void Sysdeps<LibcLog>::operator()(const char *msg) {
	etos::syscall(
	    etos::dispatch(etos::GLOBAL, etos::CALL_SERIAL_LOG, 0),
	    reinterpret_cast<uint64_t>(msg),
	    strlen(msg)
	);
	etos::syscall(
	    etos::dispatch(etos::GLOBAL, etos::CALL_SERIAL_LOG, 0), reinterpret_cast<uint64_t>("\n"), 1
	);
}

int Sysdeps<Isatty>::operator()(int fd) {
	(void)fd;
	// stdout is the console pipe; treat every fd as a tty so stdio
	// line-buffers instead of trying to stat anything.
	return 0;
}

int Sysdeps<Write>::operator()(int fd, void const *buf, size_t size, ssize_t *ret) {
	// There is no stderr object: every fd maps to the STDOUT write pipe.
	(void)fd;
	auto p = reinterpret_cast<uint64_t>(buf);
	size_t written = 0;
	while (written < size) {
		// Poll first: a fresh write can report TARGET_NOT_READY while the
		// pipe (or the user-space Tty behind it) has no space yet.
		etos::syscall(etos::dispatch(etos::STDOUT, etos::CALL_PIPE_WRITE_POLL, 0));
		auto r = etos::syscall(
		    etos::dispatch(etos::STDOUT, etos::CALL_PIPE_WRITE, 2), p + written, size - written
		);
		if (r.err == etos::ERR_TARGET_NOT_READY)
			continue;
		if (r.err != 0)
			return EIO;
		written += r.a0;
	}
	*ret = static_cast<ssize_t>(written);
	return 0;
}

int Sysdeps<Read>::operator()(int fd, void *buf, unsigned long size, long *ret) {
	// Every fd maps to the STDIN read pipe.
	(void)fd;
	while (true) {
		etos::syscall(etos::dispatch(etos::STDIN, etos::CALL_PIPE_READ_POLL, 0));
		auto r = etos::syscall(
		    etos::dispatch(etos::STDIN, etos::CALL_PIPE_READ, 1),
		    reinterpret_cast<uint64_t>(buf),
		    size
		);
		if (r.err == etos::ERR_TARGET_NOT_READY)
			continue;
		if (r.err != 0)
			return EIO;
		if (r.a1 == 0 && size > 0)
			continue; // spurious wakeup with no data: poll again
		*ret = static_cast<long>(r.a1);
		return 0;
	}
}

int Sysdeps<TcbSet>::operator()(void *pointer) {
	// On x86_64 the FS base is the Tcb pointer itself (no offset).
	auto r = etos::syscall(
	    etos::dispatch(etos::GLOBAL, etos::CALL_SET_FS_BASE, 0),
	    reinterpret_cast<uint64_t>(pointer)
	);
	if (r.err != 0)
		return EINVAL;
	return 0;
}

int Sysdeps<AnonAllocate>::operator()(size_t size, void **pointer) {
	uint64_t pages = (size + 0xFFF) >> 12;
	auto r = etos::syscall(
	    etos::dispatch(etos::SELF_PROC, etos::CALL_PROC_MAP_ANON, 0), pages, /*addr hint*/ 0
	);
	if (r.err != 0 || r.a0 == UINT64_MAX)
		return ENOMEM;
	// The kernel zeroes fresh frames, as mlibc requires.
	*pointer = reinterpret_cast<void *>(r.a0);
	return 0;
}

int Sysdeps<AnonFree>::operator()(void *pointer, unsigned long size) {
	uint64_t pages = (size + 0xFFF) >> 12;
	etos::syscall(
	    etos::dispatch(etos::SELF_PROC, etos::CALL_PROC_UNMAP, 0),
	    reinterpret_cast<uint64_t>(pointer),
	    pages
	);
	return 0;
}

int Sysdeps<Seek>::operator()(int, off_t, int, off_t *) {
	return ESPIPE; // no file implementation, everything is a pipe/tty
}

void Sysdeps<Exit>::operator()(int status) {
	// ExitProcess takes no status yet; it is dropped here.
	(void)status;
	etos::syscall(etos::dispatch(etos::GLOBAL, etos::CALL_EXIT_PROCESS, 0));
	__builtin_unreachable();
}

int Sysdeps<Close>::operator()(int) {
	STUB();
}
// FutexWake/FutexWait are implemented in generic/futex.cpp.
int Sysdeps<Open>::operator()(const char *, int, unsigned int, int *) {
	STUB();
}
// Only the anonymous case is implemented — etos has no real files to back a
// mapping with ("no file implementation, everything is a pipe/tty", see
// Open/Seek above). This exists because mlibc's own internal allocator
// (options/internal/generic/allocator.cpp's MemoryAllocator, used e.g. for
// large/growth allocations distinct from the process's regular malloc) goes
// through VmMap directly rather than AnonAllocate for every anonymous
// mapping it makes, regardless of size — so leaving this a hard stub aborts
// the process the first time that allocator needs to grow, which in
// practice happens under large enough workloads (observed: Mesa's GLSL
// compiler).
//
// etos's CALL_PROC_MAP_ANON has no PROT_NONE / lazily-committed-region
// concept (freshly mapped pages are always immediately backed and
// read-write) and no MAP_FIXED support for re-mapping part of an existing
// region with different permissions. MemoryAllocator::allocate's two-call
// pattern — (1) reserve pg_size+2*pageSize as PROT_NONE, (2) MAP_FIXED a
// PROT_READ|PROT_WRITE sub-range of it, excluding one guard page — is
// approximated rather than reproduced exactly: call (1) just performs a
// real, fully-accessible mapping of the requested size (there is no way to
// make it PROT_NONE), and call (2) (recognized by `addr` being non-null and
// `flags` containing MAP_FIXED) is a no-op that reports success, since the
// range it asks to "commit" already is. The practical effect is a working
// allocator with one fewer enforced guard page than on a real POSIX target
// — a debugging aid lost, not a correctness gap: etos user pages are always
// RWX-mapped anyway (kernel/src/memory/allocator.rs never sets NO_EXECUTE
// on them), so there was no real W^X/PROT_NONE guarantee being approximated
// away to begin with.
int Sysdeps<VmMap>::operator()(void *addr, size_t length, int prot, int flags, int fd, off_t offset, void **window) {
	(void)prot;
	(void)offset;
	if (fd != -1 || !(flags & MAP_ANONYMOUS))
		return ENOTSUP; // no file-backed mmap on etos

	if (addr != nullptr && (flags & MAP_FIXED)) {
		// "Commit" call over an already fully-mapped region from a prior
		// anonymous call (see comment above): nothing to do.
		*window = addr;
		return 0;
	}

	uint64_t pages = (length + 0xFFF) >> 12;
	auto r = etos::syscall(
	    etos::dispatch(etos::SELF_PROC, etos::CALL_PROC_MAP_ANON, 0), pages, /*addr hint*/ 0
	);
	if (r.err != 0 || r.a0 == UINT64_MAX)
		return ENOMEM;
	*window = reinterpret_cast<void *>(r.a0);
	return 0;
}
int Sysdeps<VmUnmap>::operator()(void *, size_t) {
	STUB();
}
// ClockGet is implemented in generic/clock.cpp.

} // namespace mlibc
