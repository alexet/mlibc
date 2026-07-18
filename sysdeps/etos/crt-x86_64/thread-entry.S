// Entry stub for threads spawned via pthread_create (see generic/thread.cpp).
// PrepareStack pushes (entry, arg, tcb) onto the new thread's stack, in that
// order high-to-low, so they end up at [rsp], [rsp+8], [rsp+16]; pop them
// into the SysV argument registers for __mlibc_enter_thread.

.section .text
.global __mlibc_start_thread
__mlibc_start_thread:
	pop %rdi // entry
	pop %rsi // arg
	pop %rdx // tcb
	call __mlibc_enter_thread
	ud2 // __mlibc_enter_thread never returns
.section .note.GNU-stack,"",%progbits
