/* Simple Plugin API
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_META_H__
#define __SPA_META_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>

/** \page page_meta Metadata
 *
 * Metadata contains extra information on a buffer.
 */
enum spa_meta_type {
	SPA_META_Header,
	SPA_META_VideoCrop,
	SPA_META_VideoDamage,
};

/**
 * A metadata element.
 *
 * This structure is available on the buffer structure and contains
 * the type of the metadata and a pointer/size to the actual metadata
 * itself.
 */
struct spa_meta {
	uint32_t type;		/**< metadata type, one of enum spa_meta_type */
	void *data;		/**< pointer to metadata */
	uint32_t size;		/**< size of metadata */
};

#define spa_meta_first(m)	((m)->data)
#define spa_meta_end(m)		((m)->data + (m)->size)
#define spa_meta_check(p,m)	((void*)(p) + sizeof(*p) <= spa_meta_end(m))

/**
 * Describes essential buffer header metadata such as flags and
 * timestamps.
 */
struct spa_meta_header {
#define SPA_META_HEADER_FLAG_DISCONT	(1 << 0)	/**< data is not continous with previous buffer */
#define SPA_META_HEADER_FLAG_CORRUPTED	(1 << 1)	/**< data might be corrupted */
#define SPA_META_HEADER_FLAG_MARKER	(1 << 2)	/**< media specific marker */
#define SPA_META_HEADER_FLAG_HEADER	(1 << 3)	/**< data contains a codec specific header */
#define SPA_META_HEADER_FLAG_GAP	(1 << 4)	/**< data contains media neutral data */
#define SPA_META_HEADER_FLAG_DELTA_UNIT	(1 << 5)	/**< cannot be decoded independently */
	uint32_t flags;				/**< flags */
	uint32_t seq;				/**< sequence number, increments with a
						  *  media specific frequency */
	int64_t pts;				/**< presentation timestamp */
	int64_t dts_offset;			/**< decoding timestamp and a difference with pts */
};

/** metadata structure for Region or an array of these for RegionArray */
struct spa_meta_region {
	struct spa_region region;
};

#define spa_meta_region_is_valid(m)	((m)->region.size.width != 0 && (m)->region.size.height != 0)

#define spa_meta_region_for_each(pos,meta)				\
	for (pos = spa_meta_first(meta);				\
	    spa_meta_check(pos, meta);					\
            (pos)++)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_META_H__ */
