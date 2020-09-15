// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2020 Google LLC. All rights reserved.
//
// Author: Pin-chih Lin <johnylin@google.com>

#include <sof/audio/component.h>
#include <sof/audio/buffer.h>
#include <sof/audio/drc/drc.h>
#include <sof/audio/drc/drc_math.h>
#include <sof/audio/format.h>
#include <sof/audio/pipeline.h>
#include <sof/common.h>
#include <sof/debug/panic.h>
#include <sof/drivers/ipc.h>
#include <sof/lib/alloc.h>
#include <sof/lib/memory.h>
#include <sof/lib/uuid.h>
#include <sof/list.h>
#include <sof/math/decibels.h>
#include <sof/platform.h>
#include <sof/string.h>
#include <sof/ut.h>
#include <sof/trace/trace.h>
#include <ipc/control.h>
#include <ipc/stream.h>
#include <ipc/topology.h>
#include <user/eq.h>
#include <user/trace.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

static const struct comp_driver comp_drc;

/* TODO: get the real UUID */
/* 5150c0e6-27f9-4ec8-8351-c705b642d12f */
DECLARE_SOF_RT_UUID("drc", drc_uuid, 0x5150c0e6, 0x27f9, 0x4ec8,
		    0x83, 0x51, 0xc7, 0x05, 0xb6, 0x42, 0xd1, 0x2f);

DECLARE_TR_CTX(drc_tr, SOF_UUID(drc_uuid), LOG_LEVEL_INFO);

static inline void drc_free_parameters(struct sof_drc_config **config)
{
	rfree(*config);
	*config = NULL;
}

static inline void drc_reset_state(struct drc_state *state)
{
	int i;

	for (i = 0; i < PLATFORM_MAX_CHANNELS; ++i) {
		rfree(state->pre_delay_buffers[i]);
		state->pre_delay_buffers[i] = NULL;
	}

	state->detector_average = 0;
	state->compressor_gain = Q_CONVERT_FLOAT(1.0f, 30);

	state->last_pre_delay_frames = DRC_DEFAULT_PRE_DELAY_FRAMES;
	state->pre_delay_read_index = 0;
	state->pre_delay_write_index = DRC_DEFAULT_PRE_DELAY_FRAMES;

	state->envelope_rate = 0;
	state->scaled_desired_gain = 0;

	state->processed = 0;

	state->max_attack_compression_diff_db = INT32_MIN;
}

static inline int drc_init_pre_delay_buffers(struct drc_state *state,
					     size_t sample_bytes,
					     int channels)
{
	int i;

	/* Allocate pre-delay (lookahead) buffers */
	for (i = 0; i < channels; ++i) {
		state->pre_delay_buffers[i] =
			rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM,
				sample_bytes * DRC_MAX_PRE_DELAY_FRAMES);
		if (!state->pre_delay_buffers[i])
			return -ENOMEM;
		memset(state->pre_delay_buffers[i], 0, sample_bytes * DRC_MAX_PRE_DELAY_FRAMES);
	}

	return 0;
}

static inline void drc_set_pre_delay_time(struct drc_state *state,
					  int32_t pre_delay_time,
					  int32_t rate)
{
	uint32_t pre_delay_frames;

	/* Re-configure look-ahead section pre-delay if delay time has
	 * changed. */
	pre_delay_frames = pre_delay_time * rate;
	pre_delay_frames = min(pre_delay_frames, DRC_MAX_PRE_DELAY_FRAMES - 1);

	/* Make pre_delay_frames multiplies of DIVISION_FRAMES. This way we
	 * won't split a division of samples into two blocks of memory, so it is
	 * easier to process. This may make the actual delay time slightly less
	 * than the specified value, but the difference is less than 1ms. */
	pre_delay_frames &= ~DRC_DIVISION_FRAMES_MASK;

	/* We need at least one division buffer, so the incoming data won't
	 * overwrite the output data */
	pre_delay_frames = max(pre_delay_frames, DRC_DIVISION_FRAMES);

	if (state->last_pre_delay_frames != (int32_t)pre_delay_frames) {
		state->last_pre_delay_frames = (int32_t)pre_delay_frames;
		state->pre_delay_read_index = 0;
		state->pre_delay_write_index = (int32_t)pre_delay_frames;
	}
}

static int drc_setup(struct comp_data *cd, uint16_t channels, uint32_t rate)
{
	int ret = 0;

	/* Reset any previous state */
	drc_reset_state(&cd->state);

	/* Allocate pre-delay buffers */
	uint32_t sample_bytes = get_sample_bytes(cd->source_format);
	ret = drc_init_pre_delay_buffers(&cd->state, (size_t)sample_bytes, (int)channels);
	if (ret < 0)
		return ret;

	/* Set pre-dely time */
	drc_set_pre_delay_time(&cd->state, cd->config->params.pre_delay_time, rate);
	return 0;
}

/*
 * End of DRC setup code. Next the standard component methods.
 */

static struct comp_dev *drc_new(const struct comp_driver *drv,
				struct sof_ipc_comp *comp)
{
	struct comp_dev *dev;
	struct comp_data *cd;
	struct sof_ipc_comp_process *drc;
	struct sof_ipc_comp_process *ipc_drc =
		(struct sof_ipc_comp_process *)comp;
	size_t bs = ipc_drc->size;
	int ret;

	comp_cl_info(&comp_drc, "drc_new()");

	/* Check first before proceeding with dev and cd that coefficients
	 * blob size is sane.
	 */
	if (bs > SOF_DRC_MAX_SIZE) {
		comp_cl_err(&comp_drc, "drc_new(), coefficients blob size %u exceeds maximum",
			    bs);
		return NULL;
	}

	dev = comp_alloc(drv, COMP_SIZE(struct sof_ipc_comp_process));
	if (!dev)
		return NULL;

	drc = COMP_GET_IPC(dev, sof_ipc_comp_process);
	ret = memcpy_s(drc, sizeof(*drc), ipc_drc,
		       sizeof(struct sof_ipc_comp_process));
	assert(!ret);

	cd = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM, sizeof(*cd));
	if (!cd) {
		rfree(dev);
		return NULL;
	}

	comp_set_drvdata(dev, cd);

	cd->drc_func = NULL;
	cd->config = NULL;
	cd->config_new = NULL;

	if (bs) {
		cd->config = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM,
				     bs);
		if (!cd->config) {
			rfree(dev);
			rfree(cd);
			return NULL;
		}

		ret = memcpy_s(cd->config, bs, ipc_drc->data, bs);
		assert(!ret);
	}

	drc_reset_state(&cd->state);

	dev->state = COMP_STATE_READY;
	return dev;
}

static void drc_free(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	comp_info(dev, "drc_free()");

	drc_free_parameters(&cd->config);
	drc_free_parameters(&cd->config_new);

	rfree(cd);
	rfree(dev);
}

static int drc_verify_params(struct comp_dev *dev,
			     struct sof_ipc_stream_params *params)
{
	int ret;

	comp_dbg(dev, "drc_verify_params()");

	ret = comp_verify_params(dev, 0, params);
	if (ret < 0) {
		comp_err(dev, "drc_verify_params(): comp_verify_params() failed.");
		return ret;
	}

	return 0;
}

static int drc_params(struct comp_dev *dev,
		      struct sof_ipc_stream_params *params)
{
	int err;

	comp_info(dev, "drc_params()");

	err = drc_verify_params(dev, params);
	if (err < 0) {
		comp_err(dev, "drc_params(): pcm params verification failed.");
		return -EINVAL;
	}

	/* All configuration work is postponed to prepare(). */
	return 0;
}

static int drc_cmd_get_data(struct comp_dev *dev,
			    struct sof_ipc_ctrl_data *cdata, int max_size)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	unsigned char *dst;
	unsigned char *src;
	size_t offset;
	size_t bs;
	int ret = 0;

	switch (cdata->cmd) {
	case SOF_CTRL_CMD_BINARY:
		comp_info(dev, "drc_cmd_get_data(), SOF_CTRL_CMD_BINARY");

		/* Need subtract headers to calculate payload chunk size */
		max_size -= sizeof(struct sof_ipc_ctrl_data) +
			sizeof(struct sof_abi_hdr);

		/* Copy back to user space */
		if (cd->config) {
			src = (unsigned char *)cd->config;
			dst = (unsigned char *)cdata->data->data;

			/* Get size of stored entire configuration payload
			 * into bs.
			 */
			bs = cd->config->size;
			cdata->elems_remaining = 0;
			offset = 0;
			if (bs > max_size) {
				/* Use max_size or remaining data size if at
				 * last chunk of data.
				 */
				bs = (cdata->msg_index + 1) * max_size > bs ?
					bs - cdata->msg_index * max_size :
					max_size;
				/* Start from end of previous chunk */
				offset = cdata->msg_index * max_size;
				/* Remaining amount of data for next IPC */
				cdata->elems_remaining = cd->config->size -
					offset;
			}

			/* Payload size for this IPC response is set from bs */
			cdata->num_elems = bs;
			comp_info(dev, "drc_cmd_get_data(), chunk size %zu msg index %u max size %u offset %zu",
				  bs, cdata->msg_index, max_size, offset);
			ret = memcpy_s(dst, max_size, src + offset, bs);
			assert(!ret);

			cdata->data->abi = SOF_ABI_VERSION;
			cdata->data->size = bs;
		} else {
			comp_err(dev, "drc_cmd_get_data(): no config");
			ret = -EINVAL;
		}
		break;
	default:
		comp_err(dev, "drc_cmd_get_data(), invalid command");
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int drc_cmd_set_data(struct comp_dev *dev,
			    struct sof_ipc_ctrl_data *cdata)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	unsigned char *dst;
	unsigned char *src;
	uint32_t offset;
	size_t size;
	int ret = 0;

	switch (cdata->cmd) {
	case SOF_CTRL_CMD_BINARY:
		comp_info(dev, "drc_cmd_set_data(), SOF_CTRL_CMD_BINARY");

		/* Check that there is no work-in-progress previous request */
		if (cd->config_new && cdata->msg_index == 0) {
			comp_err(dev, "drc_cmd_set_data(), busy with previous request");
			return -EBUSY;
		}

		/* Copy new configuration */
		if (cdata->msg_index == 0) {
			/* Allocate buffer for copy of the blob. */
			size = cdata->num_elems + cdata->elems_remaining;
			comp_info(dev, "drc_cmd_set_data(), allocating %d for configuration blob",
				  size);
			if (size > SOF_DRC_MAX_SIZE) {
				comp_err(dev, "drc_cmd_set_data(), size exceeds %d",
					 SOF_DRC_MAX_SIZE);
				return -EINVAL;
			}

			cd->config_new = rzalloc(SOF_MEM_ZONE_RUNTIME, 0,
						 SOF_MEM_CAPS_RAM, size);
			if (!cd->config_new) {
				comp_err(dev, "drc_cmd_set_data(): buffer allocation failed");
				return -EINVAL;
			}

			offset = 0;
		} else {
			assert(cd->config_new);
			size = cd->config_new->size;
			offset = size - cdata->elems_remaining -
				cdata->num_elems;
		}

		comp_info(dev, "drc_cmd_set_data(), chunk size: %u msg_index %u",
			  cdata->num_elems, cdata->msg_index);
		dst = (unsigned char *)cd->config_new;
		src = (unsigned char *)cdata->data->data;

		/* Just copy the configuration. The EQ will be initialized in
		 * prepare().
		 */
		ret = memcpy_s(dst + offset, size - offset, src,
			       cdata->num_elems);
		assert(!ret);

		/* we can check data when elems_remaining == 0 */
		if (cdata->elems_remaining == 0) {
			/* If component state is READY we can omit old
			 * configuration immediately. When in playback/capture
			 * the new configuration presence is checked in copy().
			 */
			if (dev->state == COMP_STATE_READY)
				drc_free_parameters(&cd->config);

			/* If there is no existing configuration the received
			 * can be set to current immediately. It will be
			 * applied in prepare() when streaming starts.
			 */
			if (!cd->config) {
				cd->config = cd->config_new;
				cd->config_new = NULL;
			}
		}
		break;
	default:
		comp_err(dev, "drc_cmd_set_data(), invalid command");
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int drc_cmd(struct comp_dev *dev, int cmd, void *data,
		   int max_data_size)
{
	struct sof_ipc_ctrl_data *cdata = data;
	int ret = 0;

	comp_info(dev, "drc_cmd()");

	switch (cmd) {
	case COMP_CMD_SET_DATA:
		ret = drc_cmd_set_data(dev, cdata);
		break;
	case COMP_CMD_GET_DATA:
		ret = drc_cmd_get_data(dev, cdata, max_data_size);
		break;
	default:
		comp_err(dev, "drc_cmd(), invalid command");
		ret = -EINVAL;
	}

	return ret;
}

static int drc_trigger(struct comp_dev *dev, int cmd)
{
	comp_info(dev, "drc_trigger()");

	return comp_set_state(dev, cmd);
}

static void drc_process(struct comp_dev *dev, struct comp_buffer *source,
			struct comp_buffer *sink, int frames,
			uint32_t source_bytes, uint32_t sink_bytes)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	buffer_invalidate(source, source_bytes);

	cd->drc_func(dev, &source->stream, &sink->stream, frames);

	buffer_writeback(sink, sink_bytes);

	/* calc new free and available */
	comp_update_buffer_consume(source, source_bytes);
	comp_update_buffer_produce(sink, sink_bytes);
}

/* copy and process stream data from source to sink buffers */
static int drc_copy(struct comp_dev *dev)
{
	struct comp_copy_limits cl;
	struct comp_data *cd = comp_get_drvdata(dev);
	struct comp_buffer *sourceb;
	struct comp_buffer *sinkb;
	int ret;

	comp_dbg(dev, "drc_copy()");

	sourceb = list_first_item(&dev->bsource_list, struct comp_buffer,
				  sink_list);

	/* Check for changed configuration */
	if (cd->config_new) {
		drc_free_parameters(&cd->config);
		cd->config = cd->config_new;
		cd->config_new = NULL;
		ret = drc_setup(cd, sourceb->stream.channels, sourceb->stream.rate);
		if (ret < 0) {
			comp_err(dev, "drc_copy(), failed DRC setup");
			return ret;
		}
	}

	sinkb = list_first_item(&dev->bsink_list, struct comp_buffer,
				source_list);

	/* Get source, sink, number of frames etc. to process. */
	comp_get_copy_limits_with_lock(sourceb, sinkb, &cl);

	/* Run DRC function */
	drc_process(dev, sourceb, sinkb, cl.frames, cl.source_bytes,
		    cl.sink_bytes);

	return 0;
}

static int drc_prepare(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	struct sof_ipc_comp_config *config = dev_comp_config(dev);
	struct comp_buffer *sourceb;
	struct comp_buffer *sinkb;
	uint32_t sink_period_bytes;
	int ret;

	comp_info(dev, "drc_prepare()");

	ret = comp_set_state(dev, COMP_TRIGGER_PREPARE);
	if (ret < 0)
		return ret;

	if (ret == COMP_STATUS_STATE_ALREADY_SET)
		return PPL_STATUS_PATH_STOP;

	/* DRC component will only ever have 1 source and 1 sink buffer */
	sourceb = list_first_item(&dev->bsource_list,
				  struct comp_buffer, sink_list);
	sinkb = list_first_item(&dev->bsink_list,
				struct comp_buffer, source_list);

	/* get source data format */
	cd->source_format = sourceb->stream.frame_fmt;

	/* validate sink data format and period bytes */
	if (cd->source_format != sinkb->stream.frame_fmt) {
		comp_err(dev, "drc_prepare(): Source fmt %d and sink fmt %d are different.",
			 cd->source_format, sinkb->stream.frame_fmt);
		ret = -EINVAL;
		goto err;
	}

	sink_period_bytes = audio_stream_period_bytes(&sinkb->stream,
						      dev->frames);

	if (sinkb->stream.size < config->periods_sink * sink_period_bytes) {
		comp_err(dev, "drc_prepare(), sink buffer size %d is insufficient",
			 sinkb->stream.size);
		ret = -ENOMEM;
		goto err;
	}

	/* Initialize DRC */
	comp_info(dev, "drc_prepare(), source_format=%d, sink_format=%d",
		  cd->source_format, cd->source_format);
	if (cd->config) {
		ret = drc_setup(cd, sourceb->stream.channels, sourceb->stream.rate);
		if (ret < 0) {
			comp_err(dev, "drc_prepare(), setup failed.");
			goto err;
		}
		cd->drc_func = drc_find_proc_func(cd->source_format);
		if (!cd->drc_func) {
			comp_err(dev, "drc_prepare(), No proc func");
			ret = -EINVAL;
			goto err;
		}
		comp_info(dev, "drc_prepare(), DRC is configured.");
	} else {
		cd->drc_func = drc_find_proc_func_pass(cd->source_format);
		if (!cd->drc_func) {
			comp_err(dev, "drc_prepare(), No pass func");
			ret = -EINVAL;
			goto err;
		}
		comp_info(dev, "drc_prepare(), pass-through mode.");
	}
	return 0;

err:
	comp_set_state(dev, COMP_TRIGGER_RESET);
	return ret;
}

static int drc_reset(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	comp_info(dev, "drc_reset()");

	drc_reset_state(&cd->state);

	cd->drc_func = NULL;

	comp_set_state(dev, COMP_TRIGGER_RESET);
	return 0;
}

static const struct comp_driver comp_drc = {
	.type = SOF_COMP_DRC,
	.uid = SOF_RT_UUID(drc_uuid),
	.tctx = &drc_tr,
	.ops = {
		.create  = drc_new,
		.free    = drc_free,
		.params  = drc_params,
		.cmd     = drc_cmd,
		.trigger = drc_trigger,
		.copy    = drc_copy,
		.prepare = drc_prepare,
		.reset   = drc_reset,
	},
};

static SHARED_DATA struct comp_driver_info comp_drc_info = {
	.drv = &comp_drc,
};

UT_STATIC void sys_comp_drc_init(void)
{
	comp_register(platform_shared_get(&comp_drc_info,
					  sizeof(comp_drc_info)));
}

DECLARE_MODULE(sys_comp_drc_init);
