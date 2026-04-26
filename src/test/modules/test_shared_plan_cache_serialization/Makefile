# src/test/modules/test_shared_plan_cache_serialization/Makefile

MODULE_big = test_shared_plan_cache_serialization
OBJS = \
	$(WIN32RES) \
	test_shared_plan_cache_serialization.o
PGFILEDESC = "test_shared_plan_cache_serialization - test code for plan serialization"

EXTENSION = test_shared_plan_cache_serialization
DATA = test_shared_plan_cache_serialization--1.0.sql

REGRESS = test_shared_plan_cache_serialization

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_shared_plan_cache_serialization
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
