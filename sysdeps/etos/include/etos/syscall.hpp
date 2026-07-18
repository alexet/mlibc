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
	return Result{d, a0, a1};
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
