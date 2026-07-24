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
	Stat,
	VmMap,
	VmUnmap,
	VmProtect,
	ClockGet,
	Clone,
	PrepareStack,
	ThreadExit,
	GetPid,
	GetUid,
	GetEuid,
	GetGid,
	GetEgid
{};

template<typename Tag>
using Sysdeps = SysdepOf<EtosSysdepTags, Tag>;

struct SysdepTraits {
	static constexpr bool usesRtNetlink = false;
};

} // namespace mlibc
