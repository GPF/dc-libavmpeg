# dc-libavmpeg

A standalone KallistiOS project exploring MPEG-1 playback on the Sega Dreamcast.
It uses a vendored FFmpeg 0.5 source subset for MPEG-1 video decoding, MPEG-PS
demuxing, and MP2 audio decoding. Application and PVR code lives in `src/`;
FFmpeg-derived code and its notices remain under `vendor/ffmpeg/`.

The project builds separate video, audio/video, audio, and decoder-probe ELFs.
The default IDCT is the bit-exact integer implementation. Hardware optimization
is ongoing; build success alone does not imply a playback performance target.

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
```

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

By default the A/V player loads the whole clip into RAM before decoding, so a clip
must fit in the free heap (about 13 MB with the decoder allocated): the 13.8 MB
640x480 fixtures fail with `Out of memory` while probing. Building with
`MPEG_EXTRA_CFLAGS=-DAV_STREAM=1` reads the clip through a 16 KB buffer
(`-DAV_STREAM_BUF=<bytes>`) instead. Frames are identical, but over dcload-ip
the reads happen during playback: on the 320x240 mono clip that costs about
2.4 ms/frame of decode time and 9 dropped frames (128 KB buffer: 2.2 ms and
39 drops) against 0 drops preloaded, so keep preload for benchmarking and use
streaming for clips that do not fit or for disc playback. `-DAV_STREAM=2` adds a
prebuffer ring (`AV_RING_BYTES`, default 2 MB, filled before playback and topped up in
`AV_RING_CHUNK` = 16 KB pieces whenever the main loop is idle), so decode normally never
waits on the disc: on the 320x240 clip it costs about 0.36 ms/frame (+1.8%) and 1-2
dropped frames against preload, versus +2.4 ms and 9 drops for plain streaming. At
640x480 the loop is never idle, so the ring only helps through the prefill and every
later read is a blocking refill. A 4 MB ring with a 32 KB chunk froze the console on the
640x480 clip once (cause not found; the heap was not exhausted with a 1 MB ring), so
keep the defaults. With streaming the full
13.8 MB 640x480 clip opens and decodes all 720 frames with no errors (about 79 ms/frame
with the C IDCT and a 4-slot queue, roughly twice the 41.7 ms frame period, so it
cannot play in real time at that size).

## Project layout

- `src/` — player, decoder glue, PVR presentation, and KOS entry points
- `vendor/ffmpeg/` — FFmpeg 0.5 source and upstream license notices
- `tools/` — layout and cache-probe utilities used during SH-4 investigation
- `docs/LICENSE_NOTES.md` — FFmpeg provenance and local modifications

## Licensing

The vendored FFmpeg files retain their upstream headers and license files.
Project code must keep FFmpeg-derived changes under `vendor/ffmpeg/` and record
them in `docs/LICENSE_NOTES.md`. See those notices for component-specific
licensing details.
