#pragma once

#include <stdint.h>

// Layout of the shared page vended by etos's `Clock` capability. Mirrors
// `ClockPage` in the etos repo's `utility/shared-util/src/clock.rs` exactly —
// keep the two in sync. See docs/src/objects/clock.md in the etos repo.
//
// No RTC: "epoch" is whatever instant the kernel calibrated the TSC at boot,
// treated as 1970-01-01T00:00:00Z. Elapsed time is accurate; the absolute
// value is not real-world time. CLOCK_REALTIME and CLOCK_MONOTONIC therefore
// read the same underlying page.

namespace etos {

struct ClockPage {
	uint64_t magic;
	uint64_t seq; // seqlock generation: odd while a publish is in progress
	uint64_t tsc_freq_hz;
	uint64_t tsc_base;
	uint64_t ns_base;
	uint64_t flags;
};

inline uint64_t rdtsc() {
	uint32_t lo, hi;
	asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return (uint64_t(hi) << 32) | lo;
}

// Nanoseconds elapsed since epoch, computed purely from RDTSC — no syscall.
// Retries if it races a concurrent `publish` (none exists yet in etos, but
// the page is designed to allow one later; see the doc above).
inline uint64_t clock_page_now_ns(const ClockPage *page) {
	for (;;) {
		uint64_t s1 = __atomic_load_n(&page->seq, __ATOMIC_ACQUIRE);
		if (s1 & 1) {
			asm volatile("pause" ::: "memory");
			continue;
		}
		uint64_t freq = page->tsc_freq_hz;
		uint64_t base = page->tsc_base;
		uint64_t ns_base = page->ns_base;
		uint64_t tsc = rdtsc();
		uint64_t s2 = __atomic_load_n(&page->seq, __ATOMIC_ACQUIRE);
		if (s1 == s2) {
			// Split the multiply-then-divide to stay within 64 bits: no
			// __uint128_t (that pulls in compiler-rt's __udivti3, which this
			// freestanding/no-libgcc build doesn't link against). `rem` is
			// always < freq (a few GHz at most), so `rem * 1e9` safely stays
			// under UINT64_MAX.
			uint64_t delta = tsc >= base ? tsc - base : 0;
			uint64_t whole_s = delta / freq;
			uint64_t rem = delta % freq;
			return ns_base + whole_s * 1'000'000'000 + (rem * 1'000'000'000) / freq;
		}
	}
}

} // namespace etos
