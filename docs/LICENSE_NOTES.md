# License and Provenance Notes

- Upstream branch: `release/0.5`.
- Exact Git commit: `b256bd3be348fd37108583fdac7db6337994c26d`.
- Vendored source: complete original checkout at `vendor/ffmpeg/`.
- Retained upstream notices include `COPYING.LGPLv2.1`, `COPYING.LGPLv3`,
  `COPYING.GPLv2`, `COPYING.GPLv3`, `LICENSE`, and source-file headers.
- FFmpeg-derived files modified for this project:
  - `libavcodec/mpeg12.c`: type the hardware-acceleration slice-search state as
    `uint32_t`, matching `ff_find_start_code()`'s parameter. This avoids a
    modern SH-4 GCC pointer-type error; it preserves the 32-bit value and does
    not change software decoder behavior.
  - `libavcodec/mpeg12.c`: under `MPEG_DECODE_PROFILE`, sample macroblock parse
    and reconstruction timing at one macroblock in four. This diagnostic
    adds no calls to default builds and does not change decode behavior.
  - `libavcodec/mpegvideo.c`: under `MPEG_DECODE_PROFILE`, sample motion
    compensation and DCT residue/IDCT substage timing for those same
    macroblocks. No timer calls are compiled into default builds.
  - `libavcodec/mpegvideo.c`: under `MPEG_IDCT_BLOCK_COUNTS`, count
    `block_last_index` classes by I/P/B type and add/put dispatch. This is
    opt-in diagnostic instrumentation; no timing calls or IDCT arithmetic
    changes are included.
  - `libavcodec/mpeg12.c`: under `MPEG_DECODE_PROFILE`, one added line in the
    slice loop records the current picture type in `mpeg_decode_profile_pict`
    so sampled counters can be split by I/P/B. Diagnostic only.
  - `libavcodec/mpegvideo.c` and `libavcodec/simple_idct.c`: under
    `MPEG_CACHE_PROFILE_ADDR`, a one-shot hook records addresses (coefficient
    blocks, destination and reference rows, stack pointer, installed function
    pointers, `ff_cropTbl`) and calls the reporter in `src/cache_profile.c`.
    No arithmetic or dispatch change; nothing is compiled into default builds.
  - New application files (not FFmpeg-derived): `src/cache_profile.c/.h`,
    `src/decoder_profile.h`; the Makefile now links `src/cache_profile.o`
    (an empty object unless `MPEG_CACHE_PROFILE_ADDR` is defined).
  - `libavcodec/mpegvideo.c`: under `MPEG_ADDR_PRINT`, a `putchar`-only address
    dump on one macroblock (no string literals, no statics, so the data layout is
    unchanged). Diagnostic only.
  - `libavcodec/simple_idct.c`: under `MPEG_LAYOUT_PROBE`, three `nop`s in
    `ff_simple_idct_add` to perturb the function size for layout tests; never
    enabled in a kept build.
  - `libavcodec/dsputil.c`: under `MPEG_CROP_ALIGN`, an `aligned()` attribute on
    `ff_cropTbl` so its identity region falls in operand-cache sets 32-39. Off by
    default; no arithmetic change.
  - `libavcodec/simple_idct.c`: the C add/put implementations are now named
    `ff_simple_idct_add_body` and `ff_simple_idct_put_body`; the SH-4 assembly
    trampolines in `src/idct_island.S` retain the public FFmpeg symbols and call
    those bodies on a private stack. `tools/island_layout.py` generates the
    linker fragment that pins the trampoline/bodies and selects cache-set
    offsets. Guard, high-water, and return-check bookkeeping is compiled only
    with `MPEG_ISLAND_DEBUG`. Integer IDCT arithmetic is unchanged. The island
    matched 30 sampled reference frame checksums, but has not passed the 720/0
    presentation, robustness, or timing gates.
  - Link-time and post-link tooling, not FFmpeg-derived: `src/probe_patch.c`,
    `src/probe2_print.c`, `tools/probe_patch.sh`, `tools/probe_patch.ld`,
    `tools/probe2.py`, `tools/stack_patch.py`, `tools/alias_check.py` and `tools/layout_guard.py` inject or check counter probes into a linked
    ELF by adding a section in the gap after `.text` and patching literal-pool
    words; `src/probe_blob.c` documents a failed `--wrap` attempt.
    `src/cache_profile.c/.h` and the Makefile change are as listed above.
  - Host measurements used a scratch copy of `vendor/ffmpeg` under the session
    scratchpad with counters added to `bitstream.h`, `mpeg12.c` and
    `mpegvideo.c`; those edits are not in the repository.
- Slice 2 (no vendored source modified): `src/ffmpeg_config/config.h` now sets
  `CONFIG_MP2_DECODER`, `CONFIG_MPEGPS_DEMUXER`, `CONFIG_MPEGAUDIO_PARSER`,
  `CONFIG_MPEGVIDEO_PARSER` and `CONFIG_MPEGVIDEO_DEMUXER` to 1. Additional unmodified FFmpeg files are linked
  into the probe target: `libavcodec/{mpegaudiodec,mpegaudiodecheader,
  mpegaudio,mpegaudiodata,mpegaudio_parser,mpegvideo_parser,audioconvert,
  raw}.c` and `libavformat/{utils,cutils,aviobuf,avio,options,metadata,
  metadata_compat,raw,mpeg}.c` (all LGPL-2.1+; `mpeg.c` is the MPEG-PS
  demuxer, `raw.c` supplies the `mpegvideo` ES demuxer used for stream probing).
  The Makefile passes `-Wno-error=incompatible-pointer-types` for
  `mpegaudiodec.o` (`int32_t*` vs `int*`, both 32-bit) and `aviobuf.o`
  (`URLContext*` callbacks declared `void*`, identical SH-4 ABI) instead of
  editing those sources.
- Keep new application code under `src/` and FFmpeg-derived/modified code under
  `vendor/ffmpeg/`. No FFmpeg routines are copied into application files.
- No GPL-only component is intentionally selected. Verify the final compiled
  source list and applicable per-file headers before redistribution.
