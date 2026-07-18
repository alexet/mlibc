// Clone/PrepareStack/ThreadExit: real pthread_create support on top of
// etos's Process::create_thread. Follows the same pattern as other mlibc
// ports for kernels that expose a bare "spawn a thread at (ip, sp)"
// primitive (see sysdeps/zinnia/generic/thread.cpp): PrepareStack lays out
// (entry, arg, tcb) on the new thread's own stack; Clone starts a real OS
// thread at a small asm trampoline (crt-x86_64/thread-entry.S) that pops
// those three words and calls __mlibc_enter_thread below.

#include <abi-bits/errno.h>
#include <atomic>
#include <bits/ensure.h>
#include <etos/syscall.hpp>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/tcb.hpp>
#include <stdint.h>

extern "C" void __mlibc_start_thread();

extern "C" [[noreturn]] void __mlibc_enter_thread(void *entry, void *arg, Tcb *tcb) {
	if (mlibc::sysdep<TcbSet>(tcb))
		__ensure(!"failed to set tcb for new thread");

	// Clone() has already returned in the parent by the time this runs, but
	// the store of the synthetic tid into tcb->tid races with us starting up;
	// wait for it via the futex we just implemented in generic/futex.cpp.
	while (__atomic_load_n(&tcb->tid, __ATOMIC_ACQUIRE) == 0)
		mlibc::sysdep<FutexWait>(&tcb->tid, 0, nullptr);

	tcb->invokeThreadFunc(entry, arg);
	mlibc::thread_exit(tcb->returnValue);
}

namespace mlibc {

namespace {

// tid 1 is implicitly claimed by the main thread: options/rtld/generic/main.cpp
// sets tcb->tid = this_tid(), which falls back to the hardcoded constant 1
// since FutexTid isn't implemented (options/internal/include/mlibc/tid.hpp).
std::atomic<int> next_tid_{2};

constexpr size_t default_stack_pages = 0x200000 / 4096;

} // namespace

int Sysdeps<PrepareStack>::operator()(
    void **stack,
    void *entry,
    void *arg,
    void *tcb,
    size_t *stack_size,
    size_t *guard_size,
    void **stack_base
) {
	// No guard page in this first cut (utility/user-util/src/thread.rs's
	// spawn_thread, etos's own Rust thread-spawn helper, has the same
	// limitation today).
	*guard_size = 0;

	uint64_t pages = *stack_size ? (*stack_size + 0xFFF) / 4096 : default_stack_pages;
	*stack_size = pages * 4096;

	void *base;
	if (*stack) {
		base = *stack;
	} else {
		base = etos::map_anon_pages(pages);
		if (base == nullptr)
			return ENOMEM;
	}
	*stack_base = base;

	uint64_t top = reinterpret_cast<uint64_t>(base) + pages * 4096;
	// Pushed high-to-low: tcb, arg, entry. crt-x86_64/thread-entry.S pops
	// them back out in the opposite order (entry, arg, tcb).
	*reinterpret_cast<void **>(top - 8) = tcb;
	*reinterpret_cast<void **>(top - 16) = arg;
	*reinterpret_cast<void **>(top - 24) = entry;

	*stack = reinterpret_cast<void *>(top - 24);
	return 0;
}

int Sysdeps<Clone>::operator()(void *tcb, pid_t *pid_out, void *stack) {
	(void)tcb;
	auto r = etos::syscall(
	    etos::dispatch(etos::SELF_PROC, etos::CALL_PROC_CREATE_THREAD, 0),
	    reinterpret_cast<uint64_t>(&__mlibc_start_thread),
	    reinterpret_cast<uint64_t>(stack)
	);
	if (r.err != 0)
		return EAGAIN;

	// The Thread capability itself isn't needed afterwards: mlibc's own
	// thread_join/thread_exit are implemented entirely via FutexWait/FutexWake
	// on tcb->didExit (options/internal/generic/threads.cpp), not via etos's
	// native Thread::JOIN/WAKE, so close the slot immediately to avoid leaking
	// one per pthread_create.
	auto od = static_cast<uint32_t>(r.a0);
	etos::syscall(etos::dispatch(etos::GLOBAL, etos::CALL_RPC_CLOSE, 4), etos::SELF_PROC, od);

	*pid_out = next_tid_.fetch_add(1, std::memory_order_relaxed);
	return 0;
}

void Sysdeps<ThreadExit>::operator()() {
	etos::syscall(etos::dispatch(etos::GLOBAL, etos::CALL_EXIT_THREAD, 0));
	__builtin_unreachable();
}

} // namespace mlibc
