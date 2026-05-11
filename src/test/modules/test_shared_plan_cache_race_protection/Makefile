# src/test/modules/test_shared_plan_cache_race_protection/Makefile

MODULE_big = test_shared_plan_cache_race_protection
OBJS = \
	$(WIN32RES) \
	test_shared_plan_cache_race_protection.o
PGFILEDESC = "test_shared_plan_cache_race_protection - test code for shared plan cache race protection"

EXTENSION = test_shared_plan_cache_race_protection
DATA = test_shared_plan_cache_race_protection--1.0.sql

REGRESS = test_shared_plan_cache_race_protection
REGRESS_OPTS = --temp-config=$(srcdir)/test_spc_race.conf

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_shared_plan_cache_race_protection
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
