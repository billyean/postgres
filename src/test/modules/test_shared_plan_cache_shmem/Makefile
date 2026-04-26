# src/test/modules/test_shared_plan_cache_shmem/Makefile

MODULE_big = test_shared_plan_cache_shmem
OBJS = \
	$(WIN32RES) \
	test_shared_plan_cache_shmem.o
PGFILEDESC = "test_shared_plan_cache_shmem - test code for shared plan cache shmem setup"

EXTENSION = test_shared_plan_cache_shmem
DATA = test_shared_plan_cache_shmem--1.0.sql

REGRESS = test_shared_plan_cache_shmem

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_shared_plan_cache_shmem
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
