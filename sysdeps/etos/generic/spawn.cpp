// posix_spawn / waitpid for etos, without fork or exec.
//
// A new process on etos is built from the outside: `Proc.CreateChildProcess`
// gives an empty address space, the spawner maps the image into it, writes the
// initial stack, dups capabilities into its slot table, and only then creates
// its first thread. That work lives in exactly one place, the Rust spawner
// (`try_spawn_process` in utility/user-util/src/spawn.rs, on top of the ELF
// loader in utility/shared-util/src/elf.rs), and this file deliberately does
// *not* reimplement it. libc.a is built before, and must not depend on, any
// Rust — so the spawner is reached through one `extern "C"` symbol,
// `etos_spawn_elf`, exported by the `etos-spawn-glue` staticlib
// (utility/spawn-glue) and declared weak below. A program that links that
// archive gets a working posix_spawn; one that doesn't gets ENOSYS, and the
// link never fails either way. Link the archive in the same --start-group as
// libc.a, *and* pass -Wl,--undefined=etos_spawn_elf: a weak undefined
// reference never pulls a member out of a static archive, so without it the
// glue is silently left out and posix_spawn reports ENOSYS.
//
// What this file does itself is the POSIX side: find the executable (PATH
// search for posix_spawnp), read it, translate the file actions into slot
// operations for the glue, and track children for Waitpid.
//
// Semantics on etos:
//  * The child does not inherit the parent's slots. It gets what every etos
//    process is born with (stdin/stdout from the parent's slots 0/1, its own
//    Proc, the parent's Log) plus the parent's FileSystem and Clock at the
//    well-known FS/CLOCK slots when it has them, exactly like init's `run`.
//    Anything else has to be handed over explicitly with
//    posix_spawn_file_actions_adddup2(src, dst) — src is a slot in the parent,
//    dst the slot it lands in in the child (src == dst passes it through
//    unchanged). Sources always name the *parent's* slots, never a slot the
//    child received from an earlier action.
//  * addclose closes a slot the child was born with; addopen opens in the
//    parent and hands the result over; addchdir/addfchdir are ENOSYS (there is
//    no cwd). Spawn attributes are accepted and ignored: etos has no signal
//    dispositions, sessions, process groups, uids or scheduler classes.
//  * Only static images load (no PT_INTERP); anything else is ENOEXEC.
//  * A pid is the parent's slot for the child's Proc. Waitpid joins it. Exit
//    statuses are not reported (status is always 0), and `pid == -1`
//    ("any child") is not supported.

#include <abi-bits/errno.h>
#include <abi-bits/fcntl.h>
#include <etos-idl/etos_idl_runtime.hpp>
#include <etos-idl/proc.hpp>
#include <etos/syscall.hpp>
#include <fcntl.h>
#include <mlibc/all-sysdeps.hpp>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Must match `EtosSpawnCap` in utility/spawn-glue/src/lib.rs.
struct EtosSpawnCap {
	uint32_t source; // slot in the parent, or ETOS_SPAWN_CLOSE
	uint32_t target; // slot in the child
};
// `CLOSE_SLOT` in utility/user-util/src/spawn.rs: close `target` in the child.
constexpr uint32_t ETOS_SPAWN_CLOSE = 0xFFFF'FFFF;

extern "C" int etos_spawn_elf(const uint8_t *elf, size_t elf_len, const char *const *argv,
		const char *const *envp, const EtosSpawnCap *caps, size_t ncaps,
		uint32_t *out_proc_slot) __attribute__((weak));

namespace {

// ── children we have spawned and not yet waited for ─────────────────────────

constexpr size_t MAX_CHILDREN = 64;
uint32_t children[MAX_CHILDREN];
size_t child_count = 0;
bool children_lock_flag = false;

struct ChildLock {
	ChildLock() {
		while (__atomic_test_and_set(&children_lock_flag, __ATOMIC_ACQUIRE)) {
		}
	}
	~ChildLock() { __atomic_clear(&children_lock_flag, __ATOMIC_RELEASE); }
};

bool has_child_room() {
	ChildLock l;
	return child_count < MAX_CHILDREN;
}

void add_child(uint32_t slot) {
	ChildLock l;
	if (child_count < MAX_CHILDREN)
		children[child_count++] = slot;
}

bool take_child(uint32_t slot) {
	ChildLock l;
	for (size_t i = 0; i < child_count; i++) {
		if (children[i] == slot) {
			children[i] = children[--child_count];
			return true;
		}
	}
	return false;
}

bool is_child(uint32_t slot) {
	ChildLock l;
	for (size_t i = 0; i < child_count; i++)
		if (children[i] == slot)
			return true;
	return false;
}

// ── finding and reading the executable ──────────────────────────────────────

// Reads a whole file. Returns 0 or errno; `*data` is malloc'd.
int read_file(const char *path, uint8_t **data, size_t *size) {
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return errno;
	struct stat st;
	if (fstat(fd, &st) < 0) {
		int e = errno;
		close(fd);
		return e;
	}
	size_t cap = st.st_size > 0 ? size_t(st.st_size) : 0;
	auto *buf = static_cast<uint8_t *>(malloc(cap ? cap : 1));
	if (!buf) {
		close(fd);
		return ENOMEM;
	}
	size_t got = 0;
	while (got < cap) {
		ssize_t n = read(fd, buf + got, cap - got);
		if (n < 0) {
			int e = errno;
			free(buf);
			close(fd);
			return e;
		}
		if (n == 0)
			break;
		got += size_t(n);
	}
	close(fd);
	*data = buf;
	*size = got;
	return 0;
}

// Finds and reads the executable: `path` as given, or — for posix_spawnp, when
// it has no '/' — each PATH entry in turn. Returns 0 or errno.
int find_executable(const char *path, bool search, char *const envp[], uint8_t **data,
		size_t *size) {
	if (!*path)
		return ENOENT;
	if (!search || strchr(path, '/'))
		return read_file(path, data, size);

	// PATH comes from the *spawn's* environment when it has one (that is what
	// execvpe's caller means), falling back to ours, then to a sane default.
	const char *list = nullptr;
	for (size_t i = 0; envp && envp[i]; i++)
		if (strncmp(envp[i], "PATH=", 5) == 0)
			list = envp[i] + 5;
	if (!list)
		list = getenv("PATH");
	if (!list)
		list = "/bin:/usr/bin";

	const size_t name_len = strlen(path);
	int result = ENOENT;
	while (true) {
		const char *end = strchr(list, ':');
		size_t dir_len = end ? size_t(end - list) : strlen(list);
		char *candidate = static_cast<char *>(malloc(dir_len + name_len + 2));
		if (!candidate)
			return ENOMEM;
		memcpy(candidate, list, dir_len);
		size_t n = dir_len;
		if (dir_len)
			candidate[n++] = '/';
		memcpy(candidate + n, path, name_len + 1);
		int e = read_file(candidate, data, size);
		free(candidate);
		if (e == 0)
			return 0;
		// Like execvp: keep looking on a missing entry, but remember a
		// permission problem to report if nothing else turns up.
		if (e == EACCES)
			result = EACCES;
		else if (e != ENOENT && e != ENOTDIR)
			return e;
		if (!end)
			break;
		list = end + 1;
	}
	return result;
}

// ── file actions → slot operations ──────────────────────────────────────────

// posix_spawn_file_actions_t's list (see options/posix/generic/spawn.cpp).
struct FdOp {
	FdOp *next, *prev;
	int cmd, fd, srcfd, oflag;
	mode_t mode;
	char path[];
};
constexpr int FDOP_CLOSE = 1, FDOP_DUP2 = 2, FDOP_OPEN = 3, FDOP_CHDIR = 4, FDOP_FCHDIR = 5;

// Whether the caller has something in slot `slot`: dup it and, if that worked,
// close the copy. (There is no cheaper "is this slot occupied" query.)
bool slot_present(uint32_t slot) {
	uint32_t copy = etos_idl::try_dup(slot);
	if (copy == etos_idl::NO_SLOT)
		return false;
	etos_idl::rpc_close(copy);
	return true;
}

// The slots to install in the child on top of the standard four, and the
// parent fds `addopen` created that must be closed once the child has its own.
struct CapPlan {
	EtosSpawnCap *caps = nullptr;
	size_t ncaps = 0;
	int *opened = nullptr;
	size_t nopened = 0;

	~CapPlan() {
		free(caps);
		free(opened);
	}

	void close_opened() {
		for (size_t i = 0; i < nopened; i++)
			close(opened[i]);
		nopened = 0;
	}
};

// Builds `plan` from the defaults plus `fa`. Returns 0 or errno; on failure
// any fds already opened for the child are closed.
int plan_caps(CapPlan &plan, const posix_spawn_file_actions_t *fa) {
	size_t nops = 0;
	for (FdOp *op = fa ? static_cast<FdOp *>(fa->__actions) : nullptr; op; op = op->next)
		nops++;

	// FS and CLOCK, when the parent has them (exactly the two numbers init's
	// `run` hands out, and mlibc's `etos::FS`/`etos::CLOCK` hardcode).
	plan.caps = static_cast<EtosSpawnCap *>(malloc((nops + 2) * sizeof(EtosSpawnCap)));
	plan.opened = static_cast<int *>(malloc((nops + 1) * sizeof(int)));
	if (!plan.caps || !plan.opened)
		return ENOMEM;
	for (uint32_t slot : {etos::FS, etos::CLOCK})
		if (slot_present(slot))
			plan.caps[plan.ncaps++] = {slot, slot};

	if (!fa || !fa->__actions)
		return 0;
	// The list is head-inserted, so the oldest action is at the tail.
	FdOp *op = static_cast<FdOp *>(fa->__actions);
	while (op->next)
		op = op->next;
	for (; op; op = op->prev) {
		int e = 0;
		switch (op->cmd) {
		case FDOP_CLOSE:
			if (op->fd < 0)
				e = EBADF;
			else
				plan.caps[plan.ncaps++] = {ETOS_SPAWN_CLOSE, uint32_t(op->fd)};
			break;
		case FDOP_DUP2:
			if (op->srcfd < 0 || op->fd < 0 || !slot_present(uint32_t(op->srcfd)))
				e = EBADF;
			else
				plan.caps[plan.ncaps++] = {uint32_t(op->srcfd), uint32_t(op->fd)};
			break;
		case FDOP_OPEN: {
			if (op->fd < 0) {
				e = EBADF;
				break;
			}
			int fd = open(op->path, op->oflag, op->mode);
			if (fd < 0) {
				e = errno;
				break;
			}
			plan.opened[plan.nopened++] = fd;
			plan.caps[plan.ncaps++] = {uint32_t(fd), uint32_t(op->fd)};
			break;
		}
		case FDOP_CHDIR:
		case FDOP_FCHDIR:
			e = ENOSYS; // no working directory to change
			break;
		default:
			e = EINVAL;
			break;
		}
		if (e) {
			plan.close_opened();
			return e;
		}
	}
	return 0;
}

} // namespace

namespace mlibc {

int Sysdeps<PosixSpawn>::operator()(pid_t *__restrict pid, const char *__restrict path,
		const posix_spawn_file_actions_t *file_actions, const posix_spawnattr_t *__restrict,
		char *const argv[], char *const envp[], bool search_path) {
	if (!path)
		return EINVAL;
	if (!etos_spawn_elf)
		return ENOSYS; // this program didn't link etos-spawn-glue
	if (!has_child_room())
		return EAGAIN;

	uint8_t *file = nullptr;
	size_t file_size = 0;
	if (int e = find_executable(path, search_path, envp, &file, &file_size); e)
		return e;

	CapPlan plan;
	if (int e = plan_caps(plan, file_actions); e) {
		free(file);
		return e;
	}

	uint32_t slot = 0;
	int e = etos_spawn_elf(file, file_size, argv, envp, plan.caps, plan.ncaps, &slot);
	free(file);
	// The child holds its own copies now (or never will).
	plan.close_opened();
	if (e)
		return e;

	add_child(slot); // the slot *is* the pid, joined and closed by Waitpid
	if (pid)
		*pid = pid_t(slot);
	return 0;
}

int Sysdeps<Waitpid>::operator()(pid_t pid, int *status, int flags, struct rusage *,
		pid_t *ret_pid) {
	// Only children we spawned, one at a time: "any child" and process groups
	// have nothing to name on etos, and there is no way to poll a Proc.
	if (pid <= 0 || !is_child(uint32_t(pid)))
		return ECHILD;
	if (flags & WNOHANG)
		return ENOSYS; // Proc.Join blocks; there is no non-blocking form
	{
		Process child{uint32_t(pid)}; // closes the slot on the way out
		child.join();
	}
	// Whether Join succeeded or found the process already gone, it has ended.
	take_child(uint32_t(pid));
	if (status)
		*status = 0; // etos records no exit status
	if (ret_pid)
		*ret_pid = pid;
	return 0;
}

} // namespace mlibc
