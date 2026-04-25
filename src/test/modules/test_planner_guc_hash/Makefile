# src/test/modules/test_planner_guc_hash/Makefile

MODULE_big = test_planner_guc_hash
OBJS = \
	$(WIN32RES) \
	test_planner_guc_hash.o
PGFILEDESC = "test_planner_guc_hash - test code for planner GUC hash"

EXTENSION = test_planner_guc_hash
DATA = test_planner_guc_hash--1.0.sql

REGRESS = test_planner_guc_hash

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_planner_guc_hash
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
