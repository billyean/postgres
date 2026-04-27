# src/test/modules/test_shared_plan_cache_store/Makefile

MODULE_big = test_shared_plan_cache_store
OBJS = \
	$(WIN32RES) \
	test_shared_plan_cache_store.o
PGFILEDESC = "test_shared_plan_cache_store - test code for shared plan cache store"

EXTENSION = test_shared_plan_cache_store
DATA = test_shared_plan_cache_store--1.0.sql

REGRESS = test_shared_plan_cache_store

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_shared_plan_cache_store
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
