# dc-libavmpeg

A standalone KallistiOS project exploring MPEG-1 playback on the Sega Dreamcast.
It uses a vendored FFmpeg 0.5 source subset for MPEG-1 video decoding, MPEG-PS
demuxing, and MP2 audio decoding. Application and PVR code lives in `src/`;
FFmpeg-derived code and its notices remain under `vendor/ffmpeg/`.

The project builds separate video, audio/video, audio, and decoder-probe ELFs,
plus `libavmpeg.a`: a decoder library (demux + MPEG-1 video + MP2 audio, with
streaming input and exact frame seek) used as a movie backend by DCSinge. See
`docs/AVMPEG_API.md`. The default IDCT is the bit-exact integer implementation.
Hardware optimization is ongoing; build success alone does not imply a playback
performance target.

## Requirements

- KallistiOS and its SH-4 toolchain configured at `/opt/toolchains/dc/kos`
- `kos-cc`, `sh-elf-*`, and `kos-tool` from that KOS environment
- A Dreamcast running dcload-ip for hardware runs
- A local MPEG fixture for playback; copyrighted test clips are not included

Source KOS's environment in the same shell as each build or hardware command:

```sh
source /opt/toolchains/dc/kos/environ.sh
```

## Build

```sh
make                 # video-only decoder: dc-libavmpeg.elf
make av              # synchronized video/audio player: dc-libavmpeg-av.elf
make audio            # audio diagnostic: dc-libavmpeg-audio.elf
make probe            # decoder probe: dc-libavmpeg-probe.elf
make lib              # decoder library: libavmpeg.a (API in src/avmpeg.h)
make demo             # ELF that uses only the library API (playback + seek check)
```

Projects that embed the library out of tree (DCSinge does, as a git submodule)
build it with `libavmpeg.mk`, for example
`make -f libavmpeg.mk OUT=<dir> AV_STREAM=2`.

To load an ELF over dcload-ip, for example:

```sh
make run-av DC_IP=<dreamcast-ip>
```

`make cdi` optionally packages the default local fixture with the video-only
ELF. Build and package outputs are ignored by Git.

## Playback fixture

Fixtures are deliberately excluded from this repository. On Dreamcast, `/pc`
is KOS's BBA-backed virtual filesystem. The A/V player looks for
`/pc/config.ini` first and `/cd/config.ini` second. It resolves the configured
fixture from `/pc/fixtures/` or the disc root. Make those files available
through the dcload/BBA file service, or override the path macros at build time.
The repository's `.gitignore` excludes local benchmark clips in `fixtures/`.

### Input modes

Choose at build time with `MPEG_EXTRA_CFLAGS=-DAV_STREAM=<n>` (or `AV_STREAM=<n>`
for `libavmpeg.mk`):

| Mode | Behaviour | Use for |
| --- | --- | --- |
| `0` (default for the ELFs; `libavmpeg.mk` defaults to `2`) | Whole clip loaded into RAM first. Must fit the free heap (about 13 MB), so the 13.8 MB 640x480 fixtures fail with `Out of memory`. | Benchmarking; no I/O during playback. |
| `1` | Reads through a small buffer (`AV_STREAM_BUF`, default 16 KB). Frames are identical, but over dcload-ip reads happen during playback. | Clips that do not fit in RAM, simple setups. |
| `2` | Prebuffer ring (`AV_RING_BYTES`, default 2 MB, topped up in `AV_RING_CHUNK` = 16 KB pieces while the main loop is idle). | Disc and large-clip playback; this is what DCSinge uses. |

Measured on the 320x240 mono clip against preload: mode 1 costs about 2.4 ms/frame
and 9 dropped frames, mode 2 about 0.36 ms/frame (+1.8%) and 1-2 drops. At 640x480
the loop is never idle, so the ring only helps through the prefill. Keep the default
ring size: a 4 MB ring with a 32 KB chunk froze the console once (cause not found).
The full 13.8 MB 640x480 clip decodes all 720 frames when streamed, but at about
79 ms/frame (C IDCT) it cannot play in real time at that size. More numbers are in
`docs/RESULTS.md` and `docs/AVMPEG_API.md`. With mode 2, `avmpeg_input_stats()`
reports idle top-ups, blocking refills and the lowest ring fill.

### Seeking and encoding clips

Frame-exact seek needs a `.pidx` index next to the clip (`tools/build_pidx.py`).
`tools/encode_mpeg.sh VIDEO AUDIO OUT.mpg` encodes MPEG-1 video + MP2 audio with
closed GOPs and builds the `.pidx`; `--limit` makes short test clips and `--vf`
applies deinterlace or crop filters.

## Project layout

- `src/` — player, decoder glue, PVR presentation, and KOS entry points
- `vendor/ffmpeg/` — FFmpeg 0.5 source and upstream license notices
- `tools/` — clip encoding and `.pidx` building, plus layout and cache-probe
  utilities used during SH-4 investigation
- `docs/AVMPEG_API.md` — library API, input modes, DCSinge integration results
- `docs/STATUS.md`, `docs/RESULTS.md` — investigation log and benchmark results
- `docs/LICENSE_NOTES.md` — FFmpeg provenance and local modifications

## Licensing

The vendored FFmpeg files retain their upstream headers and license files.
Project code must keep FFmpeg-derived changes under `vendor/ffmpeg/` and record
them in `docs/LICENSE_NOTES.md`. See those notices for component-specific
licensing details.
