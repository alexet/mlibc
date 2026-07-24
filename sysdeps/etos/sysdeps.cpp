#include "mlibc/tcb.hpp"
#include <abi-bits/errno.h>
#include <abi-bits/fcntl.h>
#include <abi-bits/seek-whence.h>
#include <abi-bits/vm-flags.h>
#include <bits/ensure.h>
#include <etos/syscall.hpp>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/fsfd_target.hpp>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#define STUB()                                                                                     \
	({                                                                                             \
		__ensure(!"STUB function was called");                                                     \
		__builtin_unreachable();                                                                   \
	})

namespace mlibc {

// ── filesystem: fd table + path resolution ──────────────────────────────────────
//
// An etos object-descriptor slot is already fd-shaped (a small per-process
// integer naming a kernel-tracked resource, closed by a dedicated syscall —
// see `kernel/src/process.rs`'s slot-reusing `Vec<Slot>`), so `Open` hands the
// freshly-opened `File`'s object slot back directly as the POSIX fd; no
// separate fd-allocation layer. The only extra state needed is the read/write
// offset, since etos's `File` RPCs are offset-explicit (`read_at`/`write_at`),
// not stream-positioned like POSIX `read`/`write`. fd 0/1/2 are never real
// files (they're the stdin/stdout pipes and the self-Proc capability) so they
// never appear in this table.
namespace {

struct SpinLock {
	void lock() {
		while (__atomic_test_and_set(&flag, __ATOMIC_ACQUIRE)) {
		}
	}
	void unlock() { __atomic_clear(&flag, __ATOMIC_RELEASE); }
	char flag = 0;
};

constexpr size_t MAX_OPEN_FILES = 32;

struct OffsetEntry {
	bool used = false;
	uint32_t fd = 0;
	uint64_t offset = 0;
};

SpinLock fsLock;
uint32_t rootFolderSlot = etos::NO_SLOT; // lazily opened, guarded by fsLock
OffsetEntry offsetTable[MAX_OPEN_FILES];

// Close an object slot (etos's global RPC-close call, see thread.cpp for the
// same pattern used to tear down thread objects).
void closeSlot(uint32_t slot) {
	etos::syscall(etos::dispatch(etos::GLOBAL, etos::CALL_RPC_CLOSE, 4), etos::SELF_PROC, slot);
}

OffsetEntry *findEntryLocked(int fd) {
	for (auto &e : offsetTable) {
		if (e.used && e.fd == static_cast<uint32_t>(fd))
			return &e;
	}
	return nullptr;
}

// Open the process's FS-slot FileSystem's root Folder once and cache its
// slot. Fails if the process wasn't `run`-spawned with a FileSystem
// capability at the well-known `etos::FS` slot.
bool ensureRootLocked() {
	if (rootFolderSlot != etos::NO_SLOT)
		return true;
	auto r = etos::object_call_retrying(
	    etos::dispatch(etos::FS, etos::CALL_FS_ROOT, 14), 0, 0, etos::NO_SLOT
	);
	if (r.err != 0)
		return false;
	rootFolderSlot = static_cast<uint32_t>(r.a2);
	return true;
}

// Resolve `pathname` (relative to the FS root — leading `/` is stripped;
// etos `Folder` names reject `/`, `.`, `..` outright, so there is no `..`
// traversal to handle) to a freshly-opened `File` object slot. Intermediate
// directory slots are closed as the walk descends; only the final File slot
// is left open. Returns `UINT32_MAX` and sets `*err` on failure.
uint32_t resolvePathLocked(const char *pathname, int *err) {
	if (!ensureRootLocked()) {
		*err = ENOENT;
		return UINT32_MAX;
	}
	while (*pathname == '/')
		pathname++;
	if (*pathname == '\0') {
		*err = ENOENT;
		return UINT32_MAX;
	}

	uint32_t dir = rootFolderSlot;
	bool dirIsRoot = true;

	while (true) {
		// Hand-rolled instead of strchr/strlen: this file is linked into
		// ld.so too (rtld_sources in sysdeps/etos/meson.build), which has no
		// libc string implementation available to it.
		const char *slash = pathname;
		while (*slash != '\0' && *slash != '/')
			slash++;
		size_t len = static_cast<size_t>(slash - pathname);
		slash = (*slash == '/') ? slash : nullptr;
		bool last = (slash == nullptr) || slash[1] == '\0';
		uint16_t callIndex =
		    last ? etos::CALL_FOLDER_OPEN_FILE : etos::CALL_FOLDER_OPEN_FOLDER;

		auto r = etos::object_call_retrying(
		    etos::dispatch(dir, callIndex, 14),
		    reinterpret_cast<uint64_t>(pathname),
		    len,
		    etos::NO_SLOT,
		    last ? etos::FS_OPEN_READ : 0
		);

		if (!dirIsRoot)
			closeSlot(dir);

		if (r.err != 0) {
			*err = EIO;
			return UINT32_MAX;
		}
		if (r.a0 != 0) {
			*err = (r.a0 == etos::FS_ERR_READ_ONLY) ? EROFS : ENOENT;
			return UINT32_MAX;
		}

		if (last)
			return static_cast<uint32_t>(r.a2);

		dir = static_cast<uint32_t>(r.a2);
		dirIsRoot = false;
		pathname = slash + 1;
	}
}

} // namespace

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
	// There is no stderr object: fd 1 maps to the STDOUT write pipe. Any other
	// fd is looked up in the filesystem offset table (see resolvePathLocked).
	if (fd != etos::STDOUT) {
		fsLock.lock();
		bool isOpenFile = findEntryLocked(fd) != nullptr;
		fsLock.unlock();
		return isOpenFile ? EROFS : EBADF; // initfs is read-only
	}
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
	// fd 0 maps to the STDIN read pipe. Any other fd is a file opened via
	// Sysdeps<Open>, read positionally at its tracked offset.
	if (fd != etos::STDIN) {
		fsLock.lock();
		OffsetEntry *entry = findEntryLocked(fd);
		if (!entry) {
			fsLock.unlock();
			return EBADF;
		}
		uint64_t offset = entry->offset;
		fsLock.unlock();

		auto r = etos::object_call_retrying(
		    etos::dispatch(static_cast<uint32_t>(fd), etos::CALL_FILE_READ_AT, 1),
		    reinterpret_cast<uint64_t>(buf),
		    size,
		    offset
		);
		if (r.err != 0)
			return EIO;

		fsLock.lock();
		entry = findEntryLocked(fd);
		if (entry)
			entry->offset += r.a1;
		fsLock.unlock();

		*ret = static_cast<long>(r.a1);
		return 0;
	}
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

int Sysdeps<Seek>::operator()(int fd, off_t offset, int whence, off_t *new_offset) {
	fsLock.lock();
	OffsetEntry *entry = findEntryLocked(fd);
	if (!entry) {
		fsLock.unlock();
		return (fd == etos::STDIN || fd == etos::STDOUT) ? ESPIPE : EBADF;
	}

	uint64_t base;
	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = entry->offset;
		break;
	case SEEK_END: {
		fsLock.unlock();
		auto r = etos::object_call_retrying(
		    etos::dispatch(static_cast<uint32_t>(fd), etos::CALL_FILE_SIZE, 0)
		);
		if (r.err != 0 || r.a0 != 0)
			return EIO;
		fsLock.lock();
		entry = findEntryLocked(fd);
		if (!entry) {
			fsLock.unlock();
			return EBADF;
		}
		base = r.a1;
		break;
	}
	default:
		fsLock.unlock();
		return EINVAL;
	}

	int64_t result = static_cast<int64_t>(base) + offset;
	if (result < 0) {
		fsLock.unlock();
		return EINVAL;
	}
	entry->offset = static_cast<uint64_t>(result);
	*new_offset = static_cast<off_t>(entry->offset);
	fsLock.unlock();
	return 0;
}

void Sysdeps<Exit>::operator()(int status) {
	// ExitProcess takes no status yet; it is dropped here.
	(void)status;
	etos::syscall(etos::dispatch(etos::GLOBAL, etos::CALL_EXIT_PROCESS, 0));
	__builtin_unreachable();
}

int Sysdeps<Close>::operator()(int fd) {
	// stdin/stdout/self-proc are not individually closeable through this call
	// (matches Isatty's blanket tty treatment above).
	if (fd == etos::STDIN || fd == etos::STDOUT || fd == static_cast<int>(etos::SELF_PROC))
		return 0;

	fsLock.lock();
	OffsetEntry *entry = findEntryLocked(fd);
	if (!entry) {
		fsLock.unlock();
		return EBADF;
	}
	entry->used = false;
	fsLock.unlock();

	closeSlot(static_cast<uint32_t>(fd));
	return 0;
}
// FutexWake/FutexWait are implemented in generic/futex.cpp.

// Only O_RDONLY against the read-only initfs FileSystem (well-known slot
// `etos::FS`, handed to `run`-spawned processes by init's `cmd_run`) is
// supported — there is no writable filesystem wired up for user processes
// yet. The returned fd *is* the freshly opened File's object slot (see the
// "filesystem: fd table + path resolution" section above).
int Sysdeps<Open>::operator()(const char *pathname, int flags, mode_t mode, int *fd) {
	(void)mode;
	if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_CREAT))
		return EROFS;

	fsLock.lock();
	int err = 0;
	uint32_t slot = resolvePathLocked(pathname, &err);
	if (slot == UINT32_MAX) {
		fsLock.unlock();
		return err;
	}

	OffsetEntry *entry = nullptr;
	for (auto &e : offsetTable) {
		if (!e.used) {
			entry = &e;
			break;
		}
	}
	if (!entry) {
		fsLock.unlock();
		closeSlot(slot);
		return EMFILE;
	}
	entry->used = true;
	entry->fd = slot;
	entry->offset = 0;
	fsLock.unlock();

	*fd = static_cast<int>(slot);
	return 0;
}

// Practically required for buffered stdio (fstat-based block-size probing)
// even though it isn't in mlibc's hard-mandatory sysdep list.
int Sysdeps<Stat>::operator()(fsfd_target fsfdt, int fd, const char *path, int flags, struct stat *statbuf) {
	(void)flags;
	uint32_t slot;
	bool opened = false;

	if (fsfdt == fsfd_target::fd) {
		slot = static_cast<uint32_t>(fd);
	} else if (fsfdt == fsfd_target::path) {
		fsLock.lock();
		int err = 0;
		slot = resolvePathLocked(path, &err);
		fsLock.unlock();
		if (slot == UINT32_MAX)
			return err;
		opened = true;
	} else {
		return ENOSYS;
	}

	// Wire `FileStat` layout (see utility/user_api/src/fs.rs): u64 size, u32
	// is_dir, u32 padding.
	uint8_t buf[16];
	auto r = etos::object_call_retrying(
	    etos::dispatch(slot, etos::CALL_FILE_STAT, 1), reinterpret_cast<uint64_t>(buf), sizeof(buf)
	);

	int result = 0;
	if (r.err != 0 || r.a2 != 0) {
		result = EIO;
	} else {
		uint64_t size;
		uint32_t isDir;
		memcpy(&size, buf, sizeof(size));
		memcpy(&isDir, buf + sizeof(size), sizeof(isDir));
		memset(statbuf, 0, sizeof(*statbuf));
		statbuf->st_size = static_cast<off_t>(size);
		statbuf->st_mode = (isDir ? S_IFDIR : S_IFREG) | 0444;
		statbuf->st_nlink = 1;
	}

	if (opened)
		closeSlot(slot);
	return result;
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
// A pure userspace no-op: etos user pages are always unconditionally
// PRESENT|WRITABLE|USER_ACCESSIBLE (kernel/src/memory/allocator.rs never sets
// NO_EXECUTE), and there is no kernel-side protect syscall to call — there's
// nothing to enforce or relax. This exists only to satisfy ld.so's
// sysdep_or_panic<VmProtect> call when it "tightens" a DSO segment's
// protection after loading it read-write (options/rtld/generic/linker.cpp).
int Sysdeps<VmProtect>::operator()(void *, size_t, int) {
	return 0;
}
// ClockGet is implemented in generic/clock.cpp.

// etos has no UNIX-style user/group accounts or process-id numbering yet --
// every process is just a capability-holding object, not a numbered entry in
// a process table. These are needed because some library code (e.g. Mesa's
// llvmpipe/LLVM stack, once linked in) calls geteuid()/getpid() unconditionally
// on general init paths, not because anything here relies on real values.
// Returning a fixed pid and uid/gid 0 ("root") is the correct stand-in until
// etos grows real multi-user semantics, not a workaround for a missing
// syscall.
pid_t Sysdeps<GetPid>::operator()() {
	return 1;
}
uid_t Sysdeps<GetUid>::operator()() {
	return 0;
}
uid_t Sysdeps<GetEuid>::operator()() {
	return 0;
}
gid_t Sysdeps<GetGid>::operator()() {
	return 0;
}
gid_t Sysdeps<GetEgid>::operator()() {
	return 0;
}

} // namespace mlibc
