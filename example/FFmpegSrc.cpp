#include "FFmpegSrc.h"
#include "Extension/AAC.h"
#include "Extension/H264.h"

extern "C"
{
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
#include "libavdevice/avdevice.h"
#include "libavcodec/avcodec.h"
};

FFmpegSrc::FFmpegSrc(const string& vhost,
    const string& strApp,
    const string& strId,
    float dur_sec,
    bool bEanbleRtsp,
    bool bEanbleRtmp ,
    bool bEanbleHls ,
    bool bEnableMp4)
{
	m_Src.reset(new MultiMediaSourceMuxer(vhost,strApp,strId,dur_sec,bEanbleRtsp,bEanbleRtmp,bEanbleHls,bEnableMp4));
    m_Src->addTrack(std::make_shared<H264Track>());
    m_bStart.store(false);
}

FFmpegSrc::~FFmpegSrc()
{
}

void FFmpegSrc::inputH264(const char* pcData, int iDataLen, uint32_t dts, uint32_t pts)
{
    if (dts == 0) {
        dts = (uint32_t)m_aTicker[0].elapsedTime();
    }
    if (pts == 0) {
        pts = dts;
    }
    int prefixeSize;
    if (memcmp("\x00\x00\x00\x01", pcData, 4) == 0) {
        prefixeSize = 4;
    }
    else if (memcmp("\x00\x00\x01", pcData, 3) == 0) {
        prefixeSize = 3;
    }
    else {
        prefixeSize = 0;
    }
    m_Src->inputFrame(std::make_shared<H264FrameNoCacheAble>((char*)pcData, iDataLen, dts, pts, prefixeSize));
}

void FFmpegSrc::Start()
{
    m_bStart.store(true);
    m_pThread.reset(new std::thread([this]() {
        ThreadEntry();
        }));
}

void FFmpegSrc::Stop()
{
    m_bStart.store(false);
}

void FFmpegSrc::ThreadEntry()
{
	//av_register_all();
	avcodec_register_all();
	avfilter_register_all();
	av_log_set_level(AV_LOG_QUIET);

	int ret;
	int in_width = 640;
	int in_height = 480;
	std::atomic<int64_t> encode_count;
	encode_count.store(0);

	AVCodec* pCodecH264 = NULL; //������
	AVCodecContext* pCodeCtx = NULL;

	//����h264������
	pCodecH264 = avcodec_find_encoder(AV_CODEC_ID_H264);
	if (!pCodecH264)
	{
		fprintf(stderr, "h264 codec not found\n");
		return;
	}

	pCodeCtx = avcodec_alloc_context3(pCodecH264);
	pCodeCtx->bit_rate = 300000;// put sample parameters 
	pCodeCtx->width = in_width;// 
	pCodeCtx->height = in_height;// 

	// frames per second 
	AVRational rate;
	rate.num = 1;
	rate.den = 25;
	AVRational framerate;
	framerate.num = 25;
	framerate.den = 1;
	pCodeCtx->time_base = rate;//(AVRational){1,25};
	pCodeCtx->gop_size = 25; // emit one intra frame
	pCodeCtx->framerate = framerate;
	pCodeCtx->max_b_frames = 0;
	pCodeCtx->thread_count = 1;
	pCodeCtx->pix_fmt = AV_PIX_FMT_YUV420P;//PIX_FMT_RGB24;
	//pCodeCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	av_opt_set(pCodeCtx->priv_data, "preset", "slow", 0);
	av_opt_set(pCodeCtx->priv_data, "tune", "zerolatency", 0);

	//av_opt_set(c->priv_data, /*"preset"*/"libvpx-1080p.ffpreset", /*"slow"*/NULL, 0);
	//�򿪱�����
	if (avcodec_open2(pCodeCtx, pCodecH264, NULL) < 0)
	{
		printf("���ܴ򿪱����");
		return ;
	}

	while (m_bStart.load())
	{
		auto time1 = std::chrono::steady_clock::now();

		char sz_filter_descr[256] = { 0 };
		time_t lt = time(NULL);
		struct tm* tm_ptr = localtime(&lt);
		strftime(sz_filter_descr, 256, "drawtext=fontfile=simsunb.ttf:fontcolor=red:fontsize=50:text='osd %Y/%m/%d %H/%M/%S'", tm_ptr);
		//strncpy(sz_filter_descr,filter_descr,256);

		char args[512];
		AVFilterContext* buffersink_ctx;
		AVFilterContext* buffersrc_ctx;
		AVFilterGraph* filter_graph;
		const AVFilter* buffersrc = avfilter_get_by_name("buffer");
		const AVFilter* buffersink = avfilter_get_by_name("buffersink");
		AVFilterInOut* outputs = avfilter_inout_alloc();
		AVFilterInOut* inputs = avfilter_inout_alloc();
		enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
		AVBufferSinkParams* buffersink_params;

		filter_graph = avfilter_graph_alloc();

		/* buffer video source: the decoded frames from the decoder will be inserted here. */
		snprintf(args, sizeof(args),
			"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
			in_width, in_height, AV_PIX_FMT_YUV420P,
			1, 25, 1, 1);

		ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
			args, NULL, filter_graph);
		if (ret < 0) {
			printf("Cannot create buffer source\n");
			return ;
		}

		/* buffer video sink: to terminate the filter chain. */
		buffersink_params = av_buffersink_params_alloc();
		buffersink_params->pixel_fmts = pix_fmts;
		ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
			NULL, buffersink_params, filter_graph);
		av_free(buffersink_params);
		if (ret < 0) {
			printf("Cannot create buffer sink\n");
			return ;
		}

		/* Endpoints for the filter graph. */
		outputs->name = av_strdup("in");
		outputs->filter_ctx = buffersrc_ctx;
		outputs->pad_idx = 0;
		outputs->next = NULL;

		inputs->name = av_strdup("out");
		inputs->filter_ctx = buffersink_ctx;
		inputs->pad_idx = 0;
		inputs->next = NULL;

		if ((ret = avfilter_graph_parse_ptr(filter_graph, sz_filter_descr,
			&inputs, &outputs, NULL)) < 0)
			return ;

		if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
			return ;

		AVFrame* frame_in;
		AVFrame* frame_out;
		unsigned char* frame_buffer_in;
		//unsigned char *frame_buffer_out;

		frame_in = av_frame_alloc();
		frame_buffer_in = (unsigned char*)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, in_width, in_height, 1));
		memset(frame_buffer_in, 0, av_image_get_buffer_size(AV_PIX_FMT_YUV420P, in_width, in_height, 1));
		av_image_fill_arrays(frame_in->data, frame_in->linesize, frame_buffer_in,
			AV_PIX_FMT_YUV420P, in_width, in_height, 1);

		frame_out = av_frame_alloc();

		frame_in->width = in_width;
		frame_in->height = in_height;
		frame_in->format = AV_PIX_FMT_YUV420P;

		//input Y,U,V
		frame_in->data[0] = frame_buffer_in;
		frame_in->data[1] = frame_buffer_in + in_width * in_height;
		frame_in->data[2] = frame_buffer_in + in_width * in_height * 5 / 4;

		if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame_in, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
			printf("Error while add frame.\n");
			break;
		}

		AVPacket* avpkt = av_packet_alloc();
		/* pull filtered pictures from the filtergraph */
		while (1) {
			ret = av_buffersink_get_frame(buffersink_ctx, frame_out);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			if (ret < 0)
				return ;
			int u_size = 0;
			//h264����
			avcodec_send_frame(pCodeCtx, frame_out);
			u_size = avcodec_receive_packet(pCodeCtx, avpkt);

			if (u_size == 0)
			{
				AVPacket* pkt = avpkt;
				if (pkt && pkt->size != 0)
				{
					if (pkt->data[4] == 0x65) //0x67:sps ,0x65:IDR
					{
						uint8_t* extraData = pCodeCtx->extradata;
						uint8_t extraDatasize = pCodeCtx->extradata_size;
						uint8_t* sps = extraData + 4;
						uint8_t* pps = nullptr;
						for (uint8_t* p = sps; p < (extraData + extraDatasize - 4); p++) {
							if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
								if (p[4] == 0x68) {
									pps = p + 4;
									break;
								}
							}
						}
						if (pps == nullptr) {
							std::cout << " get pps error " << std::endl;
						}
						int sps_size = pps - sps - 4;
						int pps_size = extraDatasize - sps_size - 8;
						inputH264((char*)sps,sps_size);
						inputH264((char*)pps, pps_size);

						inputH264((char*)pkt->data, pkt->size);
					}else{
						inputH264((char*)pkt->data, pkt->size);
					}
				}
			}
		}

		if (avpkt)
		{
			av_packet_unref(avpkt);
			av_packet_free(&avpkt);
			avpkt = nullptr;
		}

		av_frame_unref(frame_out);
		av_frame_unref(frame_in);

		av_free(frame_buffer_in);
		//av_free(frame_buffer_out);

		av_frame_free(&frame_in);
		av_frame_free(&frame_out);

		avfilter_inout_free(&inputs);
		avfilter_inout_free(&outputs);
		avfilter_free(buffersrc_ctx);
		avfilter_free(buffersink_ctx);
		avfilter_graph_free(&filter_graph);


		auto time2 = std::chrono::steady_clock::now();
		auto du = std::chrono::duration_cast<std::chrono::milliseconds>(time2 - time1);
		//std::cout << "code time  " << du.count() << std::endl;
		if (du.count() < 40) {
			std::this_thread::sleep_for(std::chrono::milliseconds(40 - du.count()));
		}
		else {
		}
		encode_count++;
	}

	avcodec_close(pCodeCtx);


}
