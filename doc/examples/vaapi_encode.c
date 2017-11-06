/*
 * Video Acceleration API (video encoding) encode sample
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Intel VAAPI-accelerated encoding example.
 *
 * @example vaapi_encode.c
 * This example shows how to do VAAPI-accelerated encoding. now only support NV12
 * raw file, usage like: vaapi_enc 1920 1080 input.yuv output.h264
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>

typedef struct FilterContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph   *filter_graph;
} FilterContext;

static int width, height;
static AVBufferRef *hw_device_ctx = NULL;

static int init_filter(FilterContext *filter_ctx, char *args, AVBufferRef *hw_device_ctx)
{
    char filter_spec[] = "format=nv12,hwupload";
    int  ret = 0, i = 0;
    const AVFilter *buffersrc, *buffersink;
    AVFilterContext *buffersrc_ctx, *buffersink_ctx;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterGraph *filter_graph = avfilter_graph_alloc();

    buffersrc = avfilter_get_by_name("buffer");
    buffersink = avfilter_get_by_name("buffersink");
    if (!buffersrc || !buffersink) {
        av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source. Error code:%s\n", av_err2str(ret));
        goto fail;
    }
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       NULL, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink. Error code:%s\n", av_err2str(ret));
        goto fail;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;
    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = buffersink_ctx;
    inputs->pad_idx     = 0;
    inputs->next        = NULL;
    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_spec,
                                        &inputs, &outputs, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error code: %s\n", av_err2str(ret));
        goto fail;
    }

    if (hw_device_ctx) {
        for (i = 0; i < filter_graph->nb_filters; i++) {
            filter_graph->filters[i]->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        }
    }

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Fail to config filter graph. Error code: %s\n", av_err2str(ret));
        goto fail;
    }

    filter_ctx->buffersrc_ctx  = buffersrc_ctx;
    filter_ctx->buffersink_ctx = buffersink_ctx;
    filter_ctx->filter_graph   = filter_graph;

fail:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}

static int encode_write(AVCodecContext *avctx, AVFrame *frame, FILE *fout)
{
    int ret = 0;
    AVPacket enc_pkt;

    av_init_packet(&enc_pkt);
    enc_pkt.data = NULL;
    enc_pkt.size = 0;

    if ((ret = avcodec_send_frame(avctx, frame)) < 0) {
        fprintf(stderr, "Error code: %s\n", av_err2str(ret));
        goto end;
    }
    while (1) {
        ret = avcodec_receive_packet(avctx, &enc_pkt);
        if (ret)
            break;

        enc_pkt.stream_index = 0;
        ret = fwrite(enc_pkt.data, enc_pkt.size, 1, fout);
    }

end:
    ret = ((ret == AVERROR(EAGAIN)) ? 0 : -1);
    return ret;
}

int main(int argc, char *argv[])
{
    int ret, size;
    FILE *fin = NULL, *fout = NULL;
    AVFrame *sw_frame = NULL, *hw_frame = NULL;
    AVCodecContext *avctx = NULL;
    FilterContext *filter_ctx = NULL;
    AVCodec *codec = NULL;
    const char *enc_name = "h264_vaapi";
    char args[512];

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <width> <height> <input file> <output file>\n", argv[0]);
        return -1;
    }

    width  = atoi(argv[1]);
    height = atoi(argv[2]);
    size   = width * height;

    if (!(fin = fopen(argv[3], "r"))) {
        fprintf(stderr, "Fail to open input file : %s\n", strerror(errno));
        ret = -1;
        goto close;
    }
    if (!(fout = fopen(argv[4], "w+b"))) {
        fprintf(stderr, "Fail to open output file : %s\n", strerror(errno));
        ret = -1;
        goto close;
    }

    av_register_all();
    avfilter_register_all();

    ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                 NULL, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to create a VAAPI device. Error code: %s\n", av_err2str(ret));
        goto close;
    }

    if (!(codec = avcodec_find_encoder_by_name(enc_name))) {
        fprintf(stderr, "Could not find encoder.\n");
        ret = -1;
        goto close;
    }

    if (!(avctx = avcodec_alloc_context3(codec))) {
        ret = AVERROR(ENOMEM);
        goto close;
    }

    avctx->width     = width;
    avctx->height    = height;
    avctx->time_base = (AVRational){1, 25};
    avctx->framerate = (AVRational){25, 1};
    avctx->sample_aspect_ratio = (AVRational){1, 1};
    avctx->pix_fmt   = AV_PIX_FMT_VAAPI;

    /* create filters and binding HWDevice */
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d",
             avctx->width, avctx->height, AV_PIX_FMT_NV12,
             avctx->time_base.num, avctx->time_base.den,
             avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den,
             avctx->framerate.num, avctx->framerate.den);

    if (!(filter_ctx = av_malloc(sizeof(*filter_ctx)))) {
        ret = AVERROR(ENOMEM);
        goto close;
    }

    if ((ret = init_filter(filter_ctx, args, hw_device_ctx)) < 0) {
        fprintf(stderr, "Failed to initialize the filtering context.\n");
        goto close;
    }

    avctx->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx
                                         (filter_ctx->buffersink_ctx));
    if (!avctx->hw_frames_ctx) {
        ret = AVERROR(ENOMEM);
        goto close;
    }

    if ((ret = avcodec_open2(avctx, codec, NULL)) < 0) {
        fprintf(stderr, "Cannot open video encoder codec. Error code: %s\n", av_err2str(ret));
        goto close;
    }

    while (1) {
        if (!(sw_frame = av_frame_alloc())) {
            ret = AVERROR(ENOMEM);
            goto close;
        }
        sw_frame->width  = width;
        sw_frame->height = height;
        sw_frame->format = AV_PIX_FMT_NV12;
        if ((ret = av_frame_get_buffer(sw_frame, 32)) < 0)
            goto close;
        if ((ret = fread((uint8_t*)(sw_frame->data[0]), size, 1, fin)) <= 0)
            break;
        if ((ret = fread((uint8_t*)(sw_frame->data[1]), size/2, 1, fin)) <= 0)
            break;
        /* push the sw frame into the filtergraph */
        ret = av_buffersrc_add_frame_flags(filter_ctx->buffersrc_ctx, sw_frame, 0);
        if (ret < 0) {
            fprintf(stderr, "Error while feeding the filtergraph. Error code: %s\n", av_err2str(ret));
            goto close;
        }
        /* pull hw frames from the filtergraph */
        while (1) {
            if (!(hw_frame = av_frame_alloc())) {
                ret = AVERROR(ENOMEM);
                goto close;
            }
            if ((ret = (av_buffersink_get_frame(filter_ctx->buffersink_ctx, hw_frame))) < 0) {
                /* if no more frames for output - returns AVERROR(EAGAIN)
                 * if flushed and no more frames for output - returns AVERROR_EOF
                 * rewrite retcode to 0 to show it as normal procedure completion
                 */
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    ret = 0;
                av_frame_free(&hw_frame);
                break;
            }

            if ((ret = (encode_write(avctx, hw_frame, fout))) < 0) {
                fprintf(stderr, "Failed to encode.\n");
                goto close;
            }
            av_frame_free(&hw_frame);
        }
        av_frame_free(&sw_frame);
    }

    /* flush encode */
    ret = encode_write(avctx, NULL, fout);

close:
    if (fin)
        fclose(fin);
    if (fout)
        fclose(fout);
    if (sw_frame)
        av_frame_free(&sw_frame);
    if (hw_frame)
        av_frame_free(&hw_frame);
    if (avctx)
        avcodec_free_context(&avctx);
    if (filter_ctx) {
        avfilter_free(filter_ctx->buffersrc_ctx);
        avfilter_free(filter_ctx->buffersink_ctx);
        avfilter_graph_free(&(filter_ctx->filter_graph));
        av_free(filter_ctx);
    }
    if (hw_device_ctx)
        av_buffer_unref(&hw_device_ctx);

    return ret;
}
