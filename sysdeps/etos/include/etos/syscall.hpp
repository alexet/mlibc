#pragma once

#include <stdint.h>

// etos syscall glue. A syscall is an RPC on an object slot: the dispatch word
// in rax selects (slot, call index, encoding); rax comes back as the error
// code (0 = success) and results land in the argument registers (a0 = rdi,
// a1 = rsi). Global calls target the pseudo-slot 0xFFFF'FFFF.
// See docs/src/concepts/encoding.md in the etos repo.

namespace etos {

inline constexpr uint32_t GLOBAL = 0xFFFF'FFFF;

// Well-known object slots vended by the kernel at process start.
inline constexpr uint32_t STDIN = 0;
inline constexpr uint32_t STDOUT = 1;
inline constexpr uint32_t SELF_PROC = 2;

// Global call indices.
inline constexpr uint16_t CALL_SERIAL_LOG = 10;
inline constexpr uint16_t CALL_EXIT_PROCESS = 11;
inline constexpr uint16_t CALL_SET_FS_BASE = 12;

// Object call indices.
inline constexpr uint16_t CALL_PIPE_READ = 3;       // ReadPipe, encoding 1
inline constexpr uint16_t CALL_PIPE_WRITE = 4;      // WritePipe, encoding 2
inline constexpr uint16_t CALL_PIPE_WRITE_POLL = 5; // WritePipe, encoding 0
inline constexpr uint16_t CALL_PROC_MAP_ANON = 2;   // Proc, encoding 0
inline constexpr uint16_t CALL_PROC_UNMAP = 3;      // Proc, encoding 0

inline constexpr uint64_t dispatch(uint32_t slot, uint16_t call_index, uint8_t encoding) {
	return (uint64_t(slot) << 32) | (uint64_t(call_index) << 16) | (uint64_t(encoding) << 10);
}

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

} // namespace etos
