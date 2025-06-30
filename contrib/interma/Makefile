# contrib/interma/Makefile

MODULE_big = interma
OBJS = \
	$(WIN32RES) \
	backend_status.o

EXTENSION = interma
DATA = interma--1.0.sql
PGFILEDESC = "interma's demo extension for PostgreSQL"

REGRESS = interma

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/interma
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
