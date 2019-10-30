// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2020 Google LLC. All rights reserved.
//
// Author: Ben Zhang <benzh@chromium.org>

#include <sof/audio/buffer.h>
#include <sof/audio/component.h>
#include <sof/audio/format.h>
#include <sof/audio/kpb.h>
#include <sof/common.h>
#include <sof/debug/panic.h>
#include <sof/drivers/ipc.h>
#include <sof/lib/alloc.h>
#include <sof/lib/memory.h>
#include <sof/lib/notifier.h>
#include <sof/lib/wait.h>
#include <sof/lib/uuid.h>
#include <sof/list.h>
#include <sof/math/numbers.h>
#include <sof/string.h>
#include <sof/trace/trace.h>
#include <ipc/control.h>
#include <ipc/stream.h>
#include <ipc/topology.h>
#include <kernel/abi.h>
#include <user/detect_test.h>
#include <user/trace.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <hotword_dsp_api.h>

static const struct comp_driver ghw_driver;

/* eba8d51f-7827-47b5-82ee-de6e7743af67 */
DECLARE_SOF_RT_UUID("kd-test", keyword_uuid, 0xeba8d51f, 0x7827, 0x47b5,
		    0x82, 0xee, 0xde, 0x6e, 0x77, 0x43, 0xaf, 0x67);

DECLARE_TR_CTX(keyword_tr, SOF_UUID(keyword_uuid), LOG_LEVEL_INFO);

struct ghw_private {
	struct comp_model_data model;
	struct kpb_event_data event_data;
	struct kpb_client client_data;

	struct sof_ipc_comp_event event;
	struct ipc_msg *msg;

	int detected;
};

static void notify_host(const struct comp_dev *dev)
{
	struct ghw_private *priv = comp_get_drvdata(dev);

	comp_info(dev, "notify_host()");

	ipc_msg_send(priv->msg, &priv->event, true);
}

static void notify_kpb(const struct comp_dev *dev)
{
	struct ghw_private *priv = comp_get_drvdata(dev);

	comp_info(dev, "notify_kpb()");

	priv->client_data.r_ptr = NULL;
	priv->client_data.sink = NULL;
	priv->client_data.id = 0;
	priv->event_data.event_id = KPB_EVENT_BEGIN_DRAINING;
	priv->event_data.client_data = &priv->client_data;

	notifier_event(dev, NOTIFIER_ID_KPB_CLIENT_EVT,
		       NOTIFIER_TARGET_CORE_ALL_MASK, &priv->event_data,
		       sizeof(priv->event_data));
}

static struct comp_dev *ghw_create(const struct comp_driver *drv,
				   struct sof_ipc_comp *comp_template)
{
	struct comp_dev *dev = NULL;
	struct ghw_private *priv = NULL;
	struct sof_ipc_comp_process *comp;
	int ret;

	comp_cl_info(drv, "ghw_create()");

	/* Create component device with an effect processing component */
	dev = comp_alloc(drv, COMP_SIZE(struct sof_ipc_comp_process));
	if (!dev)
		goto fail;

	comp = COMP_GET_IPC(dev, sof_ipc_comp_process);
	ret = memcpy_s(comp, sizeof(*comp), comp_template,
		       sizeof(struct sof_ipc_comp_process));
	if (ret)
		goto fail;

	/* Create private component data */
	priv = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM,
		       sizeof(*priv));
	if (!priv)
		goto fail;
	comp_set_drvdata(dev, priv);

	/* Build component event */
	ipc_build_comp_event(&priv->event, comp->comp.type, comp->comp.id);
	priv->event.event_type = SOF_CTRL_EVENT_KD;
	priv->event.num_elems = 0;

	priv->msg = ipc_msg_init(priv->event.rhdr.hdr.cmd, sizeof(priv->event));
	if (!priv->msg) {
		comp_err(dev, "ghw_create(): ipc_msg_init failed");
		goto fail;
	}

	dev->state = COMP_STATE_READY;
	comp_info(dev, "ghw_create(): Ready");
	return dev;

fail:
	if (priv) {
		ipc_msg_free(priv->msg);
		rfree(priv);
	}
	if (dev)
		rfree(dev);
	return NULL;
}

static void ghw_free(struct comp_dev *dev)
{
	struct ghw_private *priv = comp_get_drvdata(dev);

	comp_info(dev, "ghw_free()");

	ipc_msg_free(priv->msg);
	comp_free_model_data(dev, &priv->model);
	rfree(priv);
	rfree(dev);
}

static int ghw_params(struct comp_dev *dev,
		      struct sof_ipc_stream_params *params)
{
	struct comp_buffer *sourceb;
	int ret;

	/* Detector is used only in KPB topology. It always requires channels
	 * parameter set to 1.
	 */
	params->channels = 1;

	ret = comp_verify_params(dev, 0, params);
	if (ret < 0) {
		comp_err(dev, "ghw_params(): comp_verify_params failed.");
		return -EINVAL;
	}

	/* This detector component will only ever have 1 source */
	sourceb = list_first_item(&dev->bsource_list, struct comp_buffer,
				  sink_list);

	if (sourceb->stream.channels != 1) {
		comp_err(dev, "ghw_params(): Only single-channel supported");
		return -EINVAL;
	}

	if (sourceb->stream.frame_fmt != SOF_IPC_FRAME_S16_LE) {
		comp_err(dev, "ghw_params(): Only S16_LE supported");
		return -EINVAL;
	}

	if (sourceb->stream.rate != 16000) {
		comp_err(dev, "ghw_params(): Only 16KHz supported");
		return -EINVAL;
	}

	return 0;
}

static void ghw_show_model(struct comp_dev *dev)
{
	struct ghw_private *priv = comp_get_drvdata(dev);

	comp_info(dev, "ghw_show_model: data=0x%08x, data_size=%u, crc=%u, data_pos=%u",
		  (uint32_t)priv->model.data, priv->model.data_size,
		  priv->model.crc, priv->model.data_pos);
}

static int ghw_cmd(struct comp_dev *dev, int cmd, void *data,
		   int max_data_size)
{
	struct sof_ipc_ctrl_data *cdata = data;
	struct ghw_private *priv = comp_get_drvdata(dev);
	int ret;

	if (cmd != COMP_CMD_SET_DATA && cmd != COMP_CMD_GET_DATA) {
		comp_err(dev, "ghw_cmd(): Unknown cmd %d", cmd);
		return -EINVAL;
	}

	if (cdata->cmd != SOF_CTRL_CMD_BINARY) {
		comp_err(dev, "ghw_cmd(): Only binary controls supported %d",
			 cdata->cmd);
		return -EINVAL;
	}

	if (cdata->data->type == SOF_DETECT_TEST_CONFIG) {
		comp_info(dev, "ghw_cmd(): SOF_DETECT_TEST_CONFIG no-op");
		return 0;
	}

	if (cdata->data->type != SOF_DETECT_TEST_MODEL) {
		comp_err(dev, "ghw_cmd(): Unknown cdata->data->type %d",
			 cdata->data->type);
		return -EINVAL;
	}

	if (cmd == COMP_CMD_GET_DATA) {
		ret = comp_get_model(dev, &priv->model, cdata, max_data_size);
		comp_info(dev, "ghw_cmd(): comp_get_model=%d, size=%d",
			  ret, max_data_size);
	} else {
		if (dev->state != COMP_STATE_READY) {
			comp_err(dev, "ghw_cmd(): Can't set model, state=%d",
				 dev->state);
			return -EBUSY;
		}
		ret = comp_set_model(dev, &priv->model, cdata);
		comp_info(dev, "ghw_cmd(): comp_set_model=%d", ret);
	}

	return ret;
}

static int ghw_trigger(struct comp_dev *dev, int cmd)
{
	comp_info(dev, "ghw_trigger(): %d", cmd);

	return comp_set_state(dev, cmd);
}

static void ghw_detect(struct comp_dev *dev, const void *samples,
		       int num_samples)
{
	struct ghw_private *priv = comp_get_drvdata(dev);
	int preamble_length_ms = 0;
	int ret;

	if (priv->detected)
		return;

	comp_dbg(dev, "GoogleHotwordDspProcess(0x%x, %d)",
		 (uint32_t)samples, num_samples);
	ret = GoogleHotwordDspProcess(samples, num_samples,
				      &preamble_length_ms);
	if (ret == 1) {
		comp_info(dev, "Hotword detected %dms", preamble_length_ms);
		priv->detected = 1;
		priv->client_data.drain_req = preamble_length_ms;
		notify_host(dev);
		notify_kpb(dev);
	}
}

static int ghw_copy(struct comp_dev *dev)
{
	struct comp_buffer *source;
	struct audio_stream *stream;
	uint32_t frames;
	uint32_t flags = 0;
	uint32_t tail_bytes, head_bytes = 0;

	/* keyword components will only ever have 1 source */
	source = list_first_item(&dev->bsource_list,
				 struct comp_buffer, sink_list);
	stream = &source->stream;

	buffer_lock(source, &flags);
	frames = audio_stream_get_avail_frames(&source->stream);
	buffer_unlock(source, flags);

	comp_dbg(dev, "ghw_copy() %u frames, %u bytes", frames, stream->avail);
	comp_dbg(dev, "[0x%x 0x%x]",
		 (uint32_t)stream->addr, (uint32_t)stream->end_addr);
	comp_dbg(dev, "[   (%u) 0x%x (%u)   ]",
		 (char *)stream->r_ptr - (char *)stream->addr,
		 (uint32_t)stream->r_ptr,
		 (char *)stream->end_addr - (char *)stream->r_ptr);

	/* copy and perform detection */
	buffer_invalidate(source, source->stream.avail);

	tail_bytes = (char *)stream->end_addr - (char *)stream->r_ptr;
	if (stream->avail <= tail_bytes)
		tail_bytes = stream->avail;
	else
		head_bytes = stream->avail - tail_bytes;

	if (tail_bytes)
		ghw_detect(dev, stream->r_ptr, tail_bytes / 2);
	if (head_bytes)
		ghw_detect(dev, stream->addr, head_bytes / 2);

	/* calc new available */
	comp_update_buffer_consume(source, source->stream.avail);

	return 0;
}

static int ghw_reset(struct comp_dev *dev)
{
	struct ghw_private *priv = comp_get_drvdata(dev);

	comp_info(dev, "ghw_reset()");

	priv->detected = 0;
	GoogleHotwordDspReset();

	return comp_set_state(dev, COMP_TRIGGER_RESET);
}

static int ghw_prepare(struct comp_dev *dev)
{
	struct ghw_private *priv = comp_get_drvdata(dev);
	int ret;

	comp_info(dev, "ghw_prepare()");

	if (!priv->model.data ||
	    !priv->model.data_size ||
	    priv->model.data_size != priv->model.data_pos) {
		ghw_show_model(dev);
		comp_err(dev, "Model not set");
		return -EINVAL;
	}

	comp_info(dev, "GoogleHotwordVersion %d",
		  GoogleHotwordVersion());

	ret = GoogleHotwordDspInit(priv->model.data);
	comp_info(dev, "GoogleHotwordDSPInit %d", ret);
	priv->detected = 0;

	return comp_set_state(dev, COMP_TRIGGER_PREPARE);
}

static const struct comp_driver ghw_driver = {
	.type	= SOF_COMP_KEYWORD_DETECT,
	.uid	= SOF_RT_UUID(keyword_uuid),
	.tctx	= &keyword_tr,
	.ops	= {
		.create		= ghw_create,
		.free		= ghw_free,
		.params		= ghw_params,
		.cmd		= ghw_cmd,
		.trigger	= ghw_trigger,
		.copy		= ghw_copy,
		.prepare	= ghw_prepare,
		.reset		= ghw_reset,
	},
};

static SHARED_DATA struct comp_driver_info ghw_driver_info = {
	.drv = &ghw_driver,
};

static void sys_comp_keyword_init(void)
{
	comp_register(platform_shared_get(&ghw_driver_info,
					  sizeof(ghw_driver_info)));
}

DECLARE_MODULE(sys_comp_keyword_init);
