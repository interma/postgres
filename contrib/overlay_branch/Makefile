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
REGRESS = overlay_branch_basic overlay_branch_user
REGRESS_OPTS = \
	--inputdir=$(srcdir)/test \
	--outputdir=./test_output \
	--temp-config=$(srcdir)/test/temp_instance_shared_libs.conf

# Handy helper: sync the just-built extension bits (so/sql/control) out of the
# writable /tmp/pg17-writable install tree into the user's "home" PG install at
# $${HOME}/pg17 (which is what most bare `pg_regress` / `psql` invocations pick up
# via $PATH).  Use this once after each `make install prefix=/tmp/pg17-writable`
# if your default pg_config points at $${HOME}/pg17.
HOME_PG_PREFIX ?= $(HOME)/pg17
.PHONY: install-home
install-home:
	@echo "--- overlay_branch: syncing installed bits to $(HOME_PG_PREFIX) ---"
	@$(MAKE) install prefix=$(HOME_PG_PREFIX)

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
