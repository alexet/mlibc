#include "mlibc/tcb.hpp"
#include <abi-bits/errno.h>
#include <abi-bits/fcntl.h>
#include <abi-bits/seek-whence.h>
#include <abi-bits/vm-flags.h>
#include <bits/ensure.h>
#include <etos-idl/fs.hpp>
#include <etos-idl/pipes.hpp>
#include <etos-idl/proc.hpp>
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

// Close an object slot, via the generated client's shared runtime (the same
// global RPC-close call thread.cpp's own closeSlot-equivalent uses).
void closeSlot(uint32_t slot) {
	etos_idl::rpc_close(slot);
}

OffsetEntry *findEntryLocked(int fd) {
	for (auto &e : offsetTable) {
		if (e.used && e.fd == static_cast<uint32_t>(fd))
			return &e;
	}
	return nullptr;
}

// Any declared `App` error from a Folder.OpenFile/OpenFolder call maps to
// ENOENT: real servers (kernel-native initfs, fatd, rootfsd) only ever
// actually produce `NOT_FOUND` here (see idl/fs.idl's header comment) — the
// other declared variants (NOT_A_FILE/NOT_A_FOLDER/PERMISSION_DENIED/
// INVALID_NAME) aren't produced by any real server today, so there's nothing
// finer to distinguish yet. A transport-level failure maps to EIO, matching
// this function's previous raw-syscall behavior.
template <typename E>
int mapOpenErrno(const etos_idl::CallError<E> &err) {
	return err.kind == etos_idl::ErrKind::App ? ENOENT : EIO;
}

// Open the process's FS-slot FileSystem's root Folder once and cache its
// slot. Fails if the process wasn't `run`-spawned with a FileSystem
// capability at the well-known `etos::FS` slot.
bool ensureRootLocked() {
	if (rootFolderSlot != etos::NO_SLOT)
		return true;
	// A zero-length `permissions` buffer still needs a non-null, aligned
	// pointer: the kernel's generated dispatch builds a slice via
	// `core::slice::from_raw_parts(ptr, len)`, which is UB (and trips a debug
	// assertion) for a null `ptr` even when `len == 0`.
	static const uint8_t kNoPermissions = 0;
	// `etos::FS` is a borrowed, persistent per-process capability slot, not
	// something this call owns — release the temporary wrapper immediately
	// after the call so its destructor doesn't close it (see the matching
	// comment on `dirBorrow` in resolvePathLocked for why this matters).
	Folder root(etos::NO_SLOT);
	FileSystem fs(etos::FS);
	auto err = fs.root(&kNoPermissions, 0, &root);
	fs.release();
	if (!err.is_ok())
		return false;
	rootFolderSlot = root.release();
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

		uint32_t nextSlot = etos::NO_SLOT;
		bool ok;
		// `dir` is a *borrowed* slot (the persistent root folder, or an
		// intermediate directory this loop still owns via `dirIsRoot`/manual
		// `closeSlot` below) — never construct a bare `Folder(dir)` temporary
		// to call a method on it, since its destructor would close `dir` at
		// the end of the full expression regardless of ownership. `.release()`
		// it back immediately after the call instead (confirmed as a real
		// bug: the very first `Folder(dir).open_file(...)` call closed the
		// root folder slot as a side effect, so every subsequent lookup
		// failed at the transport level with `err::BAD_SOURCE`).
		Folder dirBorrow(dir);
		if (last) {
			File file(etos::NO_SLOT);
			auto err2 = dirBorrow.open_file(
			    reinterpret_cast<const uint8_t *>(pathname), len, OpenFlagsBits::Read, &file
			);
			dirBorrow.release();
			ok = err2.is_ok();
			if (!ok) {
				if (!dirIsRoot)
					closeSlot(dir);
				*err = mapOpenErrno(err2);
				return UINT32_MAX;
			}
			nextSlot = file.release();
		} else {
			Folder folder(etos::NO_SLOT);
			auto err2 = dirBorrow.open_folder(
			    reinterpret_cast<const uint8_t *>(pathname), len, &folder
			);
			dirBorrow.release();
			ok = err2.is_ok();
			if (!ok) {
				if (!dirIsRoot)
					closeSlot(dir);
				*err = mapOpenErrno(err2);
				return UINT32_MAX;
			}
			nextSlot = folder.release();
		}

		if (!dirIsRoot)
			closeSlot(dir);

		if (last)
			return nextSlot;

		dir = nextSlot;
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
	auto p = static_cast<const uint8_t *>(buf);
	size_t written = 0;
	while (written < size) {
		// `WritePipe::write` retries internally (via `object_call_retrying`)
		// while the pipe (or the user-space Tty behind it) reports
		// TARGET_NOT_READY, so no separate poll-then-write loop is needed
		// here — see etos_idl_runtime.hpp's `object_call_retrying`.
		WritePipe stdout_(etos::STDOUT);
		uint32_t n = 0;
		auto err = stdout_.write(p + written, size - written, &n);
		stdout_.release(); // STDOUT is a borrowed, persistent slot — never close it
		if (!err.is_ok())
			return EIO;
		written += n;
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

		File file(static_cast<uint32_t>(fd));
		size_t readLen = 0;
		auto err = file.read_at(offset, size, static_cast<uint8_t *>(buf), size, &readLen);
		file.release(); // ownership of fd stays in offsetTable, not this temporary
		if (!err.is_ok())
			return EIO;

		fsLock.lock();
		entry = findEntryLocked(fd);
		if (entry)
			entry->offset += readLen;
		fsLock.unlock();

		*ret = static_cast<long>(readLen);
		return 0;
	}
	while (true) {
		// `ReadPipe::read` retries internally while the pipe reports
		// TARGET_NOT_READY (see `Write`'s matching comment above), so no
		// separate poll step is needed.
		ReadPipe stdin_(etos::STDIN);
		size_t readLen = 0;
		auto err = stdin_.read(static_cast<uint8_t *>(buf), size, &readLen);
		stdin_.release(); // STDIN is a borrowed, persistent slot — never close it
		if (!err.is_ok())
			return EIO;
		if (readLen == 0 && size > 0)
			continue; // spurious wakeup with no data: poll again
		*ret = static_cast<long>(readLen);
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
	Process self(etos::SELF_PROC);
	uint64_t addr = 0;
	auto err = self.map_anon(pages, 0, MemPermBits::Read | MemPermBits::Write, &addr);
	self.release(); // SELF_PROC is a borrowed, persistent slot — never close it
	if (!err.is_ok() || addr == UINT64_MAX)
		return ENOMEM;
	// The kernel zeroes fresh frames, as mlibc requires.
	*pointer = reinterpret_cast<void *>(addr);
	return 0;
}

int Sysdeps<AnonFree>::operator()(void *pointer, unsigned long size) {
	uint64_t pages = (size + 0xFFF) >> 12;
	Process self(etos::SELF_PROC);
	self.unmap(reinterpret_cast<uint64_t>(pointer), pages);
	self.release(); // SELF_PROC is a borrowed, persistent slot — never close it
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
		File file(static_cast<uint32_t>(fd));
		uint64_t fileSize = 0;
		auto err = file.size(&fileSize);
		file.release(); // ownership of fd stays in offsetTable, not this temporary
		if (!err.is_ok())
			return EIO;
		fsLock.lock();
		entry = findEntryLocked(fd);
		if (!entry) {
			fsLock.unlock();
			return EBADF;
		}
		base = fileSize;
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

	File file(slot);
	FileStat st{};
	auto err = file.stat(&st);
	file.release(); // ownership of slot stays with the caller (fd or a one-shot path open)

	int result = 0;
	if (!err.is_ok()) {
		result = EIO;
	} else {
		memset(statbuf, 0, sizeof(*statbuf));
		statbuf->st_size = static_cast<off_t>(st.size);
		statbuf->st_mode = (st.is_dir ? S_IFDIR : S_IFREG) | 0444;
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
// etos's Proc.MapAnon has no PROT_NONE / lazily-committed-region
// concept (freshly mapped pages are always immediately backed) and no
// MAP_FIXED support for re-mapping part of an existing region with
// different permissions. MemoryAllocator::allocate's two-call pattern — (1)
// reserve pg_size+2*pageSize as PROT_NONE, (2) MAP_FIXED a
// PROT_READ|PROT_WRITE sub-range of it, excluding one guard page — is
// approximated rather than reproduced exactly: call (1) just performs a
// real mapping of the requested size (there is no way to make it truly
// PROT_NONE — see below), and call (2) (recognized by `addr` being non-null
// and `flags` containing MAP_FIXED) issues a real `Proc.Mprotect` over that
// same range instead of a fresh mapping, applying `prot`'s *new*
// permissions to it (same call `Sysdeps<VmProtect>` below makes for a
// standalone `mprotect()`). This isn't just a guard-page nicety: callers
// that legitimately reserve a range at reduced permissions and commit it to
// something wider later (e.g. DPDK's `eal_get_virtual_area`, which reserves
// with `PROT_NONE` then `MAP_FIXED`-commits the whole thing to
// `PROT_READ|PROT_WRITE`) would otherwise silently keep call (1)'s
// permissions forever and fault the first time they actually use the
// memory — no-op'ing this call was only ever safe for callers (mlibc's own
// allocator included) that happen to already request full access in call
// (1), papering over the missing PROT_NONE support rather than genuinely
// not needing call (2) to do anything.
//
// `prot` genuinely maps to the initial permissions now — PROT_EXEC included:
// unlike a `Memory` object, anonymous memory's permission ceiling is
// unrestricted, so a later `mprotect` can still move a Read+Write mapping
// to Read+Execute (the classic JIT allocate/write/finalize pattern) even if
// `prot` didn't ask for PROT_EXEC up front.
int Sysdeps<VmMap>::operator()(void *addr, size_t length, int prot, int flags, int fd, off_t offset, void **window) {
	(void)offset;
	if (fd != -1 || !(flags & MAP_ANONYMOUS))
		return ENOTSUP; // no file-backed mmap on etos

	MemPerm perms = MemPermBits::Read;
	if (prot & PROT_WRITE)
		perms |= MemPermBits::Write;
	if (prot & PROT_EXEC)
		perms |= MemPermBits::Execute;

	if (addr != nullptr && (flags & MAP_FIXED)) {
		// "Commit" call over an already-mapped region from a prior
		// anonymous call (see comment above): actually apply the new
		// permissions via mprotect rather than assuming call (1) already
		// granted them.
		uint64_t fixed_pages = (length + 0xFFF) >> 12;
		Process fixed_self(etos::SELF_PROC);
		auto protErr = fixed_self.mprotect(reinterpret_cast<uintptr_t>(addr), fixed_pages, perms);
		fixed_self.release(); // SELF_PROC is a borrowed, persistent slot — never close it
		if (!protErr.is_ok())
			return EACCES;
		*window = addr;
		return 0;
	}

	uint64_t pages = (length + 0xFFF) >> 12;
	Process self(etos::SELF_PROC);
	uint64_t mapped = 0;
	auto err = self.map_anon(pages, 0, perms, &mapped);
	self.release(); // SELF_PROC is a borrowed, persistent slot — never close it
	if (!err.is_ok()) {
		if (err.kind == etos_idl::ErrKind::App)
			return EACCES; // Write+Execute requested together
		return EIO; // unexpected transport-level failure
	}
	if (mapped == UINT64_MAX)
		return ENOMEM;
	*window = reinterpret_cast<void *>(mapped);
	return 0;
}
// mlibc's own call sites (frigg's slab-pool huge-object free, file_window,
// the debug allocator) always unmap exactly what they mapped, so any
// declared `RegionError` here means the range wasn't (fully) mapped to begin
// with — a real error, not a partial success to tolerate.
int Sysdeps<VmUnmap>::operator()(void *pointer, size_t length) {
	auto addr = reinterpret_cast<uintptr_t>(pointer);
	if (!pointer || (addr & 0xFFF) != 0)
		return EINVAL;

	uint64_t pages = (length + 0xFFF) >> 12;
	Process self(etos::SELF_PROC);
	auto err = self.unmap(addr, pages);
	self.release(); // SELF_PROC is a borrowed, persistent slot — never close it
	if (!err.is_ok())
		return EINVAL;
	return 0;
}
// Every current caller of Mprotect (ld.so's RELRO-style "tighten after
// relocation" in options/rtld/generic/linker.cpp, and JIT memory managers
// flipping a just-written code section from RW to RX) reprotects exactly the
// range it already owns, so any declared `RegionError` here means that range
// wasn't (fully) mapped — a real error, not a partial success to tolerate.
int Sysdeps<VmProtect>::operator()(void *pointer, size_t length, int prot) {
	auto addr = reinterpret_cast<uintptr_t>(pointer);
	if (!pointer || (addr & 0xFFF) != 0)
		return EINVAL;

	MemPerm perms = MemPermBits::Read;
	if (prot & PROT_WRITE)
		perms |= MemPermBits::Write;
	if (prot & PROT_EXEC)
		perms |= MemPermBits::Execute;

	uint64_t pages = (length + 0xFFF) >> 12;
	Process self(etos::SELF_PROC);
	auto err = self.mprotect(addr, pages, perms);
	self.release(); // SELF_PROC is a borrowed, persistent slot — never close it
	if (!err.is_ok())
		return EACCES;
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
