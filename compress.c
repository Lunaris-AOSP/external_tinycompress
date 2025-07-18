/*
 * BSD LICENSE
 *
 * tinycompress library for compress audio offload in alsa
 * Copyright (c) 2011-2012, Intel Corporation
 * All rights reserved.
 *
 * Author: Vinod Koul <vinod.koul@linux.intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * Neither the name of Intel Corporation nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 * LGPL LICENSE
 *
 * tinycompress library for compress audio offload in alsa
 * Copyright (c) 2011-2012, Intel Corporation.
 *
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to
 * the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <limits.h>

#include <linux/types.h>
#include <linux/ioctl.h>
#define __force
#define __bitwise
#define __user
#include <sound/asound.h>
#include "sound/compress_params.h"
#include "sound/compress_offload.h"
#include "tinycompress/tinycompress.h"
#include "compress_ops.h"
#include "snd_utils.h"

#define COMPR_ERR_MAX 128

/* Default maximum time we will wait in a poll() - 20 seconds */
#define DEFAULT_MAX_POLL_WAIT_MS    20000

struct compress {
	int fd;
	unsigned int flags;
	char error[COMPR_ERR_MAX];
	struct compr_config *config;
	int running;
	int max_poll_wait_ms;
	int nonblocking;
	unsigned int gapless_metadata;
	unsigned int next_track;

	struct compress_ops *ops;
	void *data;
	void *snd_node;
};

extern struct compress_ops compr_hw_ops;
extern struct compress_ops compr_plug_ops;

static int oops(struct compress *compress, int e, const char *fmt, ...)
{
	va_list ap;
	int sz;

	va_start(ap, fmt);
	vsnprintf(compress->error, COMPR_ERR_MAX, fmt, ap);
	va_end(ap);
	sz = strlen(compress->error);

	snprintf(compress->error + sz, COMPR_ERR_MAX - sz,
		": %s", strerror(e));
	errno = e;

	return -1;
}

const char *compress_get_error(struct compress *compress)
{
	return compress->error;
}
static struct compress bad_compress = {
	.fd = -1,
};

int is_compress_running(struct compress *compress)
{
	return ((compress->fd >= 0) && compress->running) ? 1 : 0;
}

int is_compress_ready(struct compress *compress)
{
	return (compress->fd >= 0) ? 1 : 0;
}

static int get_compress_version(struct compress *compress)
{
	int version = 0;

	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_IOCTL_VERSION, &version)) {
		oops(compress, errno, "cant read version");
		return -1;
	}
	return version;
}

static bool _is_codec_type_supported(struct compress_ops *ops, void *data,
		struct snd_codec *codec)
{
	struct snd_compr_caps caps;
	bool found = false;
	unsigned int i;

	if (ops->ioctl(data, SNDRV_COMPRESS_GET_CAPS, &caps)) {
		oops(&bad_compress, errno, "cannot get device caps");
		return false;
	}

	for (i = 0; i < caps.num_codecs; i++) {
		if (caps.codecs[i] == codec->id) {
			/* found the codec */
			found = true;
			break;
		}
	}
	/* TODO: match the codec properties */
	return found;
}

static inline void
fill_compress_params(struct compr_config *config, struct snd_compr_params *params)
{
	params->buffer.fragment_size = config->fragment_size;
	params->buffer.fragments = config->fragments;
	memcpy(&params->codec, config->codec, sizeof(params->codec));
}

struct compress *compress_open(unsigned int card, unsigned int device,
		unsigned int flags, struct compr_config *config)
{
	struct compress *compress;
	struct snd_compr_params params;
	struct snd_compr_caps caps;
	int compress_type;

	if (!config) {
		oops(&bad_compress, EINVAL, "passed bad config");
		return &bad_compress;
	}

	compress = calloc(1, sizeof(struct compress));
	if (!compress) {
		oops(&bad_compress, errno, "cannot allocate compress object");
		return &bad_compress;
	}

	compress->next_track = 0;
	compress->gapless_metadata = 0;
	compress->config = calloc(1, sizeof(*config));
	if (!compress->config)
		goto input_fail;

	compress->max_poll_wait_ms = DEFAULT_MAX_POLL_WAIT_MS;

	compress->flags = flags;
	if (!((flags & COMPRESS_OUT) || (flags & COMPRESS_IN))) {
		oops(&bad_compress, EINVAL, "can't deduce device direction from given flags");
		goto config_fail;
	}

	compress->snd_node = snd_utils_get_dev_node(card, device, NODE_COMPRESS);
	compress_type = snd_utils_get_node_type(compress->snd_node);
	if (compress_type == SND_NODE_TYPE_PLUGIN)
		compress->ops = &compr_plug_ops;
	else
		compress->ops = &compr_hw_ops;

	compress->fd = compress->ops->open(card, device, flags,
									   &compress->data, compress->snd_node);
	if (compress->fd < 0) {
		oops(&bad_compress, errno, "cannot open card(%u) device(%u)",
			card, device);
		goto config_fail;
	}

	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_GET_CAPS, &caps)) {
		oops(compress, errno, "cannot get device caps");
		goto codec_fail;
	}

	/* If caller passed "don't care" fill in default values */
	if ((config->fragment_size == 0) || (config->fragments == 0)) {
		config->fragment_size = caps.min_fragment_size;
		config->fragments = caps.max_fragments;
	}

#if 0
	/* FIXME need to turn this On when DSP supports
	 * and treat in no support case
	 */
	if (_is_codec_supported(compress, config, &caps) == false) {
		oops(compress, errno, "codec not supported\n");
		goto codec_fail;
	}
#endif

	memcpy(compress->config, config, sizeof(*compress->config));
	fill_compress_params(config, &params);

	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_SET_PARAMS, &params)) {
		oops(&bad_compress, errno, "cannot set device");
		goto codec_fail;
	}

	return compress;

codec_fail:
	snd_utils_put_dev_node(compress->snd_node);
	compress->ops->close(compress->data);
	compress->fd = -1;
config_fail:
	free(compress->config);
input_fail:
	free(compress);
	return &bad_compress;
}

void compress_close(struct compress *compress)
{
	if (compress == &bad_compress)
		return;

	snd_utils_put_dev_node(compress->snd_node);
	compress->ops->close(compress->data);
	compress->running = 0;
	compress->fd = -1;
	free(compress->config);
	free(compress);
}

int compress_get_hpointer(struct compress *compress,
		unsigned int *avail, struct timespec *tstamp)
{
	struct snd_compr_avail kavail;
	__u64 time;

	if (!is_compress_ready(compress))
		return oops(compress, ENODEV, "device not ready");

	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_AVAIL, &kavail))
		return oops(compress, errno, "cannot get avail");
	if (0 == kavail.tstamp.sampling_rate)
		return oops(compress, ENODATA, "sample rate unknown");
	*avail = (unsigned int)kavail.avail;
	time = kavail.tstamp.pcm_io_frames / kavail.tstamp.sampling_rate;
	tstamp->tv_sec = time;
	time = kavail.tstamp.pcm_io_frames % kavail.tstamp.sampling_rate;
	tstamp->tv_nsec = time * 1000000000 / kavail.tstamp.sampling_rate;
	return 0;
}

int compress_get_tstamp(struct compress *compress,
			unsigned long *samples, unsigned int *sampling_rate)
{
	struct snd_compr_tstamp ktstamp;

	if (!is_compress_ready(compress))
		return oops(compress, ENODEV, "device not ready");

	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_TSTAMP, &ktstamp))
		return oops(compress, errno, "cannot get tstamp");

	*samples = ktstamp.pcm_io_frames;
	*sampling_rate = ktstamp.sampling_rate;
	return 0;
}

int compress_write(struct compress *compress, const void *buf, unsigned int size)
{
	struct snd_compr_avail avail;
	struct pollfd fds;
	int to_write = 0;	/* zero indicates we haven't written yet */
	int written, total = 0, ret;
	const char* cbuf = buf;
	const unsigned int frag_size = compress->config->fragment_size;

	if (!(compress->flags & COMPRESS_IN))
		return oops(compress, EINVAL, "Invalid flag set");
	if (!is_compress_ready(compress))
		return oops(compress, ENODEV, "device not ready");
	fds.events = POLLOUT;

	/*TODO: treat auto start here first */
	while (size) {
		if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_AVAIL, &avail))
			return oops(compress, errno, "cannot get avail");

		/* We can write if we have at least one fragment available
		 * or there is enough space for all remaining data
		 */
		if ((avail.avail < frag_size) && (avail.avail < size)) {

			if (compress->nonblocking)
				return total;

			ret = compress->ops->poll(compress->data, &fds, 1,
								compress->max_poll_wait_ms);
			if (fds.revents & POLLERR) {
				return oops(compress, EIO, "poll returned error!");
			}
			/* A pause will cause -EBADFD or zero.
			 * This is not an error, just stop writing */
			if ((ret == 0) || (ret < 0 && errno == EBADFD))
				break;
			if (ret < 0)
				return oops(compress, errno, "poll error");
			if (fds.revents & POLLOUT) {
				continue;
			}
		}
		/* write avail bytes */
		if (size > avail.avail)
			to_write =  avail.avail;
		else
			to_write = size;
		written = compress->ops->write(compress->data, cbuf, to_write);
		if (written < 0) {
			/* If play was paused the write returns -EBADFD */
			if (errno == EBADFD)
				break;
			return oops(compress, errno, "write failed!");
		}

		size -= written;
		cbuf += written;
		total += written;
	}
	return total;
}

int compress_read(struct compress *compress, void *buf, unsigned int size)
{
	struct snd_compr_avail avail;
	struct pollfd fds;
	int to_read = 0;
	int num_read, total = 0, ret;
	char* cbuf = buf;
	const unsigned int frag_size = compress->config->fragment_size;

	if (!(compress->flags & COMPRESS_OUT))
		return oops(compress, EINVAL, "Invalid flag set");
	if (!is_compress_ready(compress))
		return oops(compress, ENODEV, "device not ready");
	fds.events = POLLIN;

	while (size) {
		if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_AVAIL, &avail))
			return oops(compress, errno, "cannot get avail");

		if ( (avail.avail < frag_size) && (avail.avail < size) ) {
			/* Less than one fragment available and not at the
			 * end of the read, so poll
			 */
			if (compress->nonblocking)
				return total;

			ret = compress->ops->poll(compress->data, &fds, 1,
							compress->max_poll_wait_ms);
			if (fds.revents & POLLERR) {
				return oops(compress, EIO, "poll returned error!");
			}
			/* A pause will cause -EBADFD or zero.
			 * This is not an error, just stop reading */
			if ((ret == 0) || (ret < 0 && errno == EBADFD))
				break;
			if (ret < 0)
				return oops(compress, errno, "poll error");
			if (fds.revents & POLLIN) {
				continue;
			}
		}
		/* read avail bytes */
		if (size > avail.avail)
			to_read = avail.avail;
		else
			to_read = size;
		num_read = compress->ops->read(compress->data, cbuf, to_read);
		if (num_read < 0) {
			/* If play was paused the read returns -EBADFD */
			if (errno == EBADFD)
				break;
			return oops(compress, errno, "read failed!");
		}

		size -= num_read;
		cbuf += num_read;
		total += num_read;
	}

	return total;
}

int compress_start(struct compress *compress)
{
	if (!is_compress_ready(compress))
		return oops(compress, ENODEV, "device not ready");
	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_START))
		return oops(compress, errno, "cannot start the stream");
	compress->running = 1;
	return 0;

}

int compress_stop(struct compress *compress)
{
	if (!is_compress_running(compress))
		return oops(compress, ENODEV, "device not ready");
	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_STOP))
		return oops(compress, errno, "cannot stop the stream");
	return 0;
}

int compress_pause(struct compress *compress)
{
	if (!is_compress_running(compress))
		return oops(compress, ENODEV, "device not ready");
	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_PAUSE))
		return oops(compress, errno, "cannot pause the stream");
	return 0;
}

int compress_resume(struct compress *compress)
{
	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_RESUME))
		return oops(compress, errno, "cannot resume the stream");
	return 0;
}

int compress_drain(struct compress *compress)
{
	if (!is_compress_running(compress))
		return oops(compress, ENODEV, "device not ready");
	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_DRAIN))
		return oops(compress, errno, "cannot drain the stream");
	return 0;
}

int compress_partial_drain(struct compress *compress)
{
	if (!is_compress_running(compress))
		return oops(compress, ENODEV, "device not ready");

	if (!compress->next_track)
		return oops(compress, EPERM, "next track not signalled");
	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_PARTIAL_DRAIN))
		return oops(compress, errno, "cannot drain the stream\n");
	compress->next_track = 0;
	return 0;
}

int compress_next_track(struct compress *compress)
{
	if (!is_compress_running(compress))
		return oops(compress, ENODEV, "device not ready");

	if (!compress->gapless_metadata)
		return oops(compress, EPERM, "metadata not set");
	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_NEXT_TRACK))
		return oops(compress, errno, "cannot set next track\n");
	compress->next_track = 1;
	compress->gapless_metadata = 0;
	return 0;
}

int compress_set_gapless_metadata(struct compress *compress,
	struct compr_gapless_mdata *mdata)
{
	struct snd_compr_metadata metadata;
	int version;

	if (!is_compress_ready(compress))
		return oops(compress, ENODEV, "device not ready");

	version = get_compress_version(compress);
	if (version <= 0)
		return -1;

	if (version < SNDRV_PROTOCOL_VERSION(0, 1, 1))
		return oops(compress, ENXIO, "gapless apis not supported in kernel");

	metadata.key = SNDRV_COMPRESS_ENCODER_PADDING;
	metadata.value[0] = mdata->encoder_padding;
	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_SET_METADATA, &metadata))
		return oops(compress, errno, "can't set metadata for stream\n");

	metadata.key = SNDRV_COMPRESS_ENCODER_DELAY;
	metadata.value[0] = mdata->encoder_delay;
	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_SET_METADATA, &metadata))
		return oops(compress, errno, "can't set metadata for stream\n");
	compress->gapless_metadata = 1;
	return 0;
}

#ifdef ENABLE_EXTENDED_COMPRESS_FORMAT
int compress_set_next_track_param(struct compress *compress,
	union snd_codec_options *codec_options)
{
	if (!is_compress_running(compress))
		return oops(compress, ENODEV, "device not ready");

	if (codec_options == NULL)
		return oops(compress, ENODEV, "codec_option NULL");

#ifdef SNDRV_COMPRESS_SET_NEXT_TRACK_PARAM
	if (ioctl(compress->fd, SNDRV_COMPRESS_SET_NEXT_TRACK_PARAM, codec_options))
		return oops(compress, errno, "cannot set next track params\n");
#endif
	return 0;
}
#endif

bool is_codec_supported(unsigned int card, unsigned int device,
		unsigned int flags, struct snd_codec *codec)
{
	struct compress_ops *ops;
	void *snd_node, *data;
	bool ret;
	int compress_type, fd;

	snd_node = snd_utils_get_dev_node(card, device, NODE_COMPRESS);
	compress_type = snd_utils_get_node_type(snd_node);
	if (compress_type == SND_NODE_TYPE_PLUGIN)
		ops = &compr_plug_ops;
	else
		ops = &compr_hw_ops;

	fd = ops->open(card, device, flags, &data, NULL);
	if (fd < 0) {
		oops(&bad_compress, errno, "cannot open card %u, device %u",
					card, device);
		return false;
	}

	ret = _is_codec_type_supported(ops, data, codec);

	snd_utils_put_dev_node(snd_node);
	ops->close(data);
	return ret;
}

void compress_set_max_poll_wait(struct compress *compress, int milliseconds)
{
	compress->max_poll_wait_ms = milliseconds;
}

void compress_nonblock(struct compress *compress, int nonblock)
{
	compress->nonblocking = !!nonblock;
}

int compress_wait(struct compress *compress, int timeout_ms)
{
	struct pollfd fds;
	int ret;

	fds.events = POLLOUT | POLLIN;

	ret = compress->ops->poll(compress->data, &fds, 1, timeout_ms);
	if (ret > 0) {
		if (fds.revents & POLLERR)
			return oops(compress, EIO, "poll returned error!");
		if (fds.revents & (POLLOUT | POLLIN))
			return 0;
	}
	if (ret == 0)
		return oops(compress, ETIME, "poll timed out");
	if (ret < 0)
		return oops(compress, errno, "poll error");

	return oops(compress, EIO, "poll signalled unhandled event");
}

int compress_set_codec_params(struct compress *compress,
	struct snd_codec *codec) {
	struct snd_compr_params params;

	if (!is_compress_ready(compress) || !compress->next_track)
		return oops(compress, ENODEV, "device not ready");

	params.buffer.fragment_size = compress->config->fragment_size;
	params.buffer.fragments = compress->config->fragments;
	memcpy(&params.codec, codec, sizeof(params.codec));
	memcpy(&compress->config->codec, codec, sizeof(struct snd_codec));

	if (compress->ops->ioctl(compress->data, SNDRV_COMPRESS_SET_PARAMS, &params))
		return oops(compress, errno, "cannot set device");

	compress->next_track = 0;
	return 0;
}

#ifdef ENABLE_EXTENDED_COMPRESS_FORMAT
int compress_get_metadata(struct compress *compress,
		struct snd_compr_metadata *mdata) {
	int version;
	if (!is_compress_ready(compress))
		return oops(compress, ENODEV, "device not ready");

	version = get_compress_version(compress);
	if (version <= 0)
		return -1;

	if (ioctl(compress->fd, SNDRV_COMPRESS_GET_METADATA, mdata)) {
		return oops(compress, errno, "can't get metadata for stream\n");
	}
	return 0;
}

int compress_set_metadata(struct compress *compress,
		struct snd_compr_metadata *mdata) {

	int version;
	if (!is_compress_ready(compress))
		return oops(compress, ENODEV, "device not ready");

	version = get_compress_version(compress);
	if (version <= 0)
		return -1;

	if (ioctl(compress->fd, SNDRV_COMPRESS_SET_METADATA, mdata)) {
		return oops(compress, errno, "can't set metadata for stream\n");
	}
	return 0;
}
#endif
