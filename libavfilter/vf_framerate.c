/*
 * Copyright (C) 2012 Mark Himsley
 *
 * get_scene_score() Copyright (c) 2011 Stefano Sabatini
 * taken from libavfilter/vf_select.c
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
 * filter for upsampling or downsampling a progressive source
 */

#define DEBUG

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixelutils.h"

#include "avfilter.h"
#include "internal.h"
#include "video.h"

typedef struct FrameRateContext {
    const AVClass *class;
    // parameters
    AVRational dest_frame_rate;         ///< output frames per second
    int flags;                          ///< flags affecting frame rate conversion algorithm
    double scene_score;                 ///< score that denotes a scene change has happened
    int interp_start;                   ///< start of range to apply linear interpolation (same bitdepth as input)
    int interp_end;                     ///< end of range to apply linear interpolation (same bitdepth as input)
    int interp_start_param;             ///< start of range to apply linear interpolation
    int interp_end_param;               ///< end of range to apply linear interpolation

    int line_size[4];                   ///< bytes of pixel data per line for each plane
    int vsub;

    AVRational srce_time_base;          ///< timebase of source
    AVRational dest_time_base;          ///< timebase of destination

    av_pixelutils_sad_fn sad;           ///< Sum of the absolute difference function (scene detect only)
    double prev_mafd;                   ///< previous MAFD                           (scene detect only)

    int max;
    int bitdepth;
    AVFrame *work;

    AVFrame *f0;                        ///< last frame
    AVFrame *f1;                        ///< current frame
    int64_t pts0;                       ///< last frame pts in dest_time_base
    int64_t pts1;                       ///< current frame pts in dest_time_base
    int64_t delta;                      ///< pts1 to pts0 delta
    double score;                       ///< scene change score (f0 to f1)
    int flush;                          ///< 1 if the filter is being flushed
    int64_t start_pts;                  ///< pts of the first output frame
    int64_t n;                          ///< output frame counter
} FrameRateContext;

#define OFFSET(x) offsetof(FrameRateContext, x)
#define V AV_OPT_FLAG_VIDEO_PARAM
#define F AV_OPT_FLAG_FILTERING_PARAM
#define FRAMERATE_FLAG_SCD 01

static const AVOption framerate_options[] = {
    {"fps",                 "required output frames per second rate", OFFSET(dest_frame_rate), AV_OPT_TYPE_VIDEO_RATE, {.str="50"},             0,       INT_MAX, V|F },

    {"interp_start",        "point to start linear interpolation",    OFFSET(interp_start_param),AV_OPT_TYPE_INT,    {.i64=15},                 0,       255,     V|F },
    {"interp_end",          "point to end linear interpolation",      OFFSET(interp_end_param),  AV_OPT_TYPE_INT,    {.i64=240},                0,       255,     V|F },
    {"scene",               "scene change level",                     OFFSET(scene_score),     AV_OPT_TYPE_DOUBLE,   {.dbl=8.2},                0,       INT_MAX, V|F },

    {"flags",               "set flags",                              OFFSET(flags),           AV_OPT_TYPE_FLAGS,    {.i64=1},                  0,       INT_MAX, V|F, "flags" },
    {"scene_change_detect", "enable scene change detection",          0,                       AV_OPT_TYPE_CONST,    {.i64=FRAMERATE_FLAG_SCD}, INT_MIN, INT_MAX, V|F, "flags" },
    {"scd",                 "enable scene change detection",          0,                       AV_OPT_TYPE_CONST,    {.i64=FRAMERATE_FLAG_SCD}, INT_MIN, INT_MAX, V|F, "flags" },

    {NULL}
};

AVFILTER_DEFINE_CLASS(framerate);

static av_always_inline int64_t sad_8x8_16(const uint16_t *src1, ptrdiff_t stride1,
                                           const uint16_t *src2, ptrdiff_t stride2)
{
    int sum = 0;
    int x, y;

    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++)
            sum += FFABS(src1[x] - src2[x]);
        src1 += stride1;
        src2 += stride2;
    }
    return sum;
}

static int64_t scene_sad16(FrameRateContext *s, const uint16_t *p1, int p1_linesize, const uint16_t* p2, int p2_linesize, const int width, const int height)
{
    int64_t sad;
    int x, y;
    for (sad = y = 0; y < height - 7; y += 8) {
        for (x = 0; x < width - 7; x += 8) {
            sad += sad_8x8_16(p1 + y * p1_linesize + x,
                              p1_linesize,
                              p2 + y * p2_linesize + x,
                              p2_linesize);
        }
    }
    return sad;
}

static int64_t scene_sad8(FrameRateContext *s, uint8_t *p1, int p1_linesize, uint8_t* p2, int p2_linesize, const int width, const int height)
{
    int64_t sad;
    int x, y;
    for (sad = y = 0; y < height - 7; y += 8) {
        for (x = 0; x < width - 7; x += 8) {
            sad += s->sad(p1 + y * p1_linesize + x,
                          p1_linesize,
                          p2 + y * p2_linesize + x,
                          p2_linesize);
        }
    }
    emms_c();
    return sad;
}

static double get_scene_score(AVFilterContext *ctx, AVFrame *crnt, AVFrame *next)
{
    FrameRateContext *s = ctx->priv;
    double ret = 0;

    ff_dlog(ctx, "get_scene_score()\n");

    if (crnt->height == next->height &&
        crnt->width  == next->width) {
        int64_t sad;
        double mafd, diff;

        ff_dlog(ctx, "get_scene_score() process\n");
        if (s->bitdepth == 8)
            sad = scene_sad8(s, crnt->data[0], crnt->linesize[0], next->data[0], next->linesize[0], crnt->width, crnt->height);
        else
            sad = scene_sad16(s, (const uint16_t*)crnt->data[0], crnt->linesize[0] / 2, (const uint16_t*)next->data[0], next->linesize[0] / 2, crnt->width, crnt->height);

        mafd = (double)sad * 100.0 / FFMAX(1, (crnt->height & ~7) * (crnt->width & ~7)) / (1 << s->bitdepth);
        diff = fabs(mafd - s->prev_mafd);
        ret  = av_clipf(FFMIN(mafd, diff), 0, 100.0);
        s->prev_mafd = mafd;
    }
    ff_dlog(ctx, "get_scene_score() result is:%f\n", ret);
    return ret;
}

typedef struct ThreadData {
    AVFrame *copy_src1, *copy_src2;
    uint16_t src1_factor, src2_factor;
} ThreadData;

static int filter_slice8(AVFilterContext *ctx, void *arg, int job, int nb_jobs)
{
    FrameRateContext *s = ctx->priv;
    ThreadData *td = arg;
    uint16_t src1_factor = td->src1_factor;
    uint16_t src2_factor = td->src2_factor;
    int plane, line, pixel;

    for (plane = 0; plane < 4 && td->copy_src1->data[plane] && td->copy_src2->data[plane]; plane++) {
        int cpy_line_width = s->line_size[plane];
        uint8_t *cpy_src1_data = td->copy_src1->data[plane];
        int cpy_src1_line_size = td->copy_src1->linesize[plane];
        uint8_t *cpy_src2_data = td->copy_src2->data[plane];
        int cpy_src2_line_size = td->copy_src2->linesize[plane];
        int cpy_src_h = (plane > 0 && plane < 3) ? (td->copy_src1->height >> s->vsub) : (td->copy_src1->height);
        uint8_t *cpy_dst_data = s->work->data[plane];
        int cpy_dst_line_size = s->work->linesize[plane];
        const int start = (cpy_src_h *  job   ) / nb_jobs;
        const int end   = (cpy_src_h * (job+1)) / nb_jobs;
        cpy_src1_data += start * cpy_src1_line_size;
        cpy_src2_data += start * cpy_src2_line_size;
        cpy_dst_data += start * cpy_dst_line_size;

        if (plane <1 || plane >2) {
            // luma or alpha
            for (line = start; line < end; line++) {
                for (pixel = 0; pixel < cpy_line_width; pixel++) {
                    // integer version of (src1 * src1_factor) + (src2 + src2_factor) + 0.5
                    // 0.5 is for rounding
                    // 128 is the integer representation of 0.5 << 8
                    cpy_dst_data[pixel] = ((cpy_src1_data[pixel] * src1_factor) + (cpy_src2_data[pixel] * src2_factor) + 128) >> 8;
                }
                cpy_src1_data += cpy_src1_line_size;
                cpy_src2_data += cpy_src2_line_size;
                cpy_dst_data += cpy_dst_line_size;
            }
        } else {
            // chroma
            for (line = start; line < end; line++) {
                for (pixel = 0; pixel < cpy_line_width; pixel++) {
                    // as above
                    // because U and V are based around 128 we have to subtract 128 from the components.
                    // 32896 is the integer representation of 128.5 << 8
                    cpy_dst_data[pixel] = (((cpy_src1_data[pixel] - 128) * src1_factor) + ((cpy_src2_data[pixel] - 128) * src2_factor) + 32896) >> 8;
                }
                cpy_src1_data += cpy_src1_line_size;
                cpy_src2_data += cpy_src2_line_size;
                cpy_dst_data += cpy_dst_line_size;
            }
        }
    }

    return 0;
}

static int filter_slice16(AVFilterContext *ctx, void *arg, int job, int nb_jobs)
{
    FrameRateContext *s = ctx->priv;
    ThreadData *td = arg;
    uint16_t src1_factor = td->src1_factor;
    uint16_t src2_factor = td->src2_factor;
    const int half = s->max / 2;
    const int uv = (s->max + 1) * half;
    const int shift = s->bitdepth;
    int plane, line, pixel;

    for (plane = 0; plane < 4 && td->copy_src1->data[plane] && td->copy_src2->data[plane]; plane++) {
        int cpy_line_width = s->line_size[plane];
        const uint16_t *cpy_src1_data = (const uint16_t *)td->copy_src1->data[plane];
        int cpy_src1_line_size = td->copy_src1->linesize[plane] / 2;
        const uint16_t *cpy_src2_data = (const uint16_t *)td->copy_src2->data[plane];
        int cpy_src2_line_size = td->copy_src2->linesize[plane] / 2;
        int cpy_src_h = (plane > 0 && plane < 3) ? (td->copy_src1->height >> s->vsub) : (td->copy_src1->height);
        uint16_t *cpy_dst_data = (uint16_t *)s->work->data[plane];
        int cpy_dst_line_size = s->work->linesize[plane] / 2;
        const int start = (cpy_src_h *  job   ) / nb_jobs;
        const int end   = (cpy_src_h * (job+1)) / nb_jobs;
        cpy_src1_data += start * cpy_src1_line_size;
        cpy_src2_data += start * cpy_src2_line_size;
        cpy_dst_data += start * cpy_dst_line_size;

        if (plane <1 || plane >2) {
            // luma or alpha
            for (line = start; line < end; line++) {
                for (pixel = 0; pixel < cpy_line_width; pixel++)
                    cpy_dst_data[pixel] = ((cpy_src1_data[pixel] * src1_factor) + (cpy_src2_data[pixel] * src2_factor) + half) >> shift;
                cpy_src1_data += cpy_src1_line_size;
                cpy_src2_data += cpy_src2_line_size;
                cpy_dst_data += cpy_dst_line_size;
            }
        } else {
            // chroma
            for (line = start; line < end; line++) {
                for (pixel = 0; pixel < cpy_line_width; pixel++) {
                    cpy_dst_data[pixel] = (((cpy_src1_data[pixel] - half) * src1_factor) + ((cpy_src2_data[pixel] - half) * src2_factor) + uv) >> shift;
                }
                cpy_src1_data += cpy_src1_line_size;
                cpy_src2_data += cpy_src2_line_size;
                cpy_dst_data += cpy_dst_line_size;
            }
        }
    }

    return 0;
}

static int blend_frames(AVFilterContext *ctx, int interpolate)
{
    FrameRateContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    double interpolate_scene_score = 0;

    if ((s->flags & FRAMERATE_FLAG_SCD)) {
        if (s->score >= 0.0)
            interpolate_scene_score = s->score;
        else
            interpolate_scene_score = s->score = get_scene_score(ctx, s->f0, s->f1);
        ff_dlog(ctx, "blend_frames() interpolate scene score:%f\n", interpolate_scene_score);
    }
    // decide if the shot-change detection allows us to blend two frames
    if (interpolate_scene_score < s->scene_score) {
        ThreadData td;
        td.copy_src1 = s->f0;
        td.copy_src2 = s->f1;
        td.src2_factor = interpolate;
        td.src1_factor = s->max - td.src2_factor;

        // get work-space for output frame
        s->work = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!s->work)
            return AVERROR(ENOMEM);

        av_frame_copy_props(s->work, s->f0);

        ff_dlog(ctx, "blend_frames() INTERPOLATE to create work frame\n");
        ctx->internal->execute(ctx, s->bitdepth == 8 ? filter_slice8 : filter_slice16, &td, NULL, FFMIN(outlink->h, ff_filter_get_nb_threads(ctx)));
        return 1;
    }
    return 0;
}

static int process_work_frame(AVFilterContext *ctx)
{
    FrameRateContext *s = ctx->priv;
    int64_t work_pts;
    int interpolate;
    int ret;

    if (!s->f1)
        return 0;
    if (!s->f0 && !s->flush)
        return 0;

    work_pts = s->start_pts + av_rescale_q(s->n, av_inv_q(s->dest_frame_rate), s->dest_time_base);

    if (work_pts >= s->pts1 && !s->flush)
        return 0;

    if (!s->f0) {
        s->work = av_frame_clone(s->f1);
    } else {
        if (work_pts >= s->pts1 + s->delta && s->flush)
            return 0;

        interpolate = av_rescale(work_pts - s->pts0, s->max, s->delta);
        ff_dlog(ctx, "process_work_frame() interpolate:%d/%d\n", interpolate, s->max);
        if (interpolate > s->interp_end) {
            s->work = av_frame_clone(s->f1);
        } else if (interpolate < s->interp_start) {
            s->work = av_frame_clone(s->f0);
        } else {
            ret = blend_frames(ctx, interpolate);
            if (ret < 0)
                return ret;
            if (ret == 0)
                s->work = av_frame_clone(interpolate > (s->max >> 1) ? s->f1 : s->f0);
        }
    }

    if (!s->work)
        return AVERROR(ENOMEM);

    s->work->pts = work_pts;
    s->n++;

    return 1;
}

static av_cold int init(AVFilterContext *ctx)
{
    FrameRateContext *s = ctx->priv;
    s->start_pts = AV_NOPTS_VALUE;
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    FrameRateContext *s = ctx->priv;
    av_frame_free(&s->f0);
    av_frame_free(&s->f1);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUV411P, AV_PIX_FMT_YUVJ411P,
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUVJ422P,
        AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUVJ440P,
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV420P12,
        AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV422P12,
        AV_PIX_FMT_YUV444P9, AV_PIX_FMT_YUV444P10, AV_PIX_FMT_YUV444P12,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    FrameRateContext *s = ctx->priv;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(inlink->format);
    int plane;

    for (plane = 0; plane < 4; plane++) {
        s->line_size[plane] = av_image_get_linesize(inlink->format, inlink->w,
                                                    plane);
    }

    s->bitdepth = pix_desc->comp[0].depth;
    s->vsub = pix_desc->log2_chroma_h;
    s->interp_start = s->interp_start_param << (s->bitdepth - 8);
    s->interp_end = s->interp_end_param << (s->bitdepth - 8);

    s->sad = av_pixelutils_get_sad_fn(3, 3, 2, s); // 8x8 both sources aligned
    if (!s->sad)
        return AVERROR(EINVAL);

    s->srce_time_base = inlink->time_base;

    s->max = 1 << (s->bitdepth);

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *inpicref)
{
    int ret;
    AVFilterContext *ctx = inlink->dst;
    FrameRateContext *s = ctx->priv;
    int64_t pts;

    if (inpicref->interlaced_frame)
        av_log(ctx, AV_LOG_WARNING, "Interlaced frame found - the output will not be correct.\n");

    if (inpicref->pts == AV_NOPTS_VALUE) {
        av_log(ctx, AV_LOG_WARNING, "Ignoring frame without PTS.\n");
        return 0;
    }

    pts = av_rescale_q(inpicref->pts, s->srce_time_base, s->dest_time_base);
    if (s->f1 && pts == s->pts1) {
        av_log(ctx, AV_LOG_WARNING, "Ignoring frame with same PTS.\n");
        return 0;
    }

    av_frame_free(&s->f0);
    s->f0 = s->f1;
    s->pts0 = s->pts1;
    s->f1 = inpicref;
    s->pts1 = pts;
    s->delta = s->pts1 - s->pts0;
    s->score = -1.0;

    if (s->delta < 0) {
        av_log(ctx, AV_LOG_WARNING, "PTS discontinuity.\n");
        s->start_pts = s->pts1;
        s->n = 0;
        av_frame_free(&s->f0);
    }

    if (s->start_pts == AV_NOPTS_VALUE)
        s->start_pts = s->pts1;

    do {
        ret = process_work_frame(ctx);
        if (ret <= 0)
            return ret;
        ret = ff_filter_frame(ctx->outputs[0], s->work);
    } while (ret >= 0);

    return ret;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    FrameRateContext *s = ctx->priv;
    int exact;

    ff_dlog(ctx, "config_output()\n");

    ff_dlog(ctx,
           "config_output() input time base:%u/%u (%f)\n",
           ctx->inputs[0]->time_base.num,ctx->inputs[0]->time_base.den,
           av_q2d(ctx->inputs[0]->time_base));

    // make sure timebase is small enough to hold the framerate

    exact = av_reduce(&s->dest_time_base.num, &s->dest_time_base.den,
                      av_gcd((int64_t)s->srce_time_base.num * s->dest_frame_rate.num,
                             (int64_t)s->srce_time_base.den * s->dest_frame_rate.den ),
                      (int64_t)s->srce_time_base.den * s->dest_frame_rate.num, INT_MAX);

    av_log(ctx, AV_LOG_INFO,
           "time base:%u/%u -> %u/%u exact:%d\n",
           s->srce_time_base.num, s->srce_time_base.den,
           s->dest_time_base.num, s->dest_time_base.den, exact);
    if (!exact) {
        av_log(ctx, AV_LOG_WARNING, "Timebase conversion is not exact\n");
    }

    outlink->frame_rate = s->dest_frame_rate;
    outlink->time_base = s->dest_time_base;

    ff_dlog(ctx,
           "config_output() output time base:%u/%u (%f) w:%d h:%d\n",
           outlink->time_base.num, outlink->time_base.den,
           av_q2d(outlink->time_base),
           outlink->w, outlink->h);


    av_log(ctx, AV_LOG_INFO, "fps -> fps:%u/%u scene score:%f interpolate start:%d end:%d\n",
            s->dest_frame_rate.num, s->dest_frame_rate.den,
            s->scene_score, s->interp_start, s->interp_end);

    return 0;
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    FrameRateContext *s = ctx->priv;
    int ret;

    ff_dlog(ctx, "request_frame()\n");

    ret = ff_request_frame(ctx->inputs[0]);
    if (ret == AVERROR_EOF && s->f1 && !s->flush) {
        s->flush = 1;
        ret = process_work_frame(ctx);
        if (ret < 0)
            return ret;
        ret = ret ? ff_filter_frame(ctx->outputs[0], s->work) : AVERROR_EOF;
    }

    ff_dlog(ctx, "request_frame() source's request_frame() returned:%d\n", ret);
    return ret;
}

static const AVFilterPad framerate_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad framerate_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = request_frame,
        .config_props  = config_output,
    },
    { NULL }
};

AVFilter ff_vf_framerate = {
    .name          = "framerate",
    .description   = NULL_IF_CONFIG_SMALL("Upsamples or downsamples progressive source between specified frame rates."),
    .priv_size     = sizeof(FrameRateContext),
    .priv_class    = &framerate_class,
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = framerate_inputs,
    .outputs       = framerate_outputs,
    .flags         = AVFILTER_FLAG_SLICE_THREADS,
};
