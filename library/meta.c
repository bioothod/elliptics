/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
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
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "elliptics.h"

#include "elliptics/packet.h"
#include "elliptics/interface.h"

#define dnet_map_log(n, mask, fmt, a...) do { if ((n)) dnet_log((n), mask, fmt, ##a); else fprintf(stderr, fmt, ##a); } while (0)

struct dnet_meta *dnet_meta_search(struct dnet_node *n, struct dnet_meta_container *mc, uint32_t type)
{
	void *data = mc->data;
	uint32_t size = mc->size;

	struct dnet_meta m, *found = NULL;

	while (size) {
		if (size < sizeof(struct dnet_meta)) {
			dnet_map_log(n, DNET_LOG_ERROR, "Metadata size %u is too small, min %zu, searching for type 0x%x.\n",
					size, sizeof(struct dnet_meta), type);
			break;
		}

		m = *(struct dnet_meta *)data;
		//dnet_convert_meta(&m);

		if (m.size + sizeof(struct dnet_meta) > size) {
			dnet_map_log(n, DNET_LOG_ERROR, "Metadata entry broken: entry size %u, type: 0x%x, struct size: %zu, "
					"total size left: %u, searching for type: 0x%x.\n",
					m.size, m.type, sizeof(struct dnet_meta), size, type);
			break;
		}

		if (m.type == type) {
			found = data;
			break;
		}

		data += m.size + sizeof(struct dnet_meta);
		size -= m.size + sizeof(struct dnet_meta);
	}

	return found;
}

void dnet_update_check_metadata_raw(struct dnet_node *n, void *data, int size)
{
	void *ptr = data;
	struct dnet_meta *m;

	while (size) {
		m = ptr;

		ptr += sizeof(struct dnet_meta) + m->size;
		size -= sizeof(struct dnet_meta) + m->size;

		if (m->type == DNET_META_CHECK_STATUS) {
			struct timeval tv;
			struct dnet_meta_check_status *c = (struct dnet_meta_check_status *)m->data;

			gettimeofday(&tv, NULL);

			c->tsec = tv.tv_sec;
			c->tnsec = tv.tv_usec * 1000;
			c->status = 0;

			dnet_convert_meta_check_status(c);
			dnet_log(n, DNET_LOG_DSA, "Metadata updated\n");
		}

		dnet_convert_meta(m);
	}
}

int dnet_write_metadata(struct dnet_node *n, struct dnet_meta_container *mc, int convert)
{
	if (convert) {
		void *ptr = mc->data;
		int size = mc->size;
		struct dnet_meta *m;

		while (size) {
			m = ptr;

			ptr += sizeof(struct dnet_meta) + m->size;
			size -= sizeof(struct dnet_meta) + m->size;

			if (m->type == DNET_META_CHECK_STATUS) {
				struct timeval tv;
				struct dnet_meta_check_status *c = (struct dnet_meta_check_status *)m->data;

				gettimeofday(&tv, NULL);

				c->tsec = tv.tv_sec;
				c->tnsec = tv.tv_usec * 1000;
				c->status = 0;

				dnet_convert_meta_check_status(c);
			}

			dnet_convert_meta(m);
		}
	}

	dnet_log(n, DNET_LOG_DSA, "%s: writing metadata (%u bytes)\n", dnet_dump_id(&mc->id), mc->size);
	return dnet_write_data_wait(n, NULL, 0, &mc->id, mc->data, -1, 0, 0, mc->size, NULL, 0, DNET_IO_FLAGS_META);
}

int dnet_create_write_metadata(struct dnet_node *n, struct dnet_metadata_control *ctl)
{
	struct dnet_meta_container mc;
	struct dnet_meta_check_status *c;
	struct dnet_meta_update *mu;
	struct dnet_meta *m;
	int size = 0, err, nsize = 0;
	void *ns;

	size += sizeof(struct dnet_meta_check_status) + sizeof(struct dnet_meta);
	size += sizeof(struct dnet_meta_update) + sizeof(struct dnet_meta);

	if (ctl->obj && ctl->len)
		size += ctl->len + sizeof(struct dnet_meta);

	if (ctl->groups && ctl->group_num)
		size += ctl->group_num * sizeof(int) + sizeof(struct dnet_meta);

	ns = dnet_node_get_ns(n, &nsize);
	if (ns && nsize)
		size += nsize + sizeof(struct dnet_meta);

	if (!size) {
		err = -EINVAL;
		goto err_out_exit;
	}

	memset(&mc, 0, sizeof(struct dnet_meta_container));
	mc.data = malloc(size);
	if (mc.data) {
		err = -ENOMEM;
		goto err_out_exit;
	}
	memset(mc.data, 0, size);

	m = (struct dnet_meta *)(mc.data);

	c = (struct dnet_meta_check_status *)m->data;
	m->size = sizeof(struct dnet_meta_check_status);
	m->type = DNET_META_CHECK_STATUS;

	/* Check status is undefined for now, it will be filled during actual check */
	memset(c, 0, sizeof(struct dnet_meta_check_status));

	m = (struct dnet_meta *)(m->data + m->size);
	mu = (struct dnet_meta_update *)m->data;
	m->size = sizeof(*mu);
	m->type = DNET_META_UPDATE;
	mu->flags = ctl->update_flags;
	if (!ctl->ts.tv_sec) {
		struct timeval tv;

		gettimeofday(&tv, NULL);
		ctl->ts.tv_sec = tv.tv_sec;
		ctl->ts.tv_nsec = tv.tv_usec * 1000;
	}
	mu->tsec = ctl->ts.tv_sec;
	mu->tnsec = ctl->ts.tv_nsec;

	dnet_convert_meta_update(mu);
	m = (struct dnet_meta *)(m->data + m->size);

	if (ctl->obj && ctl->len) {
		m->size = ctl->len;
		m->type = DNET_META_PARENT_OBJECT;
		memcpy(m->data, ctl->obj, ctl->len);

		m = (struct dnet_meta *)(m->data + m->size);
	}

	if (ctl->groups && ctl->group_num) {
		m->size = ctl->group_num * sizeof(int);
		m->type = DNET_META_GROUPS;
		memcpy(m->data, ctl->groups, ctl->group_num * sizeof(int));

		m = (struct dnet_meta *)(m->data + m->size);
	}

	if (ns && nsize) {
		m->size = nsize;
		m->type = DNET_META_NAMESPACE;
		memcpy(m->data, ns, nsize);

		m = (struct dnet_meta *)(m->data + m->size);
	}

	mc.size = size;
	memcpy(&mc.id, &ctl->id, sizeof(struct dnet_id));

	err = dnet_write_metadata(n, &mc, 1);

	free(mc.data);
err_out_exit:
	return err;
}

