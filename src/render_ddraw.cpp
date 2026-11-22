#include "render_ddraw.h"
#include "window_manager.h"
#include "timing.h"
#include "config.h"
#include "logger.h"
#include "pattern.h"
#include "diag.h"

#include <MinHook.h>
#include <ddraw.h>
#include <emmintrin.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace RenderDDraw {
    namespace {
        bool g_ddrawVtableHooked = false;
        bool g_surfaceVtableHooked = false;
        bool g_isGogD3DWrapper = false;

        // DirectDraw function pointer types
        using PFN_DirectDrawCreate   = HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*);
        using PFN_DirectDrawCreateEx = HRESULT(WINAPI*)(GUID*, LPVOID*, REFIID, IUnknown*);

        // IDirectDraw / IDirectDraw7 vtable methods
        using PFN_DD_QueryInterface      = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, LPVOID*);
        using PFN_DD7_CreateSurface      = HRESULT(STDMETHODCALLTYPE*)(void*, LPDDSURFACEDESC2, LPDIRECTDRAWSURFACE7*, IUnknown*);
        using PFN_DD7_SetCooperativeLevel = HRESULT(STDMETHODCALLTYPE*)(void*, HWND, DWORD);
        using PFN_DD7_SetDisplayMode     = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD);

        // IDirectDrawSurface7 vtable methods
        using PFN_DDS7_Blt     = HRESULT(STDMETHODCALLTYPE*)(void*, LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDBLTFX);
        using PFN_DDS7_BltFast = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, DWORD, LPDIRECTDRAWSURFACE7, LPRECT, DWORD);
        using PFN_DDS7_Flip    = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIRECTDRAWSURFACE7, DWORD);
        using PFN_DDS7_Lock    = HRESULT(STDMETHODCALLTYPE*)(void*, LPRECT, LPDDSURFACEDESC2, DWORD, HANDLE);
        using PFN_DDS7_Unlock  = HRESULT(STDMETHODCALLTYPE*)(void*, LPRECT);
        using PFN_DDS7_Restore = HRESULT(STDMETHODCALLTYPE*)(void*);

        PFN_DirectDrawCreate        o_DirectDrawCreate        = nullptr;
        PFN_DirectDrawCreateEx      o_DirectDrawCreateEx      = nullptr;
        PFN_DD_QueryInterface       o_DD_QueryInterface       = nullptr;
        PFN_DD7_CreateSurface       o_DD7_CreateSurface       = nullptr;
        PFN_DD7_SetCooperativeLevel o_DD7_SetCooperativeLevel = nullptr;
        PFN_DD7_SetDisplayMode      o_DD7_SetDisplayMode      = nullptr;

        PFN_DDS7_Blt     o_DDS7_Blt     = nullptr;
        PFN_DDS7_BltFast o_DDS7_BltFast = nullptr;
        PFN_DDS7_Flip    o_DDS7_Flip    = nullptr;
        PFN_DDS7_Lock    o_DDS7_Lock    = nullptr;
        PFN_DDS7_Unlock  o_DDS7_Unlock  = nullptr;
        PFN_DDS7_Restore o_DDS7_Restore = nullptr;

        void* g_primarySurface = nullptr;

        // 5x7 bitmap glyph table for fast zero-GDI RGB565 FPS Overlay (A-Z, 0-9, punctuation)
        const uint8_t* GetGlyph5x7(char c) {
            static const uint8_t kSpace[7]  = {0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t kDot[7]    = {0x00,0x00,0x00,0x00,0x00,0x06,0x06};
            static const uint8_t kColon[7]  = {0x00,0x06,0x06,0x00,0x06,0x06,0x00};
            static const uint8_t kPipe[7]   = {0x04,0x04,0x04,0x04,0x04,0x04,0x04};
            static const uint8_t kLParen[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02};
            static const uint8_t kRParen[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08};
            static const uint8_t kDash[7]   = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
            static const uint8_t kDigits[10][7] = {
                {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
                {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
                {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, // 2
                {0x1F,0x02,0x04,0x06,0x01,0x11,0x0E}, // 3
                {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
                {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
                {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
                {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
                {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
                {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}  // 9
            };
            static const uint8_t kLetters[26][7] = {
                {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A
                {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
                {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
                {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, // D
                {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
                {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F
                {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // G
                {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // H
                {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // I
                {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // J
                {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
                {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
                {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
                {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // N
                {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
                {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
                {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // Q
                {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
                {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // S
                {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
                {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // U
                {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // V
                {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, // W
                {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
                {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // Y
                {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}  // Z
            };

            if (c >= '0' && c <= '9') return kDigits[c - '0'];
            if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
            if (c >= 'a' && c <= 'z') return kLetters[c - 'a'];
            if (c == '.') return kDot;
            if (c == ':') return kColon;
            if (c == '|') return kPipe;
            if (c == '(') return kLParen;
            if (c == ')') return kRParen;
            if (c == '-') return kDash;
            return kSpace;
        }

        void DrawFPSOverlayOnSurface(LPDIRECTDRAWSURFACE7 surf) {
            if (!surf || !SherwoodConfig::Get().showFPSOverlay || !o_DDS7_Lock || !o_DDS7_Unlock) {
                return;
            }

            static uint64_t s_lastTimeUs = 0;
            static uint32_t s_frames = 0;
            static double   s_displayFps = 60.0;
            static double   s_displayMs  = 16.6;

            const uint64_t nowUs = Timing::GetElapsedMicroseconds();
            if (s_lastTimeUs == 0) {
                s_lastTimeUs = nowUs;
            }
            ++s_frames;
            const uint64_t elapsedUs = nowUs - s_lastTimeUs;
            if (elapsedUs >= 250000ULL) {
                s_displayFps = (static_cast<double>(s_frames) * 1000000.0) / static_cast<double>(elapsedUs);
                s_displayMs  = (s_displayFps > 0.1) ? (1000.0 / s_displayFps) : 0.0;
                s_frames     = 0;
                s_lastTimeUs = nowUs;
            }

            DDSURFACEDESC2 sd{};
            sd.dwSize = sizeof(sd);
            if (FAILED(surf->GetSurfaceDesc(&sd)) ||
                sd.dwWidth < 320 || sd.dwHeight < 32 ||
                sd.ddpfPixelFormat.dwRGBBitCount != 16) {
                return;
            }

            int resW = 0, resH = 0;
            WindowManager::GetInternalResolution(resW, resH);

            char text[96]{};
            std::snprintf(text, sizeof(text), "FPS: %.1f (%.1f MS) | %dX%d",
                          s_displayFps, s_displayMs, resW, resH);

            const int scale = 2; // 10x14 px per char
            const int startX = 10;
            const int startY = 8;
            const int textLen = static_cast<int>(std::strlen(text));
            const int boxW = textLen * (6 * scale) + 12;
            const int boxH = 7 * scale + 10;

            RECT rcOverlay{};
            rcOverlay.left   = 0;
            rcOverlay.top    = 0;
            rcOverlay.right  = std::min<LONG>(startX - 6 + boxW + 2, static_cast<LONG>(sd.dwWidth));
            rcOverlay.bottom = std::min<LONG>(startY - 4 + boxH + 2, static_cast<LONG>(sd.dwHeight));
            if (rcOverlay.right <= startX || rcOverlay.bottom <= startY) {
                return;
            }

            DDSURFACEDESC2 desc{};
            desc.dwSize = sizeof(desc);
            if (FAILED(o_DDS7_Lock(surf, &rcOverlay, &desc, DDLOCK_WAIT | DDLOCK_NOSYSLOCK, nullptr))) {
                return;
            }

            const int maxX = std::min<int>(rcOverlay.right, static_cast<int>(sd.dwWidth));
            const int maxY = std::min<int>(rcOverlay.bottom, static_cast<int>(sd.dwHeight));
            if (desc.lpSurface && desc.ddpfPixelFormat.dwRGBBitCount == 16) {
                auto* base = static_cast<uint8_t*>(desc.lpSurface);
                const LONG pitch = desc.lPitch;

                // Dark translucent-shaded background box
                for (int y = startY - 4; y < startY + boxH - 4 && y < maxY; ++y) {
                    auto* row = reinterpret_cast<uint16_t*>(base + y * pitch);
                    for (int x = startX - 6; x < startX + boxW - 6 && x < maxX; ++x) {
                        row[x] = (row[x] & 0xF7DEu) >> 1; // 50% darken existing RGB565 pixel
                    }
                }

                // Bright emerald green RGB565 (0x07E0) text
                const uint16_t fgColor = 0x07E0u;
                for (int i = 0; i < textLen; ++i) {
                    const uint8_t* glyph = GetGlyph5x7(text[i]);
                    const int charX = startX + i * (6 * scale);
                    for (int gy = 0; gy < 7; ++gy) {
                        uint8_t bits = glyph[gy];
                        for (int gx = 0; gx < 5; ++gx) {
                            if (bits & (0x10 >> gx)) {
                                for (int sy = 0; sy < scale; ++sy) {
                                    int py = startY + gy * scale + sy;
                                    if (py >= maxY) continue;
                                    auto* row = reinterpret_cast<uint16_t*>(base + py * pitch);
                                    for (int sx = 0; sx < scale; ++sx) {
                                        int px = charX + gx * scale + sx;
                                        if (px < maxX) {
                                            row[px] = fgColor;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            o_DDS7_Unlock(surf, &rcOverlay);
        }

        // Forward declarations
        void HookSurfaceVtable(void* pSurface7);
        void HookDDraw7Vtable(void* pDD7);

        HRESULT STDMETHODCALLTYPE Hook_DDS7_Lock(void* pThis, LPRECT lpDestRect, LPDDSURFACEDESC2 lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent) {
            Diag::Scope diagScope(Diag::C_DD_LOCK);
            // Enforce DDLOCK_NOSYSLOCK | DDLOCK_WAIT to eliminate Win16/DDraw global lock contention on Windows 11
            dwFlags |= (DDLOCK_NOSYSLOCK | DDLOCK_WAIT);
            HRESULT hr = o_DDS7_Lock(pThis, lpDestRect, lpDDSurfaceDesc, dwFlags, hEvent);
            if (hr == DDERR_SURFACELOST && o_DDS7_Restore) {
                o_DDS7_Restore(pThis);
                hr = o_DDS7_Lock(pThis, lpDestRect, lpDDSurfaceDesc, dwFlags, hEvent);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE Hook_DDS7_Unlock(void* pThis, LPRECT lpRect) {
            Diag::Scope diagScope(Diag::C_DD_UNLOCK);
            return o_DDS7_Unlock(pThis, lpRect);
        }

        HRESULT STDMETHODCALLTYPE Hook_DDS7_Blt(void* pThis, LPRECT lpDestRect, LPDIRECTDRAWSURFACE7 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx) {
            if (pThis == g_primarySurface && lpDDSrcSurface) {
                const bool srcFull = (!lpSrcRect || (lpSrcRect->right - lpSrcRect->left) >= 320);
                const bool dstFull = (!lpDestRect || (lpDestRect->right - lpDestRect->left) >= 320);
                if (srcFull && dstFull) {
                    Diag::Add(Diag::C_PRESENT);
                    if (SherwoodConfig::Get().showFPSOverlay) {
                        DrawFPSOverlayOnSurface(lpDDSrcSurface);
                    }
                }
            }
            Diag::Scope diagScope(Diag::C_DD_BLT);
            HRESULT hr = o_DDS7_Blt(pThis, lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx);
            if (hr == DDERR_INVALIDRECT && lpDDSrcSurface && lpSrcRect) {
                // High-res (1080p/1440p/4K) small-map guard: clip source/dest rects to actual surface bounds
                DDSURFACEDESC2 srcDesc{};
                srcDesc.dwSize = sizeof(srcDesc);
                DDSURFACEDESC2 dstDesc{};
                dstDesc.dwSize = sizeof(dstDesc);
                auto* dstSurf = static_cast<LPDIRECTDRAWSURFACE7>(pThis);
                if (SUCCEEDED(lpDDSrcSurface->GetSurfaceDesc(&srcDesc)) &&
                    SUCCEEDED(dstSurf->GetSurfaceDesc(&dstDesc))) {
                    RECT clippedSrc = *lpSrcRect;
                    RECT clippedDst = lpDestRect ? *lpDestRect : RECT{0, 0, static_cast<LONG>(dstDesc.dwWidth), static_cast<LONG>(dstDesc.dwHeight)};

                    if (clippedSrc.left < 0) { clippedDst.left -= clippedSrc.left; clippedSrc.left = 0; }
                    if (clippedSrc.top < 0)  { clippedDst.top  -= clippedSrc.top;  clippedSrc.top  = 0; }
                    if (clippedSrc.right > static_cast<LONG>(srcDesc.dwWidth)) {
                        clippedDst.right -= (clippedSrc.right - static_cast<LONG>(srcDesc.dwWidth));
                        clippedSrc.right = static_cast<LONG>(srcDesc.dwWidth);
                    }
                    if (clippedSrc.bottom > static_cast<LONG>(srcDesc.dwHeight)) {
                        clippedDst.bottom -= (clippedSrc.bottom - static_cast<LONG>(srcDesc.dwHeight));
                        clippedSrc.bottom = static_cast<LONG>(srcDesc.dwHeight);
                    }
                    clippedDst.right  = std::min(clippedDst.right,  static_cast<LONG>(dstDesc.dwWidth));
                    clippedDst.bottom = std::min(clippedDst.bottom, static_cast<LONG>(dstDesc.dwHeight));

                    // Clear destination surface to black first to avoid hall-of-mirrors outside small maps
                    DDBLTFX fillFx{};
                    fillFx.dwSize = sizeof(fillFx);
                    fillFx.dwFillColor = 0;
                    o_DDS7_Blt(pThis, nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fillFx);

                    if (clippedSrc.right > clippedSrc.left && clippedSrc.bottom > clippedSrc.top &&
                        clippedDst.right > clippedDst.left && clippedDst.bottom > clippedDst.top) {
                        hr = o_DDS7_Blt(pThis, &clippedDst, lpDDSrcSurface, &clippedSrc, dwFlags, lpDDBltFx);
                    } else {
                        hr = DD_OK;
                    }
                }
            }
            if (hr == DDERR_SURFACELOST || hr == DDERR_INVALIDRECT) {
                return DD_OK;
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE Hook_DDS7_BltFast(void* pThis, DWORD dwX, DWORD dwY, LPDIRECTDRAWSURFACE7 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans) {
            HRESULT hr = o_DDS7_BltFast(pThis, dwX, dwY, lpDDSrcSurface, lpSrcRect, dwTrans);
            if (hr == DDERR_INVALIDRECT && lpDDSrcSurface && lpSrcRect) {
                DDSURFACEDESC2 srcDesc{};
                srcDesc.dwSize = sizeof(srcDesc);
                DDSURFACEDESC2 dstDesc{};
                dstDesc.dwSize = sizeof(dstDesc);
                auto* dstSurf = static_cast<LPDIRECTDRAWSURFACE7>(pThis);
                if (SUCCEEDED(lpDDSrcSurface->GetSurfaceDesc(&srcDesc)) &&
                    SUCCEEDED(dstSurf->GetSurfaceDesc(&dstDesc))) {
                    RECT clippedSrc = *lpSrcRect;
                    LONG dstX = static_cast<LONG>(dwX);
                    LONG dstY = static_cast<LONG>(dwY);

                    if (clippedSrc.left < 0) { dstX -= clippedSrc.left; clippedSrc.left = 0; }
                    if (clippedSrc.top < 0)  { dstY -= clippedSrc.top;  clippedSrc.top  = 0; }
                    if (dstX < 0) { clippedSrc.left -= dstX; dstX = 0; }
                    if (dstY < 0) { clippedSrc.top  -= dstY; dstY = 0; }

                    clippedSrc.right  = std::min(clippedSrc.right,  static_cast<LONG>(srcDesc.dwWidth));
                    clippedSrc.bottom = std::min(clippedSrc.bottom, static_cast<LONG>(srcDesc.dwHeight));

                    LONG maxW = static_cast<LONG>(dstDesc.dwWidth)  - dstX;
                    LONG maxH = static_cast<LONG>(dstDesc.dwHeight) - dstY;
                    if (clippedSrc.right - clippedSrc.left > maxW) clippedSrc.right = clippedSrc.left + maxW;
                    if (clippedSrc.bottom - clippedSrc.top > maxH) clippedSrc.bottom = clippedSrc.top + maxH;

                    if (clippedSrc.right > clippedSrc.left && clippedSrc.bottom > clippedSrc.top) {
                        hr = o_DDS7_BltFast(pThis, static_cast<DWORD>(dstX), static_cast<DWORD>(dstY),
                                            lpDDSrcSurface, &clippedSrc, dwTrans);
                    } else {
                        hr = DD_OK;
                    }
                }
            }
            if (hr == DDERR_SURFACELOST || hr == DDERR_INVALIDRECT) {
                return DD_OK;
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE Hook_DDS7_Flip(void* pThis, LPDIRECTDRAWSURFACE7 lpDDSurfaceTargetOverride, DWORD dwFlags) {
            if (lpDDSurfaceTargetOverride) {
                DrawFPSOverlayOnSurface(lpDDSurfaceTargetOverride);
            } else {
                DrawFPSOverlayOnSurface(static_cast<LPDIRECTDRAWSURFACE7>(pThis));
            }

            if (!SherwoodConfig::Get().vsync) {
                dwFlags |= DDFLIP_NOVSYNC;
            }
            Diag::Add(Diag::C_PRESENT);
            Diag::Scope diagScope(Diag::C_DD_FLIP);
            HRESULT hr = o_DDS7_Flip(pThis, lpDDSurfaceTargetOverride, dwFlags);
            if (hr == DDERR_SURFACELOST) {
                return DD_OK;
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE Hook_DDS7_Restore(void* pThis) {
            HRESULT hr = o_DDS7_Restore(pThis);
            if (hr == DDERR_WRONGMODE || hr == DDERR_NOEXCLUSIVEMODE) {
                return DD_OK;
            }
            return hr;
        }

        void HookSurfaceVtable(void* pSurface7) {
            if (!pSurface7 || g_surfaceVtableHooked) return;
            void** vtable = *reinterpret_cast<void***>(pSurface7);
            if (!vtable) return;

            auto HookSlot = [&](size_t index, LPVOID detour, LPVOID* orig) {
                if (MH_CreateHook(vtable[index], detour, orig) == MH_OK) {
                    MH_EnableHook(vtable[index]);
                }
            };

            // IDirectDrawSurface7 vtable indices:
            // 5: Blt (+0x14), 7: BltFast (+0x1C), 11: Flip (+0x2C), 25: Lock (+0x64), 27: Restore (+0x6C), 32: Unlock (+0x80)
            HookSlot(5,  reinterpret_cast<LPVOID>(&Hook_DDS7_Blt),     reinterpret_cast<LPVOID*>(&o_DDS7_Blt));
            HookSlot(7,  reinterpret_cast<LPVOID>(&Hook_DDS7_BltFast), reinterpret_cast<LPVOID*>(&o_DDS7_BltFast));
            HookSlot(11, reinterpret_cast<LPVOID>(&Hook_DDS7_Flip),    reinterpret_cast<LPVOID*>(&o_DDS7_Flip));
            HookSlot(25, reinterpret_cast<LPVOID>(&Hook_DDS7_Lock),    reinterpret_cast<LPVOID*>(&o_DDS7_Lock));
            HookSlot(27, reinterpret_cast<LPVOID>(&Hook_DDS7_Restore), reinterpret_cast<LPVOID*>(&o_DDS7_Restore));
            HookSlot(32, reinterpret_cast<LPVOID>(&Hook_DDS7_Unlock),  reinterpret_cast<LPVOID*>(&o_DDS7_Unlock));

            g_surfaceVtableHooked = true;
            Logger::Info("RenderDDraw: IDirectDrawSurface7 vtable hooked (Lock/Unlock/Blt/Flip/Restore).");
        }

        HRESULT STDMETHODCALLTYPE Hook_DD7_CreateSurface(
            void* pThis, LPDDSURFACEDESC2 lpDDSurfaceDesc2, LPDIRECTDRAWSURFACE7* lplpDDSurface, IUnknown* pUnkOuter)
        {
            auto& cfg = SherwoodConfig::Get();
            bool isPrimary = false;

            if (lpDDSurfaceDesc2) {
                DWORD& caps = lpDDSurfaceDesc2->ddsCaps.dwCaps;
                isPrimary = (caps & DDSCAPS_PRIMARYSURFACE) != 0;

                if (!isPrimary && cfg.forceSystemMemorySurfaces && !g_isGogD3DWrapper) {
                    // Eliminate GPU/PCIe stalls on CPU Lock/Unlock when running on native Windows ddraw.dll
                    if (caps & (DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_OFFSCREENPLAIN)) {
                        caps &= ~(DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM);
                        caps |= DDSCAPS_SYSTEMMEMORY;
                    }
                }
            }

            HRESULT hr = o_DD7_CreateSurface(pThis, lpDDSurfaceDesc2, lplpDDSurface, pUnkOuter);
            if (SUCCEEDED(hr) && lplpDDSurface && *lplpDDSurface) {
                if (isPrimary) {
                    g_primarySurface = *lplpDDSurface;
                }
                HookSurfaceVtable(*lplpDDSurface);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE Hook_DD7_SetCooperativeLevel(void* pThis, HWND hWnd, DWORD dwFlags) {
            if (SherwoodConfig::Get().windowedBorderless) {
                // Remove DDSCL_ALLOWREBOOT and prevent exclusive mode lockups on Windows 11 Alt+Tab
                dwFlags &= ~DDSCL_ALLOWREBOOT;
            }
            // Request multithreaded-safe DirectDraw behavior when SBInputThreadProc runs on a separate core
            const DWORD mtFlags = dwFlags | DDSCL_MULTITHREADED;
            HRESULT hr = o_DD7_SetCooperativeLevel(pThis, hWnd, mtFlags);
            if (FAILED(hr) && mtFlags != dwFlags) {
                hr = o_DD7_SetCooperativeLevel(pThis, hWnd, dwFlags);
                Logger::Info("IDirectDraw7::SetCooperativeLevel(hWnd=0x%08X, dwFlags=0x%08X) -> 0x%08X",
                             reinterpret_cast<uintptr_t>(hWnd), dwFlags, hr);
            } else {
                Logger::Info("IDirectDraw7::SetCooperativeLevel(hWnd=0x%08X, dwFlags=0x%08X) -> 0x%08X",
                             reinterpret_cast<uintptr_t>(hWnd), mtFlags, hr);
            }
            if (FAILED(hr) && SherwoodConfig::Get().windowedBorderless) {
                Logger::Warn("SetCooperativeLevel returned 0x%08X; forcing DD_OK for borderless stability.", hr);
                return DD_OK;
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE Hook_DD7_SetDisplayMode(
            void* pThis, DWORD dwWidth, DWORD dwHeight, DWORD dwBPP, DWORD dwRefreshRate, DWORD dwFlags)
        {
            auto& cfg = SherwoodConfig::Get();
            if (g_isGogD3DWrapper && cfg.windowedBorderless && dwRefreshRate == 0) {
                dwRefreshRate = (cfg.maxFPS > 60) ? static_cast<DWORD>(cfg.maxFPS) : 120;
            }
            Logger::Info("IDirectDraw7::SetDisplayMode(%u x %u, %u bpp, %u Hz)",
                         dwWidth, dwHeight, dwBPP, dwRefreshRate);
            WindowManager::SetInternalResolution(static_cast<int>(dwWidth), static_cast<int>(dwHeight));
            return o_DD7_SetDisplayMode(pThis, dwWidth, dwHeight, dwBPP, dwRefreshRate, dwFlags);
        }

        void HookDDraw7Vtable(void* pDD7) {
            if (!pDD7 || g_ddrawVtableHooked) return;
            void** vtable = *reinterpret_cast<void***>(pDD7);
            if (!vtable) return;

            auto HookSlot = [&](size_t index, LPVOID detour, LPVOID* orig) {
                if (MH_CreateHook(vtable[index], detour, orig) == MH_OK) {
                    MH_EnableHook(vtable[index]);
                }
            };

            // IDirectDraw7 vtable indices:
            // 6: CreateSurface (+0x18), 20: SetCooperativeLevel (+0x50), 21: SetDisplayMode (+0x54)
            HookSlot(6,  reinterpret_cast<LPVOID>(&Hook_DD7_CreateSurface),       reinterpret_cast<LPVOID*>(&o_DD7_CreateSurface));
            if (!o_DD7_SetCooperativeLevel) {
                HookSlot(20, reinterpret_cast<LPVOID>(&Hook_DD7_SetCooperativeLevel), reinterpret_cast<LPVOID*>(&o_DD7_SetCooperativeLevel));
            }
            HookSlot(21, reinterpret_cast<LPVOID>(&Hook_DD7_SetDisplayMode),      reinterpret_cast<LPVOID*>(&o_DD7_SetDisplayMode));

            g_ddrawVtableHooked = true;
            Logger::Info("RenderDDraw: IDirectDraw7 vtable hooked (CreateSurface/SetCooperativeLevel/SetDisplayMode).");
        }

        HRESULT STDMETHODCALLTYPE Hook_DD_QueryInterface(void* pThis, REFIID riid, LPVOID* ppvObj) {
            HRESULT hr = o_DD_QueryInterface(pThis, riid, ppvObj);
            if (SUCCEEDED(hr) && ppvObj && *ppvObj) {
                // Check for IID_IDirectDraw7: {15E65EC0-3B9C-11D2-B92F-00609797EA5B}
                if (riid.Data1 == 0x15E65EC0 && riid.Data2 == 0x3B9C && riid.Data3 == 0x11D2) {
                    HookDDraw7Vtable(*ppvObj);
                }
            }
            return hr;
        }

        HRESULT WINAPI Hook_DirectDrawCreate(GUID* lpGUID, LPDIRECTDRAW* lplpDD, IUnknown* pUnkOuter) {
            HRESULT hr = o_DirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
            if (SUCCEEDED(hr) && lplpDD && *lplpDD) {
                void** vtable = *reinterpret_cast<void***>(*lplpDD);
                if (vtable) {
                    if (!o_DD_QueryInterface) {
                        if (MH_CreateHook(vtable[0], reinterpret_cast<LPVOID>(&Hook_DD_QueryInterface),
                                          reinterpret_cast<LPVOID*>(&o_DD_QueryInterface)) == MH_OK) {
                            MH_EnableHook(vtable[0]);
                        }
                    }
                    // Game.exe calls SetCooperativeLevel on IDirectDraw (v1) before QueryInterface(IID_IDirectDraw7);
                    // slot 20 has identical signature (pThis, hWnd, dwFlags) across IDirectDraw and IDirectDraw7.
                    if (!o_DD7_SetCooperativeLevel) {
                        if (MH_CreateHook(vtable[20], reinterpret_cast<LPVOID>(&Hook_DD7_SetCooperativeLevel),
                                          reinterpret_cast<LPVOID*>(&o_DD7_SetCooperativeLevel)) == MH_OK) {
                            MH_EnableHook(vtable[20]);
                        }
                    }
                }
            }
            return hr;
        }

        HRESULT WINAPI Hook_DirectDrawCreateEx(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* pUnkOuter) {
            HRESULT hr = o_DirectDrawCreateEx(lpGuid, lplpDD, iid, pUnkOuter);
            if (SUCCEEDED(hr) && lplpDD && *lplpDD) {
                HookDDraw7Vtable(*lplpDD);
            }
            return hr;
        }

        void PatchSBDrawManagerSurfaceCaps() {
            if (!SherwoodConfig::Get().forceSystemMemorySurfaces || g_isGogD3DWrapper) return;

            HMODULE hExe = GetModuleHandleA(nullptr);
            // Pattern in SBDrawManager::CreateSurface (0x005E4FF1):
            // A8 02 74 0B C7 84 24 B8 00 00 00 40 40 00 00
            // Replaces 0x00004040 (DDSCAPS_VIDEOMEMORY | DDSCAPS_OFFSCREENPLAIN)
            // with     0x00000840 (DDSCAPS_SYSTEMMEMORY | DDSCAPS_OFFSCREENPLAIN)
            uintptr_t addr = Memory::FindPattern(hExe, "A8 02 74 0B C7 84 24 B8 00 00 00 40 40 00 00");
            if (addr) {
                uint32_t sysMemCaps = 0x00000840;
                if (Memory::Write(addr + 11, sysMemCaps)) {
                    Logger::Info("Patched SBDrawManager::CreateSurface @ 0x%08X: 0x4040 (VRAM) -> 0x0840 (RAM).", addr + 11);
                }
            } else {
                Logger::Warn("SBDrawManager::CreateSurface VRAM pattern not found (will rely on COM CreateSurface hook).");
            }
        }

        void SyncWrapperConfigForHighRes() {
            auto& cfg = SherwoodConfig::Get();
            char path[MAX_PATH]{};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            std::string dir(path);
            size_t pos = dir.find_last_of("\\/");
            std::string dxcfgPath = ((pos != std::string::npos) ? dir.substr(0, pos + 1) : "") + "dxcfg.ini";

            if (GetFileAttributesA(dxcfgPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_isGogD3DWrapper = true;
                // Ensure GOG's D3D9 presentation wrapper uses borderless windowed presentation,
                // desktop resolution (1440p/4K), aspect ratio preservation, and doesn't throttle FPS.
                WritePrivateProfileStringA("general", "presentation", cfg.windowedBorderless ? "windowed" : "application", dxcfgPath.c_str());
                WritePrivateProfileStringA("general", "display", cfg.windowedBorderless ? "desktop" : "application", dxcfgPath.c_str());
                WritePrivateProfileStringA("general", "aspect", cfg.fixAspectRatio ? "enabled" : "disabled", dxcfgPath.c_str());
                WritePrivateProfileStringA("general", "scaling", "fit", dxcfgPath.c_str());
                WritePrivateProfileStringA("general", "vsync", cfg.vsync ? "enabled" : "disabled", dxcfgPath.c_str());
                WritePrivateProfileStringA("additional", "fpslimit", "0", dxcfgPath.c_str());
                Logger::Info("Synchronized dxcfg.ini: presentation=%s, display=%s, VSync=%d.",
                             cfg.windowedBorderless ? "windowed" : "application",
                             cfg.windowedBorderless ? "desktop" : "application",
                             cfg.vsync ? 1 : 0);
            }
        }
    }

    bool Init() {
        SyncWrapperConfigForHighRes();
        PatchSBDrawManagerSurfaceCaps();

        HMODULE hDDraw = GetModuleHandleA("ddraw.dll");
        if (!hDDraw) {
            hDDraw = LoadLibraryA("ddraw.dll");
        }
        if (!hDDraw) {
            Logger::Error("RenderDDraw: Failed to locate ddraw.dll module.");
            return false;
        }

        FARPROC pDDCreate = GetProcAddress(hDDraw, "DirectDrawCreate");
        if (pDDCreate) {
            if (MH_CreateHook(reinterpret_cast<LPVOID>(pDDCreate),
                              reinterpret_cast<LPVOID>(&Hook_DirectDrawCreate),
                              reinterpret_cast<LPVOID*>(&o_DirectDrawCreate)) == MH_OK) {
                MH_EnableHook(reinterpret_cast<LPVOID>(pDDCreate));
                Logger::Info("Hooked DirectDrawCreate @ 0x%08X", reinterpret_cast<uintptr_t>(pDDCreate));
            }
        }

        FARPROC pDDCreateEx = GetProcAddress(hDDraw, "DirectDrawCreateEx");
        if (pDDCreateEx) {
            if (MH_CreateHook(reinterpret_cast<LPVOID>(pDDCreateEx),
                              reinterpret_cast<LPVOID>(&Hook_DirectDrawCreateEx),
                              reinterpret_cast<LPVOID*>(&o_DirectDrawCreateEx)) == MH_OK) {
                MH_EnableHook(reinterpret_cast<LPVOID>(pDDCreateEx));
                Logger::Info("Hooked DirectDrawCreateEx @ 0x%08X", reinterpret_cast<uintptr_t>(pDDCreateEx));
            }
        }

        return true;
    }

    void Shutdown() {
        g_primarySurface = nullptr;
    }

    void ConvertRGB565ToXRGB8888_SSE2(
        const uint16_t* src, int srcPitchBytes,
        uint32_t* dst, int dstPitchBytes,
        int width, int height)
    {
        const __m128i maskR = _mm_set1_epi16(static_cast<short>(0xF800));
        const __m128i maskG = _mm_set1_epi16(0x07E0);
        const __m128i maskB = _mm_set1_epi16(0x001F);
        const __m128i alpha = _mm_set1_epi16(static_cast<short>(0xFF00));

        const auto* srcRow = reinterpret_cast<const uint8_t*>(src);
        auto* dstRow       = reinterpret_cast<uint8_t*>(dst);

        for (int y = 0; y < height; ++y) {
            const auto* s = reinterpret_cast<const uint16_t*>(srcRow);
            auto* d       = reinterpret_cast<uint32_t*>(dstRow);
            int x = 0;

            for (; x + 8 <= width; x += 8) {
                __m128i pix = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + x));

                // Extract R, G, B 8-bit components into 16-bit lanes
                __m128i r = _mm_and_si128(pix, maskR);
                r = _mm_or_si128(_mm_srli_epi16(r, 8), _mm_srli_epi16(r, 13));

                __m128i g = _mm_and_si128(pix, maskG);
                g = _mm_or_si128(_mm_srli_epi16(g, 3), _mm_srli_epi16(g, 9));

                __m128i b = _mm_and_si128(pix, maskB);
                b = _mm_or_si128(_mm_slli_epi16(b, 3), _mm_srli_epi16(b, 2));

                // Pack BGRA: low byte B, high byte G; low byte R, high byte A (0xFF)
                __m128i bg = _mm_or_si128(b, _mm_slli_epi16(g, 8));
                __m128i ra = _mm_or_si128(r, alpha);

                __m128i outLo = _mm_unpacklo_epi16(bg, ra);
                __m128i outHi = _mm_unpackhi_epi16(bg, ra);

                _mm_storeu_si128(reinterpret_cast<__m128i*>(d + x), outLo);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(d + x + 4), outHi);
            }

            for (; x < width; ++x) {
                uint16_t p = s[x];
                uint32_t r = ((p >> 11) & 0x1F);
                uint32_t g = ((p >> 5)  & 0x3F);
                uint32_t b = (p         & 0x1F);
                r = (r << 3) | (r >> 2);
                g = (g << 2) | (g >> 4);
                b = (b << 3) | (b >> 2);
                d[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }

            srcRow += srcPitchBytes;
            dstRow += dstPitchBytes;
        }
    }

    // ---- notice: a short message from the mod itself, shown for a few seconds ------------------
    namespace {
        char     g_noticeText[128]{};
        uint64_t g_noticeUntilUs = 0;

        // One line of 5x7 text on a darkened box, horizontally centred, into an RGB565 frame.
        // `src` (optional): the same picture in ordinary memory - the box is darkened from it, so
        // `base` (a mapped, write-combined texture) is only written, never read.
        void DrawTextLine565(uint8_t* base, LONG pitch, int width, int height, const char* text,
                             int startY, int scale, uint16_t color, const uint8_t* src = nullptr, LONG srcPitch = 0) {
            const int textLen = static_cast<int>(std::strlen(text));
            const int startX = std::max(10, (width - textLen * (6 * scale)) / 2);
            const int boxW = textLen * (6 * scale) + 12;
            const int boxH = 7 * scale + 10;
            const int maxX = std::min(startX - 6 + boxW, width);
            const int maxY = std::min(startY - 4 + boxH, height);
            for (int y = std::max(0, startY - 4); y < maxY; ++y) {
                auto* row = reinterpret_cast<uint16_t*>(base + y * pitch);
                const auto* from = src ? reinterpret_cast<const uint16_t*>(src + y * srcPitch) : row;
                for (int x = startX - 6; x < maxX; ++x) {
                    row[x] = static_cast<uint16_t>((from[x] & 0xF7DEu) >> 1); // darken 50 %
                }
            }
            for (int i = 0; i < textLen; ++i) {
                const uint8_t* glyph = GetGlyph5x7(text[i]);
                const int charX = startX + i * (6 * scale);
                for (int gy = 0; gy < 7; ++gy) {
                    const uint8_t bits = glyph[gy];
                    for (int gx = 0; gx < 5; ++gx) {
                        if (!(bits & (0x10 >> gx))) continue;
                        for (int sy = 0; sy < scale; ++sy) {
                            const int py = startY + gy * scale + sy;
                            if (py < 0 || py >= maxY) continue;
                            auto* row = reinterpret_cast<uint16_t*>(base + py * pitch);
                            for (int sx = 0; sx < scale; ++sx) {
                                const int px = charX + gx * scale + sx;
                                if (px < maxX) {
                                    row[px] = color;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void ShowNotice(const char* text, double seconds) {
        std::snprintf(g_noticeText, sizeof(g_noticeText), "%s", text ? text : "");
        g_noticeUntilUs = Timing::GetElapsedMicroseconds() + static_cast<uint64_t>(seconds * 1000000.0);
    }

    // FPS overlay and notices for the in-memory DirectDraw presenter: drawn straight into an RGB565 frame.
    void DrawOverlayRGB565(uint8_t* base, LONG pitch, int width, int height, const uint8_t* src, LONG srcPitch) {
        if (!base || width < 320 || height < 64) {
            return;
        }
        if (g_noticeText[0]) {
            if (Timing::GetElapsedMicroseconds() < g_noticeUntilUs) {
                const int scale = (height >= 900) ? 3 : 2;
                DrawTextLine565(base, pitch, width, height, g_noticeText, (height >= 900) ? 44 : 30, scale, 0xFFE0u, src, srcPitch);
            } else {
                g_noticeText[0] = 0;
            }
        }
        if (!SherwoodConfig::Get().showFPSOverlay) {
            return;
        }

        static uint64_t s_lastTimeUs = 0;
        static uint32_t s_frames = 0;
        static double   s_displayFps = 0.0;
        static double   s_displayMs  = 0.0;

        const uint64_t nowUs = Timing::GetElapsedMicroseconds();
        if (s_lastTimeUs == 0) {
            s_lastTimeUs = nowUs;
        }
        ++s_frames;
        const uint64_t elapsedUs = nowUs - s_lastTimeUs;
        if (elapsedUs >= 250000ULL) {
            s_displayFps = (static_cast<double>(s_frames) * 1000000.0) / static_cast<double>(elapsedUs);
            s_displayMs  = (s_displayFps > 0.1) ? (1000.0 / s_displayFps) : 0.0;
            s_frames     = 0;
            s_lastTimeUs = nowUs;
        }

        char text[96]{};
        std::snprintf(text, sizeof(text), "FPS: %.1f (%.1f MS) | %dX%d", s_displayFps, s_displayMs, width, height);

        // Top centre: the game prints its own lines (money, clover) in the top-left corner and
        // keeps the minimap buttons in the top-right one.
        DrawTextLine565(base, pitch, width, height, text, 8, (height >= 900) ? 2 : 1, 0x07E0u, src, srcPitch);
    }
}
