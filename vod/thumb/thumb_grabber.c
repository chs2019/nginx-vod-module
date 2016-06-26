#include "thumb_grabber.h"
#include "../media_set.h"

#if (VOD_HAVE_LIB_AV_CODEC)
#include <libavcodec/avcodec.h>

// typedefs
typedef struct
{
	// fixed
	request_context_t* request_context;
	write_callback_t write_callback;
	void* write_context;

	// libavcodec
	AVCodecContext *decoder;
	AVCodecContext *encoder;
	AVFrame *decoded_frame;
	AVPacket output_packet;

	// frame state
	frame_list_part_t cur_frame_part;
	input_frame_t* cur_frame;
	uint32_t skip_count;
	bool_t first_time;
	bool_t frame_started;
	uint64_t dts;
	uint32_t missing_frames;

	// frame buffer state
	uint32_t max_frame_size;
	u_char* frame_buffer;
	uint32_t cur_frame_pos;

} thumb_grabber_state_t;

typedef struct {
	uint32_t codec_id;
	enum AVCodecID av_codec_id;
	const char* name;
} codec_id_mapping_t;

// globals
static AVCodec *decoder_codec[VOD_CODEC_ID_COUNT];
static AVCodec *encoder_codec = NULL;

static codec_id_mapping_t codec_mappings[] = {
	{ VOD_CODEC_ID_AVC, AV_CODEC_ID_H264, "h264" },
	{ VOD_CODEC_ID_HEVC, AV_CODEC_ID_H265, "h265" },
	{ VOD_CODEC_ID_VP8, AV_CODEC_ID_VP8, "vp8" },
	{ VOD_CODEC_ID_VP9, AV_CODEC_ID_VP9, "vp9" },
};

void
thumb_grabber_process_init(vod_log_t* log)
{
	AVCodec *cur_decoder_codec;
	codec_id_mapping_t* mapping_cur;
	codec_id_mapping_t* mapping_end;

	avcodec_register_all();

	vod_memzero(decoder_codec, sizeof(decoder_codec));

	encoder_codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
	if (encoder_codec == NULL)
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"thumb_grabber_process_init: failed to get jpeg encoder, thumbnail capture is disabled");
		return;
	}

	mapping_end = codec_mappings + vod_array_entries(codec_mappings);
	for (mapping_cur = codec_mappings; mapping_cur < mapping_end; mapping_cur++)
	{
		cur_decoder_codec = avcodec_find_decoder(mapping_cur->av_codec_id);
		if (cur_decoder_codec == NULL)
		{
			vod_log_error(VOD_LOG_WARN, log, 0,
				"thumb_grabber_process_init: failed to get %s decoder, thumbnail capture is disabled for this codec", 
				mapping_cur->name);
			continue;
		}

		decoder_codec[mapping_cur->codec_id] = cur_decoder_codec;
	}
}

static void
thumb_grabber_free_state(void* context)
{
	thumb_grabber_state_t* state = (thumb_grabber_state_t*)context;

	av_packet_unref(&state->output_packet);
	av_frame_free(&state->decoded_frame);
	avcodec_close(state->encoder);
	av_free(state->encoder);
	avcodec_close(state->decoder);
	av_free(state->decoder);
}

static vod_status_t
thumb_grabber_init_decoder(
	request_context_t* request_context,
	media_info_t* media_info,
	AVCodecContext** result)
{
	AVCodecContext *decoder;
	int avrc;

	decoder = avcodec_alloc_context3(decoder_codec[media_info->codec_id]);
	if (decoder == NULL) 
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_decoder: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	*result = decoder;

	decoder->codec_tag = media_info->format;
	decoder->time_base.num = 1;
	decoder->time_base.den = media_info->frames_timescale;
	decoder->pkt_timebase = decoder->time_base;
	decoder->extradata = media_info->extra_data.data;
	decoder->extradata_size = media_info->extra_data.len;
	decoder->width = media_info->u.video.width;
	decoder->height = media_info->u.video.height;

	avrc = avcodec_open2(decoder, decoder_codec[media_info->codec_id], NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_decoder: avcodec_open2 failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

static vod_status_t
thumb_grabber_init_encoder(
	request_context_t* request_context, 
	media_info_t* media_info, 
	AVCodecContext** result)
{
	AVCodecContext *encoder;
	int avrc;

	encoder = avcodec_alloc_context3(encoder_codec);
	if (encoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_encoder: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	*result = encoder;

	encoder->width = media_info->u.video.width;
	encoder->height = media_info->u.video.height;
	encoder->time_base = (AVRational){ 1, 1 };
	encoder->pix_fmt = AV_PIX_FMT_YUVJ420P;

	avrc = avcodec_open2(encoder, encoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_encoder: avcodec_open2 failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

uint32_t
thumb_grabber_get_max_frame_size(media_track_t* track, uint32_t limit)
{
	frame_list_part_t* part;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint32_t max_frame_size = 0;

	part = &track->frames;
	last_frame = part->last_frame;
	for (cur_frame = part->first_frame; limit > 0; cur_frame++, limit--)
	{
		if (cur_frame >= last_frame)
		{
			part = part->next;
			cur_frame = part->first_frame;
			last_frame = part->last_frame;
		}

		if (cur_frame->size > max_frame_size)
		{
			max_frame_size = cur_frame->size;
		}
	}

	return max_frame_size;
}

static vod_status_t
thumb_grabber_truncate_frames(
	request_context_t* request_context,
	media_track_t* track, 
	uint64_t requested_time, 
	uint32_t* skip_count)
{
	frame_list_part_t* last_key_frame_part = NULL;
	frame_list_part_t* min_part = NULL;
	frame_list_part_t* part;
	input_frame_t* last_key_frame = NULL;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint64_t dts = track->clip_start_time + track->first_frame_time_offset;
	uint64_t pts;
	uint64_t cur_diff;
	uint64_t min_diff = ULLONG_MAX;
	uint32_t last_key_frame_index;
	uint32_t min_index = 0;
	uint32_t index;

	part = &track->frames;
	last_frame = part->last_frame;
	cur_frame = part->first_frame;

	requested_time += cur_frame->pts_delay;

	for (;; cur_frame++, index++)
	{
		if (cur_frame >= last_frame)
		{
			if (part->next == NULL)
			{
				break;
			}
			part = part->next;
			cur_frame = part->first_frame;
			last_frame = part->last_frame;
		}

		// update last_key_frame
		if (cur_frame->key_frame)
		{
			last_key_frame_index = index;
			last_key_frame = cur_frame;
			last_key_frame_part = part;
		}

		// find the closest frame
		pts = dts + cur_frame->pts_delay;
		cur_diff = (pts >= requested_time) ? (pts - requested_time) : (requested_time - pts);
		if (cur_diff < min_diff && last_key_frame != NULL)
		{
			min_index = index - last_key_frame_index;
			min_diff = cur_diff;
			min_part = last_key_frame_part;

			// truncate any frames before the key frame of the closest frame
			min_part->first_frame = last_key_frame;
		}

		dts += cur_frame->duration;
	}

	if (min_part == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_truncate_frames: did not find any frames");
		return VOD_UNEXPECTED;
	}

	// truncate any parts before the key frame of the closest frame
	track->frames = *min_part;

	*skip_count = min_index;

	return VOD_OK;
}

vod_status_t
thumb_grabber_init_state(
	request_context_t* request_context,
	media_track_t* track, 
	uint64_t requested_time,
	write_callback_t write_callback,
	void* write_context,
	void** result)
{
	thumb_grabber_state_t* state;
	vod_pool_cleanup_t *cln;
	vod_status_t rc;
	uint32_t frame_index;

	if (decoder_codec[track->media_info.codec_id] == NULL)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"thumb_grabber_init_state: no decoder was initialized for codec %uD", track->media_info.codec_id);
		return VOD_BAD_REQUEST;
	}

	rc = thumb_grabber_truncate_frames(request_context, track, requested_time, &frame_index);
	if (rc != VOD_OK)
	{
		return rc;
	}

	vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
		"thumb_grabber_init_state: frame index is %uD", frame_index);

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"thumb_grabber_init_state: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	// clear all ffmpeg members, so that they will be initialized in case init fails
	state->decoded_frame = NULL;
	state->decoder = NULL;
	state->encoder = NULL;
	av_init_packet(&state->output_packet);
	state->output_packet.data = NULL;
	state->output_packet.size = 0;

	// add to the cleanup pool
	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"thumb_grabber_init_state: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = thumb_grabber_free_state;
	cln->data = state;

	rc = thumb_grabber_init_decoder(request_context, &track->media_info, &state->decoder);
	if (rc != VOD_OK)
	{
		return rc;
	}

	rc = thumb_grabber_init_encoder(request_context, &track->media_info, &state->encoder);
	if (rc != VOD_OK)
	{
		return rc;
	}

	state->decoded_frame = av_frame_alloc();
	if (state->decoded_frame == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"thumb_grabber_init_state: av_frame_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->request_context = request_context;
	state->write_callback = write_callback;
	state->write_context = write_context;
	state->cur_frame_part = track->frames;
	state->cur_frame = track->frames.first_frame;
	state->max_frame_size = thumb_grabber_get_max_frame_size(track, frame_index + 1);
	state->frame_buffer = NULL;
	state->skip_count = frame_index;
	state->cur_frame_pos = 0;
	state->first_time = TRUE;
	state->frame_started = FALSE;
	state->missing_frames = 0;
	state->dts = 0;

	*result = state;

	return VOD_OK;
}

static vod_status_t
thumb_grabber_decode_flush(thumb_grabber_state_t* state)
{
	AVPacket input_packet;
	int got_frame;
	int avrc;

	vod_memzero(&input_packet, sizeof(input_packet));

	for (; state->missing_frames > 0; state->missing_frames--)
	{
		avrc = avcodec_decode_video2(state->decoder, state->decoded_frame, &got_frame, &input_packet);
		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"thumb_grabber_decode_flush: avcodec_decode_video2 failed %d", avrc);
			return VOD_BAD_DATA;
		}

		if (!got_frame)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"thumb_grabber_decode_flush: avcodec_decode_video2 did not return a frame");
			return VOD_UNEXPECTED;
		}
	}

	return VOD_OK;
}

static vod_status_t 
thumb_grabber_decode_frame(thumb_grabber_state_t* state, u_char* buffer)
{
	input_frame_t* frame = state->cur_frame;
	AVPacket input_packet;
	u_char original_pad[VOD_BUFFER_PADDING_SIZE];
	u_char* frame_end;
	int got_frame;
	int avrc;
	
	vod_memzero(&input_packet, sizeof(input_packet));
	input_packet.data = buffer;
	input_packet.size = frame->size;
	input_packet.dts = state->dts;
	input_packet.pts = state->dts + frame->pts_delay;
	input_packet.duration = frame->duration;
	input_packet.flags = frame->key_frame ? AV_PKT_FLAG_KEY : 0;
	state->dts += frame->duration;
	
	av_frame_unref(state->decoded_frame);

	got_frame = 0;

	frame_end = buffer + frame->size;
	vod_memcpy(original_pad, frame_end, sizeof(original_pad));
	vod_memzero(frame_end, sizeof(original_pad));

	avrc = avcodec_decode_video2(state->decoder, state->decoded_frame, &got_frame, &input_packet);
	if (avrc < 0) 
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_decode_frame: avcodec_decode_video2 failed %d", avrc);
		return VOD_BAD_DATA;
	}

	vod_memcpy(frame_end, original_pad, sizeof(original_pad));

	if (!got_frame)
	{
		state->missing_frames++;
	}

	return VOD_OK;
}

static vod_status_t
thumb_grabber_write_frame(thumb_grabber_state_t* state)
{
	vod_status_t rc;
	int got_packet = 0;
	int avrc;

	if (state->missing_frames > 0)
	{
		rc = thumb_grabber_decode_flush(state);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	avrc = avcodec_encode_video2(state->encoder, &state->output_packet, state->decoded_frame, &got_packet);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_write_frame: avcodec_encode_video2 failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	if (!got_packet)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"thumb_grabber_write_frame: avcodec_encode_video2 did not return a packet");
		return VOD_UNEXPECTED;
	}

	rc = state->write_callback(state->write_context, state->output_packet.data, state->output_packet.size);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

vod_status_t
thumb_grabber_process(void* context)
{
	thumb_grabber_state_t* state = context;
	u_char* read_buffer;
	uint32_t read_size;
	bool_t processed_data = FALSE;
	vod_status_t rc;
	bool_t frame_done;

	for (;;)
	{
		// start a frame if needed
		if (!state->frame_started)
		{
			if (state->cur_frame >= state->cur_frame_part.last_frame)
			{
				state->cur_frame_part = *state->cur_frame_part.next;
				state->cur_frame = state->cur_frame_part.first_frame;
			}

			// start the frame
			rc = state->cur_frame_part.frames_source->start_frame(
				state->cur_frame_part.frames_source_context,
				state->cur_frame,
				ULLONG_MAX);
			if (rc != VOD_OK)
			{
				return rc;
			}

			state->frame_started = TRUE;
		}

		// read some data from the frame
		rc = state->cur_frame_part.frames_source->read(
			state->cur_frame_part.frames_source_context,
			&read_buffer,
			&read_size,
			&frame_done);
		if (rc != VOD_OK)
		{
			if (rc != VOD_AGAIN)
			{
				return rc;
			}

			if (!processed_data && !state->first_time)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"thumb_grabber_process: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			state->first_time = FALSE;
			return VOD_AGAIN;
		}

		processed_data = TRUE;

		if (!frame_done)
		{
			// didn't finish the frame, append to the frame buffer
			if (state->frame_buffer == NULL)
			{
				state->frame_buffer = vod_alloc(
					state->request_context->pool, 
					state->max_frame_size + VOD_BUFFER_PADDING_SIZE);
				if (state->frame_buffer == NULL)
				{
					vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
						"thumb_grabber_process: vod_alloc failed");
					return VOD_ALLOC_FAILED;
				}
			}

			vod_memcpy(state->frame_buffer + state->cur_frame_pos, read_buffer, read_size);
			state->cur_frame_pos += read_size;
			continue;
		}

		if (state->cur_frame_pos != 0)
		{
			// copy the remainder
			vod_memcpy(state->frame_buffer + state->cur_frame_pos, read_buffer, read_size);
			state->cur_frame_pos = 0;
			read_buffer = state->frame_buffer;
		}

		// decode the frame
		rc = thumb_grabber_decode_frame(state, read_buffer);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// if the target frame was reached, write it
		if (state->skip_count <= 0)
		{
			return thumb_grabber_write_frame(state);
		}

		state->skip_count--;

		// move to the next frame
		state->cur_frame++;
		state->frame_started = FALSE;
	}
}

#endif // VOD_HAVE_LIB_AV_CODEC
