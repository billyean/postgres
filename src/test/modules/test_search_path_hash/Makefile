# src/test/modules/test_search_path_hash/Makefile

MODULE_big = test_search_path_hash
OBJS = \
	$(WIN32RES) \
	test_search_path_hash.o
PGFILEDESC = "test_search_path_hash - test code for search path hash"

EXTENSION = test_search_path_hash
DATA = test_search_path_hash--1.0.sql

REGRESS = test_search_path_hash

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_search_path_hash
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
