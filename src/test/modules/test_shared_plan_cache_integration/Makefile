# src/test/modules/test_shared_plan_cache_integration/Makefile

MODULE_big = test_shared_plan_cache_integration
OBJS = \
	$(WIN32RES) \
	test_shared_plan_cache_integration.o
PGFILEDESC = "test_shared_plan_cache_integration - test code for shared plan cache L2 integration"

EXTENSION = test_shared_plan_cache_integration
DATA = test_shared_plan_cache_integration--1.0.sql

REGRESS = test_shared_plan_cache_integration
REGRESS_OPTS = --temp-config=$(srcdir)/test_spc_integration.conf

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_shared_plan_cache_integration
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
