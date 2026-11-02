/* The finishing effects on the graphics card with Direct3D 11 (gpu.c picks this when SDL's
 * renderer is direct3d11): the same two passes as the OpenGL version, in HLSL.
 *
 * SDL's renderer owns the device and presents with a flip-model swap chain; this borrows
 * its device and immediate context. The picture and the glow go into textures of our own,
 * pass 1 grades and lights into a texture in between, pass 2 sharpens into an SDL target
 * texture (its render target view taken from the context after SDL has bound it). Every
 * piece of pipeline state touched is read first and put back after, so SDL's cached state
 * stays true. The shaders are compiled at start with Windows' own d3dcompiler_47.dll. A
 * pair of timestamp queries measures the card's time, read pictures later, never waited for. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <SDL.h>
#include <SDL_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"

/* Both passes and the triangle that covers the target (no vertex buffer: from the
 * vertex number). */
static const char *hlsl =
    "Texture2D pic : register(t0);\n"
    "Texture2D glow : register(t1);\n"
    "SamplerState pt : register(s0);\n"
    "SamplerState lin : register(s1);\n"
    "cbuffer P : register(b0) { float2 texel; float2 glow_scale; float hdr_a; float hdr_g; float vib; float light; float peak; float3 pad; };\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VO vs(uint id : SV_VertexID) {\n"
    "    VO o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;\n"
    "}\n"
    /* hd_post's steps: the HDR look's S curve and vibrance, then the glow's screen blend. */
    "float4 grade(VO i) : SV_Target {\n"
    "    float3 c = pic.Sample(pt, i.uv).rgb;\n"
    "    if (hdr_a > 0) {\n"
    "        float3 s = c * c * (3 - 2 * c);\n"
    "        c = pow(saturate(c + (s - c) * hdr_a), hdr_g);\n"
    "        float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b)), avg = (c.r + c.g + c.b) / 3;\n"
    "        float sat = mx > 0 ? (mx - mn) / mx : 0;\n"
    "        c = saturate(avg + (c - avg) * (1 + vib * (1 - sat)));\n"
    "    }\n"
    "    if (light > 0) {\n"
    "        float3 l = min(glow.Sample(lin, i.uv * glow_scale).rgb * 4, 1);\n"
    "        c = 1 - (1 - c) * (1 - l);\n"
    "    }\n"
    "    return float4(c, 1);\n"
    "}\n"
    /* hd_sharpen's: AMD's CAS from the four neighbours, flat and gradients left alone. */
    "float4 sharpen(VO i) : SV_Target {\n"
    "    float3 e = pic.Sample(pt, i.uv).rgb;\n"
    "    float3 b = pic.Sample(pt, i.uv - float2(0, texel.y)).rgb, h = pic.Sample(pt, i.uv + float2(0, texel.y)).rgb;\n"
    "    float3 d = pic.Sample(pt, i.uv - float2(texel.x, 0)).rgb, f = pic.Sample(pt, i.uv + float2(texel.x, 0)).rgb;\n"
    "    float3 k = float3(77, 150, 29) / 256;\n"
    "    float le = dot(e, k), lb = dot(b, k), lh = dot(h, k), ld = dot(d, k), lf = dot(f, k);\n"
    "    float mn = min(le, min(min(lb, lh), min(ld, lf))), mx = max(le, max(max(lb, lh), max(ld, lf)));\n"
    "    if (mx - mn > 2.5 / 255) {\n"
    "        float room = min(mn, 1 - mx) / max(mx, 1.0 / 255);\n"
    "        float w = (room > 1 ? 1 : sqrt(room)) * peak;\n"
    "        e = saturate((e + w * (b + h + d + f)) / (1 + 4 * w));\n"
    "    }\n"
    "    return float4(e, 1);\n"
    "}\n";

typedef struct { float texel[2], glow_scale[2], hdr_a, hdr_g, vib, light, peak, pad[3]; } Params;

static bool ready;
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps_grade, *ps_sharpen;
static ID3D11SamplerState *s_point, *s_linear;
static ID3D11Buffer *params;
static ID3D11Texture2D *pic_tex, *glow_tex, *mid_tex;
static ID3D11ShaderResourceView *pic_srv, *glow_srv, *mid_srv;
static ID3D11RenderTargetView *mid_rtv;
static int pic_w, pic_h, glow_w, glow_h;
static SDL_Texture *target;
static int target_w, target_h;
static bool target_smooth;
static uint16_t *glow_px;
/* The card's timer: per picture a disjoint query and two timestamps, in a ring of 3. */
static ID3D11Query *q_disjoint[3], *q_start[3], *q_end[3];
static int q_at, q_count;

static ID3DBlob *compile(pD3DCompile compile_fn, const char *entry, const char *target_model) {
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = compile_fn(hlsl, strlen(hlsl), "gpu_d3d11", NULL, NULL, entry, target_model, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "GPU effects: the %s shader did not build: %s\n", entry, errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
        if (errors) ID3D10Blob_Release(errors);
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);
    return code;
}

bool d3d11_fx_init(SDL_Renderer *ren) {
    if (ready) return true;
    dev = SDL_RenderGetD3D11Device(ren);
    if (!dev) return false;
    ID3D11Device_GetImmediateContext(dev, &ctx);
    HMODULE lib = LoadLibraryA("d3dcompiler_47.dll");
    pD3DCompile compile_fn = lib ? (pD3DCompile)(void *)GetProcAddress(lib, "D3DCompile") : NULL;
    if (!compile_fn) { fprintf(stderr, "GPU effects: d3dcompiler_47.dll is missing\n"); return false; }
    ID3DBlob *b_vs = compile(compile_fn, "vs", "vs_4_0"), *b_grade = compile(compile_fn, "grade", "ps_4_0"),
             *b_sharpen = compile(compile_fn, "sharpen", "ps_4_0");
    bool ok = b_vs && b_grade && b_sharpen &&
              SUCCEEDED(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b_vs), ID3D10Blob_GetBufferSize(b_vs), NULL, &vs)) &&
              SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b_grade), ID3D10Blob_GetBufferSize(b_grade), NULL, &ps_grade)) &&
              SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b_sharpen), ID3D10Blob_GetBufferSize(b_sharpen), NULL, &ps_sharpen));
    if (b_vs) ID3D10Blob_Release(b_vs);
    if (b_grade) ID3D10Blob_Release(b_grade);
    if (b_sharpen) ID3D10Blob_Release(b_sharpen);
    if (!ok) return false;
    D3D11_SAMPLER_DESC sd = { .Filter = D3D11_FILTER_MIN_MAG_MIP_POINT, .AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
                              .AddressV = D3D11_TEXTURE_ADDRESS_CLAMP, .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
                              .ComparisonFunc = D3D11_COMPARISON_NEVER, .MaxLOD = D3D11_FLOAT32_MAX };
    ok = SUCCEEDED(ID3D11Device_CreateSamplerState(dev, &sd, &s_point));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ok = ok && SUCCEEDED(ID3D11Device_CreateSamplerState(dev, &sd, &s_linear));
    D3D11_BUFFER_DESC bd = { .ByteWidth = sizeof(Params), .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_CONSTANT_BUFFER };
    ok = ok && SUCCEEDED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &params));
    if (!ok) return false;
    for (int k = 0; k < 3; k++) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        ID3D11Device_CreateQuery(dev, &qd, &q_disjoint[k]);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        ID3D11Device_CreateQuery(dev, &qd, &q_start[k]);
        ID3D11Device_CreateQuery(dev, &qd, &q_end[k]);
    }
    ready = true;
    return true;
}

static void release(IUnknown **p) { if (*p) { IUnknown_Release(*p); *p = NULL; } }
#define RELEASE(p) release((IUnknown **)&(p))

/* A texture of ours, w x h, with a view for the shaders (and a render target view). */
static bool make_texture(ID3D11Texture2D **t, ID3D11ShaderResourceView **srv, ID3D11RenderTargetView **rtv,
                         int w, int h, DXGI_FORMAT fmt) {
    RELEASE(*t); RELEASE(*srv);
    if (rtv) RELEASE(*rtv);
    D3D11_TEXTURE2D_DESC td = { .Width = (UINT)w, .Height = (UINT)h, .MipLevels = 1, .ArraySize = 1, .Format = fmt,
                                .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT,
                                .BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv ? D3D11_BIND_RENDER_TARGET : 0) };
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, t))) return false;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)*t, NULL, srv))) return false;
    if (rtv && FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)*t, NULL, rtv))) return false;
    return true;
}

/* The pipeline state the passes change, as SDL left it, to put back. */
typedef struct {
    ID3D11InputLayout *layout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11ShaderResourceView *srv[2];
    ID3D11SamplerState *samplers[2];
    ID3D11Buffer *cb;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11BlendState *blend;
    float blend_factor[4];
    UINT sample_mask;
    ID3D11RasterizerState *raster;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nviewports;
} Saved;

static void save_state(Saved *s) {
    ID3D11DeviceContext_IAGetInputLayout(ctx, &s->layout);
    ID3D11DeviceContext_IAGetPrimitiveTopology(ctx, &s->topology);
    ID3D11DeviceContext_VSGetShader(ctx, &s->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(ctx, &s->ps, NULL, NULL);
    ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 2, s->srv);
    ID3D11DeviceContext_PSGetSamplers(ctx, 0, 2, s->samplers);
    ID3D11DeviceContext_PSGetConstantBuffers(ctx, 0, 1, &s->cb);
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &s->rtv, &s->dsv);
    ID3D11DeviceContext_OMGetBlendState(ctx, &s->blend, s->blend_factor, &s->sample_mask);
    ID3D11DeviceContext_RSGetState(ctx, &s->raster);
    s->nviewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetViewports(ctx, &s->nviewports, s->viewports);
}

static void restore_state(Saved *s) {
    ID3D11DeviceContext_IASetInputLayout(ctx, s->layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, s->topology);
    ID3D11DeviceContext_VSSetShader(ctx, s->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, s->ps, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s->rtv, s->dsv);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, s->srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, s->samplers);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &s->cb);
    ID3D11DeviceContext_OMSetBlendState(ctx, s->blend, s->blend_factor, s->sample_mask);
    ID3D11DeviceContext_RSSetState(ctx, s->raster);
    ID3D11DeviceContext_RSSetViewports(ctx, s->nviewports, s->viewports);
    RELEASE(s->layout); RELEASE(s->vs); RELEASE(s->ps); RELEASE(s->srv[0]); RELEASE(s->srv[1]);
    RELEASE(s->samplers[0]); RELEASE(s->samplers[1]); RELEASE(s->cb); RELEASE(s->rtv); RELEASE(s->dsv);
    RELEASE(s->blend); RELEASE(s->raster);
}

/* The card's time for a picture a couple of pictures ago, if it is in. */
static void read_timer(void) {
    if (q_count < 3) return;
    int k = q_at;                           /* the oldest: the slot about to be used again */
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT64 t0, t1;
    if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q_disjoint[k], &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q_start[k], &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q_end[k], &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    if (!dj.Disjoint && dj.Frequency) gpu_fx_card_ms = (double)(t1 - t0) * 1000 / dj.Frequency;
}

SDL_Texture *d3d11_fx_finish(SDL_Renderer *ren, const uint32_t *px, int w, int h, int pitch, const HdPost *p, bool smooth) {
    if (!ready || w <= 0 || h <= 0) return NULL;
    if (!target || target_w != w || target_h != h) {
        if (target) SDL_DestroyTexture(target);
        target = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        target_w = w; target_h = h; target_smooth = !smooth;
        if (!target) return NULL;
    }
    if (target_smooth != smooth) { SDL_SetTextureScaleMode(target, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest); target_smooth = smooth; }
    if (pic_w != w || pic_h != h) {
        if (!make_texture(&pic_tex, &pic_srv, NULL, w, h, DXGI_FORMAT_B8G8R8A8_UNORM) ||
            !make_texture(&mid_tex, &mid_srv, &mid_rtv, w, h, DXGI_FORMAT_B8G8R8A8_UNORM)) { pic_w = 0; return NULL; }
        pic_w = w; pic_h = h;
    }
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
        if (light && (glow_w != bw || glow_h != bh)) {
            if (!make_texture(&glow_tex, &glow_srv, NULL, bw, bh, DXGI_FORMAT_R16G16B16A16_UNORM)) light = false;
            else { glow_w = bw; glow_h = bh; }
        }
    }
    int hdr = p->hdr < 0 ? 0 : p->hdr > 2 ? 2 : p->hdr;
    float peak = hd_sharpen_peak(p->sharpen);

    /* SDL binds a target's view when it draws into it: one black fill (the passes cover it
     * all) and the view is there to take. */
    if (SDL_SetRenderTarget(ren, target) != 0) return NULL;
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderFillRect(ren, NULL);
    SDL_RenderFlush(ren);
    Saved s;
    memset(&s, 0, sizeof s);
    save_state(&s);
    if (!s.rtv) { restore_state(&s); SDL_SetRenderTarget(ren, NULL); return NULL; }

    read_timer();
    Uint64 t0 = SDL_GetPerformanceCounter();
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)pic_tex, 0, NULL, px, (UINT)pitch * 4, 0);
    if (light) ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)glow_tex, 0, NULL, glow_px, (UINT)bw * 8, 0);
    gpu_fx_upload_ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000 / SDL_GetPerformanceFrequency();

    Params prm = { { 1.f / w, 1.f / h }, { light ? (float)w / (f * bw) : 0, light ? (float)h / (f * bh) : 0 },
                   hdr == 2 ? .55f : hdr ? .35f : 0, hdr == 2 ? 1.08f : 1.04f, hdr == 2 ? .55f : .35f, light ? 1.f : 0.f, peak, { 0, 0, 0 } };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)params, 0, NULL, &prm, 0, 0);

    int q = q_at;
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)q_disjoint[q]);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q_start[q]);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_RSSetState(ctx, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11SamplerState *samplers[2] = { s_point, s_linear };
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, samplers);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &params);

    /* Pass 1: graded and lit, into the texture in between (or the target if nothing is
     * sharpened). */
    bool sharpen = peak < 0;
    ID3D11ShaderResourceView *none[2] = { NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, sharpen ? &mid_rtv : &s.rtv, NULL);
    ID3D11ShaderResourceView *in1[2] = { pic_srv, light ? glow_srv : NULL };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, in1);
    ID3D11DeviceContext_PSSetShader(ctx, ps_grade, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    /* Pass 2: sharpened from there into the target. */
    if (sharpen) {
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s.rtv, NULL);
        ID3D11ShaderResourceView *in2[2] = { mid_srv, NULL };
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, in2);
        ID3D11DeviceContext_PSSetShader(ctx, ps_sharpen, NULL, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
    }
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q_end[q]);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q_disjoint[q]);
    q_at = (q_at + 1) % 3;
    if (q_count < 3) q_count++;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);
    restore_state(&s);
    SDL_SetRenderTarget(ren, NULL);
    return target;
}
