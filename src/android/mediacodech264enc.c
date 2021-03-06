/*
mediastreamer2 mediacodech264enc.c
Copyright (C) 2015 Belledonne Communications SARL

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "mediastreamer2/msfilter.h"
#include "mediastreamer2/rfc3984.h"
#include "mediastreamer2/msvideo.h"
#include "mediastreamer2/msticker.h"
#include "mediastreamer2/mscodecutils.h"
#include "mediastreamer2/msjava.h"
#include "android_mediacodec.h"
#include "h264utils.h"

#include <jni.h>
#include <media/NdkMediaCodec.h>

#include "ortp/b64.h"

#define TIMEOUT_US 0

#define MS_MEDIACODECH264_CONF(required_bitrate, bitrate_limit, resolution, fps, ncpus) \
	{ required_bitrate, bitrate_limit, { MS_VIDEO_SIZE_ ## resolution ## _W, MS_VIDEO_SIZE_ ## resolution ## _H }, fps, ncpus, NULL }

static const MSVideoConfiguration mediaCodecH264_conf_list[] = {
	MS_MEDIACODECH264_CONF(2048000, 	1000000,            UXGA, 25,  2),
	MS_MEDIACODECH264_CONF(1024000, 	5000000, 	  SXGA_MINUS, 25,  2),
	MS_MEDIACODECH264_CONF(1024000,  	5000000,   			720P, 30,  2),
	MS_MEDIACODECH264_CONF(750000, 	2048000,             XGA, 25,  2),
	MS_MEDIACODECH264_CONF(500000,  	1024000,            SVGA, 15,  2),
	MS_MEDIACODECH264_CONF(256000,  	 800000,             VGA, 15,  2),
	MS_MEDIACODECH264_CONF(128000,  	 512000,             CIF, 15,  1),
	MS_MEDIACODECH264_CONF(100000,  	 380000,            QVGA, 15,  1),
	MS_MEDIACODECH264_CONF(0,      170000,            QCIF, 10,  1),
};

typedef struct _EncData {
	AMediaCodec *codec;
	const MSVideoConfiguration *vconf_list;
	MSVideoConfiguration vconf;
	Rfc3984Context *packer;
	uint64_t framenum;
	int mode;
	MSVideoStarter starter;
	MSIFrameRequestsLimiterCtx iframe_limiter;
	mblk_t *sps, *pps; /*lastly generated SPS, PPS, in case we need to repeat them*/
	bool_t avpf_enabled;
	bool isYUV;
} EncData;

static void set_mblk(mblk_t **packet, mblk_t *newone) {
	if (newone) {
		newone = copyb(newone);
	}

	if (*packet) {
		freemsg(*packet);
	}

	*packet = newone;
}

static void enc_init(MSFilter *f) {
	MSVideoSize vsize;
	EncData *d = ms_new0(EncData, 1);

	d->packer = NULL;
	d->isYUV = TRUE;
	d->mode = 1;
	d->avpf_enabled = FALSE;

	d->framenum = 0;
	d->vconf_list = mediaCodecH264_conf_list;
	MS_VIDEO_SIZE_ASSIGN(vsize, CIF);
	d->vconf = ms_video_find_best_configuration_for_size(d->vconf_list, vsize, ms_factory_get_cpu_count(f->factory));

	f->data = d;
}

static void enc_preprocess(MSFilter *f) {
	AMediaCodec *codec;
	AMediaFormat *format;
	media_status_t status = AMEDIA_ERROR_UNSUPPORTED;
	EncData *d = (EncData *)f->data;

	d->packer = rfc3984_new();
	rfc3984_set_mode(d->packer, d->mode);
	rfc3984_enable_stap_a(d->packer, FALSE);
	ms_video_starter_init(&d->starter);
	ms_iframe_requests_limiter_init(&d->iframe_limiter, 1000);

	codec = AMediaCodec_createEncoderByType("video/avc");
	d->codec = codec;

	format = AMediaFormat_new();
	AMediaFormat_setString(format, "mime", "video/avc");
	AMediaFormat_setInt32(format, "width", d->vconf.vsize.width);
	AMediaFormat_setInt32(format, "height", d->vconf.vsize.height);
	AMediaFormat_setInt32(format, "i-frame-interval", 20);
	AMediaFormat_setInt32(format, "bitrate", d->vconf.required_bitrate);
	AMediaFormat_setInt32(format, "frame-rate", d->vconf.fps);
	AMediaFormat_setInt32(format, "bitrate-mode", 1);
	AMediaFormat_setInt32(format, "level", 1); // Ask for baseline AVC profile

	if (AMediaImage_isAvailable()) {
		if (status != 0) {
			AMediaFormat_setInt32(format, "color-format", 0x7f420888); /*the new "flexible YUV", appeared in API23*/
			status = AMediaCodec_configure(d->codec, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
		}
	} else {
		if (status != 0) {
			d->isYUV = FALSE;
			AMediaFormat_setInt32(format, "color-format", 21); /*the semi-planar YUV*/
			status = AMediaCodec_configure(d->codec, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
		}

		if (status != 0) {
			d->isYUV = TRUE;
			AMediaFormat_setInt32(format, "color-format", 19); /*basic YUV420P*/
			status = AMediaCodec_configure(d->codec, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
		}
	}

	if (status != 0) {
		ms_error("MSMediaCodecH264Enc: Could not configure encoder.");
		AMediaCodec_delete(d->codec);
		d->codec = NULL;
	} else {
		int32_t color_format;

		if (!AMediaFormat_getInt32(format, "color-format", &color_format)) {
			color_format = -1;
		}

		ms_message("MSMediaCodecH264Enc: encoder successfully configured. color-format=%d", color_format);
	}

	if (d->codec) {
		if (AMediaCodec_start(d->codec) != AMEDIA_OK) {
			ms_error("MSMediaCodecH264Enc: Could not start encoder.");
			AMediaCodec_delete(d->codec);
			d->codec = NULL;
		} else {
			ms_message("MSMediaCodecH264Enc: encoder successfully started");
		}
	}

	AMediaFormat_delete(format);
}

static void enc_postprocess(MSFilter *f) {
	EncData *d = (EncData *)f->data;
	rfc3984_destroy(d->packer);

	if (d->codec) {
		AMediaCodec_flush(d->codec);
		AMediaCodec_stop(d->codec);
		AMediaCodec_delete(d->codec);
	}

	set_mblk(&d->sps, NULL);
	set_mblk(&d->pps, NULL);
	d->packer = NULL;
}

static void enc_uninit(MSFilter *f) {
	EncData *d = (EncData *)f->data;

	ms_free(d);
}

static void enc_process(MSFilter *f) {
	EncData *d = (EncData *)f->data;
	MSPicture pic = {0};
	MSQueue nalus;
	mblk_t *im;
	long long int ts = f->ticker->time * 90LL;

	if (d->codec == NULL) {
		ms_queue_flush(f->inputs[0]);
		return;
	}

	ms_queue_init(&nalus);

	while ((im = ms_queue_get(f->inputs[0])) != NULL) {
		if (ms_yuv_buf_init_from_mblk(&pic, im) == 0) {
			AMediaCodecBufferInfo info;
			size_t bufsize;
			ssize_t ibufidx, obufidx;
			bool have_seen_sps_pps;

			if (ms_iframe_requests_limiter_iframe_requested(&d->iframe_limiter, f->ticker->time) ||
			        (d->avpf_enabled == FALSE && ms_video_starter_need_i_frame(&d->starter, f->ticker->time))) {
				/*Force a key-frame*/
				AMediaCodec_setParams(d->codec, "");
			}

			ibufidx = AMediaCodec_dequeueInputBuffer(d->codec, TIMEOUT_US);

			if (ibufidx >= 0) {
				if (AMediaImage_isAvailable()) {
					AMediaImage image;

					if (AMediaCodec_getInputImage(d->codec, ibufidx, &image)) {
						if (image.format == 35 /* YUV_420_888 */) {
							MSRect src_roi = {0, 0, pic.w, pic.h};
							int src_pix_strides[4] = {1, 1, 1, 1};
							ms_yuv_buf_copy_with_pix_strides(pic.planes, pic.strides, src_pix_strides, src_roi, image.buffers, image.row_strides, image.pixel_strides, image.crop_rect);
							AMediaImage_close(&image);
							AMediaCodec_queueInputBuffer(d->codec, ibufidx, 0, (size_t)image.width * image.height * 3 / 2, f->ticker->time * 1000, 0);
						} else {
							ms_error("%s: encoder require non YUV420 format", f->desc->name);
							AMediaImage_close(&image);
						}
					}
				} else {
					uint8_t *buf = AMediaCodec_getInputBuffer(d->codec, ibufidx, &bufsize);

					if (buf != NULL) {
						if (d->isYUV) {
							int ysize = pic.w * pic.h;
							int usize = ysize / 4;
							memcpy(buf, pic.planes[0], ysize);
							memcpy(buf + ysize, pic.planes[1], usize);
							memcpy(buf + ysize + usize, pic.planes[2], usize);
						} else {
							int i;
							size_t size = (size_t) pic.w * pic.h;
							uint8_t *dst = pic.planes[0];
							memcpy(buf, dst, size);

							for (i = 0; i < pic.w / 2 * pic.h / 2; i++) {
								buf[size + 2 * i] = pic.planes[1][i];
								buf[size + 2 * i + 1] = pic.planes[2][i];
							}
						}

						AMediaCodec_queueInputBuffer(d->codec, ibufidx, 0, (size_t)(pic.w * pic.h) * 3 / 2, f->ticker->time * 1000, 0);
					}
				}
			} else if (ibufidx == AMEDIA_ERROR_UNKNOWN) {
				ms_error("MSMediaCodecH264Enc: AMediaCodec_dequeueInputBuffer() had an exception");
			}

			have_seen_sps_pps = FALSE; /*this checks whether at a single timestamp point we dequeued SPS PPS before IDR*/

			while ((obufidx = AMediaCodec_dequeueOutputBuffer(d->codec, &info, TIMEOUT_US)) >= 0) {
				uint8_t *buf = AMediaCodec_getOutputBuffer(d->codec, obufidx, &bufsize);

				if (buf) {
					mblk_t *m;
					ms_h264_bitstream_to_nalus(buf, info.size, &nalus);

					if (!ms_queue_empty(&nalus)) {
						m = ms_queue_peek_first(&nalus);

						switch (ms_h264_nalu_get_type(m)) {
							case MSH264NaluTypeIDR:
								ms_iframe_requests_limiter_notify_iframe_sent(&d->iframe_limiter, f->ticker->time);

								if (!have_seen_sps_pps) {
									ms_message("MSMediaCodecH264Enc: seeing IDR without prior SPS/PPS, so manually adding them.");

									if (d->sps && d->pps) {
										ms_queue_insert(&nalus, m, copyb(d->sps));
										ms_queue_insert(&nalus, m, copyb(d->pps));
									} else {
										ms_error("MSMediaCodecH264Enc: SPS or PPS are not known !");
									}
								}

								break;

							case MSH264NaluTypeSPS:
								ms_iframe_requests_limiter_notify_iframe_sent(&d->iframe_limiter, f->ticker->time);
								ms_message("MSMediaCodecH264Enc: seeing SPS");
								have_seen_sps_pps = TRUE;
								set_mblk(&d->sps, m);
								m = ms_queue_next(&nalus, m);

								if (!ms_queue_end(&nalus, m) && ms_h264_nalu_get_type(m) == MSH264NaluTypePPS) {
									ms_message("MSMediaCodecH264Enc: seeing PPS");
									set_mblk(&d->pps, m);
								}

								break;

							case MSH264NaluTypePPS:
								ms_warning("MSMediaCodecH264Enc: unexpecting starting PPS");
								break;
						}

						rfc3984_pack(d->packer, &nalus, f->outputs[0], ts);

						if (d->framenum == 0) {
							ms_video_starter_first_frame(&d->starter, f->ticker->time);
						}

						d->framenum++;
					} else {
						ms_error("MSMediaCodecH264Enc: no NALUs in buffer obtained from MediaCodec");
					}
				}

				AMediaCodec_releaseOutputBuffer(d->codec, obufidx, FALSE);
			}

			if (obufidx == AMEDIA_ERROR_UNKNOWN) {
				ms_error("MSMediaCodecH264Enc: AMediaCodec_dequeueOutputBuffer() had an exception");
			}
		}

		freemsg(im);
	}

}

static int enc_get_br(MSFilter *f, void *arg) {
	EncData *d = (EncData *)f->data;
	*(int *)arg = d->vconf.required_bitrate;
	return 0;
}

static int enc_set_configuration(MSFilter *f, void *arg) {
	EncData *d = (EncData *)f->data;
	const MSVideoConfiguration *vconf = (const MSVideoConfiguration *)arg;

	if (vconf != &d->vconf) memcpy(&d->vconf, vconf, sizeof(MSVideoConfiguration));

	if (d->vconf.required_bitrate > d->vconf.bitrate_limit)
		d->vconf.required_bitrate = d->vconf.bitrate_limit;

	ms_message("Video configuration set: bitrate=%dbits/s, fps=%f, vsize=%dx%d", d->vconf.required_bitrate, d->vconf.fps, d->vconf.vsize.width, d->vconf.vsize.height);
	return 0;
}

static int enc_set_br(MSFilter *f, void *arg) {
	EncData *d = (EncData *)f->data;
	int br = *(int *)arg;

	if (d->codec != NULL) {
		/* Encoding is already ongoing, do not change video size, only bitrate. */
		d->vconf.required_bitrate = br;
		enc_set_configuration(f, &d->vconf);
	} else {
		MSVideoConfiguration best_vconf = ms_video_find_best_configuration_for_bitrate(d->vconf_list, br, ms_factory_get_cpu_count(f->factory));
		enc_set_configuration(f, &best_vconf);
	}

	return 0;
}

static int enc_set_fps(MSFilter *f, void *arg) {
	EncData *d = (EncData *)f->data;
	d->vconf.fps = *(float *)arg;
	enc_set_configuration(f, &d->vconf);
	return 0;
}

static int enc_get_fps(MSFilter *f, void *arg) {
	EncData *d = (EncData *)f->data;
	*(float *)arg = d->vconf.fps;
	return 0;
}

static int enc_get_vsize(MSFilter *f, void *arg) {
	EncData *d = (EncData *)f->data;
	*(MSVideoSize *)arg = d->vconf.vsize;
	return 0;
}

static int enc_enable_avpf(MSFilter *f, void *data) {
	EncData *s = (EncData *)f->data;
	s->avpf_enabled = *((bool_t *)data) ? TRUE : FALSE;
	return 0;
}

static int enc_set_vsize(MSFilter *f, void *arg) {
	MSVideoConfiguration best_vconf;
	EncData *d = (EncData *)f->data;
	MSVideoSize *vs = (MSVideoSize *)arg;

	best_vconf = ms_video_find_best_configuration_for_size(d->vconf_list, *vs, ms_factory_get_cpu_count(f->factory));
	d->vconf.vsize = *vs;
	d->vconf.fps = best_vconf.fps;
	d->vconf.bitrate_limit = best_vconf.bitrate_limit;
	enc_set_configuration(f, &d->vconf);
	return 0;
}

static int enc_notify_pli(MSFilter *f, void *data) {
	EncData *d = (EncData *)f->data;
	ms_message("MSMediaCodecH264Enc: PLI requested");
	ms_iframe_requests_limiter_request_iframe(&d->iframe_limiter);
	return 0;
}

static int enc_notify_fir(MSFilter *f, void *data) {
	EncData *d = (EncData *)f->data;
	ms_message("MSMediaCodecH264Enc: FIR requested");
	ms_iframe_requests_limiter_request_iframe(&d->iframe_limiter);
	return 0;
}

static MSFilterMethod  mediacodec_h264_enc_methods[] = {
	{ MS_FILTER_SET_FPS,                       enc_set_fps                },
	{ MS_FILTER_SET_BITRATE,                   enc_set_br                 },
	{ MS_FILTER_GET_BITRATE,                   enc_get_br                 },
	{ MS_FILTER_GET_FPS,                       enc_get_fps                },
	{ MS_FILTER_GET_VIDEO_SIZE,                enc_get_vsize              },
	{ MS_VIDEO_ENCODER_NOTIFY_PLI,             enc_notify_pli             },
	{ MS_VIDEO_ENCODER_NOTIFY_FIR,             enc_notify_fir             },
	{ MS_FILTER_SET_VIDEO_SIZE,                enc_set_vsize              },
	{ MS_VIDEO_ENCODER_ENABLE_AVPF,            enc_enable_avpf            },
	{ 0,                                       NULL                       }
};


MSFilterDesc ms_mediacodec_h264_enc_desc = {
	.id = MS_MEDIACODEC_H264_ENC_ID,
	.name = "MSMediaCodecH264Enc",
	.text = "A H264 encoder based on MediaCodec API.",
	.category = MS_FILTER_ENCODER,
	.enc_fmt = "H264",
	.ninputs = 1,
	.noutputs = 1,
	.init = enc_init,
	.preprocess = enc_preprocess,
	.process = enc_process,
	.postprocess = enc_postprocess,
	.uninit = enc_uninit,
	.methods = mediacodec_h264_enc_methods,
	.flags = MS_FILTER_IS_PUMP
};

