# Player CPU budget, audio rate and DCSinge integration notes

Why this exists: the player has to decode MPEG-1 video and MP2 audio at 23.976 or
29.97 fps and still leave CPU for a game (DCSinge: Lua scripts, input, a 60 Hz
overlay). These notes record what was measured, what was decided, and what is
deferred. Each number is marked **measured** (from a hardware log) or **estimate**.

All runs: real hardware over dcload-ip (`/pc/`, network backed, so I/O timings are
not GD-ROM timings), the `MPEG_IDCT_ASM=2` (MAC.W) player unless stated, preload
input, `config.ini` `fixture=` selecting the clip. "Busy" is the sum of the player's
own per-second stage timers (decode, frame copy, PVR present, MP2 decode, demux)
divided by 1000 ms, averaged over whole seconds (not the first/last partial ones).

## Decisions

- **Resolution: 320x240.** 640x480 at 24 fps is not reachable with this decoder
  (about 76 ms/frame decode against a 41.7 ms period; decode cost is about 66 us per
  macroblock at both sizes, and 640x480 has 4x the macroblocks). The PVR scales the
  picture to the 640x480 display at no CPU cost. See `IDCT_MAC_RESULTS.md`.
- **Audio: FFmpeg's MP2 decoder at 22.05 kHz.** Decode cost scales with frames per
  second (an MP2 frame is 1152 samples at any rate; about 2.4 ms per frame here), so
  halving the sample rate halves the decode cost. 22.05 kHz is MPEG-2 half-rate
  framing, which FFmpeg accepts. pl_mpeg's decoder does not (it needs 32/44.1/48 kHz).
  Dragon's Lair's existing `.dcmv` audio is already 22.05 kHz mono.
- Not pursued: swapping in pl_mpeg's MP2 decoder inside FFmpeg (about 3% CPU at
  44.1 kHz, and 22.05 kHz with FFmpeg's own decoder does better without a second
  decoder).

## Measurements

### MP2 decode alone (`make audiobench`, mono, 64 kbps, 30 s clip) -- measured

| Rate | Decode | Demux | Decode + demux | Share of one CPU |
|---|---|---|---|---|
| 44.1 kHz | 84-88 ms/s (2.2-2.3 ms/frame) | 11.4 ms/s | 96-99 ms/s | 9.6-9.9% |
| 32 kHz | 66.8 ms/s | 10.8 ms/s | 77.6 ms/s | 7.8% |
| 22.05 kHz | 46.2 ms/s | 10.5 ms/s | 56.7 ms/s | 5.7% |

Demux is mostly the player skipping video packets and does not scale with the audio
rate. The same decode costs about 25% more inside the player (109 ms/s at 44.1 kHz
mono) because it shares the CPU and caches with video decode.

### Whole player, per-second stage costs (ms per second of playback) -- measured

| Clip | Decode | Copy | Present | MP2 | Demux | Busy | Idle |
|---|---|---|---|---|---|---|---|
| Lair, 24 fps, mono 44.1 kHz (C IDCT) | 489 | 45 | 44 | 109 | 16 | 70.2% | 29.8% |
| Lair, 24 fps, mono 22.05 kHz | 479 | 45 | 43 | 59 | 15 | 64.2% | 35.8% |
| Maddog, 29.97 fps, stereo 44.1 kHz | 617 | 59 | 55 | 184 | 16 | 93.0% | 7.0% |
| Maddog, 29.97 fps, stereo 22.05 kHz | 633 | 58 | 54 | 92 | 15 | 85.2% | 14.8% |

The averages hide the heavy scenes. Lair at 22.05 kHz: seconds 15-26 average about
76% busy and the worst second (19) is about 97%. Maddog at 22.05 kHz stereo: seconds 1-4
run at 97-99% busy (98.4, 98.9, 99.0, 97.0) and the worst second is 99%. A game thread will meet
those, not the average. Both 22.05 kHz runs played with no audio underruns and the
decode-ahead queue stayed nearly full (average depth 11.8 and 11.0 of 12).
Maddog dropped 1 of 900 frames. (The player's video gate still reports FAIL for
clips that do not have exactly 720 frames: `av_main.c` hard-codes 720.)

### Comparison with the pl_mpeg fork (numbers supplied from the pl_mpeg project)

Same clips (`lair_320_23976_30s_spec.mpg`, `maddog_320_2997_30s_spec.mpg`), integer
IDCT, over `/pc/`:

| | pl_mpeg | dc-libavmpeg |
|---|---|---|
| Lair video decode | 26.3 ms/frame (video only) | 20.0 (C IDCT), about 19.4 (MAC.W) |
| Maddog video decode | 25.4 (video only) to 26.9 (with MP2) ms/frame | 20.5-21.1 |
| Decode + upload/copy/present, Lair | about 27.2 ms (decode + 0.94 upload) | about 23.7 ms (20.0 + 3.7) |
| Slowest 320x240 frame | I-frames 63 ms average, 98 ms max | about 44 ms max |
| MP2, Lair mono 44.1 kHz, decode + demux | about 65-70 ms/s | 96-99 ms/s |

pl_mpeg writes the PVR's macroblock layout directly, so its upload is cheap; the
FFmpeg player pays for a frame copy and conversion. pl_mpeg's audio-only bench
returned 1236 frames for a file that ffprobe says has 1149, and a 32 kHz re-encode
returned a single frame; neither was investigated, so the pl_mpeg audio comparison
above is the Lair 44.1 kHz run only.

### Storage -- from the pl_mpeg project's notes

Full Dragon's Lair (33,759 frames) as 320x240 MPEG-1: 252 MB, against about 584 MB for
the existing 320x240 `.dcmv`. A `.dcmv` VQ frame is 21,248 bytes at 320x240 (2 bits
per pixel plus a 2 KB codebook), about 4.1 Mbps before LZ4/zstd; the MPEG-1 clips are
1.5 Mbps.

## Reproducing

```
# MP2 decode only: set fixture= in config.ini, run from the repo root
make audiobench
kos-tool -t <dc-ip> -x dc-libavmpeg-audiobench.elf -m . | grep -a audiobench

# 22.05 kHz variants: copy the video untouched, re-encode only the audio
ffmpeg -i clip.mpg -map 0:v -map 0:a -c:v copy -c:a mp2 -ar 22050 -ac 1 -b:a 64k -f mpeg clip_22k.mpg
ffmpeg -i clip.mpg -map 0:v -map 0:a -c:v copy -c:a mp2 -ar 22050 -ac 2 -b:a 96k -f mpeg clip_22k_stereo.mpg

# full player: set fixture=, run, read the "per-second stage cost" table
make av MPEG_IDCT_ASM=2
tools/ab_loop.sh A.elf B.elf 3
```

The re-encodes above start from already-compressed audio, which is fine for CPU
measurement but not for judging quality.

## DCSinge integration findings (read from the DCSinge source, not run)

- `dcfmv` already runs a decode worker thread (`worker_thread`, default priority,
  `thd_sleep(1)` per step) that preloads up to 16 frames into 24 buffers. KOS uses a
  strict priority scheduler (lower number = higher priority, default 10), so the
  worker shares round-robin with the main thread at the same priority.
- `dcfmv_audio_poll()` is called from the worker once per step. With MPEG decode a
  step is a whole frame decode, so audio polling can lag by 20-45 ms.
- The worker holds `dcfmv_state_lock` for the whole step, including the decode.
  `dcfmv_tick` does not take it; other paths in `dcfmv.c` do (around lines 1663-1723;
  I did not check which).
- Audio is raw ADPCM read by a direct stream callback into the AICA buffers
  (`snd_stream_start_adpcm`), left and right as separate streams for stereo.
- The game is interactive at 60 Hz on top of 24/30 fps video, so game-thread latency
  matters as much as average idle CPU.

## Deferred (estimates unless noted)

| Lever | Expected effect |
|---|---|
| ADPCM audio (video-only MPEG-1 + `.dca`, your existing ADPCM path) | Lair mono: about 6 points. Maddog stereo: about 9 points (busy 85% -> about 76%) |
| Remove the frame copy (decode straight into the destination) | About 4.5-6 points |
| `pref` in motion compensation (the pl_mpeg fork has it; `dsputil_align.c` does not) | Unknown; entry-point prefetch in the IDCT was measured and gave nothing |
| Game-load simulator (fake 60 Hz thread at higher priority, configurable cost) | Measures the CPU the game can really use before video drops |
| Worker priority below the main loop, audio poll out of the worker | Protects game-thread latency; needs the main loop to block while waiting for vblank |
| Make the player's video gate use the clip's real frame count | Removes the false FAIL on 900-frame clips |
