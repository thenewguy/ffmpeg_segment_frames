/*
 * Generic segmenter
 * Copyright (c) 2011, Luca Barbato
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <float.h>

#include "avformat.h"
#include "internal.h"

#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "libavutil/parseutils.h"
#include "libavutil/mathematics.h"

typedef struct {
    const AVClass *class;  /**< Class for private options. */
    int number;
    AVFormatContext *avf;
    char *format;          /**< Set by a private option. */
    char *list;            /**< Set by a private option. */
    float time;            /**< Set by a private option. */
    int  size;             /**< Set by a private option. */
    int  wrap;             /**< Set by a private option. */
    int64_t offset_time;
    int64_t recording_time;
    int has_video;
    AVIOContext *pb;
    char *valid_frames_str;    /** delimited list of valid frames to start a new segment */
    int64_t *valid_frames; /** holds parsed valid_frames_str */
    int64_t nb_valid_frames; /** count of valid frames */
    int64_t next_valid_frame; /** the next frame number that is valid for starting a new segment */
    int64_t next_valid_frame_index; /** array index of the current next_valid_frame */
    int64_t frame_count; /** current video frame count */
} SegmentContext;

static int parse_valid_frames(void *log_ctx, int64_t **valid_frames, char *valid_frames_str, int64_t *nb_valid_frames)
{
	char *p;
	int64_t i;
	char *frame = NULL;
	
	i = 0;
	for (p = (char *) valid_frames_str; *p; p++)
		if (*p == ',')
			i++;
	*nb_valid_frames = (int64_t) (i+1);

	*valid_frames = (int64_t *) av_realloc_f(NULL, sizeof(**valid_frames), i);
	if (!*valid_frames) {
		av_log(log_ctx, AV_LOG_ERROR, "Could not allocate valid frames array.\n");
		return AVERROR(ENOMEM);
	}
	
	i = 0;
	frame = av_strtok(valid_frames_str, ",", &p);
	while (frame) {
		(*valid_frames)[i] = strtol(frame, NULL, 10);

		if (i && (*valid_frames)[i] <= (*valid_frames)[i-1]) {
			av_log(log_ctx, AV_LOG_ERROR,
					"Valid frames must be specified in ascending order without duplicate values.\n");
			return AVERROR(EINVAL);
		}

		frame = av_strtok(NULL, ",", &p);
		i++;
	}

	return 0;
}

static int segment_start(AVFormatContext *s)
{
    SegmentContext *seg = s->priv_data;
    AVFormatContext *oc = seg->avf;
    int err = 0;

    if (seg->wrap)
        seg->number %= seg->wrap;

    if (av_get_frame_filename(oc->filename, sizeof(oc->filename),
                              s->filename, seg->number++) < 0)
        return AVERROR(EINVAL);

    if ((err = avio_open2(&oc->pb, oc->filename, AVIO_FLAG_WRITE,
                          &s->interrupt_callback, NULL)) < 0)
        return err;

    if (!oc->priv_data && oc->oformat->priv_data_size > 0) {
        oc->priv_data = av_mallocz(oc->oformat->priv_data_size);
        if (!oc->priv_data) {
            avio_close(oc->pb);
            return AVERROR(ENOMEM);
        }
        if (oc->oformat->priv_class) {
            *(const AVClass**)oc->priv_data = oc->oformat->priv_class;
            av_opt_set_defaults(oc->priv_data);
        }
    }

    if ((err = oc->oformat->write_header(oc)) < 0) {
        goto fail;
    }

    return 0;

fail:
    av_log(oc, AV_LOG_ERROR, "Failure occurred when starting segment '%s'\n",
           oc->filename);
    avio_close(oc->pb);
    av_freep(&oc->priv_data);

    return err;
}

static int segment_end(AVFormatContext *oc)
{
    int ret = 0;

    if (oc->oformat->write_trailer)
        ret = oc->oformat->write_trailer(oc);

    if (ret < 0)
        av_log(oc, AV_LOG_ERROR, "Failure occurred when ending segment '%s'\n",
               oc->filename);

    avio_close(oc->pb);
    if (oc->oformat->priv_class)
        av_opt_free(oc->priv_data);
    av_freep(&oc->priv_data);

    return ret;
}

static int seg_write_header(AVFormatContext *s)
{
    SegmentContext *seg = s->priv_data;
    AVFormatContext *oc;
    int ret, i;

    seg->number = 0;
    seg->offset_time = 0;
    seg->recording_time = seg->time * 1000000;
    seg->nb_valid_frames = 0;
    seg->frame_count = 0;

    oc = avformat_alloc_context();

    if (!oc)
        return AVERROR(ENOMEM);

    if (seg->list)
        if ((ret = avio_open2(&seg->pb, seg->list, AVIO_FLAG_WRITE,
                              &s->interrupt_callback, NULL)) < 0)
            goto fail;
    
	if (seg->valid_frames_str) {
		if ((ret = parse_valid_frames(s, &seg->valid_frames, seg->valid_frames_str, &seg->nb_valid_frames)) < 0)
			return ret;

		if (seg->nb_valid_frames) {
			if (seg->valid_frames[0]) {
				seg->next_valid_frame_index = 0;
			} else {
				seg->next_valid_frame_index = 1;
			}
			seg->next_valid_frame = seg->valid_frames[seg->next_valid_frame_index];
		}
	}

    for (i = 0; i< s->nb_streams; i++)
        seg->has_video +=
            (s->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO);

    if (seg->has_video > 1)
        av_log(s, AV_LOG_WARNING,
               "More than a single video stream present, "
               "expect issues decoding it.\n");

    oc->oformat = av_guess_format(seg->format, s->filename, NULL);

    if (!oc->oformat) {
        ret = AVERROR_MUXER_NOT_FOUND;
        goto fail;
    }
    if (oc->oformat->flags & AVFMT_NOFILE) {
        av_log(s, AV_LOG_ERROR, "format %s not supported.\n",
               oc->oformat->name);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    seg->avf = oc;

    oc->streams = s->streams;
    oc->nb_streams = s->nb_streams;

    if (av_get_frame_filename(oc->filename, sizeof(oc->filename),
                              s->filename, seg->number++) < 0) {
        ret = AVERROR(EINVAL);
        goto fail;
    }

    if ((ret = avio_open2(&oc->pb, oc->filename, AVIO_FLAG_WRITE,
                          &s->interrupt_callback, NULL)) < 0)
        goto fail;

    if ((ret = avformat_write_header(oc, NULL)) < 0) {
        avio_close(oc->pb);
        goto fail;
    }

    if (seg->list) {
        avio_printf(seg->pb, "%s\n", oc->filename);
        avio_flush(seg->pb);
    }

fail:
    if (ret) {
        if (oc) {
            oc->streams = NULL;
            oc->nb_streams = 0;
            avformat_free_context(oc);
        }
        if (seg->list)
            avio_close(seg->pb);
    }
    return ret;
}

static int seg_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    SegmentContext *seg = s->priv_data;
    AVFormatContext *oc = seg->avf;
    AVStream *st = oc->streams[pkt->stream_index];
    int64_t end_pts = seg->recording_time * seg->number;
    int ret;
    int can_split = (
			seg->has_video
			&& st->codec->codec_type == AVMEDIA_TYPE_VIDEO
			&& pkt->flags & AV_PKT_FLAG_KEY
    );

    if (av_compare_ts(pkt->pts, st->time_base, end_pts, AV_TIME_BASE_Q) < 0)
    	can_split = 0;

    if (seg->nb_valid_frames) {
    	if (seg->next_valid_frame < seg->frame_count && (seg->next_valid_frame_index + 1) < seg->nb_valid_frames) {
    		seg->next_valid_frame_index++;
    		seg->next_valid_frame = seg->valid_frames[seg->next_valid_frame_index];
    	}
    	if (seg->next_valid_frame != seg->frame_count)
			can_split = 0;
    }

    if (can_split) {

        av_log(s, AV_LOG_DEBUG, "Next segment starts at %d %"PRId64" with frame count of %"PRId64" \n",
                       pkt->stream_index, pkt->pts, seg->frame_count);

        ret = segment_end(oc);

        if (!ret)
            ret = segment_start(s);

        if (ret)
            goto fail;

        if (seg->list) {
            avio_printf(seg->pb, "%s\n", oc->filename);
            avio_flush(seg->pb);
            if (seg->size && !(seg->number % seg->size)) {
                avio_close(seg->pb);
                if ((ret = avio_open2(&seg->pb, seg->list, AVIO_FLAG_WRITE,
                                      &s->interrupt_callback, NULL)) < 0)
                    goto fail;
            }
        }
    }

    if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
		seg->frame_count++;

    ret = oc->oformat->write_packet(oc, pkt);

fail:
    if (ret < 0) {
        oc->streams = NULL;
        oc->nb_streams = 0;
        if (seg->list)
            avio_close(seg->pb);
        avformat_free_context(oc);
    }

    return ret;
}

static int seg_write_trailer(struct AVFormatContext *s)
{
    SegmentContext *seg = s->priv_data;
    AVFormatContext *oc = seg->avf;
    int ret = segment_end(oc);
    if (seg->list)
        avio_close(seg->pb);
    oc->streams = NULL;
    oc->nb_streams = 0;
    avformat_free_context(oc);
    return ret;
}

#define OFFSET(x) offsetof(SegmentContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "segment_format",    "container format used for the segments",  OFFSET(format),  AV_OPT_TYPE_STRING, {.str = NULL},  0, 0,       E },
    { "segment_time",      "segment length in seconds",               OFFSET(time),    AV_OPT_TYPE_FLOAT,  {.dbl = 2},     0, FLT_MAX, E },
    { "segment_list",      "output the segment list",                 OFFSET(list),    AV_OPT_TYPE_STRING, {.str = NULL},  0, 0,       E },
    { "segment_list_size", "maximum number of playlist entries",      OFFSET(size),    AV_OPT_TYPE_INT,    {.dbl = 5},     0, INT_MAX, E },
    { "segment_wrap",      "number after which the index wraps",      OFFSET(wrap),    AV_OPT_TYPE_INT,    {.dbl = 0},     0, INT_MAX, E },
    { "segment_valid_frames",     "set valid segment split frames",        OFFSET(valid_frames_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0,      E },
    { NULL },
};

static const AVClass seg_class = {
    .class_name = "segment muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVOutputFormat ff_segment_muxer = {
    .name           = "segment",
    .long_name      = NULL_IF_CONFIG_SMALL("segment muxer"),
    .priv_data_size = sizeof(SegmentContext),
    .flags          = AVFMT_GLOBALHEADER | AVFMT_NOFILE,
    .write_header   = seg_write_header,
    .write_packet   = seg_write_packet,
    .write_trailer  = seg_write_trailer,
    .priv_class     = &seg_class,
};

static const AVClass sseg_class = {
    .class_name = "stream_segment muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVOutputFormat ff_stream_segment_muxer = {
    .name           = "stream_segment,ssegment",
    .long_name      = NULL_IF_CONFIG_SMALL("streaming segment muxer"),
    .priv_data_size = sizeof(SegmentContext),
    .flags          = AVFMT_NOFILE,
    .write_header   = seg_write_header,
    .write_packet   = seg_write_packet,
    .write_trailer  = seg_write_trailer,
    .priv_class     = &sseg_class,
};
