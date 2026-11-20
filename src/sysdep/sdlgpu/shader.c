/*
 * System Interface Library for games
 * Copyright (c) 2007-2026 Andrew Church <achurch@achurch.org>
 * Released under the GNU GPL version 3 or later; NO WARRANTY is provided.
 * See the file COPYING.txt for details.
 *
 * src/sysdep/sdlgpu/shader.c: Shaders for the SDL_GPU rendering backend.
 *
 * The backend implements SIL's fixed-function rendering pipeline with a
 * small set of precompiled shaders: four vertex shader variants (with or
 * without texture coordinates, with or without vertex colors) and a single
 * fragment shader whose behavior (texture color type, fog, alpha test) is
 * controlled by uniforms.  Custom shaders (shader.h, shader generators)
 * are not supported by this backend.
 *
 * The shader format is chosen by what the GPU driver accepts
 * (SDL_GetGPUShaderFormats()):
 *   - MSL source (Metal), compiled by SDL at runtime;
 *   - SPIR-V (Vulkan), compiled offline from shaders/default.{vert,frag}
 *     by shaders/build-shaders.sh into shaders-spirv.i;
 *   - DXIL (Direct3D 12, Shader Model 6.0) and DXBC (Direct3D 12, Shader
 *     Model 5.1, for GPUs without SM 6.0), compiled offline from
 *     shaders/default.hlsl by shaders/build-shaders.sh into
 *     shaders-dxil.i and shaders-dxbc.i.
 */

#define IN_SYSDEP

#include "src/base.h"
#include "src/memory.h"
#include "src/sysdep.h"
#include "src/shader.h"
#include "src/sysdep/sdlgpu/internal.h"

/*************************************************************************/
/***************************** Shader source *****************************/
/*************************************************************************/

/* Must match VSUniforms / FSUniforms in internal.h. */
static const char msl_common[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VSUniforms {\n"
    "    float4x4 transform;\n"
    "    float4 fog_transform;\n"
    "    float2 tex_offset;\n"
    "    float point_size;\n"
    "    float pad;\n"
    "};\n"
    "struct FSUniforms {\n"
    "    float4 fixed_color;\n"
    "    float4 fog_color;\n"
    "    float2 fog_params;\n"
    "    float alpha_ref;\n"
    "    int texmode;\n"
    "    int fog;\n"
    "    int alpha_mode;\n"
    "    int pad0, pad1;\n"
    "};\n"
    "struct VSOut {\n"
    "    float4 position [[position]];\n"
    "    float2 texcoord [[user(texcoord)]];\n"
    "    float4 color [[user(color)]];\n"
    "    float fog [[user(fog)]];\n"
    "    float point_size [[point_size]];\n"
    "};\n"
    "struct FSIn {\n"
    "    float4 position [[position]];\n"
    "    float2 texcoord [[user(texcoord)]];\n"
    "    float4 color [[user(color)]];\n"
    "    float fog [[user(fog)]];\n"
    "};\n";

static const char msl_vertex[] =
    "struct VSIn {\n"
    "    float4 position [[attribute(0)]];\n"
    "#if HAS_TEX\n"
    "    float2 texcoord [[attribute(1)]];\n"
    "#endif\n"
    "#if HAS_COLOR\n"
    "    float4 color [[attribute(2)]];\n"
    "#endif\n"
    "};\n"
    "vertex VSOut vs_main(VSIn in [[stage_in]],\n"
    "                    constant VSUniforms &u [[buffer(0)]])\n"
    "{\n"
    "    VSOut out;\n"
    /* Missing position components are filled with (0,0,1) by the vertex
     * fetch, so float2/float3 positions arrive as (x,y,0,1)/(x,y,z,1). */
    "    float4 pos = u.transform * in.position;\n"
    "    out.fog = dot(u.fog_transform, in.position);\n"
    /* Flip Y so that pixel rows match OpenGL window Y, and map Z from
     * OpenGL's [-1,1] to [0,1]. */
    "    pos.y = -pos.y;\n"
    "    pos.z = (pos.z + pos.w) * 0.5;\n"
    "    out.position = pos;\n"
    "#if HAS_TEX\n"
    "    out.texcoord = in.texcoord + u.tex_offset;\n"
    "#else\n"
    "    out.texcoord = float2(0.0);\n"
    "#endif\n"
    "#if HAS_COLOR\n"
    "    out.color = in.color;\n"
    "#else\n"
    "    out.color = float4(1.0);\n"
    "#endif\n"
    "    out.point_size = u.point_size;\n"
    "    return out;\n"
    "}\n";

static const char msl_fragment[] =
    "fragment float4 fs_main(FSIn in [[stage_in]],\n"
    "                        constant FSUniforms &u [[buffer(0)]],\n"
    "                        texture2d<float> tex [[texture(0)]],\n"
    "                        sampler smp [[sampler(0)]])\n"
    "{\n"
    "    float4 color = in.color * u.fixed_color;\n"
    "    if (u.texmode != 0) {\n"
    "        float4 s = tex.sample(smp, in.texcoord);\n"
    "        if (u.texmode == 1) {\n"          // RGBA
    "            color *= s;\n"
    "        } else if (u.texmode == 2) {\n"   // RGB
    "            color *= float4(s.rgb, 1.0);\n"
    "        } else if (u.texmode == 3) {\n"   // A
    "            color.a *= s.r;\n"
    "        } else {\n"                       // L
    "            color.rgb *= s.r;\n"
    "        }\n"
    "    }\n"
    "    if (u.alpha_mode == 1) {\n"
    "        if (color.a >= u.alpha_ref) discard_fragment();\n"
    "    } else if (u.alpha_mode == 2) {\n"
    "        if (color.a > u.alpha_ref) discard_fragment();\n"
    "    } else if (u.alpha_mode == 3) {\n"
    "        if (color.a < u.alpha_ref) discard_fragment();\n"
    "    } else if (u.alpha_mode == 4) {\n"
    "        if (color.a <= u.alpha_ref) discard_fragment();\n"
    "    }\n"
    "    if (u.fog != 0) {\n"
    "        float f = clamp(abs(in.fog) * u.fog_params.x - u.fog_params.y,\n"
    "                        0.0, 1.0);\n"
    "        color = mix(color, float4(u.fog_color.rgb, color.a), f);\n"
    "    }\n"
    "    return color;\n"
    "}\n";

#include "src/sysdep/sdlgpu/shaders-spirv.i"
#include "src/sysdep/sdlgpu/shaders-dxil.i"
#include "src/sysdep/sdlgpu/shaders-dxbc.i"

/*************************************************************************/
/****************************** Local data *******************************/
/*************************************************************************/

static SDL_GPUShader *vertex_shaders[2][2];  // [has_tex][has_color]
static SDL_GPUShader *fragment_shader;
static const char *shader_language = "none";

/*************************************************************************/
/********************** Internal interface routines **********************/
/*************************************************************************/

static SDL_GPUShader *create_msl_shader(SDL_GPUShaderStage stage,
                                        const char *defines,
                                        const char *body)
{
    const size_t len = strlen(defines) + strlen(msl_common) + strlen(body);
    char *source = mem_alloc(len + 1, 0, MEM_ALLOC_TEMP);
    if (!source) {
        return NULL;
    }
    strcpy(source, defines);
    strcat(source, msl_common);
    strcat(source, body);

    SDL_GPUShaderCreateInfo info;
    mem_clear(&info, sizeof(info));
    info.code = (const Uint8 *)source;
    info.code_size = len;
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.stage = stage;
    if (stage == SDL_GPU_SHADERSTAGE_VERTEX) {
        info.entrypoint = "vs_main";
        info.num_uniform_buffers = 1;
    } else {
        info.entrypoint = "fs_main";
        info.num_samplers = 1;
        info.num_uniform_buffers = 1;
    }
    SDL_GPUShader *shader = SDL_CreateGPUShader(sdlgpu_device, &info);
    if (!shader) {
        DLOG("Failed to create %s shader: %s\nSource:\n%s",
             stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vertex" : "fragment",
             SDL_GetError(), source);
    }
    mem_free(source);
    return shader;
}

/*-----------------------------------------------------------------------*/

/* Precompiled shader sets, in order of preference.  DXIL is preferred
 * over DXBC because the Direct3D 12 driver accepts DXIL only if the GPU
 * supports Shader Model 6.0; DXBC (Shader Model 5.1) is the fallback for
 * older GPUs and virtual machine drivers. */
typedef struct BinaryShader {const uint8_t *code; size_t size;} BinaryShader;
typedef struct BinaryShaderSet {
    SDL_GPUShaderFormat format;
    const char *name;
    BinaryShader vertex[2][2];  // [has_tex][has_color]
    BinaryShader fragment;
} BinaryShaderSet;
#define BINARY_SHADER_SET(format, name, prefix) \
    {format, name, \
     {{{prefix##_vertex_t0_c0, sizeof(prefix##_vertex_t0_c0)}, \
       {prefix##_vertex_t0_c1, sizeof(prefix##_vertex_t0_c1)}}, \
      {{prefix##_vertex_t1_c0, sizeof(prefix##_vertex_t1_c0)}, \
       {prefix##_vertex_t1_c1, sizeof(prefix##_vertex_t1_c1)}}}, \
     {prefix##_fragment, sizeof(prefix##_fragment)}}
static const BinaryShaderSet binary_shader_sets[] = {
    BINARY_SHADER_SET(SDL_GPU_SHADERFORMAT_DXIL, "DXIL", dxil),
    BINARY_SHADER_SET(SDL_GPU_SHADERFORMAT_DXBC, "DXBC", dxbc),
    BINARY_SHADER_SET(SDL_GPU_SHADERFORMAT_SPIRV, "SPIR-V", spirv),
};
#undef BINARY_SHADER_SET

/*-----------------------------------------------------------------------*/

static SDL_GPUShader *create_binary_shader(const BinaryShaderSet *set,
                                           SDL_GPUShaderStage stage,
                                           const BinaryShader *code)
{
    SDL_GPUShaderCreateInfo info;
    mem_clear(&info, sizeof(info));
    info.code = code->code;
    info.code_size = code->size;
    info.entrypoint = "main";
    info.format = set->format;
    info.stage = stage;
    info.num_uniform_buffers = 1;
    info.num_samplers = (stage == SDL_GPU_SHADERSTAGE_FRAGMENT) ? 1 : 0;
    SDL_GPUShader *shader = SDL_CreateGPUShader(sdlgpu_device, &info);
    if (!shader) {
        DLOG("Failed to create %s %s shader: %s", set->name,
             stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vertex" : "fragment",
             SDL_GetError());
    }
    return shader;
}

/*-----------------------------------------------------------------------*/

int sdlgpu_shader_init(void)
{
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(sdlgpu_device);

#ifdef DEBUG
    /* For testing, the SIL_SDLGPU_SHADER_FORMAT hint (or environment
     * variable) can select one shader format ("DXIL", "DXBC", "SPIR-V" or
     * "MSL"), e.g. to test the DXBC shaders on a GPU which also supports
     * DXIL. */
    const char *forced_format = SDL_GetHint("SIL_SDLGPU_SHADER_FORMAT");
    if (forced_format && *forced_format) {
        SDL_GPUShaderFormat mask = 0;
        if (SDL_strcasecmp(forced_format, "MSL") == 0) {
            mask = SDL_GPU_SHADERFORMAT_MSL;
        }
        for (int i = 0; i < lenof(binary_shader_sets); i++) {
            if (SDL_strcasecmp(forced_format,
                               binary_shader_sets[i].name) == 0) {
                mask = binary_shader_sets[i].format;
            }
        }
        DLOG("Restricting shader format to %s (driver accepts 0x%X)",
             forced_format, formats);
        formats &= mask;
    }
#endif

    for (int i = 0; i < lenof(binary_shader_sets); i++) {
        const BinaryShaderSet *set = &binary_shader_sets[i];
        if (!(formats & set->format)) {
            continue;
        }
        shader_language = set->name;
        for (int tex = 0; tex < 2; tex++) {
            for (int color = 0; color < 2; color++) {
                vertex_shaders[tex][color] = create_binary_shader(
                    set, SDL_GPU_SHADERSTAGE_VERTEX, &set->vertex[tex][color]);
                if (!vertex_shaders[tex][color]) {
                    sdlgpu_shader_cleanup();
                    return 0;
                }
            }
        }
        fragment_shader = create_binary_shader(
            set, SDL_GPU_SHADERSTAGE_FRAGMENT, &set->fragment);
        if (!fragment_shader) {
            sdlgpu_shader_cleanup();
            return 0;
        }
        return 1;
    }

    if (!(formats & SDL_GPU_SHADERFORMAT_MSL)) {
        DLOG("No supported shader format (driver accepts 0x%X; MSL, SPIR-V,"
             " DXIL and DXBC are implemented)", formats);
        return 0;
    }
    shader_language = "MSL";
    for (int tex = 0; tex < 2; tex++) {
        for (int color = 0; color < 2; color++) {
            char defines[64];
            SDL_snprintf(defines, sizeof(defines),
                         "#define HAS_TEX %d\n#define HAS_COLOR %d\n",
                         tex, color);
            vertex_shaders[tex][color] = create_msl_shader(
                SDL_GPU_SHADERSTAGE_VERTEX, defines, msl_vertex);
            if (!vertex_shaders[tex][color]) {
                sdlgpu_shader_cleanup();
                return 0;
            }
        }
    }
    fragment_shader = create_msl_shader(SDL_GPU_SHADERSTAGE_FRAGMENT, "",
                                        msl_fragment);
    if (!fragment_shader) {
        sdlgpu_shader_cleanup();
        return 0;
    }
    return 1;
}

/*-----------------------------------------------------------------------*/

void sdlgpu_shader_cleanup(void)
{
    for (int tex = 0; tex < 2; tex++) {
        for (int color = 0; color < 2; color++) {
            if (vertex_shaders[tex][color]) {
                SDL_ReleaseGPUShader(sdlgpu_device,
                                     vertex_shaders[tex][color]);
                vertex_shaders[tex][color] = NULL;
            }
        }
    }
    if (fragment_shader) {
        SDL_ReleaseGPUShader(sdlgpu_device, fragment_shader);
        fragment_shader = NULL;
    }
}

/*-----------------------------------------------------------------------*/

SDL_GPUShader *sdlgpu_vertex_shader(int has_texcoord, int has_color)
{
    return vertex_shaders[has_texcoord != 0][has_color != 0];
}

SDL_GPUShader *sdlgpu_fragment_shader(void)
{
    return fragment_shader;
}

const char *sdlgpu_shader_language(void)
{
    return shader_language;
}

/*************************************************************************/
/******** sysdep interface: custom shaders (not supported) ***************/
/*************************************************************************/

int sys_graphics_set_shader_generator(
    void *vertex_source_callback, UNUSED void *fragment_source_callback,
    UNUSED void *key_callback, UNUSED int hash_table_size,
    UNUSED int dynamic_resize)
{
    /* Resetting to the default generator is fine; anything else is not. */
    return vertex_source_callback == NULL;
}

int sys_graphics_add_shader_uniform(UNUSED const char *name)
{
    return 0;
}

int sys_graphics_add_shader_attribute(UNUSED const char *name, UNUSED int size)
{
    return 0;
}

void sys_graphics_set_shader_uniform_int(UNUSED int uniform, UNUSED int value) {}
void sys_graphics_set_shader_uniform_float(UNUSED int uniform, UNUSED float value) {}
void sys_graphics_set_shader_uniform_vec2(UNUSED int uniform, UNUSED const Vector2f *value) {}
void sys_graphics_set_shader_uniform_vec3(UNUSED int uniform, UNUSED const Vector3f *value) {}
void sys_graphics_set_shader_uniform_vec4(UNUSED int uniform, UNUSED const Vector4f *value) {}
void sys_graphics_set_shader_uniform_mat4(UNUSED int uniform, UNUSED const Matrix4f *value) {}

int sys_graphics_enable_shader_objects(void)
{
    return 0;
}

int sys_graphics_disable_shader_objects(void)
{
    return 1;
}

int sys_shader_background_compilation_supported(void)
{
    return 0;
}

void sys_shader_enable_get_binary(UNUSED int enable) {}

int sys_shader_max_attributes(void)
{
    return 16;
}

int sys_shader_set_attribute(UNUSED int index, UNUSED const char *name)
{
    return 0;
}

void sys_shader_bind_standard_attribute(UNUSED enum ShaderAttribute attribute,
                                        UNUSED int index) {}

void sys_shader_clear_attributes(void) {}

SysShader *sys_shader_create(UNUSED enum ShaderType type,
                             UNUSED const void *data, UNUSED int size,
                             UNUSED int is_binary)
{
    DLOG("Custom shaders are not supported by the SDL_GPU backend");
    return NULL;
}

void sys_shader_destroy(UNUSED SysShader *shader) {}

void *sys_shader_get_binary(UNUSED SysShader *shader, UNUSED int *size_ret)
{
    return NULL;
}

void *sys_shader_compile(UNUSED enum ShaderType type, UNUSED const char *source,
                         UNUSED int length, UNUSED int *size_ret)
{
    return NULL;
}

int sys_shader_get_uniform_id(UNUSED SysShader *shader, UNUSED const char *name)
{
    return 0;
}

void sys_shader_set_uniform_int(UNUSED SysShader *shader, UNUSED int uniform, UNUSED int value) {}
void sys_shader_set_uniform_float(UNUSED SysShader *shader, UNUSED int uniform, UNUSED float value) {}
void sys_shader_set_uniform_vec2(UNUSED SysShader *shader, UNUSED int uniform, UNUSED const Vector2f *value) {}
void sys_shader_set_uniform_vec3(UNUSED SysShader *shader, UNUSED int uniform, UNUSED const Vector3f *value) {}
void sys_shader_set_uniform_vec4(UNUSED SysShader *shader, UNUSED int uniform, UNUSED const Vector4f *value) {}
void sys_shader_set_uniform_mat4(UNUSED SysShader *shader, UNUSED int uniform, UNUSED const Matrix4f *value) {}

SysShaderPipeline *sys_shader_pipeline_create(UNUSED SysShader *vertex_shader,
                                              UNUSED SysShader *fragment_shader_)
{
    return NULL;
}

void sys_shader_pipeline_destroy(UNUSED SysShaderPipeline *pipeline) {}

void sys_shader_pipeline_apply(UNUSED SysShaderPipeline *pipeline) {}

/*************************************************************************/
/*************************************************************************/
