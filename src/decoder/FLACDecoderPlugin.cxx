/*
 * Copyright (C) 2003-2012 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h" /* must be first for large file support */
#include "FLACDecoderPlugin.h"
#include "FLACCommon.hxx"
#include "FLACMetaData.hxx"

extern "C" {
#include "ogg_codec.h"
}

#include <glib.h>

#include <assert.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#if !defined(FLAC_API_VERSION_CURRENT) || FLAC_API_VERSION_CURRENT <= 7
#error libFLAC is too old
#endif

/* this code was based on flac123, from flac-tools */

static FLAC__StreamDecoderReadStatus
flac_read_cb(G_GNUC_UNUSED const FLAC__StreamDecoder *fd,
	     FLAC__byte buf[], size_t *bytes,
	     void *fdata)
{
	struct flac_data *data = (struct flac_data *)fdata;
	size_t r;

	r = decoder_read(data->decoder, data->input_stream,
			 (void *)buf, *bytes);
	*bytes = r;

	if (r == 0) {
		if (decoder_get_command(data->decoder) != DECODE_COMMAND_NONE ||
		    input_stream_lock_eof(data->input_stream))
			return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
		else
			return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
	}

	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderSeekStatus
flac_seek_cb(G_GNUC_UNUSED const FLAC__StreamDecoder *fd,
	     FLAC__uint64 offset, void *fdata)
{
	struct flac_data *data = (struct flac_data *) fdata;

	if (!data->input_stream->seekable)
		return FLAC__STREAM_DECODER_SEEK_STATUS_UNSUPPORTED;

	if (!input_stream_lock_seek(data->input_stream, offset, SEEK_SET,
				    nullptr))
		return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;

	return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus
flac_tell_cb(G_GNUC_UNUSED const FLAC__StreamDecoder *fd,
	     FLAC__uint64 * offset, void *fdata)
{
	struct flac_data *data = (struct flac_data *) fdata;

	if (!data->input_stream->seekable)
		return FLAC__STREAM_DECODER_TELL_STATUS_UNSUPPORTED;

	*offset = (long)(data->input_stream->offset);

	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus
flac_length_cb(G_GNUC_UNUSED const FLAC__StreamDecoder *fd,
	       FLAC__uint64 * length, void *fdata)
{
	struct flac_data *data = (struct flac_data *) fdata;

	if (data->input_stream->size < 0)
		return FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;

	*length = (size_t) (data->input_stream->size);

	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool
flac_eof_cb(G_GNUC_UNUSED const FLAC__StreamDecoder *fd, void *fdata)
{
	struct flac_data *data = (struct flac_data *) fdata;

	return (decoder_get_command(data->decoder) != DECODE_COMMAND_NONE &&
		decoder_get_command(data->decoder) != DECODE_COMMAND_SEEK) ||
		input_stream_lock_eof(data->input_stream);
}

static void
flac_error_cb(G_GNUC_UNUSED const FLAC__StreamDecoder *fd,
	      FLAC__StreamDecoderErrorStatus status, void *fdata)
{
	flac_error_common_cb(status, (struct flac_data *) fdata);
}

static void flacPrintErroredState(FLAC__StreamDecoderState state)
{
	switch (state) {
	case FLAC__STREAM_DECODER_SEARCH_FOR_METADATA:
	case FLAC__STREAM_DECODER_READ_METADATA:
	case FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC:
	case FLAC__STREAM_DECODER_READ_FRAME:
	case FLAC__STREAM_DECODER_END_OF_STREAM:
		return;

	case FLAC__STREAM_DECODER_OGG_ERROR:
	case FLAC__STREAM_DECODER_SEEK_ERROR:
	case FLAC__STREAM_DECODER_ABORTED:
	case FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR:
	case FLAC__STREAM_DECODER_UNINITIALIZED:
		break;
	}

	g_warning("%s\n", FLAC__StreamDecoderStateString[state]);
}

static void flacMetadata(G_GNUC_UNUSED const FLAC__StreamDecoder * dec,
			 const FLAC__StreamMetadata * block, void *vdata)
{
	flac_metadata_common_cb(block, (struct flac_data *) vdata);
}

static FLAC__StreamDecoderWriteStatus
flac_write_cb(const FLAC__StreamDecoder *dec, const FLAC__Frame *frame,
	      const FLAC__int32 *const buf[], void *vdata)
{
	struct flac_data *data = (struct flac_data *) vdata;
	FLAC__uint64 nbytes = 0;

	if (FLAC__stream_decoder_get_decode_position(dec, &nbytes)) {
		if (data->position > 0 && nbytes > data->position) {
			nbytes -= data->position;
			data->position += nbytes;
		} else {
			data->position = nbytes;
			nbytes = 0;
		}
	} else
		nbytes = 0;

	return flac_common_write(data, frame, buf, nbytes);
}

static bool
flac_scan_file(const char *file,
	       const struct tag_handler *handler, void *handler_ctx)
{
	return flac_scan_file2(file, handler, handler_ctx);
}

/**
 * Some glue code around FLAC__stream_decoder_new().
 */
static FLAC__StreamDecoder *
flac_decoder_new(void)
{
	FLAC__StreamDecoder *sd = FLAC__stream_decoder_new();
	if (sd == nullptr) {
		g_warning("FLAC__stream_decoder_new() failed");
		return nullptr;
	}

	if(!FLAC__stream_decoder_set_metadata_respond(sd, FLAC__METADATA_TYPE_VORBIS_COMMENT))
		g_debug("FLAC__stream_decoder_set_metadata_respond() has failed");

	return sd;
}

static bool
flac_decoder_initialize(struct flac_data *data, FLAC__StreamDecoder *sd,
			FLAC__uint64 duration)
{
	data->total_frames = duration;

	if (!FLAC__stream_decoder_process_until_end_of_metadata(sd)) {
		g_warning("problem reading metadata");
		return false;
	}

	if (data->initialized) {
		/* done */
		decoder_initialized(data->decoder, &data->audio_format,
				    data->input_stream->seekable,
				    (float)data->total_frames /
				    (float)data->audio_format.sample_rate);
		return true;
	}

	if (data->input_stream->seekable)
		/* allow the workaround below only for nonseekable
		   streams*/
		return false;

	/* no stream_info packet found; try to initialize the decoder
	   from the first frame header */
	FLAC__stream_decoder_process_single(sd);
	return data->initialized;
}

static void
flac_decoder_loop(struct flac_data *data, FLAC__StreamDecoder *flac_dec,
		  FLAC__uint64 t_start, FLAC__uint64 t_end)
{
	struct decoder *decoder = data->decoder;
	enum decoder_command cmd;

	data->first_frame = t_start;

	while (true) {
		if (data->tag != nullptr && !tag_is_empty(data->tag)) {
			cmd = decoder_tag(data->decoder, data->input_stream,
					  data->tag);
			tag_free(data->tag);
			data->tag = tag_new();
		} else
			cmd = decoder_get_command(decoder);

		if (cmd == DECODE_COMMAND_SEEK) {
			FLAC__uint64 seek_sample = t_start +
				decoder_seek_where(decoder) *
				data->audio_format.sample_rate;
			if (seek_sample >= t_start &&
			    (t_end == 0 || seek_sample <= t_end) &&
			    FLAC__stream_decoder_seek_absolute(flac_dec, seek_sample)) {
				data->next_frame = seek_sample;
				data->position = 0;
				decoder_command_finished(decoder);
			} else
				decoder_seek_error(decoder);
		} else if (cmd == DECODE_COMMAND_STOP ||
			   FLAC__stream_decoder_get_state(flac_dec) == FLAC__STREAM_DECODER_END_OF_STREAM)
			break;

		if (t_end != 0 && data->next_frame >= t_end)
			/* end of this sub track */
			break;

		if (!FLAC__stream_decoder_process_single(flac_dec) &&
		    decoder_get_command(decoder) == DECODE_COMMAND_NONE) {
			/* a failure that was not triggered by a
			   decoder command */
			flacPrintErroredState(FLAC__stream_decoder_get_state(flac_dec));
			break;
		}
	}
}

static FLAC__StreamDecoderInitStatus
stream_init_oggflac(FLAC__StreamDecoder *flac_dec, struct flac_data *data)
{
	return FLAC__stream_decoder_init_ogg_stream(flac_dec,
						    flac_read_cb,
						    flac_seek_cb,
						    flac_tell_cb,
						    flac_length_cb,
						    flac_eof_cb,
						    flac_write_cb,
						    flacMetadata,
						    flac_error_cb,
						    data);
}

static FLAC__StreamDecoderInitStatus
stream_init_flac(FLAC__StreamDecoder *flac_dec, struct flac_data *data)
{
	return FLAC__stream_decoder_init_stream(flac_dec,
						flac_read_cb, flac_seek_cb,
						flac_tell_cb, flac_length_cb,
						flac_eof_cb, flac_write_cb,
						flacMetadata,
						flac_error_cb,
						data);
}

static FLAC__StreamDecoderInitStatus
stream_init(FLAC__StreamDecoder *flac_dec, struct flac_data *data, bool is_ogg)
{
	return is_ogg
		? stream_init_oggflac(flac_dec, data)
		: stream_init_flac(flac_dec, data);
}

static void
flac_decode_internal(struct decoder * decoder,
		     struct input_stream *input_stream,
		     bool is_ogg)
{
	FLAC__StreamDecoder *flac_dec;
	struct flac_data data;

	flac_dec = flac_decoder_new();
	if (flac_dec == nullptr)
		return;

	flac_data_init(&data, decoder, input_stream);
	data.tag = tag_new();

	FLAC__StreamDecoderInitStatus status =
		stream_init(flac_dec, &data, is_ogg);
	if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		flac_data_deinit(&data);
		FLAC__stream_decoder_delete(flac_dec);
		g_warning("%s", FLAC__StreamDecoderInitStatusString[status]);
		return;
	}

	if (!flac_decoder_initialize(&data, flac_dec, 0)) {
		flac_data_deinit(&data);
		FLAC__stream_decoder_finish(flac_dec);
		FLAC__stream_decoder_delete(flac_dec);
		return;
	}

	flac_decoder_loop(&data, flac_dec, 0, 0);

	flac_data_deinit(&data);

	FLAC__stream_decoder_finish(flac_dec);
	FLAC__stream_decoder_delete(flac_dec);
}

static void
flac_decode(struct decoder * decoder, struct input_stream *input_stream)
{
	flac_decode_internal(decoder, input_stream, false);
}

static bool
oggflac_init(G_GNUC_UNUSED const struct config_param *param)
{
	return !!FLAC_API_SUPPORTS_OGG_FLAC;
}

static bool
oggflac_scan_file(const char *file,
		  const struct tag_handler *handler, void *handler_ctx)
{
	FLACMetadataChain chain;
	if (!chain.ReadOgg(file)) {
		g_debug("Failed to read OggFLAC tags: %s",
			chain.GetStatusString());
		return false;
	}

	FLACMetadataIterator iterator(chain);
	do {
		FLAC__StreamMetadata *block = iterator.GetBlock();
		if (block == nullptr)
			break;

		flac_scan_metadata(block, handler, handler_ctx);
	} while (iterator.Next());

	return true;
}

static void
oggflac_decode(struct decoder *decoder, struct input_stream *input_stream)
{
	if (ogg_codec_detect(decoder, input_stream) != OGG_CODEC_FLAC)
		return;

	/* rewind the stream, because ogg_codec_detect() has
	   moved it */
	input_stream_lock_seek(input_stream, 0, SEEK_SET, nullptr);

	flac_decode_internal(decoder, input_stream, true);
}

static const char *const oggflac_suffixes[] = { "ogg", "oga", nullptr };
static const char *const oggflac_mime_types[] = {
	"application/ogg",
	"application/x-ogg",
	"audio/ogg",
	"audio/x-flac+ogg",
	"audio/x-ogg",
	nullptr
};

const struct decoder_plugin oggflac_decoder_plugin = {
	"oggflac",
	oggflac_init,
	nullptr,
	oggflac_decode,
	nullptr,
	oggflac_scan_file,
	nullptr,
	nullptr,
	oggflac_suffixes,
	oggflac_mime_types,
};

static const char *const flac_suffixes[] = { "flac", nullptr };
static const char *const flac_mime_types[] = {
	"application/flac",
	"application/x-flac",
	"audio/flac",
	"audio/x-flac",
	nullptr
};

const struct decoder_plugin flac_decoder_plugin = {
	"flac",
	nullptr,
	nullptr,
	flac_decode,
	nullptr,
	flac_scan_file,
	nullptr,
	nullptr,
	flac_suffixes,
	flac_mime_types,
};