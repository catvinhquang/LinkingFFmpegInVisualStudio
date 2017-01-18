//#pragma warning (disable : 4996)
//
//#include <libavutil/timestamp.h>
//#include <libavformat/avformat.h>
//
//int main(int argc, char **argv)
//{
//	const char *in_filename = argv[1];
//	const char *out_filename = argv[2];
//
//	float from_seconds;
//	float end_seconds;
//	sscanf(argv[3], "%f", &from_seconds);
//	sscanf(argv[4], "%f", &end_seconds);
//
//	AVOutputFormat *ofmt = NULL;
//	AVFormatContext *ifmt_ctx = NULL;
//	AVFormatContext *ofmt_ctx = NULL;
//	AVPacket pkt;
//	int ret, i;
//	float time_base;
//
//	av_register_all();
//
//	// input
//	if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0)
//	{
//		fprintf(stderr, "Could not open input file '%s'", in_filename);
//		goto END;
//	}
//	if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0)
//	{
//		fprintf(stderr, "Failed to retrieve input stream information");
//		goto END;
//	}
//	av_dump_format(ifmt_ctx, 0, in_filename, 0);
//
//	// output
//	avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
//	if (!ofmt_ctx)
//	{
//		fprintf(stderr, "Could not create output context\n");
//		ret = AVERROR_UNKNOWN;
//		goto END;
//	}
//	ofmt = ofmt_ctx->oformat;
//	for (i = 0; i < ifmt_ctx->nb_streams; i++)
//	{
//		AVStream *in_stream = ifmt_ctx->streams[i];
//		AVStream *out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
//		if (!out_stream)
//		{
//			fprintf(stderr, "Failed allocating output stream\n");
//			ret = AVERROR_UNKNOWN;
//			goto END;
//		}
//
//		ret = avcodec_copy_context(out_stream->codec, in_stream->codec);
//		if (ret < 0)
//		{
//			fprintf(stderr, "Failed to copy context from input to output stream codec context\n");
//			goto END;
//		}
//		out_stream->codec->codec_tag = 0;
//		if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
//		{
//			out_stream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
//		}
//	}
//	av_dump_format(ofmt_ctx, 0, out_filename, 1);
//
//	if (!(ofmt->flags & AVFMT_NOFILE))
//	{
//		ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
//		if (ret < 0)
//		{
//			fprintf(stderr, "Could not open output file '%s'", out_filename);
//			goto END;
//		}
//	}
//
//	ret = avformat_write_header(ofmt_ctx, NULL);
//	if (ret < 0)
//	{
//		fprintf(stderr, "Error occurred when opening output file\n");
//		goto END;
//	}
//
//	// seek
//	ret = av_seek_frame(ifmt_ctx, -1, from_seconds * AV_TIME_BASE, AVSEEK_FLAG_ANY);
//	if (ret < 0)
//	{
//		fprintf(stderr, "Seeking failed\n");
//		goto END;
//	}
//
//	while (1)
//	{
//		AVStream *in_stream, *out_stream;
//
//		ret = av_read_frame(ifmt_ctx, &pkt);
//		if (ret < 0)
//		{
//			break;
//		}
//
//		// endpoint
//		if (av_q2d(ifmt_ctx->streams[pkt.stream_index]->time_base) * pkt.pts > end_seconds)
//		{
//			av_packet_unref(&pkt);
//			break;
//		}
//
//		in_stream = ifmt_ctx->streams[pkt.stream_index];
//		out_stream = ofmt_ctx->streams[pkt.stream_index];
//
//		// log_packet(ifmt_ctx, &pkt, "in");
//
//		// copy packet
//		time_base = in_stream->time_base.den / in_stream->time_base.num;
//		pkt.pts = av_rescale_q_rnd(pkt.pts - from_seconds * time_base, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
//		pkt.dts = av_rescale_q_rnd(pkt.dts - from_seconds * time_base, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
//		pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
//		// pkt.pos = -1;
//		// log_packet(ofmt_ctx, &pkt, "out");
//
//		ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
//		if (ret < 0)
//		{
//			fprintf(stderr, "Error muxing packet\n");
//			break;
//		}
//		av_packet_unref(&pkt);
//	}
//
//	av_write_trailer(ofmt_ctx);
//
//END:
//	avformat_close_input(&ifmt_ctx);
//	if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
//	{
//		avio_closep(&ofmt_ctx->pb);
//	}
//	avformat_free_context(ofmt_ctx);
//
//	if (ret < 0 && ret != AVERROR_EOF)
//	{
//		fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
//		return 1;
//	}
//
//	system("result.mp4");
//	return 0;
//}
