/*
 * Extended segmenter
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

#if defined(_WIN32) || defined(_WIN64)
    #define OS_WINDOWS
    #include <windows.h>
#else
    #define _LARGEFILE64_SOURCE
    #include <sys/statvfs.h>
    #include <sys/stat.h>
    #include <glob.h>
    #include <pthread.h>
#endif

#include <strings.h>
#include <float.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "avformat.h"
#include "internal.h"

#include "libavutil/atomic.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "libavutil/parseutils.h"
#include "libavutil/mathematics.h"

#define PTS_MILLIS_NONE                          (-1)
#define MAXIMUM_IGNORED_ERROR_COUNT              300
#define MINIMUM_VIDEO_FRAMES_INTERVAL_MILLIS     (1000/50)
#define MAX_AVG_ARRAY_SIZE                       30
#define MAX_PACKET_INTERVAL_LIMIT_MILLIS         (60 * 1000)
#define DEFAULT_MAXIMUM_PACKET_BUFFER_SIZE_BYTES 0
#define STREAM_COUNT_LIMIT                       4
#define FILE_FORMAT                              "matroska"
#define FILE_NAME_TIME_FORMAT                    "/%Y-%m-%d_%H-%M-%S.mkv"
#define FILE_NAME_MASK                           "/*-*-*_*-*-*.mkv"
#define DEFAULT_FILE_DURATION_MILLIS             30000
#define DEFAULT_RESERVED_DISK_SPACE_MB           50 /* MegaBytes */
#define MAX_OLD_FILES_TO_REMOVE                  15
#define MAX_THREAD_STACK_SIZE                    (512 * 1024)


typedef struct
{
    int array[MAX_AVG_ARRAY_SIZE];
    int index;
    int size;
} AvgArray;


typedef struct {
    AVClass         *class; /* Class for private options. */
    AVFormatContext *avf;
    AVOutputFormat  *oformat;
    char            *control_file_path; /* Set by a private option. */
    int             control_file_id;
#ifndef OS_WINDOWS
    struct flock    control_file_lock;
#endif
    int             need_create_new_file;
    int             is_file_writing_stopped;  /* Set by a private option. */
    int             has_video;
    int64_t         max_file_duration_millis; /* Set by a private option. */
    int64_t         first_packet_millis_array[STREAM_COUNT_LIMIT]; /* PTS for first packet in current segment for each stream */
    int64_t         last_packet_millis_array[STREAM_COUNT_LIMIT];  /* PTS for last packet in current segment for each stream */
    int64_t         first_packet_pts_array[STREAM_COUNT_LIMIT]; /* PTS for each stream(audio/video/other) */
    int64_t         prev_video_packet_millis;
    int64_t         file_writing_stopped_packet_millis;
    AvgArray        avg_frames_interval;
    int             error_count;
    AVPacketList    *packet_buffer_start; /* Used for buffering packets between key frames if file write is stopped */
    AVPacketList    *packet_buffer_end;
    int             packet_buffer_size;  /* Current buffer size in bytes */
    int             maximum_packet_buffer_size; /* Set by a private option. */
    int64_t         prerecord_time;  /* Set by a private option. */
    int64_t         postrecord_time; /* Set by a private option. */
    int64_t         reserved_disk_space_mb; /* Megabytes. Set by a private option. */
    char            segment_filemask[256];
    char            segment_dirpath[256];
    int             process_only_keyframes; /* Set by a private option. */

#ifdef OS_WINDOWS
    char            segment_filepath_buffer[256];
    char            segment_filename_buffer[256];
#else
    pthread_t       free_space_thread;
    int             need_check_free_space;
    int             need_stop_free_space_thread;
    glob_t          glob_info;
    int             glob_position;
#endif
} SegmentContext;


static void clear_avg_array(AvgArray *arr)
{
    arr->index = -1;
    arr->size  = 0;
}

static void add_to_avg_array(AvgArray *arr, int val)
{
    if (arr->size < MAX_AVG_ARRAY_SIZE)
    {
        arr->size++;
    }
    if (arr->index + 1 < MAX_AVG_ARRAY_SIZE)
    {
        arr->index++;
    }
    else
    {
        arr->index = 0;
    }
    arr->array[arr->index] = val;
}


static int get_avg_value(AvgArray *arr)
{
    int ret = 0;
    if (arr->size > 0)
    {
        int acc = 0;
        for (int i = 0; i < arr->size; ++i)
        {
            acc += arr->array[i];
        }
        ret = acc / arr->size;
    }
    return ret;
}


static void fill_array(int64_t *arr, size_t size, int64_t value)
{
    for (size_t i = 0; i < size; ++i)
    {
        arr[i] = value;
    }
}

static void set_new_filename(AVFormatContext *oc, const char *filename_macro, int tag)
{
    time_t now;
    struct tm tm;

    time(&now);
    if (tag == 'L')
    {
        tm = *localtime(&now);
    }
    else
    {
        tm = *gmtime(&now);
    }

    strftime(oc->filename, sizeof(oc->filename), filename_macro, &tm);
}


static int64_t get_pkt_timestamp_milliseconds(AVFormatContext *s, AVPacket *pkt)
{
    AVStream *st = s->streams[pkt->stream_index];
    return (int64_t)((double) pkt->pts * av_q2d(st->time_base) * 1000.0);
}


/* Return interval in milliseconds between 'packet_buffer' start and 'pkt' timestamps */
static int64_t get_pkt_millis_from_buffer_start(AVFormatContext *s, AVPacketList *packet_buffer, AVPacket *pkt)
{
    int64_t pkt_delta_millis = 0;

    AVPacketList *current_pkt_list = packet_buffer;
    while (current_pkt_list)
    {
        AVPacket *buffered_pkt = &current_pkt_list->pkt;
        if (buffered_pkt->stream_index == pkt->stream_index) /* We must compare packets only from one stream */
        {
            int64_t pkt_millis = get_pkt_timestamp_milliseconds(s, pkt);
            int64_t buffered_pkt_millis = get_pkt_timestamp_milliseconds(s, buffered_pkt);
            if (pkt_millis > buffered_pkt_millis)
            {
                pkt_delta_millis = pkt_millis - buffered_pkt_millis;
            }
            break;
        }
        current_pkt_list = current_pkt_list->next;
    }

    return pkt_delta_millis;
}


static void free_context(SegmentContext *seg)
{
    if (seg->avf)
    {
        if (seg->avf->pb)
        {
            avio_close(seg->avf->pb);
        }
        avformat_free_context(seg->avf);
        seg->avf = NULL;
    }
}


static int is_segment_started(SegmentContext *seg)
{
    /* If segment was previously started 'seg->avf' must be not NULL */
    return seg->avf != NULL ? 1 : 0;
}


static int segment_end(AVFormatContext *oc, SegmentContext *seg)
{
    int ret = 0;

    if (!is_segment_started(seg))
    {
        /* If segment not started - nothing to close */
        return ret;
    }

    av_write_frame(oc, NULL); /* Flush any buffered data (fragmented mp4) */

    if (oc->oformat->write_trailer)
    {
        ret = av_write_trailer(oc);
    }

    free_context(seg);

    return ret;
}


static void pop_first_packet_from_buffer(SegmentContext *seg)
{
    AVPacketList *pkt_list = seg->packet_buffer_start;
    seg->packet_buffer_start = pkt_list->next;
    seg->packet_buffer_size -= pkt_list->pkt.size;
    av_packet_unref(&(pkt_list->pkt));
    av_free(pkt_list);
}


static void clear_packet_buffer(SegmentContext *seg)
{
    while (seg->packet_buffer_start)
    {
        pop_first_packet_from_buffer(seg);
    }

    seg->packet_buffer_start = NULL;
    seg->packet_buffer_end   = NULL;
    seg->packet_buffer_size  = 0;
}


static int is_packet_contain_video(AVFormatContext *s, AVPacket *pkt)
{
    AVStream *st = s->streams[pkt->stream_index];
    SegmentContext *seg = s->priv_data;

    return seg->has_video && st->codec->codec_type == AVMEDIA_TYPE_VIDEO;
}


static int is_packet_contain_video_key_frame(AVFormatContext *s, AVPacket *pkt)
{
    return is_packet_contain_video(s, pkt) && (pkt->flags & AV_PKT_FLAG_KEY);
}


static int shrink_packet_buffer(AVFormatContext *s)
{
    SegmentContext *seg = s->priv_data;

    AVPacketList *new_buffer_start = NULL;
    AVPacketList *cur_packet = seg->packet_buffer_start;
    int64_t expected_millis = 0;
    int expected_bytes = seg->packet_buffer_size;
    int prev_packet_size = 0;
    int shrink_success = 0;

    do
    {
        if (is_packet_contain_video_key_frame(s, &cur_packet->pkt))
        {
            new_buffer_start = cur_packet;
        }

        expected_millis = get_pkt_millis_from_buffer_start(s, cur_packet, &seg->packet_buffer_end->pkt);
        expected_bytes -= prev_packet_size;
        prev_packet_size = cur_packet->pkt.size;
        cur_packet = cur_packet->next;

    } while (cur_packet && (expected_millis > seg->prerecord_time || expected_bytes > seg->maximum_packet_buffer_size));

    shrink_success = seg->packet_buffer_start != new_buffer_start;

    while (seg->packet_buffer_start != new_buffer_start)
    {
        pop_first_packet_from_buffer(seg);
    }

    return shrink_success;
}


static int get_disk_free_space_mb(SegmentContext *seg)
{
#ifdef OS_WINDOWS
    ULARGE_INTEGER free_bytes;

    return GetDiskFreeSpaceExA(seg->segment_dirpath, &free_bytes, NULL, NULL) == TRUE
        ? (int) (free_bytes.QuadPart / 1024 / 1024)
        : 0;
#else
    struct statvfs64 stat_buf;

    return statvfs64(seg->segment_dirpath, &stat_buf) == 0
        ? (int) ((uint64_t)stat_buf.f_bsize * (uint64_t)stat_buf.f_bavail / 1024 / 1024)
        : 0;
#endif
}


static void free_glob_info(SegmentContext *seg)
{
#ifndef OS_WINDOWS
    if (seg->glob_position != -1)
    {
        globfree(&seg->glob_info);
        seg->glob_position = -1;

        av_log(NULL, AV_LOG_INFO, "extsegment free stored glob info\n");
    }
#endif
}


#ifdef OS_WINDOWS

static int remove_oldest_segment_file(SegmentContext *seg)
{
    int is_file_removed = 0;

    WIN32_FIND_DATA data = {0};
    FILETIME oldest_time = {0};
    HANDLE handle = FindFirstFileA(seg->segment_filemask, &data);

    int has_next = handle != INVALID_HANDLE_VALUE;
    while (has_next)
    {
        if ((oldest_time.dwLowDateTime == 0 && oldest_time.dwHighDateTime == 0) ||
            CompareFileTime(&oldest_time, &data.ftLastWriteTime) > 0)
        {
            oldest_time = data.ftLastWriteTime;
            av_strlcpy(seg->segment_filename_buffer, data.cFileName, sizeof(seg->segment_filename_buffer));
        }
        has_next = FindNextFileA(handle, &data) == TRUE;
    }
    FindClose(handle);

    if (oldest_time.dwLowDateTime != 0 && oldest_time.dwHighDateTime != 0)
    {
        av_strlcpy(seg->segment_filepath_buffer, seg->segment_dirpath, sizeof(seg->segment_filepath_buffer));
        av_strlcat(seg->segment_filepath_buffer, seg->segment_filename_buffer, sizeof(seg->segment_filepath_buffer));
        is_file_removed = DeleteFileA(seg->segment_filepath_buffer);

        av_log(NULL, AV_LOG_INFO, "extsegment remove file: %s\n", seg->segment_filepath_buffer);
    }

    return is_file_removed;
}


static void remove_old_segments_by_reserved_space(SegmentContext *seg)
{
    if (seg->reserved_disk_space_mb > 0)
    {
        int removed_files = 0;

        while (get_disk_free_space_mb(seg) < seg->reserved_disk_space_mb && removed_files++ < MAX_OLD_FILES_TO_REMOVE)
        {
            av_log(NULL, AV_LOG_INFO, "extsegment not enough disk free space\n");

            if (!remove_oldest_segment_file(seg))
            {
               break;
            }
        }
    }
}

#else

static void remove_old_segments_by_reserved_space(SegmentContext *seg)
{
    if (seg->reserved_disk_space_mb > 0 &&
        get_disk_free_space_mb(seg) < seg->reserved_disk_space_mb)
    {
        av_log(NULL, AV_LOG_INFO, "extsegment not enough disk free space\n");

        if (seg->glob_position == -1 ||
            seg->glob_position >= seg->glob_info.gl_pathc)
        {
            free_glob_info(seg);

            /* Store glob() result in internal variable 'seg->glob_info' to speedup next calls to 'remove_old_segments_by_reserved_space' */
            if (glob(seg->segment_filemask, 0, NULL, &seg->glob_info) == 0) /* Must return sorted file array */
            {
                seg->glob_position = 0;
                av_log(NULL, AV_LOG_INFO, "extsegment perform new glob\n");
            }
            else
            {
                av_log(NULL, AV_LOG_ERROR, "extsegment glob error\n");
            }
        }
        else
        {
            av_log(NULL, AV_LOG_INFO, "extsegment reuse stored glob result: %i from %i\n", (int)seg->glob_position, (int)seg->glob_info.gl_pathc);
        }

        if (seg->glob_position != -1)
        {
            int max_files_pos = FFMIN(seg->glob_position + MAX_OLD_FILES_TO_REMOVE, seg->glob_info.gl_pathc);
            for (int i = seg->glob_position; i < max_files_pos && get_disk_free_space_mb(seg) < seg->reserved_disk_space_mb; ++i)
            {
                remove(seg->glob_info.gl_pathv[i]);
                seg->glob_position = i + 1;
                av_log(NULL, AV_LOG_INFO, "extsegment remove file: %s\n", seg->glob_info.gl_pathv[i]);
            }
        }
        else
        {
            av_log(NULL, AV_LOG_INFO, "extsegment can't free disk space: undefined glob_position\n");
        }
    }
}

#endif


static void* free_space_thread(void *param)
{
#ifndef OS_WINDOWS
    SegmentContext *seg = (SegmentContext*)param;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    while (avpriv_atomic_int_get(&seg->need_stop_free_space_thread) == 0)
    {
        if (__sync_bool_compare_and_swap(&seg->need_check_free_space, 1, 0))
        {
            remove_old_segments_by_reserved_space(seg);
        }

        sleep(1);
    }

    av_log(NULL, AV_LOG_INFO, "extsegment free space thread exited\n");
#endif

    return NULL;
}


static void start_free_space_thread(SegmentContext *seg)
{
#ifndef OS_WINDOWS
    seg->need_check_free_space = 0;
    seg->need_stop_free_space_thread = 0;
    seg->free_space_thread = 0;

    pthread_attr_t attribute;

    if (pthread_attr_init(&attribute) == 0)
    {
        size_t cur_stack_size = 0;
        if (pthread_attr_getstacksize(&attribute, &cur_stack_size) == 0 &&
            cur_stack_size > MAX_THREAD_STACK_SIZE)
        {
            pthread_attr_setstacksize(&attribute, MAX_THREAD_STACK_SIZE);
            av_log(NULL, AV_LOG_INFO, "extsegment reduce free space thread stack size from: %i to %i bytes\n", (int)cur_stack_size, MAX_THREAD_STACK_SIZE);
        }
    }

    if (pthread_create(&seg->free_space_thread, &attribute, free_space_thread, (void *)seg) != 0 ||
        seg->free_space_thread == 0)
    {
        av_log(NULL, AV_LOG_ERROR, "extsegment can't start free space thread\n");
    }

    pthread_attr_destroy(&attribute);
#endif
}


static void stop_free_space_thread(SegmentContext *seg)
{
#ifndef OS_WINDOWS
    if (seg->free_space_thread != 0)
    {
        avpriv_atomic_int_set(&seg->need_stop_free_space_thread, 1);
        pthread_join(seg->free_space_thread, NULL);

        seg->free_space_thread = 0;
    }
#endif
}


static void check_free_space(SegmentContext *seg)
{
#ifndef OS_WINDOWS
    if (seg->free_space_thread != 0)
    {
        avpriv_atomic_int_set(&seg->need_check_free_space, 1);
    }
    else
#endif
    {
        remove_old_segments_by_reserved_space(seg);
    }
}


static int segment_start(AVFormatContext *s, AVPacket *pkt)
{
    SegmentContext *seg = s->priv_data;
    AVFormatContext *oc = seg->avf;

    seg->need_create_new_file = 0;
    int err = segment_end(oc, seg); /* Segment closing will be performed only if required */
    if (err != 0)
    {
        goto fail;
    }

    check_free_space(seg);

    /* Clear timestamps arrays */
    fill_array(seg->first_packet_millis_array, STREAM_COUNT_LIMIT, PTS_MILLIS_NONE);
    fill_array(seg->last_packet_millis_array, STREAM_COUNT_LIMIT, PTS_MILLIS_NONE);
    /* Clear first pts array. */
    fill_array(seg->first_packet_pts_array, STREAM_COUNT_LIMIT, AV_NOPTS_VALUE);
    seg->prev_video_packet_millis = PTS_MILLIS_NONE;
    seg->error_count = 0;
    clear_avg_array(&seg->avg_frames_interval);

    /* Init context for new file */
    oc = seg->avf = avformat_alloc_context();
    if (!oc)
    {
        return AVERROR(ENOMEM);
    }
    oc->oformat            = seg->oformat;
    oc->interrupt_callback = s->interrupt_callback;
    av_dict_copy(&oc->metadata, s->metadata, 0);
    for (int i = 0; i < s->nb_streams; i++)
    {
        AVStream *st;
        AVCodecContext *icodec, *ocodec;

        if (!(st = avformat_new_stream(oc, NULL)))
        {
            err = AVERROR(ENOMEM);
            goto fail;
        }
        icodec = s->streams[i]->codec;
        ocodec = st->codec;
        avcodec_copy_context(ocodec, icodec);
        if (!oc->oformat->codec_tag ||
            av_codec_get_id (oc->oformat->codec_tag, icodec->codec_tag) == ocodec->codec_id ||
            av_codec_get_tag(oc->oformat->codec_tag, icodec->codec_id) <= 0)
        {
            ocodec->codec_tag = icodec->codec_tag;
        }
        else
        {
            ocodec->codec_tag = 0;
        }
        st->sample_aspect_ratio = s->streams[i]->sample_aspect_ratio;
    }

    set_new_filename(oc, s->filename, 'L'); /* s->filename contain full path with date-time mask. */
    av_log(s, AV_LOG_INFO, "new file name: %s\n", oc->filename);

    if ((err = avio_open2(&oc->pb, oc->filename, AVIO_FLAG_WRITE, &s->interrupt_callback, NULL)) < 0)
    {
        goto fail;
    }

    if (oc->oformat->priv_class && oc->priv_data)
    {
        av_opt_set(oc->priv_data, "resend_headers", "1", 0); /* mpegts specific */
    }

    if ((err = avformat_write_header(oc, NULL)) < 0)
    {
        goto fail;
    }

    return 0;

fail:
    clear_packet_buffer(seg);
    free_context(seg);
    return err;
}


static int seg_write_header(AVFormatContext *s)
{
    SegmentContext *seg = s->priv_data;
    int ret = 0;

    seg->error_count = 0;
    seg->avf         = NULL;
    seg->oformat     = NULL;
    seg->packet_buffer_start = NULL;
    seg->packet_buffer_end   = NULL;
    seg->packet_buffer_size  = 0;
    seg->need_create_new_file = 1;
    seg->file_writing_stopped_packet_millis = PTS_MILLIS_NONE;

#ifndef OS_WINDOWS
    seg->glob_position = -1;
#endif

    clear_avg_array(&seg->avg_frames_interval);

    for (int i = 0; i < s->nb_streams; i++)
    {
        seg->has_video +=
            (s->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO);
    }

    if (seg->has_video > 1)
    {
        av_log(s, AV_LOG_WARNING,
               "more than a single video stream present, "
               "expect issues decoding it.\n");
    }

    seg->oformat = av_guess_format(FILE_FORMAT, NULL, NULL);
    if (!seg->oformat)
    {
        return AVERROR_MUXER_NOT_FOUND;
    }

    if (seg->oformat->flags & AVFMT_NOFILE)
    {
        av_log(s, AV_LOG_ERROR, "format %s not supported.\n", seg->oformat->name);
        return AVERROR(EINVAL);
    }

    /* Initially s->filename contain path to folder with segment files.
    Store filename mask for removing old files by reserved disk space. */
    av_strlcpy(seg->segment_dirpath, s->filename, sizeof(seg->segment_dirpath));
    av_strlcat(seg->segment_dirpath, "/", sizeof(seg->segment_dirpath));
    av_strlcpy(seg->segment_filemask, s->filename, sizeof(seg->segment_filemask));
    av_strlcat(seg->segment_filemask, FILE_NAME_MASK, sizeof(seg->segment_filemask));

    /* Store filename date-time mask in s->filename for future use. */
    av_strlcat(s->filename, FILE_NAME_TIME_FORMAT, sizeof(s->filename));

    if (seg->control_file_path)
    {
#ifndef OS_WINDOWS
        seg->control_file_lock.l_type = F_RDLCK;
        seg->control_file_lock.l_whence = SEEK_SET;
        seg->control_file_lock.l_start = 0;
        seg->control_file_lock.l_len = 0;
        seg->control_file_lock.l_pid = getpid();
#endif

        seg->control_file_id = open(seg->control_file_path, O_RDONLY | O_CREAT, 0666);
        if (seg->control_file_id < 0)
        {
            av_log(s, AV_LOG_ERROR, "can't open shared control file: %s.\n", seg->control_file_path);
        }
        else
        {
            av_log(s, AV_LOG_INFO, "success open shared control file: %s.\n", seg->control_file_path);
        }
    }
    else
    {
        seg->control_file_id = -1;
    }

    if (!seg->is_file_writing_stopped)
    {
        ret = segment_start(s, NULL);
    }

    start_free_space_thread(seg);

    return ret;
}


static void write_packet_to_buffer(AVFormatContext *s, AVPacket *pkt)
{
    SegmentContext *seg = s->priv_data;

    if (seg->maximum_packet_buffer_size > 0)
    {
        int is_audio_packet = s->streams[pkt->stream_index]->codec->codec_type == AVMEDIA_TYPE_AUDIO;
        int can_add_packet_to_buffer =
            !seg->process_only_keyframes ||
            is_packet_contain_video_key_frame(s, pkt) ||
            is_audio_packet;

        if (can_add_packet_to_buffer &&
           (!is_audio_packet || seg->packet_buffer_size < seg->maximum_packet_buffer_size)) /* Drop audio on buffer overflow */
        {
            AVPacketList *pkt_list = (AVPacketList*) av_mallocz(sizeof(AVPacketList));
            if (pkt_list)
            {
               if (av_packet_ref(&pkt_list->pkt, pkt) >= 0)
               {
                    pkt_list->next = NULL;

                    /* Add packet to buffer */
                    if (seg->packet_buffer_start == NULL)
                    {
                        seg->packet_buffer_start = pkt_list;
                    }
                    else
                    {
                        seg->packet_buffer_end->next = pkt_list;
                    }
                    seg->packet_buffer_end = pkt_list;

                    seg->packet_buffer_size += pkt_list->pkt.size;

                    int64_t packet_buffer_size_in_millis = get_pkt_millis_from_buffer_start(s, seg->packet_buffer_start, pkt);
                    int is_packet_buffer_overflow = seg->packet_buffer_size > seg->maximum_packet_buffer_size;

                    if (is_packet_buffer_overflow)
                    {
                        av_log(s, AV_LOG_WARNING, "packet buffer overflow: %i kB %i millis\n",
                            seg->packet_buffer_size / 1024, (int)packet_buffer_size_in_millis);
                    }

                    if (packet_buffer_size_in_millis > seg->prerecord_time || is_packet_buffer_overflow)
                    {
                        shrink_packet_buffer(s);
                    }
                }
                else
                {
                    av_free(pkt_list);
                }
            }
        }
    }
}


static int write_packet_to_file(AVFormatContext *s, AVPacket *pkt)
{
    if (pkt->stream_index >= STREAM_COUNT_LIMIT)
    {
        return AVERROR_INVALIDDATA;
    }

    SegmentContext *seg = s->priv_data;
    AVFormatContext *oc = seg->avf;
    AVStream *st = s->streams[pkt->stream_index];
    int ret = 0;
    int64_t packet_pts_millis = PTS_MILLIS_NONE;

    /* Get packet timestamp in milliseconds and update first Packet Pts for this stream. */
    if (pkt->pts != AV_NOPTS_VALUE)
    {
        packet_pts_millis = get_pkt_timestamp_milliseconds(s, pkt);
        if (seg->first_packet_pts_array[pkt->stream_index] == AV_NOPTS_VALUE)
        {
            seg->first_packet_pts_array[pkt->stream_index] = av_rescale_q(pkt->pts, st->time_base, AV_TIME_BASE_Q);
        }
    }

    if (seg->first_packet_millis_array[pkt->stream_index] == PTS_MILLIS_NONE ||
        seg->first_packet_millis_array[pkt->stream_index] > packet_pts_millis)
    {
        seg->first_packet_millis_array[pkt->stream_index] = packet_pts_millis;
    }
    else if (seg->last_packet_millis_array[pkt->stream_index] != PTS_MILLIS_NONE &&
        seg->last_packet_millis_array[pkt->stream_index] + MAX_PACKET_INTERVAL_LIMIT_MILLIS < packet_pts_millis)
    {
        /* Some rtsp streams change timestamp after 5-6 seconds. For prevent wrong file duration calculation we ignore this video - overwrite file with new timestamps */
        seg->need_create_new_file = 1;

        av_log(s, AV_LOG_ERROR, "need to rewrite file due to big timestamp difference!\n");
        goto end;
    }

    if (packet_pts_millis > seg->last_packet_millis_array[pkt->stream_index])
    {
        seg->last_packet_millis_array[pkt->stream_index] = packet_pts_millis;
    }

    int64_t ts_offset = av_rescale_q(seg->first_packet_pts_array[pkt->stream_index], AV_TIME_BASE_Q, st->time_base);
    if (pkt->pts != AV_NOPTS_VALUE)
    {
        pkt->pts -= ts_offset;
    }
    if (pkt->dts != AV_NOPTS_VALUE)
    {
        pkt->dts -= ts_offset;  /* For DTS correction we must use pts offset (otherwise dts will be calculated incorrectly)! */
    }

    if (!seg->process_only_keyframes ||
        is_packet_contain_video_key_frame(s, pkt) ||
        st->codec->codec_type == AVMEDIA_TYPE_AUDIO)
    {
        /* Use interleave write function because we disable input packets interleaving to get frames without buffering(in real-time)*/
        ret = ff_write_chained_interleave(oc, pkt->stream_index, pkt, s);
    }

    if (ret < 0) /* Ignore format errors for prevent exiting from ffmpeg. */
    {
        if (++seg->error_count < MAXIMUM_IGNORED_ERROR_COUNT)
        {
            av_log(s, AV_LOG_ERROR, "write packet error! Error count: %i\n", seg->error_count);
            ret = 0;
        }
        else
        {
            av_log(s, AV_LOG_ERROR, "maximum error count reached, stop writing\n");
        }
    }

    if (seg->has_video &&
        packet_pts_millis != PTS_MILLIS_NONE &&
        st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
    {
        if (seg->prev_video_packet_millis != PTS_MILLIS_NONE &&
            av_q2d(st->avg_frame_rate) > 40.0) /* If wrong fps detected: >40 */
        {
            add_to_avg_array(&seg->avg_frames_interval, (int)(packet_pts_millis - seg->prev_video_packet_millis));
            int avg_frame_interval = get_avg_value(&seg->avg_frames_interval);
            if (seg->avg_frames_interval.size ==  MAX_AVG_ARRAY_SIZE &&
               (seg->prev_video_packet_millis > packet_pts_millis ||
                avg_frame_interval < MINIMUM_VIDEO_FRAMES_INTERVAL_MILLIS))
            {
                av_log(s, AV_LOG_ERROR, "write packet: too big fps! Stop working[%i ms]\n", avg_frame_interval);
                segment_end(oc, seg); /* Close current output. */
                ret = AVERROR_INVALIDDATA;
                goto end;
            }
        }
        seg->prev_video_packet_millis = packet_pts_millis;
    }

end:
    return ret;
}


static void write_packets_from_buffer(AVFormatContext *s, SegmentContext *seg)
{
    AVPacketList *pkt_list = seg->packet_buffer_start;
    while (pkt_list)
    {
        write_packet_to_file(s, &(pkt_list->pkt));
        pkt_list = pkt_list->next;
    }
    clear_packet_buffer(seg);
}


static int is_postrecord_active(AVFormatContext *s, SegmentContext *seg, AVPacket *pkt)
{
    int postrec_active = 0;

    if (seg->postrecord_time > 0 && seg->file_writing_stopped_packet_millis != PTS_MILLIS_NONE)
    {
        postrec_active = 1;

        int64_t current_millis = get_pkt_timestamp_milliseconds(s, pkt);
        if (seg->file_writing_stopped_packet_millis == 0)
        {
            av_log(s, AV_LOG_INFO, "postrecord timer started\n");
            seg->file_writing_stopped_packet_millis = current_millis;
        }
        else if (current_millis > seg->file_writing_stopped_packet_millis &&
                 seg->file_writing_stopped_packet_millis + seg->postrecord_time < current_millis)
        {
            postrec_active = 0;
        }
    }

    return postrec_active;
}


static void process_control_file(SegmentContext *seg)
{
    if (seg->control_file_id >= 0)
    {
#ifndef OS_WINDOWS
        seg->control_file_lock.l_type = F_RDLCK;
        if (fcntl(seg->control_file_id, F_SETLK, &seg->control_file_lock) >= 0)
        {
#endif
            char need_record = 0;
            if (lseek(seg->control_file_id, 0, SEEK_SET) == 0 &&
                read(seg->control_file_id, &need_record, 1) > 0)
            {
                seg->is_file_writing_stopped = (need_record == '0');
            }
#ifndef OS_WINDOWS
            seg->control_file_lock.l_type = F_UNLCK;
            fcntl(seg->control_file_id, F_SETLKW, &seg->control_file_lock);
        }
#endif
    }
}


static int seg_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    SegmentContext *seg = s->priv_data;
    AVFormatContext *oc = seg->avf;
    int ret = 0;

    int need_start_new_segment    = 0;
    int need_add_packet_to_buffer = 0;

    if (is_packet_contain_video(s, pkt) || !seg->has_video)
    {
        process_control_file(seg);
    }

    int has_video_key_frame =
        seg->packet_buffer_start != NULL || /* First packet in buffer must be always video key frame */
        is_packet_contain_video_key_frame(s, pkt);

    if (seg->is_file_writing_stopped && !is_postrecord_active(s, seg, pkt))
    {
        seg->need_create_new_file = 1;
        need_add_packet_to_buffer = has_video_key_frame;
    }
    else
    {
        if (!seg->need_create_new_file &&
            pkt->pts != AV_NOPTS_VALUE && seg->first_packet_millis_array[pkt->stream_index] != PTS_MILLIS_NONE &&
            seg->first_packet_millis_array[pkt->stream_index] + seg->max_file_duration_millis < get_pkt_timestamp_milliseconds(s, pkt))
        {
            seg->need_create_new_file = 1;
            av_log(s, AV_LOG_INFO, "need create new file by duration limit\n");
        }

        need_start_new_segment = seg->need_create_new_file && has_video_key_frame;

        if (!seg->is_file_writing_stopped)
        {
            seg->file_writing_stopped_packet_millis = 0;
        }
    }

    if (need_start_new_segment)
    {
        av_log(s, AV_LOG_DEBUG, "next segment starts at %d %"PRId64"\n",
               pkt->stream_index, pkt->pts);

        ret = segment_start(s, pkt);

        if (ret)
        {
            goto fail;
        }
    }

    if (need_add_packet_to_buffer) /* If file write is stopped, we need store packets in our internal buffer */
    {
        ret = segment_end(oc, seg);
        if (ret != 0)
        {
            goto fail;
        }

        write_packet_to_buffer(s, pkt);
    }
    else if (is_segment_started(seg)) /* Need add packet to file */
    {
        write_packets_from_buffer(s, seg); /* Write buffered packets to new file (if packet buffer is set). */
        ret = write_packet_to_file(s, pkt);
    }

fail:
    if (ret < 0)
    {
        clear_packet_buffer(seg);
        free_context(seg);
    }

    return ret;
}


static int seg_write_trailer(struct AVFormatContext *s)
{
    SegmentContext *seg = s->priv_data;
    AVFormatContext *oc = seg->avf;

    if (seg->control_file_id >= 0)
    {
        close(seg->control_file_id);
        seg->control_file_id = -1;
    }

    int ret = segment_end(oc, seg);

    stop_free_space_thread(seg);
    free_glob_info(seg);

    return ret;
}

#define OFFSET(x) offsetof(SegmentContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "s_stop",                   "flag for stop saving to file",                  OFFSET(is_file_writing_stopped),    AV_OPT_TYPE_INT,    {.i64 = 1}, 0, INT_MAX, E },
    { "s_max_prerecord_bytes",    "maximum prerecord buffer size in bytes",        OFFSET(maximum_packet_buffer_size), AV_OPT_TYPE_INT,    {.i64 = DEFAULT_MAXIMUM_PACKET_BUFFER_SIZE_BYTES}, 0, INT_MAX, E },
    { "s_max_file_duration_ms",   "maximum segment duration millis",               OFFSET(max_file_duration_millis),   AV_OPT_TYPE_INT64,  {.i64 = DEFAULT_FILE_DURATION_MILLIS}, 0, LLONG_MAX, E },
    { "s_prerecord_ms",           "minimal prerecord buffer size in milliseconds", OFFSET(prerecord_time),             AV_OPT_TYPE_INT64,  {.i64 = 0}, 0, LLONG_MAX, E },
    { "s_postrecord_ms",          "minimal postrecord size in milliseconds",       OFFSET(postrecord_time),            AV_OPT_TYPE_INT64,  {.i64 = 0}, 0, LLONG_MAX, E },
    { "s_control_file",           "shared control file for start/stop recording",  OFFSET(control_file_path),          AV_OPT_TYPE_STRING, {.str = NULL},  0, 0,   E },
    { "s_only_keyframes",         "flag for recording only keyframes(low fps)",    OFFSET(process_only_keyframes),     AV_OPT_TYPE_INT,    {.i64 = 0}, 0, INT_MAX, E },
    { "s_reserved_disk_space_mb", "minimum available free space on disk(Mb)",      OFFSET(reserved_disk_space_mb),     AV_OPT_TYPE_INT64,  {.i64 = DEFAULT_RESERVED_DISK_SPACE_MB}, 0, LLONG_MAX, E },
    { NULL },
};


static const AVClass seg_class = {
    .class_name = "ext segment muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};


AVOutputFormat ff_extsegment_muxer = {
    .name           = "extsegment",
    .long_name      = NULL_IF_CONFIG_SMALL("ext segment muxer"),
    .priv_data_size = sizeof(SegmentContext),
    .flags          = AVFMT_GLOBALHEADER | AVFMT_NOFILE | AVFMT_TS_NONSTRICT,
    .write_header   = seg_write_header,
    .write_packet   = seg_write_packet,
    .write_trailer  = seg_write_trailer,
    .priv_class     = &seg_class,
};
