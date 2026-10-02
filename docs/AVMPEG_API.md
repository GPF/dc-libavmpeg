# avmpeg: decoder library API

`src/avmpeg.h` wraps the vendored FFmpeg 0.5 demux + MPEG-1 video + MP2 audio decode behind a
small API. It has no PVR, no sound hardware and no threads; the caller owns presentation,
timing and scheduling (DCSinge's `dcfmv` worker, or `src/avmpeg_demo.c`). Calls are not thread
safe: serialise them on one handle.

Build: `make demo` (ELF using only the API), `make lib` (`libavmpeg.a`). Link the final program
with the IDCT island linker script, as the existing targets do. Choose the input mode with
`-DAV_STREAM=0/1/2` (see `src/av_source.h`); `MPEG_IDCT_ASM=2` selects the MAC.W IDCT.

```c
avmpeg_t *m = avmpeg_open("/cd/clip.mpg", NULL);
avmpeg_load_index(m, "/cd/clip.pidx");          /* needed for seeking */
const avmpeg_info_t *info = avmpeg_info(m);     /* size, fps, sample rate, channels, av_offset_us */

avmpeg_frame_t f;
int r = avmpeg_video_next(m, &f);               /* YUV420P planes, valid until the next call */
if (r == AVMPEG_AGAIN) { /* audio queue is full: drain audio first */ }

int16_t pcm[1152 * 2];
int n = avmpeg_audio_read(m, pcm, 1152);        /* interleaved PCM16, for snd_stream_set_callback */

avmpeg_seek_frame(m, 480);                      /* exact: next video_next() returns frame 480 */
```

A program stream interleaves both streams and the video runs ahead of the audio, so the library
queues demuxed packets per stream. `AVMPEG_AGAIN` means the stream you asked for has no packet and
the other stream's queue is full: drain the other stream, then retry.

## Seeking

FFmpeg 0.5's PS demuxer cannot seek by time on a freshly opened file, so seeking uses the `.pidx`
index from the pl_mpeg project (`tools/build_pidx.py`). `avmpeg_seek_frame()` byte-seeks to the
preceding indexed I-frame's PES packet, flushes both decoders, decodes forward (discarding) to the
target, and drops audio older than the target's media time.

Verified on hardware with `avmpeg_demo` (Lair 30 s clip, 320x240): frames 0, 7, 120, 480, 700,
100 (backward) and 400 all return pictures identical to the full-pass decode, with a fresh
decoder per seek and with one decoder for all seeks. Seek cost is the forward decode from the
I-frame: 0 ms when landing on an I-frame, about 0.25-0.3 s for 10-16 frames, about 1.0 s for the
74-frame jump to frame 100. The clip's I-frames are uneven (gaps up to about 90 frames), so worst
case is roughly 1.5-2 s; re-encode with a short fixed GOP if scene jumps must feel instant.

Lesson learned: the whole-file input needs a seek callback that answers `AVSEEK_SIZE`, otherwise
`av_seek_frame(..., AVSEEK_FLAG_BYTE)` silently does not move (see `src/av_source.c`).

## Measured (320x240 Lair clip, MAC.W IDCT)

Full pass through the API: 720 frames, 0 errors, first 30 frame checksums identical to FFmpeg
master's; video decode 20.6-20.9 ms/frame (the standalone player gets about 19.4; link layout
moves this by about a millisecond between builds), MP2 about 2.1-2.3 ms/frame.

## Not done yet

- `av_main.c` still has its own copy of the demux/decode logic; it is not ported onto the library.
- Open-GOP streams (B-frames after an I-frame that refer to the previous GOP) were not tested; `tools/encode_mpeg.sh` makes closed GOPs.

## Embedding in another project

`libavmpeg.mk` builds `libavmpeg.a` out of tree with the KOS environment active:

    make -f libavmpeg.mk OUT=/path/to/build AV_STREAM=2

It fixes the MAC.W IDCT (no IDCT-island linker script needed) and defaults to the ring input.
`ffmpeg_sources.mk` holds the FFmpeg source lists shared with the main Makefile. Link the archive
last, with `-lm`.

## Status: used by DCSinge

DCSinge (branch `mpeg-dcfmv`, `vendor/dc-libavmpeg` submodule) uses this library as a third
`dcfmv` backend. Verified on real hardware (GD-EMU, CDI image) and in an emulator with the full
33,759-frame Dragon's Lair (320x240, 22.05 kHz mono MP2, 268 MB `.mpg` plus `.pidx`):

- Playback of the whole title with the Lua scripts, about 25 scene jumps in a session, no errors.
- Scene jumps (`discSkipToFrame`) complete in about 0.3-0.6 s, plus about 1.5 s for the first one
  after startup (cold ring). With a closed GOP of 12 each seek decodes at most 11 frames forward.
- Input mode `AV_STREAM=2` (2 MB ring); `avmpeg_set_io_lock()` shares DCSinge's file-I/O mutex.

Not measured yet: per-frame decode, YUV422 conversion and MP2 cost under the game's load, and late
frames or audio underruns. Open: Mad Dog (29.97 fps stereo) encode and test.

### Encoding

`tools/encode_mpeg.sh VIDEO AUDIO OUT.mpg --expect FRAMES` encodes MPEG-1 video + MP2 audio and
builds the `.pidx`. It keeps every source frame (the `.m2v` is a raw stream with no timestamps, so
`--fps` states its real rate), uses closed GOPs of 12, and disables scene-cut I-frames (required
by FFmpeg's closed-GOP encoder). Lair: 1440x1080 `.m2v` + Vorbis `.ogg` to 320x240, 22.05 kHz mono.
