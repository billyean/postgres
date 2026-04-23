# src/test/modules/test_buffered_insert/Makefile

MODULE_big = test_buffered_insert
OBJS = \
	$(WIN32RES) \
	test_buffered_insert.o
PGFILEDESC = "test_buffered_insert - test Table AM buffered-insert lifecycle"

EXTENSION = test_buffered_insert
DATA = test_buffered_insert--1.0.sql

REGRESS = test_buffered_insert

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_buffered_insert
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
