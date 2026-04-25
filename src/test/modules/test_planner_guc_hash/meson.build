# Copyright (c) 2026, PostgreSQL Global Development Group

test_planner_guc_hash_sources = files(
  'test_planner_guc_hash.c',
)

if host_system == 'windows'
  test_planner_guc_hash_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_planner_guc_hash',
    '--FILEDESC', 'test_planner_guc_hash - test code for planner GUC hash',])
endif

test_planner_guc_hash = shared_module('test_planner_guc_hash',
  test_planner_guc_hash_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_planner_guc_hash

test_install_data += files(
  'test_planner_guc_hash.control',
  'test_planner_guc_hash--1.0.sql',
)

tests += {
  'name': 'test_planner_guc_hash',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_planner_guc_hash',
    ],
  },
  'tap': {
    'tests': [
      't/001_planner_guc_hash.pl',
    ],
  },
}
