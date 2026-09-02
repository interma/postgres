# contrib/overlay_branch/Makefile

MODULE_big = overlay_branch
OBJS = \
	$(WIN32RES) \
	src/overlay_branch.o

EXTENSION = overlay_branch
DATA = overlay_branch--1.0.sql
PGFILEDESC = "overlay_branch - speculative database state using table overlay and delta store"

# Header files live in include/
override CPPFLAGS += -I$(srcdir)/include

# Regression tests live in test/{sql,expected}
REGRESS = overlay_branch_basic
REGRESS_OPTS = \
	--inputdir=$(srcdir)/test \
	--outputdir=./test_output

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/overlay_branch
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
