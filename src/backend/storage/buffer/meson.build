# Copyright (c) 2022-2026, PostgreSQL Global Development Group

backend_sources += files(
  'buf_init.c',
  'buf_table.c',
  'buf_usage_scan.c',
  'bufmgr.c',
  'freelist.c',
  'localbuf.c',
)

# SIMD usage-scan implementations
if host_cpu == 'x86_64'
  backend_sources += files('buf_usage_scan_sse2.c')
  if cdata.has('USE_AVX2_USAGE_SCAN_WITH_RUNTIME_CHECK')
    backend_sources += files('buf_usage_scan_avx2.c')
  endif
elif host_cpu == 'aarch64'
  backend_sources += files('buf_usage_scan_neon.c')
endif
