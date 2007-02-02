/* the Music Player Daemon (MPD)
 * (c)2003-2006 by Warren Dukes (warren.dukes@gmail.com)
 * This project's homepage is: http://www.musicpd.org
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "pcm_utils.h"

#include "mpd_types.h"
#include "log.h"
#include "utils.h"
#include "conf.h"

#include <string.h>
#include <math.h>
#include <assert.h>

#ifdef HAVE_LIBSAMPLERATE
#include <samplerate.h>
#endif

void pcm_volumeChange(char *buffer, int bufferSize, AudioFormat * format,
		      int volume)
{
	mpd_sint32 temp32;
	mpd_sint8 *buffer8 = (mpd_sint8 *) buffer;
	mpd_sint16 *buffer16 = (mpd_sint16 *) buffer;

	if (volume >= 1000)
		return;

	if (volume <= 0) {
		memset(buffer, 0, bufferSize);
		return;
	}

	switch (format->bits) {
	case 16:
		while (bufferSize > 0) {
			temp32 = *buffer16;
			temp32 *= volume;
			temp32 += rand() & 511;
			temp32 -= rand() & 511;
			temp32 += 500;
			temp32 /= 1000;
			*buffer16 = temp32 > 32767 ? 32767 :
			    (temp32 < -32768 ? -32768 : temp32);
			buffer16++;
			bufferSize -= 2;
		}
		break;
	case 8:
		while (bufferSize > 0) {
			temp32 = *buffer8;
			temp32 *= volume;
			temp32 += rand() & 511;
			temp32 -= rand() & 511;
			temp32 += 500;
			temp32 /= 1000;
			*buffer8 = temp32 > 127 ? 127 :
			    (temp32 < -128 ? -128 : temp32);
			buffer8++;
			bufferSize--;
		}
		break;
	default:
		ERROR("%i bits not supported by pcm_volumeChange!\n",
		      format->bits);
		exit(EXIT_FAILURE);
	}
}

static void pcm_add(char *buffer1, char *buffer2, size_t bufferSize1,
		    size_t bufferSize2, int vol1, int vol2,
		    AudioFormat * format)
{
	mpd_sint32 temp32;
	mpd_sint8 *buffer8_1 = (mpd_sint8 *) buffer1;
	mpd_sint8 *buffer8_2 = (mpd_sint8 *) buffer2;
	mpd_sint16 *buffer16_1 = (mpd_sint16 *) buffer1;
	mpd_sint16 *buffer16_2 = (mpd_sint16 *) buffer2;

	switch (format->bits) {
	case 16:
		while (bufferSize1 > 0 && bufferSize2 > 0) {
			temp32 =
			    (vol1 * (*buffer16_1) +
			     vol2 * (*buffer16_2));
			temp32 += rand() & 511;
			temp32 -= rand() & 511;
			temp32 += 500;
			temp32 /= 1000;
			*buffer16_1 =
			    temp32 > 32767 ? 32767 : (temp32 <
						      -32768 ? -32768 : temp32);
			buffer16_1++;
			buffer16_2++;
			bufferSize1 -= 2;
			bufferSize2 -= 2;
		}
		if (bufferSize2 > 0)
			memcpy(buffer16_1, buffer16_2, bufferSize2);
		break;
	case 8:
		while (bufferSize1 > 0 && bufferSize2 > 0) {
			temp32 =
			    (vol1 * (*buffer8_1) + vol2 * (*buffer8_2));
			temp32 += rand() & 511;
			temp32 -= rand() & 511;
			temp32 += 500;
			temp32 /= 1000;
			*buffer8_1 =
			    temp32 > 127 ? 127 : (temp32 <
						  -128 ? -128 : temp32);
			buffer8_1++;
			buffer8_2++;
			bufferSize1--;
			bufferSize2--;
		}
		if (bufferSize2 > 0)
			memcpy(buffer8_1, buffer8_2, bufferSize2);
		break;
	default:
		ERROR("%i bits not supported by pcm_add!\n", format->bits);
		exit(EXIT_FAILURE);
	}
}

void pcm_mix(char *buffer1, char *buffer2, size_t bufferSize1,
	     size_t bufferSize2, AudioFormat * format, float portion1)
{
	int vol1;
	float s = sin(M_PI_2 * portion1);
	s *= s;

	vol1 = s * 1000 + 0.5;
	vol1 = vol1 > 1000 ? 1000 : (vol1 < 0 ? 0 : vol1);

	pcm_add(buffer1, buffer2, bufferSize1, bufferSize2, vol1, 1000 - vol1,
		format);
}

#ifdef HAVE_LIBSAMPLERATE
static int pcm_getSamplerateConverter(void) {
	const char *conf, *test;
	int convalgo = SRC_SINC_FASTEST;
	int newalgo;
	size_t len;
 
	conf = getConfigParamValue(CONF_SAMPLERATE_CONVERTER);
	if(conf) {
		newalgo = strtol(conf, (char **)&test, 10);
		if(*test) {
			len = strlen(conf);
			for(newalgo = 0; ; newalgo++) {
				test = src_get_name(newalgo);
				if(!test)
					break; /* FAIL */
				if(!strncasecmp(test, conf, len)) {
					convalgo = newalgo;
					break;
				}
			}
		} else {
			if(src_get_name(newalgo))
				convalgo = newalgo;
			/* else FAIL */
		}
	}
	DEBUG("Selecting samplerate converter '%s'\n", src_get_name(convalgo));
	return convalgo;
}
#endif

/* outFormat bits must be 16 and channels must be 2! */
void pcm_convertAudioFormat(AudioFormat * inFormat, char *inBuffer, size_t
			    inSize, AudioFormat * outFormat, char *outBuffer)
{
	static char *bitConvBuffer;
	static int bitConvBufferLength;
	static char *channelConvBuffer;
	static int channelConvBufferLength;
	char *dataChannelConv;
	int dataChannelLen;
	char *dataBitConv;
	int dataBitLen;

	assert(outFormat->bits == 16);
	assert(outFormat->channels == 2 || outFormat->channels == 1);

	/* converts */
	switch (inFormat->bits) {
	case 8:
		dataBitLen = inSize << 1;
		if (dataBitLen > bitConvBufferLength) {
			bitConvBuffer = xrealloc(bitConvBuffer, dataBitLen);
			bitConvBufferLength = dataBitLen;
		}
		dataBitConv = bitConvBuffer;
		{
			mpd_sint8 *in = (mpd_sint8 *) inBuffer;
			mpd_sint16 *out = (mpd_sint16 *) dataBitConv;
			int i;
			for (i = 0; i < inSize; i++) {
				*out++ = (*in++) << 8;
			}
		}
		break;
	case 16:
		dataBitConv = inBuffer;
		dataBitLen = inSize;
		break;
	case 24:
		/* put dithering code from mp3_decode here */
	default:
		ERROR("only 8 or 16 bits are supported for conversion!\n");
		exit(EXIT_FAILURE);
	}

	/* converts only between 16 bit audio between mono and stereo */
	if (inFormat->channels == outFormat->channels) {
		dataChannelConv = dataBitConv;
		dataChannelLen = dataBitLen;
	} else {
		switch (inFormat->channels) {
			/* convert from 1 -> 2 channels */
		case 1:
			dataChannelLen = (dataBitLen >> 1) << 2;
			if (dataChannelLen > channelConvBufferLength) {
				channelConvBuffer = xrealloc(channelConvBuffer,
							    dataChannelLen);
				channelConvBufferLength = dataChannelLen;
			}
			dataChannelConv = channelConvBuffer;
			{
				mpd_sint16 *in = (mpd_sint16 *) dataBitConv;
				mpd_sint16 *out =
				    (mpd_sint16 *) dataChannelConv;
				int i, inSamples = dataBitLen >> 1;
				for (i = 0; i < inSamples; i++) {
					*out++ = *in;
					*out++ = *in++;
				}
			}
			break;
			/* convert from 2 -> 1 channels */
		case 2:
			dataChannelLen = dataBitLen >> 1;
			if (dataChannelLen > channelConvBufferLength) {
				channelConvBuffer = xrealloc(channelConvBuffer,
							    dataChannelLen);
				channelConvBufferLength = dataChannelLen;
			}
			dataChannelConv = channelConvBuffer;
			{
				mpd_sint16 *in = (mpd_sint16 *) dataBitConv;
				mpd_sint16 *out =
				    (mpd_sint16 *) dataChannelConv;
				int i, inSamples = dataBitLen >> 2;
				for (i = 0; i < inSamples; i++) {
					*out = (*in++) / 2;
					*out++ += (*in++) / 2;
				}
			}
			break;
		default:
			ERROR
			    ("only 1 or 2 channels are supported for conversion!\n");
			exit(EXIT_FAILURE);
		}
	}

	if (inFormat->sampleRate == outFormat->sampleRate) {
		memcpy(outBuffer, dataChannelConv, dataChannelLen);
	} else {
#ifdef HAVE_LIBSAMPLERATE
		static SRC_STATE *state = NULL;
		static SRC_DATA data;
		int error;
		static double ratio = 0;
		double newratio;

		if(!state) {
			state = src_new(pcm_getSamplerateConverter(), outFormat->channels, &error);
			if(!state) {
				ERROR("Cannot create new samplerate state: %s\n", src_strerror(error));
				exit(EXIT_FAILURE);
			} else {
				DEBUG("Samplerate converter initialized\n");
			}
		}

		newratio = (double)outFormat->sampleRate / (double)inFormat->sampleRate;
		if(newratio != ratio) {
			src_set_ratio(state, ratio = newratio);
			DEBUG("Setting samplerate conversion ratio to %.2lf\n", ratio);
		}

		data.input_frames = dataChannelLen / 2 / outFormat->channels;
		data.output_frames = pcm_sizeOfOutputBufferForAudioFormatConversion(inFormat, dataChannelLen, outFormat) / 2 / outFormat->channels;
		data.src_ratio = (double)data.output_frames / (double)data.input_frames;

		float conversionInBuffer[data.input_frames * outFormat->channels];
		float conversionOutBuffer[data.output_frames * outFormat->channels];
		data.data_in = conversionInBuffer;
		data.data_out = conversionOutBuffer;

		src_short_to_float_array((short *)dataChannelConv, data.data_in, data.input_frames * outFormat->channels);
		error = src_process(state, &data);
		if(error) {
			ERROR("Cannot process samples: %s\n", src_strerror(error));
			exit(EXIT_FAILURE);
		}

		src_float_to_short_array(data.data_out, (short *)outBuffer, data.output_frames * outFormat->channels);
#else
		/* only works if outFormat is 16-bit stereo! */
		/* resampling code blatantly ripped from ESD */
		mpd_uint32 rd_dat = 0;
		mpd_uint32 wr_dat = 0;
		mpd_sint16 lsample, rsample;
		mpd_sint16 *out = (mpd_sint16 *) outBuffer;
		mpd_sint16 *in = (mpd_sint16 *) dataChannelConv;
		mpd_uint32 nlen = pcm_sizeOfOutputBufferForAudioFormatConversion(inFormat, inSize, outFormat) / sizeof(mpd_sint16);

		switch (outFormat->channels) {
		case 1:
			while (wr_dat < nlen) {
				rd_dat = wr_dat * inFormat->sampleRate /
				    outFormat->sampleRate;

				lsample = in[rd_dat++];

				out[wr_dat++] = lsample;
			}
			break;
		case 2:
			while (wr_dat < nlen) {
				rd_dat = wr_dat * inFormat->sampleRate /
				    outFormat->sampleRate;
				rd_dat &= ~1;

				lsample = in[rd_dat++];
				rsample = in[rd_dat++];

				out[wr_dat++] = lsample;
				out[wr_dat++] = rsample;
			}
			break;
		}
#endif
	}

	return;
}

size_t pcm_sizeOfOutputBufferForAudioFormatConversion(AudioFormat * inFormat,
						      size_t inSize,
						      AudioFormat * outFormat)
{
	const int shift = sizeof(mpd_sint16) * outFormat->channels;
	size_t outSize = inSize;

	switch (inFormat->bits) {
	case 8:
		outSize = outSize << 1;
		break;
	case 16:
		break;
	default:
		ERROR("only 8 or 16 bits are supported for conversion!\n");
		exit(EXIT_FAILURE);
	}

	if (inFormat->channels != outFormat->channels) {
		switch (inFormat->channels) {
		case 1:
			outSize = (outSize >> 1) << 2;
			break;
		case 2:
			outSize >>= 1;
			break;
		}
	}

	outSize /=  shift;
	outSize = floor(0.5 + (double)outSize *
		((double)outFormat->sampleRate / (double)inFormat->sampleRate));
	outSize *= shift;

	return outSize;
}
