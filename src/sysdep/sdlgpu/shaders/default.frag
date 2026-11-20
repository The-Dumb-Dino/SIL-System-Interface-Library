/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdlgpu/shaders/default.frag: Default fragment shader (GLSL
 * for SPIR-V/Vulkan).  Must behave identically to the MSL source in
 * shader.c.
 *
 * SDL_GPU SPIR-V binding rules: fragment samplers are in set 2, fragment
 * uniform buffers in set 3.
 */

#version 450

layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec4 v_color;
layout(location = 2) in float v_fog;

layout(set = 2, binding = 0) uniform sampler2D tex;

layout(std140, set = 3, binding = 0) uniform FSUniforms {
    vec4 fixed_color;
    vec4 fog_color;
    vec2 fog_params;
    float alpha_ref;
    int texmode;
    int fog;
    int alpha_mode;
    int pad0, pad1;
} u;

layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 color = v_color * u.fixed_color;
    if (u.texmode != 0) {
        vec4 s = texture(tex, v_texcoord);
        if (u.texmode == 1) {          // RGBA
            color *= s;
        } else if (u.texmode == 2) {   // RGB
            color *= vec4(s.rgb, 1.0);
        } else if (u.texmode == 3) {   // A
            color.a *= s.r;
        } else {                       // L
            color.rgb *= s.r;
        }
    }
    if (u.alpha_mode == 1) {
        if (color.a >= u.alpha_ref) discard;
    } else if (u.alpha_mode == 2) {
        if (color.a > u.alpha_ref) discard;
    } else if (u.alpha_mode == 3) {
        if (color.a < u.alpha_ref) discard;
    } else if (u.alpha_mode == 4) {
        if (color.a <= u.alpha_ref) discard;
    }
    if (u.fog != 0) {
        float f = clamp(abs(v_fog) * u.fog_params.x - u.fog_params.y,
                        0.0, 1.0);
        color = mix(color, vec4(u.fog_color.rgb, color.a), f);
    }
    frag_color = color;
}
