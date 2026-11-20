/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdlgpu/shaders/default.vert: Default vertex shader (GLSL for
 * SPIR-V/Vulkan).  Must behave identically to the MSL source in shader.c.
 * Compiled by build-shaders.sh in four variants (HAS_TEX x HAS_COLOR).
 *
 * SDL_GPU SPIR-V binding rules: vertex uniform buffers are in set 1.
 */

#version 450

layout(location = 0) in vec4 in_position;
#if HAS_TEX
layout(location = 1) in vec2 in_texcoord;
#endif
#if HAS_COLOR
layout(location = 2) in vec4 in_color;
#endif

layout(std140, set = 1, binding = 0) uniform VSUniforms {
    mat4 transform;
    vec4 fog_transform;
    vec2 tex_offset;
    float point_size;
    float pad;
} u;

layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec4 v_color;
layout(location = 2) out float v_fog;

void main()
{
    /* Missing position components are filled with (0,0,1) by the vertex
     * fetch, so 2- and 3-component positions arrive as (x,y,0,1) and
     * (x,y,z,1). */
    vec4 pos = u.transform * in_position;
    v_fog = dot(u.fog_transform, in_position);
    /* Flip Y so that pixel rows match OpenGL window Y, and map Z from
     * OpenGL's [-1,1] to [0,1]. */
    pos.y = -pos.y;
    pos.z = (pos.z + pos.w) * 0.5;
    gl_Position = pos;
#if HAS_TEX
    v_texcoord = in_texcoord + u.tex_offset;
#else
    v_texcoord = vec2(0.0);
#endif
#if HAS_COLOR
    v_color = in_color;
#else
    v_color = vec4(1.0);
#endif
    gl_PointSize = u.point_size;
}
