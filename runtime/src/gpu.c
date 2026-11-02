/* The finishing effects on the graphics card: lighting, the HDR look and sharpening, the
 * same steps as hd_post on the CPU (hd.c).
 *
 * SDL's OpenGL renderer draws everything else; this borrows its context. The picture goes
 * into a texture of its own and the glow for this game frame (made on the CPU, a few
 * thousand cells) into another. A first pass grades and lights each pixel once into a
 * texture in between (8 bits a channel, as the CPU keeps it); a second sharpens from there
 * into an SDL target texture, which the display filters then draw like any picture. (One
 * pass would grade every pixel five times over, once for each pixel that sharpens from
 * it: too much for many cards at 6x.) Without sharpening the first pass draws straight into
 * the target. Every piece of OpenGL state touched is put back as SDL left it, so its own
 * drawing carries on undisturbed. A timer query measures the card's own time for the
 * passes, read a picture later so it never waits. */
#include "gpu.h"

#include <SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double gpu_fx_upload_ms, gpu_fx_card_ms;

/* ---- OpenGL entry points (all through SDL, so nothing more to link) ------------------- */
#define GL_FUNCS(X) \
    X(CreateShader, GLuint, (GLenum)) X(ShaderSource, void, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(CompileShader, void, (GLuint)) X(GetShaderiv, void, (GLuint, GLenum, GLint *)) \
    X(GetShaderInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *)) X(CreateProgram, GLuint, (void)) \
    X(AttachShader, void, (GLuint, GLuint)) X(LinkProgram, void, (GLuint)) \
    X(GetProgramiv, void, (GLuint, GLenum, GLint *)) X(UseProgram, void, (GLuint)) \
    X(GetUniformLocation, GLint, (GLuint, const GLchar *)) X(Uniform1i, void, (GLint, GLint)) \
    X(Uniform1f, void, (GLint, GLfloat)) X(Uniform2f, void, (GLint, GLfloat, GLfloat)) \
    X(ActiveTexture, void, (GLenum)) X(GenTextures, void, (GLsizei, GLuint *)) \
    X(BindTexture, void, (GLenum, GLuint)) \
    X(TexImage2D, void, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(TexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(TexParameteri, void, (GLenum, GLenum, GLint)) X(PixelStorei, void, (GLenum, GLint)) \
    X(GetIntegerv, void, (GLenum, GLint *)) X(IsEnabled, GLboolean, (GLenum)) \
    X(Enable, void, (GLenum)) X(Disable, void, (GLenum)) X(Viewport, void, (GLint, GLint, GLsizei, GLsizei)) \
    X(Begin, void, (GLenum)) X(End, void, (void)) X(Vertex2f, void, (GLfloat, GLfloat)) \
    X(TexCoord2f, void, (GLfloat, GLfloat)) \
    X(GenFramebuffers, void, (GLsizei, GLuint *)) X(BindFramebuffer, void, (GLenum, GLuint)) \
    X(FramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint)) X(CheckFramebufferStatus, GLenum, (GLenum))
/* The timer, where the card has one (OpenGL 3.3 or ARB_timer_query): optional. */
#define GL_TIMER_FUNCS(X) \
    X(GenQueries, void, (GLsizei, GLuint *)) X(BeginQuery, void, (GLenum, GLuint)) X(EndQuery, void, (GLenum)) \
    X(GetQueryObjectiv, void, (GLuint, GLenum, GLint *)) X(GetQueryObjectui64v, void, (GLuint, GLenum, GLuint64 *))

#define DECLARE(name, ret, args) static ret (APIENTRY *gl_##name) args;
GL_FUNCS(DECLARE)
GL_TIMER_FUNCS(DECLARE)
#undef DECLARE

static bool load_funcs(void) {
    bool ok = true;
#define LOAD(name, ret, args) { *(void **)&gl_##name = SDL_GL_GetProcAddress("gl" #name); ok &= gl_##name != NULL; }
    GL_FUNCS(LOAD)
#undef LOAD
    return ok;
}

static bool load_timer(void) {
    bool ok = true;
#define LOAD(name, ret, args) { *(void **)&gl_##name = SDL_GL_GetProcAddress("gl" #name); ok &= gl_##name != NULL; }
    GL_TIMER_FUNCS(LOAD)
#undef LOAD
    return ok;
}

/* ---- The shaders --------------------------------------------------------------------- */
static const char *vertex_src =
    "#version 110\n"
    "varying vec2 uv;\n"
    "void main() { uv = gl_MultiTexCoord0.xy; gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0); }\n";

/* hd_post's steps for each pixel: the HDR look's S curve and vibrance, then the glow's
 * screen blend. */
static const char *grade_src =
    "#version 110\n"
    "uniform sampler2D pic, glow;\n"
    "uniform vec2 glow_scale;\n"
    "uniform float hdr_a, hdr_g, vib, light;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    vec3 c = texture2D(pic, uv).rgb;\n"
    "    if (hdr_a > 0.0) {\n"
    "        vec3 s = c * c * (3.0 - 2.0 * c);\n"
    "        c = pow(clamp(c + (s - c) * hdr_a, 0.0, 1.0), vec3(hdr_g));\n"
    "        float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b)), avg = (c.r + c.g + c.b) / 3.0;\n"
    "        float sat = mx > 0.0 ? (mx - mn) / mx : 0.0;\n"
    "        c = clamp(avg + (c - avg) * (1.0 + vib * (1.0 - sat)), 0.0, 1.0);\n"
    "    }\n"
    "    if (light > 0.0) {\n"
    "        vec3 l = min(texture2D(glow, uv * glow_scale).rgb * 4.0, 1.0);\n"
    "        c = 1.0 - (1.0 - c) * (1.0 - l);\n"
    "    }\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

/* hd_sharpen's: AMD's CAS from the four neighbours, flat and gradient neighbourhoods left
 * alone. */
static const char *sharpen_src =
    "#version 110\n"
    "uniform sampler2D pic;\n"
    "uniform vec2 texel;\n"
    "uniform float peak;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    vec3 e = texture2D(pic, uv).rgb;\n"
    "    vec3 b = texture2D(pic, uv - vec2(0.0, texel.y)).rgb, h = texture2D(pic, uv + vec2(0.0, texel.y)).rgb;\n"
    "    vec3 d = texture2D(pic, uv - vec2(texel.x, 0.0)).rgb, f = texture2D(pic, uv + vec2(texel.x, 0.0)).rgb;\n"
    "    vec3 k = vec3(77.0, 150.0, 29.0) / 256.0;\n"
    "    float le = dot(e, k), lb = dot(b, k), lh = dot(h, k), ld = dot(d, k), lf = dot(f, k);\n"
    "    float mn = min(le, min(min(lb, lh), min(ld, lf))), mx = max(le, max(max(lb, lh), max(ld, lf)));\n"
    "    if (mx - mn > 2.5 / 255.0) {\n"
    "        float room = min(mn, 1.0 - mx) / max(mx, 1.0 / 255.0);\n"
    "        float w = (room > 1.0 ? 1.0 : sqrt(room)) * peak;\n"
    "        e = clamp((e + w * (b + h + d + f)) / (1.0 + 4.0 * w), 0.0, 1.0);\n"
    "    }\n"
    "    gl_FragColor = vec4(e, 1.0);\n"
    "}\n";

static bool ready, timer;
static GLuint grade_prog, sharpen_prog, pic_tex, glow_tex, mid_tex, mid_fbo;
static GLint g_pic, g_glow, g_glow_scale, g_hdr_a, g_hdr_g, g_vib, g_light, s_pic, s_texel, s_peak;
static int pic_w, pic_h, glow_w, glow_h, mid_w, mid_h;
static SDL_Texture *target;
static int target_w, target_h;
static bool target_smooth;
static uint16_t *glow_px;
static GLuint queries[2];
static int query_at, queries_out;

static GLuint compile(GLenum kind, const char *src) {
    GLuint s = gl_CreateShader(kind);
    gl_ShaderSource(s, 1, &src, NULL);
    gl_CompileShader(s);
    GLint ok = 0;
    gl_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        gl_GetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "GPU effects: a shader did not build: %s\n", log);
        return 0;
    }
    return s;
}

static GLuint link(const char *fragment) {
    GLuint vs = compile(GL_VERTEX_SHADER, vertex_src), fs = compile(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) return 0;
    GLuint p = gl_CreateProgram();
    gl_AttachShader(p, vs);
    gl_AttachShader(p, fs);
    gl_LinkProgram(p);
    GLint ok = 0;
    gl_GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "GPU effects: a shader did not link\n"); return 0; }
    return p;
}

static void new_texture(GLuint *t, GLint filter) {
    gl_GenTextures(1, t);
    gl_BindTexture(GL_TEXTURE_2D, *t);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* The OpenGL state this changes, kept to put back. */
typedef struct { GLint program, active, tex0, tex1, viewport[4], row_length, fbo; GLboolean blend, scissor, cull, depth; } SavedState;

static void save_state(SavedState *s) {
    gl_GetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    gl_GetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
    gl_ActiveTexture(GL_TEXTURE1); gl_GetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex1);
    gl_ActiveTexture(GL_TEXTURE0); gl_GetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex0);
    gl_GetIntegerv(GL_VIEWPORT, s->viewport);
    gl_GetIntegerv(GL_UNPACK_ROW_LENGTH, &s->row_length);
    gl_GetIntegerv(GL_FRAMEBUFFER_BINDING, &s->fbo);
    s->blend = gl_IsEnabled(GL_BLEND); s->scissor = gl_IsEnabled(GL_SCISSOR_TEST);
    s->cull = gl_IsEnabled(GL_CULL_FACE); s->depth = gl_IsEnabled(GL_DEPTH_TEST);
}

static void set_enabled(GLenum cap, GLboolean on) { if (on) gl_Enable(cap); else gl_Disable(cap); }

static void restore_state(const SavedState *s) {
    gl_BindFramebuffer(GL_FRAMEBUFFER, (GLuint)s->fbo);
    gl_UseProgram((GLuint)s->program);
    gl_ActiveTexture(GL_TEXTURE1); gl_BindTexture(GL_TEXTURE_2D, (GLuint)s->tex1);
    gl_ActiveTexture(GL_TEXTURE0); gl_BindTexture(GL_TEXTURE_2D, (GLuint)s->tex0);
    gl_ActiveTexture((GLenum)s->active);
    gl_Viewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    gl_PixelStorei(GL_UNPACK_ROW_LENGTH, s->row_length);
    set_enabled(GL_BLEND, s->blend); set_enabled(GL_SCISSOR_TEST, s->scissor);
    set_enabled(GL_CULL_FACE, s->cull); set_enabled(GL_DEPTH_TEST, s->depth);
}

static bool gl_fx_init(SDL_Renderer *ren) {
    if (ready) return true;
    if (!load_funcs()) { fprintf(stderr, "GPU effects: OpenGL 2.0 with framebuffer objects is not available\n"); return false; }
    SDL_RenderFlush(ren);
    SavedState s;
    save_state(&s);
    grade_prog = link(grade_src);
    sharpen_prog = grade_prog ? link(sharpen_src) : 0;
    if (!sharpen_prog) { restore_state(&s); return false; }
    g_pic = gl_GetUniformLocation(grade_prog, "pic"); g_glow = gl_GetUniformLocation(grade_prog, "glow");
    g_glow_scale = gl_GetUniformLocation(grade_prog, "glow_scale");
    g_hdr_a = gl_GetUniformLocation(grade_prog, "hdr_a"); g_hdr_g = gl_GetUniformLocation(grade_prog, "hdr_g");
    g_vib = gl_GetUniformLocation(grade_prog, "vib"); g_light = gl_GetUniformLocation(grade_prog, "light");
    s_pic = gl_GetUniformLocation(sharpen_prog, "pic"); s_texel = gl_GetUniformLocation(sharpen_prog, "texel");
    s_peak = gl_GetUniformLocation(sharpen_prog, "peak");
    gl_ActiveTexture(GL_TEXTURE0);
    new_texture(&pic_tex, GL_NEAREST);
    new_texture(&glow_tex, GL_LINEAR);
    new_texture(&mid_tex, GL_NEAREST);
    gl_GenFramebuffers(1, &mid_fbo);
    timer = load_timer();
    if (timer) gl_GenQueries(2, queries);
    restore_state(&s);
    ready = true;
    return true;
}

static void quad(void) {
    /* Texture row 0 is the picture's top, and SDL keeps a target's row 0 at the top too:
     * clip-space y -1 is the first row. */
    gl_Begin(GL_QUADS);
    gl_TexCoord2f(0, 0); gl_Vertex2f(-1, -1);
    gl_TexCoord2f(1, 0); gl_Vertex2f(1, -1);
    gl_TexCoord2f(1, 1); gl_Vertex2f(1, 1);
    gl_TexCoord2f(0, 1); gl_Vertex2f(-1, 1);
    gl_End();
}

static SDL_Texture *gl_fx_finish(SDL_Renderer *ren, const uint32_t *px, int w, int h, int pitch, const HdPost *p, bool smooth) {
    if (!ready || w <= 0 || h <= 0) return NULL;
    if (!target || target_w != w || target_h != h) {
        if (target) SDL_DestroyTexture(target);
        target = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        target_w = w; target_h = h; target_smooth = !smooth;
        if (!target) return NULL;
    }
    if (target_smooth != smooth) { SDL_SetTextureScaleMode(target, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest); target_smooth = smooth; }
    /* The glow for this game frame, as 16-bit cells of a quarter of its strength (it can
     * reach 4x full light before the shader clips it to 1). */
    const float *cells = NULL;
    int bw = 0, bh = 0, f = 1;
    float k = 0;
    bool light = hd_glow(px, w, h, pitch, p, &cells, &bw, &bh, &f, &k);
    if (light) {
        static size_t cap;
        if ((size_t)bw * bh > cap) { free(glow_px); glow_px = malloc((size_t)bw * bh * 8); cap = glow_px ? (size_t)bw * bh : 0; }
        if (!glow_px) light = false;
        else
            for (int i = 0; i < bw * bh; i++)
                for (int c = 0; c < 4; c++) {
                    float v = c < 3 ? cells[i * 3 + c] * k / 4 : 1;
                    glow_px[i * 4 + c] = (uint16_t)(v <= 0 ? 0 : v >= 1 ? 65535 : v * 65535 + .5f);
                }
    }
    int hdr = p->hdr < 0 ? 0 : p->hdr > 2 ? 2 : p->hdr;
    float peak = hd_sharpen_peak(p->sharpen);

    if (SDL_SetRenderTarget(ren, target) != 0) return NULL;
    SDL_RenderFlush(ren);
    SavedState s;
    save_state(&s);
    /* The card's time for the last picture but one, if it is in (never waited for). */
    if (timer && queries_out) {
        GLint done = 0;
        GLuint q = queries[query_at ^ 1];
        gl_GetQueryObjectiv(q, GL_QUERY_RESULT_AVAILABLE, &done);
        if (done) {
            GLuint64 ns = 0;
            gl_GetQueryObjectui64v(q, GL_QUERY_RESULT, &ns);
            gpu_fx_card_ms = ns / 1e6;
        }
    }
    Uint64 t0 = SDL_GetPerformanceCounter();
    gl_ActiveTexture(GL_TEXTURE0);
    gl_BindTexture(GL_TEXTURE_2D, pic_tex);
    gl_PixelStorei(GL_UNPACK_ROW_LENGTH, pitch);
    /* BGRA as 8-8-8-8 reversed: the layout drivers copy straight in. */
    if (pic_w != w || pic_h != h) {
        gl_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
        pic_w = w; pic_h = h;
    } else gl_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    gl_PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gpu_fx_upload_ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000 / SDL_GetPerformanceFrequency();
    gl_ActiveTexture(GL_TEXTURE1);
    gl_BindTexture(GL_TEXTURE_2D, glow_tex);
    if (light) {
        if (glow_w != bw || glow_h != bh) {
            gl_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, bw, bh, 0, GL_RGBA, GL_UNSIGNED_SHORT, glow_px);
            glow_w = bw; glow_h = bh;
        } else gl_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bw, bh, GL_RGBA, GL_UNSIGNED_SHORT, glow_px);
    }
    gl_Disable(GL_BLEND); gl_Disable(GL_SCISSOR_TEST); gl_Disable(GL_CULL_FACE); gl_Disable(GL_DEPTH_TEST);
    gl_Viewport(0, 0, w, h);
    if (timer) gl_BeginQuery(GL_TIME_ELAPSED, queries[query_at]);

    /* Pass 1: graded and lit, into the texture in between (or the target if nothing is
     * sharpened). */
    bool sharpen = peak < 0;
    if (sharpen) {
        gl_ActiveTexture(GL_TEXTURE0);
        gl_BindTexture(GL_TEXTURE_2D, mid_tex);
        if (mid_w != w || mid_h != h) {
            gl_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
            mid_w = w; mid_h = h;
        }
        gl_BindFramebuffer(GL_FRAMEBUFFER, mid_fbo);
        gl_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mid_tex, 0);
        if (gl_CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) sharpen = false;
        if (!sharpen) gl_BindFramebuffer(GL_FRAMEBUFFER, (GLuint)s.fbo);
    }
    gl_ActiveTexture(GL_TEXTURE0);
    gl_BindTexture(GL_TEXTURE_2D, pic_tex);
    gl_UseProgram(grade_prog);
    gl_Uniform1i(g_pic, 0);
    gl_Uniform1i(g_glow, 1);
    /* A glow cell covers f picture pixels; the CPU samples it at (x + .5) / f - .5 cells. */
    gl_Uniform2f(g_glow_scale, light ? (float)w / (f * bw) : 0, light ? (float)h / (f * bh) : 0);
    gl_Uniform1f(g_hdr_a, hdr == 2 ? .55f : hdr ? .35f : 0);
    gl_Uniform1f(g_hdr_g, hdr == 2 ? 1.08f : 1.04f);
    gl_Uniform1f(g_vib, hdr == 2 ? .55f : .35f);
    gl_Uniform1f(g_light, light ? 1 : 0);
    quad();

    /* Pass 2: sharpened from the texture in between into the target. */
    if (sharpen) {
        gl_BindFramebuffer(GL_FRAMEBUFFER, (GLuint)s.fbo);
        gl_BindTexture(GL_TEXTURE_2D, mid_tex);
        gl_UseProgram(sharpen_prog);
        gl_Uniform1i(s_pic, 0);
        gl_Uniform2f(s_texel, 1.f / w, 1.f / h);
        gl_Uniform1f(s_peak, peak);
        quad();
    }
    if (timer) {
        gl_EndQuery(GL_TIME_ELAPSED);
        query_at ^= 1;
        queries_out = 1;
    }
    restore_state(&s);
    SDL_SetRenderTarget(ren, NULL);
    return target;
}

/* ---- Which one -------------------------------------------------------------------------- *
 * Direct3D 11 where SDL renders with it (it presents through a flip-model swap chain, the
 * fast way on Windows), OpenGL where SDL renders with that. */
static bool on_d3d11;

bool gpu_fx_init(SDL_Renderer *ren) {
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(ren, &info) != 0) return false;
#ifdef _WIN32
    if (!strcmp(info.name, "direct3d11")) return on_d3d11 = d3d11_fx_init(ren);
#endif
    return !strcmp(info.name, "opengl") && gl_fx_init(ren);
}

SDL_Texture *gpu_fx_finish(SDL_Renderer *ren, const uint32_t *px, int w, int h, int pitch, const HdPost *p, bool smooth) {
#ifdef _WIN32
    if (on_d3d11) return d3d11_fx_finish(ren, px, w, h, pitch, p, smooth);
#endif
    return gl_fx_finish(ren, px, w, h, pitch, p, smooth);
}
