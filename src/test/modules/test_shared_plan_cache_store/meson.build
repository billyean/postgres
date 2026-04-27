# Copyright (c) 2026, PostgreSQL Global Development Group

test_shared_plan_cache_store_sources = files(
  'test_shared_plan_cache_store.c',
)

if host_system == 'windows'
  test_shared_plan_cache_store_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_shared_plan_cache_store',
    '--FILEDESC', 'test_shared_plan_cache_store - test code for shared plan cache store',])
endif

test_shared_plan_cache_store = shared_module('test_shared_plan_cache_store',
  test_shared_plan_cache_store_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_shared_plan_cache_store

test_install_data += files(
  'test_shared_plan_cache_store.control',
  'test_shared_plan_cache_store--1.0.sql',
)

tests += {
  'name': 'test_shared_plan_cache_store',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_shared_plan_cache_store',
    ],
  },
  'tap': {
    'tests': [
      't/001_shared_plan_cache_store.pl',
    ],
  },
}
