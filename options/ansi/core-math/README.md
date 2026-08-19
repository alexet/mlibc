# CORE-MATH

This directory vendors selected `double` (binary64) and `float` (binary32)
implementations from [CORE-MATH](https://core-math.gitlabpages.inria.fr/)
([repository](https://gitlab.inria.fr/core-math/core-math)), a collection
of correctly rounded (<= 0.5 ulp error) libm functions, and uses them in
place of the equivalent musl-derived implementations in
`../musl-generic-math` for the functions listed below.

Vendored from commit `0279f1cfba3e17c0d4f761fb7aa60ffe651bab38` of
`https://gitlab.inria.fr/core-math/core-math.git`.

## Layout

* `binaryNN/<function>/` — the upstream implementation file for
  `<function>`, plus any header it directly `#include`s, copied verbatim.
  Each file exports its function as `cr_<name>` (CORE-MATH's own
  convention); nothing else in this tree is modified.
* `glue/template.c.in` — `meson.build` instantiates this once per function
  (`configure_file()`, driven by the `core_math_funcs` table in
  `meson.build`), `#define`ing `cr_<name>` to the libm name and
  `#include`ing the corresponding `binaryNN/<function>/` file, e.g.
  `#define cr_exp exp` then `#include ".../binary64/exp/exp.c"`.
  Compiling this way, rather than checking in one small wrapper file per
  function that calls `cr_<name>`, keeps each implementation in a single
  translation unit — no cross-TU call for a hot path like exp/sin/pow —
  and leaves no `cr_<name>` symbol in the final binary, without having 58
  nearly-identical files to maintain by hand. (They can't be merged into
  fewer translation units instead: CORE-MATH's own internal helper names
  like `fasttwosum`/`muldd`/`as_ldexp` are reused, unqualified, across
  many otherwise-unrelated functions, so two of these files in the same
  TU routinely redefine the same static helper.)

## Coverage

Replaced (double and float): acos, acosh, asin, asinh, atan, atan2, atanh,
cbrt, cos, cosh, erf, erfc, exp, exp10 (and the `pow10` alias), exp2,
expm1, hypot, lgamma (and `lgamma_r`), log, log10, log1p, log2, pow, sin,
sincos, sinh, tan, tanh, tgamma.

Left on the musl-derived implementation:

* Everything for `long double`: CORE-MATH's 80-bit long double coverage is
  partial (only atan2l, cbrtl, cosl, expl, exp2l, hypotl, log2l, powl,
  sinl), so long double keeps a single, consistent implementation instead
  of a partial swap.
* `j0`, `j1`, `jn`: not provided by CORE-MATH.
* Exact/hardware operations that don't need a correctly-rounded
  replacement: `sqrt`, `fma`, `ceil`, `floor`, `round`, `trunc`, `fmod`,
  `remainder`, `ldexp`, `frexp`, `modf`, `copysign`, `nextafter`,
  `scalbn`, `fdim`, `fmax`, `fmin`, `ilogb`, `logb`, and similar.

## Updating

1. Clone `https://gitlab.inria.fr/core-math/core-math.git` at the desired
   revision.
2. For each function above, copy `src/binary64/<f>/<f>.c` (double) and
   `src/binary32/<f>/<f>f.c` (float) into `binaryNN/<f>/`, along with any
   file it `#include "..."`s from the same upstream directory (test
   scaffolding such as `function_under_test.h`, `check_special.c`,
   `*_mpfr.c`, `*.sollya`, `*.wc`, `*.sage` is not needed and should not be
   copied).
3. Update the commit hash above and re-run the build.

CORE-MATH is MIT-licensed; see `LICENSE` in this directory.
