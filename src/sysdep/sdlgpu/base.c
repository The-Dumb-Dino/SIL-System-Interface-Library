/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdlgpu/base.c: Core of the SDL_GPU rendering backend: device
 * and frame management, render targets, render state, draw recording and
 * batch submission, pipeline cache, clearing and pixel readback.
 *
 * See internal.h for an overview of the design.
 */

#define IN_SYSDEP

#include "src/base.h"
#include "src/graphics.h"
#include "src/math.h"
#include "src/memory.h"
#include "src/sysdep.h"
#include "src/sysdep/sdlgpu/internal.h"
#include "src/sysdep/sdlgpu/sdlgpu.h"

/*************************************************************************/
/****************************** Local data *******************************/
/*************************************************************************/

SDL_GPUDevice *sdlgpu_device;
uint32_t sdlgpu_batch_serial = 1;
struct SysFramebuffer *sdlgpu_current_framebuffer;
struct SysTexture *sdlgpu_current_texture;

static SDL_Window *window;
static uint8_t vsync_enabled;

/* Offscreen display render target (see internal.h). */
static SDL_GPUTexture *display_color, *display_depth;
static SDL_GPUTextureFormat display_depth_format =
    SDL_GPU_TEXTUREFORMAT_INVALID;
static int display_width, display_height;
static int requested_depth_bits, requested_stencil_bits;

/*---------------------------- Render state -----------------------------*/

static int viewport_x, viewport_y, viewport_w, viewport_h;
static float depth_near = 0, depth_far = 1;
static uint8_t clip_enabled;
static int clip_x, clip_y, clip_w, clip_h;

static uint8_t blend_enabled = 1;
static uint8_t blend_op = GRAPHICS_BLEND_ADD;
static uint8_t blend_src = GRAPHICS_BLEND_SRC_ALPHA;
static uint8_t blend_dst = GRAPHICS_BLEND_INV_SRC_ALPHA;
static uint8_t blend_alpha_separate;
static uint8_t blend_alpha_src = GRAPHICS_BLEND_ONE;
static uint8_t blend_alpha_dst = GRAPHICS_BLEND_ZERO;
static Vector4f blend_color;
static uint8_t color_write_mask = 0xF;

static uint8_t depth_test;
static uint8_t depth_compare = GRAPHICS_COMPARISON_LESS;
static uint8_t depth_write = 1;
static uint8_t face_cull;
static uint8_t face_cull_cw;

static uint8_t stencil_test;
static uint8_t stencil_compare = GRAPHICS_COMPARISON_TRUE;
static unsigned int stencil_ref;
static unsigned int stencil_mask = 0xFF;
static uint8_t stencil_sfail = GRAPHICS_STENCIL_KEEP;
static uint8_t stencil_dfail = GRAPHICS_STENCIL_KEEP;
static uint8_t stencil_dpass = GRAPHICS_STENCIL_KEEP;

static uint8_t alpha_test;
static uint8_t alpha_comparison = GRAPHICS_COMPARISON_GREATER_EQUAL;
static float alpha_ref;
static Vector4f fixed_color = {1, 1, 1, 1};
static uint8_t fog_enabled;
static float fog_start = 0, fog_end = 1;
static Vector4f fog_color = {1, 1, 1, 1};
static float point_size = 1;
static Vector2f tex_offset;

static Matrix4f projection_matrix, view_matrix, model_matrix;

/*--------------------------- Command recording --------------------------*/

enum {CMD_DRAW = 1, CMD_CLEAR};

typedef struct Command {
    uint8_t type;
    /* CMD_CLEAR */
    uint8_t clear_color_flag, clear_depth_flag;
    uint8_t clear_stencil;
    SDL_FColor clear_color;
    float clear_depth;
    /* CMD_DRAW */
    uint8_t has_index, index32;
    uint8_t stencil_ref;
    SDL_GPUGraphicsPipeline *pipeline;
    SDL_GPUBuffer *vbuf;        // NULL = stream buffer
    SDL_GPUBuffer *ibuf;        // NULL = stream buffer (if has_index)
    uint32_t voffset, ioffset;
    uint32_t first, count;
    SDL_GPUTexture *texture;
    SDL_GPUSampler *sampler;
    SDL_GPUViewport viewport;
    SDL_Rect scissor;
    SDL_FColor blend_const;
    VSUniforms vs;
    FSUniforms fs;
} Command;

static Command *commands;
static int num_commands, max_commands;

/* CPU arena for immediate vertex/index data, and its GPU counterparts. */
static uint8_t *stream_data;
static uint32_t stream_size, stream_capacity;
static SDL_GPUBuffer *stream_buffer;
static uint32_t stream_buffer_size;
static SDL_GPUTransferBuffer *stream_transfer;
static uint32_t stream_transfer_size;

/* Objects to release after the next flush. */
static SDL_GPUBuffer **release_buffers;
static int num_release_buffers, max_release_buffers;
static SDL_GPUTexture **release_textures;
static int num_release_textures, max_release_textures;

/*----------------------------- Pipeline cache ---------------------------*/

typedef struct PipelineEntry {
    PipelineKey key;
    uint32_t hash;
    SDL_GPUGraphicsPipeline *pipeline;
} PipelineEntry;

static PipelineEntry *pipelines;
static int pipeline_table_size;  // Power of 2.
static int num_pipelines;

/*************************************************************************/
/************************** Local declarations ***************************/
/*************************************************************************/

static int create_display_target(int width, int height);
static void destroy_display_target(void);
static void process_deferred_releases(void);
static void target_textures(SDL_GPUTexture **color_ret,
                            SDL_GPUTexture **depth_ret,
                            SDL_GPUTextureFormat *depth_format_ret);
static Command *new_command(void);
static int stream_append(const void *data, uint32_t size, uint32_t *offset_ret);
static SDL_GPUGraphicsPipeline *get_pipeline(const PipelineKey *key);
static void clear_pipeline_cache(void);
static void fill_state_key(PipelineKey *key);
static void fill_common_draw_state(Command *cmd);
static void safe_clear(const Vector4f *color, const float *depth,
                       unsigned int stencil);
static SDL_GPUBlendFactor convert_blend_factor(int factor, int alpha);
static SDL_GPUCompareOp convert_comparison(int type);
static SDL_GPUStencilOp convert_stencil_op(int op);

/*************************************************************************/
/*************** Platform interface (see sdlgpu.h) ***********************/
/*************************************************************************/

int sdlgpu_create_device(void)
{
    if (sdlgpu_device) {
        return 1;
    }
    const int debug =
#ifdef DEBUG
        SDL_GetHintBoolean("SIL_SDLGPU_DEBUG", false);
#else
        0;
#endif
    sdlgpu_device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB
            | SDL_GPU_SHADERFORMAT_SPIRV,
        debug, NULL);
    if (!sdlgpu_device) {
        DLOG("SDL_CreateGPUDevice() failed: %s", SDL_GetError());
        return 0;
    }
    DLOG("SDL_GPU device created: driver=%s",
         SDL_GetGPUDeviceDriver(sdlgpu_device));
    return 1;
}

/*-----------------------------------------------------------------------*/

int sdlgpu_driver_is_vulkan(void)
{
    return sdlgpu_device
        && strcmp(SDL_GetGPUDeviceDriver(sdlgpu_device), "vulkan") == 0;
}

/*-----------------------------------------------------------------------*/

int sdlgpu_init(SDL_Window *window_, int depth_bits, int stencil_bits,
                int vsync)
{
    window = window_;
    requested_depth_bits = depth_bits;
    requested_stencil_bits = stencil_bits;

    if (!sdlgpu_create_device()) {
        return 0;
    }
    if (!SDL_ClaimWindowForGPUDevice(sdlgpu_device, window)) {
        DLOG("SDL_ClaimWindowForGPUDevice() failed: %s", SDL_GetError());
        SDL_DestroyGPUDevice(sdlgpu_device);
        sdlgpu_device = NULL;
        return 0;
    }
    sdlgpu_set_vsync(vsync);

    if (!sdlgpu_shader_init()) {
        SDL_ReleaseWindowFromGPUDevice(sdlgpu_device, window);
        SDL_DestroyGPUDevice(sdlgpu_device);
        sdlgpu_device = NULL;
        return 0;
    }
    sdlgpu_texture_init();

    projection_matrix = mat4_identity;
    view_matrix = mat4_identity;
    model_matrix = mat4_identity;

    pipeline_table_size = 256;
    pipelines = mem_alloc(sizeof(*pipelines) * pipeline_table_size, 0,
                          MEM_ALLOC_CLEAR);
    if (!pipelines) {
        sdlgpu_cleanup();
        return 0;
    }

    return 1;
}

/*-----------------------------------------------------------------------*/

void sdlgpu_cleanup(void)
{
    if (!sdlgpu_device) {
        return;
    }
    sdlgpu_flush();
    SDL_WaitForGPUIdle(sdlgpu_device);
    process_deferred_releases();
    clear_pipeline_cache();
    mem_free(pipelines);
    pipelines = NULL;
    pipeline_table_size = 0;
    destroy_display_target();
    sdlgpu_primitive_cleanup();
    sdlgpu_texture_cleanup();
    sdlgpu_shader_cleanup();
    if (stream_buffer) {
        SDL_ReleaseGPUBuffer(sdlgpu_device, stream_buffer);
        stream_buffer = NULL;
        stream_buffer_size = 0;
    }
    if (stream_transfer) {
        SDL_ReleaseGPUTransferBuffer(sdlgpu_device, stream_transfer);
        stream_transfer = NULL;
        stream_transfer_size = 0;
    }
    mem_free(stream_data);
    stream_data = NULL;
    stream_size = stream_capacity = 0;
    mem_free(commands);
    commands = NULL;
    num_commands = max_commands = 0;
    mem_free(release_buffers);
    release_buffers = NULL;
    num_release_buffers = max_release_buffers = 0;
    mem_free(release_textures);
    release_textures = NULL;
    num_release_textures = max_release_textures = 0;
    SDL_ReleaseWindowFromGPUDevice(sdlgpu_device, window);
    SDL_DestroyGPUDevice(sdlgpu_device);
    sdlgpu_device = NULL;
    window = NULL;
}

/*-----------------------------------------------------------------------*/

void sdlgpu_set_vsync(int vsync)
{
    vsync_enabled = (vsync != 0);
    if (!sdlgpu_device) {
        return;
    }
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (!vsync_enabled) {
        if (SDL_WindowSupportsGPUPresentMode(sdlgpu_device, window,
                                             SDL_GPU_PRESENTMODE_IMMEDIATE)) {
            mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
        } else if (SDL_WindowSupportsGPUPresentMode(
                       sdlgpu_device, window, SDL_GPU_PRESENTMODE_MAILBOX)) {
            mode = SDL_GPU_PRESENTMODE_MAILBOX;
        }
    }
    if (!SDL_SetGPUSwapchainParameters(sdlgpu_device, window,
                                       SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                       mode)) {
        DLOG("SDL_SetGPUSwapchainParameters() failed: %s", SDL_GetError());
    } else {
        DLOG("Present mode: %s", mode == SDL_GPU_PRESENTMODE_VSYNC ? "vsync"
             : mode == SDL_GPU_PRESENTMODE_IMMEDIATE ? "immediate"
             : "mailbox");
    }
}

/*-----------------------------------------------------------------------*/

void sdlgpu_start_frame(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    if (width != display_width || height != display_height
     || !display_color) {
        sdlgpu_flush();
        destroy_display_target();
        if (!create_display_target(width, height)) {
            DLOG("Failed to create %dx%d display target", width, height);
        }
    }
}

/*-----------------------------------------------------------------------*/

void sdlgpu_finish_frame(void)
{
    if (sdlgpu_current_framebuffer) {
        sdlgpu_set_render_target(NULL);
    }
    sdlgpu_flush();
    if (!display_color) {
        return;
    }

    SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(sdlgpu_device);
    if (!cmdbuf) {
        DLOG("SDL_AcquireGPUCommandBuffer() failed: %s", SDL_GetError());
        return;
    }
    SDL_GPUTexture *swapchain;
    Uint32 sw, sh;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmdbuf, window, &swapchain,
                                               &sw, &sh)) {
        DLOG("Failed to acquire swapchain texture: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmdbuf);
        return;
    }
    if (swapchain) {
        /* The display texture is stored with GL row order (row 0 at the
         * bottom), so flip it vertically while copying. */
        SDL_GPUBlitInfo blit;
        mem_clear(&blit, sizeof(blit));
        blit.source.texture = display_color;
        blit.source.w = display_width;
        blit.source.h = display_height;
        blit.destination.texture = swapchain;
        blit.destination.w = sw;
        blit.destination.h = sh;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.flip_mode = SDL_FLIP_VERTICAL;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(cmdbuf, &blit);
    }
    if (!SDL_SubmitGPUCommandBuffer(cmdbuf)) {
        DLOG("SDL_SubmitGPUCommandBuffer() failed: %s", SDL_GetError());
    }
}

/*-----------------------------------------------------------------------*/

void sdlgpu_sync(UNUSED int flush)
{
    if (sdlgpu_device) {
        sdlgpu_flush();
        SDL_WaitForGPUIdle(sdlgpu_device);
        process_deferred_releases();
    }
}

/*************************************************************************/
/******************** sysdep interface: basic graphics *******************/
/*************************************************************************/

const char *sys_graphics_renderer_info(void)
{
    static char buf[100];
    if (!sdlgpu_device) {
        return "SDL_GPU (not initialized)";
    }
    SDL_snprintf(buf, sizeof(buf), "SDL_GPU %d.%d.%d (%s, %s shaders)",
                 SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
                 SDL_GetGPUDeviceDriver(sdlgpu_device),
                 sdlgpu_shader_language());
    return buf;
}

/*-----------------------------------------------------------------------*/

void sys_graphics_clear(const Vector4f *color, const float *depth,
                        unsigned int stencil)
{
    int target_w, target_h;
    sdlgpu_target_size(&target_w, &target_h);
    const int full_clip = !clip_enabled
        || (clip_x <= 0 && clip_y <= 0
            && clip_x + clip_w >= target_w && clip_y + clip_h >= target_h);
    if (!full_clip || (color && color_write_mask != 0xF)) {
        safe_clear(color, depth, stencil);
        return;
    }

    /* Merge with an immediately preceding clear if possible. */
    Command *cmd;
    if (num_commands > 0 && commands[num_commands-1].type == CMD_CLEAR) {
        cmd = &commands[num_commands-1];
    } else {
        cmd = new_command();
        if (!cmd) {
            return;
        }
        cmd->type = CMD_CLEAR;
    }
    if (color) {
        cmd->clear_color_flag = 1;
        cmd->clear_color.r = color->x;
        cmd->clear_color.g = color->y;
        cmd->clear_color.b = color->z;
        cmd->clear_color.a = color->w;
    }
    if (depth) {
        cmd->clear_depth_flag = 1;
        cmd->clear_depth = bound(*depth, 0.0f, 1.0f);
        cmd->clear_stencil = (uint8_t)stencil;
    }
}

/*-----------------------------------------------------------------------*/

int sys_graphics_read_pixels(int x, int y, int w, int h, int stride,
                             void *buffer)
{
    sdlgpu_flush();
    SDL_GPUTexture *color, *depth;
    SDL_GPUTextureFormat depth_format;
    target_textures(&color, &depth, &depth_format);
    if (!color) {
        return 0;
    }
    int target_w, target_h;
    sdlgpu_target_size(&target_w, &target_h);
    if (x + w > target_w) {
        w = target_w - x;
    }
    if (y + h > target_h) {
        h = target_h - y;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }
    if (stride == w) {
        return sdlgpu_read_texture(color, sdlgpu_color_format(),
                                   x, y, w, h, buffer);
    }
    uint8_t *temp = mem_alloc(w * h * 4, 0, MEM_ALLOC_TEMP);
    if (!temp) {
        return 0;
    }
    const int ok = sdlgpu_read_texture(color, sdlgpu_color_format(),
                                       x, y, w, h, temp);
    if (ok) {
        for (int row = 0; row < h; row++) {
            memcpy((uint8_t *)buffer + row * stride * 4, temp + row * w * 4,
                   w * 4);
        }
    }
    mem_free(temp);
    return ok;
}

/*************************************************************************/
/******************** sysdep interface: render state *********************/
/*************************************************************************/

void sys_graphics_set_viewport(int left, int bottom, int width, int height)
{
    viewport_x = left;
    viewport_y = bottom;
    viewport_w = width;
    viewport_h = height;
}

void sys_graphics_set_clip_region(int left, int bottom, int width, int height)
{
    clip_x = left;
    clip_y = bottom;
    clip_w = width;
    clip_h = height;
}

void sys_graphics_set_depth_range(float near, float far)
{
    depth_near = near;
    depth_far = far;
}

/*-----------------------------------------------------------------------*/

int sys_graphics_set_blend(int operation, int src_factor, int dest_factor)
{
    blend_op = (uint8_t)operation;
    blend_src = (uint8_t)src_factor;
    blend_dst = (uint8_t)dest_factor;
    blend_enabled = !(operation == GRAPHICS_BLEND_ADD
                      && src_factor == GRAPHICS_BLEND_ONE
                      && dest_factor == GRAPHICS_BLEND_ZERO);
    return 1;
}

int sys_graphics_set_blend_alpha(int enable, int src_factor, int dest_factor)
{
    blend_alpha_separate = (enable != 0);
    blend_alpha_src = (uint8_t)src_factor;
    blend_alpha_dst = (uint8_t)dest_factor;
    return 1;
}

/*-----------------------------------------------------------------------*/

void sys_graphics_set_int_param(SysGraphicsParam id, int value)
{
    switch (id) {
      case SYS_GRAPHICS_PARAM_ALPHA_TEST:
        alpha_test = (value != 0); return;
      case SYS_GRAPHICS_PARAM_ALPHA_TEST_COMPARISON:
        alpha_comparison = (uint8_t)value; return;
      case SYS_GRAPHICS_PARAM_CLIP:
        clip_enabled = (value != 0); return;
      case SYS_GRAPHICS_PARAM_COLOR_WRITE:
        color_write_mask = (uint8_t)(value & 0xF); return;
      case SYS_GRAPHICS_PARAM_DEPTH_TEST:
        depth_test = (value != 0); return;
      case SYS_GRAPHICS_PARAM_DEPTH_TEST_COMPARISON:
        depth_compare = (uint8_t)value; return;
      case SYS_GRAPHICS_PARAM_DEPTH_WRITE:
        depth_write = (value != 0); return;
      case SYS_GRAPHICS_PARAM_FACE_CULL:
        face_cull = (value != 0); return;
      case SYS_GRAPHICS_PARAM_FACE_CULL_CW:
        face_cull_cw = (value != 0); return;
      case SYS_GRAPHICS_PARAM_FOG:
        fog_enabled = (value != 0); return;
      case SYS_GRAPHICS_PARAM_STENCIL_TEST:
        stencil_test = (value != 0); return;
      case SYS_GRAPHICS_PARAM_STENCIL_COMPARISON:
        stencil_compare = (uint8_t)value; return;
      case SYS_GRAPHICS_PARAM_STENCIL_REFERENCE:
        stencil_ref = (unsigned int)value; return;
      case SYS_GRAPHICS_PARAM_STENCIL_MASK:
        stencil_mask = (unsigned int)value; return;
      case SYS_GRAPHICS_PARAM_STENCIL_OP_SFAIL:
        stencil_sfail = (uint8_t)value; return;
      case SYS_GRAPHICS_PARAM_STENCIL_OP_DFAIL:
        stencil_dfail = (uint8_t)value; return;
      case SYS_GRAPHICS_PARAM_STENCIL_OP_DPASS:
        stencil_dpass = (uint8_t)value; return;
      default:
        ASSERT(!"wrong type", return);
    }
}

void sys_graphics_set_float_param(SysGraphicsParam id, float value)
{
    switch (id) {
      case SYS_GRAPHICS_PARAM_ALPHA_REFERENCE:
        alpha_ref = bound(value, 0.0f, 1.0f); return;
      case SYS_GRAPHICS_PARAM_FOG_START:
        fog_start = value; return;
      case SYS_GRAPHICS_PARAM_FOG_END:
        fog_end = value; return;
      case SYS_GRAPHICS_PARAM_POINT_SIZE:
        point_size = value; return;
      default:
        ASSERT(!"wrong type", return);
    }
}

void sys_graphics_set_vec2_param(SysGraphicsParam id, const Vector2f *value)
{
    switch (id) {
      case SYS_GRAPHICS_PARAM_TEXTURE_OFFSET:
        tex_offset = *value; return;
      default:
        ASSERT(!"wrong type", return);
    }
}

void sys_graphics_set_vec4_param(SysGraphicsParam id, const Vector4f *value)
{
    Vector4f clamped = {bound(value->x, 0.0f, 1.0f), bound(value->y, 0.0f, 1.0f),
                        bound(value->z, 0.0f, 1.0f), bound(value->w, 0.0f, 1.0f)};
    switch (id) {
      case SYS_GRAPHICS_PARAM_BLEND_COLOR:
        blend_color = clamped; return;
      case SYS_GRAPHICS_PARAM_FIXED_COLOR:
        fixed_color = clamped; return;
      case SYS_GRAPHICS_PARAM_FOG_COLOR:
        fog_color = clamped; return;
      default:
        ASSERT(!"wrong type", return);
    }
}

void sys_graphics_set_matrix_param(SysGraphicsParam id, const Matrix4f *value)
{
    switch (id) {
      case SYS_GRAPHICS_PARAM_PROJECTION_MATRIX:
        projection_matrix = *value; return;
      case SYS_GRAPHICS_PARAM_VIEW_MATRIX:
        view_matrix = *value; return;
      case SYS_GRAPHICS_PARAM_MODEL_MATRIX:
        model_matrix = *value; return;
      default:
        ASSERT(!"wrong type", return);
    }
}

void sys_graphics_get_matrix_param(SysGraphicsParam id, Matrix4f *value_ret)
{
    switch (id) {
      case SYS_GRAPHICS_PARAM_PROJECTION_MATRIX:
        *value_ret = projection_matrix; return;
      case SYS_GRAPHICS_PARAM_VIEW_MATRIX:
        *value_ret = view_matrix; return;
      case SYS_GRAPHICS_PARAM_MODEL_MATRIX:
        *value_ret = model_matrix; return;
      default:
        ASSERT(!"wrong type", return);
    }
}

float sys_graphics_max_point_size(void)
{
    return 64;
}

/*************************************************************************/
/***************** Internal interface: render targets ********************/
/*************************************************************************/

void sdlgpu_set_render_target(struct SysFramebuffer *framebuffer)
{
    if (framebuffer == sdlgpu_current_framebuffer) {
        return;
    }
    sdlgpu_flush();
    sdlgpu_current_framebuffer = framebuffer;
}

/*-----------------------------------------------------------------------*/

void sdlgpu_target_size(int *width_ret, int *height_ret)
{
    if (sdlgpu_current_framebuffer) {
        *width_ret = sdlgpu_current_framebuffer->width;
        *height_ret = sdlgpu_current_framebuffer->height;
    } else {
        *width_ret = display_width;
        *height_ret = display_height;
    }
}

/*-----------------------------------------------------------------------*/

SDL_GPUTextureFormat sdlgpu_color_format(void)
{
    return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
}

/*-----------------------------------------------------------------------*/

SDL_GPUTextureFormat sdlgpu_depth_format(int depth_bits, int stencil_bits)
{
    static const SDL_GPUTextureFormat with_stencil[] = {
        SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT,
    };
    static const SDL_GPUTextureFormat depth_24[] = {
        SDL_GPU_TEXTUREFORMAT_D24_UNORM,
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT,
    };
    static const SDL_GPUTextureFormat depth_16[] = {
        SDL_GPU_TEXTUREFORMAT_D16_UNORM,
        SDL_GPU_TEXTUREFORMAT_D24_UNORM,
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
    };
    const SDL_GPUTextureFormat *list;
    int count;
    if (stencil_bits > 0) {
        list = with_stencil; count = lenof(with_stencil);
    } else if (depth_bits > 16) {
        list = depth_24; count = lenof(depth_24);
    } else if (depth_bits > 0) {
        list = depth_16; count = lenof(depth_16);
    } else {
        return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
    for (int i = 0; i < count; i++) {
        if (SDL_GPUTextureSupportsFormat(
                sdlgpu_device, list[i], SDL_GPU_TEXTURETYPE_2D,
                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            return list[i];
        }
    }
    return SDL_GPU_TEXTUREFORMAT_INVALID;
}

/*-----------------------------------------------------------------------*/

void sdlgpu_defer_release_buffer(SDL_GPUBuffer *buffer)
{
    if (!buffer) {
        return;
    }
    if (num_release_buffers >= max_release_buffers) {
        const int new_max = max_release_buffers ? max_release_buffers * 2 : 64;
        SDL_GPUBuffer **new_list = mem_realloc(
            release_buffers, sizeof(*new_list) * new_max, 0);
        if (!new_list) {
            /* Better to leak than to release a buffer still in use. */
            DLOG("No memory for deferred release list");
            return;
        }
        release_buffers = new_list;
        max_release_buffers = new_max;
    }
    release_buffers[num_release_buffers++] = buffer;
}

void sdlgpu_defer_release_texture(SDL_GPUTexture *texture)
{
    if (!texture) {
        return;
    }
    if (num_release_textures >= max_release_textures) {
        const int new_max =
            max_release_textures ? max_release_textures * 2 : 64;
        SDL_GPUTexture **new_list = mem_realloc(
            release_textures, sizeof(*new_list) * new_max, 0);
        if (!new_list) {
            DLOG("No memory for deferred release list");
            return;
        }
        release_textures = new_list;
        max_release_textures = new_max;
    }
    release_textures[num_release_textures++] = texture;
}

/*************************************************************************/
/******************* Internal interface: draw recording ******************/
/*************************************************************************/

void sdlgpu_record_draw(struct SysPrimitive *primitive, int first, int count)
{
    if (count <= 0) {
        return;
    }
    SDL_GPUTexture *color_target, *depth_target;
    SDL_GPUTextureFormat depth_format;
    target_textures(&color_target, &depth_target, &depth_format);
    if (!color_target) {
        return;
    }

    PipelineKey key;
    mem_clear(&key, sizeof(key));
    key.prim_type = (uint8_t)primitive->type;
    key.pos_format = primitive->pos_format;
    key.color_format = primitive->color_format;
    key.has_texcoord = primitive->has_texcoord;
    key.stride = primitive->stride;
    key.pos_offset = primitive->pos_offset;
    key.tex_offset = primitive->has_texcoord ? primitive->tex_offset : 0;
    key.color_offset = primitive->color_format ? primitive->color_offset : 0;
    fill_state_key(&key);
    key.depth_target_format = (uint8_t)depth_format;
    if (!depth_target) {
        key.depth_test = key.depth_write = key.depth_compare = 0;
        key.stencil_test = key.stencil_compare = key.stencil_fail = 0;
        key.stencil_dfail = key.stencil_pass = key.stencil_mask = 0;
    }
    SDL_GPUGraphicsPipeline *pipeline = get_pipeline(&key);
    if (!pipeline) {
        return;
    }

    /* Store immediate data in the stream arena before allocating the
     * command, since the arena may need to grow. */
    uint32_t voffset = 0, ioffset = 0;
    if (primitive->immediate) {
        if (!stream_append(primitive->vdata, primitive->vdata_size,
                           &voffset)) {
            return;
        }
        if (primitive->idata
         && !stream_append(primitive->idata, primitive->idata_size,
                           &ioffset)) {
            return;
        }
    }

    Command *cmd = new_command();
    if (!cmd) {
        return;
    }
    cmd->type = CMD_DRAW;
    cmd->pipeline = pipeline;
    if (primitive->immediate) {
        cmd->vbuf = NULL;
        cmd->voffset = voffset;
        cmd->has_index = (primitive->idata != NULL);
        cmd->ibuf = NULL;
        cmd->ioffset = ioffset;
    } else {
        cmd->vbuf = primitive->vbuf;
        cmd->voffset = 0;
        cmd->has_index = (primitive->ibuf != NULL);
        cmd->ibuf = primitive->ibuf;
        cmd->ioffset = 0;
    }
    cmd->index32 = primitive->index32;
    cmd->first = (uint32_t)first;
    cmd->count = (uint32_t)count;
    fill_common_draw_state(cmd);

    /* Texture and fragment shader texture mode. */
    struct SysTexture *texture = sdlgpu_current_texture;
    if (texture && primitive->has_texcoord && texture->tex) {
        cmd->texture = texture->tex;
        cmd->sampler = sdlgpu_texture_sampler(texture);
        cmd->fs.texmode = texture->color_type;
        texture->batch_serial = sdlgpu_batch_serial;
    } else {
        cmd->texture = sdlgpu_dummy_texture();
        cmd->sampler = sdlgpu_texture_sampler(NULL);
        cmd->fs.texmode = TEXCOLOR_NONE;
    }
}

/*************************************************************************/
/************************ Internal interface: flush **********************/
/*************************************************************************/

/* Begin a render pass on the current target, applying the given clear
 * command (may be NULL) as load operations. */
static SDL_GPURenderPass *begin_pass(SDL_GPUCommandBuffer *cmdbuf,
                                     SDL_GPUTexture *color,
                                     SDL_GPUTexture *depth,
                                     SDL_GPUTextureFormat depth_format,
                                     const Command *clear)
{
    SDL_GPUColorTargetInfo color_info;
    mem_clear(&color_info, sizeof(color_info));
    color_info.texture = color;
    color_info.store_op = SDL_GPU_STOREOP_STORE;
    if (clear && clear->clear_color_flag) {
        color_info.load_op = SDL_GPU_LOADOP_CLEAR;
        color_info.clear_color = clear->clear_color;
    } else {
        color_info.load_op = SDL_GPU_LOADOP_LOAD;
    }

    SDL_GPUDepthStencilTargetInfo depth_info;
    mem_clear(&depth_info, sizeof(depth_info));
    if (depth) {
        const int has_stencil =
            (depth_format == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT
             || depth_format == SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT);
        depth_info.texture = depth;
        depth_info.store_op = SDL_GPU_STOREOP_STORE;
        depth_info.stencil_store_op =
            has_stencil ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
        if (clear && clear->clear_depth_flag) {
            depth_info.load_op = SDL_GPU_LOADOP_CLEAR;
            depth_info.clear_depth = clear->clear_depth;
            depth_info.stencil_load_op =
                has_stencil ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_DONT_CARE;
            depth_info.clear_stencil = clear->clear_stencil;
        } else {
            depth_info.load_op = SDL_GPU_LOADOP_LOAD;
            depth_info.stencil_load_op =
                has_stencil ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
        }
    }

    return SDL_BeginGPURenderPass(cmdbuf, &color_info, 1,
                                  depth ? &depth_info : NULL);
}

/*-----------------------------------------------------------------------*/

void sdlgpu_flush(void)
{
    if (num_commands == 0) {
        process_deferred_releases();
        return;
    }

    SDL_GPUTexture *color, *depth;
    SDL_GPUTextureFormat depth_format;
    target_textures(&color, &depth, &depth_format);

    SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(sdlgpu_device);
    if (!cmdbuf || !color) {
        DLOG("Cannot flush: %s", !cmdbuf ? SDL_GetError() : "no target");
        if (cmdbuf) {
            SDL_CancelGPUCommandBuffer(cmdbuf);
        }
        num_commands = 0;
        stream_size = 0;
        sdlgpu_batch_serial++;
        return;
    }

    /* Upload immediate-mode vertex and index data. */
    if (stream_size > 0) {
        if (stream_buffer_size < stream_size) {
            if (stream_buffer) {
                SDL_ReleaseGPUBuffer(sdlgpu_device, stream_buffer);
            }
            uint32_t new_size = 65536;
            while (new_size < stream_size) {
                new_size *= 2;
            }
            SDL_GPUBufferCreateInfo info;
            mem_clear(&info, sizeof(info));
            info.usage = SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_INDEX;
            info.size = new_size;
            stream_buffer = SDL_CreateGPUBuffer(sdlgpu_device, &info);
            stream_buffer_size = stream_buffer ? new_size : 0;
        }
        if (stream_transfer_size < stream_size) {
            if (stream_transfer) {
                SDL_ReleaseGPUTransferBuffer(sdlgpu_device, stream_transfer);
            }
            SDL_GPUTransferBufferCreateInfo info;
            mem_clear(&info, sizeof(info));
            info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            info.size = stream_buffer_size;
            stream_transfer = SDL_CreateGPUTransferBuffer(sdlgpu_device, &info);
            stream_transfer_size = stream_transfer ? stream_buffer_size : 0;
        }
        if (stream_buffer && stream_transfer) {
            void *map = SDL_MapGPUTransferBuffer(sdlgpu_device,
                                                 stream_transfer, true);
            if (map) {
                memcpy(map, stream_data, stream_size);
                SDL_UnmapGPUTransferBuffer(sdlgpu_device, stream_transfer);
                SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmdbuf);
                const SDL_GPUTransferBufferLocation src = {
                    .transfer_buffer = stream_transfer, .offset = 0};
                const SDL_GPUBufferRegion dest = {
                    .buffer = stream_buffer, .offset = 0, .size = stream_size};
                SDL_UploadToGPUBuffer(copy, &src, &dest, true);
                SDL_EndGPUCopyPass(copy);
            }
        } else {
            DLOG("Failed to allocate stream buffers (%u bytes)", stream_size);
        }
    }

    /* Replay the command list. */
    SDL_GPURenderPass *pass = NULL;
    const Command *pending_clear = NULL;
    SDL_GPUGraphicsPipeline *cur_pipeline = NULL;
    const SDL_GPUBuffer *cur_vbuf = NULL;
    uint32_t cur_voffset = 0;
    SDL_GPUTexture *cur_texture = NULL;
    SDL_GPUSampler *cur_sampler = NULL;
    SDL_GPUViewport cur_viewport;
    SDL_Rect cur_scissor;
    int cur_stencil_ref = -1;
    SDL_FColor cur_blend = {-1, -1, -1, -1};
    VSUniforms cur_vs;
    FSUniforms cur_fs;
    int state_valid = 0;

    for (int i = 0; i < num_commands; i++) {
        const Command *cmd = &commands[i];
        if (cmd->type == CMD_CLEAR) {
            if (pass) {
                SDL_EndGPURenderPass(pass);
                pass = NULL;
            }
            pending_clear = cmd;
            continue;
        }

        if (!pass) {
            pass = begin_pass(cmdbuf, color, depth, depth_format,
                              pending_clear);
            pending_clear = NULL;
            state_valid = 0;
            if (!pass) {
                DLOG("SDL_BeginGPURenderPass() failed: %s", SDL_GetError());
                break;
            }
        }

        const SDL_GPUBuffer *vbuf = cmd->vbuf ? cmd->vbuf : stream_buffer;
        if (!vbuf) {
            continue;
        }

        if (!state_valid || cmd->pipeline != cur_pipeline) {
            SDL_BindGPUGraphicsPipeline(pass, cmd->pipeline);
            cur_pipeline = cmd->pipeline;
        }
        if (!state_valid
         || memcmp(&cmd->viewport, &cur_viewport, sizeof(cur_viewport)) != 0) {
            SDL_SetGPUViewport(pass, &cmd->viewport);
            cur_viewport = cmd->viewport;
        }
        if (!state_valid
         || memcmp(&cmd->scissor, &cur_scissor, sizeof(cur_scissor)) != 0) {
            SDL_SetGPUScissor(pass, &cmd->scissor);
            cur_scissor = cmd->scissor;
        }
        if (!state_valid || cmd->stencil_ref != cur_stencil_ref) {
            SDL_SetGPUStencilReference(pass, cmd->stencil_ref);
            cur_stencil_ref = cmd->stencil_ref;
        }
        if (!state_valid
         || memcmp(&cmd->blend_const, &cur_blend, sizeof(cur_blend)) != 0) {
            SDL_SetGPUBlendConstants(pass, cmd->blend_const);
            cur_blend = cmd->blend_const;
        }
        if (!state_valid || vbuf != cur_vbuf || cmd->voffset != cur_voffset) {
            const SDL_GPUBufferBinding binding = {
                .buffer = (SDL_GPUBuffer *)vbuf, .offset = cmd->voffset};
            SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
            cur_vbuf = vbuf;
            cur_voffset = cmd->voffset;
        }
        if (!state_valid || cmd->texture != cur_texture
         || cmd->sampler != cur_sampler) {
            const SDL_GPUTextureSamplerBinding binding = {
                .texture = cmd->texture, .sampler = cmd->sampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            cur_texture = cmd->texture;
            cur_sampler = cmd->sampler;
        }
        if (!state_valid || memcmp(&cmd->vs, &cur_vs, sizeof(cur_vs)) != 0) {
            SDL_PushGPUVertexUniformData(cmdbuf, 0, &cmd->vs, sizeof(cmd->vs));
            cur_vs = cmd->vs;
        }
        if (!state_valid || memcmp(&cmd->fs, &cur_fs, sizeof(cur_fs)) != 0) {
            SDL_PushGPUFragmentUniformData(cmdbuf, 0, &cmd->fs, sizeof(cmd->fs));
            cur_fs = cmd->fs;
        }
        state_valid = 1;

        if (cmd->has_index) {
            const SDL_GPUBuffer *ibuf = cmd->ibuf ? cmd->ibuf : stream_buffer;
            const SDL_GPUBufferBinding binding = {
                .buffer = (SDL_GPUBuffer *)ibuf, .offset = cmd->ioffset};
            SDL_BindGPUIndexBuffer(pass, &binding,
                                   cmd->index32
                                       ? SDL_GPU_INDEXELEMENTSIZE_32BIT
                                       : SDL_GPU_INDEXELEMENTSIZE_16BIT);
            SDL_DrawGPUIndexedPrimitives(pass, cmd->count, 1, cmd->first, 0, 0);
        } else {
            SDL_DrawGPUPrimitives(pass, cmd->count, 1, cmd->first, 0);
        }
    }

    if (pending_clear && !pass) {
        pass = begin_pass(cmdbuf, color, depth, depth_format, pending_clear);
    }
    if (pass) {
        SDL_EndGPURenderPass(pass);
    }
    if (!SDL_SubmitGPUCommandBuffer(cmdbuf)) {
        DLOG("SDL_SubmitGPUCommandBuffer() failed: %s", SDL_GetError());
    }

    num_commands = 0;
    stream_size = 0;
    sdlgpu_batch_serial++;
    process_deferred_releases();
}

/*************************************************************************/
/******************* Internal interface: data transfer *******************/
/*************************************************************************/

static int format_bpp(SDL_GPUTextureFormat format)
{
    return (format == SDL_GPU_TEXTUREFORMAT_R8_UNORM) ? 1 : 4;
}

/*-----------------------------------------------------------------------*/

int sdlgpu_read_texture(SDL_GPUTexture *texture, SDL_GPUTextureFormat format,
                        int x, int y, int w, int h, void *buffer)
{
    const int bpp = format_bpp(format);
    const uint32_t size = (uint32_t)(w * h * bpp);
    SDL_GPUTransferBufferCreateInfo tinfo;
    mem_clear(&tinfo, sizeof(tinfo));
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tinfo.size = size;
    SDL_GPUTransferBuffer *transfer =
        SDL_CreateGPUTransferBuffer(sdlgpu_device, &tinfo);
    if (!transfer) {
        DLOG("Failed to create download buffer: %s", SDL_GetError());
        return 0;
    }
    SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(sdlgpu_device);
    if (!cmdbuf) {
        SDL_ReleaseGPUTransferBuffer(sdlgpu_device, transfer);
        return 0;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmdbuf);
    SDL_GPUTextureRegion region;
    mem_clear(&region, sizeof(region));
    region.texture = texture;
    region.x = x;
    region.y = y;
    region.w = w;
    region.h = h;
    region.d = 1;
    const SDL_GPUTextureTransferInfo dest = {
        .transfer_buffer = transfer, .offset = 0,
        .pixels_per_row = (Uint32)w, .rows_per_layer = (Uint32)h};
    SDL_DownloadFromGPUTexture(copy, &region, &dest);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdbuf);
    int ok = 0;
    if (fence) {
        SDL_WaitForGPUFences(sdlgpu_device, true, &fence, 1);
        SDL_ReleaseGPUFence(sdlgpu_device, fence);
        const void *map = SDL_MapGPUTransferBuffer(sdlgpu_device, transfer,
                                                   false);
        if (map) {
            memcpy(buffer, map, size);
            SDL_UnmapGPUTransferBuffer(sdlgpu_device, transfer);
            ok = 1;
        }
    }
    SDL_ReleaseGPUTransferBuffer(sdlgpu_device, transfer);
    return ok;
}

/*-----------------------------------------------------------------------*/

int sdlgpu_upload_texture(struct SysTexture *texture, int level,
                          int x, int y, int w, int h,
                          const void *data, int pixels_per_row)
{
    if (texture->batch_serial == sdlgpu_batch_serial && num_commands > 0) {
        /* Draws recorded earlier in this batch must see the old data. */
        sdlgpu_flush();
    }

    const int bpp = format_bpp(texture->format);
    const uint32_t size = (uint32_t)(pixels_per_row * h * bpp);
    SDL_GPUTransferBufferCreateInfo tinfo;
    mem_clear(&tinfo, sizeof(tinfo));
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = size;
    SDL_GPUTransferBuffer *transfer =
        SDL_CreateGPUTransferBuffer(sdlgpu_device, &tinfo);
    if (!transfer) {
        DLOG("Failed to create upload buffer: %s", SDL_GetError());
        return 0;
    }
    void *map = SDL_MapGPUTransferBuffer(sdlgpu_device, transfer, false);
    if (!map) {
        SDL_ReleaseGPUTransferBuffer(sdlgpu_device, transfer);
        return 0;
    }
    memcpy(map, data, size);
    SDL_UnmapGPUTransferBuffer(sdlgpu_device, transfer);

    SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(sdlgpu_device);
    if (!cmdbuf) {
        SDL_ReleaseGPUTransferBuffer(sdlgpu_device, transfer);
        return 0;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmdbuf);
    const SDL_GPUTextureTransferInfo src = {
        .transfer_buffer = transfer, .offset = 0,
        .pixels_per_row = (Uint32)pixels_per_row, .rows_per_layer = (Uint32)h};
    SDL_GPUTextureRegion region;
    mem_clear(&region, sizeof(region));
    region.texture = texture->tex;
    region.mip_level = level;
    region.x = x;
    region.y = y;
    region.w = w;
    region.h = h;
    region.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &region, false);
    SDL_EndGPUCopyPass(copy);
    if (texture->auto_mipmaps && level == 0 && texture->num_levels > 1) {
        SDL_GenerateMipmapsForGPUTexture(cmdbuf, texture->tex);
    }
    const int ok = SDL_SubmitGPUCommandBuffer(cmdbuf);
    SDL_ReleaseGPUTransferBuffer(sdlgpu_device, transfer);
    texture->empty = 0;
    return ok;
}

/*-----------------------------------------------------------------------*/

int sdlgpu_upload_buffer(SDL_GPUBuffer *buffer, const void *data,
                         uint32_t size)
{
    SDL_GPUTransferBufferCreateInfo tinfo;
    mem_clear(&tinfo, sizeof(tinfo));
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = size;
    SDL_GPUTransferBuffer *transfer =
        SDL_CreateGPUTransferBuffer(sdlgpu_device, &tinfo);
    if (!transfer) {
        return 0;
    }
    void *map = SDL_MapGPUTransferBuffer(sdlgpu_device, transfer, false);
    if (!map) {
        SDL_ReleaseGPUTransferBuffer(sdlgpu_device, transfer);
        return 0;
    }
    memcpy(map, data, size);
    SDL_UnmapGPUTransferBuffer(sdlgpu_device, transfer);
    SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(sdlgpu_device);
    if (!cmdbuf) {
        SDL_ReleaseGPUTransferBuffer(sdlgpu_device, transfer);
        return 0;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmdbuf);
    const SDL_GPUTransferBufferLocation src = {.transfer_buffer = transfer,
                                               .offset = 0};
    const SDL_GPUBufferRegion dest = {.buffer = buffer, .offset = 0,
                                      .size = size};
    SDL_UploadToGPUBuffer(copy, &src, &dest, false);
    SDL_EndGPUCopyPass(copy);
    const int ok = SDL_SubmitGPUCommandBuffer(cmdbuf);
    SDL_ReleaseGPUTransferBuffer(sdlgpu_device, transfer);
    return ok;
}

/*-----------------------------------------------------------------------*/

int sdlgpu_copy_from_target(struct SysTexture *texture, int src_x, int src_y,
                            int dest_x, int dest_y, int w, int h)
{
    sdlgpu_flush();
    SDL_GPUTexture *color, *depth;
    SDL_GPUTextureFormat depth_format;
    target_textures(&color, &depth, &depth_format);
    if (!color) {
        return 0;
    }
    int target_w, target_h;
    sdlgpu_target_size(&target_w, &target_h);
    /* Clip the source rectangle to the render target. */
    if (src_x < 0) {
        dest_x -= src_x;
        w += src_x;
        src_x = 0;
    }
    if (src_y < 0) {
        dest_y -= src_y;
        h += src_y;
        src_y = 0;
    }
    if (src_x + w > target_w) {
        w = target_w - src_x;
    }
    if (src_y + h > target_h) {
        h = target_h - src_y;
    }
    if (w <= 0 || h <= 0) {
        return 1;
    }
    SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(sdlgpu_device);
    if (!cmdbuf) {
        return 0;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmdbuf);
    SDL_GPUTextureLocation src, dest;
    mem_clear(&src, sizeof(src));
    mem_clear(&dest, sizeof(dest));
    src.texture = color;
    src.x = src_x;
    src.y = src_y;
    dest.texture = texture->tex;
    dest.x = dest_x;
    dest.y = dest_y;
    SDL_CopyGPUTextureToTexture(copy, &src, &dest, w, h, 1, false);
    SDL_EndGPUCopyPass(copy);
    if (texture->auto_mipmaps && texture->num_levels > 1) {
        SDL_GenerateMipmapsForGPUTexture(cmdbuf, texture->tex);
    }
    texture->empty = 0;
    return SDL_SubmitGPUCommandBuffer(cmdbuf);
}

/*************************************************************************/
/**************************** Local routines *****************************/
/*************************************************************************/

static int create_display_target(int width, int height)
{
    SDL_GPUTextureCreateInfo info;
    mem_clear(&info, sizeof(info));
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = sdlgpu_color_format();
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
               | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    display_color = SDL_CreateGPUTexture(sdlgpu_device, &info);
    if (!display_color) {
        DLOG("Failed to create display texture: %s", SDL_GetError());
        return 0;
    }
    display_depth_format = sdlgpu_depth_format(requested_depth_bits,
                                               requested_stencil_bits);
    if (display_depth_format != SDL_GPU_TEXTUREFORMAT_INVALID) {
        info.format = display_depth_format;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        display_depth = SDL_CreateGPUTexture(sdlgpu_device, &info);
        if (!display_depth) {
            DLOG("Failed to create display depth buffer: %s", SDL_GetError());
            display_depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
        }
    }
    display_width = width;
    display_height = height;
    return 1;
}

/*-----------------------------------------------------------------------*/

static void destroy_display_target(void)
{
    sdlgpu_defer_release_texture(display_color);
    sdlgpu_defer_release_texture(display_depth);
    display_color = display_depth = NULL;
    display_depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    display_width = display_height = 0;
}

/*-----------------------------------------------------------------------*/

static void process_deferred_releases(void)
{
    for (int i = 0; i < num_release_buffers; i++) {
        SDL_ReleaseGPUBuffer(sdlgpu_device, release_buffers[i]);
    }
    num_release_buffers = 0;
    for (int i = 0; i < num_release_textures; i++) {
        SDL_ReleaseGPUTexture(sdlgpu_device, release_textures[i]);
    }
    num_release_textures = 0;
}

/*-----------------------------------------------------------------------*/

static void target_textures(SDL_GPUTexture **color_ret,
                            SDL_GPUTexture **depth_ret,
                            SDL_GPUTextureFormat *depth_format_ret)
{
    if (sdlgpu_current_framebuffer) {
        *color_ret = sdlgpu_current_framebuffer->texture.tex;
        *depth_ret = sdlgpu_current_framebuffer->depth;
        *depth_format_ret = sdlgpu_current_framebuffer->depth
            ? sdlgpu_current_framebuffer->depth_format
            : SDL_GPU_TEXTUREFORMAT_INVALID;
    } else {
        *color_ret = display_color;
        *depth_ret = display_depth;
        *depth_format_ret = display_depth_format;
    }
}

/*-----------------------------------------------------------------------*/

static Command *new_command(void)
{
    if (num_commands >= max_commands) {
        const int new_max = max_commands ? max_commands * 2 : 256;
        Command *new_list = mem_realloc(commands, sizeof(*new_list) * new_max,
                                        0);
        if (!new_list) {
            DLOG("No memory for command list");
            return NULL;
        }
        commands = new_list;
        max_commands = new_max;
    }
    Command *cmd = &commands[num_commands++];
    mem_clear(cmd, sizeof(*cmd));
    return cmd;
}

/*-----------------------------------------------------------------------*/

static int stream_append(const void *data, uint32_t size,
                         uint32_t *offset_ret)
{
    const uint32_t offset = (stream_size + 15) & ~15u;
    if (offset + size > stream_capacity) {
        uint32_t new_capacity = stream_capacity ? stream_capacity : 65536;
        while (new_capacity < offset + size) {
            new_capacity *= 2;
        }
        uint8_t *new_data = mem_realloc(stream_data, new_capacity, 0);
        if (!new_data) {
            DLOG("No memory for stream data (%u bytes)", new_capacity);
            return 0;
        }
        stream_data = new_data;
        stream_capacity = new_capacity;
    }
    memcpy(stream_data + offset, data, size);
    stream_size = offset + size;
    *offset_ret = offset;
    return 1;
}

/*-----------------------------------------------------------------------*/

static void fill_state_key(PipelineKey *key)
{
    key->blend_enable = blend_enabled
        || (blend_alpha_separate
            && !(blend_alpha_src == GRAPHICS_BLEND_ONE
                 && blend_alpha_dst == GRAPHICS_BLEND_ZERO));
    key->color_op = blend_op;
    key->src_color = blend_src;
    key->dst_color = blend_dst;
    key->alpha_op = blend_op;
    if (blend_alpha_separate) {
        key->src_alpha = blend_alpha_src;
        key->dst_alpha = blend_alpha_dst;
    } else {
        key->src_alpha = blend_src;
        key->dst_alpha = blend_dst;
    }
    key->write_mask = color_write_mask;

    key->depth_test = depth_test;
    key->depth_write = depth_test && depth_write;
    key->depth_compare = depth_compare;

    key->stencil_test = stencil_test;
    if (stencil_test) {
        /* SIL compares (stencil OP ref), the GPU compares (ref OP
         * stencil), so the relational comparisons are swapped. */
        uint8_t compare = stencil_compare;
        switch (compare) {
          case GRAPHICS_COMPARISON_LESS:
            compare = GRAPHICS_COMPARISON_GREATER; break;
          case GRAPHICS_COMPARISON_LESS_EQUAL:
            compare = GRAPHICS_COMPARISON_GREATER_EQUAL; break;
          case GRAPHICS_COMPARISON_GREATER_EQUAL:
            compare = GRAPHICS_COMPARISON_LESS_EQUAL; break;
          case GRAPHICS_COMPARISON_GREATER:
            compare = GRAPHICS_COMPARISON_LESS; break;
          default:
            break;
        }
        key->stencil_compare = compare;
        key->stencil_fail = stencil_sfail;
        key->stencil_dfail = stencil_dfail;
        key->stencil_pass = stencil_dpass;
        key->stencil_mask = (uint8_t)stencil_mask;
    }

    key->cull_mode = face_cull;
    key->front_face = face_cull ? face_cull_cw : 0;
    key->color_target_format = (uint8_t)sdlgpu_color_format();
}

/*-----------------------------------------------------------------------*/

static void fill_common_draw_state(Command *cmd)
{
    int target_w, target_h;
    sdlgpu_target_size(&target_w, &target_h);

    cmd->viewport.x = (float)viewport_x;
    cmd->viewport.y = (float)viewport_y;
    cmd->viewport.w = (float)viewport_w;
    cmd->viewport.h = (float)viewport_h;
    cmd->viewport.min_depth = depth_near;
    cmd->viewport.max_depth = depth_far;

    int sx = 0, sy = 0, sw = target_w, sh = target_h;
    if (clip_enabled) {
        sx = clip_x;
        sy = clip_y;
        sw = clip_w;
        sh = clip_h;
        /* Scissor rectangles must lie within the render target. */
        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sx + sw > target_w) { sw = target_w - sx; }
        if (sy + sh > target_h) { sh = target_h - sy; }
        if (sw < 0) { sw = 0; }
        if (sh < 0) { sh = 0; }
    }
    cmd->scissor.x = sx;
    cmd->scissor.y = sy;
    cmd->scissor.w = sw;
    cmd->scissor.h = sh;

    cmd->stencil_ref = (uint8_t)stencil_ref;
    cmd->blend_const.r = blend_color.x;
    cmd->blend_const.g = blend_color.y;
    cmd->blend_const.b = blend_color.z;
    cmd->blend_const.a = blend_color.w;

    Matrix4f modelview;
    mat4_mul(&modelview, &model_matrix, &view_matrix);
    mat4_mul(&cmd->vs.transform, &modelview, &projection_matrix);
    cmd->vs.fog_transform.x = modelview._13;
    cmd->vs.fog_transform.y = modelview._23;
    cmd->vs.fog_transform.z = modelview._33;
    cmd->vs.fog_transform.w = modelview._43;
    cmd->vs.tex_offset = tex_offset;
    cmd->vs.point_size = point_size;

    cmd->fs.fixed_color = fixed_color;
    cmd->fs.fog_color = fog_color;
    if (fog_end != fog_start) {
        cmd->fs.fog_params.x = 1.0f / (fog_end - fog_start);
        cmd->fs.fog_params.y = fog_start / (fog_end - fog_start);
    }
    cmd->fs.alpha_ref = alpha_ref;
    cmd->fs.fog = fog_enabled;
    cmd->fs.alpha_mode = ALPHATEST_OFF;
    if (alpha_test) {
        switch (alpha_comparison) {
          case GRAPHICS_COMPARISON_LESS:
            cmd->fs.alpha_mode = ALPHATEST_LESS; break;
          case GRAPHICS_COMPARISON_LESS_EQUAL:
            cmd->fs.alpha_mode = ALPHATEST_LESS_EQUAL; break;
          case GRAPHICS_COMPARISON_GREATER_EQUAL:
            cmd->fs.alpha_mode = ALPHATEST_GREATER_EQUAL; break;
          case GRAPHICS_COMPARISON_GREATER:
            cmd->fs.alpha_mode = ALPHATEST_GREATER; break;
          default:
            break;
        }
    }
}

/*-----------------------------------------------------------------------*/

static void safe_clear(const Vector4f *color, const float *depth,
                       unsigned int stencil)
{
    SDL_GPUTexture *color_target, *depth_target;
    SDL_GPUTextureFormat depth_format;
    target_textures(&color_target, &depth_target, &depth_format);
    if (!color_target) {
        return;
    }
    if (!depth_target) {
        depth = NULL;
    }
    if (!color && !depth) {
        return;
    }

    PipelineKey key;
    mem_clear(&key, sizeof(key));
    key.prim_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    key.pos_format = VFMT_FLOAT4;
    key.stride = 16;
    key.write_mask = color ? color_write_mask : 0;
    if (depth) {
        key.depth_test = 1;
        key.depth_write = 1;
        key.depth_compare = GRAPHICS_COMPARISON_TRUE;
        if (depth_format == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT
         || depth_format == SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT) {
            key.stencil_test = 1;
            key.stencil_compare = GRAPHICS_COMPARISON_TRUE;
            key.stencil_fail = GRAPHICS_STENCIL_REPLACE;
            key.stencil_dfail = GRAPHICS_STENCIL_REPLACE;
            key.stencil_pass = GRAPHICS_STENCIL_REPLACE;
            key.stencil_mask = 0xFF;
        }
    }
    key.color_target_format = (uint8_t)sdlgpu_color_format();
    key.depth_target_format = (uint8_t)depth_format;
    SDL_GPUGraphicsPipeline *pipeline = get_pipeline(&key);
    if (!pipeline) {
        return;
    }

    const float z = depth ? bound(*depth, 0.0f, 1.0f) * 2.0f - 1.0f : 0.0f;
    const float vertices[6][4] = {
        {-1, -1, z, 1}, {1, -1, z, 1}, {-1, 1, z, 1},
        {-1,  1, z, 1}, {1, -1, z, 1}, { 1, 1, z, 1},
    };
    uint32_t voffset;
    if (!stream_append(vertices, sizeof(vertices), &voffset)) {
        return;
    }
    Command *cmd = new_command();
    if (!cmd) {
        return;
    }
    cmd->type = CMD_DRAW;
    cmd->pipeline = pipeline;
    cmd->voffset = voffset;
    cmd->first = 0;
    cmd->count = 6;
    fill_common_draw_state(cmd);
    int target_w, target_h;
    sdlgpu_target_size(&target_w, &target_h);
    cmd->viewport.x = 0;
    cmd->viewport.y = 0;
    cmd->viewport.w = (float)target_w;
    cmd->viewport.h = (float)target_h;
    cmd->viewport.min_depth = 0;
    cmd->viewport.max_depth = 1;
    cmd->stencil_ref = (uint8_t)stencil;
    cmd->vs.transform = mat4_identity;
    if (color) {
        cmd->fs.fixed_color = *color;
    }
    cmd->fs.texmode = TEXCOLOR_NONE;
    cmd->fs.fog = 0;
    cmd->fs.alpha_mode = ALPHATEST_OFF;
    cmd->texture = sdlgpu_dummy_texture();
    cmd->sampler = sdlgpu_texture_sampler(NULL);
}

/*-----------------------------------------------------------------------*/

static uint32_t hash_key(const PipelineKey *key)
{
    /* FNV-1a */
    const uint8_t *p = (const uint8_t *)key;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < sizeof(*key); i++) {
        hash = (hash ^ p[i]) * 16777619u;
    }
    return hash;
}

static SDL_GPUVertexElementFormat convert_vertex_format(int format)
{
    switch (format) {
      case VFMT_FLOAT2:      return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
      case VFMT_FLOAT3:      return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
      case VFMT_FLOAT4:      return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
      case VFMT_UBYTE4_NORM: return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    }
    return SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
}

static SDL_GPUGraphicsPipeline *create_pipeline(const PipelineKey *key)
{
    SDL_GPUVertexAttribute attributes[3];
    int num_attributes = 0;
    mem_clear(attributes, sizeof(attributes));
    attributes[num_attributes].location = 0;
    attributes[num_attributes].buffer_slot = 0;
    attributes[num_attributes].format = convert_vertex_format(key->pos_format);
    attributes[num_attributes].offset = key->pos_offset;
    num_attributes++;
    if (key->has_texcoord) {
        attributes[num_attributes].location = 1;
        attributes[num_attributes].buffer_slot = 0;
        attributes[num_attributes].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attributes[num_attributes].offset = key->tex_offset;
        num_attributes++;
    }
    if (key->color_format) {
        attributes[num_attributes].location = 2;
        attributes[num_attributes].buffer_slot = 0;
        attributes[num_attributes].format =
            convert_vertex_format(key->color_format);
        attributes[num_attributes].offset = key->color_offset;
        num_attributes++;
    }
    SDL_GPUVertexBufferDescription vbuf_desc;
    mem_clear(&vbuf_desc, sizeof(vbuf_desc));
    vbuf_desc.slot = 0;
    vbuf_desc.pitch = key->stride;
    vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUColorTargetDescription color_desc;
    mem_clear(&color_desc, sizeof(color_desc));
    color_desc.format = (SDL_GPUTextureFormat)key->color_target_format;
    color_desc.blend_state.enable_blend = key->blend_enable;
    color_desc.blend_state.src_color_blendfactor =
        convert_blend_factor(key->src_color, 0);
    color_desc.blend_state.dst_color_blendfactor =
        convert_blend_factor(key->dst_color, 0);
    color_desc.blend_state.src_alpha_blendfactor =
        convert_blend_factor(key->src_alpha, 1);
    color_desc.blend_state.dst_alpha_blendfactor =
        convert_blend_factor(key->dst_alpha, 1);
    const SDL_GPUBlendOp op =
        key->color_op == GRAPHICS_BLEND_SUB ? SDL_GPU_BLENDOP_SUBTRACT :
        key->color_op == GRAPHICS_BLEND_RSUB ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT :
        SDL_GPU_BLENDOP_ADD;
    color_desc.blend_state.color_blend_op = op;
    color_desc.blend_state.alpha_blend_op = op;
    color_desc.blend_state.color_write_mask = key->write_mask;
    color_desc.blend_state.enable_color_write_mask = true;

    SDL_GPUGraphicsPipelineCreateInfo info;
    mem_clear(&info, sizeof(info));
    info.vertex_shader = sdlgpu_vertex_shader(key->has_texcoord,
                                              key->color_format != 0);
    info.fragment_shader = sdlgpu_fragment_shader();
    info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = num_attributes;
    info.primitive_type = (SDL_GPUPrimitiveType)key->prim_type;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    /* The vertex shader flips Y, so OpenGL's counterclockwise triangles
     * are clockwise in the GPU's window coordinates.  To cull OpenGL-CW
     * triangles, treat GPU-CW (= GL-CCW) as front and cull back faces. */
    info.rasterizer_state.cull_mode =
        key->cull_mode ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face =
        key->front_face ? SDL_GPU_FRONTFACE_CLOCKWISE
                        : SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip = true;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.enable_depth_test = key->depth_test;
    info.depth_stencil_state.enable_depth_write = key->depth_write;
    info.depth_stencil_state.compare_op = convert_comparison(key->depth_compare);
    info.depth_stencil_state.enable_stencil_test = key->stencil_test;
    if (key->stencil_test) {
        SDL_GPUStencilOpState state;
        state.fail_op = convert_stencil_op(key->stencil_fail);
        state.depth_fail_op = convert_stencil_op(key->stencil_dfail);
        state.pass_op = convert_stencil_op(key->stencil_pass);
        state.compare_op = convert_comparison(key->stencil_compare);
        info.depth_stencil_state.front_stencil_state = state;
        info.depth_stencil_state.back_stencil_state = state;
        info.depth_stencil_state.compare_mask = key->stencil_mask;
        info.depth_stencil_state.write_mask = key->stencil_mask;
    }
    info.target_info.color_target_descriptions = &color_desc;
    info.target_info.num_color_targets = 1;
    info.target_info.depth_stencil_format =
        (SDL_GPUTextureFormat)key->depth_target_format;
    info.target_info.has_depth_stencil_target =
        (key->depth_target_format != SDL_GPU_TEXTUREFORMAT_INVALID);

    SDL_GPUGraphicsPipeline *pipeline =
        SDL_CreateGPUGraphicsPipeline(sdlgpu_device, &info);
    if (!pipeline) {
        DLOG("SDL_CreateGPUGraphicsPipeline() failed: %s", SDL_GetError());
    }
    return pipeline;
}

/*-----------------------------------------------------------------------*/

static int insert_pipeline(PipelineEntry *table, int table_size,
                           const PipelineEntry *entry)
{
    const int mask = table_size - 1;
    for (int i = entry->hash & mask; ; i = (i + 1) & mask) {
        if (!table[i].pipeline) {
            table[i] = *entry;
            return i;
        }
    }
}

static SDL_GPUGraphicsPipeline *get_pipeline(const PipelineKey *key)
{
    const uint32_t hash = hash_key(key);
    const int mask = pipeline_table_size - 1;
    for (int i = hash & mask; pipelines[i].pipeline; i = (i + 1) & mask) {
        if (pipelines[i].hash == hash
         && memcmp(&pipelines[i].key, key, sizeof(*key)) == 0) {
            return pipelines[i].pipeline;
        }
    }

    SDL_GPUGraphicsPipeline *pipeline = create_pipeline(key);
    if (!pipeline) {
        return NULL;
    }

    if ((num_pipelines + 1) * 4 > pipeline_table_size * 3) {
        const int new_size = pipeline_table_size * 2;
        PipelineEntry *new_table = mem_alloc(sizeof(*new_table) * new_size,
                                             0, MEM_ALLOC_CLEAR);
        if (new_table) {
            for (int i = 0; i < pipeline_table_size; i++) {
                if (pipelines[i].pipeline) {
                    insert_pipeline(new_table, new_size, &pipelines[i]);
                }
            }
            mem_free(pipelines);
            pipelines = new_table;
            pipeline_table_size = new_size;
        } else if (num_pipelines + 1 >= pipeline_table_size) {
            SDL_ReleaseGPUGraphicsPipeline(sdlgpu_device, pipeline);
            return NULL;
        }
    }
    PipelineEntry entry;
    entry.key = *key;
    entry.hash = hash;
    entry.pipeline = pipeline;
    insert_pipeline(pipelines, pipeline_table_size, &entry);
    num_pipelines++;
    return pipeline;
}

static void clear_pipeline_cache(void)
{
    for (int i = 0; i < pipeline_table_size; i++) {
        if (pipelines[i].pipeline) {
            SDL_ReleaseGPUGraphicsPipeline(sdlgpu_device,
                                           pipelines[i].pipeline);
            pipelines[i].pipeline = NULL;
        }
    }
    num_pipelines = 0;
}

/*-----------------------------------------------------------------------*/

static SDL_GPUBlendFactor convert_blend_factor(int factor, int alpha)
{
    switch (factor) {
      case GRAPHICS_BLEND_ZERO:
        return SDL_GPU_BLENDFACTOR_ZERO;
      case GRAPHICS_BLEND_ONE:
        return SDL_GPU_BLENDFACTOR_ONE;
      case GRAPHICS_BLEND_SRC_COLOR:
        return alpha ? SDL_GPU_BLENDFACTOR_SRC_ALPHA
                     : SDL_GPU_BLENDFACTOR_SRC_COLOR;
      case GRAPHICS_BLEND_SRC_ALPHA:
        return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
      case GRAPHICS_BLEND_INV_SRC_ALPHA:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
      case GRAPHICS_BLEND_DEST_COLOR:
        return alpha ? SDL_GPU_BLENDFACTOR_DST_ALPHA
                     : SDL_GPU_BLENDFACTOR_DST_COLOR;
      case GRAPHICS_BLEND_DEST_ALPHA:
        return SDL_GPU_BLENDFACTOR_DST_ALPHA;
      case GRAPHICS_BLEND_INV_DEST_ALPHA:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
      case GRAPHICS_BLEND_CONSTANT:
        return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
      case GRAPHICS_BLEND_INV_CONSTANT:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    }
    return SDL_GPU_BLENDFACTOR_ONE;
}

static SDL_GPUCompareOp convert_comparison(int type)
{
    switch (type) {
      case GRAPHICS_COMPARISON_FALSE:         return SDL_GPU_COMPAREOP_NEVER;
      case GRAPHICS_COMPARISON_TRUE:          return SDL_GPU_COMPAREOP_ALWAYS;
      case GRAPHICS_COMPARISON_EQUAL:         return SDL_GPU_COMPAREOP_EQUAL;
      case GRAPHICS_COMPARISON_NOT_EQUAL:     return SDL_GPU_COMPAREOP_NOT_EQUAL;
      case GRAPHICS_COMPARISON_LESS:          return SDL_GPU_COMPAREOP_LESS;
      case GRAPHICS_COMPARISON_LESS_EQUAL:    return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
      case GRAPHICS_COMPARISON_GREATER_EQUAL: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
      case GRAPHICS_COMPARISON_GREATER:       return SDL_GPU_COMPAREOP_GREATER;
    }
    return SDL_GPU_COMPAREOP_ALWAYS;
}

static SDL_GPUStencilOp convert_stencil_op(int op)
{
    switch (op) {
      case GRAPHICS_STENCIL_KEEP:    return SDL_GPU_STENCILOP_KEEP;
      case GRAPHICS_STENCIL_CLEAR:   return SDL_GPU_STENCILOP_ZERO;
      case GRAPHICS_STENCIL_REPLACE: return SDL_GPU_STENCILOP_REPLACE;
      case GRAPHICS_STENCIL_INCR:    return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
      case GRAPHICS_STENCIL_DECR:    return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
      case GRAPHICS_STENCIL_INVERT:  return SDL_GPU_STENCILOP_INVERT;
    }
    return SDL_GPU_STENCILOP_KEEP;
}

/*************************************************************************/
/*************************************************************************/
