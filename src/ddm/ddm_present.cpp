#include "ddm_present.h"
#include "../config.h"
#include "../logger.h"
#include "../render_ddraw.h"
#include "../settings_menu.h"
#include "../test_harness.h"
#include "../ui_scale.h"
#include "../view_zoom.h"
#include "ddm_hd.h"
#include "ddm_hd_menu.h"
#include "ddm_hd_patch.h"

#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace DDM::Present {
namespace {
    std::mutex g_lock;
    HWND g_hwnd = nullptr;

    enum class Backend { None, D3D11, GDI };
    Backend g_backend = Backend::None;
    bool g_initTried = false;

    // ---- Direct3D 11 state
    ID3D11Device*           g_dev = nullptr;
    ID3D11DeviceContext*    g_ctx = nullptr;
    IDXGISwapChain1*        g_swap = nullptr;
    ID3D11RenderTargetView* g_rtv = nullptr;
    ID3D11VertexShader*     g_vs = nullptr;
    ID3D11PixelShader*      g_ps = nullptr;
    ID3D11SamplerState*     g_smpPoint = nullptr;
    ID3D11SamplerState*     g_smpLinear = nullptr;
    ID3D11Buffer*           g_cb = nullptr;
    ID3D11Texture2D*        g_tex = nullptr;
    ID3D11ShaderResourceView* g_srv = nullptr;
    int  g_texW = 0, g_texH = 0;
    bool g_tex565 = false;
    bool g_has565 = false;
    bool g_tearing = false;
    UINT g_bbW = 0, g_bbH = 0;

    struct ShaderConsts {
        float texW, texH;
        float outW, outH;
        float mode;      // 0 = nearest, 1 = sharp bilinear, 2 = bilinear
        float layer;     // 1 = HD menu pictures (ddm_hd_menu.h) in t1 (originals) and t2 (HD)
                         // 2 = a mission's dialog: its backdrop (t3) at its own size
                         // 3 = smooth zoom (view_zoom.h): the world (t4) magnified, the interface not
        float cropX, cropY;                  // where the texture's top-left pixel lies in the frame
        float fullX, fullY, fullW, fullH;    // the whole frame's rectangle on the output (layer 2)
        float lupaM, lupaWorldH;             // layer 3: magnification, frame rows that hold the world
        float lupaX, lupaY;                  // layer 3: frame position at the output's top-left corner
        float patchOn, pad0, pad1, pad2;     // menus: HD patches (t6 frame pixel it belongs to, t7 HD) over the layers
    };

    // the world before the mission's windows were drawn (layer 3), same format as the frame texture
    ID3D11Texture2D*          g_snapTex = nullptr;
    ID3D11ShaderResourceView* g_snapSrv = nullptr;
    int g_snapW = 0, g_snapH = 0;

    // backdrop of a dialog in a mission (same format as the frame texture, frame-sized); the shown
    // version (the world magnified as the smooth zoom showed it) in a second texture
    ID3D11Texture2D*          g_backdropTex = nullptr;
    ID3D11ShaderResourceView* g_backdropSrv = nullptr;
    ID3D11Texture2D*          g_backShowTex = nullptr;
    ID3D11ShaderResourceView* g_backShowSrv = nullptr;
    bool                      g_backShowOn = false;
    int      g_backdropW = 0, g_backdropH = 0;
    unsigned g_backdropGen = 0;
    std::vector<uint32_t> g_backdropRow8888;

    // HD menu layer: the original picture pixels (R32_UINT, 0 = none) and their HD version (2x)
    ID3D11Texture2D*          g_layerOrig = nullptr;
    ID3D11ShaderResourceView* g_layerOrigSrv = nullptr;
    ID3D11Texture2D*          g_layerHd = nullptr;
    ID3D11ShaderResourceView* g_layerHdSrv = nullptr;
    int      g_layerW = 0, g_layerH = 0;
    unsigned g_layerGen = 0;

    const char kVS[] =
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut main(uint id : SV_VertexID) {\n"
        "  VSOut o; float2 t = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = t; o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "  return o; }\n";

    // Sharp bilinear: pixels are pre-scaled by the largest whole factor and only the remaining
    // fraction is filtered, so the picture stays crisp at any output size without shimmer.
    // HD menu layer: where the frame still holds the original pixel of a menu picture (same
    // RGB565 value as recorded), the HD picture is shown instead.
    // Layer 2 (a dialog in a mission): where the enlarged frame shows a pixel of the dialog's
    // backdrop, the backdrop is shown at its own size instead - only the dialog is enlarged.
    // Layer 3 (smooth zoom): where the frame pixel equals the world as it was before the windows
    // were drawn, the output shows the world magnified (from the copy, so the interface is not
    // magnified with it); elsewhere the frame pixel itself - the interface at its place.
    const char kPS[] =
        "Texture2D tex : register(t0);\n"
        "Texture2D<uint> layerOrig : register(t1);\n"
        "Texture2D layerHd : register(t2);\n"
        "Texture2D backdrop : register(t3);\n"
        "Texture2D world : register(t4);\n"
        "Texture2D backShow : register(t5);\n"
        "Texture2D<uint> patchRef : register(t6);\n"
        "Texture2D patchHd : register(t7);\n"
        "SamplerState smpPoint : register(s0);\n"
        "SamplerState smpLinear : register(s1);\n"
        "cbuffer C : register(b0) { float2 texSize; float2 outSize; float mode; float layer; float2 cropOrigin; float4 full; float4 lupa; float4 extra; };\n"
        "uint Pack565(float3 c) {\n"
        "  return ((((uint)(c.r * 255.0 + 0.5)) >> 3) << 11) | ((((uint)(c.g * 255.0 + 0.5)) >> 2) << 5) | (((uint)(c.b * 255.0 + 0.5)) >> 3);\n"
        "}\n"
        // layer 3: is this frame pixel the world (as copied before the windows were drawn)?
        "bool WorldAt(int2 p) {\n"
        "  p = clamp(p, int2(0, 0), int2(texSize) - 1);\n"
        "  return Pack565(tex.Load(int3(p, 0)).rgb) == Pack565(world.Load(int3(p, 0)).rgb);\n"
        "}\n"
        // layer 2: does the frame show the dialog's backdrop here (the darkened level)?
        "bool BackAt(int2 p) {\n"
        "  p = clamp(p, int2(0, 0), int2(texSize) - 1);\n"
        "  float4 b = backdrop.Load(int3(p + int2(cropOrigin), 0));\n"
        "  return b.a > 0.5 && Pack565(tex.Load(int3(p, 0)).rgb) == Pack565(b.rgb);\n"
        "}\n"
        // layer 1: does the frame show the menu picture here (the original the HD picture replaces)?
        "bool PictureAt(int2 p) {\n"
        "  p = clamp(p, int2(0, 0), int2(texSize) - 1);\n"
        "  uint o = layerOrig.Load(int3(p, 0));\n"
        "  return o != 0 && Pack565(tex.Load(int3(p, 0)).rgb) == (o & 0xFFFF);\n"
        "}\n"
        // The three layers decide per pixel whether the frame shows the picture they replace. A
        // pixel of a translucent picture drawn over it (shadows, darkened boxes, soft edges) can
        // come out equal by chance (black stays black): alone among such pixels it showed a dot
        // of the replacement (user report 2026-11-15: spots in the bottom bar and in the menus).
        // So a match counts only when at most four of its eight neighbours do not match.
        "#define FEW_OTHERS(fn, t) (CountNot_##fn(t) <= 4)\n"
        "int CountNot_WorldAt(int2 t) { int n = 0; [unroll] for (int dy = -1; dy <= 1; ++dy) [unroll] for (int dx = -1; dx <= 1; ++dx) if ((dx != 0 || dy != 0) && !WorldAt(t + int2(dx, dy))) ++n; return n; }\n"
        "int CountNot_BackAt(int2 t) { int n = 0; [unroll] for (int dy = -1; dy <= 1; ++dy) [unroll] for (int dx = -1; dx <= 1; ++dx) if ((dx != 0 || dy != 0) && !BackAt(t + int2(dx, dy))) ++n; return n; }\n"
        "int CountNot_PictureAt(int2 t) { int n = 0; [unroll] for (int dy = -1; dy <= 1; ++dy) [unroll] for (int dx = -1; dx <= 1; ++dx) if ((dx != 0 || dy != 0) && !PictureAt(t + int2(dx, dy))) ++n; return n; }\n"
        // menus (ddm_hd_patch.h): does the frame still hold the pixel an HD patch was made for? Letters
        // are thin - only neighbours that have a patch and no longer match count against it (a lone
        // match among changed patch pixels is chance)
        "int PatchState(int2 p) {\n"
        "  p = clamp(p, int2(0, 0), int2(texSize) - 1);\n"
        "  uint o = patchRef.Load(int3(p, 0));\n"
        "  if (o == 0) return 0;\n"
        "  return Pack565(tex.Load(int3(p, 0)).rgb) == (o & 0xFFFF) ? 1 : 2;\n"
        "}\n"
        "int CountPatchOther(int2 t) { int n = 0; [unroll] for (int dy = -1; dy <= 1; ++dy) [unroll] for (int dx = -1; dx <= 1; ++dx) if ((dx != 0 || dy != 0) && PatchState(t + int2(dx, dy)) == 2) ++n; return n; }\n"
        "float3 Filtered(Texture2D t, float2 texel, float2 scale) {\n"
        "  if (mode < 0.5) return t.Sample(smpPoint, texel / texSize).rgb;\n"
        "  if (mode > 1.5) return t.Sample(smpLinear, texel / texSize).rgb;\n"
        "  float2 s = max(floor(scale), 1.0);\n"
        "  float2 i = floor(texel); float2 f = texel - i;\n"
        "  float2 region = 0.5 - 0.5 / s;\n"
        "  float2 c = f - 0.5;\n"
        "  float2 ff = (c - clamp(c, -region, region)) * s + 0.5;\n"
        "  return t.Sample(smpLinear, (i + ff) / texSize).rgb;\n"
        "}\n"
        // Catmull-Rom (16 texels): the smooth zoom between 1x and 2x magnifies the world the engine drew
        // at 1.0 - bilinear made walls, animals and cloaks soft (user report 2026-11-15); this keeps edges
        // and texture as sharp as an upscale without new detail can (overshoot clipped).
        "float3 CatRom(Texture2D t, float2 texel) {\n"
        "  float2 p = texel - 0.5; float2 i = floor(p); float2 f = p - i;\n"
        "  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
        "  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
        "  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
        "  float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
        "  float wx[4] = { w0.x, w1.x, w2.x, w3.x }; float wy[4] = { w0.y, w1.y, w2.y, w3.y };\n"
        "  float3 acc = 0.0; int2 hi = int2(texSize) - 1;\n"
        "  [unroll] for (int y = 0; y < 4; ++y) { [unroll] for (int x = 0; x < 4; ++x) {\n"
        "    int2 q = clamp(int2(i) + int2(x - 1, y - 1), int2(0, 0), hi);\n"
        "    acc += t.Load(int3(q, 0)).rgb * (wx[x] * wy[y]); } }\n"
        "  return saturate(acc);\n"
        "}\n"
        "float3 WorldMagnified(float2 texel, float m) {\n"
        "  if (mode >= 0.5 && m > 1.001 && m < 1.999) return CatRom(world, texel);\n"
        "  return Filtered(world, texel, outSize / texSize * m);\n"
        "}\n"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  if (extra.x > 0.5) {\n"
        "    int2 t = int2(min(floor(uv * texSize), texSize - 1.0));\n"
        "    if (PatchState(t) == 1 && CountPatchOther(t) <= 4) {\n"
        // inside the pixel's own 2x2 block (its neighbours may hold other things)
        "      float2 hp = clamp(uv * texSize * 2.0, float2(t) * 2.0 + 0.5, float2(t) * 2.0 + 1.5);\n"
        "      return float4(patchHd.Sample(smpLinear, hp / (texSize * 2.0)).rgb, 1.0);\n"
        "    }\n"
        "  }\n"
        "  if (layer > 2.5) {\n"
        "    float2 fp = uv * texSize;\n"
        "    if (fp.y < lupa.y) {\n"
        "      int2 t = int2(min(floor(fp), texSize - 1.0));\n"
        "      if (WorldAt(t) && FEW_OTHERS(WorldAt, t)) {\n"
        "        float2 wp = lupa.zw + fp / lupa.x;\n"
        "        return float4(WorldMagnified(wp, lupa.x), 1.0);\n"
        "      }\n"
        "    }\n"
        "  } else if (layer > 1.5) {\n"
        "    int2 t = int2(min(floor(uv * texSize), texSize - 1.0));\n"
        "    if (BackAt(t) && FEW_OTHERS(BackAt, t)) {\n"
        "      float2 u = (pos.xy - full.xy) / full.zw;\n"
        "      if (lupa.x > 0.5) return float4(backShow.Sample(smpLinear, u).rgb, 1.0);\n"
        "      return float4(backdrop.Sample(smpLinear, u).rgb, 1.0);\n"
        "    }\n"
        "  } else if (layer > 0.5) {\n"
        "    int2 t = int2(min(floor(uv * texSize), texSize - 1.0));\n"
        "    if (PictureAt(t) && FEW_OTHERS(PictureAt, t)) return float4(layerHd.Sample(smpLinear, uv).rgb, 1.0);\n"
        "  }\n"
        "  if (mode < 0.5) return float4(tex.Sample(smpPoint, uv).rgb, 1.0);\n"
        "  if (mode > 1.5) return float4(tex.Sample(smpLinear, uv).rgb, 1.0);\n"
        "  float2 texel = uv * texSize;\n"
        "  float2 scale = max(floor(outSize / texSize), 1.0);\n"
        "  float2 i = floor(texel); float2 f = texel - i;\n"
        "  float2 region = 0.5 - 0.5 / scale;\n"
        "  float2 c = f - 0.5;\n"
        "  float2 ff = (c - clamp(c, -region, region)) * scale + 0.5;\n"
        "  return float4(tex.Sample(smpLinear, (i + ff) / texSize).rgb, 1.0);\n"
        "}\n";

    template <typename T>
    void SafeRelease(T*& p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }

    // ---- frame dumps for the background test mode (24-bit BMP, bottom-up)
    const std::string* g_capturePath = nullptr; // scaled-output dump wanted from the next D3D11 frame

    template <typename RowFn>
    bool WriteBmp24(const std::string& path, int w, int h, RowFn&& fillRowBgr) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const uint32_t rowBytes = (static_cast<uint32_t>(w) * 3u + 3u) & ~3u;
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + rowBytes * static_cast<uint32_t>(h);
        ih.biSize = sizeof(ih);
        ih.biWidth = w;
        ih.biHeight = h;
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        std::fwrite(&fh, sizeof(fh), 1, f);
        std::fwrite(&ih, sizeof(ih), 1, f);
        std::vector<uint8_t> row(rowBytes, 0);
        for (int y = h - 1; y >= 0; --y) {
            fillRowBgr(y, row.data());
            std::fwrite(row.data(), 1, rowBytes, f);
        }
        std::fclose(f);
        return true;
    }

    bool SaveFrame565(const std::string& path, const uint8_t* bits, int pitch, int w, int h) {
        return WriteBmp24(path, w, h, [&](int y, uint8_t* out) {
            const auto* src = reinterpret_cast<const uint16_t*>(bits + static_cast<size_t>(y) * pitch);
            for (int x = 0; x < w; ++x) {
                const uint16_t p = src[x];
                out[x * 3 + 0] = static_cast<uint8_t>(((p & 0x1F) * 527 + 23) >> 6);         // B
                out[x * 3 + 1] = static_cast<uint8_t>((((p >> 5) & 0x3F) * 259 + 33) >> 6);   // G
                out[x * 3 + 2] = static_cast<uint8_t>((((p >> 11) & 0x1F) * 527 + 23) >> 6);  // R
            }
        });
    }

    bool SaveBackBuffer(const std::string& path) {
        ID3D11Texture2D* bb = nullptr;
        if (!g_swap || FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) {
            return false;
        }
        D3D11_TEXTURE2D_DESC td{};
        bb->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        bool ok = false;
        if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &staging))) {
            g_ctx->CopyResource(staging, bb);
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
                const auto* data = static_cast<const uint8_t*>(map.pData);
                ok = WriteBmp24(path, static_cast<int>(td.Width), static_cast<int>(td.Height), [&](int y, uint8_t* out) {
                    const uint8_t* src = data + static_cast<size_t>(y) * map.RowPitch; // B8G8R8A8
                    for (UINT x = 0; x < td.Width; ++x) {
                        out[x * 3 + 0] = src[x * 4 + 0];
                        out[x * 3 + 1] = src[x * 4 + 1];
                        out[x * 3 + 2] = src[x * 4 + 2];
                    }
                });
                g_ctx->Unmap(staging, 0);
            }
            staging->Release();
        }
        bb->Release();
        return ok;
    }

    void ReleaseFrameTextures();

    void ReleaseBackdrop() {
        SafeRelease(g_backdropSrv);
        SafeRelease(g_backdropTex);
        SafeRelease(g_backShowSrv);
        SafeRelease(g_backShowTex);
        g_backShowOn = false;
        g_backdropW = g_backdropH = 0;
        g_backdropGen = 0;
    }

    // the backdrop of a mission's dialog in a texture like the frame's; true = use it
    bool PrepareBackdrop(int frameW, int frameH) {
        DDM::HD::Menu::Backdrop bd;
        if (!DDM::HD::Menu::GetBackdrop(bd) || bd.w != frameW || bd.h != frameH) return false;
        if (!g_backdropTex || g_backdropW != bd.w || g_backdropH != bd.h) {
            ReleaseBackdrop();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(bd.w);
            td.Height = static_cast<UINT>(bd.h);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // alpha: level (1) or dialog picture (0)
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_backdropTex)) ||
                FAILED(g_dev->CreateShaderResourceView(g_backdropTex, nullptr, &g_backdropSrv)) ||
                FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_backShowTex)) ||
                FAILED(g_dev->CreateShaderResourceView(g_backShowTex, nullptr, &g_backShowSrv))) {
                ReleaseBackdrop();
                return false;
            }
            g_backdropW = bd.w;
            g_backdropH = bd.h;
        }
        if (g_backdropGen != bd.generation) {
            g_ctx->UpdateSubresource(g_backdropTex, 0, nullptr, bd.bgra, static_cast<UINT>(bd.w * 4), 0);
            g_backShowOn = bd.show != nullptr;
            if (g_backShowOn) g_ctx->UpdateSubresource(g_backShowTex, 0, nullptr, bd.show, static_cast<UINT>(bd.w * 4), 0);
            g_backdropGen = bd.generation;
        }
        return true;
    }

    bool g_snapValid = false;            // the world texture holds this frame's world
    std::vector<uint32_t> g_snap8888;    // conversion buffer when the card has no RGB565 textures

    void ReleaseSnap() {
        SafeRelease(g_snapSrv);
        SafeRelease(g_snapTex);
        g_snapW = g_snapH = 0;
        g_snapValid = false;
    }

    // the world texture of the smooth zoom (filled by UploadWorld straight from the back buffer)
    bool EnsureSnap(int w, int h) {
        if (g_snapTex && g_snapW == w && g_snapH == h) return true;
        ReleaseSnap();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = g_has565 ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_snapTex)) ||
            FAILED(g_dev->CreateShaderResourceView(g_snapTex, nullptr, &g_snapSrv))) {
            ReleaseSnap();
            return false;
        }
        g_snapW = w;
        g_snapH = h;
        g_snapValid = false;
        Logger::Info("Present: smooth zoom world texture %dx%d.", w, h);
        return true;
    }

    void ReleaseLayer() {
        SafeRelease(g_layerOrigSrv);
        SafeRelease(g_layerOrig);
        SafeRelease(g_layerHdSrv);
        SafeRelease(g_layerHd);
        g_layerW = g_layerH = 0;
        g_layerGen = 0;
    }

    bool EnsureLayer(int w, int h) {
        if (g_layerOrig && g_layerHd && g_layerW == w && g_layerH == h) return true;
        ReleaseLayer();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_UINT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bool ok = SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_layerOrig)) &&
                  SUCCEEDED(g_dev->CreateShaderResourceView(g_layerOrig, nullptr, &g_layerOrigSrv));
        td.Width = static_cast<UINT>(w * 2);
        td.Height = static_cast<UINT>(h * 2);
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_layerHd)) &&
             SUCCEEDED(g_dev->CreateShaderResourceView(g_layerHd, nullptr, &g_layerHdSrv));
        if (!ok) {
            ReleaseLayer();
            return false;
        }
        g_layerW = w;
        g_layerH = h;
        g_layerGen = 0;     // everything is uploaded next
        Logger::Info("Present: HD menu layer %dx%d (HD %dx%d).", w, h, w * 2, h * 2);
        return true;
    }

    // the HD menu layer for this frame (crop of w x h): upload what changed; true = use it
    bool PrepareLayer(const RECT& crop, int w, int h) {
        DDM::HD::Menu::Layer layer;
        if (!DDM::HD::Menu::GetLayer(crop, layer) || layer.w != w || layer.h != h || !EnsureLayer(w, h)) return false;
        if (layer.generation != g_layerGen) {
            g_ctx->UpdateSubresource(g_layerOrig, 0, nullptr, layer.orig, static_cast<UINT>(w * 4), 0);
            g_ctx->UpdateSubresource(g_layerHd, 0, nullptr, layer.hd, static_cast<UINT>(w * 2 * 4), 0);
            g_layerGen = layer.generation;
        } else if (layer.dirty.right > layer.dirty.left && layer.dirty.bottom > layer.dirty.top) {
            const RECT& d = layer.dirty;
            const D3D11_BOX box1{ static_cast<UINT>(d.left), static_cast<UINT>(d.top), 0, static_cast<UINT>(d.right), static_cast<UINT>(d.bottom), 1 };
            g_ctx->UpdateSubresource(g_layerOrig, 0, &box1, layer.orig + static_cast<size_t>(d.top) * w + d.left, static_cast<UINT>(w * 4), 0);
            const D3D11_BOX box2{ static_cast<UINT>(d.left * 2), static_cast<UINT>(d.top * 2), 0,
                                  static_cast<UINT>(d.right * 2), static_cast<UINT>(d.bottom * 2), 1 };
            g_ctx->UpdateSubresource(g_layerHd, 0, &box2, layer.hd + static_cast<size_t>(d.top * 2) * (w * 2) + d.left * 2,
                                     static_cast<UINT>(w * 2 * 4), 0);
        }
        return true;
    }

    // HD patches of menus and dialogs (ddm_hd_patch.h): one pair of textures per presented storage
    // (the flip chain alternates two), like the HD menu layer
    struct PatchSlot {
        ID3D11Texture2D* ref = nullptr;
        ID3D11ShaderResourceView* refSrv = nullptr;
        ID3D11Texture2D* hd = nullptr;
        ID3D11ShaderResourceView* hdSrv = nullptr;
        int w = 0, h = 0;
        uintptr_t identity = 0;
        unsigned lastUse = 0;
    };
    PatchSlot g_patch[2];
    unsigned  g_patchUse = 0;

    void ReleasePatch(PatchSlot& s) {
        SafeRelease(s.refSrv);
        SafeRelease(s.ref);
        SafeRelease(s.hdSrv);
        SafeRelease(s.hd);
        s.w = s.h = 0;
        s.identity = 0;
    }

    // the shadow of the presented storage `frameBits` over `crop` (w x h): uploaded; nullptr = none
    PatchSlot* PreparePatch(const uint8_t* frameBits, const RECT& crop, int w, int h) {
        DDM::HD::Menu::Layer layer;
        uintptr_t identity = 0;
        if (!DDM::HD::Patch::GetPatch(frameBits, crop, layer, identity) || layer.w != w || layer.h != h) return nullptr;
        PatchSlot* slot = nullptr;
        for (auto& s : g_patch) {
            if (s.identity == identity && s.w == w && s.h == h && s.ref) slot = &s;
        }
        bool full = false;
        if (!slot) {
            slot = g_patch[0].lastUse <= g_patch[1].lastUse ? &g_patch[0] : &g_patch[1];
            if (!slot->ref || slot->w != w || slot->h != h) {
                ReleasePatch(*slot);
                D3D11_TEXTURE2D_DESC td{};
                td.Width = static_cast<UINT>(w);
                td.Height = static_cast<UINT>(h);
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R32_UINT;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                bool ok = SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &slot->ref)) &&
                          SUCCEEDED(g_dev->CreateShaderResourceView(slot->ref, nullptr, &slot->refSrv));
                td.Width = static_cast<UINT>(w * 2);
                td.Height = static_cast<UINT>(h * 2);
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &slot->hd)) &&
                     SUCCEEDED(g_dev->CreateShaderResourceView(slot->hd, nullptr, &slot->hdSrv));
                if (!ok) {
                    ReleasePatch(*slot);
                    return nullptr;
                }
                slot->w = w;
                slot->h = h;
                Logger::Info("Present: HD patch textures %dx%d (HD %dx%d).", w, h, w * 2, h * 2);
            }
            slot->identity = identity;
            full = true;
        }
        slot->lastUse = ++g_patchUse;
        // a new shadow: all of its refs (they clear what an earlier one left in the texture), of the HD
        // part only what was drawn (the rest is never shown: its refs are 0)
        if (full) g_ctx->UpdateSubresource(slot->ref, 0, nullptr, layer.orig, static_cast<UINT>(w * 4), 0);
        if (layer.dirty.right > layer.dirty.left && layer.dirty.bottom > layer.dirty.top) {
            const RECT& d = layer.dirty;
            if (!full) {
                const D3D11_BOX box1{ static_cast<UINT>(d.left), static_cast<UINT>(d.top), 0, static_cast<UINT>(d.right), static_cast<UINT>(d.bottom), 1 };
                g_ctx->UpdateSubresource(slot->ref, 0, &box1, layer.orig + static_cast<size_t>(d.top) * w + d.left, static_cast<UINT>(w * 4), 0);
            }
            const D3D11_BOX box2{ static_cast<UINT>(d.left * 2), static_cast<UINT>(d.top * 2), 0,
                                  static_cast<UINT>(d.right * 2), static_cast<UINT>(d.bottom * 2), 1 };
            g_ctx->UpdateSubresource(slot->hd, 0, &box2, layer.hd + static_cast<size_t>(d.top * 2) * (w * 2) + d.left * 2,
                                     static_cast<UINT>(w * 2 * 4), 0);
        }
        return slot;
    }

    void ReleaseD3D() {
        ReleasePatch(g_patch[0]);
        ReleasePatch(g_patch[1]);
        ReleaseLayer();
        ReleaseBackdrop();
        ReleaseSnap();
        ReleaseFrameTextures();
        SafeRelease(g_cb);
        SafeRelease(g_smpLinear);
        SafeRelease(g_smpPoint);
        SafeRelease(g_ps);
        SafeRelease(g_vs);
        SafeRelease(g_rtv);
        SafeRelease(g_swap);
        SafeRelease(g_ctx);
        SafeRelease(g_dev);
        g_texW = g_texH = 0;
    }

    bool CreateBackbufferView() {
        SafeRelease(g_rtv);
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) {
            return false;
        }
        D3D11_TEXTURE2D_DESC td{};
        bb->GetDesc(&td);
        g_bbW = td.Width;
        g_bbH = td.Height;
        const HRESULT hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
        return SUCCEEDED(hr);
    }

    bool InitD3D11() {
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3,
                                       D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
        if (FAILED(hr)) {
            Logger::Warn("Present: D3D11CreateDevice failed (0x%08X).", static_cast<unsigned>(hr));
            return false;
        }

        // Queue at most one frame: with VSync on, Present then blocks until the previous frame
        // has reached the screen, which paces the game loop and keeps latency at one refresh.
        IDXGIDevice1* dxgiDev1 = nullptr;
        if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgiDev1)))) {
            dxgiDev1->SetMaximumFrameLatency(1);
            dxgiDev1->Release();
        }

        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        if (FAILED(g_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) ||
            FAILED(dxgiDev->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)))) {
            SafeRelease(adapter);
            SafeRelease(dxgiDev);
            Logger::Warn("Present: DXGI 1.2 factory not available.");
            return false;
        }

        const auto& cfg = SherwoodConfig::Get();
        IDXGIFactory5* factory5 = nullptr;
        if (cfg.allowTearing && SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5)))) {
            BOOL allow = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) {
                g_tearing = (allow != FALSE);
            }
            factory5->Release();
        }

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 0;  // use the window's client size
        sd.Height = 0;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        hr = factory->CreateSwapChainForHwnd(g_dev, g_hwnd, &sd, nullptr, nullptr, &g_swap);
        if (SUCCEEDED(hr)) {
            factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        }
        factory->Release();
        adapter->Release();
        dxgiDev->Release();
        if (FAILED(hr)) {
            Logger::Warn("Present: CreateSwapChainForHwnd failed (0x%08X).", static_cast<unsigned>(hr));
            return false;
        }
        if (!CreateBackbufferView()) {
            Logger::Warn("Present: could not create the back buffer view.");
            return false;
        }

        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        ID3DBlob* err = nullptr;
        hr = D3DCompile(kVS, sizeof(kVS) - 1, "vs", nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, &err);
        if (SUCCEEDED(hr)) {
            SafeRelease(err);
            hr = D3DCompile(kPS, sizeof(kPS) - 1, "ps", nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, &err);
        }
        if (FAILED(hr)) {
            Logger::Warn("Present: shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            SafeRelease(err);
            SafeRelease(vsBlob);
            SafeRelease(psBlob);
            return false;
        }
        hr = g_dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
        if (SUCCEEDED(hr)) {
            hr = g_dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);
        }
        vsBlob->Release();
        psBlob->Release();
        if (FAILED(hr)) {
            Logger::Warn("Present: shader creation failed (0x%08X).", static_cast<unsigned>(hr));
            return false;
        }

        D3D11_SAMPLER_DESC smp{};
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        g_dev->CreateSamplerState(&smp, &g_smpPoint);
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        g_dev->CreateSamplerState(&smp, &g_smpLinear);

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(ShaderConsts);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_cb)) || !g_smpPoint || !g_smpLinear) {
            Logger::Warn("Present: state object creation failed.");
            return false;
        }

        UINT support = 0;
        g_has565 = SUCCEEDED(g_dev->CheckFormatSupport(DXGI_FORMAT_B5G6R5_UNORM, &support)) &&
                   (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) && (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);

        Logger::Info("Present: Direct3D 11 ready (feature level 0x%04X, back buffer %ux%u, RGB565 textures=%d, tearing=%d).",
                     static_cast<unsigned>(got), g_bbW, g_bbH, g_has565 ? 1 : 0, g_tearing ? 1 : 0);
        return true;
    }

    // Frame textures of two sizes are kept: the menu zoom shows a part of the frame, and switching
    // between it and the whole frame (in-game menu: Esc) must not create a texture each time.
    struct FrameTexture {
        ID3D11Texture2D*          tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
        uint64_t lastUse = 0;
    };
    FrameTexture g_frameTex[2];
    uint64_t     g_frameUse = 0;

    void ReleaseFrameTextures() {
        for (FrameTexture& f : g_frameTex) {
            SafeRelease(f.srv);
            SafeRelease(f.tex);
            f.w = f.h = 0;
        }
        g_tex = nullptr;
        g_srv = nullptr;
        g_texW = g_texH = 0;
    }

    bool EnsureTexture(int w, int h) {
        ++g_frameUse;
        FrameTexture* slot = nullptr;
        for (FrameTexture& f : g_frameTex) {
            if (f.tex && f.w == w && f.h == h) slot = &f;
        }
        if (!slot) {
            slot = &g_frameTex[0];
            for (FrameTexture& f : g_frameTex) {
                if (!f.tex) { slot = &f; break; }
                if (f.lastUse < slot->lastUse) slot = &f;
            }
            SafeRelease(slot->srv);
            SafeRelease(slot->tex);
            slot->w = slot->h = 0;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(w);
            td.Height = static_cast<UINT>(h);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = g_has565 ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &slot->tex)) ||
                FAILED(g_dev->CreateShaderResourceView(slot->tex, nullptr, &slot->srv))) {
                SafeRelease(slot->tex);
                return false;
            }
            slot->w = w;
            slot->h = h;
            Logger::Info("Present: frame texture %dx%d (%s).", w, h, g_has565 ? "RGB565" : "BGRX8888");
        }
        slot->lastUse = g_frameUse;
        g_tex = slot->tex;
        g_srv = slot->srv;
        g_tex565 = g_has565;
        g_texW = w;
        g_texH = h;
        return true;
    }

    // Largest rectangle with the frame's aspect ratio that fits the output.
    void FitRect(int w, int h, int outW, int outH, int& x, int& y, int& cw, int& ch) {
        if (!SherwoodConfig::Get().fixAspectRatio) {
            x = 0; y = 0; cw = outW; ch = outH;
            return;
        }
        const long long lhs = static_cast<long long>(outW) * h;
        const long long rhs = static_cast<long long>(outH) * w;
        if (lhs > rhs) {            // output is wider: pillarbox
            ch = outH;
            cw = static_cast<int>(rhs / h);
        } else {                    // output is taller: letterbox
            cw = outW;
            ch = static_cast<int>(lhs / w);
        }
        x = (outW - cw) / 2;
        y = (outH - ch) / 2;
    }

    bool FrameD3D11(const uint8_t* bits, int pitch, int w, int h) {
        RECT rc{};
        GetClientRect(g_hwnd, &rc);
        const UINT cw = static_cast<UINT>(rc.right - rc.left);
        const UINT chh = static_cast<UINT>(rc.bottom - rc.top);
        if (cw == 0 || chh == 0) {
            return true; // minimised
        }
        if (cw != g_bbW || chh != g_bbH) {
            g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
            SafeRelease(g_rtv);
            if (FAILED(g_swap->ResizeBuffers(0, cw, chh, DXGI_FORMAT_UNKNOWN,
                                             g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)) ||
                !CreateBackbufferView()) {
                return false;
            }
        }
        // menu zoom: only the middle part of the frame (menus and dialogs laid out for 1024x768)
        RECT crop{};
        bool layerOn = false;
        bool backdropOn = false;
        bool lupaOn = false;
        PatchSlot* patch = nullptr;
        const int frameW = w, frameH = h;
        const uint8_t* const frameBits = bits;
        ViewZoom::View lupa;
        const bool lupaFrame = ViewZoom::TakeView(lupa);           // the mission's world, magnified
        if (UiScale::MenuZoomCrop(w, h, crop)) {
            bits += static_cast<size_t>(crop.top) * pitch + static_cast<size_t>(crop.left) * 2;
            w = crop.right - crop.left;
            h = crop.bottom - crop.top;
            const unsigned dbg = DDM::HD::DebugFlags();              // test harness: 64/128/256 leave a layer out
            backdropOn = !(dbg & 256u) && PrepareBackdrop(frameW, frameH);           // a mission's dialog: only it enlarged
            layerOn = !backdropOn && !(dbg & 128u) && PrepareLayer(crop, w, h);      // HD menu pictures, where there are some
            patch = PreparePatch(frameBits, crop, w, h);                            // HD letters and pictures over both
            if (dbg & 64u) patch = nullptr;
        } else if (lupaFrame && g_snapValid && g_snapTex && g_snapW == w && g_snapH == h) {
            lupaOn = true;      // the world went to its texture before the windows were drawn
        }
        if (!EnsureTexture(w, h)) {
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(g_ctx->Map(g_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            return false;
        }
        auto* dst = static_cast<uint8_t*>(map.pData);
        if (g_tex565) {
            const size_t rowBytes = static_cast<size_t>(w) * 2;
            for (int y = 0; y < h; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * map.RowPitch, bits + static_cast<size_t>(y) * pitch, rowBytes);
            }
            RenderDDraw::DrawOverlayRGB565(dst, static_cast<LONG>(map.RowPitch), w, h, bits, pitch);
            SettingsMenu::Draw(dst, static_cast<LONG>(map.RowPitch), false, w, h, crop.left, crop.top, bits, pitch);
        } else {
            RenderDDraw::ConvertRGB565ToXRGB8888_SSE2(reinterpret_cast<const uint16_t*>(bits), pitch,
                                                      reinterpret_cast<uint32_t*>(dst), static_cast<int>(map.RowPitch), w, h);
            SettingsMenu::Draw(dst, static_cast<LONG>(map.RowPitch), true, w, h, crop.left, crop.top, bits, pitch);
        }
        g_ctx->Unmap(g_tex, 0);

        int vx, vy, vw, vh;
        FitRect(w, h, static_cast<int>(g_bbW), static_cast<int>(g_bbH), vx, vy, vw, vh);

        if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            ShaderConsts c{};
            c.texW = static_cast<float>(w);
            c.texH = static_cast<float>(h);
            c.outW = static_cast<float>(vw);
            c.outH = static_cast<float>(vh);
            c.mode = static_cast<float>(SherwoodConfig::Get().shaderFilter);
            c.layer = backdropOn ? 2.0f : (layerOn ? 1.0f : (lupaOn ? 3.0f : 0.0f));
            if (lupaOn) {
                c.lupaM = lupa.m;
                c.lupaWorldH = lupa.worldH;
                c.lupaX = lupa.cropX;
                c.lupaY = lupa.cropY;
            } else if (backdropOn && g_backShowOn) {
                c.lupaM = 1.0f;     // layer 2: the backdrop's shown version (t5) instead of t3
            }
            c.cropX = static_cast<float>(crop.left);
            c.cropY = static_cast<float>(crop.top);
            c.patchOn = patch ? 1.0f : 0.0f;
            int fx = 0, fy = 0, fw = 0, fh = 0;
            FitRect(frameW, frameH, static_cast<int>(g_bbW), static_cast<int>(g_bbH), fx, fy, fw, fh);
            c.fullX = static_cast<float>(fx);
            c.fullY = static_cast<float>(fy);
            c.fullW = static_cast<float>(fw > 0 ? fw : 1);
            c.fullH = static_cast<float>(fh > 0 ? fh : 1);
            std::memcpy(map.pData, &c, sizeof(c));
            g_ctx->Unmap(g_cb, 0);
        }

        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, black);
        D3D11_VIEWPORT vp{};
        vp.TopLeftX = static_cast<float>(vx);
        vp.TopLeftY = static_cast<float>(vy);
        vp.Width = static_cast<float>(vw);
        vp.Height = static_cast<float>(vh);
        vp.MaxDepth = 1.0f;
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->PSSetShader(g_ps, nullptr, 0);
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
        ID3D11ShaderResourceView* srvs[8] = { g_srv, layerOn ? g_layerOrigSrv : nullptr, layerOn ? g_layerHdSrv : nullptr,
                                              backdropOn ? g_backdropSrv : nullptr, lupaOn ? g_snapSrv : nullptr,
                                              (backdropOn && g_backShowOn) ? g_backShowSrv : nullptr,
                                              patch ? patch->refSrv : nullptr, patch ? patch->hdSrv : nullptr };
        g_ctx->PSSetShaderResources(0, 8, srvs);
        ID3D11SamplerState* samplers[2] = { g_smpPoint, g_smpLinear };
        g_ctx->PSSetSamplers(0, 2, samplers);
        g_ctx->Draw(3, 0);

        if (g_capturePath) {
            TestHarness::ShotDone(SaveBackBuffer(*g_capturePath), static_cast<int>(g_bbW), static_cast<int>(g_bbH));
            g_capturePath = nullptr;
        }

        const bool vsync = SherwoodConfig::Get().vsync;
        const HRESULT hr = g_swap->Present(vsync ? 1 : 0, (!vsync && g_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            Logger::Warn("Present: device lost (0x%08X).", static_cast<unsigned>(hr));
            return false;
        }
        return true;
    }

    void FrameGDI(const uint8_t* bits, int pitch, int w, int h) {
        RECT rc{};
        GetClientRect(g_hwnd, &rc);
        const int outW = rc.right - rc.left;
        const int outH = rc.bottom - rc.top;
        if (outW <= 0 || outH <= 0) return;

        struct {
            BITMAPINFOHEADER hdr;
            DWORD masks[3];
        } bmi{};
        bmi.hdr.biSize = sizeof(BITMAPINFOHEADER);
        bmi.hdr.biWidth = pitch / 2; // DIB rows are DWORD aligned, same as our surface pitch
        bmi.hdr.biHeight = -h;
        bmi.hdr.biPlanes = 1;
        bmi.hdr.biBitCount = 16;
        bmi.hdr.biCompression = BI_BITFIELDS;
        bmi.masks[0] = 0xF800;
        bmi.masks[1] = 0x07E0;
        bmi.masks[2] = 0x001F;

        int x, y, cw, ch;
        FitRect(w, h, outW, outH, x, y, cw, ch);
        HDC dc = GetDC(g_hwnd);
        if (!dc) return;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, x, y, cw, ch, 0, 0, w, h, bits, reinterpret_cast<const BITMAPINFO*>(&bmi), DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(g_hwnd, dc);
    }

    void EnsureBackend() {
        if (g_initTried || !g_hwnd) return;
        g_initTried = true;
        if (SherwoodConfig::Get().presenter != 1 && InitD3D11()) {
            g_backend = Backend::D3D11;
            return;
        }
        ReleaseD3D();
        g_backend = Backend::GDI;
        Logger::Warn("Present: using the GDI fallback presenter.");
    }
}

void SetWindow(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (hwnd == g_hwnd) return;
    if (g_backend == Backend::D3D11) {
        ReleaseD3D();
    }
    g_backend = Backend::None;
    g_initTried = false;
    g_hwnd = hwnd;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_lock);
    ReleaseD3D();
    g_backend = Backend::None;
    g_hwnd = nullptr;
}

void Frame(const uint8_t* bits, int pitch, int w, int h) {
    if (!bits || w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    EnsureBackend();
    SettingsMenu::Poll();

    // background test mode: frame dump on request
    std::string shotPath;
    bool shotScaled = false;
    if (TestHarness::TakeShotRequest(shotPath, shotScaled)) {
        if (!shotScaled) {
            TestHarness::ShotDone(SaveFrame565(shotPath, bits, pitch, w, h), w, h);
        } else if (g_backend == Backend::D3D11) {
            g_capturePath = &shotPath; // taken inside FrameD3D11, after the scaling shader
        } else {
            TestHarness::ShotDone(false, 0, 0);
        }
    }

    if (g_backend == Backend::D3D11) {
        if (!FrameD3D11(bits, pitch, w, h)) {
            Logger::Warn("Present: Direct3D 11 frame failed - switching to GDI.");
            ReleaseD3D();
            g_backend = Backend::GDI;
        }
        if (g_capturePath) { // the frame ended before the dump point
            TestHarness::ShotDone(false, 0, 0);
            g_capturePath = nullptr;
        }
    }
    if (g_backend == Backend::GDI) {
        UiScale::MenuZoomUnavailable();
        FrameGDI(bits, pitch, w, h);
    }
}

bool UploadWorld(const uint8_t* bits, int pitch, int w, int h) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!bits || w <= 0 || h <= 0 || g_backend != Backend::D3D11 || !g_dev || !g_ctx || !EnsureSnap(w, h)) {
        g_snapValid = false;
        return false;
    }
    if (g_has565) {
        g_ctx->UpdateSubresource(g_snapTex, 0, nullptr, bits, static_cast<UINT>(pitch), 0);
    } else {
        g_snap8888.resize(static_cast<size_t>(w) * h);
        RenderDDraw::ConvertRGB565ToXRGB8888_SSE2(reinterpret_cast<const uint16_t*>(bits), pitch, g_snap8888.data(), w * 4, w, h);
        g_ctx->UpdateSubresource(g_snapTex, 0, nullptr, g_snap8888.data(), static_cast<UINT>(w * 4), 0);
    }
    g_snapValid = true;
    return true;
}

bool ReadWorld(std::vector<uint16_t>& out, int& w, int& h) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_snapValid || !g_snapTex || !g_dev || !g_ctx) return false;
    D3D11_TEXTURE2D_DESC td{};
    g_snapTex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &staging))) return false;
    g_ctx->CopyResource(staging, g_snapTex);
    D3D11_MAPPED_SUBRESOURCE map{};
    bool ok = SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map));
    if (ok) {
        w = static_cast<int>(td.Width);
        h = static_cast<int>(td.Height);
        out.resize(static_cast<size_t>(w) * h);
        const auto* src = static_cast<const uint8_t*>(map.pData);
        for (int y = 0; y < h; ++y) {
            if (g_has565) {
                std::memcpy(out.data() + static_cast<size_t>(y) * w, src + static_cast<size_t>(y) * map.RowPitch, static_cast<size_t>(w) * 2);
            } else {
                const auto* row = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(y) * map.RowPitch);
                for (int x = 0; x < w; ++x) {
                    const uint32_t c = row[x];
                    out[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
                }
            }
        }
        g_ctx->Unmap(staging, 0);
    }
    staging->Release();
    return ok;
}

const char* BackendName() {
    switch (g_backend) {
        case Backend::D3D11: return "Direct3D 11";
        case Backend::GDI:   return "GDI";
        default:             return "none";
    }
}

} // namespace DDM::Present
