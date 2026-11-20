/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdlgpu/shaders/default.hlsl: Default vertex and fragment
 * shaders for Direct3D 12 (HLSL compiled to DXIL).  Must behave
 * identically to the MSL source in shader.c and to default.vert /
 * default.frag.
 *
 * Compiled by build-shaders.sh: four vertex variants (HAS_TEX x HAS_COLOR)
 * to vs_6_0 and one fragment shader to ps_6_0.
 *
 * SDL_GPU DXIL binding rules: vertex uniform buffers are in space1,
 * fragment textures/samplers in space2, fragment uniform buffers in
 * space3.  Vertex semantics are TEXCOORD<n>, with n matching the vertex
 * attribute location (0 = position, 1 = texcoord, 2 = color); the same
 * applies to the values passed from the vertex to the fragment shader
 * (0 = texcoord, 1 = color, 2 = fog).
 *
 * Ask for a DXIL compile target with -DVERTEX_SHADER (vertex shader) or
 * -DFRAGMENT_SHADER (fragment shader); build-shaders.sh does this.
 */

/* The two shaders share the uniform blocks below, but they must be
 * compiled separately, so this file uses the stage macros instead of
 * having one entry point per stage. */

#if defined(VERTEX_SHADER)

struct VSInput {
    float4 position : TEXCOORD0;
#if HAS_TEX
    float2 texcoord : TEXCOORD1;
#endif
#if HAS_COLOR
    float4 color : TEXCOORD2;
#endif
};

struct VSOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float4 color : TEXCOORD1;
    float fog : TEXCOORD2;
};

cbuffer VSUniforms : register(b0, space1) {
    float4x4 transform;
    float4 fog_transform;
    float2 tex_offset;
    float point_size;
    float pad;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    /* Missing position components are filled with (0,0,1) by the vertex
     * fetch, so 2- and 3-component positions arrive as (x,y,0,1) and
     * (x,y,z,1).  The matrix is stored in column-major order in the
     * constant buffer, like the GLSL mat4 in an std140 block, so
     * mul(transform, position) is the same as "transform * position". */
    float4 pos = mul(transform, input.position);
    output.fog = dot(fog_transform, input.position);
    /* Flip Y so that pixel rows match OpenGL window Y, and map Z from
     * OpenGL's [-1,1] to [0,1]. */
    pos.y = -pos.y;
    pos.z = (pos.z + pos.w) * 0.5;
    output.position = pos;
#if HAS_TEX
    output.texcoord = input.texcoord + tex_offset;
#else
    output.texcoord = float2(0.0, 0.0);
#endif
#if HAS_COLOR
    output.color = input.color;
#else
    output.color = float4(1.0, 1.0, 1.0, 1.0);
#endif
    /* Direct3D has no per-vertex point size; point_size is unused. */
    return output;
}

#elif defined(FRAGMENT_SHADER)

struct PSInput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float4 color : TEXCOORD1;
    float fog : TEXCOORD2;
};

Texture2D tex : register(t0, space2);
SamplerState smp : register(s0, space2);

cbuffer FSUniforms : register(b0, space3) {
    float4 fixed_color;
    float4 fog_color;
    float2 fog_params;
    float alpha_ref;
    int texmode;
    int fog;
    int alpha_mode;
    int pad0;
    int pad1;
};

float4 main(PSInput input) : SV_Target
{
    float4 color = input.color * fixed_color;
    if (texmode != 0) {
        float4 s = tex.Sample(smp, input.texcoord);
        if (texmode == 1) {         // RGBA
            color *= s;
        } else if (texmode == 2) {  // RGB
            color *= float4(s.rgb, 1.0);
        } else if (texmode == 3) {  // A
            color.a *= s.r;
        } else {                    // L
            color.rgb *= s.r;
        }
    }
    if (alpha_mode == 1) {
        if (color.a >= alpha_ref) discard;
    } else if (alpha_mode == 2) {
        if (color.a > alpha_ref) discard;
    } else if (alpha_mode == 3) {
        if (color.a < alpha_ref) discard;
    } else if (alpha_mode == 4) {
        if (color.a <= alpha_ref) discard;
    }
    if (fog != 0) {
        float f = clamp(abs(input.fog) * fog_params.x - fog_params.y,
                        0.0, 1.0);
        color = float4(lerp(color.rgb, fog_color.rgb, f), color.a);
    }
    return color;
}

#endif
