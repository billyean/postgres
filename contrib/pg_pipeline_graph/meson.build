# contrib/pg_pipeline_graph/meson.build

pg_pipeline_graph_sources = files(
  'pg_pipeline_graph.c',
)

if host_system == 'windows'
  pg_pipeline_graph_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_pipeline_graph',
    '--FILEDESC', 'pg_pipeline_graph - diagnostic PlanState classifier prototype',])
endif

pg_pipeline_graph = shared_module('pg_pipeline_graph',
  pg_pipeline_graph_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_pipeline_graph

install_data(
  'pg_pipeline_graph.control',
  'pg_pipeline_graph--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_pipeline_graph',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
    ],
  },
}
