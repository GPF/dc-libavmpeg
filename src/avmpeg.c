#include "avmpeg.h"

#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "av_source.h"
#include "libavutil/mem.h"

int avmpeg_verbose;

#define PF_CAP 128          /* packets per stream queue */
#define PCM_CHUNK_BYTES AVCODEC_MAX_AUDIO_FRAME_SIZE

typedef struct {
    AVPacket pk[PF_CAP];
    unsigned head, count;
} pkt_fifo_t;

struct avmpeg {
    av_source_t src;
    avmpeg_info_t info;
    avmpeg_stats_t stats;
    pkt_fifo_t vfifo, afifo;
    AVFrame *frame;
    int16_t *pcm;               /* decoded, not yet handed out */
    size_t pcm_pos, pcm_len;    /* in sample frames */
    int demux_eof;
    int video_flushed;
    unsigned long vindex;
    struct { double time; uint32_t off; } *idx;   /* .pidx entries */
    uint32_t idx_count;
    long picture_count;
    int64_t audio_drop_before_us;   /* seek: drop audio packets older than this clock */
    int audio_dropping;
};

static void fifo_push(pkt_fifo_t *f, const AVPacket *pkt) {
    f->pk[(f->head + f->count) % PF_CAP] = *pkt;
    f->count++;
}

static AVPacket *fifo_front(pkt_fifo_t *f) {
    return &f->pk[f->head];
}

static void fifo_pop(pkt_fifo_t *f) {
    av_free_packet(&f->pk[f->head]);
    f->head = (f->head + 1) % PF_CAP;
    f->count--;
}

static void fifo_clear(pkt_fifo_t *f) {
    while (f->count)
        fifo_pop(f);
}

/* Read one packet from the file into the queue of the stream it belongs to; packets
 * of other streams are dropped. Returns 1 if a packet was read, 0 at end of input,
 * AVMPEG_AGAIN if either queue has no room for it. */
static int demux_one(avmpeg_t *m, int64_t *vpts_us, int64_t *apts_us) {
    AVPacket pkt;
    uint64_t t0;
    int ret;

    if (m->demux_eof)
        return 0;
    if (m->vfifo.count >= PF_CAP || m->afifo.count >= PF_CAP)
        return AVMPEG_AGAIN;
    t0 = timer_us_gettime64();
    ret = av_read_frame(m->src.ic, &pkt);
    if (ret < 0) {
        m->demux_eof = 1;
        m->stats.demux_us += timer_us_gettime64() - t0;
        return 0;
    }
    if (pkt.stream_index == m->src.vidx || pkt.stream_index == m->src.aidx) {
        int is_a = pkt.stream_index == m->src.aidx;

        if (pkt.pts != AV_NOPTS_VALUE) {
            int64_t *dst = is_a ? apts_us : vpts_us;

            if (dst && *dst == AV_NOPTS_VALUE)
                *dst = av_rescale_q(pkt.pts, m->src.ic->streams[pkt.stream_index]->time_base,
                                    AV_TIME_BASE_Q);
        }
        if (is_a && m->audio_dropping) {
            int64_t at = pkt.pts != AV_NOPTS_VALUE ?
                av_rescale_q(pkt.pts, m->src.ic->streams[pkt.stream_index]->time_base,
                             AV_TIME_BASE_Q) : AV_NOPTS_VALUE;

            if (at != AV_NOPTS_VALUE && at < m->audio_drop_before_us) {
                av_free_packet(&pkt);
                m->stats.demux_us += timer_us_gettime64() - t0;
                return 1;
            }
            m->audio_dropping = 0;
        }
        av_dup_packet(&pkt);
        fifo_push(is_a ? &m->afifo : &m->vfifo, &pkt);
        if (m->vfifo.count > m->stats.video_queue_max)
            m->stats.video_queue_max = m->vfifo.count;
        if (m->afifo.count > m->stats.audio_queue_max)
            m->stats.audio_queue_max = m->afifo.count;
    } else {
        av_free_packet(&pkt);
    }
    m->stats.demux_us += timer_us_gettime64() - t0;
    return 1;
}

avmpeg_t *avmpeg_open(const char *path, const avmpeg_config_t *cfg) {
    avmpeg_t *m = calloc(1, sizeof(*m));
    AVStream *vs;
    int64_t vpts = AV_NOPTS_VALUE, apts = AV_NOPTS_VALUE;
    int ret;

    if (!m)
        return NULL;
    m->src.vidx = m->src.aidx = -1;
    m->picture_count = -1;
    ret = av_source_open(&m->src, path, cfg && cfg->idct_algo ? cfg->idct_algo : FF_IDCT_SIMPLE);
    if (ret != 0) {
        if (ret == AV_SOURCE_NO_FILE)
            printf("avmpeg: cannot read %s\n", path);
        free(m);
        return NULL;
    }
    av_source_prefill(&m->src);
    if (m->src.vc->pix_fmt != PIX_FMT_YUV420P || (m->src.vc->width & 1) ||
        (m->src.vc->height & 1) || m->src.ac->channels < 1 || m->src.ac->channels > 2) {
        printf("avmpeg: unsupported format (pix_fmt %d, %dx%d, %d ch)\n", m->src.vc->pix_fmt,
               m->src.vc->width, m->src.vc->height, m->src.ac->channels);
        goto fail;
    }
    m->frame = avcodec_alloc_frame();
    m->pcm = av_malloc(PCM_CHUNK_BYTES);
    if (!m->frame || !m->pcm) {
        printf("avmpeg: out of memory\n");
        goto fail;
    }

    vs = m->src.ic->streams[m->src.vidx];
    m->info.width = m->src.vc->width;
    m->info.height = m->src.vc->height;
    m->info.fps_num = vs->r_frame_rate.num > 0 ? vs->r_frame_rate.num : 24000;
    m->info.fps_den = vs->r_frame_rate.num > 0 ? vs->r_frame_rate.den : 1001;
    m->info.frame_us = (int64_t)1000000 * m->info.fps_den / m->info.fps_num;
    m->info.sample_rate = m->src.ac->sample_rate;
    m->info.channels = m->src.ac->channels;

    /* The PS runs video ahead of audio; read until both first timestamps are known
     * (the packets stay queued for the decoders). */
    while ((vpts == AV_NOPTS_VALUE || apts == AV_NOPTS_VALUE) &&
           demux_one(m, &vpts, &apts) == 1)
        ;
    m->info.av_offset_us = (vpts != AV_NOPTS_VALUE && apts != AV_NOPTS_VALUE) ? vpts - apts : 0;
    memset(&m->stats, 0, sizeof(m->stats));
    return m;
fail:
    avmpeg_close(m);
    return NULL;
}

void avmpeg_close(avmpeg_t *m) {
    if (!m)
        return;
    fifo_clear(&m->vfifo);
    fifo_clear(&m->afifo);
    free(m->idx);
    av_free(m->pcm);
    av_free(m->frame);
    av_source_close(&m->src);
    free(m);
}

const avmpeg_info_t *avmpeg_info(const avmpeg_t *m) {
    return &m->info;
}

static int decode_video_packet(avmpeg_t *m, const uint8_t *data, int size, int *got) {
    uint64_t t0 = timer_us_gettime64();
    int ret;

    *got = 0;
    ret = avcodec_decode_video(m->src.vc, m->frame, got, data, size);
    m->stats.video_decode_us += timer_us_gettime64() - t0;
    if (ret < 0)
        m->stats.video_errors++;
    return ret;
}

int avmpeg_video_next(avmpeg_t *m, avmpeg_frame_t *out) {
    for (;;) {
        int got = 0;

        if (m->vfifo.count) {
            AVPacket *p = fifo_front(&m->vfifo);

            decode_video_packet(m, p->data, p->size, &got);
            fifo_pop(&m->vfifo);
        } else {
            int r = demux_one(m, NULL, NULL);

            if (r == AVMPEG_AGAIN)
                return AVMPEG_AGAIN;
            if (r == 1)
                continue;
            if (m->video_flushed)
                return AVMPEG_EOF;
            /* end of input: drain the decoder's delayed frames */
            decode_video_packet(m, NULL, 0, &got);
            if (!got) {
                m->video_flushed = 1;
                return AVMPEG_EOF;
            }
        }
        if (got) {
            int i;

            for (i = 0; i < 3; i++) {
                out->plane[i] = m->frame->data[i];
                out->stride[i] = m->frame->linesize[i];
            }
            out->width = m->info.width;
            out->height = m->info.height;
            out->index = m->vindex++;
            out->pict_type = m->frame->pict_type;
            m->stats.video_frames++;
            return AVMPEG_OK;
        }
    }
}

int avmpeg_audio_read(avmpeg_t *m, int16_t *dst, int max_frames) {
    const int ch = m->info.channels;

    if (max_frames <= 0)
        return AVMPEG_ERROR;
    while (m->pcm_pos >= m->pcm_len) {
        AVPacket *p;
        const uint8_t *data;
        int size;

        m->pcm_pos = m->pcm_len = 0;
        if (!m->afifo.count) {
            int r = demux_one(m, NULL, NULL);

            if (r == AVMPEG_AGAIN)
                return AVMPEG_AGAIN;
            if (r == 1)
                continue;
            return AVMPEG_EOF;
        }
        p = fifo_front(&m->afifo);
        data = p->data;
        size = p->size;
        /* one MP2 frame per packet (the parser guarantees it); samples of a packet
         * that holds more are decoded in sequence and appended */
        while (size > 0) {
            int out_bytes = PCM_CHUNK_BYTES - (int)(m->pcm_len * ch * 2);
            uint64_t t0 = timer_us_gettime64();
            int len;

            len = avcodec_decode_audio2(m->src.ac, m->pcm + m->pcm_len * ch, &out_bytes, data,
                                        size);
            m->stats.audio_decode_us += timer_us_gettime64() - t0;
            if (len <= 0) {
                if (len < 0)
                    m->stats.audio_errors++;
                break;
            }
            data += len;
            size -= len;
            if (out_bytes > 0) {
                m->pcm_len += (size_t)out_bytes / (size_t)(2 * ch);
                m->stats.audio_chunks++;
            }
        }
        fifo_pop(&m->afifo);
    }
    {
        size_t n = m->pcm_len - m->pcm_pos;

        if (n > (size_t)max_frames)
            n = (size_t)max_frames;
        memcpy(dst, m->pcm + m->pcm_pos * ch, n * ch * 2);
        m->pcm_pos += n;
        return (int)n;
    }
}

int avmpeg_pump(avmpeg_t *m) {
    return av_source_pump(&m->src);
}

int avmpeg_load_index(avmpeg_t *m, const char *pidx_path) {
    FILE *fp = fopen(pidx_path, "rb");
    uint8_t raw[12];
    uint32_t count, i;

    if (!fp) {
        printf("avmpeg: cannot open index %s\n", pidx_path);
        return -1;
    }
    if (fread(&count, 4, 1, fp) != 1 || count == 0 || count > (1u << 24)) {
        printf("avmpeg: bad index %s\n", pidx_path);
        fclose(fp);
        return -1;
    }
    free(m->idx);
    m->idx = malloc((size_t)count * sizeof(*m->idx));
    if (!m->idx) {
        fclose(fp);
        return -1;
    }
    for (i = 0; i < count; i++) {
        /* explicit 12-byte entries: the in-memory struct is 16 bytes on SH-4 */
        if (fread(raw, 1, 12, fp) != 12) {
            printf("avmpeg: truncated index %s\n", pidx_path);
            free(m->idx);
            m->idx = NULL;
            fclose(fp);
            return -1;
        }
        memcpy(&m->idx[i].time, raw, 8);
        memcpy(&m->idx[i].off, raw + 8, 4);
    }
    m->idx_count = count;
    m->picture_count = -1;
    if (fread(raw, 1, 8, fp) == 8 && !memcmp(raw, "PCNT", 4)) {
        uint32_t pc;

        memcpy(&pc, raw + 4, 4);
        m->picture_count = (long)pc;
    }
    fclose(fp);
    return 0;
}

long avmpeg_frame_count(const avmpeg_t *m) {
    return m->picture_count;
}

int avmpeg_seek_frame(avmpeg_t *m, long frame) {
    const double fps = (double)m->info.fps_num / m->info.fps_den;
    double target, t0;
    uint32_t lo = 0, hi, mid;
    long land;
    int64_t pos;
    avmpeg_frame_t f;

    if (!m->idx || frame < 0)
        return -1;
    t0 = m->idx[0].time;
    target = t0 + (double)frame / fps;
    /* last entry with time <= target (1 ms slack for an I-frame exactly on target) */
    hi = m->idx_count;
    while (lo + 1 < hi) {
        mid = (lo + hi) / 2;
        if (m->idx[mid].time <= target + 0.001)
            lo = mid;
        else
            hi = mid;
    }
    land = (long)((m->idx[lo].time - t0) * fps + 0.5);
    pos = (int64_t)m->idx[lo].off - 4;      /* back to the 00 00 01 E0 start code */
    if (avmpeg_verbose)
        printf("avmpeg: seek frame %ld -> index entry %lu (pts %.3f s, byte %lld) = frame %ld\n",
               frame, (unsigned long)lo, m->idx[lo].time, (long long)pos, land);
    if (av_seek_frame(m->src.ic, -1, pos, AVSEEK_FLAG_BYTE) < 0) {
        if (avmpeg_verbose)
            printf("avmpeg: av_seek_frame(byte %lld) failed\n", (long long)pos);
        return -1;
    }
    fifo_clear(&m->vfifo);
    fifo_clear(&m->afifo);
    avcodec_flush_buffers(m->src.vc);
    avcodec_flush_buffers(m->src.ac);
    m->pcm_pos = m->pcm_len = 0;
    m->demux_eof = 0;
    m->video_flushed = 0;
    m->vindex = (unsigned long)land;
    /* audio from the target frame's media time on (PS clock, same base as the index) */
    m->audio_drop_before_us = (int64_t)(target * AV_TIME_BASE);
    m->audio_dropping = 1;
    while (m->vindex < (unsigned long)frame) {
        int r = avmpeg_video_next(m, &f);

        if (r != AVMPEG_OK) {
            if (avmpeg_verbose)
                printf("avmpeg: forward decode stopped at frame %lu of %ld: %d\n", m->vindex,
                       frame, r);
            return -1;      /* AGAIN here means the audio queue filled: not expected */
        }
    }
    return 0;
}

int avmpeg_seek(avmpeg_t *m, double seconds) {
    return avmpeg_seek_frame(m, (long)(seconds * m->info.fps_num / m->info.fps_den + 0.5));
}

const avmpeg_stats_t *avmpeg_stats(const avmpeg_t *m) {
    return &m->stats;
}

void avmpeg_reset_stats(avmpeg_t *m) {
    memset(&m->stats, 0, sizeof(m->stats));
}

void avmpeg_set_io_lock(void (*lock)(void), void (*unlock)(void)) {
    av_source_io_lock = lock;
    av_source_io_unlock = unlock;
}
