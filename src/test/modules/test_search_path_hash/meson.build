# Copyright (c) 2026, PostgreSQL Global Development Group

test_search_path_hash_sources = files(
  'test_search_path_hash.c',
)

if host_system == 'windows'
  test_search_path_hash_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_search_path_hash',
    '--FILEDESC', 'test_search_path_hash - test code for search path hash',])
endif

test_search_path_hash = shared_module('test_search_path_hash',
  test_search_path_hash_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_search_path_hash

test_install_data += files(
  'test_search_path_hash.control',
  'test_search_path_hash--1.0.sql',
)

tests += {
  'name': 'test_search_path_hash',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_search_path_hash',
    ],
  },
  'tap': {
    'tests': [
      't/001_search_path_hash.pl',
    ],
  },
}
