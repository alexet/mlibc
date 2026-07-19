sysdep_supported_options = {
	'posix': true,
}

# The etos ELF loader applies only R_X86_64_RELATIVE relocations, so ifunc
# routines (R_X86_64_IRELATIVE) cannot be used.
sysdep_supports_ifunc = false

rtld_sources += files(
	'generic/futex.cpp',
	'crt-x86_64/watchdog-entry.S',
	'sysdeps.cpp',
)
rtld_include_dirs += include_directories('include')

libc_sources += files(
	'generic/clock.cpp',
	'generic/entry.cpp',
	'generic/futex.cpp',
	'generic/thread.cpp',
	'sysdeps.cpp',
	'crt-x86_64/thread-entry.S',
	'crt-x86_64/watchdog-entry.S',
)
libc_include_dirs += include_directories('include')

if not no_headers
	install_headers(
		'include/abi-bits/auxv.h',
		'include/abi-bits/blkcnt_t.h',
		'include/abi-bits/blksize_t.h',
		'include/abi-bits/clockid_t.h',
		'include/abi-bits/dev_t.h',
		'include/abi-bits/errno.h',
		'include/abi-bits/fcntl.h',
		'include/abi-bits/gid_t.h',
		'include/abi-bits/ino_t.h',
		'include/abi-bits/limits.h',
		'include/abi-bits/mode_t.h',
		'include/abi-bits/nlink_t.h',
		'include/abi-bits/pid_t.h',
		'include/abi-bits/seek-whence.h',
		'include/abi-bits/signal.h',
		'include/abi-bits/stat.h',
		'include/abi-bits/uid_t.h',
		'include/abi-bits/vm-flags.h',
		'include/abi-bits/wait.h',
		'include/abi-bits/sigevent.h',
		'include/abi-bits/sigval.h',
		'include/abi-bits/sa_family_t.h',
		'include/abi-bits/sockaddr_storage.h',
		'include/abi-bits/sig-limits.h',
		'include/abi-bits/suseconds_t.h',
		'include/abi-bits/access.h',
		'include/abi-bits/socklen_t.h',
		'include/abi-bits/socket.h',
		'include/abi-bits/poll.h',
		'include/abi-bits/resource.h',
		'include/abi-bits/in.h',
		'include/abi-bits/rlim_t.h',
		'include/abi-bits/utsname.h',
		'include/abi-bits/fd_set.h',
		'include/abi-bits/sem.h',
		'include/abi-bits/time.h',
		'include/abi-bits/ipc.h',
		'include/abi-bits/statvfs.h',
		'include/abi-bits/fsblkcnt_t.h',
		'include/abi-bits/fsfilcnt_t.h',
		'include/abi-bits/shm.h',
		'include/abi-bits/termios.h',
		'include/abi-bits/msg.h',
		'include/abi-bits/mqueue.h',
		'include/abi-bits/utmp-defines.h',
		'include/abi-bits/utmpx.h',
		'include/abi-bits/sigset_t.h',
		'include/abi-bits/aio.h',
		subdir: 'abi-bits',
		follow_symlinks: true
	)
endif

if not headers_only
	crt = custom_target('crt1',
		build_by_default: true,
		command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
		input: 'crt-x86_64/crt1.S',
		output: 'crt1.o',
		install: true,
		install_dir: get_option('libdir')
	)

	crt_pie = custom_target('Scrt1',
		build_by_default: true,
		command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
		input: 'crt-x86_64/crt1.S',
		output: 'Scrt1.o',
		install: true,
		install_dir: get_option('libdir')
	)

	custom_target('crti',
		build_by_default: true,
		command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
		input: 'crt-x86_64/crti.S',
		output: 'crti.o',
		install: true,
		install_dir: get_option('libdir')
	)

	custom_target('crtn',
		build_by_default: true,
		command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
		input: 'crt-x86_64/crtn.S',
		output: 'crtn.o',
		install: true,
		install_dir: get_option('libdir')
	)
endif
