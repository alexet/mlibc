// posix_spawn / waitpid for etos, without fork or exec.
//
// A new process on etos is built from the outside: `Proc.CreateChildProcess`
// gives an empty address space, the spawner maps the image into it, writes the
// initial stack, dups capabilities into its slot table, and only then creates
// its first thread. This is the C++ counterpart of `spawn_process_with_caps`
// in utility/user-util/src/spawn.rs (and of the ELF loading in
// utility/shared-util/src/elf.rs) — it can't simply call those, because they
// are Rust and libc.a is built before, and must not depend on, any Rust code.
// Keep the two in step: the fixed image base, the auxv contents, the child's
// slot layout and the 2MiB stack are deliberately the same as the Rust
// spawner's, so a program behaves identically however it was started.
//
// What is and isn't supported:
//  * Static-PIE (or static ET_EXEC) images with only R_X86_64_RELATIVE
//    relocations — what every etos program is built as. A PT_INTERP image is
//    rejected with ENOEXEC (the Rust spawner has a separate dynamic path).
//  * The child does not inherit the parent's slots. It gets what every etos
//    process is born with (stdin/stdout from the parent's slots 0/1, its own
//    Proc, the parent's Log) plus the parent's FileSystem and Clock at the
//    well-known FS/CLOCK slots when it has them, exactly like init's `run`.
//    Anything else has to be handed over explicitly with
//    posix_spawn_file_actions_adddup2(src, dst) — src is a slot in the parent,
//    and dst the slot it lands in in the child (src == dst passes it through
//    unchanged). Sources always name the *parent's* slots, never a slot the
//    child received from an earlier action.
//  * addclose closes a slot the child was born with; addopen opens in the
//    parent and hands the result over; addchdir/addfchdir are ENOSYS (there is
//    no cwd). Spawn attributes are accepted and ignored: etos has no signal
//    dispositions, sessions, process groups, uids or scheduler classes.
//  * A pid is the parent's slot for the child's Proc. Waitpid joins it. Exit
//    statuses are not reported (status is always 0), and `pid == -1`
//    ("any child") is not supported.

#include <abi-bits/errno.h>
#include <abi-bits/fcntl.h>
#include <bits/ensure.h>
#include <etos-idl/memory.hpp>
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

using namespace etos_idl;

namespace {

// ── ELF64 (only what the loader reads) ──────────────────────────────────────

struct Elf64Ehdr {
	uint8_t e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct Elf64Phdr {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

struct Elf64Dyn {
	int64_t d_tag;
	uint64_t d_val;
};

struct Elf64Rela {
	uint64_t r_offset;
	uint64_t r_info;
	int64_t r_addend;
};

constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PT_DYNAMIC = 2;
constexpr uint32_t PT_INTERP = 3;
constexpr uint32_t PT_TLS = 7;
constexpr uint32_t PF_X = 1, PF_W = 2, PF_R = 4;
constexpr int64_t DT_NULL = 0, DT_RELA = 7, DT_RELASZ = 8, DT_RELAENT = 9, DT_REL = 17;
constexpr uint32_t R_X86_64_RELATIVE = 8, R_X86_64_GLOB_DAT = 6, R_X86_64_JUMP_SLOT = 7;
constexpr uint16_t ET_EXEC = 2, ET_DYN = 3, EM_X86_64 = 62;

// ── layout constants shared with utility/user-util/src/spawn.rs ─────────────

// Where the image goes in the (fresh) child. `BASE_ADDR` there.
constexpr uint64_t IMAGE_BASE = 0x0000'00AB'C000'000;
constexpr uint64_t STACK_PAGES = 512; // DEFAULT_STACK_SIZE_PAGES
constexpr uint64_t PAGE = 4096;
// etos-private auxv tag pointing at the PT_TLS descriptor, read by Rust
// children's `_start` (utility/user-util/src/tls.rs). mlibc children find
// PT_TLS themselves via AT_PHDR and ignore it.
constexpr uint64_t AT_ETOS_TLS = 0x4554'0001;
constexpr uint64_t AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6;

// Slots in a freshly created process (docs/src/concepts/object-slots.md).
constexpr uint32_t SLOT_LOG = etos::LOG;

// Global `Dup2` (idl-less, see etos/syscall.hpp's comment on globals): copy
// `src` from `src_proc`'s table into `dst_proc`'s table at slot `dst`.
bool dup2_between(uint32_t src_proc, uint32_t dst_proc, uint32_t src, uint32_t dst) {
	Regs args{};
	args.a0 = src_proc;
	args.a1 = dst_proc;
	args.a2 = src;
	args.a3 = dst;
	return raw_syscall(dispatch_word(NO_SLOT, 2, 11), args) == 0;
}

// Global `RpcClose` against another process's table (`rpc_close` does it for
// our own).
void close_in(uint32_t proc, uint32_t od) {
	Regs args{};
	args.a0 = proc;
	args.a1 = od;
	raw_syscall(dispatch_word(NO_SLOT, 3, 4), args);
}

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

bool reserve_child_slot() {
	ChildLock l;
	return child_count < MAX_CHILDREN;
}

void add_child(uint32_t slot) {
	ChildLock l;
	if (child_count < MAX_CHILDREN)
		children[child_count++] = slot;
}

// Removes `slot` if it is one of ours. Returns whether it was.
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

// ── image loading ───────────────────────────────────────────────────────────

// A PT_LOAD region backed by a `Memory` object that is mapped both into us
// (`local`, RW, so the loader can write with plain stores) and into the child
// at its real address and permissions.
struct Region {
	uint64_t target_base;
	uint64_t local_base;
	uint64_t pages;
};

constexpr size_t MAX_REGIONS = 16;

struct Image {
	const Process &child;
	Region regions[MAX_REGIONS];
	size_t region_count = 0;

	explicit Image(const Process &c) : child(c) {}

	~Image() {
		Process self(etos::SELF_PROC);
		for (size_t i = 0; i < region_count; i++)
			self.unmap(regions[i].local_base, regions[i].pages);
		self.release(); // SELF_PROC is a borrowed, persistent slot — never close it
	}

	Region *find(uint64_t addr, uint64_t len) {
		for (size_t i = 0; i < region_count; i++) {
			Region &r = regions[i];
			if (addr >= r.target_base && addr + len <= r.target_base + r.pages * PAGE)
				return &r;
		}
		return nullptr;
	}

	// Write into the child: a plain store when the target is in a shared
	// region, else `WriteMem` (executable segments, which can't be shared —
	// see the `Memory` note in `back`). `WriteMem`'s buffer is capped at
	// 16MiB by the kernel, so chunk comfortably under it.
	bool write(uint64_t addr, const void *buf, uint64_t len) {
		if (len == 0)
			return true;
		if (Region *r = find(addr, len)) {
			memcpy(reinterpret_cast<void *>(r->local_base + (addr - r->target_base)), buf, len);
			return true;
		}
		constexpr uint64_t CHUNK = 8 * 1024 * 1024;
		auto *p = static_cast<const uint8_t *>(buf);
		for (uint64_t done = 0; done < len; done += CHUNK) {
			uint64_t n = len - done < CHUNK ? len - done : CHUNK;
			uint64_t written = 0;
			auto err = child.write_mem(addr + done, p + done, n, &written);
			if (!err.is_ok() || written != n)
				return false;
		}
		return true;
	}

	// Back [addr, addr+size) in the child with `perms`.
	bool back(uint64_t addr, uint64_t size, uint64_t perms) {
		uint64_t page_base = addr & ~(PAGE - 1);
		uint64_t pages = ((addr + size + PAGE - 1) & ~(PAGE - 1)) - page_base;
		pages /= PAGE;

		// A capability `Memory` object is never executable, so an executable
		// segment has to be a plain anonymous mapping, filled by `write_mem`.
		// Relocations never target code, so it costs nothing.
		if (perms & MemPermBits::Execute) {
			uint64_t got = 0;
			auto err = child.map_anon(pages, page_base, perms, &got);
			return err.is_ok() && got == page_base;
		}

		if (region_count == MAX_REGIONS)
			return false;
		Process self(etos::SELF_PROC);
		Memory mem(NO_SLOT);
		uint64_t local = 0, remote = 0;
		bool ok = self.alloc_memory(pages, &mem).is_ok()
		          && self.map_memory(mem, 0, MemPermBits::Read | MemPermBits::Write, &local).is_ok()
		          && child.map_memory(mem, page_base, perms, &remote).is_ok() && remote == page_base;
		self.release();
		if (!ok)
			return false;
		// `mem` closing here is fine: each mapping keeps the frames alive.
		regions[region_count++] = Region{page_base, local, pages};
		return true;
	}
};

struct Loaded {
	uint64_t entry;
	uint64_t phdr_addr; // 0 if the table isn't inside a PT_LOAD
	uint64_t phent, phnum;
	bool has_tls;
	uint64_t tls_vaddr, tls_filesz, tls_memsz, tls_align;
};

// Loads the image in `data` into `img`'s child. Returns 0 or an errno value.
int load_image(Image &img, const uint8_t *data, size_t size, Loaded &out) {
	if (size < sizeof(Elf64Ehdr))
		return ENOEXEC;
	Elf64Ehdr eh;
	memcpy(&eh, data, sizeof eh);
	if (memcmp(eh.e_ident, "\177ELF", 4) != 0 || eh.e_ident[4] != 2 /*64-bit*/
	    || eh.e_ident[5] != 1 /*LE*/ || eh.e_machine != EM_X86_64
	    || (eh.e_type != ET_DYN && eh.e_type != ET_EXEC)
	    || eh.e_phentsize != sizeof(Elf64Phdr))
		return ENOEXEC;
	if (eh.e_phoff > size || size - eh.e_phoff < uint64_t(eh.e_phnum) * sizeof(Elf64Phdr))
		return ENOEXEC;

	auto phdr = [&](size_t i) {
		Elf64Phdr ph;
		memcpy(&ph, data + eh.e_phoff + i * sizeof(Elf64Phdr), sizeof ph);
		return ph;
	};

	uint64_t addr_min = UINT64_MAX;
	bool any_load = false;
	for (size_t i = 0; i < eh.e_phnum; i++) {
		Elf64Phdr ph = phdr(i);
		if (ph.p_type == PT_INTERP)
			return ENOEXEC; // dynamic executables aren't supported here
		if (ph.p_type != PT_LOAD)
			continue;
		if (ph.p_offset > size || size - ph.p_offset < ph.p_filesz || ph.p_filesz > ph.p_memsz)
			return ENOEXEC;
		any_load = true;
		if (ph.p_vaddr < addr_min)
			addr_min = ph.p_vaddr;
	}
	if (!any_load)
		return ENOEXEC;
	const uint64_t load_offset = IMAGE_BASE - addr_min;

	// Segments.
	for (size_t i = 0; i < eh.e_phnum; i++) {
		Elf64Phdr ph = phdr(i);
		if (ph.p_type != PT_LOAD)
			continue;
		uint64_t perms = 0;
		if (ph.p_flags & PF_R)
			perms |= MemPermBits::Read;
		if (ph.p_flags & PF_W)
			perms |= MemPermBits::Write;
		if (ph.p_flags & PF_X)
			perms |= MemPermBits::Execute;
		uint64_t start = ph.p_vaddr + load_offset;
		if (!img.back(start, ph.p_memsz, perms))
			return ENOMEM;
		// Fresh frames are zeroed by the kernel, so only the file-backed part
		// needs writing; the .bss tail is already zero.
		if (!img.write(start, data + ph.p_offset, ph.p_filesz))
			return ENOMEM;
	}

	// Relocations: R_X86_64_RELATIVE only. The rela table's address is a
	// vaddr; find its bytes in the file through the PT_LOAD that holds it.
	auto vaddr_to_file = [&](uint64_t vaddr, uint64_t len) -> const uint8_t * {
		for (size_t i = 0; i < eh.e_phnum; i++) {
			Elf64Phdr ph = phdr(i);
			if (ph.p_type == PT_LOAD && vaddr >= ph.p_vaddr && len <= ph.p_filesz
			    && vaddr - ph.p_vaddr <= ph.p_filesz - len)
				return data + ph.p_offset + (vaddr - ph.p_vaddr);
		}
		return nullptr;
	};
	for (size_t i = 0; i < eh.e_phnum; i++) {
		Elf64Phdr ph = phdr(i);
		if (ph.p_type != PT_DYNAMIC)
			continue;
		uint64_t rela = 0, relasz = 0, relaent = 0;
		bool have_rela = false;
		for (uint64_t off = 0; off + sizeof(Elf64Dyn) <= ph.p_filesz; off += sizeof(Elf64Dyn)) {
			Elf64Dyn d;
			memcpy(&d, data + ph.p_offset + off, sizeof d);
			if (d.d_tag == DT_NULL)
				break;
			if (d.d_tag == DT_REL)
				return ENOEXEC;
			if (d.d_tag == DT_RELA) {
				rela = d.d_val;
				have_rela = true;
			} else if (d.d_tag == DT_RELASZ) {
				relasz = d.d_val;
			} else if (d.d_tag == DT_RELAENT) {
				relaent = d.d_val;
			}
		}
		if (!have_rela)
			continue;
		if (relaent != sizeof(Elf64Rela))
			return ENOEXEC;
		const uint8_t *table = vaddr_to_file(rela, relasz);
		if (!table)
			return ENOEXEC;
		for (uint64_t n = 0; n < relasz / relaent; n++) {
			Elf64Rela r;
			memcpy(&r, table + n * sizeof r, sizeof r);
			switch (uint32_t(r.r_info)) {
			case R_X86_64_RELATIVE: {
				uint64_t value = uint64_t(r.r_addend) + load_offset;
				if (!img.write(r.r_offset + load_offset, &value, sizeof value))
					return ENOEXEC;
				break;
			}
			case R_X86_64_GLOB_DAT:
			case R_X86_64_JUMP_SLOT:
				break; // resolved by ld.so, which this loader doesn't run
			default:
				return ENOEXEC;
			}
		}
	}

	out = Loaded{};
	out.entry = eh.e_entry + load_offset;
	out.phent = eh.e_phentsize;
	out.phnum = eh.e_phnum;
	for (size_t i = 0; i < eh.e_phnum; i++) {
		Elf64Phdr ph = phdr(i);
		if (ph.p_type == PT_TLS && !out.has_tls) {
			out.has_tls = true;
			out.tls_vaddr = ph.p_vaddr + load_offset;
			out.tls_filesz = ph.p_filesz;
			out.tls_memsz = ph.p_memsz;
			out.tls_align = ph.p_align;
		}
		// The program-header table's loaded address (how a libc finds PT_TLS).
		if (ph.p_type == PT_LOAD && out.phdr_addr == 0 && ph.p_offset <= eh.e_phoff
		    && eh.e_phoff + uint64_t(eh.e_phnum) * sizeof(Elf64Phdr) <= ph.p_offset + ph.p_filesz)
			out.phdr_addr = ph.p_vaddr + (eh.e_phoff - ph.p_offset) + load_offset;
	}
	return 0;
}

// ── initial stack ───────────────────────────────────────────────────────────

size_t count(char *const v[]) {
	size_t n = 0;
	while (v && v[n])
		n++;
	return n;
}

// Builds the SysV entry block in the child: strings at the very top, the
// argc/argv/envp/auxv block below them. Mirrors `spawn_process_with_caps`.
// Returns 0 or errno; `*sp_out` is the initial stack pointer.
int build_stack(const Process &child, const Loaded &loaded, char *const argv[],
		char *const envp[], uint64_t *sp_out) {
	uint64_t got = 0;
	if (!child.map_anon(STACK_PAGES, 0, MemPermBits::Read | MemPermBits::Write, &got).is_ok()
	    || got == UINT64_MAX)
		return ENOMEM;
	const uint64_t stack_top = got + STACK_PAGES * PAGE;

	const size_t argc = count(argv), envc = count(envp);
	uint64_t strings_total = 0;
	for (size_t i = 0; i < argc; i++)
		strings_total += strlen(argv[i]) + 1;
	for (size_t i = 0; i < envc; i++)
		strings_total += strlen(envp[i]) + 1;

	// A stack this small can't hold an argv this big; fail rather than
	// scribble below it.
	if (strings_total + (argc + envc + 64) * 8 >= STACK_PAGES * PAGE / 2)
		return E2BIG;

	auto *strings = static_cast<uint8_t *>(malloc(strings_total ? strings_total : 1));
	if (!strings)
		return ENOMEM;
	const uint64_t strings_base = stack_top - strings_total;

	// argc + argv[] + NULL + envp[] + NULL + auxv (up to 4 pairs + AT_NULL)
	// + the TLS descriptor words + alignment pad.
	const size_t max_words = 1 + (argc + 1) + (envc + 1) + 2 * 5 + 2 + 4 + 1;
	auto *block = static_cast<uint64_t *>(malloc(max_words * sizeof(uint64_t)));
	if (!block) {
		free(strings);
		return ENOMEM;
	}

	size_t w = 0, s = 0;
	block[w++] = argc;
	for (size_t i = 0; i < argc; i++) {
		block[w++] = strings_base + s;
		size_t n = strlen(argv[i]) + 1;
		memcpy(strings + s, argv[i], n);
		s += n;
	}
	block[w++] = 0;
	for (size_t i = 0; i < envc; i++) {
		block[w++] = strings_base + s;
		size_t n = strlen(envp[i]) + 1;
		memcpy(strings + s, envp[i], n);
		s += n;
	}
	block[w++] = 0;
	if (loaded.phdr_addr) {
		block[w++] = AT_PHDR;
		block[w++] = loaded.phdr_addr;
		block[w++] = AT_PHENT;
		block[w++] = loaded.phent;
		block[w++] = AT_PHNUM;
		block[w++] = loaded.phnum;
	}
	block[w++] = AT_PAGESZ;
	block[w++] = PAGE;
	size_t tls_tag_index = 0;
	if (loaded.has_tls) {
		tls_tag_index = w;
		block[w++] = AT_ETOS_TLS;
		block[w++] = 0; // patched below
	}
	block[w++] = 0; // AT_NULL
	block[w++] = 0;
	// The descriptor goes *after* AT_NULL so no auxv reader ever sees it.
	size_t descriptor_index = w;
	if (loaded.has_tls) {
		block[w++] = loaded.tls_vaddr;
		block[w++] = loaded.tls_filesz;
		block[w++] = loaded.tls_memsz;
		block[w++] = loaded.tls_align;
	}
	if (w % 2 != 0)
		block[w++] = 0; // `sp` must be 16-byte aligned
	__ensure(w <= max_words);

	const uint64_t block_top = strings_base & ~uint64_t(0xF);
	const uint64_t sp = block_top - w * 8;
	if (loaded.has_tls)
		block[tls_tag_index + 1] = sp + descriptor_index * 8;

	uint64_t written = 0;
	bool ok = true;
	if (strings_total)
		ok = child.write_mem(strings_base, strings, strings_total, &written).is_ok()
		     && written == strings_total;
	if (ok)
		ok = child.write_mem(sp, reinterpret_cast<uint8_t *>(block), w * 8, &written).is_ok()
		     && written == w * 8;
	free(strings);
	free(block);
	if (!ok)
		return ENOMEM;
	*sp_out = sp;
	return 0;
}

// ── helpers around POSIX ────────────────────────────────────────────────────

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

// Finds the executable: `path` as given, or — for posix_spawnp, when it has
// no '/' — each PATH entry in turn. Returns 0 or errno; fills `out` (`cap`).
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

// posix_spawn_file_actions_t's list (see options/posix/generic/spawn.cpp).
struct FdOp {
	FdOp *next, *prev;
	int cmd, fd, srcfd, oflag;
	mode_t mode;
	char path[];
};
constexpr int FDOP_CLOSE = 1, FDOP_DUP2 = 2, FDOP_OPEN = 3, FDOP_CHDIR = 4, FDOP_FCHDIR = 5;

// Runs the file actions against the child's slot table. Returns 0 or errno.
int apply_file_actions(const Process &child, const posix_spawn_file_actions_t *fa) {
	if (!fa || !fa->__actions)
		return 0;
	// The list is head-inserted, so the oldest action is at the tail.
	FdOp *op = static_cast<FdOp *>(fa->__actions);
	while (op->next)
		op = op->next;
	for (; op; op = op->prev) {
		switch (op->cmd) {
		case FDOP_CLOSE:
			if (op->fd < 0)
				return EBADF;
			close_in(child.slot(), uint32_t(op->fd));
			break;
		case FDOP_DUP2:
			if (op->srcfd < 0 || op->fd < 0)
				return EBADF;
			if (!dup2_between(etos::SELF_PROC, child.slot(), uint32_t(op->srcfd), uint32_t(op->fd)))
				return EBADF;
			break;
		case FDOP_OPEN: {
			if (op->fd < 0)
				return EBADF;
			int fd = open(op->path, op->oflag, op->mode);
			if (fd < 0)
				return errno;
			bool ok = dup2_between(etos::SELF_PROC, child.slot(), uint32_t(fd), uint32_t(op->fd));
			close(fd);
			if (!ok)
				return EBADF;
			break;
		}
		case FDOP_CHDIR:
		case FDOP_FCHDIR:
			return ENOSYS; // no working directory to change
		default:
			return EINVAL;
		}
	}
	return 0;
}

// Wires up the slots every etos process starts with, mirroring
// `spawn_process_with_caps`: 0/1 from our own 0/1, 2 the child's own Proc, 3
// the Log (best effort — a parent without one leaves the child's empty, and
// its logging falls back to serial), plus FS/CLOCK when we have them.
int install_default_slots(const Process &child) {
	if (!dup2_between(etos::SELF_PROC, child.slot(), etos::STDIN, etos::STDIN)
	    || !dup2_between(etos::SELF_PROC, child.slot(), etos::STDOUT, etos::STDOUT)
	    || !dup2_between(etos::SELF_PROC, child.slot(), child.slot(), etos::SELF_PROC))
		return EAGAIN;
	dup2_between(etos::SELF_PROC, child.slot(), SLOT_LOG, SLOT_LOG);
	dup2_between(etos::SELF_PROC, child.slot(), etos::FS, etos::FS);
	dup2_between(etos::SELF_PROC, child.slot(), etos::CLOCK, etos::CLOCK);
	return 0;
}

} // namespace

namespace mlibc {

int Sysdeps<PosixSpawn>::operator()(pid_t *__restrict pid, const char *__restrict path,
		const posix_spawn_file_actions_t *file_actions, const posix_spawnattr_t *__restrict,
		char *const argv[], char *const envp[], bool search_path) {
	if (!path)
		return EINVAL;
	if (!reserve_child_slot())
		return EAGAIN;

	uint8_t *file = nullptr;
	size_t file_size = 0;
	if (int e = find_executable(path, search_path, envp, &file, &file_size); e)
		return e;

	// `Process self` would close the borrowed SELF_PROC slot on destruction,
	// so it is only ever created briefly and `release()`d.
	Process self(etos::SELF_PROC);
	Process child(NO_SLOT);
	auto created = self.create_child_process(&child);
	self.release();
	if (!created.is_ok()) {
		free(file);
		return EAGAIN;
	}

	int e = 0;
	Loaded loaded{};
	uint64_t sp = 0;
	{
		Image img(child); // unmaps our local views of the image on the way out
		e = load_image(img, file, file_size, loaded);
	}
	free(file);
	if (!e)
		e = build_stack(child, loaded, argv, envp, &sp);
	if (!e)
		e = install_default_slots(child);
	// Caller-specified slots go in last so they can override the defaults
	// (e.g. dup2(pipe, STDOUT)).
	if (!e)
		e = apply_file_actions(child, file_actions);
	if (!e) {
		// Creating the thread starts the child running; the thread keeps going
		// regardless of our handle to it.
		Thread thread(NO_SLOT);
		if (!child.create_thread(loaded.entry, sp, /*start_paused*/ false, &thread).is_ok())
			e = EAGAIN;
	}
	if (e) {
		child.kill(); // don't leave a half-built process around
		return e;
	}

	uint32_t slot = child.release(); // kept: it *is* the pid, joined by Waitpid
	add_child(slot);
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
	Process child{uint32_t(pid)};
	child.join();
	// Whether Join succeeded or found the process already gone, it has ended.
	take_child(uint32_t(pid));
	// `child` closes the slot on the way out.
	if (status)
		*status = 0; // etos records no exit status
	if (ret_pid)
		*ret_pid = pid;
	return 0;
}

} // namespace mlibc
