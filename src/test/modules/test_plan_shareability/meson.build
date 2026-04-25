# Copyright (c) 2026, PostgreSQL Global Development Group

test_plan_shareability_sources = files(
  'test_plan_shareability.c',
)

if host_system == 'windows'
  test_plan_shareability_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_plan_shareability',
    '--FILEDESC', 'test_plan_shareability - test code for plan shareability predicate',])
endif

test_plan_shareability = shared_module('test_plan_shareability',
  test_plan_shareability_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_plan_shareability

test_install_data += files(
  'test_plan_shareability.control',
  'test_plan_shareability--1.0.sql',
)

tests += {
  'name': 'test_plan_shareability',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_plan_shareability',
    ],
  },
  'tap': {
    'tests': [
      't/001_plan_shareability.pl',
    ],
  },
}
