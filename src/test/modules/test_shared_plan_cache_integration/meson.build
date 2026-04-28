# Copyright (c) 2026, PostgreSQL Global Development Group

test_shared_plan_cache_integration_sources = files(
  'test_shared_plan_cache_integration.c',
)

if host_system == 'windows'
  test_shared_plan_cache_integration_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_shared_plan_cache_integration',
    '--FILEDESC', 'test_shared_plan_cache_integration - test code for shared plan cache L2 integration',])
endif

test_shared_plan_cache_integration = shared_module('test_shared_plan_cache_integration',
  test_shared_plan_cache_integration_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_shared_plan_cache_integration

test_install_data += files(
  'test_shared_plan_cache_integration.control',
  'test_shared_plan_cache_integration--1.0.sql',
)

tests += {
  'name': 'test_shared_plan_cache_integration',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_shared_plan_cache_integration',
    ],
    'regress_args': ['--temp-config', files('test_spc_integration.conf')],
  },
  'tap': {
    'tests': [
      't/001_shared_plan_cache_integration.pl',
    ],
  },
}
