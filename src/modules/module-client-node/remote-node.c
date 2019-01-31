/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/mman.h>

#include <spa/pod/parser.h>
#include <spa/debug/types.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"

#include "extensions/protocol-native.h"
#include "extensions/client-node.h"

#define MAX_MIX	4096

/** \cond */

struct mapping {
	void *ptr;
	struct pw_map_range map;
	int prot;
};

struct mem {
	uint32_t id;
	int fd;
	uint32_t flags;
	uint32_t ref;
	struct mapping map;
};

struct buffer_mem {
	uint32_t mem_id;
	struct mapping map;
};

struct buffer {
	uint32_t id;
	struct spa_buffer *buf;
	struct buffer_mem *mem;
	uint32_t n_mem;
};

struct mix {
	struct spa_list link;
	struct pw_port *port;
	uint32_t mix_id;
	struct pw_port_mix mix;
	struct pw_array buffers;
	bool active;
};

struct node_data {
	struct pw_remote *remote;
	struct pw_core *core;

	uint32_t remote_id;
	int rtwritefd;
	struct spa_source *rtsocket_source;

	struct mix mix_pool[MAX_MIX];
	struct spa_list mix[2];
	struct spa_list free_mix;

        struct pw_array mems;

	struct pw_node *node;
	struct spa_hook node_listener;
	bool do_free;

        struct pw_client_node_proxy *node_proxy;
	struct spa_hook node_proxy_listener;
	struct spa_hook proxy_listener;

	struct spa_io_position *position;

	struct spa_graph_node_callbacks callbacks;
        void *callbacks_data;

	struct spa_graph_state state;
	struct spa_graph_link link;
};

/** \endcond */

static int
do_remove_source(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct node_data *d = user_data;

	if (d->rtsocket_source) {
		pw_loop_destroy_source(d->core->data_loop, d->rtsocket_source);
		d->rtsocket_source = NULL;
	}
        return 0;
}


static void unhandle_socket(struct node_data *data)
{
        pw_loop_invoke(data->core->data_loop,
                       do_remove_source, 1, NULL, 0, true, data);
}

static void
on_rtsocket_condition(void *user_data, int fd, enum spa_io mask)
{
	struct pw_proxy *proxy = user_data;
	struct node_data *data = proxy->user_data;
	struct spa_graph_node *node = &data->node->rt.root;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("got error");
		unhandle_socket(data);
		return;
	}

	if (mask & SPA_IO_IN) {
		uint64_t cmd;

		if (read(fd, &cmd, sizeof(uint64_t)) != sizeof(uint64_t) || cmd != 1)
			pw_log_warn("proxy %p: read %"PRIu64" failed %m", proxy, cmd);

		pw_log_trace("remote %p: process %p", data->remote, proxy);
		spa_graph_run(node->graph);
	}
}

static struct mem *find_mem(struct node_data *data, uint32_t id)
{
	struct mem *m;
	pw_array_for_each(m, &data->mems) {
		if (m->id == id)
			return m;
	}
	return NULL;
}

static struct mem *find_mem_ptr(struct node_data *data, void *ptr)
{
	struct mem *m;
	pw_array_for_each(m, &data->mems) {
		if (m->map.ptr == ptr)
			return m;
	}
	return NULL;
}

static void *mem_map(struct node_data *data, struct mapping *map,
		int fd, int prot, uint32_t offset, uint32_t size)
{
	struct mapping m;
	void *ptr;

	pw_map_range_init(&m.map, offset, size, data->core->sc_pagesize);

	if (map->ptr == NULL || map->map.offset != m.map.offset || map->map.size != m.map.size) {
		map->ptr = mmap(map->ptr, m.map.size, prot, MAP_SHARED, fd, m.map.offset);
		if (map->ptr == MAP_FAILED) {
			pw_log_error("remote %p: Failed to mmap memory %d: %m", data, size);
			return NULL;
		}
		map->map = m.map;
	}
	ptr = SPA_MEMBER(map->ptr, map->map.start, void);
	pw_log_debug("remote %p: fd %d mapped %d %d %p", data, fd, offset, size, ptr);

	return ptr;
}

static void *mem_unmap(struct node_data *data, void *ptr, struct pw_map_range *range)
{
	if (ptr != NULL) {
                if (munmap(SPA_MEMBER(ptr, -range->start, void), range->size) < 0)
			pw_log_warn("failed to unmap: %m");
	}
	return NULL;
}

static void clear_mem(struct node_data *data, struct mem *m)
{
	if (m->fd != -1) {
		bool has_ref = false;
		int fd;
		struct mem *m2;

		pw_log_debug("remote %p: clear mem %d", data, m->id);

		fd = m->fd;
		m->fd = -1;
		m->id = SPA_ID_INVALID;

		pw_array_for_each(m2, &data->mems) {
			if (m2->fd == fd) {
				has_ref = true;
				break;
			}
		}
		if (!has_ref) {
			m->map.ptr = mem_unmap(data, m->map.ptr, &m->map.map);
			close(fd);
		}
	}
}

static void clean_transport(struct node_data *data)
{
	struct mem *m;

	if (data->rtsocket_source == NULL)
		return;

	unhandle_socket(data);

	pw_array_for_each(m, &data->mems)
		clear_mem(data, m);
	pw_array_clear(&data->mems);

	close(data->rtwritefd);
	data->remote_id = SPA_ID_INVALID;
}

static void mix_init(struct mix *mix, struct pw_port *port, uint32_t mix_id)
{
	mix->port = port;
	mix->mix_id = mix_id;
	pw_port_init_mix(port, &mix->mix);
	mix->active = false;
	pw_array_init(&mix->buffers, 32);
	pw_array_ensure_size(&mix->buffers, sizeof(struct buffer) * 64);
}

static int
do_deactivate_mix(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct mix *mix = user_data;
	spa_graph_port_remove(&mix->mix.port);
        return 0;
}

static int
deactivate_mix(struct node_data *data, struct mix *mix)
{
	if (mix->active) {
		pw_log_debug("node %p: mix %p deactivate", data, mix);
		pw_loop_invoke(data->core->data_loop,
                       do_deactivate_mix, SPA_ID_INVALID, NULL, 0, true, mix);
		mix->active = false;
	}
	return 0;
}

static int
do_activate_mix(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct mix *mix = user_data;
	spa_graph_port_add(&mix->port->rt.mix_node, &mix->mix.port);
        return 0;
}

static int
activate_mix(struct node_data *data, struct mix *mix)
{
	if (!mix->active) {
		pw_log_debug("node %p: mix %p activate", data, mix);
		pw_loop_invoke(data->core->data_loop,
                       do_activate_mix, SPA_ID_INVALID, NULL, 0, false, mix);
		mix->active = true;
	}
	return 0;
}

static struct mix *find_mix(struct node_data *data,
		enum spa_direction direction, uint32_t port_id, uint32_t mix_id)
{
	struct mix *mix;

	spa_list_for_each(mix, &data->mix[direction], link) {
		if (mix->port->port_id == port_id &&
		    mix->mix_id == mix_id)
			return mix;
	}
	return NULL;
}

static struct mix *ensure_mix(struct node_data *data,
		enum spa_direction direction, uint32_t port_id, uint32_t mix_id)
{
	struct mix *mix;
	struct pw_port *port;

	if ((mix = find_mix(data, direction, port_id, mix_id)))
		return mix;

	if (spa_list_is_empty(&data->free_mix))
		return NULL;

	port = pw_node_find_port(data->node, direction, port_id);
	if (port == NULL)
		return NULL;

	mix = spa_list_first(&data->free_mix, struct mix, link);
	spa_list_remove(&mix->link);

	mix_init(mix, port, mix_id);
	spa_list_append(&data->mix[direction], &mix->link);

	return mix;
}

static void client_node_add_mem(void *object,
				uint32_t mem_id,
				uint32_t type, int memfd, uint32_t flags)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct mem *m;

	m = find_mem(data, mem_id);
	if (m) {
		pw_log_warn("duplicate mem %u, fd %d, flags %d",
			     mem_id, memfd, flags);
		return;
	}

	m = pw_array_add(&data->mems, sizeof(struct mem));
	pw_log_debug("add mem %u, fd %d, flags %d", mem_id, memfd, flags);

	m->id = mem_id;
	m->fd = memfd;
	m->flags = flags;
	m->ref = 0;
	m->map.map = PW_MAP_RANGE_INIT;
	m->map.ptr = NULL;
}

static void client_node_transport(void *object, uint32_t node_id,
                                  int readfd, int writefd)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_remote *remote = proxy->remote;

	clean_transport(data);

	data->remote_id = node_id;

	pw_log_debug("remote-node %p: create transport with fds %d %d for node %u",
		proxy, readfd, writefd, node_id);

        data->rtwritefd = writefd;
        data->rtsocket_source = pw_loop_add_io(remote->core->data_loop,
                                               readfd,
                                               SPA_IO_ERR | SPA_IO_HUP,
                                               true, on_rtsocket_condition, proxy);
	if (data->node->active)
		pw_client_node_proxy_set_active(data->node_proxy, true);

	pw_remote_events_exported(remote, proxy->id, node_id);
}

static void add_port_update(struct pw_proxy *proxy, struct pw_port *port, uint32_t change_mask)
{
	struct node_data *data = proxy->user_data;
	const struct spa_port_info *port_info = NULL;
	struct spa_port_info pi;
	uint32_t n_params = 0;
	struct spa_pod **params = NULL;

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_PARAMS) {
		uint32_t idx1, idx2, id;
		uint8_t buf[2048];
		struct spa_pod_builder b = { 0 };

		for (idx1 = 0;;) {
			struct spa_pod *param;

			spa_pod_builder_init(&b, buf, sizeof(buf));
                        if (spa_node_port_enum_params(port->node->node,
						      port->direction, port->port_id,
						      SPA_PARAM_List, &idx1,
						      NULL, &param, &b) <= 0)
                                break;

			spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamList, NULL,
				SPA_PARAM_LIST_id, SPA_POD_Id(&id));

			params = realloc(params, sizeof(struct spa_pod *) * (n_params + 1));
			params[n_params++] = pw_spa_pod_copy(param);

			for (idx2 = 0;;) {
				spa_pod_builder_init(&b, buf, sizeof(buf));
	                        if (spa_node_port_enum_params(port->node->node,
							      port->direction, port->port_id,
							      id, &idx2,
							      NULL, &param, &b) <= 0)
	                                break;

				params = realloc(params, sizeof(struct spa_pod *) * (n_params + 1));
				params[n_params++] = pw_spa_pod_copy(param);
			}
                }
	}
	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_INFO) {
		spa_node_port_get_info(port->node->node, port->direction, port->port_id, &port_info);
		pi = * port_info;
		pi.flags &= ~SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
	}

        pw_client_node_proxy_port_update(data->node_proxy,
                                         port->direction,
                                         port->port_id,
                                         change_mask,
                                         n_params,
                                         (const struct spa_pod **)params,
					 &pi);
	if (params) {
		while (n_params > 0)
			free(params[--n_params]);
		free(params);
	}
}

static void
client_node_set_param(void *object, uint32_t seq, uint32_t id, uint32_t flags,
		      const struct spa_pod *param)
{
	pw_log_warn("set param not implemented");
}


static void
client_node_set_io(void *object,
		   uint32_t id,
		   uint32_t memid,
		   uint32_t offset,
		   uint32_t size)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct mem *m;
	void *ptr;

	if (memid == SPA_ID_INVALID) {
		ptr = NULL;
		size = 0;
	}
	else {
		m = find_mem(data, memid);
		if (m == NULL) {
			pw_log_warn("unknown memory id %u", memid);
			return;
		}
		ptr = mem_map(data, &m->map, m->fd,
			PROT_READ|PROT_WRITE, offset, size);
		if (ptr == NULL)
			return;
		m->ref++;
	}

	pw_log_debug("node %p: set io %s %p", proxy,
			spa_debug_type_find_name(spa_type_io, id), ptr);

	if (id == SPA_IO_Position) {
		if (ptr == NULL && data->position) {
			m = find_mem_ptr(data, data->position);
			if (m && --m->ref == 0)
				clear_mem(data, m);
		}
		data->position = ptr;
	}
	spa_node_set_io(data->node->node, id, ptr, size);
}

static void client_node_event(void *object, const struct spa_event *event)
{
	pw_log_warn("unhandled node event %d", SPA_EVENT_TYPE(event));
}

static int
do_pause_source(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct node_data *d = user_data;
	pw_loop_update_io(d->core->data_loop,
			  d->rtsocket_source,
			  SPA_IO_ERR | SPA_IO_HUP);
        return 0;
}

static void client_node_command(void *object, uint32_t seq, const struct spa_command *command)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_remote *remote = proxy->remote;
	int res;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Pause:
		pw_log_debug("node %p: pause %d", proxy, seq);

		if (data->rtsocket_source) {
			pw_loop_invoke(data->core->data_loop,
				do_pause_source, 1, NULL, 0, true, data);
		}
		if ((res = spa_node_send_command(data->node->node, command)) < 0)
			pw_log_warn("node %p: pause failed", proxy);

		pw_client_node_proxy_done(data->node_proxy, seq, res);
		break;
	case SPA_NODE_COMMAND_Start:
		pw_log_debug("node %p: start %d", proxy, seq);

		if ((res = spa_node_send_command(data->node->node, command)) < 0) {
			pw_log_warn("node %p: start failed", proxy);
		}
		else if (data->rtsocket_source) {
			pw_loop_update_io(remote->core->data_loop,
				  data->rtsocket_source,
				  SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);
		}

		pw_client_node_proxy_done(data->node_proxy, seq, res);
		break;
	default:
		pw_log_warn("unhandled node command %d", SPA_NODE_COMMAND_ID(command));
		pw_client_node_proxy_done(data->node_proxy, seq, -ENOTSUP);
	}
}

static void
client_node_add_port(void *object, uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	pw_log_warn("add port not supported");
}

static void
client_node_remove_port(void *object, uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	pw_log_warn("remove port not supported");
}

static void clear_buffers(struct node_data *data, struct mix *mix)
{
	struct pw_port *port = mix->port;
        struct buffer *b;
	uint32_t i;
	int res;

        pw_log_debug("port %p: clear buffers %d", port, mix->mix_id);
	if ((res = pw_port_use_buffers(port, mix->mix_id, NULL, 0)) < 0) {
		pw_log_error("port %p: error clear buffers %s", port, spa_strerror(res));
		return;
	}

        pw_array_for_each(b, &mix->buffers) {
		for (i = 0; i < b->n_mem; i++) {
			struct buffer_mem *bm = &b->mem[i];
			struct mem *m;

			pw_log_debug("port %p: clear buffer %d mem %d",
				port, b->id, bm->mem_id);

			m = find_mem(data, bm->mem_id);
			if (m && --m->ref == 0)
				clear_mem(data, m);
		}
		b->n_mem = 0;
                free(b->buf);
        }
	mix->buffers.size = 0;
}

static void
client_node_port_set_param(void *object,
			   uint32_t seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t flags,
			   const struct spa_pod *param)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_port *port;
	int res;

	port = pw_node_find_port(data->node, direction, port_id);
	if (port == NULL) {
		res = -EINVAL;
		goto done;
	}

        pw_log_debug("port %p: set param %d %p", port, id, param);

        if (id == SPA_PARAM_Format) {
		struct mix *mix;
		spa_list_for_each(mix, &data->mix[direction], link) {
			if (mix->port->port_id == port_id)
				clear_buffers(data, mix);
		}
	}

	res = pw_port_set_param(port, SPA_ID_INVALID, id, flags, param);
	if (res < 0)
		goto done;

	add_port_update(proxy, port,
			PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
			PW_CLIENT_NODE_PORT_UPDATE_INFO);

      done:
	pw_client_node_proxy_done(data->node_proxy, seq, res);
}

static void
client_node_port_use_buffers(void *object,
			     uint32_t seq,
			     enum spa_direction direction, uint32_t port_id, uint32_t mix_id,
			     uint32_t n_buffers, struct pw_client_node_buffer *buffers)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct buffer *bid;
	uint32_t i, j;
	struct spa_buffer *b, **bufs;
	struct mix *mix;
	int res, prot;

	mix = ensure_mix(data, direction, port_id, mix_id);
	if (mix == NULL) {
		res = -EINVAL;
		goto done;
	}

	prot = PROT_READ | (direction == SPA_DIRECTION_OUTPUT ? PROT_WRITE : 0);

	/* clear previous buffers */
	clear_buffers(data, mix);

	bufs = alloca(n_buffers * sizeof(struct spa_buffer *));

	for (i = 0; i < n_buffers; i++) {
		struct buffer_mem bmem = { 0, };
		size_t size;
		off_t offset;
		struct mem *m;

		m = find_mem(data, buffers[i].mem_id);
		if (m == NULL) {
			pw_log_error("unknown memory id %u", buffers[i].mem_id);
			res = -EINVAL;
			goto cleanup;
		}

		bid = pw_array_add(&mix->buffers, sizeof(struct buffer));
		bid->id = i;

		bmem.mem_id = m->id;
		bmem.map.ptr = mem_map(data, &bmem.map, m->fd, prot,
				buffers[i].offset, buffers[i].size);
		if (bmem.map.ptr == NULL) {
			res = -errno;
			goto cleanup;
		}
		if (mlock(bmem.map.ptr, bmem.map.map.size) < 0)
			pw_log_warn("Failed to mlock memory %u %u: %m",
					bmem.map.map.offset, bmem.map.map.size);

		size = sizeof(struct spa_buffer);
		size += sizeof(struct buffer_mem);
		for (j = 0; j < buffers[i].buffer->n_metas; j++)
			size += sizeof(struct spa_meta);
		for (j = 0; j < buffers[i].buffer->n_datas; j++) {
			size += sizeof(struct spa_data);
			size += sizeof(struct buffer_mem);
		}

		b = bid->buf = malloc(size);
		if (b == NULL) {
			res = -ENOMEM;
			goto cleanup;
		}
		memcpy(b, buffers[i].buffer, sizeof(struct spa_buffer));

		b->metas = SPA_MEMBER(b, sizeof(struct spa_buffer), struct spa_meta);
		b->datas = SPA_MEMBER(b->metas, sizeof(struct spa_meta) * b->n_metas,
				       struct spa_data);
		bid->mem = SPA_MEMBER(b->datas, sizeof(struct spa_data) * b->n_datas,
			       struct buffer_mem);
		bid->n_mem = 0;

		bid->mem[bid->n_mem++] = bmem;
		m->ref++;

		pw_log_debug("add buffer %d %d %u %u", m->id,
				bid->id, bmem.map.map.offset, bmem.map.map.size);

		offset = 0;
		for (j = 0; j < b->n_metas; j++) {
			struct spa_meta *m = &b->metas[j];
			memcpy(m, &buffers[i].buffer->metas[j], sizeof(struct spa_meta));
			m->data = SPA_MEMBER(bmem.map.ptr, offset, void);
			offset += SPA_ROUND_UP_N(m->size, 8);
		}

		for (j = 0; j < b->n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			memcpy(d, &buffers[i].buffer->datas[j], sizeof(struct spa_data));
			d->chunk =
			    SPA_MEMBER(bmem.map.ptr, offset + sizeof(struct spa_chunk) * j,
				       struct spa_chunk);

			if (d->type == SPA_DATA_MemFd || d->type == SPA_DATA_DmaBuf) {
				uint32_t mem_id = SPA_PTR_TO_UINT32(d->data);
				struct mem *bm = find_mem(data, mem_id);
				struct buffer_mem bm2;

				if (bm == NULL) {
					pw_log_error("unknown buffer mem %u", mem_id);
					res = -EINVAL;
					goto cleanup;
				}

				d->fd = bm->fd;
				bm->ref++;
				bm2.mem_id = bm->id;
				bm2.map.ptr = NULL;
				d->data = bm2.map.ptr;

				bid->mem[bid->n_mem++] = bm2;

				pw_log_debug(" data %d %u -> fd %d maxsize %d",
						j, bm->id, bm->fd, d->maxsize);
			} else if (d->type == SPA_DATA_MemPtr) {
				int offs = SPA_PTR_TO_INT(d->data);
				d->data = SPA_MEMBER(bmem.map.ptr, offs, void);
				d->fd = -1;
				pw_log_debug(" data %d %u -> mem %p maxsize %d",
						j, bid->id, d->data, d->maxsize);
			} else {
				pw_log_warn("unknown buffer data type %d", d->type);
			}
		}
		bufs[i] = b;
	}

	res = pw_port_use_buffers(mix->port, mix->mix_id, bufs, n_buffers);

      done:
	pw_client_node_proxy_done(data->node_proxy, seq, res);
	return;

     cleanup:
	clear_buffers(data, mix);
	goto done;

}

static void
client_node_port_command(void *object,
                         uint32_t direction,
                         uint32_t port_id,
                         const struct spa_command *command)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_port *port;

	port = pw_node_find_port(data->node, direction, port_id);
	if (port == NULL)
		return;

	pw_port_send_command(port, true, command);
}

static void
client_node_port_set_io(void *object,
                        uint32_t seq,
                        uint32_t direction,
                        uint32_t port_id,
                        uint32_t mix_id,
                        uint32_t id,
                        uint32_t memid,
                        uint32_t offset,
                        uint32_t size)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct mix *mix;
	struct mem *m;
	void *ptr;

	mix = ensure_mix(data, direction, port_id, mix_id);
	if (mix == NULL)
		return;

	if (memid == SPA_ID_INVALID) {
		ptr = NULL;
		size = 0;
	}
	else {
		m = find_mem(data, memid);
		if (m == NULL) {
			pw_log_warn("unknown memory id %u", memid);
			return;
		}
		ptr = mem_map(data, &m->map, m->fd,
			PROT_READ|PROT_WRITE, offset, size);
		if (ptr == NULL)
			return;

		m->ref++;
	}

	pw_log_debug("port %p: set io %s %p", mix->port,
			spa_debug_type_find_name(spa_type_io, id), ptr);

	if (id == SPA_IO_Buffers) {
		if (ptr == NULL && mix->mix.io) {
			deactivate_mix(data, mix);
                        m = find_mem_ptr(data, mix->mix.io);
                        if (m && --m->ref == 0)
                                clear_mem(data, m);
                }
		mix->mix.io = ptr;
		if (ptr)
			activate_mix(data, mix);
	} else {
		spa_node_port_set_io(mix->port->node->node,
				     direction, port_id,
				     id,
				     ptr,
				     size);
	}
}

static const struct pw_client_node_proxy_events client_node_events = {
	PW_VERSION_CLIENT_NODE_PROXY_EVENTS,
	.add_mem = client_node_add_mem,
	.transport = client_node_transport,
	.set_param = client_node_set_param,
	.set_io = client_node_set_io,
	.event = client_node_event,
	.command = client_node_command,
	.add_port = client_node_add_port,
	.remove_port = client_node_remove_port,
	.port_set_param = client_node_port_set_param,
	.port_use_buffers = client_node_port_use_buffers,
	.port_command = client_node_port_command,
	.port_set_io = client_node_port_set_io,
};

static void do_node_init(struct pw_proxy *proxy)
{
	struct node_data *data = proxy->user_data;
	struct pw_port *port;

	pw_log_debug("%p: init", data);
        pw_client_node_proxy_update(data->node_proxy,
                                    PW_CLIENT_NODE_UPDATE_MAX_INPUTS |
				    PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS |
				    PW_CLIENT_NODE_UPDATE_PARAMS,
				    data->node->info.max_input_ports,
				    data->node->info.max_output_ports,
				    0, NULL, NULL);

	spa_list_for_each(port, &data->node->input_ports, link) {
		add_port_update(proxy, port,
				PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
				PW_CLIENT_NODE_PORT_UPDATE_INFO);
	}
	spa_list_for_each(port, &data->node->output_ports, link) {
		add_port_update(proxy, port,
				PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
				PW_CLIENT_NODE_PORT_UPDATE_INFO);
	}
        pw_client_node_proxy_done(data->node_proxy, 0, 0);
}

static void clear_mix(struct node_data *data, struct mix *mix)
{
	clear_buffers(data, mix);
	pw_array_clear(&mix->buffers);

	deactivate_mix(data, mix);

	spa_list_remove(&mix->link);
	spa_list_append(&data->free_mix, &mix->link);
}

static void clean_node(struct node_data *d)
{
	struct mix *mix, *tmp;

	if (d->remote_id != SPA_ID_INVALID) {
		spa_list_for_each_safe(mix, tmp, &d->mix[SPA_DIRECTION_INPUT], link)
			clear_mix(d, mix);
		spa_list_for_each_safe(mix, tmp, &d->mix[SPA_DIRECTION_OUTPUT], link)
			clear_mix(d, mix);
	}
	clean_transport(d);
}

static void node_destroy(void *data)
{
	struct node_data *d = data;
	struct pw_remote *remote = d->remote;
	struct pw_proxy *proxy = (struct pw_proxy*) d->node_proxy;

	pw_log_debug("%p: destroy", d);

	if (remote->core_proxy)
		pw_core_proxy_destroy(remote->core_proxy, proxy);

	clean_node(d);

	spa_hook_remove(&d->proxy_listener);
}

static void node_info_changed(void *data, const struct pw_node_info *info)
{
	struct node_data *d = data;
	uint32_t change_mask = 0;

	pw_log_debug("info changed %p", d);

	if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) {
		change_mask |= PW_CLIENT_NODE_UPDATE_PROPS;
	}
        pw_client_node_proxy_update(d->node_proxy,
				    change_mask,
				    0, 0,
				    0, NULL,
				    info->props);
}

static void node_active_changed(void *data, bool active)
{
	struct node_data *d = data;
	pw_log_debug("active %d", active);
	pw_client_node_proxy_set_active(d->node_proxy, active);
}


static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_destroy,
	.info_changed = node_info_changed,
	.active_changed = node_active_changed,
};

static void node_proxy_destroy(void *_data)
{
	struct node_data *data = _data;
	clean_node(data);
	spa_hook_remove(&data->node_listener);
	if (data->do_free)
		pw_node_destroy(data->node);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = node_proxy_destroy,
};

static int remote_impl_signal(void *data)
{
	struct node_data *d = data;
	uint64_t cmd = 1;
	pw_log_trace("remote %p: send process", data);
	write(d->rtwritefd, &cmd, 8);
        return 0;
}

static inline int remote_process(void *data, struct spa_graph_node *node)
{
	struct node_data *d = data;
        spa_debug("remote %p: begin graph", data);
	spa_graph_state_reset(&d->state);
	return d->callbacks.process(d->callbacks_data, node);
}

static const struct spa_graph_node_callbacks impl_root = {
	SPA_VERSION_GRAPH_NODE_CALLBACKS,
	.process = remote_process,
};

static struct pw_proxy *node_export(struct pw_remote *remote, void *object, bool do_free)
{
	struct pw_node *node = object;
	struct pw_proxy *proxy;
	struct node_data *data;
	int i;

	proxy = pw_core_proxy_create_object(remote->core_proxy,
					    "client-node",
					    PW_TYPE_INTERFACE_ClientNode,
					    PW_VERSION_CLIENT_NODE,
					    &node->properties->dict,
					    sizeof(struct node_data));
        if (proxy == NULL)
                return NULL;

	data = pw_proxy_get_user_data(proxy);
	data->remote = remote;
	data->node = node;
	data->do_free = do_free;
	data->core = pw_node_get_core(node);
	data->node_proxy = (struct pw_client_node_proxy *)proxy;
	data->remote_id = SPA_ID_INVALID;

	data->link.signal = remote_impl_signal;
	data->link.signal_data = data;
	data->callbacks = *node->rt.root.callbacks;
	spa_graph_node_set_callbacks(&node->rt.root, &impl_root, data);
	spa_graph_link_add(&node->rt.root, &data->state, &data->link);
	spa_graph_node_add(node->rt.driver, &node->rt.root);

	node->exported = true;

	spa_list_init(&data->free_mix);
	spa_list_init(&data->mix[0]);
	spa_list_init(&data->mix[1]);
	for (i = 0; i < MAX_MIX; i++)
		spa_list_append(&data->free_mix, &data->mix_pool[i].link);

        pw_array_init(&data->mems, 64);
        pw_array_ensure_size(&data->mems, sizeof(struct mem) * 64);

	pw_proxy_add_listener(proxy, &data->proxy_listener, &proxy_events, data);
	pw_node_add_listener(node, &data->node_listener, &node_events, data);

        pw_client_node_proxy_add_listener(data->node_proxy,
					  &data->node_proxy_listener,
					  &client_node_events,
					  proxy);
        do_node_init(proxy);

	return proxy;
}

struct pw_proxy *pw_remote_node_export(struct pw_remote *remote,
		uint32_t type, struct pw_properties *props, void *object)
{
	return node_export(remote, object, false);
}

struct pw_proxy *pw_remote_spa_node_export(struct pw_remote *remote,
		uint32_t type, struct pw_properties *props, void *object)
{
	struct pw_node *node;

	node = pw_node_new(pw_remote_get_core(remote), NULL, props, 0);
	if (node == NULL)
		return NULL;

	pw_node_set_implementation(node, (struct spa_node*)object);
	pw_node_register(node, NULL, NULL, NULL);
	pw_node_set_active(node, true);

	return node_export(remote, node, true);
}