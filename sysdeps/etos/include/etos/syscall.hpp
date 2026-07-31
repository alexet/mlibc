#pragma once

#include <etos-idl/proc.hpp>
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

// Global call indices — no per-object protocol exists for these (they
// dispatch against the GLOBAL pseudo-slot, not an object), so idlc has
// nothing to generate for them; they stay hand-rolled here.
inline constexpr uint16_t CALL_PARK = 4;         // encoding 0
inline constexpr uint16_t CALL_EXIT_THREAD = 7;  // encoding 0
inline constexpr uint16_t CALL_SELF_THREAD = 8;  // encoding 0; returns a fresh Thread od in a0
inline constexpr uint16_t CALL_SLEEP = 9;        // encoding 0; a0 = milliseconds
inline constexpr uint16_t CALL_SERIAL_LOG = 10;
inline constexpr uint16_t CALL_EXIT_PROCESS = 11;
inline constexpr uint16_t CALL_SET_FS_BASE = 12;

// FileSystem/Folder/File, every Proc method (CreateThread/MapAnon/Unmap/
// MapMemory/Mprotect), Clock.Map, Thread.Wake, and ReadPipe/WritePipe are no
// longer hand-rolled here: sysdeps.cpp (and friends) call through the
// generated `etos-idl/{fs,proc,clock,pipes}.hpp` clients (`idlc
// gen-cpp-client`) instead — see tools/build-mlibc.sh's `etos_idl_include`
// wiring. The hand-written versions of these drifted out of sync with the
// real wire format more than once as the underlying `.idl` files gained
// declared `error`s (wrong reply register), changed encodings/request
// layouts (`CreateThread`), or non-sequential call indices (`WritePipe`'s
// hand-written `CALL_PIPE_WRITE` used `BlockingWrite`'s index, not
// `Write`'s) — see docs/src/objects/proc.md and this file's own git history.
// `rpc_close`/`try_dup` (the global `Dup`/`RpcClose` calls) similarly moved
// to the shared `etos_idl::rpc_close`/`etos_idl::try_dup` helpers in
// `etos_idl_runtime.hpp` rather than hand-rolled dispatch, even though
// those two *are* global (no per-object protocol) — they're common enough
// to be worth sharing one implementation with the generated clients.

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
	// `SELF_PROC` is a borrowed, persistent per-process capability slot —
	// release the temporary wrapper immediately after the call so its
	// destructor doesn't close it (see the matching comment on `dirBorrow`
	// in sysdeps.cpp's resolvePathLocked for why this matters).
	Process self(SELF_PROC);
	uint64_t addr = 0;
	auto err = self.map_anon(pages, 0, MemPermBits::Read | MemPermBits::Write, &addr);
	self.release();
	if (!err.is_ok() || addr == UINT64_MAX)
		return nullptr;
	return reinterpret_cast<void *>(addr);
}

} // namespace etos
