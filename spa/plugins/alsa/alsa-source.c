/* Spa ALSA Source
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stddef.h>

#include <asoundlib.h>

#include <spa/node/node.h>
#include <spa/utils/list.h>
#include <spa/param/audio/format.h>
#include <spa/pod/filter.h>

#define NAME "alsa-source"

#include "alsa-utils.h"

#define CHECK_PORT(this,d,p)    ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)

static const char default_device[] = "hw:0";
static const uint32_t default_min_latency = 64;
static const uint32_t default_max_latency = 1024;

static void reset_props(struct props *props)
{
	strncpy(props->device, default_device, 64);
	props->min_latency = default_min_latency;
	props->max_latency = default_max_latency;
}

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **result,
				 struct spa_pod_builder *builder)
{
	struct state *this;
	struct spa_pod *param;
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };
	struct props *p;


	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);
	p = &this->props;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_ID_PARAM_List:
	{
		uint32_t list[] = { SPA_ID_PARAM_PropInfo,
				    SPA_ID_PARAM_Props, };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b, id, SPA_ID_OBJECT_ParamList,
				":", SPA_PARAM_LIST_id, "I", list[*index]);
		else
			return 0;
		break;
	}
	case SPA_ID_PARAM_PropInfo:
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, SPA_ID_OBJECT_PropInfo,
				":", SPA_PROP_INFO_id, "I", SPA_PROP_device,
				":", SPA_PROP_INFO_name, "s", "The ALSA device",
				":", SPA_PROP_INFO_type, "S", p->device, sizeof(p->device));
			break;
		case 1:
			param = spa_pod_builder_object(&b,
				id, SPA_ID_OBJECT_PropInfo,
				":", SPA_PROP_INFO_id, "I", SPA_PROP_deviceName,
				":", SPA_PROP_INFO_name, "s", "The ALSA device name",
				":", SPA_PROP_INFO_type, "S-r", p->device_name, sizeof(p->device_name));
			break;
		case 2:
			param = spa_pod_builder_object(&b,
				id, SPA_ID_OBJECT_PropInfo,
				":", SPA_PROP_INFO_id, "I", SPA_PROP_cardName,
				":", SPA_PROP_INFO_name, "s", "The ALSA card name",
				":", SPA_PROP_INFO_type, "S-r", p->card_name, sizeof(p->card_name));
			break;
		case 3:
			param = spa_pod_builder_object(&b,
				id, SPA_ID_OBJECT_PropInfo,
				":", SPA_PROP_INFO_id, "I", SPA_PROP_minLatency,
				":", SPA_PROP_INFO_name, "s", "The minimum latency",
				":", SPA_PROP_INFO_type, "ir", p->min_latency,
					SPA_POD_PROP_MIN_MAX(1, INT32_MAX));
			break;
		case 4:
			param = spa_pod_builder_object(&b,
				id, SPA_ID_OBJECT_PropInfo,
				":", SPA_PROP_INFO_id,   "I", SPA_PROP_maxLatency,
				":", SPA_PROP_INFO_name, "s", "The maximum latency",
				":", SPA_PROP_INFO_type, "ir", p->max_latency,
					SPA_POD_PROP_MIN_MAX(1, INT32_MAX));
			break;
		default:
			return 0;
		}
		break;

	case SPA_ID_PARAM_Props:
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, SPA_ID_OBJECT_Props,
				":", SPA_PROP_device,     "S",   p->device, sizeof(p->device),
				":", SPA_PROP_deviceName, "S-r", p->device_name, sizeof(p->device_name),
				":", SPA_PROP_cardName,   "S-r", p->card_name, sizeof(p->card_name),
				":", SPA_PROP_minLatency, "i",   p->min_latency,
				":", SPA_PROP_maxLatency, "i",   p->max_latency);
			break;
		default:
			return 0;
		}
		break;

	default:
		return -ENOENT;
	}

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	switch (id) {
	case SPA_ID_PARAM_Props:
	{
		struct props *p = &this->props;

		if (param == NULL) {
			reset_props(p);
			return 0;
		}
		spa_pod_object_parse(param,
			":", SPA_PROP_device,     "?S", p->device, sizeof(p->device),
			":", SPA_PROP_minLatency, "?i", &p->min_latency,
			":", SPA_PROP_maxLatency, "?i", &p->max_latency, NULL);
		break;
	}
	default:
		return -ENOENT;
	}

	return 0;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct state *this;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	switch (SPA_COMMAND_TYPE(command)) {
	case SPA_ID_COMMAND_NODE_Start:
		if (!this->have_format)
			return -EIO;
		if (this->n_buffers == 0)
			return -EIO;

		if ((res = spa_alsa_start(this, false)) < 0)
			return res;
		break;
	case SPA_ID_COMMAND_NODE_Pause:
		if ((res = spa_alsa_pause(this, false)) < 0)
			return res;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	this->callbacks = callbacks;
	this->callbacks_data = data;

	return 0;
}

static int
impl_node_get_n_ports(struct spa_node *node,
		      uint32_t *n_input_ports,
		      uint32_t *max_input_ports,
		      uint32_t *n_output_ports,
		      uint32_t *max_output_ports)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	if (n_input_ports)
		*n_input_ports = 0;
	if (max_input_ports)
		*max_input_ports = 0;
	if (n_output_ports)
		*n_output_ports = 1;
	if (max_output_ports)
		*max_output_ports = 1;

	return 0;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t *input_ids,
		       uint32_t n_input_ids,
		       uint32_t *output_ids,
		       uint32_t n_output_ids)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	if (n_output_ids > 0 && output_ids != NULL)
		output_ids[0] = 0;

	return 0;
}


static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction, uint32_t port_id, const struct spa_port_info **info)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	*info = &this->info;

	return 0;
}

static void recycle_buffer(struct state *this, uint32_t buffer_id)
{
	struct buffer *b = &this->buffers[buffer_id];

	if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_OUT)) {
		spa_log_trace(this->log, NAME " %p: recycle buffer %u", this, buffer_id);
		spa_list_append(&this->free, &b->link);
		SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_OUT);
	}
}

static int port_get_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t *index,
			   struct spa_pod **param,
			   struct spa_pod_builder *builder)
{
	struct state *this = SPA_CONTAINER_OF(node, struct state, node);

	if (!this->have_format)
		return -EIO;
	if (*index > 0)
		return 0;

	*param = spa_pod_builder_object(builder,
		SPA_ID_PARAM_Format, SPA_ID_OBJECT_Format,
		"I", SPA_MEDIA_TYPE_audio,
		"I", SPA_MEDIA_SUBTYPE_raw,
		":", SPA_FORMAT_AUDIO_format,   "I", this->current_format.info.raw.format,
		":", SPA_FORMAT_AUDIO_layout,   "i", this->current_format.info.raw.layout,
		":", SPA_FORMAT_AUDIO_rate,     "i", this->current_format.info.raw.rate,
		":", SPA_FORMAT_AUDIO_channels, "i", this->current_format.info.raw.channels);

	return 1;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{
	struct state *this;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_ID_PARAM_List:
	{
		uint32_t list[] = { SPA_ID_PARAM_EnumFormat,
				    SPA_ID_PARAM_Format,
				    SPA_ID_PARAM_Buffers,
				    SPA_ID_PARAM_Meta };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b, id, SPA_ID_OBJECT_ParamList,
				":", SPA_PARAM_LIST_id, "I", list[*index]);
		else
			return 0;
		break;
	}
	case SPA_ID_PARAM_EnumFormat:
		return spa_alsa_enum_format(this, index, filter, result, builder);

	case SPA_ID_PARAM_Format:
		if ((res = port_get_format(node, direction, port_id, index, &param, &b)) <= 0)
			return res;
		break;

	case SPA_ID_PARAM_Buffers:
		if (!this->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_pod_builder_object(&b,
			id, SPA_ID_OBJECT_ParamBuffers,
			":", SPA_PARAM_BUFFERS_buffers, "ir", 2,
				SPA_POD_PROP_MIN_MAX(1, MAX_BUFFERS),
			":", SPA_PARAM_BUFFERS_blocks,  "i", 1,
			":", SPA_PARAM_BUFFERS_size,    "iru", this->props.max_latency *
							      this->frame_size,
				SPA_POD_PROP_MIN_MAX(this->props.min_latency * this->frame_size,
						     INT32_MAX),
			":", SPA_PARAM_BUFFERS_stride,  "i", this->frame_size,
			":", SPA_PARAM_BUFFERS_align,   "i", 16);
		break;

	case SPA_ID_PARAM_Meta:
		if (!this->have_format)
			return -EIO;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, SPA_ID_OBJECT_ParamMeta,
				":", SPA_PARAM_META_type, "I", SPA_META_Header,
				":", SPA_PARAM_META_size, "i", sizeof(struct spa_meta_header));
			break;
		default:
			return 0;
		}
		break;

	case SPA_ID_PARAM_IO:
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, SPA_ID_OBJECT_ParamIO,
				":", SPA_PARAM_IO_id,   "I", SPA_ID_IO_Buffers,
				":", SPA_PARAM_IO_size, "i", sizeof(struct spa_io_buffers));
			break;
		case 1:
			param = spa_pod_builder_object(&b,
				id, SPA_ID_OBJECT_ParamIO,
				":", SPA_PARAM_IO_id,   "I", SPA_ID_IO_Clock,
				":", SPA_PARAM_IO_size, "i", sizeof(struct spa_io_clock));
			break;
		default:
			return 0;
		}
		break;

	default:
		return -ENOENT;
	}

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int clear_buffers(struct state *this)
{
	if (this->n_buffers > 0) {
		spa_list_init(&this->free);
		spa_list_init(&this->ready);
		this->n_buffers = 0;
	}
	return 0;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags, const struct spa_pod *format)
{
	struct state *this = SPA_CONTAINER_OF(node, struct state, node);
	int err;

	if (format == NULL) {
		spa_alsa_pause(this, false);
		clear_buffers(this);
		spa_alsa_close(this);
		this->have_format = false;
	} else {
		struct spa_audio_info info = { 0 };

		spa_pod_object_parse(format,
			"I", &info.media_type,
			"I", &info.media_subtype);

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		if ((err = spa_alsa_set_format(this, &info, flags)) < 0)
			return err;

		this->current_format = info;
		this->have_format = true;
	}

	if (this->have_format) {
		this->info.rate = this->rate;
	}

	return 0;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(node, direction, port_id), -EINVAL);

	if (id == SPA_ID_PARAM_Format) {
		return port_set_format(node, direction, port_id, flags, param);
	}
	else
		return -ENOENT;
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct state *this;
	int res;
	int i;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (!this->have_format)
		return -EIO;

	if (this->n_buffers > 0) {
		spa_alsa_pause(this, false);
		if ((res = clear_buffers(this)) < 0)
			return res;
	}
	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &this->buffers[i];
		struct spa_data *d = buffers[i]->datas;

		b->buf = buffers[i];
		b->flags = 0;

		b->h = spa_buffer_find_meta_data(b->buf, SPA_META_Header, sizeof(*b->h));

		if (!((d[0].type == SPA_DATA_MemFd ||
		       d[0].type == SPA_DATA_DmaBuf ||
		       d[0].type == SPA_DATA_MemPtr) && d[0].data != NULL)) {
			spa_log_error(this->log, NAME " %p: need mapped memory", this);
			return -EINVAL;
		}
		spa_list_append(&this->free, &b->link);

		this->threshold = SPA_MIN(d[0].maxsize / this->frame_size,
				this->props.max_latency);
	}
	this->n_buffers = n_buffers;

	return 0;
}


static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_pod **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(buffers != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (this->n_buffers == 0)
		return -EIO;

	return -ENOTSUP;
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (id == SPA_ID_IO_Buffers)
		this->io = data;
	else if (id == SPA_ID_IO_Clock)
		this->clock = data;
	else
		return -ENOENT;

	return 0;
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(port_id == 0, -EINVAL);

	if (this->n_buffers == 0)
		return -EIO;

	if (buffer_id >= this->n_buffers)
		return -EINVAL;

	recycle_buffer(this, buffer_id);

	return 0;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction, uint32_t port_id, const struct spa_command *command)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);
	return -ENOTSUP;
}

static int impl_node_process(struct spa_node *node)
{
	struct state *this;
	struct spa_io_buffers *io;
	struct buffer *b;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);
	io = this->io;
	spa_return_val_if_fail(io != NULL, -EIO);

	if (io->status == SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_HAVE_BUFFER;

	if (io->buffer_id < this->n_buffers) {
		recycle_buffer(this, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}

	if (spa_list_is_empty(&this->ready))
		return -EPIPE;

	b = spa_list_first(&this->ready, struct buffer, link);
	spa_list_remove(&b->link);

	spa_log_trace(this->log, NAME " %p: dequeue buffer %d", node, b->buf->id);

	io->buffer_id = b->buf->id;
	io->status = SPA_STATUS_HAVE_BUFFER;

	return SPA_STATUS_HAVE_BUFFER;
}

static const struct spa_dict_item node_info_items[] = {
	{ "media.class", "Audio/Source" },
	{ "node.driver", "true" },
};

static const struct spa_dict node_info = {
	node_info_items,
	SPA_N_ELEMENTS(node_info_items)
};

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	&node_info,
	impl_node_enum_params,
	impl_node_set_param,
	impl_node_send_command,
	impl_node_set_callbacks,
	impl_node_get_n_ports,
	impl_node_get_port_ids,
	impl_node_add_port,
	impl_node_remove_port,
	impl_node_port_get_info,
	impl_node_port_enum_params,
	impl_node_port_set_param,
	impl_node_port_use_buffers,
	impl_node_port_alloc_buffers,
	impl_node_port_set_io,
	impl_node_port_reuse_buffer,
	impl_node_port_send_command,
	impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct state *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct state *) handle;

	if (interface_id == SPA_ID_INTERFACE_Node)
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct state);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct state *this;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct state *) handle;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_ID_INTERFACE_Log)
			this->log = support[i].data;
		else if (support[i].type == SPA_ID_INTERFACE_DataLoop)
			this->data_loop = support[i].data;
		else if (support[i].type == SPA_ID_INTERFACE_MainLoop)
			this->main_loop = support[i].data;
	}
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data loop is needed");
		return -EINVAL;
	}
	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main loop is needed");
		return -EINVAL;
	}

	this->node = impl_node;
	this->stream = SND_PCM_STREAM_CAPTURE;
	reset_props(&this->props);

	this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
			   SPA_PORT_INFO_FLAG_LIVE |
			   SPA_PORT_INFO_FLAG_PHYSICAL |
			   SPA_PORT_INFO_FLAG_TERMINAL;

	spa_list_init(&this->free);
	spa_list_init(&this->ready);

	for (i = 0; info && i < info->n_items; i++) {
		if (!strcmp(info->items[i].key, "alsa.card")) {
			snprintf(this->props.device, 63, "%s", info->items[i].value);
		}
	}
	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_ID_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];

	return 1;
}

static const struct spa_dict_item info_items[] = {
	{ "factory.author", "Wim Taymans <wim.taymans@gmail.com>" },
	{ "factory.description", "Record audio with the alsa API" },
};

static const struct spa_dict info = {
	info_items,
	SPA_N_ELEMENTS(info_items)
};

const struct spa_handle_factory spa_alsa_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
