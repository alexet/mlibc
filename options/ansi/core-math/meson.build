# CORE-MATH (https://core-math.gitlabpages.inria.fr/) provides correctly
# rounded (<= 0.5 ulp error) implementations of the IEEE 754 binary32 and
# binary64 libm functions. The upstream sources are vendored below under
# binary64/ and binary32/, one subdirectory per function, unmodified except
# for dropping the test/benchmark/proof scaffolding that ships alongside
# each function upstream (see README.md in this directory for the exact
# revision vendored and how to refresh it).
#
# Each vendored file defines its function under the upstream `cr_<name>`
# symbol. Rather than checking in one small wrapper file per function that
# calls cr_<name>, glue/template.c.in is instantiated once per function
# below: it #defines cr_<name> to the libm name and #includes the vendored
# file, so the implementation ends up compiled once, directly under the
# libm name, in its own translation unit (no cross-TU call for a hot path
# like exp/sin/pow, and no cr_<name> symbol left in the final binary).
#
# Every vendored function shares the same trivial structure except for
# exp10 (which also needs to define the pow10 alias) and lgamma (which
# also needs to define lgamma_r using the shared signgam global) -- both
# carry that in their 'extra' entry below, appended after the #include.
#
# Only binary64 (double) and binary32 (float) are covered here: CORE-MATH's
# coverage of 80-bit long double is partial, so `long double` keeps using
# the musl-derived implementation in ../musl-generic-math.

# One entry per binary64/<dir>/ function; the binary32 counterpart is
# derived below as <name>f, from binary32/<dir>/<name>f.c.
core_math_funcs = [
	{'name': 'acos'},
	{'name': 'acosh'},
	{'name': 'asin'},
	{'name': 'asinh'},
	{'name': 'atan'},
	{'name': 'atan2'},
	{'name': 'atanh'},
	{'name': 'cbrt'},
	{'name': 'cos'},
	{'name': 'cosh'},
	{'name': 'erf'},
	{'name': 'erfc'},
	{'name': 'exp'},
	{
		'name': 'exp10',
		'extra_d': 'double pow10(double x)\n{\n\treturn exp10(x);\n}\n',
		'extra_f': 'float pow10f(float x)\n{\n\treturn exp10f(x);\n}\n',
	},
	{'name': 'exp2'},
	{'name': 'expm1'},
	{'name': 'hypot'},
	{
		'name': 'lgamma',
		'extra_d': 'double lgamma_r(double x, int *sign)\n{\n\tdouble r = lgamma(x);\n\t*sign = signgam;\n\treturn r;\n}\n',
		'extra_f': 'float lgammaf_r(float x, int *sign)\n{\n\tfloat r = lgammaf(x);\n\t*sign = signgam;\n\treturn r;\n}\n',
	},
	{'name': 'log'},
	{'name': 'log10'},
	{'name': 'log1p'},
	{'name': 'log2'},
	{'name': 'pow'},
	{'name': 'sin'},
	{'name': 'sincos'},
	{'name': 'sinh'},
	{'name': 'tan'},
	{'name': 'tanh'},
	{'name': 'tgamma'},
]

core_math_glue_template = 'glue/template.c.in'
core_math_sources = []

foreach f : core_math_funcs
	dir = f['name']

	core_math_sources += configure_file(
		input: core_math_glue_template,
		output: f['name'] + '.c',
		configuration: {
			'CR_NAME': 'cr_' + f['name'],
			'LIBM_NAME': f['name'],
			'VENDOR_FILE': meson.current_source_dir() / 'binary64' / dir / (f['name'] + '.c'),
			'EXTRA': f.get('extra_d', ''),
		},
	)

	name_f = f['name'] + 'f'
	core_math_sources += configure_file(
		input: core_math_glue_template,
		output: name_f + '.c',
		configuration: {
			'CR_NAME': 'cr_' + name_f,
			'LIBM_NAME': name_f,
			'VENDOR_FILE': meson.current_source_dir() / 'binary32' / dir / (name_f + '.c'),
			'EXTRA': f.get('extra_f', ''),
		},
	)
endforeach

libc_sublibs += static_library('mlibc-core-math',
	core_math_sources,
	pic: true,
	include_directories: libc_include_dirs,
	dependencies: libc_deps,
	c_args: [mlibc_c_args, compiler_c_args, '-DCORE_MATH_SUPPORT_ERRNO', '-D_GNU_SOURCE',
		'-Wno-unused', '-Wno-maybe-uninitialized', '-Wno-unknown-pragmas'],
	link_args: [mlibc_link_args])
