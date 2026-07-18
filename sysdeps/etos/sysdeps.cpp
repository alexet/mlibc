#include "mlibc/tcb.hpp"
#include <abi-bits/errno.h>
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
int Sysdeps<VmMap>::operator()(void *, size_t, int, int, int, off_t, void **) {
	STUB();
}
int Sysdeps<VmUnmap>::operator()(void *, size_t) {
	STUB();
}
int Sysdeps<ClockGet>::operator()(int, time_t *, long *) {
	STUB();
}

} // namespace mlibc
