// FutexWait/FutexWake emulation, built entirely in userspace on top of etos's
// Park/Thread::wake primitives (there is no address-keyed futex syscall in
// the kernel). See docs/notes in the etos repo's plan for the design.
//
// The wait table below is a *fixed-capacity static array*, not a
// dynamically-allocated container: mlibc's own allocator
// (MemoryAllocator/MemoryPool, options/internal/include/mlibc/allocator.hpp)
// is itself protected by a FutexLock, i.e. a contended malloc() can call back
// into FutexWait/FutexWake. If this table allocated memory while holding its
// own spinlock, a nested contended allocation would try to take that same
// spinlock again and deadlock. A static array sidesteps that hazard
// entirely -- no allocation ever happens while the lock is held.

#include <abi-bits/errno.h>
#include <bits/ensure.h>
#include <etos-idl/proc.hpp>
#include <etos/syscall.hpp>
#include <mlibc/all-sysdeps.hpp>
#include <stddef.h>
#include <stdint.h>

namespace mlibc {

namespace {

constexpr size_t kMaxWaiters = 256;

struct Waiter {
	bool in_use = false;
	bool claimed = false; // being processed by exactly one of FutexWake/the watchdog
	bool timed_out = false;
	uintptr_t addr = 0;
	uint32_t thread_od = 0;
};

Waiter waiters_[kMaxWaiters];

// A raw CAS spinlock. Must never be built on Park/wake (that would recurse
// into the futex machinery it protects) and must never be held across an
// allocation (see file comment above).
class RawSpinlock {
public:
	void lock() {
		while (__atomic_exchange_n(&locked_, 1, __ATOMIC_ACQUIRE)) {
			while (__atomic_load_n(&locked_, __ATOMIC_RELAXED))
				asm volatile("pause" ::: "memory");
		}
	}
	void unlock() { __atomic_store_n(&locked_, 0, __ATOMIC_RELEASE); }

private:
	int locked_ = 0;
};

RawSpinlock table_lock_;

// Caller must hold table_lock_.
int alloc_waiter_locked() {
	for (size_t i = 0; i < kMaxWaiters; i++) {
		if (!waiters_[i].in_use)
			return static_cast<int>(i);
	}
	return -1;
}

void wake_and_close(uint32_t thread_od) {
	// `Thread(thread_od)` owns the slot here (unlike the borrowed-slot
	// pattern used elsewhere in this file) — this function's whole job is
	// "wake it, then close it", so letting the temporary's destructor close
	// it is exactly what's wanted; no `.release()`.
	Thread(thread_od).wake();
}

struct WatchdogCtx {
	int idx;
	uint64_t sleep_ms;
	uint64_t stack_base;
	uint64_t stack_pages;
};

constexpr uint64_t kWatchdogStackPages = 4;

} // namespace

} // namespace mlibc

extern "C" void __etos_futex_watchdog_start();

extern "C" [[noreturn]] void __etos_futex_watchdog_entry(void *slot) {
	using namespace mlibc;

	auto ctx = *reinterpret_cast<WatchdogCtx **>(slot);
	int idx = ctx->idx;
	uint64_t sleep_ms = ctx->sleep_ms;
	uint64_t stack_base = ctx->stack_base;
	uint64_t stack_pages = ctx->stack_pages;

	etos::syscall(etos::dispatch(etos::GLOBAL, etos::CALL_SLEEP, 0), sleep_ms);

	table_lock_.lock();
	if (waiters_[idx].in_use && !waiters_[idx].claimed) {
		waiters_[idx].claimed = true;
		waiters_[idx].timed_out = true;
		uint32_t od = waiters_[idx].thread_od;
		table_lock_.unlock();
		wake_and_close(od);
	} else {
		// FutexWake already claimed this waiter; nothing to do.
		table_lock_.unlock();
	}

	Process self(etos::SELF_PROC);
	self.unmap(stack_base, stack_pages);
	self.release(); // SELF_PROC is a borrowed, persistent slot — never close it
	etos::syscall(etos::dispatch(etos::GLOBAL, etos::CALL_EXIT_THREAD, 0));
	__builtin_trap();
}

namespace mlibc {

int Sysdeps<FutexWait>::operator()(int *pointer, int expected, timespec const *time) {
	table_lock_.lock();
	if (__atomic_load_n(pointer, __ATOMIC_SEQ_CST) != expected) {
		table_lock_.unlock();
		return EAGAIN;
	}

	int idx = alloc_waiter_locked();
	if (idx < 0) {
		table_lock_.unlock();
		return ENOMEM;
	}

	auto self = etos::syscall(etos::dispatch(etos::GLOBAL, etos::CALL_SELF_THREAD, 0));
	waiters_[idx] = Waiter{
	    /*in_use=*/true,
	    /*claimed=*/false,
	    /*timed_out=*/false,
	    reinterpret_cast<uintptr_t>(pointer),
	    static_cast<uint32_t>(self.a0),
	};
	table_lock_.unlock();

	// No kernel "park with timeout" exists, so approximate one with a
	// short-lived watchdog thread: it sleeps for the requested duration and,
	// if we haven't already been woken by FutexWake by then, claims and wakes
	// us itself, marking the wait as timed out. This is coarse-grained
	// (millisecond Sleep resolution) but only used by the *_timedlock/
	// *_timedwait slow paths, never by the plain mutex fast/slow path (which
	// always passes time == nullptr).
	if (time != nullptr) {
		uint64_t ms = static_cast<uint64_t>(time->tv_sec) * 1000
		            + (static_cast<uint64_t>(time->tv_nsec) + 999'999) / 1'000'000;

		void *stack_base = etos::map_anon_pages(kWatchdogStackPages);
		if (stack_base != nullptr) {
			uint64_t top = reinterpret_cast<uint64_t>(stack_base) + kWatchdogStackPages * 4096;

			// Reserve room for the WatchdogCtx itself plus the pointer-to-it
			// slot the trampoline reads, both within the stack's own top
			// page (mirrors utility/user-util/src/thread.rs's spawn_thread).
			auto *ctx = reinterpret_cast<WatchdogCtx *>(top - sizeof(WatchdogCtx));
			*ctx = WatchdogCtx{
			    idx, ms, reinterpret_cast<uint64_t>(stack_base), kWatchdogStackPages
			};
			uint64_t sp = (reinterpret_cast<uint64_t>(ctx) - 16) & ~0xFULL;
			*reinterpret_cast<WatchdogCtx **>(sp) = ctx;

			Process procSelf(etos::SELF_PROC);
			Thread watchdogThread(etos::NO_SLOT);
			procSelf.create_thread(
			    reinterpret_cast<uint64_t>(&__etos_futex_watchdog_start), sp,
			    /*start_paused*/ false, &watchdogThread
			);
			procSelf.release(); // SELF_PROC is borrowed, persistent — never close it
			// watchdogThread closes on scope exit: this call is fire-and-forget
			// (no result checked, degrades to an untimed wait on failure — see
			// the comment below), and mlibc's own thread_join/thread_exit don't
			// use etos's native Thread::JOIN/WAKE (see Sysdeps<Clone>'s matching
			// comment in generic/thread.cpp), so nothing needs to keep it open.
		}
		// If the stack allocation failed, fall through without a watchdog:
		// the wait degrades to an untimed wait rather than spuriously
		// failing the caller.
	}

	etos::syscall(etos::dispatch(etos::GLOBAL, etos::CALL_PARK, 0));

	table_lock_.lock();
	bool timed_out = waiters_[idx].timed_out;
	waiters_[idx].in_use = false;
	table_lock_.unlock();

	return timed_out ? ETIMEDOUT : 0;
}

int Sysdeps<FutexWake>::operator()(int *pointer, bool all) {
	auto addr = reinterpret_cast<uintptr_t>(pointer);

	while (true) {
		table_lock_.lock();
		int idx = -1;
		for (size_t i = 0; i < kMaxWaiters; i++) {
			if (waiters_[i].in_use && !waiters_[i].claimed && waiters_[i].addr == addr) {
				idx = static_cast<int>(i);
				break;
			}
		}
		if (idx < 0) {
			table_lock_.unlock();
			return 0;
		}
		waiters_[idx].claimed = true;
		uint32_t od = waiters_[idx].thread_od;
		table_lock_.unlock();

		wake_and_close(od);

		if (!all)
			return 0;
	}
}

} // namespace mlibc
