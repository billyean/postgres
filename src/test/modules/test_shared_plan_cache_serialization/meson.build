# Copyright (c) 2026, PostgreSQL Global Development Group

test_shared_plan_cache_serialization_sources = files(
  'test_shared_plan_cache_serialization.c',
)

if host_system == 'windows'
  test_shared_plan_cache_serialization_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_shared_plan_cache_serialization',
    '--FILEDESC', 'test_shared_plan_cache_serialization - test code for plan serialization',])
endif

test_shared_plan_cache_serialization = shared_module('test_shared_plan_cache_serialization',
  test_shared_plan_cache_serialization_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_shared_plan_cache_serialization

test_install_data += files(
  'test_shared_plan_cache_serialization.control',
  'test_shared_plan_cache_serialization--1.0.sql',
)

tests += {
  'name': 'test_shared_plan_cache_serialization',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_shared_plan_cache_serialization',
    ],
  },
}
