#pragma once

#include <mlibc/sysdep-signatures.hpp>

namespace mlibc {

struct EtosSysdepTags :
	LibcPanic,
	LibcLog,
	Isatty,
	Write,
	TcbSet,
	AnonAllocate,
	AnonFree,
	Seek,
	Exit,
	Close,
	FutexWake,
	FutexWait,
	Read,
	Open,
	VmMap,
	VmUnmap,
	ClockGet,
	Clone,
	PrepareStack,
	ThreadExit
{};

template<typename Tag>
using Sysdeps = SysdepOf<EtosSysdepTags, Tag>;

struct SysdepTraits {
	static constexpr bool usesRtNetlink = false;
};

} // namespace mlibc
