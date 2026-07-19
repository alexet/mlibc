#pragma once

#include <stdint.h>

// etos syscall glue. A syscall is an RPC on an object slot: the dispatch word
// in rax selects (slot, call index, encoding); rax comes back as the error
// code (0 = success) and results land in the argument registers (a0 = rdi,
// a1 = rsi). Global calls target the pseudo-slot 0xFFFF'FFFF.
// See docs/src/concepts/encoding.md in the etos repo.

namespace etos {

inline constexpr uint32_t GLOBAL = 0xFFFF'FFFF;
// Sentinel for "let the kernel pick a free slot", passed as the target_od
// argument to calls that return a new object (e.g. Clock::map). Same bit
// pattern as GLOBAL, but a distinct name for that argument position.
inline constexpr uint32_t NO_SLOT = 0xFFFF'FFFF;

// Well-known object slots vended by the kernel at process start.
inline constexpr uint32_t STDIN = 0;
inline constexpr uint32_t STDOUT = 1;
inline constexpr uint32_t SELF_PROC = 2;
// The read-only initfs FileSystem, when the process was `run`-spawned by init
// (see init/src/main.rs's cmd_run) — not present for every process.
inline constexpr uint32_t FS = 3;
inline constexpr uint32_t CLOCK = 4;

// Global call indices.
inline constexpr uint16_t CALL_RPC_CLOSE = 3;    // encoding 4; a0 = source_proc, a1 = od
inline constexpr uint16_t CALL_PARK = 4;         // encoding 0
inline constexpr uint16_t CALL_EXIT_THREAD = 7;  // encoding 0
inline constexpr uint16_t CALL_SELF_THREAD = 8;  // encoding 0; returns a fresh Thread od in a0
inline constexpr uint16_t CALL_SLEEP = 9;        // encoding 0; a0 = milliseconds
inline constexpr uint16_t CALL_SERIAL_LOG = 10;
inline constexpr uint16_t CALL_EXIT_PROCESS = 11;
inline constexpr uint16_t CALL_SET_FS_BASE = 12;

// Object call indices.
inline constexpr uint16_t CALL_PIPE_READ_POLL = 2;    // ReadPipe, encoding 0
inline constexpr uint16_t CALL_PIPE_READ = 3;         // ReadPipe, encoding 1
inline constexpr uint16_t CALL_PIPE_WRITE = 4;        // WritePipe, encoding 2
inline constexpr uint16_t CALL_PIPE_WRITE_POLL = 5;   // WritePipe, encoding 0
inline constexpr uint16_t CALL_PROC_CREATE_THREAD = 1; // Proc, encoding 0; a0 = ip, a1 = sp
inline constexpr uint16_t CALL_PROC_MAP_ANON = 2;     // Proc, encoding 0
inline constexpr uint16_t CALL_PROC_UNMAP = 3;        // Proc, encoding 0
inline constexpr uint16_t CALL_PROC_MAP_MEMORY = 8;   // Proc, encoding 0; a0 = mem_od, a1 = addr (0 = kernel picks)
inline constexpr uint16_t CALL_THREAD_WAKE = 8;       // Thread, encoding 0
inline constexpr uint16_t CALL_CLOCK_MAP = 1;         // Clock, encoding 13; a0 = target_od

// FileSystem/Folder/File call indices (see utility/user_api/src/fs.rs, the
// canonical client — this is a hand-rolled C++ counterpart of the same wire
// protocol, since mlibc's freestanding C++ build can't link the Rust crate).
inline constexpr uint16_t CALL_FS_ROOT = 1;          // FileSystem, encoding 14
inline constexpr uint16_t CALL_FOLDER_OPEN_FILE = 1;   // Folder, encoding 14
inline constexpr uint16_t CALL_FOLDER_OPEN_FOLDER = 2; // Folder, encoding 14
inline constexpr uint16_t CALL_FILE_READ_AT = 1;     // File, encoding 1
inline constexpr uint16_t CALL_FILE_STAT = 3;        // File, encoding 1
inline constexpr uint16_t CALL_FILE_SIZE = 5;        // File, encoding 0

// Folder/File open-flags bitmask (see OpenFlags in fs.rs).
inline constexpr uint32_t FS_OPEN_READ = 0x1;

// Filesystem status-word error codes (see err::NOT_FOUND/READ_ONLY etc. in
// utility/user_api/src/syscall.rs).
inline constexpr uint64_t FS_ERR_NOT_FOUND = 258;
inline constexpr uint64_t FS_ERR_READ_ONLY = 259;

inline constexpr uint64_t dispatch(uint32_t slot, uint16_t call_index, uint8_t encoding) {
	return (uint64_t(slot) << 32) | (uint64_t(call_index) << 16) | (uint64_t(encoding) << 10);
}

// Error codes (rax on return; 0 = success).
inline constexpr uint64_t ERR_TARGET_NOT_READY = 1;
inline constexpr uint64_t ERR_TARGET_CLOSED = 2;

struct Result {
	uint64_t err; // 0 = success, otherwise an etos error code
	uint64_t a0;
	uint64_t a1;
	uint64_t a2;
};

inline Result syscall(
    uint64_t d,
    uint64_t a0 = 0,
    uint64_t a1 = 0,
    uint64_t a2 = 0,
    uint64_t a3 = 0,
    uint64_t a4 = 0,
    uint64_t a5 = 0
) {
	register uint64_t r10 asm("r10") = a3;
	register uint64_t r8 asm("r8") = a4;
	register uint64_t r9 asm("r9") = a5;
	asm volatile("int $0x80"
	             : "+a"(d), "+D"(a0), "+S"(a1), "+d"(a2), "+r"(r10), "+r"(r8), "+r"(r9)
	             :
	             : "memory");
	return Result{d, a0, a1, a2};
}

// [`object_call_retrying`]'s C++ counterpart: like `syscall`, but busy-retries
// while the target reports `TARGET_NOT_READY` (e.g. right after its server
// thread is spawned but hasn't parked to serve it yet) — mirrors the
// poll-and-retry pattern already used for the STDIN/STDOUT pipes below,
// rather than a kernel-assisted blocking wait.
inline Result object_call_retrying(
    uint64_t d,
    uint64_t a0 = 0,
    uint64_t a1 = 0,
    uint64_t a2 = 0,
    uint64_t a3 = 0,
    uint64_t a4 = 0,
    uint64_t a5 = 0
) {
	while (true) {
		auto r = syscall(d, a0, a1, a2, a3, a4, a5);
		if (r.err != ERR_TARGET_NOT_READY)
			return r;
	}
}

// Map `pages` fresh anonymous zero-filled pages into this process (used for
// thread/watchdog stacks). Returns nullptr on failure.
inline void *map_anon_pages(uint64_t pages) {
	auto r = syscall(dispatch(SELF_PROC, CALL_PROC_MAP_ANON, 0), pages, /*addr hint*/ 0);
	if (r.err != 0 || r.a0 == UINT64_MAX)
		return nullptr;
	return reinterpret_cast<void *>(r.a0);
}

} // namespace etos
