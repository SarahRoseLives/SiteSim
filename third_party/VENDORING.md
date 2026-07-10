# Vendored dependencies

Full source is vendored here so SiteSim rebuilds on a clean machine (no network
fetch, no submodules). SiteSim's CMake builds these from source; see the
top-level `CMakeLists.txt` (`ExternalProject`/superbuild wiring).

## mbelib-neo
- Upstream: https://github.com/arancormonk/mbelib-neo
- Vendored commit: `1bf4c73ba5c761c949363f9bc046083baf3782f9`
- Unmodified.

## dsd-neo
- Upstream: https://github.com/arancormonk/dsd-neo
- Vendored commit: `7ef48c4a864049692a664d957261ae6d2f187f04`
- Local modifications (SiteSim P25 ISP support + mingw portability):
  1. `include/dsd-neo/runtime/p25_isp_hooks.h` — new: ISP TSBK hook interface
     (`dsd_p25_isp_hooks_set`, `P25_ISP_*` opcodes).
  2. `src/runtime/p25_isp_hooks.c` — new: hook storage/dispatch. Registered in
     `src/runtime/CMakeLists.txt`.
  3. `src/protocol/p25/phase1/p25p1_tsbk_isp.{c,h}` — new: ISP TSBK decoder
     (`processTSBK_ISP`). Registered in `src/protocol/p25/CMakeLists.txt`.
  4. `include/dsd-neo/core/opts.h` — added `int p25_isp_mode;` field.
  5. `src/protocol/p25/phase1/p25p1_tsbk.c` — include ISP header and, in
     `tsbk_dispatch_message()`, route clear ISP TSBKs to `processTSBK_ISP` when
     `opts->p25_isp_mode` is set.
  6. `src/runtime/cli/args.c` — include `<getopt.h>` on mingw (upstream only
     provides getopt for MSVC; not used by SiteSim but part of the runtime lib).

To refresh from upstream: re-clone the pinned commit, re-apply items 1-6, and
copy the tree here (excluding `.git` and any `build*` directories).
