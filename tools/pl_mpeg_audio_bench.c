/* MP2 decode-only benchmark for the pl_mpeg-based mpeg.c (companion to
 * src/audio_bench.c in dc-libavmpeg, which measures FFmpeg's MP2 decoder).
 *
 * Paste at the END of mpeg.c (it already has PL_MPEG_IMPLEMENTATION and the
 * MPEG_FILE_* macros), declare `void mpeg_bench_audio_decode(const char *path,
 * int passes);` in mpeg.h, and call it once from the example, e.g.
 *     mpeg_bench_audio_decode("/pc/tmp/lair_320_23976_30s_spec.mpg", 4);
 * Use the SAME clip as the FFmpeg bench (lair_320_23976_30s_spec.mpg, mono).
 *
 * What it does: preloads the file (no I/O inside the timed region), then per
 * pass creates a plm_t from memory with VIDEO DISABLED (as pl_mpeg's header
 * advises for audio-only decoding) and times each plm_decode_audio() call. That
 * call also parses the demux packets, so the number is "decode + demux" and is
 * directly comparable to the "decode+demux" figure the FFmpeg bench prints.
 * No sound stream, no AICA, no planar copy: pure decoder cost. */
void mpeg_bench_audio_decode(const char *path, int passes) {
    MPEG_FILE_TYPE fh = MPEG_FILE_OPEN(path);
    uint8_t *buf;
    long size;
    long got = 0;

    if(fh == MPEG_FILE_INVALID_HANDLE) {
        printf("plbench: cannot open %s\n", path);
        return;
    }
    MPEG_FILE_SEEK(fh, 0, SEEK_END);
    size = (long)MPEG_FILE_TELL(fh);
    MPEG_FILE_SEEK(fh, 0, SEEK_SET);
    buf = (uint8_t *)MPEG_MALLOC((size_t)size);
    if(!buf) {
        printf("plbench: out of memory for %ld bytes\n", size);
        MPEG_FILE_CLOSE(fh);
        return;
    }
    while(got < size) {
        ssize_t n = MPEG_FILE_READ(fh, buf + got, (size_t)(size - got));
        if(n <= 0)
            break;
        got += n;
    }
    MPEG_FILE_CLOSE(fh);
    if(got != size) {
        printf("plbench: short read %ld/%ld\n", got, size);
        MPEG_FREE(buf);
        return;
    }
    printf("plbench: loaded %s (%ld bytes)\n", path, size);

    for(int pass = 1; pass <= passes; pass++) {
        plm_t *plm = plm_create_with_memory(buf, (size_t)size, 0);
        uint64_t total_ns = 0, max_ns = 0;
        unsigned long frames = 0;
        int rate, chans;

        if(!plm) {
            printf("plbench: plm_create_with_memory failed\n");
            break;
        }
        plm_set_video_enabled(plm, 0);
        rate = plm_get_samplerate(plm);   /* initialises the decoders, untimed */
        chans = plm_get_audio_channels(plm);
        if(rate <= 0) {
            printf("plbench: no usable MPEG-1 audio (rate %d)\n", rate);
            plm_destroy(plm);
            break;
        }

        for(;;) {
            uint64_t t0 = timer_ns_gettime64();
            plm_samples_t *s = plm_decode_audio(plm);
            uint64_t dt = timer_ns_gettime64() - t0;

            if(!s)
                break;
            total_ns += dt;
            if(dt > max_ns)
                max_ns = dt;
            frames++;
        }

        {
            double secs = (double)frames * PLM_AUDIO_SAMPLES_PER_FRAME / (double)rate;
            double ms = total_ns / 1e6;

            printf("plbench: pass %d: %d Hz %d ch, %lu frames, %.2f s of audio\n",
                   pass, rate, chans, frames, secs);
            printf("plbench:   decode+demux %.3f ms/frame (max %.3f)  = %.1f ms per second of audio"
                   " = %.1f%% of one CPU\n",
                   frames ? ms / frames : 0.0, max_ns / 1e6, secs > 0 ? ms / secs : 0.0,
                   secs > 0 ? ms / 10.0 / secs : 0.0);
        }
        plm_destroy(plm);
    }
    printf("plbench: done\n");
    MPEG_FREE(buf);
}
