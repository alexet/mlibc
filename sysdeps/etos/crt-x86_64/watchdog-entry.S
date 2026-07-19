// Entry stub for the futex-timeout watchdog thread (see generic/futex.cpp).
// Process::create_thread only takes an instruction pointer and a stack
// pointer -- no argument register -- so the caller writes a single context
// pointer at the top of the fresh stack and we hand rsp itself (which equals
// the address of that pointer) to the C entry point, exactly like
// utility/user-util/src/thread.rs's thread_trampoline does for pthread-style
// spawns in the Rust half of this codebase.

.section .text
.global __etos_futex_watchdog_start
// Hidden, not just global: default visibility makes lld emit a GLOB_DAT
// relocation for futex.cpp's address-of reference to this symbol (even
// though it's defined in the same object), since a default-visibility
// symbol is preemptible in a shared object. ld.so's own relocateSelf()
// bootstraps itself before any symbol table is usable and can only apply
// symbol-less RELATIVE relocations -- it __builtin_trap()s on anything
// else (see options/rtld/generic/main.cpp's relocateSelf), which is what
// hit this before .hidden was added here.
.hidden __etos_futex_watchdog_start
__etos_futex_watchdog_start:
	mov %rsp, %rdi
	call __etos_futex_watchdog_entry
	ud2 // __etos_futex_watchdog_entry never returns
.section .note.GNU-stack,"",%progbits
