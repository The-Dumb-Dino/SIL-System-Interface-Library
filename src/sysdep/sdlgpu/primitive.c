/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdlgpu/primitive.c: Primitive (vertex/index data) handling
 * for the SDL_GPU rendering backend.
 */

#define IN_SYSDEP

#include "src/base.h"
#include "src/graphics.h"
#include "src/memory.h"
#include "src/sysdep.h"
#include "src/sysdep/sdlgpu/internal.h"

/*************************************************************************/
/************************** Interface routines ***************************/
/*************************************************************************/

SysPrimitive *sys_graphics_create_primitive(
    enum GraphicsPrimitiveType type, const void *data, const uint32_t *format,
    int size, int count, const void *index_data, int index_size,
    int index_count, int immediate)
{
    void *local_vdata = NULL;   // Converted vertex data, if any.
    void *local_idata = NULL;   // Converted/generated index data, if any.

    SysPrimitive *primitive = mem_alloc(sizeof(*primitive), 0,
                                        MEM_ALLOC_CLEAR
                                        | (immediate ? MEM_ALLOC_TEMP : 0));
    if (!primitive) {
        DLOG("No memory for SysPrimitive");
        return NULL;
    }
    primitive->immediate = (immediate != 0);
    primitive->vertex_count = count;
    primitive->render_count = index_data ? index_count : count;

    int need_quad_indices = 0;
    switch (type) {
      case GRAPHICS_PRIMITIVE_POINTS:
        primitive->type = SDL_GPU_PRIMITIVETYPE_POINTLIST; break;
      case GRAPHICS_PRIMITIVE_LINES:
        primitive->type = SDL_GPU_PRIMITIVETYPE_LINELIST; break;
      case GRAPHICS_PRIMITIVE_LINE_STRIP:
        primitive->type = SDL_GPU_PRIMITIVETYPE_LINESTRIP; break;
      case GRAPHICS_PRIMITIVE_TRIANGLES:
        primitive->type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; break;
      case GRAPHICS_PRIMITIVE_TRIANGLE_STRIP:
        primitive->type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP; break;
      case GRAPHICS_PRIMITIVE_QUADS:
        primitive->type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        primitive->converted_quads = 1;
        if (primitive->render_count >= 4) {
            need_quad_indices = 1;
        } else {
            primitive->render_count = 0;
        }
        break;
      case GRAPHICS_PRIMITIVE_QUAD_STRIP:
        primitive->type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
        primitive->converted_quads = 2;
        if (primitive->render_count < 4) {
            primitive->render_count = 0;
        } else {
            primitive->render_count &= ~1;
        }
        break;
    }

    /* Parse the vertex format.  Only the standard attributes are
     * supported (no custom shader attributes). */
    int pos_type = 0;
    for (int i = 0; format[i] != 0; i++) {
        const int vtype = GRAPHICS_VERTEX_FORMAT_TYPE(format[i]);
        const int offset = GRAPHICS_VERTEX_FORMAT_OFFSET(format[i]);
        switch (vtype) {
          case GRAPHICS_VERTEX_POSITION_2S:
          case GRAPHICS_VERTEX_POSITION_2F:
          case GRAPHICS_VERTEX_POSITION_3F:
          case GRAPHICS_VERTEX_POSITION_4F:
            pos_type = vtype;
            primitive->pos_offset = (uint16_t)offset;
            break;
          case GRAPHICS_VERTEX_TEXCOORD_2F:
            primitive->has_texcoord = 1;
            primitive->tex_offset = (uint16_t)offset;
            break;
          case GRAPHICS_VERTEX_COLOR_4NUB:
            primitive->color_format = VFMT_UBYTE4_NORM;
            primitive->color_offset = (uint16_t)offset;
            break;
          case GRAPHICS_VERTEX_COLOR_4F:
            primitive->color_format = VFMT_FLOAT4;
            primitive->color_offset = (uint16_t)offset;
            break;
          default:
            DLOG("Unsupported vertex attribute 0x%04X (custom shader"
                 " attributes are not supported by SDL_GPU backend)", vtype);
            /* Ignore it; the default shaders don't use it anyway. */
            break;
        }
    }
    switch (pos_type) {
      case GRAPHICS_VERTEX_POSITION_2S:
      case GRAPHICS_VERTEX_POSITION_2F:
        primitive->pos_format = VFMT_FLOAT2; break;
      case GRAPHICS_VERTEX_POSITION_3F:
        primitive->pos_format = VFMT_FLOAT3; break;
      case GRAPHICS_VERTEX_POSITION_4F:
        primitive->pos_format = VFMT_FLOAT4; break;
      default:
        DLOG("No position attribute in vertex format");
        goto error;
    }
    primitive->stride = (uint16_t)size;

    /* GPUs have no 16-bit integer position format that the shaders can
     * read as float, so convert POSITION_2S to floats.  The converted
     * data appends the float position after the original vertex data. */
    uint32_t vdata_size = (uint32_t)size * count;
    if (pos_type == GRAPHICS_VERTEX_POSITION_2S) {
        const int new_size = align_up(size, 4) + 8;
        local_vdata = mem_alloc(new_size * count, 4, MEM_ALLOC_TEMP);
        if (!local_vdata) {
            DLOG("No memory for vertex conversion");
            goto error;
        }
        const uint8_t *src = data;
        uint8_t *dest = local_vdata;
        for (int i = 0; i < count; i++, src += size, dest += new_size) {
            memcpy(dest, src, size);
            int16_t pos[2];
            memcpy(pos, src + primitive->pos_offset, sizeof(pos));
            const float fpos[2] = {pos[0], pos[1]};
            memcpy(dest + align_up(size, 4), fpos, sizeof(fpos));
        }
        primitive->pos_offset = (uint16_t)align_up(size, 4);
        primitive->stride = (uint16_t)new_size;
        data = local_vdata;
        vdata_size = (uint32_t)new_size * count;
    }

    /* Convert 8-bit indices to 16-bit, and 16-bit indices containing
     * 65535 (which could be interpreted as a primitive restart marker)
     * to 32-bit. */
    if (index_data && index_size == 1) {
        uint16_t *idx16 = mem_alloc(2 * index_count, 0, MEM_ALLOC_TEMP);
        if (!idx16) {
            goto error;
        }
        for (int i = 0; i < index_count; i++) {
            idx16[i] = ((const uint8_t *)index_data)[i];
        }
        local_idata = idx16;
        index_data = idx16;
        index_size = 2;
    } else if (index_data && index_size == 2) {
        int has_ffff = 0;
        for (int i = 0; i < index_count; i++) {
            if (((const uint16_t *)index_data)[i] == 0xFFFF) {
                has_ffff = 1;
                break;
            }
        }
        if (has_ffff) {
            uint32_t *idx32 = mem_alloc(4 * index_count, 0, MEM_ALLOC_TEMP);
            if (!idx32) {
                goto error;
            }
            for (int i = 0; i < index_count; i++) {
                idx32[i] = ((const uint16_t *)index_data)[i];
            }
            local_idata = idx32;
            index_data = idx32;
            index_size = 4;
        }
    }

    /* Convert QUADS to TRIANGLES by generating an index list. */
    if (need_quad_indices) {
        const int num_quads = primitive->render_count / 4;
        const int out_size = (index_data && index_size == 4)
                             || (!index_data && count > 65535) ? 4 : 2;
        void *quad_idx = mem_alloc(out_size * 6 * num_quads, 0,
                                   MEM_ALLOC_TEMP);
        if (!quad_idx) {
            goto error;
        }
        for (int q = 0; q < num_quads; q++) {
            uint32_t v[4];
            for (int j = 0; j < 4; j++) {
                const int n = q*4 + j;
                if (!index_data) {
                    v[j] = n;
                } else if (index_size == 2) {
                    v[j] = ((const uint16_t *)index_data)[n];
                } else {
                    v[j] = ((const uint32_t *)index_data)[n];
                }
            }
            const uint32_t tri[6] = {v[0], v[1], v[3], v[3], v[1], v[2]};
            for (int j = 0; j < 6; j++) {
                if (out_size == 2) {
                    ((uint16_t *)quad_idx)[q*6+j] = (uint16_t)tri[j];
                } else {
                    ((uint32_t *)quad_idx)[q*6+j] = tri[j];
                }
            }
        }
        mem_free(local_idata);
        local_idata = quad_idx;
        index_data = quad_idx;
        index_size = out_size;
        index_count = 6 * num_quads;
        primitive->render_count = index_count;
    }

    primitive->index32 = (index_size == 4);
    const uint32_t idata_size = index_data ? (uint32_t)index_size * index_count
                                           : 0;

    if (immediate) {
        /* The caller guarantees that the data remains valid until the
         * primitive is drawn, so only keep our own converted copies. */
        primitive->vdata = data;
        primitive->vdata_size = vdata_size;
        primitive->vdata_owned = (local_vdata != NULL);
        primitive->idata = index_data;
        primitive->idata_size = idata_size;
        primitive->idata_owned = (local_idata != NULL);
        return primitive;
    }

    /* Persistent primitive: create GPU buffers. */
    SDL_GPUBufferCreateInfo info;
    mem_clear(&info, sizeof(info));
    info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    info.size = vdata_size;
    primitive->vbuf = SDL_CreateGPUBuffer(sdlgpu_device, &info);
    if (!primitive->vbuf
     || !sdlgpu_upload_buffer(primitive->vbuf, data, vdata_size)) {
        DLOG("Failed to create vertex buffer: %s", SDL_GetError());
        goto error;
    }
    if (index_data) {
        info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        info.size = idata_size;
        primitive->ibuf = SDL_CreateGPUBuffer(sdlgpu_device, &info);
        if (!primitive->ibuf
         || !sdlgpu_upload_buffer(primitive->ibuf, index_data, idata_size)) {
            DLOG("Failed to create index buffer: %s", SDL_GetError());
            goto error;
        }
    }
    mem_free(local_vdata);
    mem_free(local_idata);
    return primitive;

  error:
    if (primitive->vbuf) {
        SDL_ReleaseGPUBuffer(sdlgpu_device, primitive->vbuf);
    }
    if (primitive->ibuf) {
        SDL_ReleaseGPUBuffer(sdlgpu_device, primitive->ibuf);
    }
    mem_free(local_vdata);
    mem_free(local_idata);
    mem_free(primitive);
    return NULL;
}

/*-----------------------------------------------------------------------*/

void sys_graphics_draw_primitive(SysPrimitive *primitive, int start, int count)
{
    int unit = 1;
    if (primitive->converted_quads == 1) {
        /* Convert vertex counts to index counts (4 vertices = 6 indices). */
        if (start % 4 != 0) {
            DLOG("WARNING: unaligned partial draw of QUADS primitive"
                 " (start=%d count=%d)", start, count);
        }
        start = (start / 4) * 6;
        if (count > 0) {
            count = (count / 4) * 6;
        }
        unit = 6;
    } else if (primitive->converted_quads == 2) {
        unit = 2;
    }
    if (start >= primitive->render_count) {
        return;
    }
    if (count < 0 || count > primitive->render_count - start) {
        count = primitive->render_count - start;
    }
    count -= count % unit;
    if (count <= 0) {
        return;
    }
    sdlgpu_record_draw(primitive, start, count);
}

/*-----------------------------------------------------------------------*/

void sys_graphics_destroy_primitive(SysPrimitive *primitive)
{
    sdlgpu_defer_release_buffer(primitive->vbuf);
    sdlgpu_defer_release_buffer(primitive->ibuf);
    if (primitive->vdata_owned) {
        mem_free((void *)primitive->vdata);
    }
    if (primitive->idata_owned) {
        mem_free((void *)primitive->idata);
    }
    mem_free(primitive);
}

/*************************************************************************/
/********************** Internal interface routines **********************/
/*************************************************************************/

void sdlgpu_primitive_cleanup(void)
{
    /* Nothing to do (primitives are owned by the caller). */
}

/*************************************************************************/
/*************************************************************************/
