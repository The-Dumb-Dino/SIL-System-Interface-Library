/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdlgpu/texture.c: Texture and framebuffer handling for the
 * SDL_GPU rendering backend.
 *
 * All color formats are converted to RGBA8 on upload, except for alpha-
 * and luminance-only textures, which are stored as R8 and expanded by the
 * fragment shader.  Texture row 0 is stored at texture coordinate V=0, as
 * with OpenGL.
 */

#define IN_SYSDEP

#include "src/base.h"
#include "src/framebuffer.h"
#include "src/memory.h"
#include "src/sysdep.h"
#include "src/sysdep/sdlgpu/internal.h"
#include "src/texture.h"
#include "src/utility/pixformat.h"

/*************************************************************************/
/****************************** Local data *******************************/
/*************************************************************************/

/* Sampler cache, indexed by SAMPLER_* bits.  Index 16 is the sampler used
 * when no texture is applied. */
#define SAMPLER_LINEAR    (1 << 0)
#define SAMPLER_MIPMAPS   (1 << 1)
#define SAMPLER_REPEAT_U  (1 << 2)
#define SAMPLER_REPEAT_V  (1 << 3)
static SDL_GPUSampler *samplers[16];

/* 1x1 white texture. */
static SDL_GPUTexture *dummy_texture;

/*************************************************************************/
/************************** Local declarations ***************************/
/*************************************************************************/

static SDL_GPUTexture *create_gpu_texture(int width, int height,
                                          SDL_GPUTextureFormat format,
                                          int num_levels, int render_target);

/*************************************************************************/
/******************* Interface: texture creation/deletion ****************/
/*************************************************************************/

SysTexture *sys_texture_create(
    int width, int height, enum TextureFormat data_format, int num_levels,
    void *data, int stride, const int32_t *level_offsets,
    const int32_t *level_sizes, int mipmaps, int mem_flags, int reuse)
{
    (void)level_sizes;

    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    int color_type = TEXCOLOR_RGBA;
    int input_bpp = 32;
    int is_palette = 0;
    PixelConvertFunc *convert = NULL;

    switch (data_format) {
      case TEX_FORMAT_RGBA8888:
        break;
      case TEX_FORMAT_BGRA8888:
        convert = pixel_convert_bgra8888_rgba8888; break;
      case TEX_FORMAT_RGB565:
        convert = pixel_convert_rgb565_rgba8888; input_bpp = 16; break;
      case TEX_FORMAT_RGBA5551:
        convert = pixel_convert_rgba5551_rgba8888; input_bpp = 16; break;
      case TEX_FORMAT_RGBA4444:
        convert = pixel_convert_rgba4444_rgba8888; input_bpp = 16; break;
      case TEX_FORMAT_BGR565:
        convert = pixel_convert_bgr565_rgba8888; input_bpp = 16; break;
      case TEX_FORMAT_BGRA5551:
        convert = pixel_convert_bgra5551_rgba8888; input_bpp = 16; break;
      case TEX_FORMAT_BGRA4444:
        convert = pixel_convert_bgra4444_rgba8888; input_bpp = 16; break;
      case TEX_FORMAT_A8:
        format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        color_type = TEXCOLOR_A;
        input_bpp = 8;
        break;
      case TEX_FORMAT_L8:
        format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        color_type = TEXCOLOR_L;
        input_bpp = 8;
        break;
      case TEX_FORMAT_PALETTE8_RGBA8888:
        is_palette = 1;
        input_bpp = 8;
        break;
      case TEX_FORMAT_PSP_RGBA8888:
      case TEX_FORMAT_PSP_RGB565:
      case TEX_FORMAT_PSP_RGBA5551:
      case TEX_FORMAT_PSP_RGBA4444:
      case TEX_FORMAT_PSP_A8:
      case TEX_FORMAT_PSP_PALETTE8_RGBA8888:
      case TEX_FORMAT_PSP_L8:
      case TEX_FORMAT_PSP_RGBA8888_SWIZZLED:
      case TEX_FORMAT_PSP_RGB565_SWIZZLED:
      case TEX_FORMAT_PSP_RGBA5551_SWIZZLED:
      case TEX_FORMAT_PSP_RGBA4444_SWIZZLED:
      case TEX_FORMAT_PSP_A8_SWIZZLED:
      case TEX_FORMAT_PSP_L8_SWIZZLED:
      case TEX_FORMAT_PSP_PALETTE8_RGBA8888_SWIZZLED:
      case TEX_FORMAT_S3TC_DXT1:
      case TEX_FORMAT_S3TC_DXT3:
      case TEX_FORMAT_S3TC_DXT5:
      case TEX_FORMAT_PVRTC2_RGBA:
      case TEX_FORMAT_PVRTC4_RGBA:
      case TEX_FORMAT_PVRTC2_RGB:
      case TEX_FORMAT_PVRTC4_RGB:
        DLOG("Texture format 0x%02X not supported by SDL_GPU backend",
             data_format);
        if (reuse) {
            mem_free(data);
        }
        return NULL;
    }

    SysTexture *texture = mem_alloc(sizeof(*texture), 0,
                                    mem_flags | MEM_ALLOC_CLEAR);
    if (!texture) {
        if (reuse) {
            mem_free(data);
        }
        return NULL;
    }
    texture->width = width;
    texture->height = height;
    texture->format = format;
    texture->color_type = (uint8_t)color_type;
    texture->repeat_u = texture->repeat_v = 1;
    texture->antialias = 1;
    texture->empty = 1;

    /* Mipmap setup: either auto-generated or supplied by the caller. */
    int total_levels = 1;
    if (mipmaps || num_levels > 1) {
        int w = width, h = height;
        while (w > 1 || h > 1) {
            w = lbound(w/2, 1);
            h = lbound(h/2, 1);
            total_levels++;
        }
        if (mipmaps) {
            texture->auto_mipmaps = 1;
            num_levels = ubound(num_levels, 1);
        } else {
            total_levels = ubound(total_levels, num_levels);
        }
    }
    texture->num_levels = (uint8_t)total_levels;

    texture->tex = create_gpu_texture(width, height, format, total_levels,
                                      texture->auto_mipmaps);
    if (!texture->tex) {
        mem_free(texture);
        if (reuse) {
            mem_free(data);
        }
        return NULL;
    }

    /* Upload each supplied level, converting to the GPU format. */
    const uint8_t *palette = is_palette ? (const uint8_t *)data
                                          + level_offsets[0] : NULL;
    for (int level = 0; level < num_levels && level < total_levels && data;
         level++) {
        const int lw = lbound(width >> level, 1);
        const int lh = lbound(height >> level, 1);
        const int lstride = (level == 0) ? stride : lw;
        const uint8_t *src = (const uint8_t *)data + level_offsets[level];
        if (is_palette && level == 0) {
            src += 256*4;
        }
        const int out_bpp = (format == SDL_GPU_TEXTUREFORMAT_R8_UNORM) ? 1 : 4;
        uint8_t *buf = mem_alloc(lw * lh * out_bpp, 0, MEM_ALLOC_TEMP);
        if (!buf) {
            DLOG("No memory for texture conversion");
            break;
        }
        for (int y = 0; y < lh; y++) {
            const uint8_t *row = src + (size_t)y * lstride * (input_bpp / 8);
            uint8_t *out = buf + (size_t)y * lw * out_bpp;
            if (is_palette) {
                for (int x = 0; x < lw; x++) {
                    memcpy(out + x*4, palette + row[x]*4, 4);
                }
            } else if (convert) {
                (*convert)(out, row, lw);
            } else {
                memcpy(out, row, lw * out_bpp);
            }
        }
        sdlgpu_upload_texture(texture, level, 0, 0, lw, lh, buf, lw);
        mem_free(buf);
    }
    if (num_levels == 0) {
        /* Initialize empty textures to transparent black, as reading
         * uninitialized GPU memory gives random results. */
        const int out_bpp = (format == SDL_GPU_TEXTUREFORMAT_R8_UNORM) ? 1 : 4;
        uint8_t *zero = mem_alloc(width * height * out_bpp, 0,
                                  MEM_ALLOC_TEMP | MEM_ALLOC_CLEAR);
        if (zero) {
            sdlgpu_upload_texture(texture, 0, 0, 0, width, height, zero,
                                  width);
            mem_free(zero);
        }
        texture->empty = 1;
    }

    if (reuse) {
        mem_free(data);
    }
    return texture;
}

/*-----------------------------------------------------------------------*/

void sys_texture_destroy(SysTexture *texture)
{
    if (sdlgpu_current_texture == texture) {
        sdlgpu_current_texture = NULL;
    }
    sdlgpu_defer_release_texture(texture->tex);
    mem_free(texture->lock_buf);
    mem_free(texture);
}

/*************************************************************************/
/******************** Interface: texture information *********************/
/*************************************************************************/

int sys_texture_width(SysTexture *texture)
{
    return texture->width;
}

int sys_texture_height(SysTexture *texture)
{
    return texture->height;
}

int sys_texture_has_mipmaps(SysTexture *texture)
{
    return texture->num_levels > 1;
}

/*************************************************************************/
/******************** Interface: pixel data access ***********************/
/*************************************************************************/

SysTexture *sys_texture_grab(int x, int y, int w, int h,
                             UNUSED int readable, int mipmaps, int mem_flags)
{
    SysTexture *texture = mem_alloc(sizeof(*texture), 0,
                                    mem_flags | MEM_ALLOC_CLEAR);
    if (!texture) {
        return NULL;
    }
    texture->width = w;
    texture->height = h;
    texture->format = sdlgpu_color_format();
    texture->color_type = TEXCOLOR_RGB;  // As OpenGL: grabbed alpha is 1.
    texture->repeat_u = texture->repeat_v = 1;
    texture->antialias = 1;
    int levels = 1;
    if (mipmaps) {
        for (int lw = w, lh = h; lw > 1 || lh > 1;
             lw = lbound(lw/2, 1), lh = lbound(lh/2, 1)) {
            levels++;
        }
        texture->auto_mipmaps = 1;
    }
    texture->num_levels = (uint8_t)levels;
    texture->tex = create_gpu_texture(w, h, texture->format, levels, mipmaps);
    if (!texture->tex) {
        mem_free(texture);
        return NULL;
    }
    if (x < 0 || y < 0) {
        /* Parts outside the display: start from transparent black. */
        uint8_t *zero = mem_alloc(w * h * 4, 0, MEM_ALLOC_TEMP | MEM_ALLOC_CLEAR);
        if (zero) {
            sdlgpu_upload_texture(texture, 0, 0, 0, w, h, zero, w);
            mem_free(zero);
        }
    }
    if (!sdlgpu_copy_from_target(texture, x, y, 0, 0, w, h)) {
        sdlgpu_defer_release_texture(texture->tex);
        mem_free(texture);
        return NULL;
    }
    return texture;
}

/*-----------------------------------------------------------------------*/

void *sys_texture_lock(SysTexture *texture, SysTextureLockMode lock_mode,
                       int x, int y, int w, int h)
{
    const uint32_t size = (uint32_t)(w * h * 4);
    texture->lock_buf = mem_alloc(size, 0, MEM_ALLOC_TEMP);
    if (!texture->lock_buf) {
        DLOG("No memory for lock buffer (%u bytes)", size);
        return NULL;
    }
    texture->lock_x = x;
    texture->lock_y = y;
    texture->lock_w = w;
    texture->lock_h = h;

    if (lock_mode == SYS_TEXTURE_LOCK_DISCARD) {
        return texture->lock_buf;
    }
    if (texture->empty) {
        mem_clear(texture->lock_buf, size);
        return texture->lock_buf;
    }

    /* Make sure pending rendering (e.g. into a framebuffer texture) is
     * complete before reading. */
    sdlgpu_flush();
    uint8_t *pixels = texture->lock_buf;
    if (texture->format == SDL_GPU_TEXTUREFORMAT_R8_UNORM) {
        /* Read 1 byte per pixel into the end of the buffer, then expand. */
        uint8_t *src = pixels + size - w*h;
        if (!sdlgpu_read_texture(texture->tex, texture->format, x, y, w, h,
                                 src)) {
            goto fail;
        }
        for (int i = 0; i < w*h; i++) {
            const uint8_t v = src[i];
            if (texture->color_type == TEXCOLOR_A) {
                pixels[i*4+0] = pixels[i*4+1] = pixels[i*4+2] = 255;
                pixels[i*4+3] = v;
            } else {
                pixels[i*4+0] = pixels[i*4+1] = pixels[i*4+2] = v;
                pixels[i*4+3] = 255;
            }
        }
    } else {
        if (!sdlgpu_read_texture(texture->tex, texture->format, x, y, w, h,
                                 pixels)) {
            goto fail;
        }
        if (texture->color_type == TEXCOLOR_RGB) {
            for (int i = 0; i < w*h; i++) {
                pixels[i*4+3] = 255;
            }
        }
    }
    return texture->lock_buf;

  fail:
    DLOG("Failed to read texture data");
    mem_free(texture->lock_buf);
    texture->lock_buf = NULL;
    return NULL;
}

/*-----------------------------------------------------------------------*/

void sys_texture_unlock(SysTexture *texture, int update)
{
    if (update && texture->lock_buf) {
        if (texture->format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) {
            /* Alpha/luminance textures become RGBA textures. */
            SDL_GPUTexture *new_tex = create_gpu_texture(
                texture->width, texture->height,
                SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, texture->num_levels,
                texture->auto_mipmaps);
            if (new_tex) {
                if (texture->batch_serial == sdlgpu_batch_serial) {
                    sdlgpu_flush();
                }
                sdlgpu_defer_release_texture(texture->tex);
                texture->tex = new_tex;
                texture->format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            }
        }
        texture->color_type = TEXCOLOR_RGBA;
        sdlgpu_upload_texture(texture, 0, texture->lock_x, texture->lock_y,
                              texture->lock_w, texture->lock_h,
                              texture->lock_buf, texture->lock_w);
    }
    mem_free(texture->lock_buf);
    texture->lock_buf = NULL;
}

/*-----------------------------------------------------------------------*/

void sys_texture_flush(UNUSED SysTexture *texture)
{
    /* Nothing to do. */
}

/*************************************************************************/
/********************* Interface: rendering control **********************/
/*************************************************************************/

void sys_texture_set_repeat(SysTexture *texture, int repeat_u, int repeat_v)
{
    texture->repeat_u = (repeat_u != 0);
    texture->repeat_v = (repeat_v != 0);
}

void sys_texture_set_antialias(SysTexture *texture, int on)
{
    texture->antialias = (on != 0);
}

void sys_texture_apply(int unit, SysTexture *texture)
{
    if (unit == 0) {
        sdlgpu_current_texture = texture;
    }
}

int sys_texture_num_units(void)
{
    return 1;
}

/*************************************************************************/
/************************ Interface: framebuffers ************************/
/*************************************************************************/

int sys_framebuffer_supported(void)
{
    return 1;
}

/*-----------------------------------------------------------------------*/

SysFramebuffer *sys_framebuffer_create(
    int width, int height, enum FramebufferColorType color_type,
    int depth_bits, int stencil_bits)
{
    SysFramebuffer *framebuffer = mem_alloc(sizeof(*framebuffer), 0,
                                            MEM_ALLOC_CLEAR);
    if (!framebuffer) {
        return NULL;
    }
    framebuffer->width = width;
    framebuffer->height = height;
    SysTexture *texture = &framebuffer->texture;
    texture->width = width;
    texture->height = height;
    texture->format = sdlgpu_color_format();
    texture->color_type =
        (color_type == FBCOLOR_RGB8) ? TEXCOLOR_RGB : TEXCOLOR_RGBA;
    texture->is_framebuffer = 1;
    texture->num_levels = 1;
    texture->repeat_u = texture->repeat_v = 0;
    texture->antialias = 1;
    texture->tex = create_gpu_texture(width, height, texture->format, 1, 1);
    if (!texture->tex) {
        mem_free(framebuffer);
        return NULL;
    }
    framebuffer->depth_format = sdlgpu_depth_format(depth_bits, stencil_bits);
    if (framebuffer->depth_format != SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_GPUTextureCreateInfo info;
        mem_clear(&info, sizeof(info));
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = framebuffer->depth_format;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        framebuffer->depth = SDL_CreateGPUTexture(sdlgpu_device, &info);
        if (!framebuffer->depth) {
            DLOG("Failed to create framebuffer depth buffer: %s",
                 SDL_GetError());
            SDL_ReleaseGPUTexture(sdlgpu_device, texture->tex);
            mem_free(framebuffer);
            return NULL;
        }
    } else if (depth_bits > 0 || stencil_bits > 0) {
        DLOG("No supported depth format for %d/%d bits",
             depth_bits, stencil_bits);
        SDL_ReleaseGPUTexture(sdlgpu_device, texture->tex);
        mem_free(framebuffer);
        return NULL;
    }
    return framebuffer;
}

/*-----------------------------------------------------------------------*/

void sys_framebuffer_destroy(SysFramebuffer *framebuffer)
{
    if (sdlgpu_current_framebuffer == framebuffer) {
        sdlgpu_set_render_target(NULL);
    }
    if (sdlgpu_current_texture == &framebuffer->texture) {
        sdlgpu_current_texture = NULL;
    }
    sdlgpu_defer_release_texture(framebuffer->texture.tex);
    sdlgpu_defer_release_texture(framebuffer->depth);
    mem_free(framebuffer->texture.lock_buf);
    mem_free(framebuffer);
}

/*-----------------------------------------------------------------------*/

void sys_framebuffer_bind(SysFramebuffer *framebuffer)
{
    sdlgpu_set_render_target(framebuffer);
}

/*-----------------------------------------------------------------------*/

SysTexture *sys_framebuffer_get_texture(SysFramebuffer *framebuffer)
{
    return &framebuffer->texture;
}

/*-----------------------------------------------------------------------*/

void sys_framebuffer_set_antialias(SysFramebuffer *framebuffer, int on)
{
    framebuffer->texture.antialias = (on != 0);
}

/*-----------------------------------------------------------------------*/

void sys_framebuffer_discard_data(UNUSED SysFramebuffer *framebuffer)
{
    /* Nothing to do; the next render pass simply overwrites the data. */
}

/*************************************************************************/
/********************** Internal interface routines **********************/
/*************************************************************************/

void sdlgpu_texture_init(void)
{
    mem_clear(samplers, sizeof(samplers));
    dummy_texture = create_gpu_texture(1, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                       1, 0);
    if (dummy_texture) {
        SysTexture temp;
        mem_clear(&temp, sizeof(temp));
        temp.tex = dummy_texture;
        temp.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        static const uint8_t white[4] = {255, 255, 255, 255};
        sdlgpu_upload_texture(&temp, 0, 0, 0, 1, 1, white, 1);
    }
}

/*-----------------------------------------------------------------------*/

void sdlgpu_texture_cleanup(void)
{
    for (int i = 0; i < lenof(samplers); i++) {
        if (samplers[i]) {
            SDL_ReleaseGPUSampler(sdlgpu_device, samplers[i]);
            samplers[i] = NULL;
        }
    }
    if (dummy_texture) {
        SDL_ReleaseGPUTexture(sdlgpu_device, dummy_texture);
        dummy_texture = NULL;
    }
}

/*-----------------------------------------------------------------------*/

SDL_GPUSampler *sdlgpu_texture_sampler(const SysTexture *texture)
{
    int index = 0;
    if (texture) {
        index = (texture->antialias ? SAMPLER_LINEAR : 0)
              | (texture->num_levels > 1 ? SAMPLER_MIPMAPS : 0)
              | (texture->repeat_u ? SAMPLER_REPEAT_U : 0)
              | (texture->repeat_v ? SAMPLER_REPEAT_V : 0);
    }
    if (!samplers[index]) {
        SDL_GPUSamplerCreateInfo info;
        mem_clear(&info, sizeof(info));
        const SDL_GPUFilter filter = (index & SAMPLER_LINEAR)
            ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        info.min_filter = filter;
        info.mag_filter = filter;
        info.mipmap_mode = (index & SAMPLER_LINEAR)
            ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR
            : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        info.address_mode_u = (index & SAMPLER_REPEAT_U)
            ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT
            : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_v = (index & SAMPLER_REPEAT_V)
            ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT
            : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.min_lod = 0;
        info.max_lod = (index & SAMPLER_MIPMAPS) ? 1000.0f : 0.0f;
        samplers[index] = SDL_CreateGPUSampler(sdlgpu_device, &info);
        if (!samplers[index]) {
            DLOG("SDL_CreateGPUSampler() failed: %s", SDL_GetError());
        }
    }
    return samplers[index];
}

/*-----------------------------------------------------------------------*/

SDL_GPUTexture *sdlgpu_dummy_texture(void)
{
    return dummy_texture;
}

/*************************************************************************/
/**************************** Local routines *****************************/
/*************************************************************************/

static SDL_GPUTexture *create_gpu_texture(int width, int height,
                                          SDL_GPUTextureFormat format,
                                          int num_levels, int render_target)
{
    SDL_GPUTextureCreateInfo info;
    mem_clear(&info, sizeof(info));
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER
               | (render_target ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = num_levels;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(sdlgpu_device, &info);
    if (!texture) {
        DLOG("SDL_CreateGPUTexture(%dx%d, fmt %d, %d levels) failed: %s",
             width, height, format, num_levels, SDL_GetError());
    }
    return texture;
}

/*************************************************************************/
/*************************************************************************/
