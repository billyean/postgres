# Copyright (c) 2026, PostgreSQL Global Development Group

test_shared_plan_cache_race_protection_sources = files(
  'test_shared_plan_cache_race_protection.c',
)

if host_system == 'windows'
  test_shared_plan_cache_race_protection_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_shared_plan_cache_race_protection',
    '--FILEDESC', 'test_shared_plan_cache_race_protection - test code for shared plan cache race protection',])
endif

test_shared_plan_cache_race_protection = shared_module('test_shared_plan_cache_race_protection',
  test_shared_plan_cache_race_protection_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_shared_plan_cache_race_protection

test_install_data += files(
  'test_shared_plan_cache_race_protection.control',
  'test_shared_plan_cache_race_protection--1.0.sql',
)

tests += {
  'name': 'test_shared_plan_cache_race_protection',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_shared_plan_cache_race_protection',
    ],
    'regress_args': ['--temp-config', files('test_spc_race.conf')],
  },
  'tap': {
    'tests': [
      't/001_shared_plan_cache_race_protection.pl',
    ],
  },
}
