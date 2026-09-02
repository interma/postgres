# Copyright (c) 2024, PostgreSQL Global Development Group

overlay_branch_sources = files(
  'src/overlay_branch.c',
)

if host_system == 'windows'
  overlay_branch_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'overlay_branch',
    '--FILEDESC', 'overlay_branch - speculative database state using table overlay and delta store',])
endif

overlay_branch_inc = include_directories('include')

overlay_branch = shared_module('overlay_branch',
  overlay_branch_sources,
  include_directories: overlay_branch_inc,
  kwargs: contrib_mod_args,
)
contrib_targets += overlay_branch

install_data(
  'overlay_branch--1.0.sql',
  'overlay_branch.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'overlay_branch',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'overlay_branch_basic',
    ],
    'inputdir': 'test',
    'outputdir': 'test_output',
  },
}
