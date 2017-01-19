#pragma warning (disable : 4996)

#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

static AVFormatContext *ifmt_ctx = NULL;
static AVFormatContext *ofmt_ctx = NULL;
static char *input_file = NULL;
static char *output_file = NULL;
static int src_w = 0;
static int src_h = 0;
static int dst_w = 0;
static int dst_h = 0;

// Return zero on success else nonzero
static int open_input_file(const char *filename) {
	ifmt_ctx = NULL;
	if (avformat_open_input(&ifmt_ctx, filename, NULL, NULL) != 0)
	{
		printf("ERROR: Cannot open input file.\n");
		return -1;
	}

	if (avformat_find_stream_info(ifmt_ctx, NULL) < 0) {
		printf("ERROR: Cannot find stream information.\n");
		return -1;
	}

	for (int i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		AVStream *stream;
		AVCodecContext *codec_ctx;
		stream = ifmt_ctx->streams[i];
		codec_ctx = stream->codec;

		if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			if (avcodec_open2(codec_ctx, avcodec_find_decoder(codec_ctx->codec_id), NULL) != 0)
			{
				printf("ERROR: Failed to open decoder for stream #%u.\n", i);
				return -1;
			}
		}
	}

	av_dump_format(ifmt_ctx, 0, filename, 0);
	return 0;
}

// Return zero on success else nonzero
static int open_output_file(const char *filename) {
	AVStream *out_stream;
	AVStream *in_stream;
	AVCodecContext *dec_ctx, *enc_ctx;
	AVCodec *encoder;

	ofmt_ctx = NULL;
	avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
	if (!ofmt_ctx)
	{
		printf("ERROR: Could not create output context.\n");
		return -1;
	}

	for (int i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		out_stream = avformat_new_stream(ofmt_ctx, NULL);
		if (!out_stream)
		{
			printf("ERROR: Failed allocating output stream.\n");
			return -1;
		}

		in_stream = ifmt_ctx->streams[i];
		dec_ctx = in_stream->codec;
		enc_ctx = out_stream->codec;

		if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO || dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			encoder = avcodec_find_encoder(dec_ctx->codec_id);
			if (!encoder)
			{
				printf("ERROR: Necessary encoder not found.\n");
				return -1;
			}

			if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
			{
				enc_ctx = out_stream->codec = avcodec_alloc_context3(encoder);
				if (!enc_ctx)
				{
					printf("ERROR: Could not allocate video codec context.\n");
					exit(1);
				}
				src_h = dec_ctx->height;
				src_w = dec_ctx->width;
				dst_h = src_h / 2;
				dst_w = src_w / 2;
				enc_ctx->height = dst_h;
				enc_ctx->width = dst_w;
				enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
				enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
				enc_ctx->time_base = dec_ctx->time_base;
				enc_ctx->bit_rate = dec_ctx->bit_rate / 2;
				enc_ctx->gop_size = dec_ctx->gop_size;
			}
			else
			{
				enc_ctx->sample_rate = dec_ctx->sample_rate;
				enc_ctx->channel_layout = dec_ctx->channel_layout;
				enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
				enc_ctx->sample_fmt = encoder->sample_fmts[0];
				enc_ctx->time_base = (AVRational) { 1, enc_ctx->sample_rate };
			}

			if (avcodec_open2(enc_ctx, encoder, NULL) != 0)
			{
				printf("ERROR: Cannot open video encoder for stream #%u.\n", i);
				return -1;
			}
		}
		else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN)
		{
			printf("ERROR: Elementary stream #%d is of unknown type, cannot proceed.\n", i);
			return -1;
		}
		else
		{
			/* if this stream must be remuxed */
			if (avcodec_copy_context(ofmt_ctx->streams[i]->codec, ifmt_ctx->streams[i]->codec) != 0) {
				printf("ERROR: Copying stream context failed.\n");
				return -1;
			}
		}

		if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
		{
			enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}

	}
	av_dump_format(ofmt_ctx, 0, filename, 1);

	if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
	{
		if (avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE) < 0)
		{
			printf("ERROR: Could not open output file '%s'.\n", filename);
			return -1;
		}
	}

	/* init muxer, write output file header */
	if (avformat_write_header(ofmt_ctx, NULL) != 0)
	{
		printf("ERROR: Error occurred when opening output file.\n");
		return -1;
	}

	return 0;
}

// Return zero on success else nonzero
static int encode_write_frame(AVFrame *filt_frame, unsigned int stream_index, int *got_frame) {
	int ret;
	int got_frame_local;
	AVPacket enc_pkt;
	int(*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *) =
		(ifmt_ctx->streams[stream_index]->codec->codec_type ==
			AVMEDIA_TYPE_VIDEO) ? avcodec_encode_video2 : avcodec_encode_audio2;

	if (!got_frame)
	{
		got_frame = &got_frame_local;
	}

	/* encode filtered frame */
	enc_pkt.data = NULL;
	enc_pkt.size = 0;
	av_init_packet(&enc_pkt);
	ret = enc_func(ofmt_ctx->streams[stream_index]->codec, &enc_pkt, filt_frame, got_frame);
	av_frame_free(&filt_frame);
	if (ret < 0)
	{
		return ret;
	}
	if (!(*got_frame))
	{
		return 0;
	}

	/* prepare packet for muxing */
	enc_pkt.stream_index = stream_index;
	av_packet_rescale_ts(&enc_pkt,
		ofmt_ctx->streams[stream_index]->codec->time_base,
		ofmt_ctx->streams[stream_index]->time_base);

	/* mux encoded frame */
	ret = av_interleaved_write_frame(ofmt_ctx, &enc_pkt);
	return ret;
}

// Return zero on success else nonzero
static int flush_encoder(int stream_index)
{
	int ret;
	int got_frame;

	if (!(ofmt_ctx->streams[stream_index]->codec->codec->capabilities & AV_CODEC_CAP_DELAY))
	{
		return 0;
	}

	while (1)
	{
		ret = encode_write_frame(NULL, stream_index, &got_frame);
		if (ret != 0)
		{
			break;
		}
		if (!got_frame)
		{
			return 0;
		}
	}

	return ret;
}

// Return zero on success else nonzero
static int trimWithReencode(double from, double to)
{
	av_register_all();

	if (open_input_file(input_file) != 0)
	{
		return -1;
	}

	if (open_output_file(output_file) != 0)
	{
		return -1;
	}

	struct SwsContext *sws_ctx = sws_getContext(src_w, src_h, AV_PIX_FMT_YUV420P,
		dst_w, dst_h, AV_PIX_FMT_YUV420P,
		SWS_BILINEAR, NULL, NULL, NULL);
	if (!sws_ctx)
	{
		printf("ERROR: Could not create scaling context.\n");
		return 1;
	}

	enum AVMediaType type;
	AVPacket packet;
	AVFrame *frame = NULL;
	int ret = 0;

	// Seeking
	if (from != 0 && (ret = av_seek_frame(ifmt_ctx, -1, from * AV_TIME_BASE, AVSEEK_FLAG_ANY)) < 0)
	{
		printf("ERROR: Seeking failed.\n");
		goto END;
	}
	ret = 0;

	while (1)
	{
		if ((ret = av_read_frame(ifmt_ctx, &packet)) != 0)
		{
			break;
		}

		// Endpoint
		if (av_q2d(ifmt_ctx->streams[packet.stream_index]->time_base) * packet.pts > to)
		{
			av_packet_unref(&packet);
			break;
		}

		int stream_index = packet.stream_index;
		type = ifmt_ctx->streams[packet.stream_index]->codec->codec_type;
		av_packet_rescale_ts(&packet,
			ifmt_ctx->streams[stream_index]->time_base,
			ifmt_ctx->streams[stream_index]->codec->time_base);

		// Not reencode if type is audio
		if (type == AVMEDIA_TYPE_AUDIO)
		{
			av_interleaved_write_frame(ofmt_ctx, &packet);
			av_packet_unref(&packet);
			continue;
		}

		if (type == AVMEDIA_TYPE_VIDEO)
		{
			frame = av_frame_alloc();
			if (!frame)
			{
				ret = AVERROR(ENOMEM);
				break;
			}

			int got_frame;
			ret = avcodec_decode_video2(ifmt_ctx->streams[stream_index]->codec, frame, &got_frame, &packet);
			if (ret < 0)
			{
				av_frame_free(&frame);
				av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
				break;
			}

			if (got_frame)
			{
				// only scale video frame
				if (ifmt_ctx->streams[stream_index]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
				{
					// clone original frame
					AVFrame *scaled_frame = av_frame_alloc();
					scaled_frame->format = frame->format;
					scaled_frame->width = frame->width;
					scaled_frame->height = frame->height;
					scaled_frame->channels = frame->channels;
					scaled_frame->channel_layout = frame->channel_layout;
					scaled_frame->nb_samples = frame->nb_samples;
					av_frame_get_buffer(scaled_frame, 32);
					av_frame_copy(scaled_frame, frame);
					av_frame_copy_props(scaled_frame, frame);

					sws_scale(sws_ctx, frame->data, frame->linesize,
						0, src_h, scaled_frame->data, scaled_frame->linesize);

					av_frame_free(&frame);
					frame = scaled_frame;
				}

				// encode and write
				frame->pts = av_frame_get_best_effort_timestamp(frame);
				ret = encode_write_frame(frame, stream_index, NULL);

				if (ret < 0)
				{
					goto END;
				}
			}
			else
			{
				av_frame_free(&frame);
			}
			av_packet_unref(&packet);
		}
	}

	for (int i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		if ((ret = flush_encoder(i)) != 0)
		{
			printf("ERROR: Flushing encoder failed.\n");
			goto END;
		}
	}

	av_write_trailer(ofmt_ctx);

END:
	sws_freeContext(sws_ctx);
	av_packet_unref(&packet);
	for (int i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		avcodec_close(ifmt_ctx->streams[i]->codec);
		avcodec_close(ofmt_ctx->streams[i]->codec);
	}
	avformat_close_input(&ifmt_ctx);
	avio_closep(&ofmt_ctx->pb);
	avformat_free_context(ofmt_ctx);

	return ret;
}

// Return zero on success else nonzero
static int trimWithoutReencode(double from, double to)
{
	av_register_all();

	int ret;

	// Processing input file
	if ((ret = avformat_open_input(&ifmt_ctx, input_file, 0, 0)) != 0)
	{
		printf("ERROR: Could not open input file.\n");
		goto END;
	}
	if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0)
	{
		printf("ERROR: Failed to retrieve input stream information.\n");
		goto END;
	}
	ret = 0;

	av_dump_format(ifmt_ctx, 0, input_file, 0);
	// End processing input file

	// Processing output file
	if ((ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, output_file)) < 0)
	{
		printf("ERROR: Could not create output context.\n");
		goto END;
	}
	ret = 0;

	// Copy codec & metadata
	for (int i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		AVStream *in_stream = ifmt_ctx->streams[i];
		AVStream *out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
		if (!out_stream)
		{
			printf("ERROR: Failed allocating output stream.\n");
			ret = -1;
			goto END;
		}

		if ((ret = avcodec_copy_context(out_stream->codec, in_stream->codec)) != 0)
		{
			printf("ERROR: Failed to copy context from input to output stream codec context.\n");
			goto END;
		}

		out_stream->codec->codec_tag = 0;
		if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
		{
			out_stream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}

		// Copy metadata -> Fix rotate video bug
		AVDictionaryEntry *tag = NULL;
		while ((tag = av_dict_get(in_stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
		{
			av_dict_set(&out_stream->metadata, tag->key, tag->value, 0);
		}
	}
	av_dump_format(ofmt_ctx, 0, output_file, 1);

	if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
	{
		if ((ret = avio_open(&ofmt_ctx->pb, output_file, AVIO_FLAG_WRITE)) < 0)
		{
			printf("ERROR: Could not open output file.\n");
			goto END;
		}
		ret = 0;
	}

	if ((ret = avformat_write_header(ofmt_ctx, NULL)) != 0)
	{
		printf(stderr, "ERROR: Could not write header.\n");
		goto END;
	}
	// End processing output file

	// Seeking
	if (from != 0 && (ret = av_seek_frame(ifmt_ctx, -1, from * AV_TIME_BASE, AVSEEK_FLAG_ANY)) < 0)
	{
		printf("ERROR: Seeking failed.\n");
		goto END;
	}
	ret = 0;

	AVPacket pkt;
	while (1)
	{
		if ((ret = av_read_frame(ifmt_ctx, &pkt)) != 0)
		{
			printf("Could not read frame - error or end of file.\n");
			break;
		}

		AVStream *in_stream = ifmt_ctx->streams[pkt.stream_index];
		AVStream *out_stream = ofmt_ctx->streams[pkt.stream_index];

		// Endpoint
		if (av_q2d(in_stream->time_base) * pkt.pts > to)
		{
			av_packet_unref(&pkt);
			break;
		}

		pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
		pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
		pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
		pkt.pos = -1;

		if ((ret = av_interleaved_write_frame(ofmt_ctx, &pkt)) != 0)
		{
			printf("ERROR: Muxing failed.\n");
			break;
		}
		av_packet_unref(&pkt);
	}

	av_write_trailer(ofmt_ctx);

END:
	avformat_close_input(&ifmt_ctx);
	if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
	{
		avio_closep(&ofmt_ctx->pb);
	}
	avformat_free_context(ofmt_ctx);

	return ret;
}

// Return zero on success else nonzero
static int trim(const char *in, const char *out, const char *quality, double from, double to)
{
	input_file = in;
	output_file = out;

	if ((from == to && from == 0) || (from < to && to != 0))
	{
		if (strcmp(quality, "hd") == 0)
		{
			return trimWithoutReencode(from, to);
		}

		if (strcmp(quality, "sd") == 0)
		{
			return trimWithReencode(from, to);
		}
	}

	printf("ERROR: Invalid parameter.\n");
	return 1;
}

// Return zero on success else nonzero
int main(int argc, char **argv)
{
	int ret = trim(argv[1], argv[2], argv[3], atof(argv[4]), atof(argv[5]));
	system("result.mp4");
	return ret;
}