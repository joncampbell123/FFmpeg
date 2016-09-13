
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

enum {
    FIELD608_ODD=0,
    FIELD608_EVEN=1
};

enum {
    CH608_NONE=-1,
    CH608_CC1,
    CH608_CC2,
    CH608_CC3,
    CH608_CC4,
    CH608_TEXT1,
    CH608_TEXT2,
    CH608_TEXT3,
    CH608_TEXT4,
    CH608_XDS
};

const char *choose_608_channel_name(int n) {
    switch (n) {
        case CH608_CC1:     return "CC1";
        case CH608_CC2:     return "CC2";
        case CH608_CC3:     return "CC3";
        case CH608_CC4:     return "CC4";
        case CH608_TEXT1:   return "TEXT1";
        case CH608_TEXT2:   return "TEXT2";
        case CH608_TEXT3:   return "TEXT3";
        case CH608_TEXT4:   return "TEXT4";
        case CH608_XDS:     return "XDS";
    };

    return "";
};

int                     choose_608_field = -1;
int                     choose_608_channel = -1;        // enum CH608_*
int                     choose_708_channel = -1;
bool                    verbose = false;

int                     video_stream_index = 0;

string                  input_file;

AVFormatContext*        input_avfmt = NULL;
AVStream*               input_avstream_video = NULL;	// do not free
AVCodecContext*         input_avstream_video_codec_context = NULL; // do not free
AVFrame*                input_avstream_video_frame = NULL;

enum {
    MODE608_NONE=0,
    MODE608_CC1,        // CC3 if second field
    MODE608_CC2,        // CC4 if second field
    MODE608_CC3,        // CC1 if first field
    MODE608_CC4,        // CC2 if first field
    MODE608_TEXT1,      // TEXT3 if second field
    MODE608_TEXT2,      // TEXT4 if second field
    MODE608_TEXT3,      // TEXT1 if first field
    MODE608_TEXT4,      // TEXT2 if first field
    MODE608_XDS         // CC3 XDS
};

static unsigned int     mode608 = MODE608_NONE;
static unsigned int     mode608_xds_return = MODE608_NONE; // what to return to when XDS finishes

static const char *mode608_str(void) {
    switch (mode608) {
        case MODE608_NONE:      return "(none)";
        case MODE608_CC1:       return "CC1";
        case MODE608_CC2:       return "CC2";
        case MODE608_CC3:       return "CC3";
        case MODE608_CC4:       return "CC4";
        case MODE608_TEXT1:     return "TEXT1";
        case MODE608_TEXT2:     return "TEXT2";
        case MODE608_TEXT3:     return "TEXT3";
        case MODE608_TEXT4:     return "TEXT4";
        case MODE608_XDS:       return "XDS";
    };

    return "";
}

static void on_608_cc_mode_watch(unsigned int ccword,bool evenfield) {
    if ((ccword&0x6000) != 0)
        return; // plain text/data non-control

    unsigned int pmode = mode608;

    if (mode608 == MODE608_XDS && (ccword&0x7F00) == 0x0F00) {
        // end of XDS packet
        mode608 = mode608_xds_return;
    }
    else if ((ccword&0x7070) == 0 && (ccword&0x0F0F) != 0) {
        // start of XDS packet. this is supposed to happen only on the even field
        if (mode608 != MODE608_XDS) {
            mode608_xds_return = mode608;
            mode608 = MODE608_XDS;
        }
    }
    else if (mode608 != MODE608_XDS) {
        // control word. bit 11 determines whether it goes to CC1/CC3 or CC2/CC4
        unsigned char cc2 = (ccword & 0x0800) ? 1 : 0;
        ccword &= ~0x0800; // filter out bit 11

        if (ccword == 0x1420/* Resume Caption Loading*/ ||
            (ccword >= 0x1425 && ccword <= 0x1427)/* Roll-up caption mode*/ ||
            ccword == 0x1429/* Resume Direct Captioning*/) {
            mode608 = (cc2 ? MODE608_CC2 : MODE608_CC1) + (evenfield ? 2 : 0);
        }
        else if (ccword == 0x142B/* Resume Text Display */) {
            mode608 = (cc2 ? MODE608_TEXT2 : MODE608_TEXT1) + (evenfield ? 2 : 0);
        }
    }

    if (mode608 != pmode) {
        if (verbose)
            fprintf(stderr,"CC processing has switched to mode %s\n",mode608_str());
    }
}

unsigned int cc_xpos = 0;

static void on_608_cc(unsigned int ccword,bool evenfield) {
    ccword &= 0x7F7F; /* strip parity bits */

    if (ccword == 0) return;

    if (verbose)
        fprintf(stderr,"Processing CC word 0x%04x (%s field)\n",ccword,evenfield?"even":"odd");

    on_608_cc_mode_watch(ccword,evenfield); // keep track of the decoder "mode" to make sure we pick out the right CC channel

    /* don't pay attention to the data unless it's the CC channel we want */
    if (choose_608_channel == CH608_CC1 && mode608 != MODE608_CC1)
        return;
    if (choose_608_channel == CH608_CC2 && mode608 != MODE608_CC2)
        return;
    if (choose_608_channel == CH608_CC3 && mode608 != MODE608_CC3)
        return;
    if (choose_608_channel == CH608_CC4 && mode608 != MODE608_CC4)
        return;
    if (choose_608_channel == CH608_TEXT1 && mode608 != MODE608_TEXT1)
        return;
    if (choose_608_channel == CH608_TEXT2 && mode608 != MODE608_TEXT2)
        return;
    if (choose_608_channel == CH608_TEXT3 && mode608 != MODE608_TEXT3)
        return;
    if (choose_608_channel == CH608_TEXT4 && mode608 != MODE608_TEXT4)
        return;

    if (verbose)
        fprintf(stderr,"Demuxed CC word is 0x%04x\n",ccword);

    // this is a very dumb simple decoder.
    // it may make some text harder to read, but it's a proof of concept that works for now.
    if ((ccword&0x6000) != 0) { // plain text
        fputc(ccword>>8,stdout);
        cc_xpos++;

        if ((ccword&0xFF) != 0) {
            fputc(ccword&0xFF,stdout);
            cc_xpos++;
        }

        fflush(stdout);
    }
    else {
        if (cc_xpos != 0) {
            cc_xpos = 0;
            fprintf(stdout,"\n");
        }
    }
}

static int parse_argv(int argc,char **argv) {
	const char *a;
	int i;

	for (i=1;i < argc;) {
		a = argv[i++];

		if (*a == '-') {
			do { a++; } while (*a == '-');

            if (!strcmp(a,"v")) {
                verbose = true;
            }
            else if (!strcmp(a,"i")) {
				input_file = argv[i++];
			}
            else if (!strcmp(a,"odd")) {
                choose_608_field = 0;
                choose_708_channel = -1;
            }
            else if (!strcmp(a,"even")) {
                choose_608_field = 0;
                choose_708_channel = -1;
            }
            else if (!strcmp(a,"dtv")) {
                choose_608_field = -1;
                choose_708_channel = 0;
            }
            else if (!strcmp(a,"cc1")) {
                choose_608_field = FIELD608_ODD;
                choose_608_channel = CH608_CC1;
            }
            else if (!strcmp(a,"cc2")) {
                choose_608_field = FIELD608_ODD;
                choose_608_channel = CH608_CC2;
            }
            else if (!strcmp(a,"cc3")) {
                choose_608_field = FIELD608_EVEN;
                choose_608_channel = CH608_CC3;
            }
            else if (!strcmp(a,"cc4")) {
                choose_608_field = FIELD608_EVEN;
                choose_608_channel = CH608_CC4;
            }
            else if (!strcmp(a,"text1")) {
                choose_608_field = FIELD608_ODD;
                choose_608_channel = CH608_TEXT1;
            }
            else if (!strcmp(a,"text2")) {
                choose_608_field = FIELD608_ODD;
                choose_608_channel = CH608_TEXT2;
            }
            else if (!strcmp(a,"text3")) {
                choose_608_field = FIELD608_EVEN;
                choose_608_channel = CH608_TEXT3;
            }
            else if (!strcmp(a,"text4")) {
                choose_608_field = FIELD608_EVEN;
                choose_608_channel = CH608_TEXT4;
            }
            else if (!strcmp(a,"xds")) {
                choose_608_field = FIELD608_EVEN;
                choose_608_channel = CH608_XDS;
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

    if (choose_608_field < 0 && choose_708_channel < 0) {
        // default 608
        choose_608_field = 0; // odd field, CC1/CC2, where most caption data resides
    }
    if (choose_608_channel < 0 && choose_608_field >= 0) {
        if (choose_608_field == 0)
            choose_608_channel = CH608_CC1;
        if (choose_608_field == 1)
            choose_608_channel = CH608_CC3;
    }

    printf("EIA-608: decoding field=%d channel=%s\n",
        choose_608_field,choose_608_channel_name(choose_608_channel));

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

            if (verbose)
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
                        if (verbose)
                            printf("CEA-708 CC data 0x%02x%02x\n",scan[1],scan[2]);
                    }
                    else if (*scan >= 0xFC) {
                        // EIA-608 CC data
                        if (verbose) {
                            printf("EIA-608 CC data field=%u(%s) 0x%02x%02x\n",
                                *scan & 1,(*scan & 1) ? "even" : "odd",scan[1],scan[2]);
                        }

                        if (choose_608_field == (*scan & 1) ? FIELD608_EVEN : FIELD608_ODD)
                            on_608_cc(((unsigned int)scan[1] << 8) + (unsigned int)scan[2],(*scan & 1)!=0/*even field*/);
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

