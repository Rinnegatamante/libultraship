#include "core/Window.h"
#ifdef ENABLE_OPENGL

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include <map>
#include <unordered_map>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include "libultraship/libultra/gbi.h"

#ifdef __MINGW32__
#define FOR_WINDOWS 1
#else
#define FOR_WINDOWS 0
#endif

#ifdef _MSC_VER
#include <SDL2/SDL.h>
// #define GL_GLEXT_PROTOTYPES 1
#include <GL/glew.h>
#elif FOR_WINDOWS
#include <GL/glew.h>
#include "SDL.h"
#define GL_GLEXT_PROTOTYPES 1
#include "SDL_opengl.h"
#elif __APPLE__
#include <SDL2/SDL.h>
#include <GL/glew.h>
#elif __SWITCH__
#include <SDL2/SDL.h>
#include <glad/glad.h>
#else
#include <SDL2/SDL.h>
#ifdef __vita__
#include <vitaGL.h>
#else
#include <GL/glew.h>
#endif
#define GL_GLEXT_PROTOTYPES 1
// #include <SDL2/SDL_opengles2.h>
#endif

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "menu/ImGuiImpl.h"
#include "core/Window.h"
#include "gfx_pc.h"
#include <core/bridge/consolevariablebridge.h>

using namespace std;

struct ShaderProgram {
    GLuint opengl_program_id;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    GLint attrib_locations[16];
    uint8_t attrib_sizes[16];
    uint8_t num_attribs;
    GLint frame_count_location;
    GLint noise_scale_location;
#ifdef __vita__
    GLint uTexSize[2];
#endif
};

struct Framebuffer {
    uint32_t width, height;
    bool has_depth_buffer;
    uint32_t msaa_level;
    bool invert_y;

    GLuint fbo, clrbuf, clrbuf_msaa, rbo;
};

static map<pair<uint64_t, uint32_t>, struct ShaderProgram> shader_program_pool;
static GLuint opengl_vbo;
#ifdef __APPLE__
static GLuint opengl_vao;
#endif
static bool current_depth_mask;

static uint32_t frame_count;

static vector<Framebuffer> framebuffers;
static size_t current_framebuffer;
static float current_noise_scale;
#ifdef __vita__
static FilteringMode current_filter_mode = FILTER_LINEAR;
#else
static FilteringMode current_filter_mode = FILTER_THREE_POINT;
#endif

GLuint pixel_depth_rb, pixel_depth_fb;
size_t pixel_depth_rb_size;

static int gfx_opengl_get_max_texture_size() {
    GLint max_texture_size;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    return max_texture_size;
}

static const char* gfx_opengl_get_name() {
    return "OpenGL";
}

static struct GfxClipParameters gfx_opengl_get_clip_parameters(void) {
    return { false, framebuffers[current_framebuffer].invert_y };
}

static void gfx_opengl_vertex_array_set_attribs(struct ShaderProgram* prg) {
    size_t num_floats = prg->num_floats;
    size_t pos = 0;

    for (int i = 0; i < prg->num_attribs; i++) {
        glEnableVertexAttribArray(prg->attrib_locations[i]);
        glVertexAttribPointer(prg->attrib_locations[i], prg->attrib_sizes[i], GL_FLOAT, GL_FALSE,
                              num_floats * sizeof(float), (void*)(pos * sizeof(float)));
        pos += prg->attrib_sizes[i];
    }
}

static void gfx_opengl_set_uniforms(struct ShaderProgram* prg) {
    glUniform1i(prg->frame_count_location, frame_count);
    glUniform1f(prg->noise_scale_location, current_noise_scale);
}

static void gfx_opengl_unload_shader(struct ShaderProgram* old_prg) {
    if (old_prg != NULL) {
        for (int i = 0; i < old_prg->num_attribs; i++) {
            glDisableVertexAttribArray(old_prg->attrib_locations[i]);
        }
    }
}

static void gfx_opengl_load_shader(struct ShaderProgram* new_prg) {
    // if (!new_prg) return;
    glUseProgram(new_prg->opengl_program_id);
    gfx_opengl_vertex_array_set_attribs(new_prg);
    gfx_opengl_set_uniforms(new_prg);
#ifdef __vita__
    cur_gl_program = new_prg;
#endif
}

static void append_str(char* buf, size_t* len, const char* str) {
    while (*str != '\0') {
        buf[(*len)++] = *str++;
    }
}

static void append_line(char* buf, size_t* len, const char* str) {
    while (*str != '\0') {
        buf[(*len)++] = *str++;
    }
    buf[(*len)++] = '\n';
}

#define RAND_NOISE "((random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + 1.0) / 2.0)"

static const char* shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                                      bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
#ifdef __vita__
            case SHADER_0:
                return with_alpha ? "float4(0.0, 0.0, 0.0, 0.0)" : "float3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "float4(1.0, 1.0, 1.0, 1.0)" : "float3(1.0, 1.0, 1.0)";
#else
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
#endif
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
#ifdef __vita__
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a"
                                           : (with_alpha ? "float4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                         : "float3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a"
                                           : (with_alpha ? "float4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                         : "float3(texVal1.a, texVal1.a, texVal1.a)");
#else
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a"
                                           : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                         : "vec3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a"
                                           : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                         : "vec3(texVal1.a, texVal1.a, texVal1.a)");
#endif
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
#ifdef __vita__
                return with_alpha ? "float4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "float3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
#else
                return with_alpha ? "vec4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "vec3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
#endif
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1A:
                return "texVal1.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
            case SHADER_COMBINED:
                return "texel.a";
            case SHADER_NOISE:
                return RAND_NOISE;
        }
    }
    return "";
}

#undef RAND_NOISE

static void append_formula(char* buf, size_t* len, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix,
                           bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
#ifdef __vita__
        append_str(buf, len, "lerp(");
#else
        append_str(buf, len, "mix(");
#endif
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

static struct ShaderProgram* gfx_opengl_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    struct CCFeatures cc_features;
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    char vs_buf[1024];
    char fs_buf[3000];
    size_t vs_len = 0;
    size_t fs_len = 0;
    size_t num_floats = 4;
    GLuint shader_program;
	
#ifdef __vita__
    #define SHADER_MAGIC 1
    char fname[256];
    sprintf(fname, "ux0:data/soh/shader_cache/%08X_%016llX_%d.bin", shader_id1, shader_id0, SHADER_MAGIC);
    FILE *f = fopen(fname, "rb");
    if (f) {
        shader_program = glCreateProgram();
        fseek(f, 0, SEEK_END);
        int prog_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        void *prog_bin = malloc(prog_size - 4);
		fread(&num_floats, 1, sizeof(size_t), f);
        fread(prog_bin, 1, prog_size, f);
        fclose(f);
        glProgramBinary(shader_program, 0, prog_bin, prog_size);
        free(prog_bin);
    } else {
		
    int texcoord_id = 0;
#endif

    // Vertex shader
#ifdef __APPLE__
    append_line(vs_buf, &vs_len, "#version 410 core");
    append_line(vs_buf, &vs_len, "in vec4 aVtxPos;");
#elif defined(__vita__)
    append_line(vs_buf, &vs_len, "void main(");
    append_line(vs_buf, &vs_len, "float4 aVtxPos,");
#else
    append_line(vs_buf, &vs_len, "#version 110");
    append_line(vs_buf, &vs_len, "attribute vec4 aVtxPos;");
#endif
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
#ifdef __APPLE__
            vs_len += sprintf(vs_buf + vs_len, "in vec2 aTexCoord%d;\n", i);
            vs_len += sprintf(vs_buf + vs_len, "out vec2 vTexCoord%d;\n", i);
#elif defined(__vita__)
            vs_len += sprintf(vs_buf + vs_len, "float2 aTexCoord%d,\n", i);
            vs_len += sprintf(vs_buf + vs_len, "float2 out vTexCoord%d : TEXCOORD%d,\n", i, texcoord_id++);
#else
            vs_len += sprintf(vs_buf + vs_len, "attribute vec2 aTexCoord%d;\n", i);
            vs_len += sprintf(vs_buf + vs_len, "varying vec2 vTexCoord%d;\n", i);
#endif
            num_floats += 2;
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
#ifdef __APPLE__
                    vs_len += sprintf(vs_buf + vs_len, "in float aTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
                    vs_len += sprintf(vs_buf + vs_len, "out float vTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
#elif defined(__vita__)
                    vs_len += sprintf(vs_buf + vs_len, "float aTexClamp%s%d,\n", j == 0 ? "S" : "T", i);
                    vs_len += sprintf(vs_buf + vs_len, "float out vTexClamp%s%d : TEXCOORD%d,\n", j == 0 ? "S" : "T", i, texcoord_id++);
#else
                    vs_len += sprintf(vs_buf + vs_len, "attribute float aTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
                    vs_len += sprintf(vs_buf + vs_len, "varying float vTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
#endif
                    num_floats += 1;
                }
            }
        }
    }
    if (cc_features.opt_fog) {
#ifdef __APPLE__
        append_line(vs_buf, &vs_len, "in vec4 aFog;");
        append_line(vs_buf, &vs_len, "out vec4 vFog;");
#elif defined(__vita__)
        append_line(vs_buf, &vs_len, "float4 aFog,");
        vs_len += sprintf(vs_buf + vs_len, "float4 out vFog : TEXCOORD%d,", texcoord_id++);
#else
        append_line(vs_buf, &vs_len, "attribute vec4 aFog;");
        append_line(vs_buf, &vs_len, "varying vec4 vFog;");
#endif
        num_floats += 4;
    }

    if (cc_features.opt_grayscale) {
#ifdef __APPLE__
        append_line(vs_buf, &vs_len, "in vec4 aGrayscaleColor;");
        append_line(vs_buf, &vs_len, "out vec4 vGrayscaleColor;");
#elif defined(__vita__)
        append_line(vs_buf, &vs_len, "float4 aGrayscaleColor,");
        append_line(vs_buf, &vs_len, "float4 out vGrayscaleColor : COLOR,");
#else
        append_line(vs_buf, &vs_len, "attribute vec4 aGrayscaleColor;");
        append_line(vs_buf, &vs_len, "varying vec4 vGrayscaleColor;");
#endif
        num_floats += 4;
    }

    for (int i = 0; i < cc_features.num_inputs; i++) {
#ifdef __APPLE__
        vs_len += sprintf(vs_buf + vs_len, "in vec%d aInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        vs_len += sprintf(vs_buf + vs_len, "out vec%d vInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
#elif defined(__vita__)
        vs_len += sprintf(vs_buf + vs_len, "float%d aInput%d,\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        vs_len += sprintf(vs_buf + vs_len, "float%d out vInput%d : TEXCOORD%d,\n", cc_features.opt_alpha ? 4 : 3, i + 1, texcoord_id++);
#else
        vs_len += sprintf(vs_buf + vs_len, "attribute vec%d aInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        vs_len += sprintf(vs_buf + vs_len, "varying vec%d vInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
#endif
        num_floats += cc_features.opt_alpha ? 4 : 3;
    }
#ifdef __vita__
    append_line(vs_buf, &vs_len, "float4 out gl_Position : POSITION) {");
#else
    append_line(vs_buf, &vs_len, "void main() {");
#endif
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            vs_len += sprintf(vs_buf + vs_len, "vTexCoord%d = aTexCoord%d;\n", i, i);
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    vs_len += sprintf(vs_buf + vs_len, "vTexClamp%s%d = aTexClamp%s%d;\n", j == 0 ? "S" : "T", i,
                                      j == 0 ? "S" : "T", i);
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        append_line(vs_buf, &vs_len, "vFog = aFog;");
    }
    if (cc_features.opt_grayscale) {
        append_line(vs_buf, &vs_len, "vGrayscaleColor = aGrayscaleColor;");
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "vInput%d = aInput%d;\n", i + 1, i + 1);
    }
    append_line(vs_buf, &vs_len, "gl_Position = aVtxPos;");
    append_line(vs_buf, &vs_len, "}");
	
#ifdef __vita__
    texcoord_id = 0;

    append_line(fs_buf, &fs_len, "float random(float3 value) {");
    append_line(fs_buf, &fs_len, "    float res = dot(sin(value), float3(12.9898, 78.233, 37.719));");
    append_line(fs_buf, &fs_len, "    return frac(sin(res) * 143758.5453);");
    append_line(fs_buf, &fs_len, "}");

    //if (current_filter_mode == FILTER_THREE_POINT) {
    //    append_line(fs_buf, &fs_len, "#define TEX_OFFSET(off) tex2D(tex_sampler, texCoord - (off)/texSize)");
    //    append_line(fs_buf, &fs_len, "float4 filter3point(sampler2D tex_sampler, float2 texCoord, float2 texSize) {");
    //    append_line(fs_buf, &fs_len, "    float2 offset = frac(texCoord*texSize - float2(0.5));");
    //    append_line(fs_buf, &fs_len, "    offset -= step(1.0, offset.x + offset.y);");
    //    append_line(fs_buf, &fs_len, "    float4 c0 = TEX_OFFSET(offset);");
    //    append_line(fs_buf, &fs_len, "    float4 c1 = TEX_OFFSET(float2(offset.x - sign(offset.x), offset.y));");
    //    append_line(fs_buf, &fs_len, "    float4 c2 = TEX_OFFSET(float2(offset.x, offset.y - sign(offset.y)));");
    //    append_line(fs_buf, &fs_len, "    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);");
    //    append_line(fs_buf, &fs_len, "}");
    //    append_line(fs_buf, &fs_len, "float4 hookTexture2D(sampler2D tex_sampler, float2 uv, float2 texSize) {");
    //    append_line(fs_buf, &fs_len, "    return filter3point(tex_sampler, uv, texSize);");
    //    append_line(fs_buf, &fs_len, "}");
    //} else
    {
        append_line(fs_buf, &fs_len, "float4 hookTexture2D(sampler2D tex, float2 uv, float2 texSize) {");
        append_line(fs_buf, &fs_len, "    return tex2D(tex, uv);");
        append_line(fs_buf, &fs_len, "}");
    }
#endif

    // Fragment shader
#ifdef __APPLE__
    append_line(fs_buf, &fs_len, "#version 410 core");
#elif defined(__vita__)
    append_line(fs_buf, &fs_len, "float4 main(");
#else
    append_line(fs_buf, &fs_len, "#version 130");
#endif
    // append_line(fs_buf, &fs_len, "precision mediump float;");
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
#ifdef __APPLE__
            fs_len += sprintf(fs_buf + fs_len, "in vec2 vTexCoord%d;\n", i);
#elif defined(__vita__)
            fs_len += sprintf(fs_buf + fs_len, "uniform float2 texSize%d,\n", i);
            fs_len += sprintf(fs_buf + fs_len, "float2 vTexCoord%d : TEXCOORD%d,\n", i, texcoord_id++);
#else
            fs_len += sprintf(fs_buf + fs_len, "varying vec2 vTexCoord%d;\n", i);
#endif
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
#ifdef __APPLE__
                    fs_len += sprintf(fs_buf + fs_len, "in float vTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
#elif defined(__vita__)
                    fs_len += sprintf(fs_buf + fs_len, "in float vTexClamp%s%d : TEXCOORD%d,\n", j == 0 ? "S" : "T", i, texcoord_id++);
#else
                    fs_len += sprintf(fs_buf + fs_len, "varying float vTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
#endif
                }
            }
        }
    }
    if (cc_features.opt_fog) {
#ifdef __APPLE__
        append_line(fs_buf, &fs_len, "in vec4 vFog;");
#elif defined(__vita__)
        fs_len += sprintf(fs_buf + fs_len, "float4 vFog : TEXCOORD%d,", texcoord_id++);
#else
        append_line(fs_buf, &fs_len, "varying vec4 vFog;");
#endif
    }
    if (cc_features.opt_grayscale) {
#ifdef __APPLE__
        append_line(fs_buf, &fs_len, "in vec4 vGrayscaleColor;");
#elif defined(__vita__)
        append_line(fs_buf, &fs_len, "float4 vGrayscaleColor : COLOR,");
#else
        append_line(fs_buf, &fs_len, "varying vec4 vGrayscaleColor;");
#endif
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
#ifdef __APPLE__
        fs_len += sprintf(fs_buf + fs_len, "in vec%d vInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
    #elif defined(__vita__)
        fs_len += sprintf(fs_buf + fs_len, "float%d vInput%d : TEXCOORD%d,\n", cc_features.opt_alpha ? 4 : 3, i + 1, texcoord_id++);
#else
        fs_len += sprintf(fs_buf + fs_len, "varying vec%d vInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
#endif
    }
#ifdef __vita__
    if (cc_features.used_textures[0]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex0,");
    }
    if (cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex1,");
    }

    append_line(fs_buf, &fs_len, "uniform int frame_count,");
    append_line(fs_buf, &fs_len, "uniform float noise_scale,");
#else
    if (cc_features.used_textures[0]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex0;");
    }
    if (cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex1;");
    }

    append_line(fs_buf, &fs_len, "uniform int frame_count;");
    append_line(fs_buf, &fs_len, "uniform float noise_scale;");
#endif
#ifndef __vita__
    append_line(fs_buf, &fs_len, "float random(in vec3 value) {");
    append_line(fs_buf, &fs_len, "    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));");
    append_line(fs_buf, &fs_len, "    return fract(sin(random) * 143758.5453);");
    append_line(fs_buf, &fs_len, "}");

    if (current_filter_mode == FILTER_THREE_POINT) {
#if __APPLE__
        append_line(fs_buf, &fs_len, "#define TEX_OFFSET(off) texture(tex, texCoord - (off)/texSize)");
#else
        append_line(fs_buf, &fs_len, "#define TEX_OFFSET(off) texture2D(tex, texCoord - (off)/texSize)");
#endif
        append_line(fs_buf, &fs_len, "vec4 filter3point(in sampler2D tex, in vec2 texCoord, in vec2 texSize) {");
        append_line(fs_buf, &fs_len, "    vec2 offset = fract(texCoord*texSize - vec2(0.5));");
        append_line(fs_buf, &fs_len, "    offset -= step(1.0, offset.x + offset.y);");
        append_line(fs_buf, &fs_len, "    vec4 c0 = TEX_OFFSET(offset);");
        append_line(fs_buf, &fs_len, "    vec4 c1 = TEX_OFFSET(vec2(offset.x - sign(offset.x), offset.y));");
        append_line(fs_buf, &fs_len, "    vec4 c2 = TEX_OFFSET(vec2(offset.x, offset.y - sign(offset.y)));");
        append_line(fs_buf, &fs_len, "    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);");
        append_line(fs_buf, &fs_len, "}");
        append_line(fs_buf, &fs_len, "vec4 hookTexture2D(in sampler2D tex, in vec2 uv, in vec2 texSize) {");
        append_line(fs_buf, &fs_len, "    return filter3point(tex, uv, texSize);");
        append_line(fs_buf, &fs_len, "}");
    } else {
        append_line(fs_buf, &fs_len, "vec4 hookTexture2D(in sampler2D tex, in vec2 uv, in vec2 texSize) {");
#if __APPLE__
        append_line(fs_buf, &fs_len, "    return texture(tex, uv);");
#else
        append_line(fs_buf, &fs_len, "    return texture2D(tex, uv);");
#endif
        append_line(fs_buf, &fs_len, "}");
    }
#endif
#if __APPLE__
    append_line(fs_buf, &fs_len, "out vec4 outColor;");
#endif
#ifdef __vita__
    append_line(fs_buf, &fs_len, "float4 gl_FragCoord : WPOS) {");
#else
    append_line(fs_buf, &fs_len, "void main() {");
#edif
    // Reference approach to color wrapping as per GLideN64
    // Return wrapped value of x in interval [low, high)
#ifdef __vita__
    append_line(fs_buf, &fs_len, "#define WRAP(x, low, high) fmod((x)-(low), (high)-(low)) + (low)");
#else
    append_line(fs_buf, &fs_len, "#define WRAP(x, low, high) mod((x)-(low), (high)-(low)) + (low)");
#endif
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            bool s = cc_features.clamp[i][0], t = cc_features.clamp[i][1];
#ifdef __vita__
            if (!s && !t) {
                fs_len += sprintf(fs_buf + fs_len, "float4 texVal%d = hookTexture2D(uTex%d, vTexCoord%d, texSize%d);\n", i, i, i, i);
            } else {
                if (s && t) {
                    fs_len += sprintf(fs_buf + fs_len, "float4 texVal%d = hookTexture2D(uTex%d, clamp(vTexCoord%d, 0.5 / texSize%d, float2(vTexClampS%d, vTexClampT%d)), texSize%d);\n", i, i, i, i, i, i, i);
                } else if (s) {
                    fs_len += sprintf(fs_buf + fs_len, "float4 texVal%d = hookTexture2D(uTex%d, float2(clamp(vTexCoord%d.x, 0.5 / texSize%d.x, vTexClampS%d), vTexCoord%d.y), texSize%d);\n", i, i, i, i, i, i, i);
                } else {
                    fs_len += sprintf(fs_buf + fs_len, "float4 texVal%d = hookTexture2D(uTex%d, float2(vTexCoord%d.x, clamp(vTexCoord%d.y, 0.5 / texSize%d.y, vTexClampT%d)), texSize%d);\n", i, i, i, i, i, i, i);
#else
            fs_len += sprintf(fs_buf + fs_len, "vec2 texSize%d = textureSize(uTex%d, 0);\n", i, i);

            if (!s && !t) {
                fs_len += sprintf(fs_buf + fs_len, "vec4 texVal%d = hookTexture2D(uTex%d, vTexCoord%d, texSize%d);\n",
                                  i, i, i, i);
            } else {
                if (s && t) {
                    fs_len += sprintf(fs_buf + fs_len,
                                      "vec4 texVal%d = hookTexture2D(uTex%d, clamp(vTexCoord%d, 0.5 / texSize%d, "
                                      "vec2(vTexClampS%d, vTexClampT%d)), texSize%d);\n",
                                      i, i, i, i, i, i, i);
                } else if (s) {
                    fs_len += sprintf(fs_buf + fs_len,
                                      "vec4 texVal%d = hookTexture2D(uTex%d, vec2(clamp(vTexCoord%d.s, 0.5 / "
                                      "texSize%d.s, vTexClampS%d), vTexCoord%d.t), texSize%d);\n",
                                      i, i, i, i, i, i, i);
                } else {
                    fs_len += sprintf(fs_buf + fs_len,
                                      "vec4 texVal%d = hookTexture2D(uTex%d, vec2(vTexCoord%d.s, clamp(vTexCoord%d.t, "
                                      "0.5 / texSize%d.t, vTexClampT%d)), texSize%d);\n",
                                      i, i, i, i, i, i, i);
#endif
                }
            }
        }
    }
#ifdef __vita__
    append_line(fs_buf, &fs_len, cc_features.opt_alpha ? "float4 texel;" : "float3 texel;");
    for (int c = 0; c < (cc_features.opt_2cyc ? 2 : 1); c++) {
        append_str(fs_buf, &fs_len, "texel = ");
        if (!cc_features.color_alpha_same[c] && cc_features.opt_alpha) {
            append_str(fs_buf, &fs_len, "float4(");
#else
    append_line(fs_buf, &fs_len, cc_features.opt_alpha ? "vec4 texel;" : "vec3 texel;");
    for (int c = 0; c < (cc_features.opt_2cyc ? 2 : 1); c++) {
        append_str(fs_buf, &fs_len, "texel = ");
        if (!cc_features.color_alpha_same[c] && cc_features.opt_alpha) {
            append_str(fs_buf, &fs_len, "vec4(");
#endif
            append_formula(fs_buf, &fs_len, cc_features.c[c], cc_features.do_single[c][0],
                           cc_features.do_multiply[c][0], cc_features.do_mix[c][0], false, false, true);
            append_str(fs_buf, &fs_len, ", ");
            append_formula(fs_buf, &fs_len, cc_features.c[c], cc_features.do_single[c][1],
                           cc_features.do_multiply[c][1], cc_features.do_mix[c][1], true, true, true);
            append_str(fs_buf, &fs_len, ")");
        } else {
            append_formula(fs_buf, &fs_len, cc_features.c[c], cc_features.do_single[c][0],
                           cc_features.do_multiply[c][0], cc_features.do_mix[c][0], cc_features.opt_alpha, false,
                           cc_features.opt_alpha);
        }
        append_line(fs_buf, &fs_len, ";");

        if (c == 0) {
            append_str(fs_buf, &fs_len, "texel = WRAP(texel, -1.01, 1.01);");
        }
    }
#ifdef __vita__
    append_str(fs_buf, &fs_len, "texel = WRAP(texel, -0.51, 1.51);");
    append_str(fs_buf, &fs_len, "texel = clamp(texel, 0.0, 1.0);");
    // TODO discard if alpha is 0?
    if (cc_features.opt_fog)
    {
        if (cc_features.opt_alpha)
        {
            append_line(fs_buf, &fs_len, "texel = float4(lerp(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        }
        else
        {
            append_line(fs_buf, &fs_len, "texel = lerp(texel, vFog.rgb, vFog.a);");
        }
    }

    if (cc_features.opt_texture_edge && cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len, "if (texel.a > 0.19) texel.a = 1.0; else discard;");
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len, "texel.a *= floor(clamp(random(float3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + texel.a, 0.0, 1.0));");
    }

    if (cc_features.opt_grayscale) {
        append_line(fs_buf, &fs_len, "float intensity = (texel.r + texel.g + texel.b) / 3.0;");
        append_line(fs_buf, &fs_len, "float3 new_texel = vGrayscaleColor.rgb * intensity;");
        append_line(fs_buf, &fs_len, "texel.rgb = lerp(texel.rgb, new_texel, vGrayscaleColor.a);");
    }

    if (cc_features.opt_alpha) {
        if (cc_features.opt_alpha_threshold) {
            append_line(fs_buf, &fs_len, "if (texel.a < 8.0 / 256.0) discard;");
        }
        if (cc_features.opt_invisible) {
            append_line(fs_buf, &fs_len, "texel.a = 0.0;");
        }
        append_line(fs_buf, &fs_len, "return texel;");
    } else {
        append_line(fs_buf, &fs_len, "return float4(texel, 1.0);");
    }
    append_line(fs_buf, &fs_len, "}");
#else
    append_str(fs_buf, &fs_len, "texel = WRAP(texel, -0.51, 1.51);");
    append_str(fs_buf, &fs_len, "texel = clamp(texel, 0.0, 1.0);");
    // TODO discard if alpha is 0?
    if (cc_features.opt_fog) {
        if (cc_features.opt_alpha) {
            append_line(fs_buf, &fs_len, "texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(fs_buf, &fs_len, "texel = mix(texel, vFog.rgb, vFog.a);");
        }
    }

    if (cc_features.opt_texture_edge && cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len, "if (texel.a > 0.19) texel.a = 1.0; else discard;");
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len,
                    "texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + "
                    "texel.a, 0.0, 1.0));");
    }

    if (cc_features.opt_grayscale) {
        append_line(fs_buf, &fs_len, "float intensity = (texel.r + texel.g + texel.b) / 3.0;");
        append_line(fs_buf, &fs_len, "vec3 new_texel = vGrayscaleColor.rgb * intensity;");
        append_line(fs_buf, &fs_len, "texel.rgb = mix(texel.rgb, new_texel, vGrayscaleColor.a);");
    }

    if (cc_features.opt_alpha) {
        if (cc_features.opt_alpha_threshold) {
            append_line(fs_buf, &fs_len, "if (texel.a < 8.0 / 256.0) discard;");
        }
        if (cc_features.opt_invisible) {
            append_line(fs_buf, &fs_len, "texel.a = 0.0;");
        }
#if __APPLE__
        append_line(fs_buf, &fs_len, "outColor = texel;");
#else
        append_line(fs_buf, &fs_len, "gl_FragColor = texel;");
#endif
    } else {
#if __APPLE__
        append_line(fs_buf, &fs_len, "outColor = vec4(texel, 1.0);");
#else
        append_line(fs_buf, &fs_len, "gl_FragColor = vec4(texel, 1.0);");
#endif
    }
    append_line(fs_buf, &fs_len, "}");
#endif
    vs_buf[vs_len] = '\0';
    fs_buf[fs_len] = '\0';

    /*puts("Vertex shader:");
    puts(vs_buf);
    puts("Fragment shader:");
    puts(fs_buf);
    puts("End");*/

    const GLchar* sources[2] = { vs_buf, fs_buf };
    const GLint lengths[2] = { (GLint)vs_len, (GLint)fs_len };
    GLint success;

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
#ifdef __vita__
    glShaderSource(vertex_shader, 1, &sources[0], NULL);
#else
    glShaderSource(vertex_shader, 1, &sources[0], &lengths[0]);
#endif
    glCompileShader(vertex_shader);
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
#ifdef __vita__
        printf("VERTEX SHADER:\n%s\n", vs_buf);
#endif
        GLint max_length = 0;
        glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        // fprintf(stderr, "Vertex shader compilation failed\n");
        glGetShaderInfoLog(vertex_shader, max_length, &max_length, &error_log[0]);
        // fprintf(stderr, "%s\n", &error_log[0]);
        abort();
    }

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
#ifdef __vita__
    glShaderSource(fragment_shader, 1, &sources[1], NULL);
#else
    glShaderSource(fragment_shader, 1, &sources[1], &lengths[1]);
#endif
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
#ifdef __vita__
        printf("FRAGMENT SHADER:\n%s\n", fs_buf);
#endif
        GLint max_length = 0;
        glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        fprintf(stderr, "Fragment shader compilation failed\n");
        glGetShaderInfoLog(fragment_shader, max_length, &max_length, &error_log[0]);
        fprintf(stderr, "%s\n", &error_log[0]);
        abort();
    }

    shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);

#ifdef __vita__
    // Caching precompiled shader on filesystem
    f = fopen(fname, "wb");
    int prog_size, prog_len = 0;
    unsigned int prog_format = 0;
    glGetProgramiv(shader_program, GL_PROGRAM_BINARY_LENGTH, &prog_size);
    void *prog_bin = malloc(prog_size);
    glGetProgramBinary(shader_program, prog_size, &prog_len, &prog_format, prog_bin);
	fwrite(&num_floats, 1, sizeof(size_t), f);
    fwrite(prog_bin, 1, prog_len, f);
    fclose(f);
    free(prog_bin);

    }
#endif

    size_t cnt = 0;

    struct ShaderProgram* prg = &shader_program_pool[make_pair(shader_id0, shader_id1)];
    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aVtxPos");
    prg->attrib_sizes[cnt] = 4;
    ++cnt;

    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            char name[32];
            sprintf(name, "aTexCoord%d", i);
            prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
            prg->attrib_sizes[cnt] = 2;
            ++cnt;

            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    sprintf(name, "aTexClamp%s%d", j == 0 ? "S" : "T", i);
                    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
                    prg->attrib_sizes[cnt] = 1;
                    ++cnt;
                }
            }
        }
    }

    if (cc_features.opt_fog) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aFog");
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }

    if (cc_features.opt_grayscale) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aGrayscaleColor");
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }

    for (int i = 0; i < cc_features.num_inputs; i++) {
        char name[16];
        sprintf(name, "aInput%d", i + 1);
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
        prg->attrib_sizes[cnt] = cc_features.opt_alpha ? 4 : 3;
        ++cnt;
    }

    prg->opengl_program_id = shader_program;
    prg->num_inputs = cc_features.num_inputs;
    prg->used_textures[0] = cc_features.used_textures[0];
    prg->used_textures[1] = cc_features.used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;

    gfx_opengl_load_shader(prg);

    if (cc_features.used_textures[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex0");
        glUniform1i(sampler_location, 0);
#ifdef __vita__
        prg->uTexSize[0] = glGetUniformLocation(shader_program, "texSize0");
#endif
    }
    if (cc_features.used_textures[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex1");
        glUniform1i(sampler_location, 1);
#ifdef __vita__
        prg->uTexSize[1] = glGetUniformLocation(shader_program, "texSize1");
#endif
    }

    prg->frame_count_location = glGetUniformLocation(shader_program, "frame_count");
    prg->noise_scale_location = glGetUniformLocation(shader_program, "noise_scale");

    return prg;
}

static struct ShaderProgram* gfx_opengl_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    auto it = shader_program_pool.find(make_pair(shader_id0, shader_id1));
    return it == shader_program_pool.end() ? nullptr : &it->second;
}

static void gfx_opengl_shader_get_info(struct ShaderProgram* prg, uint8_t* num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static GLuint gfx_opengl_new_texture(void) {
    GLuint ret;
    glGenTextures(1, &ret);
    return ret;
}

static void gfx_opengl_delete_texture(uint32_t texID) {
    glDeleteTextures(1, &texID);
}

static void gfx_opengl_select_texture(int tile, GLuint texture_id) {
    glActiveTexture(GL_TEXTURE0 + tile);
    glBindTexture(GL_TEXTURE_2D, texture_id);
#ifdef __vita__
    SceGxmTexture *gxm_tex = vglGetGxmTexture(GL_TEXTURE_2D);
    if (tile == 0) {
        tex0_size[0] = sceGxmTextureGetWidth(gxm_tex);
        tex0_size[1] = sceGxmTextureGetHeight(gxm_tex);
        cur_tex_size = tex0_size;
    } else {
        tex1_size[0] = sceGxmTextureGetWidth(gxm_tex);
        tex1_size[1] = sceGxmTextureGetHeight(gxm_tex);
        cur_tex_size = tex1_size;
    }
#endif
}

#ifdef __vita__
#define GL_RGBA8 GL_RGBA
#define GL_RGB8 GL_RGB
#endif

static void gfx_opengl_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
#ifdef __vita__
    cur_tex_size[0] = width;
    cur_tex_size[1] = height;
#endif
}

#if defined(__SWITCH__) || defined(__vita__)
#define GL_MIRROR_CLAMP_TO_EDGE 0x8743
#endif

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return GL_CLAMP_TO_EDGE;
        case G_TX_MIRROR | G_TX_WRAP:
            return GL_MIRRORED_REPEAT;
        case G_TX_MIRROR | G_TX_CLAMP:
            return GL_MIRROR_CLAMP_TO_EDGE;
        case G_TX_NOMIRROR | G_TX_WRAP:
            return GL_REPEAT;
    }
    return 0;
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    const GLint filter = linear_filter && current_filter_mode == FILTER_LINEAR ? GL_LINEAR : GL_NEAREST;
    glActiveTexture(GL_TEXTURE0 + tile);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengl(cmt));
}

static void gfx_opengl_set_depth_test_and_mask(bool depth_test, bool z_upd) {
    if (depth_test || z_upd) {
        glEnable(GL_DEPTH_TEST);
        glDepthMask(z_upd ? GL_TRUE : GL_FALSE);
        glDepthFunc(depth_test ? GL_LEQUAL : GL_ALWAYS);
        current_depth_mask = z_upd;
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

static void gfx_opengl_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {

        // SSDB = SlopeScaledDepthBias 120 leads to -2 at 240p which is the same as N64 mode which has very little
        // fighting
        const int n64modeFactor = 120;
        const int noVanishFactor = 100;
        GLfloat SSDB = -2;
        switch (CVarGetInteger("gDirtPathFix", 0)) {
            // scaled z-fighting (N64 mode like)
            case 1:
                if (framebuffers.size() > current_framebuffer) { // safety check for vector size can probably be removed
                    SSDB = -1.0f * (GLfloat)framebuffers[current_framebuffer].height / n64modeFactor;
                }
                break;
            // no vanishing paths
            case 2:
                if (framebuffers.size() > current_framebuffer) { // safety check for vector size can probably be removed
                    SSDB = -1.0f * (GLfloat)framebuffers[current_framebuffer].height / noVanishFactor;
                }
                break;
            // disabled
            case 0:
            default:
                SSDB = -2;
        }
        glPolygonOffset(SSDB, -2);
        glEnable(GL_POLYGON_OFFSET_FILL);
    } else {
        glPolygonOffset(0, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
    glScissor(x, y, width, height);
}

static void gfx_opengl_set_use_alpha(bool use_alpha) {
    if (use_alpha) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    // printf("flushing %d tris\n", buf_vbo_num_tris);
#ifdef __vita__
    if (cur_gl_program->used_textures[0]) {
        glUniform2fv(cur_gl_program->uTexSize[0], 1, tex0_size);
    }
    if (cur_gl_program->used_textures[1]) {
        glUniform2fv(cur_gl_program->uTexSize[1], 1, tex1_size);
    }
#endif
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
}

static void gfx_opengl_init(void) {
#if !defined(__SWITCH__) && !defined(__vita__)
    glewInit();
#endif

    glGenBuffers(1, &opengl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);

#ifdef __APPLE__
    glGenVertexArrays(1, &opengl_vao);
    glBindVertexArray(opengl_vao);
#endif
#ifndef __vita__
    glEnable(GL_DEPTH_CLAMP);
#endif
    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    framebuffers.resize(1); // for the default screen buffer

    glGenRenderbuffers(1, &pixel_depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, pixel_depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1, 1);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &pixel_depth_fb);
    glBindFramebuffer(GL_FRAMEBUFFER, pixel_depth_fb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, pixel_depth_rb);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    pixel_depth_rb_size = 1;
}

static void gfx_opengl_on_resize(void) {
}

static void gfx_opengl_start_frame(void) {
    frame_count++;
}

static void gfx_opengl_end_frame(void) {
    glFlush();
}

static void gfx_opengl_finish_render(void) {
}

static int gfx_opengl_create_framebuffer() {
    GLuint clrbuf;
    glGenTextures(1, &clrbuf);
    glBindTexture(GL_TEXTURE_2D, clrbuf);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    GLuint clrbuf_msaa;
    glGenRenderbuffers(1, &clrbuf_msaa);

    GLuint rbo;
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1, 1);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);

    size_t i = framebuffers.size();
    framebuffers.resize(i + 1);

    framebuffers[i].fbo = fbo;
    framebuffers[i].clrbuf = clrbuf;
    framebuffers[i].clrbuf_msaa = clrbuf_msaa;
    framebuffers[i].rbo = rbo;

    return i;
}

static void gfx_opengl_update_framebuffer_parameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                     bool opengl_invert_y, bool render_target, bool has_depth_buffer,
                                                     bool can_extract_depth) {
    Framebuffer& fb = framebuffers[fb_id];

    width = max(width, 1U);
    height = max(height, 1U);

    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

    if (fb_id != 0) {
        if (fb.width != width || fb.height != height || fb.msaa_level != msaa_level) {
            if (msaa_level <= 1) {
                glBindTexture(GL_TEXTURE_2D, fb.clrbuf);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
                glBindTexture(GL_TEXTURE_2D, 0);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb.clrbuf, 0);
#ifndef __vita__
            } else {
                glBindRenderbuffer(GL_RENDERBUFFER, fb.clrbuf_msaa);
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_level, GL_RGB8, width, height);
                glBindRenderbuffer(GL_RENDERBUFFER, 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fb.clrbuf_msaa);
            }
#endif
        }

        if (has_depth_buffer &&
            (fb.width != width || fb.height != height || fb.msaa_level != msaa_level || !fb.has_depth_buffer)) {
            glBindRenderbuffer(GL_RENDERBUFFER, fb.rbo);
            if (msaa_level <= 1) {
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
            }
#ifndef __vita__
			else {
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_level, GL_DEPTH24_STENCIL8, width, height);
            }
#endif
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
        }

        if (!fb.has_depth_buffer && has_depth_buffer) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fb.rbo);
        } else if (fb.has_depth_buffer && !has_depth_buffer) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        }
    }

    fb.width = width;
    fb.height = height;
    fb.has_depth_buffer = has_depth_buffer;
    fb.msaa_level = msaa_level;
    fb.invert_y = opengl_invert_y;
}

void gfx_opengl_start_draw_to_framebuffer(int fb_id, float noise_scale) {
    Framebuffer& fb = framebuffers[fb_id];

    if (noise_scale != 0.0f) {
        current_noise_scale = 1.0f / noise_scale;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
    current_framebuffer = fb_id;
}

void gfx_opengl_clear_framebuffer() {
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDepthMask(current_depth_mask ? GL_TRUE : GL_FALSE);
    glEnable(GL_SCISSOR_TEST);
}

void gfx_opengl_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {
#ifndef __vita__
    Framebuffer& fb_dst = framebuffers[fb_id_target];
    Framebuffer& fb_src = framebuffers[fb_id_source];
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_dst.fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb_src.fbo);
    glBlitFramebuffer(0, 0, fb_src.width, fb_src.height, 0, 0, fb_dst.width, fb_dst.height, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);
#endif
}

void* gfx_opengl_get_framebuffer_texture_id(int fb_id) {
    return (void*)(uintptr_t)framebuffers[fb_id].clrbuf;
}

void gfx_opengl_select_texture_fb(int fb_id) {
    // glDisable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, framebuffers[fb_id].clrbuf);
}

static std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
gfx_opengl_get_pixel_depth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res;

    Framebuffer& fb = framebuffers[fb_id];

    if (coordinates.size() == 1) {
        uint32_t depth_stencil_value;
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        int x = coordinates.begin()->first;
        int y = coordinates.begin()->second;
#ifndef __vita__ // TODO
        glReadPixels(x, fb.invert_y ? fb.height - y : y, 1, 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8,
                     &depth_stencil_value);
#endif
        res.emplace(*coordinates.begin(), (depth_stencil_value >> 18) << 2);
    } else {
        if (pixel_depth_rb_size < coordinates.size()) {
            // Resizing a renderbuffer seems broken with Intel's driver, so recreate one instead.
            glBindFramebuffer(GL_FRAMEBUFFER, pixel_depth_fb);
            glDeleteRenderbuffers(1, &pixel_depth_rb);
            glGenRenderbuffers(1, &pixel_depth_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, pixel_depth_rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, coordinates.size(), 1);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, pixel_depth_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);

            pixel_depth_rb_size = coordinates.size();
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.fbo);
#ifndef __vita__
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pixel_depth_fb);
#endif
        glDisable(GL_SCISSOR_TEST); // needed for the blit operation
#ifndef __vita__
        {
            size_t i = 0;
            for (const auto& coord : coordinates) {
                int x = coord.first;
                int y = coord.second;
                if (fb.invert_y) {
                    y = fb.height - y;
                }
                glBlitFramebuffer(x, y, x + 1, y + 1, i, 0, i + 1, 1, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                                  GL_NEAREST);
                ++i;
            }
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, pixel_depth_fb);
#endif
        vector<uint32_t> depth_stencil_values(coordinates.size());
#ifndef __vita__ // TODO
        glReadPixels(0, 0, coordinates.size(), 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, depth_stencil_values.data());
#endif
        {
            size_t i = 0;
            for (const auto& coord : coordinates) {
                res.emplace(coord, (depth_stencil_values[i++] >> 18) << 2);
            }
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);

    return res;
}

void gfx_opengl_set_texture_filter(FilteringMode mode) {
#ifdef __vita__
    if (mode == FILTER_THREE_POINT)
        mode = FILTER_LINEAR;
#endif
    current_filter_mode = mode;
    gfx_texture_cache_clear();
}

FilteringMode gfx_opengl_get_texture_filter(void) {
    return current_filter_mode;
}

struct GfxRenderingAPI gfx_opengl_api = { gfx_opengl_get_name,
                                          gfx_opengl_get_max_texture_size,
                                          gfx_opengl_get_clip_parameters,
                                          gfx_opengl_unload_shader,
                                          gfx_opengl_load_shader,
                                          gfx_opengl_create_and_load_new_shader,
                                          gfx_opengl_lookup_shader,
                                          gfx_opengl_shader_get_info,
                                          gfx_opengl_new_texture,
                                          gfx_opengl_select_texture,
                                          gfx_opengl_upload_texture,
                                          gfx_opengl_set_sampler_parameters,
                                          gfx_opengl_set_depth_test_and_mask,
                                          gfx_opengl_set_zmode_decal,
                                          gfx_opengl_set_viewport,
                                          gfx_opengl_set_scissor,
                                          gfx_opengl_set_use_alpha,
                                          gfx_opengl_draw_triangles,
                                          gfx_opengl_init,
                                          gfx_opengl_on_resize,
                                          gfx_opengl_start_frame,
                                          gfx_opengl_end_frame,
                                          gfx_opengl_finish_render,
                                          gfx_opengl_create_framebuffer,
                                          gfx_opengl_update_framebuffer_parameters,
                                          gfx_opengl_start_draw_to_framebuffer,
                                          gfx_opengl_clear_framebuffer,
                                          gfx_opengl_resolve_msaa_color_buffer,
                                          gfx_opengl_get_pixel_depth,
                                          gfx_opengl_get_framebuffer_texture_id,
                                          gfx_opengl_select_texture_fb,
                                          gfx_opengl_delete_texture,
                                          gfx_opengl_set_texture_filter,
                                          gfx_opengl_get_texture_filter };

#endif
