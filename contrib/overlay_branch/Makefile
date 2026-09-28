# contrib/overlay_branch/Makefile

MODULE_big = overlay_branch
OBJS = \
	$(WIN32RES) \
	src/overlay_branch.o \
	src/branch_scan.o \
	src/delta_store.o \
	src/write_redirect.o \
	src/branch_lifecycle.o

EXTENSION = overlay_branch
DATA = overlay_branch--1.0.sql
PGFILEDESC = "overlay_branch - speculative database state using table overlay and delta store"

# Header files live in include/
override CPPFLAGS += -I$(srcdir)/include

# Directory layout for test/ (clean separation of 3 test tiers):
#   test/regress/{sql,expected}   L3 pg_regress (make check gate)
#   test/isolation/specs          L1 pg_isolation_regress (T8 concurrency)
#   test/isolation/expected       L1 baseline .out files
#   test/isolation/isolation_schedule
#   test/bench/                   L2 pgbench manual race-detection fixture
# Temp instance shared-libs conf lives at root level (shared by both harnesses).
REGRESS = \
  overlay_branch_basic \
  overlay_branch_user \
  overlay_branch_scan_pk \
  overlay_branch_scan_type \
  overlay_branch_puredelta_mix \
  overlay_branch_upsert \
  overlay_branch_puredelta \
  overlay_branch_mvcc_usage \
  overlay_branch_mvcc_bounds \
  overlay_branch_mvcc_review \
  overlay_branch_rentry
REGRESS_OPTS = \
	--inputdir=$(srcdir)/test/regress \
	--outputdir=./test_output \
	--temp-config=$(srcdir)/temp_instance_shared_libs.conf
ISOLATION_OPTS = \
	--inputdir=$(srcdir)/test/isolation \
	--temp-config=$(srcdir)/temp_instance_shared_libs.conf

# L1 pg_isolation_regress concurrency tests (T8 total acceptance).
ISOLATION = ob_apply_mutex ob_state_inval ob_applying_freeze ob_snapshot_mode ob_branchscan_rescan pure_delta_upsert pure_delta_update pure_delta_delete
EXTRA_CLEAN += test_output output_iso tmp_check_iso

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
