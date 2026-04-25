# src/test/modules/test_plan_shareability/Makefile

MODULE_big = test_plan_shareability
OBJS = \
	$(WIN32RES) \
	test_plan_shareability.o
PGFILEDESC = "test_plan_shareability - test code for plan shareability predicate"

EXTENSION = test_plan_shareability
DATA = test_plan_shareability--1.0.sql

REGRESS = test_plan_shareability

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_plan_shareability
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
