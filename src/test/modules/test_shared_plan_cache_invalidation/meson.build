# Copyright (c) 2026, PostgreSQL Global Development Group

test_shared_plan_cache_invalidation_sources = files(
  'test_shared_plan_cache_invalidation.c',
)

if host_system == 'windows'
  test_shared_plan_cache_invalidation_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_shared_plan_cache_invalidation',
    '--FILEDESC', 'test_shared_plan_cache_invalidation - test code for shared plan cache relation invalidation',])
endif

test_shared_plan_cache_invalidation = shared_module('test_shared_plan_cache_invalidation',
  test_shared_plan_cache_invalidation_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_shared_plan_cache_invalidation

test_install_data += files(
  'test_shared_plan_cache_invalidation.control',
  'test_shared_plan_cache_invalidation--1.0.sql',
)

tests += {
  'name': 'test_shared_plan_cache_invalidation',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_shared_plan_cache_invalidation',
    ],
    'regress_args': ['--temp-config', files('test_spc_invalidation.conf')],
  },
  'tap': {
    'tests': [
      't/001_shared_plan_cache_invalidation.pl',
    ],
  },
}
