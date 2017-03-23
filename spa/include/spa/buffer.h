/* Simple Plugin API
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

#ifndef __SPA_BUFFER_H__
#define __SPA_BUFFER_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaBuffer SpaBuffer;

#define SPA_TYPE__Buffer            "Spa:Pointer:Buffer"
#define SPA_TYPE_BUFFER_BASE        SPA_TYPE__Buffer ":"

/**
 * SpaMetaType:
 * @SPA_META_TYPE_INVALID: invalid metadata, should be ignored
 * @SPA_META_TYPE_HEADER: header metadata
 * @SPA_META_TYPE_POINTER: a generic pointer
 * @SPA_META_TYPE_VIDEO_CROP: video cropping region
 * @SPA_META_TYPE_RINGBUFFER: a ringbuffer
 * @SPA_META_TYPE_SHARED: buffer data and metadata memory can be shared
 */
typedef enum {
  SPA_META_TYPE_INVALID               = 0,
  SPA_META_TYPE_HEADER,
  SPA_META_TYPE_POINTER,
  SPA_META_TYPE_VIDEO_CROP,
  SPA_META_TYPE_RINGBUFFER,
  SPA_META_TYPE_SHARED,
} SpaMetaType;

/**
 * SpaDataType:
 * @SPA_DATA_TYPE_INVALID: invalid data, should be ignored
 * @SPA_DATA_TYPE_MEMPTR: data points to CPU accessible memory
 * @SPA_DATA_TYPE_MEMFD: fd is memfd, data can be mmapped
 * @SPA_DATA_TYPE_DMABUF: fd is dmabuf, data can be mmapped
 * @SPA_DATA_TYPE_ID: data is an id use SPA_PTR_TO_INT32. The definition of
 *          the ID is conveyed in some other way
 */
typedef enum {
  SPA_DATA_TYPE_INVALID               = 0,
  SPA_DATA_TYPE_MEMPTR,
  SPA_DATA_TYPE_MEMFD,
  SPA_DATA_TYPE_DMABUF,
  SPA_DATA_TYPE_ID,
} SpaDataType;

#include <spa/defs.h>
#include <spa/ringbuffer.h>

/**
 * SpaBufferFlags:
 * @SPA_BUFFER_FLAG_NONE: no flag
 * @SPA_BUFFER_FLAG_DISCONT: the buffer marks a data discontinuity
 * @SPA_BUFFER_FLAG_CORRUPTED: the buffer data might be corrupted
 * @SPA_BUFFER_FLAG_MARKER: the buffer contains a media specific marker
 * @SPA_BUFFER_FLAG_HEADER: the buffer contains a header
 * @SPA_BUFFER_FLAG_GAP: the buffer has been constructed to fill a gap
 *                       and contains media neutral data
 * @SPA_BUFFER_FLAG_DELTA_UNIT: the media cannot be decoded independently
 */
typedef enum {
  SPA_BUFFER_FLAG_NONE               =  0,
  SPA_BUFFER_FLAG_DISCONT            = (1 << 0),
  SPA_BUFFER_FLAG_CORRUPTED          = (1 << 1),
  SPA_BUFFER_FLAG_MARKER             = (1 << 2),
  SPA_BUFFER_FLAG_HEADER             = (1 << 3),
  SPA_BUFFER_FLAG_GAP                = (1 << 4),
  SPA_BUFFER_FLAG_DELTA_UNIT         = (1 << 5),
} SpaBufferFlags;

typedef struct {
  SpaBufferFlags flags;
  uint32_t seq;
  int64_t pts;
  int64_t dts_offset;
} SpaMetaHeader;

typedef struct {
  const char *ptr_type;
  void       *ptr;
} SpaMetaPointer;

/**
 * SpaMetaVideoCrop:
 * @x:
 * @y:
 * @width:
 * @height
 */
typedef struct {
  int32_t   x, y;
  int32_t   width, height;
} SpaMetaVideoCrop;

/**
 * SpaMetaRingbuffer:
 * @ringbuffer:
 */
typedef struct {
  SpaRingbuffer ringbuffer;
} SpaMetaRingbuffer;

/**
 * SpaMetaShared:
 * @type:
 * @flags:
 * @fd:
 * @size:
 */
typedef struct {
  SpaDataType    type;
  int32_t        flags;
  int            fd;
  int32_t        offset;
  uint32_t       size;
} SpaMetaShared;

/**
 * SpaMeta:
 * @type: metadata type
 * @data: pointer to metadata
 * @size: size of metadata
 */
typedef struct {
  SpaMetaType  type;
  void        *data;
  uint32_t     size;
} SpaMeta;

/**
 * SpaChunk:
 * @offset: offset of valid data
 * @size: size of valid data
 * @stride: stride of data if applicable
 */
typedef struct {
  uint32_t       offset;
  uint32_t       size;
  int32_t        stride;
} SpaChunk;

/**
 * SpaData:
 * @type: memory type
 * @flags: memory flags
 * @fd: file descriptor
 * @offset: start offset when mapping @fd
 * @maxsize: maximum size of the memory
 * @data: pointer to memory
 * @chunk: pointer to chunk with valid offset
 */
typedef struct {
  SpaDataType    type;
  uint32_t       flags;
  int            fd;
  uint32_t       mapoffset;
  uint32_t       maxsize;
  void          *data;
  SpaChunk      *chunk;
} SpaData;

/**
 * SpaBuffer:
 * @id: buffer id
 * @n_metas: number of metadata
 * @metas: offset of array of @n_metas metadata
 * @n_datas: number of data pointers
 * @datas: offset of array of @n_datas data pointers
 */
struct _SpaBuffer {
  uint32_t       id;
  uint32_t       n_metas;
  SpaMeta       *metas;
  uint32_t       n_datas;
  SpaData       *datas;
};


static inline void *
spa_buffer_find_meta (SpaBuffer *b, SpaMetaType type)
{
  uint32_t i;

  for (i = 0; i < b->n_metas; i++)
    if (b->metas[i].type == type)
      return b->metas[i].data;
  return NULL;
}

static inline uint32_t
spa_meta_type_get_size (SpaMetaType  type)
{
  static const uint32_t header_sizes[] = {
    0,
    sizeof (SpaMetaHeader),
    sizeof (SpaMetaPointer),
    sizeof (SpaMetaVideoCrop),
    sizeof (SpaMetaRingbuffer),
    sizeof (SpaMetaShared),
  };
  if (type <= 0 || type >= SPA_N_ELEMENTS (header_sizes))
    return 0;
  return header_sizes[type];
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_BUFFER_H__ */
