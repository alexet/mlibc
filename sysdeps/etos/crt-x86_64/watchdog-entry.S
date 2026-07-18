// Entry stub for the futex-timeout watchdog thread (see generic/futex.cpp).
// Process::create_thread only takes an instruction pointer and a stack
// pointer -- no argument register -- so the caller writes a single context
// pointer at the top of the fresh stack and we hand rsp itself (which equals
// the address of that pointer) to the C entry point, exactly like
// utility/user-util/src/thread.rs's thread_trampoline does for pthread-style
// spawns in the Rust half of this codebase.

.section .text
.global __etos_futex_watchdog_start
__etos_futex_watchdog_start:
	mov %rsp, %rdi
	call __etos_futex_watchdog_entry
	ud2 // __etos_futex_watchdog_entry never returns
.section .note.GNU-stack,"",%progbits
