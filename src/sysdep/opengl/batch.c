/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/opengl/batch.c: Batching of immediate-mode primitives.
 *
 * Games built on SIL typically draw sprites one at a time with
 * graphics_draw_vertices(), which creates, draws and destroys an
 * immediate-mode primitive for each sprite.  Each of these costs a vertex
 * buffer upload and a draw call, and the per-draw-call overhead of OpenGL
 * drivers (in particular the OpenGL-on-Metal layer of current macOS) is
 * high enough to dominate frame time for sprite-heavy scenes.
 *
 * This module merges consecutive immediate-mode primitives into a single
 * primitive when doing so cannot change the rendered result:
 *    - the primitive is a list type (points, lines, triangles, quads);
 *    - it has the same vertex format and the same applied texture as the
 *      primitives already in the batch;
 *    - no render state has changed since the batch was started.
 *
 * The last condition is enforced by OPENGL_BATCH_FLUSH(), which every
 * state-changing sysdep entry point calls before modifying any state.
 * texture_apply() is special-cased: while a batch is pending, binding a
 * texture is deferred, so that the common pattern "apply texture, draw,
 * apply no texture" does not break batches when consecutive sprites use
 * the same texture.
 *
 * Batching can be disabled at runtime by setting the environment variable
 * SIL_OPENGL_BATCHING=0 (for comparison and debugging).
 */

#define IN_SYSDEP

#include "src/base.h"
#include "src/graphics.h"
#include "src/memory.h"
#include "src/sysdep.h"
#include "src/sysdep/opengl/opengl.h"
#include "src/sysdep/opengl/internal.h"

#include <stdlib.h>

/*************************************************************************/
/****************************** Local data *******************************/
/*************************************************************************/

/* Maximum number of vertices in a batch (16-bit indices are used). */
#define MAX_BATCH_VERTICES  65535

/* Maximum number of vertex format entries supported for batching. */
#define MAX_FORMAT_ENTRIES  16

uint8_t opengl_batch_pending;

/* Is batching enabled?  (-1 = not yet determined.) */
static int batching_enabled = -1;

/* Set while flushing, so that the primitive created for the flush is not
 * itself batched. */
static uint8_t in_flush;

/* Properties of the current batch. */
static int batch_type;          // GRAPHICS_PRIMITIVE_{POINTS,LINES,TRIANGLES}
static int batch_vertex_size;
static uint32_t batch_format[MAX_FORMAT_ENTRIES + 1];
static SysTexture *batch_texture;

/* Batch data. */
static uint8_t *batch_vertices;
static int batch_vertex_count, batch_vertex_capacity;
static uint16_t *batch_indices;
static int batch_index_count, batch_index_capacity;

/* Deferred texture binding (see above). */
static uint8_t texture_deferred;
static SysTexture *deferred_texture;

/*************************************************************************/
/************************** Local declarations ***************************/
/*************************************************************************/

static int batch_type_for(int type);
static int ensure_capacity(int vertex_size, int vertex_count,
                           int index_count);

/*************************************************************************/
/********************** Internal interface routines **********************/
/*************************************************************************/

SysPrimitive *opengl_batch_create(
    int type, const void *data, const uint32_t *format, int size, int count,
    const void *index_data, int index_size, int index_count)
{
    if (UNLIKELY(batching_enabled < 0)) {
        const char *env = getenv("SIL_OPENGL_BATCHING");
        batching_enabled = !(env && *env == '0');
    }
    if (!batching_enabled || in_flush || opengl_shader_objects_enabled) {
        return NULL;
    }
    if (!batch_type_for(type) || count <= 0 || count > MAX_BATCH_VERTICES) {
        return NULL;
    }
    int num_entries = 0;
    while (format[num_entries] != 0) {
        if (++num_entries > MAX_FORMAT_ENTRIES) {
            return NULL;
        }
    }

    SysPrimitive *primitive = mem_alloc(sizeof(*primitive), 0,
                                        MEM_ALLOC_TEMP | MEM_ALLOC_CLEAR);
    if (UNLIKELY(!primitive)) {
        return NULL;
    }
    primitive->generation = opengl_device_generation;
    primitive->is_batched = 1;
    primitive->batch_sil_type = type;
    primitive->batch_vdata = data;
    primitive->batch_format = format;
    primitive->batch_vsize = size;
    primitive->batch_vcount = count;
    primitive->batch_idata = index_data;
    primitive->batch_isize = (uint8_t)index_size;
    primitive->batch_icount = index_data ? index_count : 0;
    return primitive;
}

/*-----------------------------------------------------------------------*/

int opengl_batch_draw(SysPrimitive *primitive, int start, int count)
{
    PRECOND(primitive->is_batched, return 0);

    /* Partial draws are rare; let the caller draw them normally. */
    if (start != 0 || count >= 0) {
        const int total = primitive->batch_icount ? primitive->batch_icount
                                                  : primitive->batch_vcount;
        if (start != 0 || count < total) {
            return 0;
        }
    }

    const int type = batch_type_for(primitive->batch_sil_type);
    SysTexture *texture =
        texture_deferred ? deferred_texture : opengl_current_texture;

    /* Number of elements contributed by this primitive. */
    const int elements = primitive->batch_icount ? primitive->batch_icount
                                                 : primitive->batch_vcount;
    int new_indices;
    if (primitive->batch_sil_type == GRAPHICS_PRIMITIVE_QUADS) {
        new_indices = (elements / 4) * 6;
    } else {
        new_indices = elements;
    }
    if (new_indices <= 0) {
        return 1;  // Nothing to draw (e.g. a 3-vertex QUADS primitive).
    }

    /* Check whether the primitive can join the current batch. */
    if (opengl_batch_pending) {
        int compatible = (type == batch_type
                          && primitive->batch_vsize == batch_vertex_size
                          && texture == batch_texture
                          && batch_vertex_count + primitive->batch_vcount
                                 <= MAX_BATCH_VERTICES);
        for (int i = 0; compatible; i++) {
            if (primitive->batch_format[i] != batch_format[i]) {
                compatible = 0;
            } else if (batch_format[i] == 0) {
                break;
            }
        }
        if (!compatible) {
            opengl_batch_flush();
            texture = opengl_current_texture;
        }
    }

    if (!ensure_capacity(primitive->batch_vsize,
                         batch_vertex_count + primitive->batch_vcount,
                         batch_index_count + new_indices)) {
        if (opengl_batch_pending) {
            opengl_batch_flush();
        }
        return 0;
    }

    if (!opengl_batch_pending) {
        batch_type = type;
        batch_vertex_size = primitive->batch_vsize;
        int i = 0;
        for (; primitive->batch_format[i] != 0; i++) {
            batch_format[i] = primitive->batch_format[i];
        }
        batch_format[i] = 0;
        batch_texture = texture;
        batch_vertex_count = 0;
        batch_index_count = 0;
        opengl_batch_pending = 1;
    }

    /* Append vertex data. */
    const int base = batch_vertex_count;
    memcpy(batch_vertices + (size_t)base * batch_vertex_size,
           primitive->batch_vdata,
           (size_t)primitive->batch_vcount * batch_vertex_size);
    batch_vertex_count += primitive->batch_vcount;

    /* Append indices, rebased to the batch's vertex array. */
    uint16_t *out = batch_indices + batch_index_count;
    const void *idata = primitive->batch_idata;
    const int isize = primitive->batch_isize;
    #define INDEX(n)  (idata == NULL ? (uint32_t)(n) \
                       : isize == 1 ? ((const uint8_t *)idata)[n] \
                       : isize == 2 ? ((const uint16_t *)idata)[n] \
                       : ((const uint32_t *)idata)[n])
    if (primitive->batch_sil_type == GRAPHICS_PRIMITIVE_QUADS) {
        for (int q = 0; q < elements / 4; q++) {
            const uint32_t a = INDEX(q*4+0), b = INDEX(q*4+1);
            const uint32_t c = INDEX(q*4+2), d = INDEX(q*4+3);
            *out++ = (uint16_t)(base + a);
            *out++ = (uint16_t)(base + b);
            *out++ = (uint16_t)(base + d);
            *out++ = (uint16_t)(base + d);
            *out++ = (uint16_t)(base + b);
            *out++ = (uint16_t)(base + c);
        }
    } else {
        for (int n = 0; n < elements; n++) {
            *out++ = (uint16_t)(base + INDEX(n));
        }
    }
    #undef INDEX
    batch_index_count += new_indices;
    return 1;
}

/*-----------------------------------------------------------------------*/

void opengl_batch_flush(void)
{
    if (!opengl_batch_pending) {
        return;
    }
    opengl_batch_pending = 0;

    in_flush = 1;
    SysPrimitive *primitive = sys_(graphics_create_primitive)(
        batch_type, batch_vertices, batch_format, batch_vertex_size,
        batch_vertex_count, batch_indices, 2, batch_index_count, 1);
    if (primitive) {
        sys_(graphics_draw_primitive)(primitive, 0, -1);
        sys_(graphics_destroy_primitive)(primitive);
    } else {
        DLOG("Failed to create batch primitive (%d vertices, %d indices)",
             batch_vertex_count, batch_index_count);
    }
    in_flush = 0;
    batch_vertex_count = 0;
    batch_index_count = 0;

    if (texture_deferred) {
        texture_deferred = 0;
        sys_(texture_apply)(0, deferred_texture);
    }
}

/*-----------------------------------------------------------------------*/

int opengl_batch_texture_apply(SysTexture *texture)
{
    if (!opengl_batch_pending) {
        return 0;
    }
    if (texture == batch_texture) {
        texture_deferred = 0;
    } else {
        texture_deferred = 1;
        deferred_texture = texture;
    }
    return 1;
}

/*-----------------------------------------------------------------------*/

void opengl_batch_texture_destroyed(SysTexture *texture)
{
    /* Called (after flushing) when a texture is destroyed. */
    if (deferred_texture == texture) {
        deferred_texture = NULL;
    }
    if (batch_texture == texture) {
        batch_texture = NULL;
    }
}

/*-----------------------------------------------------------------------*/

void opengl_batch_cleanup(void)
{
    opengl_batch_pending = 0;
    texture_deferred = 0;
    deferred_texture = NULL;
    batch_texture = NULL;
    mem_free(batch_vertices);
    batch_vertices = NULL;
    batch_vertex_capacity = batch_vertex_count = 0;
    mem_free(batch_indices);
    batch_indices = NULL;
    batch_index_capacity = batch_index_count = 0;
}

/*************************************************************************/
/**************************** Local routines *****************************/
/*************************************************************************/

static int batch_type_for(int type)
{
    switch (type) {
      case GRAPHICS_PRIMITIVE_POINTS:    return GRAPHICS_PRIMITIVE_POINTS;
      case GRAPHICS_PRIMITIVE_LINES:     return GRAPHICS_PRIMITIVE_LINES;
      case GRAPHICS_PRIMITIVE_TRIANGLES: return GRAPHICS_PRIMITIVE_TRIANGLES;
      case GRAPHICS_PRIMITIVE_QUADS:     return GRAPHICS_PRIMITIVE_TRIANGLES;
      default:                           return 0;
    }
}

/*-----------------------------------------------------------------------*/

static int ensure_capacity(int vertex_size, int vertex_count,
                           int index_count)
{
    /* The vertex size may differ between batches, so capacity is
     * tracked in bytes. */
    const int vertex_bytes = vertex_count * vertex_size;
    if (vertex_bytes > batch_vertex_capacity) {
        int new_capacity = lbound(batch_vertex_capacity * 2, 65536);
        while (new_capacity < vertex_bytes) {
            new_capacity *= 2;
        }
        uint8_t *new_buf = mem_realloc(batch_vertices, new_capacity, 0);
        if (!new_buf) {
            return 0;
        }
        batch_vertices = new_buf;
        batch_vertex_capacity = new_capacity;
    }
    if (index_count > batch_index_capacity) {
        int new_capacity = lbound(batch_index_capacity * 2, 16384);
        while (new_capacity < index_count) {
            new_capacity *= 2;
        }
        uint16_t *new_buf = mem_realloc(batch_indices,
                                        new_capacity * sizeof(*new_buf), 0);
        if (!new_buf) {
            return 0;
        }
        batch_indices = new_buf;
        batch_index_capacity = new_capacity;
    }
    return 1;
}

/*************************************************************************/
/*************************************************************************/
