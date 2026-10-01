#include "av_source.h"

#include <stdio.h>
#include <string.h>

#include "libavutil/mem.h"
#include "ffmpeg_av.h"

#if AV_STREAM
#ifndef AV_STREAM_BUF
#define AV_STREAM_BUF (128 * 1024)
#endif

static int stream_read(void *opaque, uint8_t *buf, int size) {
    FILE *fp = opaque;
    size_t n = fread(buf, 1, (size_t)size, fp);

    if (n > 0)
        return (int)n;
    return ferror(fp) ? -1 : 0;
}

static int64_t stream_seek(void *opaque, int64_t offset, int whence) {
    FILE *fp = opaque;

    if (whence & AVSEEK_SIZE) {
        long cur = ftell(fp), size = -1;

        if (cur >= 0 && fseek(fp, 0, SEEK_END) == 0) {
            size = ftell(fp);
            fseek(fp, cur, SEEK_SET);
        }
        return size;
    }
    if (fseek(fp, (long)offset, whence) != 0)
        return -1;
    return ftell(fp);
}
#endif

#if !AV_STREAM
static uint8_t *load_file(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data;

    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = av_malloc((unsigned int)size + FF_INPUT_BUFFER_PADDING_SIZE);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
        av_free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    memset(data + size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
    *size_out = (size_t)size;
    return data;
}
#endif

int av_source_open(av_source_t *src, const char *path, int idct_algo) {
    static int registered;
    AVProbeData pd;
    AVInputFormat *fmt;
    AVCodec *vdec, *adec;
    unsigned i;

#if AV_STREAM
    uint8_t probe[4096];
#endif

    memset(src, 0, sizeof(*src));
    src->vidx = src->aidx = -1;
#if AV_STREAM
    {
        long size;

        src->fp = fopen(path, "rb");
        if (!src->fp)
            return AV_SOURCE_NO_FILE;
        if (fseek(src->fp, 0, SEEK_END) != 0 || (size = ftell(src->fp)) <= 0 ||
            fseek(src->fp, 0, SEEK_SET) != 0) {
            av_source_close(src);
            return AV_SOURCE_NO_FILE;
        }
        src->file_size = (size_t)size;
        src->iobuf = av_malloc(AV_STREAM_BUF);
        pd.buf_size = (int)fread(probe, 1, sizeof(probe), src->fp);
        if (!src->iobuf || pd.buf_size <= 0 || fseek(src->fp, 0, SEEK_SET) != 0) {
            av_source_close(src);
            return AV_SOURCE_NO_FILE;
        }
        pd.buf = probe;
    }
#else
    src->file_data = load_file(path, &src->file_size);
    if (!src->file_data)
        return AV_SOURCE_NO_FILE;
    pd.buf = src->file_data;
    pd.buf_size = src->file_size > 4096 ? 4096 : (int)src->file_size;
#endif

    if (!registered) {
        avcodec_init();
        ffmpeg_register_mpegps_av();
        registered = 1;
    }
#if AV_STREAM
    init_put_byte(&src->pb, src->iobuf, AV_STREAM_BUF, 0, src->fp, stream_read,
                  NULL, stream_seek);
#else
    init_put_byte(&src->pb, src->file_data, (int)src->file_size, 0, NULL, NULL,
                  NULL, NULL);
#endif
    pd.filename = path;
    fmt = av_probe_input_format(&pd, 1);
    if (!fmt || av_open_input_stream(&src->ic, &src->pb, path, fmt, NULL) < 0 ||
        av_find_stream_info(src->ic) < 0) {
        printf("av: demuxer open failed for %s\n", path);
        av_source_close(src);
        return -1;
    }
    for (i = 0; i < src->ic->nb_streams; i++) {
        AVCodecContext *c = src->ic->streams[i]->codec;

        if (c->codec_type == CODEC_TYPE_VIDEO && src->vidx < 0 &&
            c->codec_id == CODEC_ID_MPEG1VIDEO)
            src->vidx = (int)i;
        else if (c->codec_type == CODEC_TYPE_AUDIO && src->aidx < 0 &&
                 c->codec_id == CODEC_ID_MP2)
            src->aidx = (int)i;
    }
    if (src->vidx < 0 || src->aidx < 0) {
        printf("av: need MPEG-1 video and MP2 audio (video %d, audio %d)\n",
               src->vidx, src->aidx);
        av_source_close(src);
        return -1;
    }
    src->vc = src->ic->streams[src->vidx]->codec;
    src->ac = src->ic->streams[src->aidx]->codec;
    src->vc->idct_algo = idct_algo;
    vdec = avcodec_find_decoder(src->vc->codec_id);
    adec = avcodec_find_decoder(src->ac->codec_id);
    if (!vdec || !adec || avcodec_open(src->vc, vdec) < 0 ||
        avcodec_open(src->ac, adec) < 0) {
        printf("av: decoder open failed\n");
        /* contexts not opened must not be closed */
        src->vc = src->ac = NULL;
        av_source_close(src);
        return -1;
    }
    return 0;
}

void av_source_close(av_source_t *src) {
    if (src->vc)
        avcodec_close(src->vc);
    if (src->ac)
        avcodec_close(src->ac);
    if (src->ic)
        av_close_input_stream(src->ic);
    av_free(src->file_data);
#if AV_STREAM
    av_free(src->iobuf);
    if (src->fp)
        fclose((FILE *)src->fp);
#endif
    memset(src, 0, sizeof(*src));
    src->vidx = src->aidx = -1;
}
