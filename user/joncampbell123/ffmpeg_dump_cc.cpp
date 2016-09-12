
#define __STDC_CONSTANT_MACROS

#include <sys/types.h>
#include <signal.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/pixelutils.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavformat/version.h>

#include <libswscale/swscale.h>
#include <libswscale/version.h>

#include <libswresample/swresample.h>
#include <libswresample/version.h>
}

using namespace std;

#include <map>
#include <string>
#include <vector>

volatile int DIE = 0;

void sigma(int x) {
	if (++DIE >= 20) abort();
}

int                     video_stream_index = 0;

string                  input_file;

AVFormatContext*        input_avfmt = NULL;
AVStream*               input_avstream_video = NULL;	// do not free
AVCodecContext*         input_avstream_video_codec_context = NULL; // do not free
AVFrame*                input_avstream_video_frame = NULL;

static int parse_argv(int argc,char **argv) {
	const char *a;
	int i;

	for (i=1;i < argc;) {
		a = argv[i++];

		if (*a == '-') {
			do { a++; } while (*a == '-');

			if (!strcmp(a,"i")) {
				input_file = argv[i++];
			}
			else {
				fprintf(stderr,"Unknown switch '%s'\n",a);
				return 1;
			}
		}
		else {
			fprintf(stderr,"Unhandled arg '%s'\n",a);
			return 1;
		}
	}

    if (input_file.empty()) {
		fprintf(stderr,"You must specify an input file (-i).\n");
		return 1;
	}

	return 0;
}

bool do_video_decode_and_render(AVPacket &pkt) {
    int got_frame = 0;

    if (avcodec_decode_video2(input_avstream_video_codec_context,input_avstream_video_frame,&got_frame,&pkt) >= 0 && got_frame) {
        /* we care about A53 caption data in this demo */
        AVFrameSideData *a53 = av_frame_get_side_data(input_avstream_video_frame,AV_FRAME_DATA_A53_CC);

        if (a53 != NULL) {
            /* FFMPEG sends us the caption data in CEA-708 format even if it came from DVD captions (EIA-608) format.
             * packet headers and CDP data are stripped, the data is a straight array of 708 3-byte data packets. */
            unsigned int i;
            unsigned int packets = a53->size / 3;
            const unsigned char *scan = a53->data;

            fprintf(stderr,"Caption data (%u bytes)\n",a53->size);
            if (a53->size % 3)
                fprintf(stderr,"A53 caption data warning, %u extra bytes\n",a53->size % 3);

            if (a53->data == NULL) {
                fprintf(stderr,"HEY! data==NULL\n");
                abort();
            }

            scan = a53->data;
            for (i=0;i < packets;i++,scan+=3) {
                if (*scan >= 0xF8) {
                    if (*scan >= 0xFE) {
                        // DTVCC CEA 708 data
                        printf("CEA-708 CC data 0x%02x%02x\n",scan[1],scan[2]);
                    }
                    else if (*scan >= 0xFC) {
                        // EIA-608 CC data
                        printf("EIA-608 CC data field=%u(%s) 0x%02x%02x\n",
                            *scan & 1,(*scan & 1) ? "even" : "odd",scan[1],scan[2]);
                    }
                }
                else {
                    fprintf(stderr,"  ---aborting packet decode, junk data\n");
                }
            }
        }
    }

    return (got_frame != 0);
}

int main(int argc,char **argv) {
	if (parse_argv(argc,argv))
		return 1;

	av_register_all();
	avformat_network_init();
	avcodec_register_all();

	assert(input_avfmt == NULL);
	if (avformat_open_input(&input_avfmt,input_file.c_str(),NULL,NULL) < 0) {
		fprintf(stderr,"Failed to open input file\n");
		return 1;
	}

	if (avformat_find_stream_info(input_avfmt,NULL) < 0)
		fprintf(stderr,"WARNING: Did not find stream info on input\n");

	/* scan streams for one video */
	{
		size_t i;
		AVStream *is;
		int ac=0,vc=0;
		AVCodecContext *isctx;

		fprintf(stderr,"Input format: %u streams found\n",input_avfmt->nb_streams);
		for (i=0;i < (size_t)input_avfmt->nb_streams;i++) {
			is = input_avfmt->streams[i];
			if (is == NULL) continue;

			isctx = is->codec;
			if (isctx == NULL) continue;

			if (isctx->codec_type == AVMEDIA_TYPE_VIDEO) {
				if (input_avstream_video == NULL && vc == video_stream_index) {
					if (avcodec_open2(isctx,avcodec_find_decoder(isctx->codec_id),NULL) >= 0) {
						input_avstream_video = is;
						input_avstream_video_codec_context = isctx;
						fprintf(stderr,"Found video stream idx=%zu\n",i);
					}
					else {
						fprintf(stderr,"Found video stream but not able to decode\n");
					}
				}

				vc++;
			}
		}

		if (input_avstream_video == NULL) {
			fprintf(stderr,"Video not found\n");
			return 1;
		}
	}

	/* soft break on CTRL+C */
	signal(SIGINT,sigma);
	signal(SIGHUP,sigma);
	signal(SIGQUIT,sigma);
	signal(SIGTERM,sigma);

	/* prepare video decoding */
	if (input_avstream_video != NULL) {
		input_avstream_video_frame = av_frame_alloc();
		if (input_avstream_video_frame == NULL) {
			fprintf(stderr,"Failed to alloc video frame\n");
			return 1;
		}
	}

	// PARSE
	{
        unsigned long long av_frame_counter = 0;
		unsigned long long video_field = 0;
        double adj_time = 0;
        int got_frame = 0;
        double t,pt = -1;
		AVPacket pkt;

		av_init_packet(&pkt);
		while (av_read_frame(input_avfmt,&pkt) >= 0) {
			if (DIE != 0) break;

			if (input_avstream_video != NULL && pkt.stream_index == input_avstream_video->index)
                do_video_decode_and_render(/*&*/pkt);

			av_packet_unref(&pkt);
			av_init_packet(&pkt);
		}

        av_packet_unref(&pkt);
        av_init_packet(&pkt);

        /* the encoder usually has a delay.
         * we need the encoder to flush those frames out. */
        {
            do {
                if (DIE != 0) break;

                pkt.size = 0;
                pkt.data = NULL;
                if (input_avstream_video != NULL)
                    got_frame = do_video_decode_and_render(/*&*/pkt) ? 1 : 0;
            } while (got_frame);
        }
	}

	if (input_avstream_video_frame != NULL)
		av_frame_free(&input_avstream_video_frame);
    if (input_avstream_video_codec_context != NULL) {
        avcodec_close(input_avstream_video_codec_context);
        input_avstream_video_codec_context = NULL;
        input_avstream_video = NULL;
    }
	avformat_close_input(&input_avfmt);
	return 0;
}

