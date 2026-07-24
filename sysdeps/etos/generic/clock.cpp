// ClockGet backed by etos's shared `ClockPage` — mapped once, then read with
// no further syscalls. See etos/clock.hpp and docs/src/objects/clock.md in
// the etos repo.

#include <abi-bits/errno.h>
#include <bits/ensure.h>
#include <etos/clock.hpp>
#include <etos/syscall.hpp>
#include <mlibc/all-sysdeps.hpp>
#include <stdint.h>

namespace mlibc {

namespace {

enum : uint32_t { kUninit = 0, kMapping = 1, kReady = 2 };

uint32_t g_state = kUninit;
const etos::ClockPage *g_page = nullptr;

// Map the Clock capability's shared page exactly once, even under concurrent
// callers: the first caller through the CAS does the mapping, everyone else
// spins until it publishes the pointer. Mirrors the CAS pattern in
// generic/futex.cpp.
const etos::ClockPage *ensure_clock_page() {
	uint32_t cur = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
	if (cur == kReady)
		return g_page;

	uint32_t expected = kUninit;
	if (__atomic_compare_exchange_n(
	        &g_state, &expected, kMapping, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE
	    )) {
		auto map_res =
		    etos::syscall(etos::dispatch(etos::CLOCK, etos::CALL_CLOCK_MAP, 13), etos::NO_SLOT);
		__ensure(map_res.err == 0);
		// perms = READ | WRITE (bits 0 and 1) — see etos::MemPerm.
		auto mem_res = etos::syscall(
		    etos::dispatch(etos::SELF_PROC, etos::CALL_PROC_MAP_MEMORY, 0), map_res.a0, 0, 0b011
		);
		__ensure(mem_res.err == 0);
		g_page = reinterpret_cast<const etos::ClockPage *>(mem_res.a0);
		__atomic_store_n(&g_state, kReady, __ATOMIC_RELEASE);
	} else {
		while (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) != kReady)
			asm volatile("pause" ::: "memory");
	}
	return g_page;
}

} // namespace

// etos has no RTC: CLOCK_REALTIME and CLOCK_MONOTONIC both read the same
// elapsed-since-boot page (see etos/clock.hpp). Every other clock id gets the
// same answer too, rather than failing — most callers only ever pass one of
// the two anyway.
int Sysdeps<ClockGet>::operator()(int, time_t *secs, long *nanos) {
	const etos::ClockPage *page = ensure_clock_page();
	uint64_t now_ns = etos::clock_page_now_ns(page);
	*secs = static_cast<time_t>(now_ns / 1'000'000'000);
	*nanos = static_cast<long>(now_ns % 1'000'000'000);
	return 0;
}

} // namespace mlibc
