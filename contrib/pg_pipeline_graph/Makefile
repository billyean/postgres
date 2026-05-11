# contrib/pg_pipeline_graph/Makefile

MODULE_big = pg_pipeline_graph
OBJS = \
	$(WIN32RES) \
	pg_pipeline_graph.o
PGFILEDESC = "pg_pipeline_graph - diagnostic PlanState classifier prototype"

EXTENSION = pg_pipeline_graph
DATA = pg_pipeline_graph--1.0.sql

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_pipeline_graph
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
