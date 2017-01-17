#pragma warning (disable : 4996 4703)

#include "config.h"
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswresample/swresample.h"
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/fifo.h"
#include "libavutil/internal.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/dict.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avstring.h"
#include "libavutil/libm.h"
#include "libavutil/imgutils.h"
#include "libavutil/timestamp.h"
#include "libavutil/bprint.h"
#include "libavutil/time.h"
#include "libavutil/threadmessage.h"
#include "libavcodec/mathops.h"
#include "libavformat/os_support.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/buffersink.h"
#include <time.h>
#include "ffmpeg.h"
#include "cmdutils.h"
#include "libavutil/avassert.h"

const char program_name[] = "ffmpeg";
const int program_birth_year = 2000;

static FILE *vstats_file;

static int run_as_daemon = 0;
static int nb_frames_dup = 0;
static int nb_frames_drop = 0;
static int64_t decode_error_stat[2];

static int current_time;
AVIOContext *progress_avio = NULL;

static uint8_t *subtitle_out;

InputStream **input_streams = NULL;
int        nb_input_streams = 0;
InputFile   **input_files = NULL;
int        nb_input_files = 0;

OutputStream **output_streams = NULL;
int         nb_output_streams = 0;
OutputFile   **output_files = NULL;
int         nb_output_files = 0;

FilterGraph **filtergraphs;
int        nb_filtergraphs;

static volatile int received_sigterm = 0;
static volatile int received_nb_signals = 0;
static volatile int transcode_init_done = 0;
static volatile int ffmpeg_exited = 0;
static int main_return_code = 0;

static int decode_interrupt_cb(void *ctx)
{
	return received_nb_signals > transcode_init_done;
}

const AVIOInterruptCB int_cb = { decode_interrupt_cb, NULL };

static void ffmpeg_cleanup(int ret)
{
	int i, j;

	for (i = 0; i < nb_filtergraphs; i++) {
		FilterGraph *fg = filtergraphs[i];
		avfilter_graph_free(&fg->graph);
		for (j = 0; j < fg->nb_inputs; j++) {
			av_freep(&fg->inputs[j]->name);
			av_freep(&fg->inputs[j]);
		}
		av_freep(&fg->inputs);
		for (j = 0; j < fg->nb_outputs; j++) {
			av_freep(&fg->outputs[j]->name);
			av_freep(&fg->outputs[j]);
		}
		av_freep(&fg->outputs);
		av_freep(&fg->graph_desc);

		av_freep(&filtergraphs[i]);
	}
	av_freep(&filtergraphs);

	av_freep(&subtitle_out);

	/* close files */
	for (i = 0; i < nb_output_files; i++) {
		OutputFile *of = output_files[i];
		AVFormatContext *s;
		if (!of)
			continue;
		s = of->ctx;
		if (s && s->oformat && !(s->oformat->flags & AVFMT_NOFILE))
			avio_closep(&s->pb);
		avformat_free_context(s);
		av_dict_free(&of->opts);

		av_freep(&output_files[i]);
	}
	for (i = 0; i < nb_output_streams; i++) {
		OutputStream *ost = output_streams[i];

		if (!ost)
			continue;

		for (j = 0; j < ost->nb_bitstream_filters; j++)
			av_bsf_free(&ost->bsf_ctx[j]);
		av_freep(&ost->bsf_ctx);
		av_freep(&ost->bsf_extradata_updated);

		av_frame_free(&ost->filtered_frame);
		av_frame_free(&ost->last_frame);
		av_dict_free(&ost->encoder_opts);

		av_parser_close(ost->parser);
		avcodec_free_context(&ost->parser_avctx);

		av_freep(&ost->forced_keyframes);
		av_expr_free(ost->forced_keyframes_pexpr);
		av_freep(&ost->avfilter);
		av_freep(&ost->logfile_prefix);

		av_freep(&ost->audio_channels_map);
		ost->audio_channels_mapped = 0;

		av_dict_free(&ost->sws_dict);

		avcodec_free_context(&ost->enc_ctx);
		avcodec_parameters_free(&ost->ref_par);

		while (ost->muxing_queue && av_fifo_size(ost->muxing_queue)) {
			AVPacket pkt;
			av_fifo_generic_read(ost->muxing_queue, &pkt, sizeof(pkt), NULL);
			av_packet_unref(&pkt);
		}
		av_fifo_freep(&ost->muxing_queue);

		av_freep(&output_streams[i]);
	}
#if HAVE_PTHREADS
	free_input_threads();
#endif
	for (i = 0; i < nb_input_files; i++) {
		avformat_close_input(&input_files[i]->ctx);
		av_freep(&input_files[i]);
	}
	for (i = 0; i < nb_input_streams; i++) {
		InputStream *ist = input_streams[i];

		av_frame_free(&ist->decoded_frame);
		av_frame_free(&ist->filter_frame);
		av_dict_free(&ist->decoder_opts);
		avsubtitle_free(&ist->prev_sub.subtitle);
		av_frame_free(&ist->sub2video.frame);
		av_freep(&ist->filters);
		av_freep(&ist->hwaccel_device);
		av_freep(&ist->dts_buffer);

		avcodec_free_context(&ist->dec_ctx);

		av_freep(&input_streams[i]);
	}

	if (vstats_file) {
		if (fclose(vstats_file))
			av_log(NULL, AV_LOG_ERROR,
				"Error closing vstats file, loss of information possible: %s\n",
				av_err2str(AVERROR(errno)));
	}
	av_freep(&vstats_filename);

	av_freep(&input_streams);
	av_freep(&input_files);
	av_freep(&output_streams);
	av_freep(&output_files);

	uninit_opts();

	avformat_network_deinit();

	if (received_sigterm) {
		av_log(NULL, AV_LOG_INFO, "Exiting normally, received signal %d.\n",
			(int)received_sigterm);
	}
	else if (ret && transcode_init_done) {
		av_log(NULL, AV_LOG_INFO, "Conversion failed!\n");
	}
	ffmpeg_exited = 1;

	// Preview
	system("result.mp4");
	remove("result.mp4");
}

static void close_all_output_streams(OutputStream *ost, OSTFinished this_stream, OSTFinished others)
{
	int i;
	for (i = 0; i < nb_output_streams; i++) {
		OutputStream *ost2 = output_streams[i];
		ost2->finished |= ost == ost2 ? this_stream : others;
	}
}

static void write_packet(OutputFile *of, AVPacket *pkt, OutputStream *ost)
{
	AVFormatContext *s = of->ctx;
	AVStream *st = ost->st;
	int ret;

	if (!of->header_written) {
		AVPacket tmp_pkt;
		/* the muxer is not initialized yet, buffer the packet */
		if (!av_fifo_space(ost->muxing_queue)) {
			int new_size = FFMIN(2 * av_fifo_size(ost->muxing_queue),
				ost->max_muxing_queue_size);
			if (new_size <= av_fifo_size(ost->muxing_queue)) {
				av_log(NULL, AV_LOG_ERROR,
					"Too many packets buffered for output stream %d:%d.\n",
					ost->file_index, ost->st->index);
				exit_program(1);
			}
			ret = av_fifo_realloc2(ost->muxing_queue, new_size);
			if (ret < 0)
				exit_program(1);
		}
		av_packet_move_ref(&tmp_pkt, pkt);
		av_fifo_generic_write(ost->muxing_queue, &tmp_pkt, sizeof(tmp_pkt), NULL);
		return;
	}

	/*
	* Audio encoders may split the packets --  #frames in != #packets out.
	* But there is no reordering, so we can limit the number of output packets
	* by simply dropping them here.
	* Counting encoded video frames needs to be done separately because of
	* reordering, see do_video_out()
	*/
	if (!(st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && ost->encoding_needed)) {
		if (ost->frame_number >= ost->max_frames) {
			av_packet_unref(pkt);
			return;
		}
		ost->frame_number++;
	}
	if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
		int i;
		uint8_t *sd = av_packet_get_side_data(pkt, AV_PKT_DATA_QUALITY_STATS,
			NULL);
		ost->quality = sd ? AV_RL32(sd) : -1;
		ost->pict_type = sd ? sd[4] : AV_PICTURE_TYPE_NONE;

		for (i = 0; i < FF_ARRAY_ELEMS(ost->error); i++) {
			if (sd && i < sd[5])
				ost->error[i] = AV_RL64(sd + 8 + 8 * i);
			else
				ost->error[i] = -1;
		}

		if (ost->frame_rate.num && ost->is_cfr) {
			if (pkt->duration > 0)
				av_log(NULL, AV_LOG_WARNING, "Overriding packet duration by frame rate, this should not happen\n");
			pkt->duration = av_rescale_q(1, av_inv_q(ost->frame_rate),
				ost->st->time_base);
		}
	}

	ost->last_mux_dts = pkt->dts;

	ost->data_size += pkt->size;
	ost->packets_written++;

	pkt->stream_index = ost->index;

	ret = av_interleaved_write_frame(s, pkt);
	av_packet_unref(pkt);
}

static void close_output_stream(OutputStream *ost)
{
	OutputFile *of = output_files[ost->file_index];

	ost->finished |= ENCODER_FINISHED;
	if (of->shortest) {
		int64_t end = av_rescale_q(ost->sync_opts - ost->first_pts, ost->enc_ctx->time_base, AV_TIME_BASE_Q);
		of->recording_time = FFMIN(of->recording_time, end);
	}
}

static void output_packet(OutputFile *of, AVPacket *pkt, OutputStream *ost)
{
	int ret = 0;

	/* apply the output bitstream filters, if any */
	if (ost->nb_bitstream_filters) {
		int idx;

		av_packet_split_side_data(pkt);
		ret = av_bsf_send_packet(ost->bsf_ctx[0], pkt);
		if (ret < 0)
			goto finish;

		idx = 1;
		while (idx) {
			/* get a packet from the previous filter up the chain */
			ret = av_bsf_receive_packet(ost->bsf_ctx[idx - 1], pkt);
			/* HACK! - aac_adtstoasc updates extradata after filtering the first frame when
			* the api states this shouldn't happen after init(). Propagate it here to the
			* muxer and to the next filters in the chain to workaround this.
			* TODO/FIXME - Make aac_adtstoasc use new packet side data instead of changing
			* par_out->extradata and adapt muxers accordingly to get rid of this. */
			if (!(ost->bsf_extradata_updated[idx - 1] & 1)) {
				ret = avcodec_parameters_copy(ost->st->codecpar, ost->bsf_ctx[idx - 1]->par_out);
				if (ret < 0)
					goto finish;
				ost->bsf_extradata_updated[idx - 1] |= 1;
			}
			if (ret == AVERROR(EAGAIN)) {
				ret = 0;
				idx--;
				continue;
			}
			else if (ret < 0)
				goto finish;

			/* send it to the next filter down the chain or to the muxer */
			if (idx < ost->nb_bitstream_filters) {
				/* HACK/FIXME! - See above */
				if (!(ost->bsf_extradata_updated[idx] & 2)) {
					ret = avcodec_parameters_copy(ost->bsf_ctx[idx]->par_out, ost->bsf_ctx[idx - 1]->par_out);
					if (ret < 0)
						goto finish;
					ost->bsf_extradata_updated[idx] |= 2;
				}
				ret = av_bsf_send_packet(ost->bsf_ctx[idx], pkt);
				if (ret < 0)
					goto finish;
				idx++;
			}
			else
				write_packet(of, pkt, ost);
		}
	}
	else
		write_packet(of, pkt, ost);

finish:
	if (ret < 0 && ret != AVERROR_EOF) {
		av_log(NULL, AV_LOG_ERROR, "Error applying bitstream filters to an output "
			"packet for stream #%d:%d.\n", ost->file_index, ost->index);
		if (exit_on_error)
			exit_program(1);
	}
}

static void do_audio_out(OutputFile *of, OutputStream *ost, AVFrame *frame)
{
	AVCodecContext *enc = ost->enc_ctx;
	AVPacket pkt;
	int ret;

	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	if (frame->pts == AV_NOPTS_VALUE || audio_sync_method < 0)
		frame->pts = ost->sync_opts;
	ost->sync_opts = frame->pts + frame->nb_samples;
	ost->samples_encoded += frame->nb_samples;
	ost->frames_encoded++;

	ret = avcodec_send_frame(enc, frame);
	if (ret < 0)
		goto error;

	while (1) {
		ret = avcodec_receive_packet(enc, &pkt);
		if (ret == AVERROR(EAGAIN))
			break;
		if (ret < 0)
			goto error;

		av_packet_rescale_ts(&pkt, enc->time_base, ost->st->time_base);

		output_packet(of, &pkt, ost);
	}

	return;
error:
	av_log(NULL, AV_LOG_FATAL, "Audio encoding failed\n");
	exit_program(1);
}

static void do_video_out(OutputFile *of, OutputStream *ost, AVFrame *next_picture, double sync_ipts)
{
	int ret, format_video_sync;
	AVPacket pkt;
	AVCodecContext *enc = ost->enc_ctx;
	AVCodecParameters *mux_par = ost->st->codecpar;
	int nb_frames, nb0_frames, i;
	double delta, delta0;
	double duration = 0;
	int frame_size = 0;
	InputStream *ist = NULL;
	AVFilterContext *filter = ost->filter->filter;

	if (ost->source_index >= 0)
		ist = input_streams[ost->source_index];

	if (filter->inputs[0]->frame_rate.num > 0 &&
		filter->inputs[0]->frame_rate.den > 0)
		duration = 1 / (av_q2d(filter->inputs[0]->frame_rate) * av_q2d(enc->time_base));

	if (ist && ist->st->start_time != AV_NOPTS_VALUE && ist->st->first_dts != AV_NOPTS_VALUE && ost->frame_rate.num)
		duration = FFMIN(duration, 1 / (av_q2d(ost->frame_rate) * av_q2d(enc->time_base)));

	if (!ost->filters_script &&
		!ost->filters &&
		next_picture &&
		ist &&
		lrintf(av_frame_get_pkt_duration(next_picture) * av_q2d(ist->st->time_base) / av_q2d(enc->time_base)) > 0) {
		duration = lrintf(av_frame_get_pkt_duration(next_picture) * av_q2d(ist->st->time_base) / av_q2d(enc->time_base));
	}

	if (!next_picture) {
		//end, flushing
		nb0_frames = nb_frames = mid_pred(ost->last_nb0_frames[0],
			ost->last_nb0_frames[1],
			ost->last_nb0_frames[2]);
	}
	else {
		delta0 = sync_ipts - ost->sync_opts; // delta0 is the "drift" between the input frame (next_picture) and where it would fall in the output.
		delta = delta0 + duration;

		/* by default, we output a single frame */
		nb0_frames = 0; // tracks the number of times the PREVIOUS frame should be duplicated, mostly for variable framerate (VFR)
		nb_frames = 1;

		format_video_sync = video_sync_method;
		if (format_video_sync == VSYNC_AUTO) {
			if (!strcmp(of->ctx->oformat->name, "avi")) {
				format_video_sync = VSYNC_VFR;
			}
			else
				format_video_sync = (of->ctx->oformat->flags & AVFMT_VARIABLE_FPS) ? ((of->ctx->oformat->flags & AVFMT_NOTIMESTAMPS) ? VSYNC_PASSTHROUGH : VSYNC_VFR) : VSYNC_CFR;
			if (ist
				&& format_video_sync == VSYNC_CFR
				&& input_files[ist->file_index]->ctx->nb_streams == 1
				&& input_files[ist->file_index]->input_ts_offset == 0) {
				format_video_sync = VSYNC_VSCFR;
			}
			if (format_video_sync == VSYNC_CFR && copy_ts) {
				format_video_sync = VSYNC_VSCFR;
			}
		}
		ost->is_cfr = (format_video_sync == VSYNC_CFR || format_video_sync == VSYNC_VSCFR);

		if (delta0 < 0 &&
			delta > 0 &&
			format_video_sync != VSYNC_PASSTHROUGH &&
			format_video_sync != VSYNC_DROP) {
			if (delta0 < -0.6) {
				av_log(NULL, AV_LOG_WARNING, "Past duration %f too large\n", -delta0);
			}
			else
				av_log(NULL, AV_LOG_DEBUG, "Clipping frame in rate conversion by %f\n", -delta0);
			sync_ipts = ost->sync_opts;
			duration += delta0;
			delta0 = 0;
		}

		switch (format_video_sync) {
		case VSYNC_VSCFR:
			if (ost->frame_number == 0 && delta0 >= 0.5) {
				av_log(NULL, AV_LOG_DEBUG, "Not duplicating %d initial frames\n", (int)lrintf(delta0));
				delta = duration;
				delta0 = 0;
				ost->sync_opts = lrint(sync_ipts);
			}
		case VSYNC_CFR:
			// FIXME set to 0.5 after we fix some dts/pts bugs like in avidec.c
			if (frame_drop_threshold && delta < frame_drop_threshold && ost->frame_number) {
				nb_frames = 0;
			}
			else if (delta < -1.1)
				nb_frames = 0;
			else if (delta > 1.1) {
				nb_frames = lrintf(delta);
				if (delta0 > 1.1)
					nb0_frames = lrintf(delta0 - 0.6);
			}
			break;
		case VSYNC_VFR:
			if (delta <= -0.6)
				nb_frames = 0;
			else if (delta > 0.6)
				ost->sync_opts = lrint(sync_ipts);
			break;
		case VSYNC_DROP:
		case VSYNC_PASSTHROUGH:
			ost->sync_opts = lrint(sync_ipts);
			break;
		default:
			av_assert0(0);
		}
	}

	nb_frames = FFMIN(nb_frames, ost->max_frames - ost->frame_number);
	nb0_frames = FFMIN(nb0_frames, nb_frames);

	memmove(ost->last_nb0_frames + 1,
		ost->last_nb0_frames,
		sizeof(ost->last_nb0_frames[0]) * (FF_ARRAY_ELEMS(ost->last_nb0_frames) - 1));
	ost->last_nb0_frames[0] = nb0_frames;
	ost->last_dropped = nb_frames == nb0_frames && next_picture;

	/* duplicates frame if needed */
	for (i = 0; i < nb_frames; i++) {
		AVFrame *in_picture;
		av_init_packet(&pkt);
		pkt.data = NULL;
		pkt.size = 0;

		if (i < nb0_frames && ost->last_frame) {
			in_picture = ost->last_frame;
		}
		else
			in_picture = next_picture;

		if (!in_picture)
			return;

		in_picture->pts = ost->sync_opts;

#if FF_API_LAVF_FMT_RAWPICTURE
		if (of->ctx->oformat->flags & AVFMT_RAWPICTURE &&
			enc->codec->id == AV_CODEC_ID_RAWVIDEO) {
			/* raw pictures are written as AVPicture structure to
			avoid any copies. We support temporarily the older
			method. */
			if (in_picture->interlaced_frame)
				mux_par->field_order = in_picture->top_field_first ? AV_FIELD_TB : AV_FIELD_BT;
			else
				mux_par->field_order = AV_FIELD_PROGRESSIVE;
			pkt.data = (uint8_t *)in_picture;
			pkt.size = sizeof(AVPicture);
			pkt.pts = av_rescale_q(in_picture->pts, enc->time_base, ost->st->time_base);
			pkt.flags |= AV_PKT_FLAG_KEY;

			output_packet(of, &pkt, ost);
		}
		else
#endif
		{
			int forced_keyframe = 0;
			double pts_time;

			if (enc->flags & (AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME) &&
				ost->top_field_first >= 0)
				in_picture->top_field_first = !!ost->top_field_first;

			if (in_picture->interlaced_frame) {
				if (enc->codec->id == AV_CODEC_ID_MJPEG)
					mux_par->field_order = in_picture->top_field_first ? AV_FIELD_TT : AV_FIELD_BB;
				else
					mux_par->field_order = in_picture->top_field_first ? AV_FIELD_TB : AV_FIELD_BT;
			}
			else
				mux_par->field_order = AV_FIELD_PROGRESSIVE;

			in_picture->quality = enc->global_quality;
			in_picture->pict_type = 0;

			pts_time = in_picture->pts != AV_NOPTS_VALUE ?
				in_picture->pts * av_q2d(enc->time_base) : NAN;

			ost->frames_encoded++;

			ret = avcodec_send_frame(enc, in_picture);
			if (ret < 0)
				goto error;

			while (1) {
				ret = avcodec_receive_packet(enc, &pkt);
				if (ret == AVERROR(EAGAIN))
					break;
				if (ret < 0)
					goto error;

				if (debug_ts) {
					av_log(NULL, AV_LOG_INFO, "encoder -> type:video "
						"pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s\n",
						av_ts2str(pkt.pts), av_ts2timestr(pkt.pts, &enc->time_base),
						av_ts2str(pkt.dts), av_ts2timestr(pkt.dts, &enc->time_base));
				}

				if (pkt.pts == AV_NOPTS_VALUE && !(enc->codec->capabilities & AV_CODEC_CAP_DELAY))
					pkt.pts = ost->sync_opts;

				av_packet_rescale_ts(&pkt, enc->time_base, ost->st->time_base);

				if (debug_ts) {
					av_log(NULL, AV_LOG_INFO, "encoder -> type:video "
						"pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s\n",
						av_ts2str(pkt.pts), av_ts2timestr(pkt.pts, &ost->st->time_base),
						av_ts2str(pkt.dts), av_ts2timestr(pkt.dts, &ost->st->time_base));
				}

				frame_size = pkt.size;
				output_packet(of, &pkt, ost);

				/* if two pass, output log */
				if (ost->logfile && enc->stats_out) {
					fprintf(ost->logfile, "%s", enc->stats_out);
				}
			}
		}
		ost->sync_opts++;
		/*
		* For video, number of frames in == number of packets out.
		* But there may be reordering, so we can't throw away frames on encoder
		* flush, we need to limit them here, before they go into encoder.
		*/
		ost->frame_number++;
	}

	if (!ost->last_frame)
		ost->last_frame = av_frame_alloc();
	av_frame_unref(ost->last_frame);
	if (next_picture && ost->last_frame)
		av_frame_ref(ost->last_frame, next_picture);
	else
		av_frame_free(&ost->last_frame);

	return;
error:
	av_log(NULL, AV_LOG_FATAL, "Video encoding failed\n");
	exit_program(1);
}

static double psnr(double d)
{
	return -10.0 * log10(d);
}

static void finish_output_stream(OutputStream *ost)
{
	OutputFile *of = output_files[ost->file_index];
	int i;

	ost->finished = ENCODER_FINISHED | MUXER_FINISHED;

	if (of->shortest) {
		for (i = 0; i < of->ctx->nb_streams; i++)
			output_streams[of->ost_index + i]->finished = ENCODER_FINISHED | MUXER_FINISHED;
	}
}

/**
* Get and encode new output from any of the filtergraphs, without causing
* activity.
*
* @return  0 for success, <0 for severe errors
*/
static int reap_filters(int flush)
{
	AVFrame *filtered_frame = NULL;
	int i;

	/* Reap all buffers present in the buffer sinks */
	for (i = 0; i < nb_output_streams; i++) {
		OutputStream *ost = output_streams[i];
		OutputFile    *of = output_files[ost->file_index];
		AVFilterContext *filter;
		AVCodecContext *enc = ost->enc_ctx;
		int ret = 0;

		if (!ost->filter)
			continue;
		filter = ost->filter->filter;

		if (!ost->filtered_frame && !(ost->filtered_frame = av_frame_alloc())) {
			return AVERROR(ENOMEM);
		}
		filtered_frame = ost->filtered_frame;

		while (1) {
			double float_pts = AV_NOPTS_VALUE; // this is identical to filtered_frame.pts but with higher precision
			ret = av_buffersink_get_frame_flags(filter, filtered_frame,
				AV_BUFFERSINK_FLAG_NO_REQUEST);
			if (ret < 0) {
				if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
					av_log(NULL, AV_LOG_WARNING,
						"Error in av_buffersink_get_frame_flags(): %s\n", av_err2str(ret));
				}
				else if (flush && ret == AVERROR_EOF) {
					if (filter->inputs[0]->type == AVMEDIA_TYPE_VIDEO)
						do_video_out(of, ost, NULL, AV_NOPTS_VALUE);
				}
				break;
			}
			if (ost->finished) {
				av_frame_unref(filtered_frame);
				continue;
			}
			if (filtered_frame->pts != AV_NOPTS_VALUE) {
				int64_t start_time = (of->start_time == AV_NOPTS_VALUE) ? 0 : of->start_time;
				AVRational tb = enc->time_base;
				int extra_bits = av_clip(29 - av_log2(tb.den), 0, 16);

				tb.den <<= extra_bits;
				float_pts =
					av_rescale_q(filtered_frame->pts, filter->inputs[0]->time_base, tb) -
					av_rescale_q(start_time, AV_TIME_BASE_Q, tb);
				float_pts /= 1 << extra_bits;
				// avoid exact midoints to reduce the chance of rounding differences, this can be removed in case the fps code is changed to work with integers
				float_pts += FFSIGN(float_pts) * 1.0 / (1 << 17);

				filtered_frame->pts =
					av_rescale_q(filtered_frame->pts, filter->inputs[0]->time_base, enc->time_base) -
					av_rescale_q(start_time, AV_TIME_BASE_Q, enc->time_base);
			}
			//if (ost->source_index >= 0)
			//    *filtered_frame= *input_streams[ost->source_index]->decoded_frame; //for me_threshold

			switch (filter->inputs[0]->type) {
			case AVMEDIA_TYPE_VIDEO:
				if (!ost->frame_aspect_ratio.num)
					enc->sample_aspect_ratio = filtered_frame->sample_aspect_ratio;

				if (debug_ts) {
					av_log(NULL, AV_LOG_INFO, "filter -> pts:%s pts_time:%s exact:%f time_base:%d/%d\n",
						av_ts2str(filtered_frame->pts), av_ts2timestr(filtered_frame->pts, &enc->time_base),
						float_pts,
						enc->time_base.num, enc->time_base.den);
				}

				do_video_out(of, ost, filtered_frame, float_pts);
				break;
			case AVMEDIA_TYPE_AUDIO:
				if (!(enc->codec->capabilities & AV_CODEC_CAP_PARAM_CHANGE) &&
					enc->channels != av_frame_get_channels(filtered_frame)) {
					av_log(NULL, AV_LOG_ERROR,
						"Audio filter graph output is not normalized and encoder does not support parameter changes\n");
					break;
				}
				do_audio_out(of, ost, filtered_frame);
				break;
			default:
				// TODO support subtitle filters
				av_assert0(0);
			}

			av_frame_unref(filtered_frame);
		}
	}

	return 0;
}

static void flush_encoders(void)
{
	int i, ret;

	for (i = 0; i < nb_output_streams; i++) {
		OutputStream   *ost = output_streams[i];
		AVCodecContext *enc = ost->enc_ctx;
		OutputFile      *of = output_files[ost->file_index];
		int stop_encoding = 0;

		if (!ost->encoding_needed)
			continue;

		if (enc->codec_type == AVMEDIA_TYPE_AUDIO && enc->frame_size <= 1)
			continue;
#if FF_API_LAVF_FMT_RAWPICTURE
		if (enc->codec_type == AVMEDIA_TYPE_VIDEO && (of->ctx->oformat->flags & AVFMT_RAWPICTURE) && enc->codec->id == AV_CODEC_ID_RAWVIDEO)
			continue;
#endif

		if (enc->codec_type != AVMEDIA_TYPE_VIDEO && enc->codec_type != AVMEDIA_TYPE_AUDIO)
			continue;

		avcodec_send_frame(enc, NULL);

		for (;;) {
			const char *desc = NULL;

			switch (enc->codec_type) {
			case AVMEDIA_TYPE_AUDIO:
				desc = "audio";
				break;
			case AVMEDIA_TYPE_VIDEO:
				desc = "video";
				break;
			default:
				av_assert0(0);
			}

			if (1) {
				AVPacket pkt;
				int pkt_size;
				av_init_packet(&pkt);
				pkt.data = NULL;
				pkt.size = 0;

				ret = avcodec_receive_packet(enc, &pkt);
				if (ret < 0 && ret != AVERROR_EOF) {
					av_log(NULL, AV_LOG_FATAL, "%s encoding failed: %s\n",
						desc,
						av_err2str(ret));
					exit_program(1);
				}
				if (ost->logfile && enc->stats_out) {
					fprintf(ost->logfile, "%s", enc->stats_out);
				}
				if (ret == AVERROR_EOF) {
					stop_encoding = 1;
					break;
				}
				if (ost->finished & MUXER_FINISHED) {
					av_packet_unref(&pkt);
					continue;
				}
				av_packet_rescale_ts(&pkt, enc->time_base, ost->st->time_base);
				pkt_size = pkt.size;
				output_packet(of, &pkt, ost);
			}

			if (stop_encoding)
				break;
		}
	}
}

/*
* Check whether a packet from ist should be written into ost at this time
*/
static int check_output_constraints(InputStream *ist, OutputStream *ost)
{
	OutputFile *of = output_files[ost->file_index];
	int ist_index = input_files[ist->file_index]->ist_index + ist->st->index;

	if (ost->source_index != ist_index)
		return 0;

	if (ost->finished)
		return 0;

	if (of->start_time != AV_NOPTS_VALUE && ist->pts < of->start_time)
		return 0;

	return 1;
}

int guess_input_channel_layout(InputStream *ist)
{
	AVCodecContext *dec = ist->dec_ctx;

	if (!dec->channel_layout) {
		char layout_name[256];

		if (dec->channels > ist->guess_layout_max)
			return 0;
		dec->channel_layout = av_get_default_channel_layout(dec->channels);
		if (!dec->channel_layout)
			return 0;
		av_get_channel_layout_string(layout_name, sizeof(layout_name),
			dec->channels, dec->channel_layout);
		av_log(NULL, AV_LOG_WARNING, "Guessed Channel Layout for Input Stream "
			"#%d.%d : %s\n", ist->file_index, ist->st->index, layout_name);
	}
	return 1;
}

// This does not quite work like avcodec_decode_audio4/avcodec_decode_video2.
// There is the following difference: if you got a frame, you must call
// it again with pkt=NULL. pkt==NULL is treated differently from pkt.size==0
// (pkt==NULL means get more output, pkt.size==0 is a flush/drain packet)
static int decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt)
{
	int ret;

	*got_frame = 0;

	if (pkt) {
		ret = avcodec_send_packet(avctx, pkt);
		// In particular, we don't expect AVERROR(EAGAIN), because we read all
		// decoded frames with avcodec_receive_frame() until done.
		if (ret < 0 && ret != AVERROR_EOF)
			return ret;
	}

	ret = avcodec_receive_frame(avctx, frame);
	if (ret < 0 && ret != AVERROR(EAGAIN))
		return ret;
	if (ret >= 0)
		*got_frame = 1;

	return 0;
}

static int decode_audio(InputStream *ist, AVPacket *pkt, int *got_output)
{
	AVFrame *decoded_frame, *f;
	AVCodecContext *avctx = ist->dec_ctx;
	int i, ret, err = 0, resample_changed;
	AVRational decoded_frame_tb;

	if (!ist->decoded_frame && !(ist->decoded_frame = av_frame_alloc()))
		return AVERROR(ENOMEM);
	if (!ist->filter_frame && !(ist->filter_frame = av_frame_alloc()))
		return AVERROR(ENOMEM);
	decoded_frame = ist->decoded_frame;

	ret = decode(avctx, decoded_frame, got_output, pkt);

	if (ret >= 0 && avctx->sample_rate <= 0) {
		av_log(avctx, AV_LOG_ERROR, "Sample rate %d invalid\n", avctx->sample_rate);
		ret = AVERROR_INVALIDDATA;
	}

	if (!*got_output || ret < 0)
		return ret;

	ist->samples_decoded += decoded_frame->nb_samples;
	ist->frames_decoded++;

#if 1
	/* increment next_dts to use for the case where the input stream does not
	have timestamps or there are multiple frames in the packet */
	ist->next_pts += ((int64_t)AV_TIME_BASE * decoded_frame->nb_samples) /
		avctx->sample_rate;
	ist->next_dts += ((int64_t)AV_TIME_BASE * decoded_frame->nb_samples) /
		avctx->sample_rate;
#endif

	resample_changed = ist->resample_sample_fmt != decoded_frame->format ||
		ist->resample_channels != avctx->channels ||
		ist->resample_channel_layout != decoded_frame->channel_layout ||
		ist->resample_sample_rate != decoded_frame->sample_rate;
	if (resample_changed) {
		char layout1[64], layout2[64];

		if (!guess_input_channel_layout(ist)) {
			av_log(NULL, AV_LOG_FATAL, "Unable to find default channel "
				"layout for Input Stream #%d.%d\n", ist->file_index,
				ist->st->index);
			exit_program(1);
		}
		decoded_frame->channel_layout = avctx->channel_layout;

		av_get_channel_layout_string(layout1, sizeof(layout1), ist->resample_channels,
			ist->resample_channel_layout);
		av_get_channel_layout_string(layout2, sizeof(layout2), avctx->channels,
			decoded_frame->channel_layout);

		av_log(NULL, AV_LOG_INFO,
			"Input stream #%d:%d frame changed from rate:%d fmt:%s ch:%d chl:%s to rate:%d fmt:%s ch:%d chl:%s\n",
			ist->file_index, ist->st->index,
			ist->resample_sample_rate, av_get_sample_fmt_name(ist->resample_sample_fmt),
			ist->resample_channels, layout1,
			decoded_frame->sample_rate, av_get_sample_fmt_name(decoded_frame->format),
			avctx->channels, layout2);

		ist->resample_sample_fmt = decoded_frame->format;
		ist->resample_sample_rate = decoded_frame->sample_rate;
		ist->resample_channel_layout = decoded_frame->channel_layout;
		ist->resample_channels = avctx->channels;

		for (i = 0; i < nb_filtergraphs; i++)
			if (ist_in_filtergraph(filtergraphs[i], ist)) {
				FilterGraph *fg = filtergraphs[i];
				if (configure_filtergraph(fg) < 0) {
					av_log(NULL, AV_LOG_FATAL, "Error reinitializing filters!\n");
					exit_program(1);
				}
			}
	}

	if (decoded_frame->pts != AV_NOPTS_VALUE) {
		decoded_frame_tb = ist->st->time_base;
	}
	else if (pkt && pkt->pts != AV_NOPTS_VALUE) {
		decoded_frame->pts = pkt->pts;
		decoded_frame_tb = ist->st->time_base;
	}
	else {
		decoded_frame->pts = ist->dts;
		decoded_frame_tb = AV_TIME_BASE_Q;
	}
	if (decoded_frame->pts != AV_NOPTS_VALUE)
		decoded_frame->pts = av_rescale_delta(decoded_frame_tb, decoded_frame->pts,
		(AVRational) {
		1, avctx->sample_rate
	}, decoded_frame->nb_samples, &ist->filter_in_rescale_delta_last,
			(AVRational) {
		1, avctx->sample_rate
	});
	ist->nb_samples = decoded_frame->nb_samples;
	for (i = 0; i < ist->nb_filters; i++) {
		if (i < ist->nb_filters - 1) {
			f = ist->filter_frame;
			err = av_frame_ref(f, decoded_frame);
			if (err < 0)
				break;
		}
		else
			f = decoded_frame;
		err = av_buffersrc_add_frame_flags(ist->filters[i]->filter, f,
			AV_BUFFERSRC_FLAG_PUSH);
		if (err == AVERROR_EOF)
			err = 0; /* ignore */
		if (err < 0)
			break;
	}
	decoded_frame->pts = AV_NOPTS_VALUE;

	av_frame_unref(ist->filter_frame);
	av_frame_unref(decoded_frame);
	return err < 0 ? err : ret;
}

static int decode_video(InputStream *ist, AVPacket *pkt, int *got_output, int eof)
{
	AVFrame *decoded_frame, *f;
	int i, ret = 0, err = 0, resample_changed;
	int64_t best_effort_timestamp;
	int64_t dts = AV_NOPTS_VALUE;
	AVRational *frame_sample_aspect;
	AVPacket avpkt;

	// With fate-indeo3-2, we're getting 0-sized packets before EOF for some
	// reason. This seems like a semi-critical bug. Don't trigger EOF, and
	// skip the packet.
	if (!eof && pkt && pkt->size == 0)
		return 0;

	if (!ist->decoded_frame && !(ist->decoded_frame = av_frame_alloc()))
		return AVERROR(ENOMEM);
	if (!ist->filter_frame && !(ist->filter_frame = av_frame_alloc()))
		return AVERROR(ENOMEM);
	decoded_frame = ist->decoded_frame;
	if (ist->dts != AV_NOPTS_VALUE)
		dts = av_rescale_q(ist->dts, AV_TIME_BASE_Q, ist->st->time_base);
	if (pkt) {
		avpkt = *pkt;
		avpkt.dts = dts; // ffmpeg.c probably shouldn't do this
	}

	// The old code used to set dts on the drain packet, which does not work
	// with the new API anymore.
	if (eof) {
		void *new = av_realloc_array(ist->dts_buffer, ist->nb_dts_buffer + 1, sizeof(ist->dts_buffer[0]));
		if (!new)
			return AVERROR(ENOMEM);
		ist->dts_buffer = new;
		ist->dts_buffer[ist->nb_dts_buffer++] = dts;
	}

	ret = decode(ist->dec_ctx, decoded_frame, got_output, pkt ? &avpkt : NULL);

	// The following line may be required in some cases where there is no parser
	// or the parser does not has_b_frames correctly
	if (ist->st->codecpar->video_delay < ist->dec_ctx->has_b_frames) {
		if (ist->dec_ctx->codec_id == AV_CODEC_ID_H264) {
			ist->st->codecpar->video_delay = ist->dec_ctx->has_b_frames;
		}
		else
			av_log(ist->dec_ctx, AV_LOG_WARNING,
				"video_delay is larger in decoder than demuxer %d > %d.\n"
				"If you want to help, upload a sample "
				"of this file to ftp://upload.ffmpeg.org/incoming/ "
				"and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)",
				ist->dec_ctx->has_b_frames,
				ist->st->codecpar->video_delay);
	}

	if (*got_output && ret >= 0) {
		if (ist->dec_ctx->width != decoded_frame->width ||
			ist->dec_ctx->height != decoded_frame->height ||
			ist->dec_ctx->pix_fmt != decoded_frame->format) {
			av_log(NULL, AV_LOG_DEBUG, "Frame parameters mismatch context %d,%d,%d != %d,%d,%d\n",
				decoded_frame->width,
				decoded_frame->height,
				decoded_frame->format,
				ist->dec_ctx->width,
				ist->dec_ctx->height,
				ist->dec_ctx->pix_fmt);
		}
	}

	if (!*got_output || ret < 0)
		return ret;

	if (ist->top_field_first >= 0)
		decoded_frame->top_field_first = ist->top_field_first;

	ist->frames_decoded++;

	if (ist->hwaccel_retrieve_data && decoded_frame->format == ist->hwaccel_pix_fmt) {
		err = ist->hwaccel_retrieve_data(ist->dec_ctx, decoded_frame);
		if (err < 0)
			goto fail;
	}
	ist->hwaccel_retrieved_pix_fmt = decoded_frame->format;

	best_effort_timestamp = av_frame_get_best_effort_timestamp(decoded_frame);

	if (eof && best_effort_timestamp == AV_NOPTS_VALUE && ist->nb_dts_buffer > 0) {
		best_effort_timestamp = ist->dts_buffer[0];

		for (i = 0; i < ist->nb_dts_buffer - 1; i++)
			ist->dts_buffer[i] = ist->dts_buffer[i + 1];
		ist->nb_dts_buffer--;
	}

	if (best_effort_timestamp != AV_NOPTS_VALUE) {
		int64_t ts = av_rescale_q(decoded_frame->pts = best_effort_timestamp, ist->st->time_base, AV_TIME_BASE_Q);

		if (ts != AV_NOPTS_VALUE)
			ist->next_pts = ist->pts = ts;
	}

	if (debug_ts) {
		av_log(NULL, AV_LOG_INFO, "decoder -> ist_index:%d type:video "
			"frame_pts:%s frame_pts_time:%s best_effort_ts:%"PRId64" best_effort_ts_time:%s keyframe:%d frame_type:%d time_base:%d/%d\n",
			ist->st->index, av_ts2str(decoded_frame->pts),
			av_ts2timestr(decoded_frame->pts, &ist->st->time_base),
			best_effort_timestamp,
			av_ts2timestr(best_effort_timestamp, &ist->st->time_base),
			decoded_frame->key_frame, decoded_frame->pict_type,
			ist->st->time_base.num, ist->st->time_base.den);
	}

	if (ist->st->sample_aspect_ratio.num)
		decoded_frame->sample_aspect_ratio = ist->st->sample_aspect_ratio;

	resample_changed = ist->resample_width != decoded_frame->width ||
		ist->resample_height != decoded_frame->height ||
		ist->resample_pix_fmt != decoded_frame->format;
	if (resample_changed) {
		av_log(NULL, AV_LOG_INFO,
			"Input stream #%d:%d frame changed from size:%dx%d fmt:%s to size:%dx%d fmt:%s\n",
			ist->file_index, ist->st->index,
			ist->resample_width, ist->resample_height, av_get_pix_fmt_name(ist->resample_pix_fmt),
			decoded_frame->width, decoded_frame->height, av_get_pix_fmt_name(decoded_frame->format));

		ist->resample_width = decoded_frame->width;
		ist->resample_height = decoded_frame->height;
		ist->resample_pix_fmt = decoded_frame->format;

		for (i = 0; i < nb_filtergraphs; i++) {
			if (ist_in_filtergraph(filtergraphs[i], ist) && ist->reinit_filters &&
				configure_filtergraph(filtergraphs[i]) < 0) {
				av_log(NULL, AV_LOG_FATAL, "Error reinitializing filters!\n");
				exit_program(1);
			}
		}
	}

	frame_sample_aspect = av_opt_ptr(avcodec_get_frame_class(), decoded_frame, "sample_aspect_ratio");
	for (i = 0; i < ist->nb_filters; i++) {
		if (!frame_sample_aspect->num)
			*frame_sample_aspect = ist->st->sample_aspect_ratio;

		if (i < ist->nb_filters - 1) {
			f = ist->filter_frame;
			err = av_frame_ref(f, decoded_frame);
			if (err < 0)
				break;
		}
		else
			f = decoded_frame;
		err = av_buffersrc_add_frame_flags(ist->filters[i]->filter, f, AV_BUFFERSRC_FLAG_PUSH);
		if (err == AVERROR_EOF) {
			err = 0; /* ignore */
		}
		else if (err < 0) {
			av_log(NULL, AV_LOG_FATAL,
				"Failed to inject frame into filter network: %s\n", av_err2str(err));
			exit_program(1);
		}
	}

fail:
	av_frame_unref(ist->filter_frame);
	av_frame_unref(decoded_frame);
	return err < 0 ? err : ret;
}

static int transcode_subtitles(InputStream *ist, AVPacket *pkt, int *got_output)
{
	AVSubtitle subtitle;
	int i, ret = avcodec_decode_subtitle2(ist->dec_ctx,
		&subtitle, got_output, pkt);

	check_decode_result(NULL, got_output, ret);

	if (ist->fix_sub_duration) {
		int end = 1;
		if (ist->prev_sub.got_output) {
			end = av_rescale(subtitle.pts - ist->prev_sub.subtitle.pts,
				1000, AV_TIME_BASE);
			if (end < ist->prev_sub.subtitle.end_display_time) {
				av_log(ist->dec_ctx, AV_LOG_DEBUG,
					"Subtitle duration reduced from %d to %d%s\n",
					ist->prev_sub.subtitle.end_display_time, end,
					end <= 0 ? ", dropping it" : "");
				ist->prev_sub.subtitle.end_display_time = end;
			}
		}
		FFSWAP(int, *got_output, ist->prev_sub.got_output);
		FFSWAP(int, ret, ist->prev_sub.ret);
		FFSWAP(AVSubtitle, subtitle, ist->prev_sub.subtitle);
		if (end <= 0)
			goto out;
	}

	if (!*got_output)
		return ret;

	if (!subtitle.num_rects)
		goto out;

	ist->frames_decoded++;

	for (i = 0; i < nb_output_streams; i++) {
		OutputStream *ost = output_streams[i];

		if (!check_output_constraints(ist, ost) || !ost->encoding_needed
			|| ost->enc->type != AVMEDIA_TYPE_SUBTITLE)
			continue;
	}

out:
	avsubtitle_free(&subtitle);
	return ret;
}

static int send_filter_eof(InputStream *ist)
{
	int i, ret;
	for (i = 0; i < ist->nb_filters; i++) {
		ret = av_buffersrc_add_frame(ist->filters[i]->filter, NULL);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* pkt = NULL means EOF (needed to flush decoder buffers) */
static int process_input_packet(InputStream *ist, const AVPacket *pkt, int no_eof)
{
	int ret = 0, i;
	int repeating = 0;
	int eof_reached = 0;

	AVPacket avpkt;
	if (!ist->saw_first_ts) {
		ist->dts = ist->st->avg_frame_rate.num ? -ist->dec_ctx->has_b_frames * AV_TIME_BASE / av_q2d(ist->st->avg_frame_rate) : 0;
		ist->pts = 0;
		ist->saw_first_ts = 1;
	}

	if (ist->next_dts == AV_NOPTS_VALUE)
		ist->next_dts = ist->dts;
	if (ist->next_pts == AV_NOPTS_VALUE)
		ist->next_pts = ist->pts;

	if (!pkt) {
		/* EOF handling */
		av_init_packet(&avpkt);
		avpkt.data = NULL;
		avpkt.size = 0;
	}
	else {
		avpkt = *pkt;
	}

	if (pkt && pkt->dts != AV_NOPTS_VALUE) {
		ist->next_dts = ist->dts = av_rescale_q(pkt->dts, ist->st->time_base, AV_TIME_BASE_Q);
		if (ist->dec_ctx->codec_type != AVMEDIA_TYPE_VIDEO || !ist->decoding_needed)
			ist->next_pts = ist->pts = ist->dts;
	}

	// while we have more to decode or while the decoder did output something on EOF
	while (ist->decoding_needed) {
		int duration = 0;
		int got_output = 0;

		ist->pts = ist->next_pts;
		ist->dts = ist->next_dts;

		switch (ist->dec_ctx->codec_type) {
		case AVMEDIA_TYPE_AUDIO:
			ret = decode_audio(ist, repeating ? NULL : &avpkt, &got_output);
			break;
		case AVMEDIA_TYPE_VIDEO:
			ret = decode_video(ist, repeating ? NULL : &avpkt, &got_output, !pkt);
			if (!repeating || !pkt || got_output) {
				if (pkt && pkt->duration) {
					duration = av_rescale_q(pkt->duration, ist->st->time_base, AV_TIME_BASE_Q);
				}
				else if (ist->dec_ctx->framerate.num != 0 && ist->dec_ctx->framerate.den != 0) {
					int ticks = av_stream_get_parser(ist->st) ? av_stream_get_parser(ist->st)->repeat_pict + 1 : ist->dec_ctx->ticks_per_frame;
					duration = ((int64_t)AV_TIME_BASE *
						ist->dec_ctx->framerate.den * ticks) /
						ist->dec_ctx->framerate.num / ist->dec_ctx->ticks_per_frame;
				}

				if (ist->dts != AV_NOPTS_VALUE && duration) {
					ist->next_dts += duration;
				}
				else
					ist->next_dts = AV_NOPTS_VALUE;
			}

			if (got_output)
				ist->next_pts += duration; //FIXME the duration is not correct in some cases
			break;
		default:
			return -1;
		}

		if (ret == AVERROR_EOF) {
			eof_reached = 1;
			break;
		}

		if (!got_output)
			break;

		// During draining, we might get multiple output frames in this loop.
		// ffmpeg.c does not drain the filter chain on configuration changes,
		// which means if we send multiple frames at once to the filters, and
		// one of those frames changes configuration, the buffered frames will
		// be lost. This can upset certain FATE tests.
		// Decode only 1 frame per call on EOF to appease these FATE tests.
		// The ideal solution would be to rewrite decoding to use the new
		// decoding API in a better way.
		if (!pkt)
			break;

		repeating = 1;
	}

	/* after flushing, send an EOF on all the filter inputs attached to the stream */
	/* except when looping we need to flush but not to send an EOF */
	if (!pkt && ist->decoding_needed && eof_reached && !no_eof) {
		send_filter_eof(ist);
	}

	return !eof_reached;
}

static int get_buffer(AVCodecContext *s, AVFrame *frame, int flags)
{
	return avcodec_default_get_buffer2(s, frame, flags);
}

static int init_input_stream(int ist_index, char *error, int error_len)
{
	int ret;
	InputStream *ist = input_streams[ist_index];

	if (ist->decoding_needed) {
		AVCodec *codec = ist->dec;
		ist->dec_ctx->opaque = ist;
		ist->dec_ctx->get_buffer2 = get_buffer;
		ist->dec_ctx->thread_safe_callbacks = 1;
		av_opt_set_int(ist->dec_ctx, "refcounted_frames", 1, 0);
		av_dict_set(&ist->decoder_opts, "sub_text_format", "ass", AV_DICT_DONT_OVERWRITE);

		/* Useful for subtitles retiming by lavf (FIXME), skipping samples in
		* audio, and video decoders such as cuvid or mediacodec */
		av_codec_set_pkt_timebase(ist->dec_ctx, ist->st->time_base);

		if (!av_dict_get(ist->decoder_opts, "threads", NULL, 0))
			av_dict_set(&ist->decoder_opts, "threads", "auto", 0);
		avcodec_open2(ist->dec_ctx, codec, &ist->decoder_opts);
	}

	ist->next_pts = AV_NOPTS_VALUE;
	ist->next_dts = AV_NOPTS_VALUE;

	return 0;
}

static InputStream *get_input_stream(OutputStream *ost)
{
	if (ost->source_index >= 0)
		return input_streams[ost->source_index];
	return NULL;
}

static int check_init_output_file(OutputFile *of, int file_index)
{
	for (int i = 0; i < of->ctx->nb_streams; i++) {
		OutputStream *ost = output_streams[of->ost_index + i];
		if (!ost->initialized) {
			return 0;
		}
	}
	of->ctx->interrupt_callback = int_cb;
	avformat_write_header(of->ctx, &of->opts);
	of->header_written = 1;
	av_dump_format(of->ctx, file_index, of->ctx->filename, 1);
	return 0;
}

static int init_output_bsfs(OutputStream *ost)
{
	AVBSFContext *ctx;
	int i, ret;

	if (!ost->nb_bitstream_filters)
		return 0;

	for (i = 0; i < ost->nb_bitstream_filters; i++) {
		ctx = ost->bsf_ctx[i];
		ret = avcodec_parameters_copy(ctx->par_in, i ? ost->bsf_ctx[i - 1]->par_out : ost->st->codecpar);
		ctx->time_base_in = i ? ost->bsf_ctx[i - 1]->time_base_out : ost->st->time_base;
		ret = av_bsf_init(ctx);
	}

	ctx = ost->bsf_ctx[ost->nb_bitstream_filters - 1];
	avcodec_parameters_copy(ost->st->codecpar, ctx->par_out);
	ost->st->time_base = ctx->time_base_out;

	return 0;
}

static int init_output_stream(OutputStream *ost, char *error, int error_len)
{
	int ret = 0;

	if (ost->encoding_needed) {
		AVCodec      *codec = ost->enc;
		AVCodecContext *dec = NULL;
		InputStream *ist;

		if ((ist = get_input_stream(ost)))
			dec = ist->dec_ctx;
		if (dec && dec->subtitle_header) {
			/* ASS code assumes this buffer is null terminated so add extra byte. */
			ost->enc_ctx->subtitle_header = av_mallocz(dec->subtitle_header_size + 1);
			if (!ost->enc_ctx->subtitle_header)
				return AVERROR(ENOMEM);
			memcpy(ost->enc_ctx->subtitle_header, dec->subtitle_header, dec->subtitle_header_size);
			ost->enc_ctx->subtitle_header_size = dec->subtitle_header_size;
		}
		if (!av_dict_get(ost->encoder_opts, "threads", NULL, 0))
			av_dict_set(&ost->encoder_opts, "threads", "auto", 0);
		if (ost->enc->type == AVMEDIA_TYPE_AUDIO &&
			!codec->defaults &&
			!av_dict_get(ost->encoder_opts, "b", NULL, 0) &&
			!av_dict_get(ost->encoder_opts, "ab", NULL, 0))
			av_dict_set(&ost->encoder_opts, "b", "128000", 0);

		if (ost->filter && ost->filter->filter->inputs[0]->hw_frames_ctx) {
			ost->enc_ctx->hw_frames_ctx = av_buffer_ref(ost->filter->filter->inputs[0]->hw_frames_ctx);
			if (!ost->enc_ctx->hw_frames_ctx)
				return AVERROR(ENOMEM);
		}

		if ((ret = avcodec_open2(ost->enc_ctx, codec, &ost->encoder_opts)) < 0) {
			//if (ret == AVERROR_EXPERIMENTAL)
			//	abort_codec_experimental(codec, 1);
			snprintf(error, error_len,
				"Error while opening encoder for output stream #%d:%d - "
				"maybe incorrect parameters such as bit_rate, rate, width or height",
				ost->file_index, ost->index);
			return ret;
		}
		if (ost->enc->type == AVMEDIA_TYPE_AUDIO &&
			!(ost->enc->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
			av_buffersink_set_frame_size(ost->filter->filter,
				ost->enc_ctx->frame_size);
		//assert_avoptions(ost->encoder_opts);
		if (ost->enc_ctx->bit_rate && ost->enc_ctx->bit_rate < 1000)
			av_log(NULL, AV_LOG_WARNING, "The bitrate parameter is set too low."
				" It takes bits/s as argument, not kbits/s\n");

		ret = avcodec_parameters_from_context(ost->st->codecpar, ost->enc_ctx);
		if (ret < 0) {
			av_log(NULL, AV_LOG_FATAL,
				"Error initializing the output stream codec context.\n");
			exit_program(1);
		}
		/*
		* FIXME: ost->st->codec should't be needed here anymore.
		*/
		ret = avcodec_copy_context(ost->st->codec, ost->enc_ctx);
		if (ret < 0)
			return ret;

		if (ost->enc_ctx->nb_coded_side_data) {
			int i;

			ost->st->side_data = av_realloc_array(NULL, ost->enc_ctx->nb_coded_side_data,
				sizeof(*ost->st->side_data));
			if (!ost->st->side_data)
				return AVERROR(ENOMEM);

			for (i = 0; i < ost->enc_ctx->nb_coded_side_data; i++) {
				const AVPacketSideData *sd_src = &ost->enc_ctx->coded_side_data[i];
				AVPacketSideData *sd_dst = &ost->st->side_data[i];

				sd_dst->data = av_malloc(sd_src->size);
				if (!sd_dst->data)
					return AVERROR(ENOMEM);
				memcpy(sd_dst->data, sd_src->data, sd_src->size);
				sd_dst->size = sd_src->size;
				sd_dst->type = sd_src->type;
				ost->st->nb_side_data++;
			}
		}

		// copy timebase while removing common factors
		ost->st->time_base = av_add_q(ost->enc_ctx->time_base, (AVRational) { 0, 1 });
		ost->st->codec->codec = ost->enc_ctx->codec;
	}

	/* initialize bitstream filters for the output stream
	* needs to be done here, because the codec id for streamcopy is not
	* known until now */
	ret = init_output_bsfs(ost);
	if (ret < 0)
		return ret;

	ost->initialized = 1;

	ret = check_init_output_file(output_files[ost->file_index], ost->file_index);
	if (ret < 0)
		return ret;

	return ret;
}

static void set_encoder_id(OutputFile *of, OutputStream *ost)
{
	int encoder_string_len = sizeof(LIBAVCODEC_IDENT) + strlen(ost->enc->name) + 2;
	uint8_t *encoder_string = av_mallocz(encoder_string_len);

	int format_flags = 0;
	int codec_flags = 0;
	if (!(format_flags & AVFMT_FLAG_BITEXACT) && !(codec_flags & AV_CODEC_FLAG_BITEXACT))
		av_strlcpy(encoder_string, LIBAVCODEC_IDENT " ", encoder_string_len);
	else
		av_strlcpy(encoder_string, "Lavc ", encoder_string_len);
	av_strlcat(encoder_string, ost->enc->name, encoder_string_len);
	av_dict_set(&ost->st->metadata, "encoder", encoder_string, AV_DICT_DONT_STRDUP_VAL | AV_DICT_DONT_OVERWRITE);
}

static int transcode_init(void)
{
	int ret = 0, i, j, k;
	AVFormatContext *oc;
	OutputStream *ost;
	InputStream *ist;
	char error[1024] = { 0 };

	/* for each output stream, we compute the right encoding parameters */
	for (i = 0; i < nb_output_streams; i++) {
		ost = output_streams[i];
		oc = output_files[ost->file_index]->ctx;
		ist = get_input_stream(ost);

		if (ist) {
			ost->st->disposition = ist->st->disposition;
		}

		if (!ost->stream_copy) {
			AVCodecContext *enc_ctx = ost->enc_ctx;
			AVCodecContext *dec_ctx = NULL;

			set_encoder_id(output_files[ost->file_index], ost);

			if (ist) {
				dec_ctx = ist->dec_ctx;
				enc_ctx->chroma_sample_location = dec_ctx->chroma_sample_location;
			}

			if ((enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO || enc_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
				&& filtergraph_is_simple(ost->filter->graph)) {
				FilterGraph *fg = ost->filter->graph;
				configure_filtergraph(fg);
			}

			if (enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
				if (!ost->frame_rate.num)
					ost->frame_rate = av_buffersink_get_frame_rate(ost->filter->filter);
				if (ist && !ost->frame_rate.num)
					ost->frame_rate = ist->framerate;
				if (ist && !ost->frame_rate.num)
					ost->frame_rate = ist->st->r_frame_rate;
				if (ist && !ost->frame_rate.num) {
					ost->frame_rate = (AVRational) { 25, 1 };
				}
				if (ost->enc && ost->enc->supported_framerates && !ost->force_fps) {
					int idx = av_find_nearest_q_idx(ost->frame_rate, ost->enc->supported_framerates);
					ost->frame_rate = ost->enc->supported_framerates[idx];
				}
				// reduce frame rate for mpeg4 to be within the spec limits
				if (enc_ctx->codec_id == AV_CODEC_ID_MPEG4) {
					av_reduce(&ost->frame_rate.num, &ost->frame_rate.den, ost->frame_rate.num, ost->frame_rate.den, 65535);
				}
			}

			switch (enc_ctx->codec_type) {
			case AVMEDIA_TYPE_AUDIO:
				enc_ctx->sample_fmt = ost->filter->filter->inputs[0]->format;
				if (dec_ctx)
					enc_ctx->bits_per_raw_sample = FFMIN(dec_ctx->bits_per_raw_sample,
						av_get_bytes_per_sample(enc_ctx->sample_fmt) << 3);
				enc_ctx->sample_rate = ost->filter->filter->inputs[0]->sample_rate;
				enc_ctx->channel_layout = ost->filter->filter->inputs[0]->channel_layout;
				enc_ctx->channels = avfilter_link_get_channels(ost->filter->filter->inputs[0]);
				enc_ctx->time_base = (AVRational) { 1, enc_ctx->sample_rate };
				break;
			case AVMEDIA_TYPE_VIDEO:
				enc_ctx->time_base = av_inv_q(ost->frame_rate);
				if (!(enc_ctx->time_base.num && enc_ctx->time_base.den))
					enc_ctx->time_base = ost->filter->filter->inputs[0]->time_base;
				for (j = 0; j < ost->forced_kf_count; j++)
					ost->forced_kf_pts[j] = av_rescale_q(ost->forced_kf_pts[j], AV_TIME_BASE_Q, enc_ctx->time_base);

				enc_ctx->width = ost->filter->filter->inputs[0]->w;
				enc_ctx->height = ost->filter->filter->inputs[0]->h;
				enc_ctx->sample_aspect_ratio = ost->st->sample_aspect_ratio =
					ost->frame_aspect_ratio.num ? // overridden by the -aspect cli option
					av_mul_q(ost->frame_aspect_ratio, (AVRational) { enc_ctx->height, enc_ctx->width }) :
					ost->filter->filter->inputs[0]->sample_aspect_ratio;
				if (!strncmp(ost->enc->name, "libx264", 7) &&
					enc_ctx->pix_fmt == AV_PIX_FMT_NONE &&
					ost->filter->filter->inputs[0]->format != AV_PIX_FMT_YUV420P)
					av_log(NULL, AV_LOG_WARNING,
						"No pixel format specified, %s for H.264 encoding chosen.\n"
						"Use -pix_fmt yuv420p for compatibility with outdated media players.\n",
						av_get_pix_fmt_name(ost->filter->filter->inputs[0]->format));
				if (!strncmp(ost->enc->name, "mpeg2video", 10) &&
					enc_ctx->pix_fmt == AV_PIX_FMT_NONE &&
					ost->filter->filter->inputs[0]->format != AV_PIX_FMT_YUV420P)
					av_log(NULL, AV_LOG_WARNING,
						"No pixel format specified, %s for MPEG-2 encoding chosen.\n"
						"Use -pix_fmt yuv420p for compatibility with outdated media players.\n",
						av_get_pix_fmt_name(ost->filter->filter->inputs[0]->format));
				enc_ctx->pix_fmt = ost->filter->filter->inputs[0]->format;
				if (dec_ctx)
					enc_ctx->bits_per_raw_sample = FFMIN(dec_ctx->bits_per_raw_sample,
						av_pix_fmt_desc_get(enc_ctx->pix_fmt)->comp[0].depth);

				ost->st->avg_frame_rate = ost->frame_rate;

				if (!dec_ctx || enc_ctx->width != dec_ctx->width ||
					enc_ctx->height != dec_ctx->height || enc_ctx->pix_fmt != dec_ctx->pix_fmt) {
					enc_ctx->bits_per_raw_sample = frame_bits_per_raw_sample;
				}
				break;
			case AVMEDIA_TYPE_SUBTITLE:
				enc_ctx->time_base = (AVRational) { 1, 1000 };
				if (!enc_ctx->width) {
					enc_ctx->width = input_streams[ost->source_index]->st->codecpar->width;
					enc_ctx->height = input_streams[ost->source_index]->st->codecpar->height;
				}
				break;
			case AVMEDIA_TYPE_DATA:
				break;
			default:
				abort();
				break;
			}
		}
	}

	/* init input streams */
	for (i = 0; i < nb_input_streams; i++)
		if ((ret = init_input_stream(i, error, sizeof(error))) < 0) {
			for (i = 0; i < nb_output_streams; i++) {
				ost = output_streams[i];
				avcodec_close(ost->enc_ctx);
			}
			goto dump_format;
		}

	/* open each encoder */
	for (i = 0; i < nb_output_streams; i++) {
		ret = init_output_stream(output_streams[i], error, sizeof(error));
		if (ret < 0)
			goto dump_format;
	}

	/* write headers for files with no streams */
	for (i = 0; i < nb_output_files; i++) {
		oc = output_files[i]->ctx;
		if (oc->oformat->flags & AVFMT_NOSTREAMS && oc->nb_streams == 0) {
			ret = check_init_output_file(output_files[i], i);
			if (ret < 0)
				goto dump_format;
		}
	}

dump_format:
	/* dump the stream mapping */
	av_log(NULL, AV_LOG_INFO, "Stream mapping:\n");
	for (i = 0; i < nb_input_streams; i++) {
		ist = input_streams[i];

		for (j = 0; j < ist->nb_filters; j++) {
			if (!filtergraph_is_simple(ist->filters[j]->graph)) {
				av_log(NULL, AV_LOG_INFO, "  Stream #%d:%d (%s) -> %s",
					ist->file_index, ist->st->index, ist->dec ? ist->dec->name : "?",
					ist->filters[j]->name);
				if (nb_filtergraphs > 1)
					av_log(NULL, AV_LOG_INFO, " (graph %d)", ist->filters[j]->graph->index);
				av_log(NULL, AV_LOG_INFO, "\n");
			}
		}
	}

	for (i = 0; i < nb_output_streams; i++) {
		ost = output_streams[i];

		const AVCodec *in_codec = input_streams[ost->source_index]->dec;
		const AVCodec *out_codec = ost->enc;
		const char *decoder_name = "?";
		const char *in_codec_name = "?";
		const char *encoder_name = "?";
		const char *out_codec_name = "?";
		const AVCodecDescriptor *desc;

		if (in_codec) {
			decoder_name = in_codec->name;
			desc = avcodec_descriptor_get(in_codec->id);
			if (desc)
				in_codec_name = desc->name;
			if (!strcmp(decoder_name, in_codec_name))
				decoder_name = "native";
		}

		if (out_codec) {
			encoder_name = out_codec->name;
			desc = avcodec_descriptor_get(out_codec->id);
			if (desc)
				out_codec_name = desc->name;
			if (!strcmp(encoder_name, out_codec_name))
				encoder_name = "native";
		}
	}

	transcode_init_done = 1;

	return 0;
}

/* Return 1 if there remain streams where more output is wanted, 0 otherwise. */
static int need_output(void)
{
	int i;

	for (i = 0; i < nb_output_streams; i++) {
		OutputStream *ost = output_streams[i];
		OutputFile *of = output_files[ost->file_index];
		AVFormatContext *os = output_files[ost->file_index]->ctx;

		if (ost->finished ||
			(os->pb && avio_tell(os->pb) >= of->limit_filesize))
			continue;
		if (ost->frame_number >= ost->max_frames) {
			int j;
			for (j = 0; j < of->ctx->nb_streams; j++)
				close_output_stream(output_streams[of->ost_index + j]);
			continue;
		}

		return 1;
	}

	return 0;
}

/**
* Select the output stream to process.
*
* @return  selected output stream, or NULL if none available
*/
static OutputStream *choose_output(void)
{
	int i;
	int64_t opts_min = INT64_MAX;
	OutputStream *ost_min = NULL;

	for (i = 0; i < nb_output_streams; i++) {
		OutputStream *ost = output_streams[i];
		int64_t opts = ost->st->cur_dts == AV_NOPTS_VALUE ? INT64_MIN :
			av_rescale_q(ost->st->cur_dts, ost->st->time_base, AV_TIME_BASE_Q);

		if (!ost->finished && opts < opts_min) {
			opts_min = opts;
			ost_min = ost->unavailable ? NULL : ost;
		}
	}
	return ost_min;
}

static int get_input_packet(InputFile *f, AVPacket *pkt)
{
	return av_read_frame(f->ctx, pkt);
}

static void reset_eagain(void)
{
	int i;
	for (i = 0; i < nb_input_files; i++)
		input_files[i]->eagain = 0;
	for (i = 0; i < nb_output_streams; i++)
		output_streams[i]->unavailable = 0;
}

/*
* Return
* - 0 -- one packet was read and processed
* - AVERROR(EAGAIN) -- no packets were available for selected file,
*   this function should be called again
* - AVERROR_EOF -- this function should not be called again
*/
static int process_input(int file_index)
{
	InputFile *ifile = input_files[file_index];
	AVFormatContext *is;
	InputStream *ist;
	AVPacket pkt;
	int ret, i, j;
	int64_t duration;
	int64_t pkt_dts;

	is = ifile->ctx;
	ret = get_input_packet(ifile, &pkt);

	reset_eagain();

	ist = input_streams[ifile->ist_index + pkt.stream_index];

	ist->data_size += pkt.size;
	ist->nb_packets++;

	if (pkt.dts != AV_NOPTS_VALUE)
		pkt.dts += av_rescale_q(ifile->ts_offset, AV_TIME_BASE_Q, ist->st->time_base);
	if (pkt.pts != AV_NOPTS_VALUE)
		pkt.pts += av_rescale_q(ifile->ts_offset, AV_TIME_BASE_Q, ist->st->time_base);

	if (pkt.pts != AV_NOPTS_VALUE)
		pkt.pts *= ist->ts_scale;
	if (pkt.dts != AV_NOPTS_VALUE)
		pkt.dts *= ist->ts_scale;

	pkt_dts = av_rescale_q_rnd(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

	duration = av_rescale_q(ifile->duration, ifile->time_base, ist->st->time_base);
	if (pkt.pts != AV_NOPTS_VALUE) {
		pkt.pts += duration;
		ist->max_pts = FFMAX(pkt.pts, ist->max_pts);
		ist->min_pts = FFMIN(pkt.pts, ist->min_pts);
	}

	if (pkt.dts != AV_NOPTS_VALUE)
		pkt.dts += duration;

	pkt_dts = av_rescale_q_rnd(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

	if (pkt.dts != AV_NOPTS_VALUE)
		ifile->last_ts = av_rescale_q(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q);

	process_input_packet(ist, &pkt, 0);

discard_packet:
	av_packet_unref(&pkt);
	return 0;
}

/**
* Perform a step of transcoding for the specified filter graph.
*
* @param[in]  graph     filter graph to consider
* @param[out] best_ist  input stream where a frame would allow to continue
* @return  0 for success, <0 for error
*/
static int transcode_from_filter(FilterGraph *graph, InputStream **best_ist)
{
	int i, ret;
	int nb_requests, nb_requests_max = 0;
	InputFilter *ifilter;
	InputStream *ist;

	*best_ist = NULL;
	ret = avfilter_graph_request_oldest(graph->graph);
	if (ret >= 0)
		return reap_filters(0);

	if (ret == AVERROR_EOF) {
		ret = reap_filters(1);
		for (i = 0; i < graph->nb_outputs; i++)
			close_output_stream(graph->outputs[i]->ost);
		return ret;
	}

	for (i = 0; i < graph->nb_inputs; i++) {
		ifilter = graph->inputs[i];
		ist = ifilter->ist;
		nb_requests = av_buffersrc_get_nb_failed_requests(ifilter->filter);
		if (nb_requests > nb_requests_max) {
			nb_requests_max = nb_requests;
			*best_ist = ist;
		}
	}

	return 0;
}

/**
* Run a single step of transcoding.
*
* @return  0 for success, <0 for error
*/
static int transcode_step(void)
{
	OutputStream *ost;
	InputStream  *ist;
	int ret;

	ost = choose_output();

	if (ost->filter) {
		if ((ret = transcode_from_filter(ost->filter->graph, &ist)) < 0)
			return ret;
		if (!ist)
			return 0;
	}

	ret = process_input(ist->file_index);
	if (ret == AVERROR(EAGAIN)) {
		if (input_files[ist->file_index]->eagain)
			ost->unavailable = 1;
		return 0;
	}

	if (ret < 0)
		return ret == AVERROR_EOF ? 0 : ret;

	return reap_filters(0);
}

/*
* The following code is the main loop of the file converter
*/
static int transcode(void)
{
	int ret, i;
	AVFormatContext *os;
	OutputStream *ost;
	InputStream *ist;
	int64_t timer_start;
	int64_t total_packets_written = 0;

	ret = transcode_init();

	timer_start = av_gettime_relative();

	while (!received_sigterm) {
		/* check if there's any stream where output is still needed */
		if (!need_output()) {
			break;
		}
		transcode_step();
	}

	/* at the end of stream, we must flush the decoder buffers */
	for (i = 0; i < nb_input_streams; i++) {
		ist = input_streams[i];
		if (!input_files[ist->file_index]->eof_reached && ist->decoding_needed) {
			process_input_packet(ist, NULL, 0);
		}
	}
	flush_encoders();

	/* write the trailer if needed and close file */
	for (i = 0; i < nb_output_files; i++) {
		os = output_files[i]->ctx;
		av_write_trailer(os);
	}

	/* close each encoder */
	for (i = 0; i < nb_output_streams; i++) {
		ost = output_streams[i];
		if (ost->encoding_needed) {
			av_freep(&ost->enc_ctx->stats_in);
		}
		total_packets_written += ost->packets_written;
	}

	/* close each decoder */
	for (i = 0; i < nb_input_streams; i++) {
		ist = input_streams[i];
		if (ist->decoding_needed) {
			avcodec_close(ist->dec_ctx);
			if (ist->hwaccel_uninit)
				ist->hwaccel_uninit(ist->dec_ctx);
		}
	}

	av_buffer_unref(&hw_device_ctx);

	/* finished ! */
	ret = 0;

fail:

	if (output_streams) {
		for (i = 0; i < nb_output_streams; i++) {
			ost = output_streams[i];
			if (ost) {
				av_freep(&ost->forced_kf_pts);
				av_freep(&ost->apad);
				av_freep(&ost->disposition);
				av_dict_free(&ost->encoder_opts);
				av_dict_free(&ost->sws_dict);
				av_dict_free(&ost->swr_opts);
				av_dict_free(&ost->resample_opts);
			}
		}
	}
	return ret;
}

int main(int argc, char **argv)
{
	// Clean
	remove("result.mp4");

	register_exit(ffmpeg_cleanup);

	avcodec_register_all();
	avfilter_register_all();
	av_register_all();

	ffmpeg_parse_options(argc, argv);
	transcode();

	exit_program(received_nb_signals ? 255 : main_return_code);
	return main_return_code;
}