// GetEntropy (getentropy(3)) backed by the CPU's hardware random number
// generator.
//
// etos has no kernel entropy service, so this reads x86-64's own: RDSEED
// (conditioned hardware entropy, meant for seeding) when the CPU has it, else
// RDRAND (a hardware-seeded CSPRNG, NIST SP 800-90A). Both are unprivileged
// instructions, so no syscall is involved and any process can call this.
//
// Per 64-bit word: try RDSEED (it can transiently run dry, so retry with a
// pause); if it stays dry, take that word from RDRAND instead; if that fails
// too the hardware is broken or exhausted and the call fails with EIO. A CPU
// with neither instruction (or a hypervisor that hides them -- QEMU's default
// `qemu64` CPU does; `-cpu host`/`max` expose them) gets ENOSYS, exactly as
// before this sysdep existed, so callers can fall back.
//
// This is the only entropy source the system has. Callers wanting a stream
// larger than GETENTROPY_MAX (256) should use it to seed their own CSPRNG, as
// getentropy(3) intends.

#include <abi-bits/errno.h>
#include <cpuid.h>
#include <immintrin.h>
#include <mlibc/all-sysdeps.hpp>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mlibc {

namespace {

enum : int { kUnprobed = 0, kNone = 1, kHasRdrand = 2, kHasRdseed = 4 };

// Probed once; every caller computes the same value, so a benign race on the
// first few calls is harmless and needs no lock.
int g_features = kUnprobed;

int probe() {
	int f = kNone;
	unsigned eax, ebx, ecx, edx;
	// CPUID.01H:ECX.RDRAND[bit 30]
	if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & (1u << 30)))
		f |= kHasRdrand;
	// CPUID.(EAX=07H,ECX=0H):EBX.RDSEED[bit 18]
	if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) && (ebx & (1u << 18)))
		f |= kHasRdseed;
	return f;
}

// RDSEED can fail transiently when the hardware entropy source is drained;
// Intel's guidance is to retry, pausing between attempts.
__attribute__((target("rdseed")))
bool rdseed64(uint64_t *out) {
	for (int i = 0; i < 128; i++) {
		unsigned long long v;
		if (_rdseed64_step(&v)) {
			*out = v;
			return true;
		}
		_mm_pause();
	}
	return false;
}

// RDRAND is documented to succeed within 10 retries on working hardware.
__attribute__((target("rdrnd")))
bool rdrand64(uint64_t *out) {
	for (int i = 0; i < 10; i++) {
		unsigned long long v;
		if (_rdrand64_step(&v)) {
			*out = v;
			return true;
		}
	}
	return false;
}

} // namespace

int Sysdeps<GetEntropy>::operator()(void *buffer, size_t length) {
	int features = __atomic_load_n(&g_features, __ATOMIC_RELAXED);
	if (features == kUnprobed) {
		features = probe();
		__atomic_store_n(&g_features, features, __ATOMIC_RELAXED);
	}
	if (!(features & (kHasRdrand | kHasRdseed)))
		return ENOSYS;

	auto *out = static_cast<unsigned char *>(buffer);
	while (length) {
		uint64_t word = 0;
		bool ok = false;
		if (features & kHasRdseed)
			ok = rdseed64(&word);
		if (!ok && (features & kHasRdrand))
			ok = rdrand64(&word);
		if (!ok)
			return EIO;

		size_t n = length < sizeof(word) ? length : sizeof(word);
		memcpy(out, &word, n);
		out += n;
		length -= n;
	}
	return 0;
}

} // namespace mlibc
