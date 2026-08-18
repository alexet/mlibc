/* C23 roundeven(): round to the nearest integer, breaking ties to even,
 * irrespective of the current rounding mode.
 *
 * GCC (>= 10) and clang (>= 17) lower __builtin_roundeven() to a single
 * instruction on most targets, but fall back to calling this symbol when
 * they can't (e.g. no target support, or the call can't be inlined), so
 * it needs to exist even if no caller here uses the name directly. */

#include <math.h>
#include <stdint.h>

double roundeven(double x)
{
	double ix = round(x);
	if (fabs(ix - x) == 0.5) {
		/* if ix is odd, we should return ix-1 if x>0, and ix+1 if x<0 */
		union {double f; uint64_t n;} u, v;
		u.f = ix;
		v.f = ix - copysign(1.0, x);
		if (__builtin_ctzll(v.n) > __builtin_ctzll(u.n))
			ix = v.f;
	}
	return ix;
}
