# Copyright (c) 2022-2026, PostgreSQL Global Development Group

test_buffered_insert_sources = files(
  'test_buffered_insert.c',
)

if host_system == 'windows'
  test_buffered_insert_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_buffered_insert',
    '--FILEDESC', 'test_buffered_insert - test Table AM buffered-insert lifecycle',])
endif

test_buffered_insert = shared_module('test_buffered_insert',
  test_buffered_insert_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_buffered_insert

test_install_data += files(
  'test_buffered_insert.control',
  'test_buffered_insert--1.0.sql',
)

tests += {
  'name': 'test_buffered_insert',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_buffered_insert',
    ],
  },
}
