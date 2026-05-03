# src/test/modules/test_shared_plan_cache_invalidation/Makefile

MODULE_big = test_shared_plan_cache_invalidation
OBJS = \
	$(WIN32RES) \
	test_shared_plan_cache_invalidation.o
PGFILEDESC = "test_shared_plan_cache_invalidation - test code for shared plan cache relation invalidation"

EXTENSION = test_shared_plan_cache_invalidation
DATA = test_shared_plan_cache_invalidation--1.0.sql

REGRESS = test_shared_plan_cache_invalidation
REGRESS_OPTS = --temp-config=$(srcdir)/test_spc_invalidation.conf

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_shared_plan_cache_invalidation
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
