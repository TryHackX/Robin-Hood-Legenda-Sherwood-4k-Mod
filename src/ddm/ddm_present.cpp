#include "ddm_present.h"
#include "ddm.h"
#include "../config.h"
#include "../logger.h"
#include "../render_ddraw.h"
#include "../save_thumbs.h"
#include "../settings_menu.h"
#include "../test_harness.h"
#include "../ui_scale.h"
#include "../view_zoom.h"
#include "ddm_hd.h"
#include "ddm_hd_menu.h"
#include "ddm_hd_patch.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
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
    bool g_has565 = false;
    bool g_tearing = false;
    UINT g_bbW = 0, g_bbH = 0;

    std::atomic<int> g_logFrames{ 0 };    // test harness: frames left to describe in the log (LogNextFrames)
    int g_heldFrames = 0;                  // transitional frames not shown in a row (FrameD3D11)

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
        float cursorX, cursorY, cursorW, cursorH;   // menus: the game's cursor (texture pixels; W 0 = none), t8 its HD picture
        float lvlOriginX, lvlOriginY, lvlScale, lvlMode;   // level layer (LevelUse): t10 pixel at world pixel (0, 0); t9 pixels
                                                           // per t10 pixel; 0 off, 1 through the world copy, 2 straight from the frame
        float lvlSrcW, lvlSrcH, lvlRefW, lvlRefH;          // ... t9 the picture shown, t10 the one the engine drew the world from
        float lvlTap, lvlWorldH, lvlTint, pad3;            // ... the four taps' distance (t9 pixels); frame rows of the world
                                                           // (mode 2); test harness: those pixels tinted
    };

    // The level layer (HDLevelLayer, 2026-11-22, user: "zoomed all the way out the quality is not HD"): below 2.0 the
    // engine draws the level at 1.0 in the original pixels and the smooth zoom magnified them. Where the world copy
    // still shows the level's own pixel (no figure, shadow or effect over it) the output takes the level's HD picture
    // at that place instead - the HD module's picture (2x) and the level picture it was made from, as textures.
    ID3D11Texture2D*          g_lvlHdTex = nullptr;
    ID3D11ShaderResourceView* g_lvlHdSrv = nullptr;
    ID3D11Texture2D*          g_lvl1xTex = nullptr;
    ID3D11ShaderResourceView* g_lvl1xSrv = nullptr;
    unsigned g_lvlSerial = 0;
    int g_lvlHdW = 0, g_lvlHdH = 0, g_lvlW = 0, g_lvlH = 0;

    // the game's cursor in menus and dialogs, drawn from its HD picture over the output (ddm_hd_patch.h GetCursor)
    ID3D11Texture2D*          g_cursorTex = nullptr;
    ID3D11ShaderResourceView* g_cursorSrv = nullptr;
    int      g_cursorTexW = 0, g_cursorTexH = 0;
    unsigned g_cursorGen = 0;

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
        "Texture2D cursorTex : register(t8);\n"
        "Texture2D lvlSrc : register(t9);\n"
        "Texture2D lvlRef : register(t10);\n"
        "SamplerState smpPoint : register(s0);\n"
        "SamplerState smpLinear : register(s1);\n"
        "cbuffer C : register(b0) { float2 texSize; float2 outSize; float mode; float layer; float2 cropOrigin; float4 full; float4 lupa; float4 extra; float4 cursor; float4 lvl0; float4 lvl1; float4 lvl2; };\n"
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
        // smaller than the texture (a 2560x1440 game on a 1920x1080 screen, a 3840x2160 one on 2560x1440): four
        // bilinear taps over the output pixel's footprint - one tap skipped texels, leaves and edges flickered and
        // looked jagged (user report 2026-11-22, the game set to 3840x2160 on a 1440p screen)
        "float3 Minified(Texture2D t, float2 texel, float2 scale) {\n"
        "  float2 d = 0.25 / scale; float2 inv = 1.0 / texSize;\n"
        "  return 0.25 * (t.Sample(smpLinear, (texel + float2(-d.x, -d.y)) * inv).rgb + t.Sample(smpLinear, (texel + float2(d.x, -d.y)) * inv).rgb +\n"
        "                 t.Sample(smpLinear, (texel + float2(-d.x, d.y)) * inv).rgb + t.Sample(smpLinear, (texel + float2(d.x, d.y)) * inv).rgb);\n"
        "}\n"
        "float3 Filtered(Texture2D t, float2 texel, float2 scale) {\n"
        "  if (mode < 0.5) return t.Sample(smpPoint, texel / texSize).rgb;\n"
        "  if (scale.x < 0.99 || scale.y < 0.99) return Minified(t, texel, scale);\n"
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
        // level layer: world pixel w shows pixel origin + w of the picture the engine drew the world from (t10: the
        // level picture at 1.0, its HD picture at 2.0) - still that pixel there (world copy, or the frame in mode 2)?
        "int2 LvlOrigin() { return int2(floor(lvl0.xy + 0.5)); }\n"
        "bool LevelAt(int2 w) {\n"
        "  int2 lp = LvlOrigin() + w;\n"
        "  if (lp.x < 0 || lp.y < 0 || lp.x >= (int)lvl1.z || lp.y >= (int)lvl1.w) return false;\n"
        "  w = clamp(w, int2(0, 0), int2(texSize) - 1);\n"
        "  float3 s = lvl0.w > 1.5 ? tex.Load(int3(w, 0)).rgb : world.Load(int3(w, 0)).rgb;\n"
        "  return Pack565(s) == Pack565(lvlRef.Load(int3(lp, 0)).rgb);\n"
        "}\n"
        "int CountNot_LevelAt(int2 t) { int n = 0; [unroll] for (int dy = -1; dy <= 1; ++dy) [unroll] for (int dx = -1; dx <= 1; ++dx) if ((dx != 0 || dy != 0) && !LevelAt(t + int2(dx, dy))) ++n; return n; }\n"
        // ... then the picture shown (t9: HD or 4x) at world position wp, four taps a quarter output pixel apart
        "bool LevelHd(float2 wp, out float3 c) {\n"
        "  c = float3(0.0, 0.0, 0.0);\n"
        "  int2 w = int2(floor(wp));\n"
        "  if (!LevelAt(w) || CountNot_LevelAt(w) > 3) return false;\n"
        "  float2 hp = (float2(LvlOrigin()) + wp) * lvl0.z;\n"
        "  float d = lvl2.x;\n"
        "  float2 inv = 1.0 / lvl1.xy;\n"
        "  c = 0.25 * (lvlSrc.Sample(smpLinear, (hp + float2(-d, -d)) * inv).rgb + lvlSrc.Sample(smpLinear, (hp + float2(d, -d)) * inv).rgb +\n"
        "              lvlSrc.Sample(smpLinear, (hp + float2(-d, d)) * inv).rgb + lvlSrc.Sample(smpLinear, (hp + float2(d, d)) * inv).rgb);\n"
        "  if (lvl2.z > 0.5) c = lerp(c, float3(1.0, 0.0, 1.0), 0.3);\n"
        "  return true;\n"
        "}\n"
        "float3 WorldMagnified(float2 texel, float m) {\n"
        "  if (mode >= 0.5 && m > 1.001 && m < 1.999) return CatRom(world, texel);\n"
        "  return Filtered(world, texel, outSize / texSize * m);\n"
        "}\n"
        "float4 Base(float4 pos, float2 uv) {\n"
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
        "        if (lvl0.w > 0.5) { float3 lc; if (LevelHd(wp, lc)) return float4(lc, 1.0); }\n"
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
        "  } else if (lvl0.w > 1.5) {\n"
        // the world not magnified, the engine at 2.0: the level from its 4x picture where the frame shows its HD pixel
        "    float2 fp = uv * texSize;\n"
        "    float3 lc;\n"
        "    if (fp.y < lvl2.y && LevelHd(fp, lc)) return float4(lc, 1.0);\n"
        "  }\n"
        "  if (mode < 0.5) return float4(tex.Sample(smpPoint, uv).rgb, 1.0);\n"
        "  if (outSize.x < texSize.x * 0.99 || outSize.y < texSize.y * 0.99) return float4(Minified(tex, uv * texSize, outSize / texSize), 1.0);\n"
        "  if (mode > 1.5) return float4(tex.Sample(smpLinear, uv).rgb, 1.0);\n"
        "  float2 texel = uv * texSize;\n"
        "  float2 scale = max(floor(outSize / texSize), 1.0);\n"
        "  float2 i = floor(texel); float2 f = texel - i;\n"
        "  float2 region = 0.5 - 0.5 / scale;\n"
        "  float2 c = f - 0.5;\n"
        "  float2 ff = (c - clamp(c, -region, region)) * scale + 0.5;\n"
        "  return float4(tex.Sample(smpLinear, (i + ff) / texSize).rgb, 1.0);\n"
        "}\n"
        // menus: the game's cursor from its HD picture over everything (the frame texture holds what it covered,
        // so every layer shows its HD picture under it); t8 = 2x picture with a clear 1-texel border, premultiplied
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  float4 o = Base(pos, uv);\n"
        "  if (cursor.z > 0.5) {\n"
        "    float2 q = uv * texSize - cursor.xy;\n"
        "    if (q.x > -0.5 && q.y > -0.5 && q.x < cursor.z + 0.5 && q.y < cursor.w + 0.5) {\n"
        "      float4 c = cursorTex.Sample(smpLinear, (q * 2.0 + 1.0) / (cursor.zw * 2.0 + 2.0));\n"
        "      o.rgb = c.rgb + o.rgb * (1.0 - c.a);\n"
        "    }\n"
        "  }\n"
        "  return o;\n"
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
        } else {
            // the two canvases change apart: a copy of the same picture only moves where it shows (the
            // credits every frame), the HD picture itself is uploaded when another one comes
            if (layer.dirty.right > layer.dirty.left && layer.dirty.bottom > layer.dirty.top) {
                const RECT& d = layer.dirty;
                const D3D11_BOX box1{ static_cast<UINT>(d.left), static_cast<UINT>(d.top), 0, static_cast<UINT>(d.right), static_cast<UINT>(d.bottom), 1 };
                g_ctx->UpdateSubresource(g_layerOrig, 0, &box1, layer.orig + static_cast<size_t>(d.top) * w + d.left, static_cast<UINT>(w * 4), 0);
            }
            if (layer.hdDirty.right > layer.hdDirty.left && layer.hdDirty.bottom > layer.hdDirty.top) {
                const RECT& d = layer.hdDirty;
                const D3D11_BOX box2{ static_cast<UINT>(d.left * 2), static_cast<UINT>(d.top * 2), 0,
                                      static_cast<UINT>(d.right * 2), static_cast<UINT>(d.bottom * 2), 1 };
                g_ctx->UpdateSubresource(g_layerHd, 0, &box2, layer.hd + static_cast<size_t>(d.top * 2) * (w * 2) + d.left * 2,
                                         static_cast<UINT>(w * 2 * 4), 0);
            }
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

    void ReleaseCursor() {
        SafeRelease(g_cursorSrv);
        SafeRelease(g_cursorTex);
        g_cursorTexW = g_cursorTexH = 0;
        g_cursorGen = 0;
    }

    void ReleaseLevel() {
        SafeRelease(g_lvlHdSrv);
        SafeRelease(g_lvlHdTex);
        SafeRelease(g_lvl1xSrv);
        SafeRelease(g_lvl1xTex);
        g_lvlSerial = 0;
        g_lvlHdW = g_lvlHdH = g_lvlW = g_lvlH = 0;
    }

    // the level layer's two pictures (t9, t10) for the level in use: made when another one comes
    bool PrepareLevel() {
        DDM::HD::LevelPictures lp;
        if (!g_has565 || !DDM::HD::LevelPicturesNow(lp) || lp.hdW > 16384 || lp.hdH > 16384) return false;
        if (g_lvlHdTex && g_lvl1xTex && lp.serial == g_lvlSerial && lp.hdW == g_lvlHdW && lp.hdH == g_lvlHdH &&
            lp.levelW == g_lvlW && lp.levelH == g_lvlH) {
            return true;
        }
        ReleaseLevel();
        LARGE_INTEGER t0{}, t1{}, f{};
        QueryPerformanceCounter(&t0);
        D3D11_TEXTURE2D_DESC td{};
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B5G6R5_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{};
        td.Width = static_cast<UINT>(lp.hdW);
        td.Height = static_cast<UINT>(lp.hdH);
        sd.pSysMem = lp.hd;
        sd.SysMemPitch = static_cast<UINT>(lp.hdW * 2);
        bool ok = SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &g_lvlHdTex)) &&
                  SUCCEEDED(g_dev->CreateShaderResourceView(g_lvlHdTex, nullptr, &g_lvlHdSrv));
        td.Width = static_cast<UINT>(lp.levelW);
        td.Height = static_cast<UINT>(lp.levelH);
        sd.pSysMem = lp.level;
        sd.SysMemPitch = static_cast<UINT>(lp.levelW * 2);
        ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &g_lvl1xTex)) &&
             SUCCEEDED(g_dev->CreateShaderResourceView(g_lvl1xTex, nullptr, &g_lvl1xSrv));
        if (!ok) {
            ReleaseLevel();
            static bool once = false;
            if (!once) {
                once = true;
                Logger::Warn("Present: level layer textures could not be made (%dx%d) - zoomed out the level stays magnified.", lp.hdW, lp.hdH);
            }
            return false;
        }
        g_lvlSerial = lp.serial;
        g_lvlHdW = lp.hdW;
        g_lvlHdH = lp.hdH;
        g_lvlW = lp.levelW;
        g_lvlH = lp.levelH;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        Logger::Info("Present: level layer - HD picture %dx%d and level picture %dx%d on the card (%.1f ms).", lp.hdW, lp.hdH,
                     lp.levelW, lp.levelH, static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart));
        return true;
    }

    // ---- the level's 4x picture (HDMaps4k, 2026-11-22, user: "do the same for the other classic maps so that it is
    // native 4K"): hd\maps4k\<variant>\<key>.hdm4 (tools\hd\make_maps4k.py; "HDM4", u32 1, width, height, 0, RGB565
    // rows). 40-250 MB: too much to read at once on the main thread or to hold in this 32-bit process. A reader thread
    // reads it in strips into two buffers; every presented frame puts at most one strip on the card; used once whole.
    constexpr size_t kStrip4kBytes = 4u << 20;
    struct Strip4k {
        std::vector<uint16_t> px;
        int y0 = 0, rows = 0;
        bool full = false;          // read, not yet on the card (only the reader fills, only the presenter empties)
    };
    struct Reader4k {
        std::mutex lock;
        std::condition_variable cv;
        std::string path;           // presenter: the file wanted ("" = none)
        unsigned request = 0;       // ... changes with every wish
        unsigned answered = 0;      // reader: the wish the size and the strips belong to
        int w = 0, h = 0;           // -1 = no usable file
        Strip4k strips[2];
    };
    Reader4k g_r4;
    bool g_r4Thread = false;

    ID3D11Texture2D*          g_l4Tex = nullptr;
    ID3D11ShaderResourceView* g_l4Srv = nullptr;
    std::string g_l4Path;           // the file the texture is filled from
    unsigned g_l4Request = 0;
    int  g_l4W = 0, g_l4H = 0, g_l4Rows = 0;
    bool g_l4Bad = true;            // nothing to fill from (no file, a broken one, no texture)
    LARGE_INTEGER g_l4Start{};

    DWORD WINAPI Reader4kThread(void*) {
        std::unique_lock<std::mutex> lk(g_r4.lock);
        unsigned done = 0;
        for (;;) {
            g_r4.cv.wait(lk, [&] { return g_r4.request != done; });
            const unsigned req = g_r4.request;
            const std::string path = g_r4.path;
            done = req;
            lk.unlock();
            HANDLE f = path.empty() ? INVALID_HANDLE_VALUE
                                    : CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            int w = -1, h = -1;
            if (f != INVALID_HANDLE_VALUE) {
                uint32_t hdr[5]{};
                DWORD got = 0;
                LARGE_INTEGER size{};
                if (ReadFile(f, hdr, sizeof(hdr), &got, nullptr) && got == sizeof(hdr) && std::memcmp(hdr, "HDM4", 4) == 0 && hdr[1] == 1 &&
                    hdr[2] >= 1 && hdr[3] >= 1 && hdr[2] <= 16384 && hdr[3] <= 16384 && GetFileSizeEx(f, &size) &&
                    size.QuadPart >= static_cast<LONGLONG>(sizeof(hdr)) + static_cast<LONGLONG>(hdr[2]) * hdr[3] * 2) {
                    w = static_cast<int>(hdr[2]);
                    h = static_cast<int>(hdr[3]);
                }
            }
            lk.lock();
            if (g_r4.request == req) {
                g_r4.answered = req;
                g_r4.w = w;
                g_r4.h = h;
                for (auto& s : g_r4.strips) s.full = false;
                const int rowsPer = w > 0 ? (std::max)(1, static_cast<int>(kStrip4kBytes / (static_cast<size_t>(w) * 2))) : 0;
                for (int y = 0; w > 0 && y < h;) {
                    g_r4.cv.wait(lk, [&] { return g_r4.request != req || !g_r4.strips[0].full || !g_r4.strips[1].full; });
                    if (g_r4.request != req) break;
                    Strip4k& s = !g_r4.strips[0].full ? g_r4.strips[0] : g_r4.strips[1];
                    const int rows = (std::min)(rowsPer, h - y);
                    lk.unlock();
                    bool ok = true;
                    try {
                        s.px.resize(static_cast<size_t>(rows) * static_cast<size_t>(w));
                    } catch (...) {
                        ok = false;
                    }
                    DWORD got = 0;
                    const DWORD want = static_cast<DWORD>(rows) * static_cast<DWORD>(w) * 2u;
                    ok = ok && ReadFile(f, s.px.data(), want, &got, nullptr) && got == want;
                    lk.lock();
                    if (g_r4.request != req) break;
                    if (!ok) {
                        g_r4.w = g_r4.h = -1;
                        break;
                    }
                    s.y0 = y;
                    s.rows = rows;
                    s.full = true;
                    y += rows;
                }
            }
            lk.unlock();
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            lk.lock();
        }
    }

    void Release4k() {
        SafeRelease(g_l4Srv);
        SafeRelease(g_l4Tex);
        g_l4W = g_l4H = g_l4Rows = 0;
    }

    // the reader is to fill the texture from `path` ("" = nothing); the same wish again changes nothing
    void Want4k(const std::string& path) {
        if (g_l4Request && path == g_l4Path) return;
        Release4k();
        g_l4Path = path;
        g_l4Bad = path.empty();
        {
            std::lock_guard<std::mutex> lk(g_r4.lock);
            g_r4.path = path;
            g_l4Request = ++g_r4.request;
        }
        g_r4.cv.notify_all();
        if (!g_r4Thread && !path.empty()) {
            if (HANDLE t = CreateThread(nullptr, 0, Reader4kThread, nullptr, 0, nullptr)) {
                CloseHandle(t);
                g_r4Thread = true;
            } else {
                g_l4Bad = true;
            }
        }
        QueryPerformanceCounter(&g_l4Start);
    }

    // after the device was released: the next wish reads the file again
    void Forget4k() {
        Release4k();
        g_l4Path.clear();
        g_l4Request = 0;
        g_l4Bad = true;
    }

    // hd\maps4k\<variant>\<key>.hdm4 of the level in use ("" = none; only the variant shown - its 2x picture is what
    // the player chose, F6/F7 compare variants)
    const std::string& Path4k(const DDM::HD::LevelPictures& lp) {
        static uint64_t key = 0;
        static std::string variant;
        static std::string found;
        static bool known = false;
        if (known && key == lp.key && variant == lp.variant) return found;
        known = true;
        key = lp.key;
        variant = lp.variant;
        found.clear();
        char name[48];
        std::snprintf(name, sizeof(name), "\\%016llx.hdm4", static_cast<unsigned long long>(lp.key));
        const std::string path = DDM::HD::GameFolder() + "hd\\maps4k\\" + variant + name;
        const DWORD a = variant.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesA(path.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) found = path;
        return found;
    }

    // one more strip of the wished 4x picture on the card; true once all of it is there
    bool Step4k() {
        if (g_l4Bad) return false;
        if (g_l4Tex && g_l4Rows >= g_l4H) return true;
        std::unique_lock<std::mutex> lk(g_r4.lock);
        if (g_r4.answered != g_l4Request || g_r4.w == 0) return false;    // the reader has not opened it yet
        if (g_r4.w < 0) {
            g_l4Bad = true;
            Release4k();
            Logger::Warn("Present: the 4x level picture %s cannot be read - the HD picture stays.", g_l4Path.c_str());
            return false;
        }
        if (!g_l4Tex) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(g_r4.w);
            td.Height = static_cast<UINT>(g_r4.h);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B5G6R5_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_l4Tex)) || FAILED(g_dev->CreateShaderResourceView(g_l4Tex, nullptr, &g_l4Srv))) {
                Release4k();
                g_l4Bad = true;
                g_r4.path.clear();          // the reader stops
                g_l4Request = ++g_r4.request;
                lk.unlock();
                g_r4.cv.notify_all();
                Logger::Warn("Present: no texture of %ux%u for the 4x level picture - the HD picture stays.", td.Width, td.Height);
                return false;
            }
            g_l4W = g_r4.w;
            g_l4H = g_r4.h;
            g_l4Rows = 0;
            QueryPerformanceCounter(&g_l4Start);    // (a dialog may have held the first strip back for a while)
        }
        for (Strip4k& s : g_r4.strips) {
            if (!s.full) continue;
            const D3D11_BOX box{ 0, static_cast<UINT>(s.y0), 0, static_cast<UINT>(g_l4W), static_cast<UINT>(s.y0 + s.rows), 1 };
            g_ctx->UpdateSubresource(g_l4Tex, 0, &box, s.px.data(), static_cast<UINT>(g_l4W * 2), 0);
            g_l4Rows += s.rows;
            s.full = false;
            break;
        }
        lk.unlock();
        g_r4.cv.notify_all();
        if (g_l4Rows < g_l4H) return false;
        LARGE_INTEGER t1{}, f{};
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        // (the time includes frames without the level - a dialog - in which no strip went up)
        Logger::Info("Present: 4x level picture %dx%d on the card (%s, %.0f ms after the first strip).", g_l4W, g_l4H, g_l4Path.c_str(),
                     static_cast<double>(t1.QuadPart - g_l4Start.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart));
        return true;
    }

    // Where the output shows the level from a picture of its own instead of the world the engine drew (the shader's
    // LevelAt / LevelHd): t10 the picture the engine drew the world from, t9 the one shown.
    //   engine 1.0, smooth zoom: t10 the level picture, t9 its HD picture (HDLevelLayer) or the 4x one;
    //   engine 2.0, smooth zoom beyond 2.0: t10 the HD picture, t9 the 4x one;
    //   engine 2.0, not magnified (larger output than the frame - 4K screens): the same, tested on the frame itself.
    // The 4x picture where an HD pixel would cover more than one output pixel (HDMaps4k=2: wherever it is there).
    struct LevelUse {
        int mode = 0;                                   // 0 none, 1 through the world copy, 2 straight from the frame
        ID3D11ShaderResourceView* src = nullptr;
        ID3D11ShaderResourceView* ref = nullptr;
        float originX = 0.0f, originY = 0.0f;           // t10 pixel at world pixel (0, 0)
        float scale = 2.0f;                             // t9 pixels per t10 pixel
        float srcW = 0.0f, srcH = 0.0f, refW = 0.0f, refH = 0.0f;
        float tap = 0.0f;
        bool is4k = false;
    };

    LevelUse ChooseLevel(bool cropOn, bool lupaOn, const ViewZoom::View& lupa, float outPerFrame) {
        LevelUse u;
        const auto& cfg = SherwoodConfig::Get();
        DDM::HD::LevelPictures lp;
        const bool have = DDM::HD::LevelPicturesNow(lp);
        if (cfg.hdMaps4k > 0 && have) Want4k(Path4k(lp));     // read in the background whatever the zoom
        if (cropOn || !have || (DDM::HD::DebugFlags() & 2048u) || outPerFrame <= 0.0f) return u;
        const bool ready4k = cfg.hdMaps4k > 0 && Step4k();
        float zoom = lupa.zoom;
        if (!lupaOn && !ViewZoom::FrameZoom(zoom)) return u;
        const float m = lupaOn ? lupa.m : 1.0f;
        const bool at1 = zoom > 0.99f && zoom < 1.01f, at2 = zoom > 1.99f && zoom < 2.01f;
        // output pixels per pixel of the picture the engine drew the world from
        const float outPerRef = m * outPerFrame;
        int ox = 0, oy = 0;
        if (at1 && lupaOn) {
            const bool use4k = ready4k && (cfg.hdMaps4k == 2 || 2.0f / outPerRef < 0.9f);
            if (!use4k && !cfg.hdLevelLayer) return u;
            u.originX = lupa.camX;
            u.originY = lupa.camY;
            u.scale = use4k ? 4.0f : 2.0f;
            u.is4k = use4k;
            u.mode = 1;
        } else if (at2 && DDM::HD::LevelOrigin(ox, oy)) {
            if (!ready4k || !(cfg.hdMaps4k == 2 || 1.0f / outPerRef < 0.9f)) return u;
            u.originX = static_cast<float>(ox);
            u.originY = static_cast<float>(oy);
            u.scale = 2.0f;
            u.is4k = true;
            u.mode = lupaOn ? 1 : 2;
        } else {
            return u;
        }
        if (!PrepareLevel()) return LevelUse{};
        u.ref = at1 ? g_lvl1xSrv : g_lvlHdSrv;
        u.refW = static_cast<float>(at1 ? g_lvlW : g_lvlHdW);
        u.refH = static_cast<float>(at1 ? g_lvlH : g_lvlHdH);
        u.src = u.is4k ? g_l4Srv : g_lvlHdSrv;
        u.srcW = static_cast<float>(u.is4k ? g_l4W : g_lvlHdW);
        u.srcH = static_cast<float>(u.is4k ? g_l4H : g_lvlHdH);
        if (u.is4k && (static_cast<int>(u.srcW) != static_cast<int>(u.refW * u.scale) ||
                       static_cast<int>(u.srcH) != static_cast<int>(u.refH * u.scale))) {
            return LevelUse{};      // another level's picture (the reader has not caught up)
        }
        // four taps a quarter of an output pixel's footprint apart (at most a 4-pixel footprint)
        u.tap = 0.25f * (std::min)(u.scale / outPerRef, 4.0f);
        return u;
    }

    // the HD picture of the game's cursor (GetCursor) in t8: a texture of its size, uploaded when it changes
    bool PrepareCursor(const DDM::HD::Patch::Cursor& c) {
        const int tw = c.w * 2 + 2, th = c.h * 2 + 2;
        if (c.w <= 0 || c.h <= 0 || tw > 1024 || th > 1024 || !c.overlay) return false;
        if (!g_cursorTex || g_cursorTexW != tw || g_cursorTexH != th) {
            ReleaseCursor();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(tw);
            td.Height = static_cast<UINT>(th);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_cursorTex)) ||
                FAILED(g_dev->CreateShaderResourceView(g_cursorTex, nullptr, &g_cursorSrv))) {
                ReleaseCursor();
                return false;
            }
            g_cursorTexW = tw;
            g_cursorTexH = th;
        }
        if (c.generation != g_cursorGen) {
            g_ctx->UpdateSubresource(g_cursorTex, 0, nullptr, c.overlay, static_cast<UINT>(tw * 4), 0);
            g_cursorGen = c.generation;
        }
        return true;
    }

    void ReleaseD3D() {
        ReleaseCursor();
        ReleaseLevel();
        Forget4k();
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

    std::string CardName(const DXGI_ADAPTER_DESC1& d) {
        char name[160]{};
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        return name;
    }

    // The graphics card to render on (2026-11-18, a YouTuber's OBS game capture stayed black also with OBS as
    // administrator; his AMD Ryzen 7 9800X3D has a small graphics unit of its own next to the graphics card, and
    // D3D11CreateDevice(nullptr) takes the adapter Windows lists first - on Windows 11 often the power-saving one
    // for a program it does not know. The game then renders on the processor's graphics, OBS on the card: game
    // capture black, display capture fine). [Display] GPU=0: the most powerful card - DXGI 1.6's high-performance
    // order, without it the hardware card with the most memory of its own; GPU=1: Windows' choice (up to 1.2.5).
    // Every card is logged once, with its memory and whether the game's monitor is connected to it.
    IDXGIAdapter1* PickAdapter() {
        static bool s_listed = false;
        IDXGIFactory1* f1 = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f1)))) return nullptr;
        const HMONITOR gameMonitor = g_hwnd ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
        // every hardware card; the one the game's monitor is connected to
        IDXGIAdapter1* best = nullptr;          // the most powerful: high-performance order, else the most memory
        SIZE_T bestMemory = 0;
        IDXGIAdapter1* monitorCard = nullptr;
        UINT monitorIndex = 0;
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* a = nullptr;
            if (f1->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND || !a) break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            int monitors = 0;
            bool game = false;
            for (UINT o = 0;; ++o) {
                IDXGIOutput* out = nullptr;
                if (a->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND || !out) break;
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(out->GetDesc(&od))) {
                    ++monitors;
                    if (od.Monitor == gameMonitor) game = true;
                }
                out->Release();
            }
            if (!s_listed) {
                // (a 32-bit program sees at most about 3-4 GB of a card's memory)
                Logger::Info("Present: graphics card %u: %s, %u MB of its own, %d monitor(s)%s%s.", i, CardName(d).c_str(),
                             static_cast<unsigned>(d.DedicatedVideoMemory >> 20), monitors, game ? " - the game's monitor" : "",
                             software ? " [software]" : "");
            }
            if (software) {
                a->Release();
                continue;
            }
            if (game && !monitorCard) {
                monitorCard = a;
                monitorCard->AddRef();
                monitorIndex = i;
            }
            if (!best || d.DedicatedVideoMemory > bestMemory) {
                if (best) best->Release();
                best = a;
                bestMemory = d.DedicatedVideoMemory;
            } else {
                a->Release();
            }
        }
        s_listed = true;
        IDXGIFactory6* f6 = nullptr;
        if (SUCCEEDED(f1->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&f6)))) {
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1* hp = nullptr;
                if (FAILED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                                          reinterpret_cast<void**>(&hp))) || !hp) {
                    break;
                }
                DXGI_ADAPTER_DESC1 d{};
                hp->GetDesc1(&d);
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    if (best) best->Release();
                    best = hp;
                    break;
                }
                hp->Release();
            }
            f6->Release();
        }
        f1->Release();
        // The card the game's monitor hangs on, when it is that same card (or the same model listed twice - a remote
        // desktop shows one card twice): the picture then goes straight to the monitor.
        if (monitorCard && best) {
            DXGI_ADAPTER_DESC1 m{}, b{};
            monitorCard->GetDesc1(&m);
            best->GetDesc1(&b);
            const bool same = (m.AdapterLuid.LowPart == b.AdapterLuid.LowPart && m.AdapterLuid.HighPart == b.AdapterLuid.HighPart) ||
                              (m.VendorId == b.VendorId && m.DeviceId == b.DeviceId);
            if (same) {
                best->Release();
                best = monitorCard;
                monitorCard = nullptr;
                Logger::Info("Present: card %u - the most powerful one, the game's monitor is connected to it.", monitorIndex);
            } else {
                Logger::Info("Present: the game's monitor is on card %u, a weaker one - the picture is made on the most powerful card.",
                             monitorIndex);
            }
        }
        if (monitorCard) monitorCard->Release();
        return best;
    }

    bool InitD3D11() {
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL got{};
        IDXGIAdapter1* card = SherwoodConfig::Get().gpu == 1 ? nullptr : PickAdapter();
        HRESULT hr = E_FAIL;
        if (card) {
            hr = D3D11CreateDevice(card, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
            if (FAILED(hr)) Logger::Warn("Present: Direct3D 11 on the chosen card failed (0x%08X) - Windows' choice.", static_cast<unsigned>(hr));
            card->Release();
        }
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
        }
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
        {
            DXGI_ADAPTER_DESC d{};
            if (SUCCEEDED(adapter->GetDesc(&d))) {
                char name[160]{};
                WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                Logger::Info("Present: Direct3D 11 on %s (%u MB of its own; GPU=%d: %s).", name,
                             static_cast<unsigned>(d.DedicatedVideoMemory >> 20), SherwoodConfig::Get().gpu,
                             SherwoodConfig::Get().gpu == 1 ? "Windows' choice" : "the most powerful card");
            }
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
        DDM::HD::Patch::Cursor cursor;
        bool cursorOn = false;
        const int frameW = w, frameH = h;
        const uint8_t* const frameBits = bits;
        ViewZoom::View lupa;
        const bool lupaFrame = ViewZoom::TakeView(lupa);           // the mission's world, magnified
        if (UiScale::MenuZoomCrop(w, h, crop)) {
            bits += static_cast<size_t>(crop.top) * pitch + static_cast<size_t>(crop.left) * 2;
            w = crop.right - crop.left;
            h = crop.bottom - crop.top;
            const unsigned dbg = DDM::HD::DebugFlags();              // test harness: 64/128/256/512 leave a layer out
            backdropOn = !(dbg & 256u) && PrepareBackdrop(frameW, frameH);           // a mission's dialog: only it enlarged
            layerOn = !backdropOn && !(dbg & 128u) && PrepareLayer(crop, w, h);      // HD menu pictures, where there are some
            patch = PreparePatch(frameBits, crop, w, h);                            // HD letters and pictures over both
            if (dbg & 64u) patch = nullptr;
            // the game's cursor from its HD picture (over everything; the frame shown without the 1x one)
            cursorOn = !(dbg & 512u) && DDM::HD::Patch::GetCursor(frameBits, pitch, cursor) && PrepareCursor(cursor);
        } else if (lupaFrame && g_snapValid && g_snapTex && g_snapW == w && g_snapH == h) {
            lupaOn = true;      // the world went to its texture before the windows were drawn
        }
        if (crop.right <= crop.left) {
            // a mission's cursor (NoteMissionCursor): over the output, its shadow over the (magnified) world shown there
            cursorOn = !(DDM::HD::DebugFlags() & 512u) && DDM::HD::Patch::GetCursor(frameBits, pitch, cursor) && PrepareCursor(cursor);
        }
        // the level from a picture of its own where the world shows its pixels (LevelUse: HD at 1.0, 4x beyond)
        int vx, vy, vw, vh;
        FitRect(w, h, static_cast<int>(g_bbW), static_cast<int>(g_bbH), vx, vy, vw, vh);
        const LevelUse level = ChooseLevel(crop.right > crop.left, lupaOn, lupa, w > 0 ? static_cast<float>(vw) / static_cast<float>(w) : 0.0f);
        const bool levelOn = level.mode != 0;
        // A frame between two states of a dialog in a mission is not shown - the previous one stays, at most
        // kHoldFrames in a row (2026-11-22, user: at the end of a raid "for a split second the scroll cut off on
        // the right, the level bright, then the right one"; the presenter's log showed the two frames): the menu
        // zoom still on after the question's dialog loop ended, nothing of a dialog in the frame - the bright level
        // magnified 1.875x; and the next dialog's first frame drawn before the smooth zoom's copy of the world -
        // the level at the engine's 1.0, not magnified.
        {
            constexpr int kHoldFrames = 6;
            const bool bareCrop = crop.right > crop.left && !backdropOn && !layerOn && !patch && !UiScale::DialogLoopRunning();
            const bool bareWorld = crop.right <= crop.left && !lupaOn && ViewZoom::Magnifying();
            const bool bare = UiScale::EngineAlive() && (bareCrop || bareWorld);
            if (bare && g_heldFrames < kHoldFrames) {
                ++g_heldFrames;
                if (g_logFrames.load(std::memory_order_relaxed) > 0) {
                    g_logFrames.fetch_sub(1, std::memory_order_relaxed);
                    Logger::Info("Present frame: held (%s) - the previous one stays", bareCrop ? "menu zoom without a dialog" : "world not magnified");
                }
                return true;
            }
            // only a normal frame starts the count again: a state that stays is shown after kHoldFrames, not 1 in 7
            if (!bare) g_heldFrames = 0;
        }
        if (!EnsureTexture(w, h)) {
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(g_ctx->Map(g_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            return false;
        }
        auto* dst = static_cast<uint8_t*>(map.pData);
        // the game's 1x cursor out of the frame texture: what it covered (its HD picture comes over the output)
        auto uncover = [&](bool is565) {
            if (!cursorOn) return;
            for (int y = 0; y < cursor.h; ++y) {
                const int ty = cursor.y + y - crop.top;
                if (ty < 0 || ty >= h) continue;
                uint8_t* row = dst + static_cast<size_t>(ty) * map.RowPitch;
                const uint16_t* under = cursor.under + static_cast<size_t>(y) * cursor.w;
                for (int x = 0; x < cursor.w; ++x) {
                    const int tx = cursor.x + x - crop.left;
                    if (tx < 0 || tx >= w) continue;
                    const uint32_t p = under[x];
                    if (is565) {
                        reinterpret_cast<uint16_t*>(row)[tx] = static_cast<uint16_t>(p);
                    } else {
                        uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
                        r = (r << 3) | (r >> 2);
                        g = (g << 2) | (g >> 4);
                        b = (b << 3) | (b >> 2);
                        reinterpret_cast<uint32_t*>(row)[tx] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    }
                }
            }
        };
        if (g_has565) {
            const size_t rowBytes = static_cast<size_t>(w) * 2;
            for (int y = 0; y < h; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * map.RowPitch, bits + static_cast<size_t>(y) * pitch, rowBytes);
            }
            uncover(true);
            RenderDDraw::DrawOverlayRGB565(dst, static_cast<LONG>(map.RowPitch), w, h, bits, pitch);
            SaveThumbs::Draw(dst, static_cast<LONG>(map.RowPitch), false, w, h, crop.left, crop.top);
            SettingsMenu::Draw(dst, static_cast<LONG>(map.RowPitch), false, w, h, crop.left, crop.top, bits, pitch);
        } else {
            // the FPS line and notices exist for RGB565 only: drawn into a copy first
            static std::vector<uint16_t> s_copy;
            s_copy.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
            for (int y = 0; y < h; ++y) {
                std::memcpy(s_copy.data() + static_cast<size_t>(y) * w, bits + static_cast<size_t>(y) * pitch, static_cast<size_t>(w) * 2);
            }
            RenderDDraw::DrawOverlayRGB565(reinterpret_cast<uint8_t*>(s_copy.data()), w * 2, w, h, bits, pitch);
            RenderDDraw::ConvertRGB565ToXRGB8888_SSE2(s_copy.data(), w * 2,
                                                      reinterpret_cast<uint32_t*>(dst), static_cast<int>(map.RowPitch), w, h);
            uncover(false);
            SaveThumbs::Draw(dst, static_cast<LONG>(map.RowPitch), true, w, h, crop.left, crop.top);
            SettingsMenu::Draw(dst, static_cast<LONG>(map.RowPitch), true, w, h, crop.left, crop.top, bits, pitch);
        }
        g_ctx->Unmap(g_tex, 0);

        if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            ShaderConsts c{};
            c.texW = static_cast<float>(w);
            c.texH = static_cast<float>(h);
            c.outW = static_cast<float>(vw);
            c.outH = static_cast<float>(vh);
            c.mode = static_cast<float>(SherwoodConfig::Get().shaderFilter);
            c.layer = backdropOn ? 2.0f : (layerOn ? 1.0f : (lupaOn ? 3.0f : 0.0f));
            const int hudBand = DDM::TakeHudBandTop();
            if (lupaOn) {
                c.lupaM = lupa.m;
                c.lupaWorldH = lupa.worldH;
                // the hedge strip along the bottom is interface in this frame (ddm.cpp HedgeStrip)
                if (hudBand > 0 && static_cast<float>(hudBand) < c.lupaWorldH) c.lupaWorldH = static_cast<float>(hudBand);
                c.lupaX = lupa.cropX;
                c.lupaY = lupa.cropY;
            } else if (backdropOn && g_backShowOn) {
                c.lupaM = 1.0f;     // layer 2: the backdrop's shown version (t5) instead of t3
            }
            c.cropX = static_cast<float>(crop.left);
            c.cropY = static_cast<float>(crop.top);
            c.patchOn = patch ? 1.0f : 0.0f;
            if (levelOn) {
                c.lvlOriginX = level.originX;
                c.lvlOriginY = level.originY;
                c.lvlScale = level.scale;
                c.lvlMode = static_cast<float>(level.mode);
                c.lvlSrcW = level.srcW;
                c.lvlSrcH = level.srcH;
                c.lvlRefW = level.refW;
                c.lvlRefH = level.refH;
                c.lvlTap = level.tap;
                // mode 2: the frame's rows above the interface (the hedge strip along the bottom: ddm.cpp HedgeStrip)
                c.lvlWorldH = (hudBand > 0 && hudBand < h) ? static_cast<float>(hudBand) : static_cast<float>(h);
                c.lvlTint = (DDM::HD::DebugFlags() & 4096u) ? 1.0f : 0.0f;
            }
            if (cursorOn) {
                c.cursorX = static_cast<float>(cursor.x - crop.left);
                c.cursorY = static_cast<float>(cursor.y - crop.top);
                c.cursorW = static_cast<float>(cursor.w);
                c.cursorH = static_cast<float>(cursor.h);
            }
            int fx = 0, fy = 0, fw = 0, fh = 0;
            FitRect(frameW, frameH, static_cast<int>(g_bbW), static_cast<int>(g_bbH), fx, fy, fw, fh);
            c.fullX = static_cast<float>(fx);
            c.fullY = static_cast<float>(fy);
            c.fullW = static_cast<float>(fw > 0 ? fw : 1);
            c.fullH = static_cast<float>(fh > 0 ? fh : 1);
            std::memcpy(map.pData, &c, sizeof(c));
            g_ctx->Unmap(g_cb, 0);
        }

        if (g_logFrames.load(std::memory_order_relaxed) > 0) {
            g_logFrames.fetch_sub(1, std::memory_order_relaxed);
            Logger::Info("Present frame: crop=%s(%ld,%ld-%ld,%ld) backdrop=%d layer=%d patch=%d lupa=%d cursor=%d dialog=%d "
                         "level=%d%s (origin %.1f,%.1f x%.0f tap %.2f; zoom %.3f m %.3f out/frame %.3f)",
                         crop.right > crop.left ? "" : "none ", crop.left, crop.top, crop.right, crop.bottom, backdropOn ? 1 : 0,
                         layerOn ? 1 : 0, patch ? 1 : 0, lupaOn ? 1 : 0, cursorOn ? 1 : 0, UiScale::DialogLoopRunning() ? 1 : 0,
                         level.mode, level.is4k ? " 4x" : "", level.originX, level.originY, level.scale, level.tap,
                         lupaOn ? lupa.zoom : 0.0f, lupaOn ? lupa.m : 1.0f, w > 0 ? static_cast<float>(vw) / static_cast<float>(w) : 0.0f);
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
        ID3D11ShaderResourceView* srvs[11] = { g_srv, layerOn ? g_layerOrigSrv : nullptr, layerOn ? g_layerHdSrv : nullptr,
                                               backdropOn ? g_backdropSrv : nullptr, lupaOn ? g_snapSrv : nullptr,
                                               (backdropOn && g_backShowOn) ? g_backShowSrv : nullptr,
                                               patch ? patch->refSrv : nullptr, patch ? patch->hdSrv : nullptr,
                                               cursorOn ? g_cursorSrv : nullptr,
                                               levelOn ? level.src : nullptr, levelOn ? level.ref : nullptr };
        g_ctx->PSSetShaderResources(0, 11, srvs);
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

        // the FPS line, notices and the F10 panel go into a copy of the frame (audit 2026-11-22: here F10 opened a
        // panel nobody could see, and it took the clicks)
        static std::vector<uint8_t> s_copy;
        s_copy.resize(static_cast<size_t>(pitch) * static_cast<size_t>(h));
        std::memcpy(s_copy.data(), bits, s_copy.size());
        RenderDDraw::DrawOverlayRGB565(s_copy.data(), pitch, w, h, bits, pitch);
        SettingsMenu::Draw(s_copy.data(), pitch, false, w, h, 0, 0, bits, pitch);

        int x, y, cw, ch;
        FitRect(w, h, outW, outH, x, y, cw, ch);
        HDC dc = GetDC(g_hwnd);
        if (!dc) return;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, x, y, cw, ch, 0, 0, w, h, s_copy.data(), reinterpret_cast<const BITMAPINFO*>(&bmi), DIB_RGB_COLORS, SRCCOPY);
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

    // A lost device (driver update, a GPU reset, sleep) used to leave the GDI fallback for the rest
    // of the session - no menu zoom, no HD layers, no smooth zoom (code review 2026-11-17). Direct3D 11
    // is tried again after 2, 5, 15 and 60 seconds.
    static DWORD retryAt = 0;
    static int retries = 0;
    if (g_backend == Backend::GDI && retryAt != 0 && GetTickCount() - retryAt < 0x80000000u) {
        retryAt = 0;
        if (InitD3D11()) {
            g_backend = Backend::D3D11;
            Logger::Info("Present: Direct3D 11 is back (attempt %d).", retries);
        } else {
            ReleaseD3D();
            static const DWORD kWait[] = { 2000, 5000, 15000, 60000 };
            if (retries < 4) retryAt = (GetTickCount() + kWait[retries++]) | 1u;
        }
    }
    if (g_backend == Backend::D3D11) {
        if (!FrameD3D11(bits, pitch, w, h)) {
            Logger::Warn("Present: Direct3D 11 frame failed - GDI for now, Direct3D 11 again in 2 s.");
            ReleaseD3D();
            g_backend = Backend::GDI;
            if (retries < 4) retryAt = (GetTickCount() + 2000) | 1u;
            ++retries;
        }
        if (g_capturePath) { // the frame ended before the dump point
            TestHarness::ShotDone(false, 0, 0);
            g_capturePath = nullptr;
        }
    }
    if (g_backend == Backend::GDI) {
        if (retryAt == 0) UiScale::MenuZoomUnavailable();      // for good: the menu zoom needs Direct3D 11
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

void LogNextFrames(int count) {
    g_logFrames.store(count < 0 ? 0 : (count > 600 ? 600 : count));
}

} // namespace DDM::Present
