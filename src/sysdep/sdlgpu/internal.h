/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdlgpu/internal.h: Internal declarations for the SDL_GPU
 * rendering backend.
 *
 * Overview of the design (see also the comments in base.c):
 *
 * - SIL's coordinate conventions are those of OpenGL.  The vertex shader
 *   negates clip-space Y and remaps Z from [-1,1] to [0,1], so that in
 *   every render target, pixel row R corresponds to OpenGL window Y = R.
 *   Viewports, scissor rectangles, read_pixels() and texture grabs can
 *   then use SIL's coordinates unchanged, and framebuffer textures are
 *   sampled exactly as under OpenGL.  The display is rendered into an
 *   offscreen texture and blitted (flipped vertically) to the swapchain.
 *
 * - SDL_GPU only allows data uploads in copy passes outside render passes,
 *   but SIL issues draw calls in immediate mode.  Draw calls are therefore
 *   recorded into a command list together with a snapshot of all render
 *   state, and vertex data for immediate primitives is collected in a CPU
 *   arena.  When the batch is flushed (render target change, end of frame,
 *   readback, ...), the arena is uploaded in one copy pass and the command
 *   list is replayed in a render pass.
 */

#ifndef SIL_SRC_SYSDEP_SDLGPU_INTERNAL_H
#define SIL_SRC_SYSDEP_SDLGPU_INTERNAL_H

#include <SDL3/SDL.h>

#ifndef SIL_SRC_MATH_H
# include "src/math.h"
#endif

/*************************************************************************/
/******************************* Constants *******************************/
/*************************************************************************/

/* Texture color types (how the fragment shader interprets samples). */
enum {
    TEXCOLOR_NONE = 0,  // No texture (or no texture coordinates).
    TEXCOLOR_RGBA,
    TEXCOLOR_RGB,       // Alpha forced to 1.
    TEXCOLOR_A,         // R8 texture holding alpha; color is white.
    TEXCOLOR_L,         // R8 texture holding luminance; alpha is 1.
};

/* Vertex attribute formats recognized by the backend. */
enum {
    VFMT_NONE = 0,
    VFMT_FLOAT2,
    VFMT_FLOAT3,
    VFMT_FLOAT4,
    VFMT_UBYTE4_NORM,
};

/* Alpha test modes (fragment shader uniform). */
enum {
    ALPHATEST_OFF = 0,
    ALPHATEST_LESS,
    ALPHATEST_LESS_EQUAL,
    ALPHATEST_GREATER_EQUAL,
    ALPHATEST_GREATER,
};

/*************************************************************************/
/************************** Object definitions ***************************/
/*************************************************************************/

struct SysTexture {
    SDL_GPUTexture *tex;
    int width, height;
    SDL_GPUTextureFormat format;
    uint8_t color_type;     // TEXCOLOR_*
    uint8_t is_framebuffer; // Owned by a SysFramebuffer?
    uint8_t auto_mipmaps;   // Generate mipmaps after each upload?
    uint8_t num_levels;
    uint8_t repeat_u, repeat_v;
    uint8_t antialias;
    uint8_t empty;          // Never written (lock returns zeroes).
    uint32_t batch_serial;  // Batch in which this texture was last used.
    void *lock_buf;         // Buffer returned from sys_texture_lock().
    int lock_x, lock_y, lock_w, lock_h;
};

struct SysFramebuffer {
    struct SysTexture texture;   // Color buffer (also the sampled texture).
    SDL_GPUTexture *depth;       // Depth/stencil buffer, or NULL.
    SDL_GPUTextureFormat depth_format;
    int width, height;
};

struct SysPrimitive {
    SDL_GPUPrimitiveType type;
    uint8_t converted_quads;    // 1 = QUADS→TRIANGLES, 2 = QUAD_STRIP.
    uint8_t immediate;          // Data is uploaded at draw time.
    uint8_t index32;            // Index buffer uses 32-bit indices.
    uint8_t pos_format, color_format, has_texcoord;
    uint16_t stride, pos_offset, tex_offset, color_offset;
    int render_count;           // Vertices (or indices if indexed).
    int vertex_count;
    /* Persistent (non-immediate) primitives: GPU buffers. */
    SDL_GPUBuffer *vbuf, *ibuf;
    /* Immediate primitives: CPU data (owned if *_owned is set). */
    const void *vdata, *idata;
    uint32_t vdata_size, idata_size;
    uint8_t vdata_owned, idata_owned;
};

/* Uniform blocks (must match the shader source in shader.c). */
typedef struct VSUniforms {
    Matrix4f transform;
    Vector4f fog_transform;
    Vector2f tex_offset;
    float point_size;
    float pad;
} VSUniforms;

typedef struct FSUniforms {
    Vector4f fixed_color;
    Vector4f fog_color;
    Vector2f fog_params;
    float alpha_ref;
    int32_t texmode;
    int32_t fog;
    int32_t alpha_mode;
    int32_t pad[2];
} FSUniforms;

/* Key identifying a graphics pipeline.  Must be fully zeroed before
 * filling in so that it can be hashed and compared with memcmp(). */
typedef struct PipelineKey {
    uint8_t prim_type;
    uint8_t pos_format, color_format, has_texcoord;
    uint16_t stride, pos_offset, tex_offset, color_offset;
    uint8_t blend_enable, src_color, dst_color, color_op;
    uint8_t src_alpha, dst_alpha, alpha_op, write_mask;
    uint8_t depth_test, depth_write, depth_compare;
    uint8_t stencil_test, stencil_compare, stencil_fail, stencil_dfail,
            stencil_pass, stencil_mask;
    uint8_t cull_mode, front_face;
    uint8_t color_target_format, depth_target_format;
    uint8_t pad[3];
} PipelineKey;

/*************************************************************************/
/***************************** Shared data *******************************/
/*************************************************************************/

extern SDL_GPUDevice *sdlgpu_device;

/* Serial number of the batch currently being recorded; incremented on
 * every flush.  Used to detect textures referenced by pending draws. */
extern uint32_t sdlgpu_batch_serial;

/* Currently bound framebuffer (NULL = display). */
extern struct SysFramebuffer *sdlgpu_current_framebuffer;

/* Currently applied texture (unit 0), or NULL. */
extern struct SysTexture *sdlgpu_current_texture;

/*************************************************************************/
/************************** Internal functions ***************************/
/*************************************************************************/

/* base.c */

/* Submit all recorded rendering for the current render target. */
extern void sdlgpu_flush(void);
/* Switch the render target (flushes pending rendering). */
extern void sdlgpu_set_render_target(struct SysFramebuffer *framebuffer);
/* Size of the current render target. */
extern void sdlgpu_target_size(int *width_ret, int *height_ret);
/* Queue a GPU object for release after the next flush. */
extern void sdlgpu_defer_release_buffer(SDL_GPUBuffer *buffer);
extern void sdlgpu_defer_release_texture(SDL_GPUTexture *texture);
/* Record a draw of the given primitive. */
extern void sdlgpu_record_draw(struct SysPrimitive *primitive,
                               int first, int count);
/* Return whether the driver ignores the vertex shader's point size
 * (Direct3D 12), so that points larger than one pixel must be drawn as
 * quads.  POINTS primitives then keep a CPU copy of their data. */
extern int sdlgpu_emulate_point_size(void);
/* Read pixels from a GPU texture into an RGBA buffer (blocking). */
extern int sdlgpu_read_texture(SDL_GPUTexture *texture,
                               SDL_GPUTextureFormat format,
                               int x, int y, int w, int h, void *buffer);
/* Upload pixel data (in the texture's own format) to a texture region,
 * using its own command buffer.  Flushes first if the texture is used
 * by pending draws. */
extern int sdlgpu_upload_texture(struct SysTexture *texture, int level,
                                 int x, int y, int w, int h,
                                 const void *data, int pixels_per_row);
/* Upload data to a GPU buffer using its own command buffer. */
extern int sdlgpu_upload_buffer(SDL_GPUBuffer *buffer, const void *data,
                                uint32_t size);
/* Copy a region of the current render target into a texture. */
extern int sdlgpu_copy_from_target(struct SysTexture *texture,
                                   int src_x, int src_y, int dest_x,
                                   int dest_y, int w, int h);
/* Color format used for all render targets. */
extern SDL_GPUTextureFormat sdlgpu_color_format(void);
/* Pick a depth/stencil format, or INVALID if none is requested. */
extern SDL_GPUTextureFormat sdlgpu_depth_format(int depth_bits,
                                                int stencil_bits);

/* shader.c */

/* Create the default shaders.  Returns false on error. */
extern int sdlgpu_shader_init(void);
extern void sdlgpu_shader_cleanup(void);
/* Return the vertex/fragment shader for the given attribute set. */
extern SDL_GPUShader *sdlgpu_vertex_shader(int has_texcoord, int has_color);
extern SDL_GPUShader *sdlgpu_fragment_shader(void);
/* Name of the shader language in use ("MSL", ...). */
extern const char *sdlgpu_shader_language(void);

/* texture.c */

extern void sdlgpu_texture_init(void);
extern void sdlgpu_texture_cleanup(void);
/* Return a sampler for the given texture's settings. */
extern SDL_GPUSampler *sdlgpu_texture_sampler(const struct SysTexture *texture);
/* 1x1 white texture bound when no texture is applied. */
extern SDL_GPUTexture *sdlgpu_dummy_texture(void);

/* primitive.c */

extern void sdlgpu_primitive_cleanup(void);

/*************************************************************************/
/*************************************************************************/

#endif  // SIL_SRC_SYSDEP_SDLGPU_INTERNAL_H
