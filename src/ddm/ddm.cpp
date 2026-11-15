#include "ddm.h"
#include "ddm_blit.h"
#include "ddm_hd.h"
#include "ddm_hd_font.h"
#include "ddm_hd_menu.h"
#include "ddm_hd_patch.h"
#include "ddm_hd_sprites.h"
#include "ddm_present.h"
#include "../config.h"
#include "../diag.h"
#include "../logger.h"
#include "../window_manager.h"
#include "../input_fix.h"
#include "../ui_scale.h"
#include "../watchdog.h"

#include <MinHook.h>
#include <ddraw.h>
#include <malloc.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace DDM {
namespace {

#define DDM_UNSUPPORTED(name)                                          \
    do {                                                                \
        static bool s_once = false;                                     \
        if (!s_once) {                                                  \
            s_once = true;                                              \
            Logger::Warn("DDM: unsupported call %s", name);             \
        }                                                               \
        return DDERR_UNSUPPORTED;                                       \
    } while (0)

class Surface;
class DDraw7;
class DDraw1;

struct State {
    std::recursive_mutex lock;   // surface list, display mode, primary chain
    HWND  hwnd = nullptr;
    DWORD coopFlags = 0;
    int   modeW = 0, modeH = 0;
    bool  modeSet = false;
    Surface* primary = nullptr;
    DDraw7*  dd7 = nullptr;
    std::vector<Surface*> surfaces;
    unsigned created = 0;
    bool active = false;
} g;

void GuidToString(REFIID iid, char* out, size_t n) {
    std::snprintf(out, n, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
                  iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
}

void CurrentMode(int& w, int& h) {
    if (g.modeSet && g.modeW > 0 && g.modeH > 0) {
        w = g.modeW;
        h = g.modeH;
    } else {
        WindowManager::GetMonitorResolution(w, h);
    }
}

void FillPixelFormat565(DDPIXELFORMAT& pf) {
    std::memset(&pf, 0, sizeof(pf));
    pf.dwSize = sizeof(DDPIXELFORMAT);
    pf.dwFlags = DDPF_RGB;
    pf.dwRGBBitCount = 16;
    pf.dwRBitMask = 0xF800;
    pf.dwGBitMask = 0x07E0;
    pf.dwBBitMask = 0x001F;
}

// ------------------------------------------------------------------------------------ clipper
class Clipper final : public IDirectDrawClipper {
public:
    LONG refs = 1;
    HWND hwnd = nullptr;

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectDrawClipper)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    STDMETHOD_(ULONG, Release)() override {
        const LONG r = InterlockedDecrement(&refs);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    STDMETHOD(GetClipList)(LPRECT, LPRGNDATA, LPDWORD) override { DDM_UNSUPPORTED("IDirectDrawClipper::GetClipList"); }
    STDMETHOD(GetHWnd)(HWND* out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = hwnd;
        return DD_OK;
    }
    STDMETHOD(Initialize)(LPDIRECTDRAW, DWORD) override { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(IsClipListChanged)(BOOL* out) override {
        if (out) *out = FALSE;
        return DD_OK;
    }
    STDMETHOD(SetClipList)(LPRGNDATA, DWORD) override { return DD_OK; }
    STDMETHOD(SetHWnd)(DWORD, HWND h) override {
        hwnd = h;
        return DD_OK;
    }
};

// ------------------------------------------------------------------------------------ surface
struct Storage {            // the part of a surface that Flip exchanges
    uint8_t* bits = nullptr;
    bool     virt = false;  // VirtualAlloc (large) vs _aligned_malloc (small)
    HBITMAP  dib = nullptr; // set once GetDC was used: bits then live inside the DIB section
    HDC      dc = nullptr;
    HGDIOBJ  oldBmp = nullptr;
};

void PresentPrimary();

// ---- call trace for the test harness (DDM::TraceCalls): the next N surface calls go to a text
// file, one line each, so the layers a frame is composed of can be read off.
std::atomic<int> g_traceLeft{ 0 };
std::mutex g_traceLock;
FILE* g_traceFile = nullptr;

void TraceLine(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_traceLock);
    if (!g_traceFile) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_traceFile, fmt, args);
    va_end(args);
    // when it happened (ms, QPC) - where a frame's time goes
    static LARGE_INTEGER s_freq{};
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    std::fprintf(g_traceFile, " @%.3f", static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(s_freq.QuadPart));
    std::fputc('\n', g_traceFile);
    if (g_traceLeft.fetch_sub(1) <= 1) {
        std::fclose(g_traceFile);
        g_traceFile = nullptr;
        g_traceLeft.store(0);
    }
}
#define DDM_TRACE(...) do { if (g_traceLeft.load(std::memory_order_relaxed) > 0) TraceLine(__VA_ARGS__); } while (0)

// Who in Game.exe is drawing? The game is compiled without frame pointers, so this is a scan of
// the stack for values that point just behind a call instruction inside Game.exe's code - good
// enough to name the routines. Only used while a trace runs.
const char* GameCallers() {
    static thread_local char text[96];
    text[0] = 0;
    if (g_traceLeft.load(std::memory_order_relaxed) <= 0) return text;
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto* top = reinterpret_cast<const uintptr_t*>(tib->StackBase);
    uintptr_t marker = 0;
    const uintptr_t* sp = &marker; // a local: everything above it on the stack belongs to the callers
    size_t pos = 0;
    int found = 0;
    for (const uintptr_t* p = sp; p < top && p < sp + 600 && found < 7; ++p) {
        const uintptr_t v = *p;
        if (v < 0x00401008 || v >= 0x00676000) continue;
        const auto* c = reinterpret_cast<const uint8_t*>(v);
        const bool afterCall = c[-5] == 0xE8 ||                               // call rel32
                               (c[-2] == 0xFF && (c[-1] & 0x38) == 0x10) ||   // call reg / [reg]
                               (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10) ||   // call [reg+disp8]
                               (c[-6] == 0xFF && (c[-5] & 0x38) == 0x10);     // call [reg+disp32] / [abs]
        if (!afterCall) continue;
        pos += static_cast<size_t>(std::snprintf(text + pos, sizeof(text) - pos, "%s%08X", found ? " " : "", static_cast<unsigned>(v)));
        ++found;
    }
    return text;
}

// The most recent Lock calls, for the crash log (see LogFaultContext).
struct LockRecord {
    const uint8_t* bits = nullptr;
    int   w = 0, h = 0, pitch = 0;
    bool  virt = false;
    DWORD caps = 0;
};
constexpr unsigned kLockHistory = 8;
LockRecord g_lastLocks[kLockHistory];
unsigned   g_lockSeq = 0;

class Surface final : public IDirectDrawSurface7 {
public:
    LONG  refs = 1;
    DWORD caps = 0;
    int   w = 0, h = 0, pitch = 0;
    Storage st;
    bool  hasKey = false;
    DDCOLORKEY key{};
    Surface* back = nullptr;     // next surface of the flip chain
    Clipper* clipper = nullptr;
    int   backBuffers = 0;
    int   lockCount = 0;
    unsigned gen = 0;            // bumped whenever the pixels may have been changed (Lock, Blt target)
    unsigned genWrite = 0;       // bumped only when the pixels were certainly changed (Blt target, GDI)
    unsigned serial = 0;         // unique per surface object (a new surface can reuse an old address)

    Surface(int width, int height, DWORD surfaceCaps) : caps(surfaceCaps), w(width), h(height) {
        static std::atomic<unsigned> s_serial{ 0 };
        serial = ++s_serial;
        pitch = (w * 2 + 3) & ~3; // DWORD aligned, identical to a 16-bit DIB section stride
    }

    ~Surface() {
        {
            std::lock_guard<std::recursive_mutex> lock(g.lock);
            if (g.primary == this) g.primary = nullptr;
            auto it = std::find(g.surfaces.begin(), g.surfaces.end(), this);
            if (it != g.surfaces.end()) g.surfaces.erase(it);
        }
        if (back) {
            back->Release();
            back = nullptr;
        }
        if (clipper) {
            clipper->Release();
            clipper = nullptr;
        }
        FreeStorage();
    }

    Buf buf() const { return Buf{ st.bits, pitch, w, h }; }
    bool isFront() const { return this == g.primary; }

    bool Alloc() {
        const size_t size = static_cast<size_t>(pitch) * static_cast<size_t>(h) + 32;
        if (size >= 256 * 1024) {
            st.bits = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            st.virt = true;
        } else {
            st.bits = static_cast<uint8_t*>(_aligned_malloc(size, 32));
            st.virt = false;
            if (st.bits) std::memset(st.bits, 0, size);
        }
        return st.bits != nullptr;
    }

    void FreeStorage() {
        if (st.dib) {
            if (st.dc) {
                SelectObject(st.dc, st.oldBmp);
                DeleteDC(st.dc);
            }
            DeleteObject(st.dib);
        } else if (st.bits) {
            if (st.virt) {
                VirtualFree(st.bits, 0, MEM_RELEASE);
            } else {
                _aligned_free(st.bits);
            }
        }
        st = Storage{};
    }

    void Resize(int width, int height) {
        FreeStorage();
        w = width;
        h = height;
        pitch = (w * 2 + 3) & ~3;
        Alloc();
    }

    // GDI needs a real bitmap: move the pixels into a 16-bit 565 DIB section (same stride).
    bool PromoteToDib() {
        if (st.dib) return true;
        struct {
            BITMAPINFOHEADER hdr;
            DWORD masks[3];
        } bmi{};
        bmi.hdr.biSize = sizeof(BITMAPINFOHEADER);
        bmi.hdr.biWidth = pitch / 2;
        bmi.hdr.biHeight = -h;
        bmi.hdr.biPlanes = 1;
        bmi.hdr.biBitCount = 16;
        bmi.hdr.biCompression = BI_BITFIELDS;
        bmi.masks[0] = 0xF800;
        bmi.masks[1] = 0x07E0;
        bmi.masks[2] = 0x001F;
        void* dibBits = nullptr;
        HBITMAP dib = CreateDIBSection(nullptr, reinterpret_cast<const BITMAPINFO*>(&bmi), DIB_RGB_COLORS, &dibBits, nullptr, 0);
        if (!dib || !dibBits) {
            if (dib) DeleteObject(dib);
            return false;
        }
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) {
            DeleteObject(dib);
            return false;
        }
        if (st.bits) {
            std::memcpy(dibBits, st.bits, static_cast<size_t>(pitch) * h);
        }
        FreeStorage();
        st.bits = static_cast<uint8_t*>(dibBits);
        st.dib = dib;
        st.dc = dc;
        st.oldBmp = SelectObject(dc, dib);
        return true;
    }

    void FillDesc(void* out, DWORD size, bool withPointer) const {
        // DDSURFACEDESC (108 bytes) and DDSURFACEDESC2 (124) share every field we fill.
        if (size > sizeof(DDSURFACEDESC2)) size = sizeof(DDSURFACEDESC2);
        DDSURFACEDESC2 d{};
        d.dwSize = size;
        d.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
        d.dwHeight = static_cast<DWORD>(h);
        d.dwWidth = static_cast<DWORD>(w);
        d.lPitch = pitch;
        if (backBuffers > 0) {
            d.dwFlags |= DDSD_BACKBUFFERCOUNT;
            d.dwBackBufferCount = static_cast<DWORD>(backBuffers);
        }
        if (hasKey) {
            d.dwFlags |= DDSD_CKSRCBLT;
            d.ddckCKSrcBlt = key;
        }
        if (withPointer) {
            d.dwFlags |= DDSD_LPSURFACE;
            d.lpSurface = st.bits;
        }
        FillPixelFormat565(d.ddpfPixelFormat);
        d.ddsCaps.dwCaps = caps;
        std::memcpy(out, &d, size);
    }

    HRESULT DoBlt(const RECT* dstRect, Surface* src, const RECT* srcRect, DWORD flags, const DDBLTFX* fx);

    // ---- IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectDrawSurface7) ||
            IsEqualGUID(riid, IID_IDirectDrawSurface4) || IsEqualGUID(riid, IID_IDirectDrawSurface3) ||
            IsEqualGUID(riid, IID_IDirectDrawSurface2) || IsEqualGUID(riid, IID_IDirectDrawSurface)) {
            *ppv = static_cast<IDirectDrawSurface7*>(this);
            AddRef();
            return S_OK;
        }
        char s[64];
        GuidToString(riid, s, sizeof(s));
        Logger::Info("DDM: Surface::QueryInterface(%s) -> E_NOINTERFACE", s);
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    STDMETHOD_(ULONG, Release)() override {
        const LONG r = InterlockedDecrement(&refs);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // ---- IDirectDrawSurface
    STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE7) override { DDM_UNSUPPORTED("Surface::AddAttachedSurface"); }
    STDMETHOD(AddOverlayDirtyRect)(LPRECT) override { DDM_UNSUPPORTED("Surface::AddOverlayDirtyRect"); }

    STDMETHOD(Blt)(LPRECT dstRect, LPDIRECTDRAWSURFACE7 srcSurface, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) override {
        Diag::Scope scope(Diag::C_DD_BLT);
        const HRESULT hr = DoBlt(dstRect, static_cast<Surface*>(srcSurface), srcRect, flags, fx);
        if (SUCCEEDED(hr) && isFront()) {
            PresentPrimary();
        }
        return hr;
    }

    STDMETHOD(BltBatch)(LPDDBLTBATCH, DWORD, DWORD) override { DDM_UNSUPPORTED("Surface::BltBatch"); }

    STDMETHOD(BltFast)(DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 srcSurface, LPRECT srcRect, DWORD trans) override {
        Diag::Scope scope(Diag::C_DD_BLT);
        auto* src = static_cast<Surface*>(srcSurface);
        if (!src) return DDERR_INVALIDPARAMS;
        const RECT s = srcRect ? *srcRect : RECT{ 0, 0, src->w, src->h };
        const RECT d{ static_cast<LONG>(x), static_cast<LONG>(y),
                      static_cast<LONG>(x) + (s.right - s.left), static_cast<LONG>(y) + (s.bottom - s.top) };
        const HRESULT hr = DoBlt(&d, src, &s, (trans & DDBLTFAST_SRCCOLORKEY) ? DDBLT_KEYSRC : 0, nullptr);
        if (SUCCEEDED(hr) && isFront()) {
            PresentPrimary();
        }
        return hr;
    }

    STDMETHOD(DeleteAttachedSurface)(DWORD, LPDIRECTDRAWSURFACE7) override { DDM_UNSUPPORTED("Surface::DeleteAttachedSurface"); }
    STDMETHOD(EnumAttachedSurfaces)(LPVOID ctx, LPDDENUMSURFACESCALLBACK7 cb) override {
        if (!cb) return DDERR_INVALIDPARAMS;
        if (back) {
            DDSURFACEDESC2 d{};
            back->FillDesc(&d, sizeof(d), false);
            back->AddRef();
            cb(back, &d, ctx);
        }
        return DD_OK;
    }
    STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK7) override { DDM_UNSUPPORTED("Surface::EnumOverlayZOrders"); }

    STDMETHOD(Flip)(LPDIRECTDRAWSURFACE7, DWORD) override {
        Diag::Scope scope(Diag::C_DD_FLIP);
        if (!back) return DDERR_NOTFLIPPABLE;
        DDM_TRACE("FLIP %p(%dx%d)", static_cast<void*>(this), w, h);
        {
            std::lock_guard<std::recursive_mutex> lock(g.lock);
            // rotate the pixel storage through the chain: front <- back1 <- back2 ... <- front
            const Storage front = st;
            Surface* s = this;
            while (s->back) {
                s->st = s->back->st;
                s = s->back;
            }
            s->st = front;
        }
        if (isFront()) {
            PresentPrimary();
        }
        return DD_OK;
    }

    STDMETHOD(GetAttachedSurface)(LPDDSCAPS2 wanted, LPDIRECTDRAWSURFACE7* out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = nullptr;
        if (!back) return DDERR_NOTFOUND;
        if (wanted && wanted->dwCaps != 0 && (back->caps & wanted->dwCaps) != wanted->dwCaps) {
            return DDERR_NOTFOUND;
        }
        back->AddRef();
        *out = back;
        return DD_OK;
    }
    STDMETHOD(GetBltStatus)(DWORD) override { return DD_OK; }
    STDMETHOD(GetCaps)(LPDDSCAPS2 out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        std::memset(out, 0, sizeof(*out));
        out->dwCaps = caps;
        return DD_OK;
    }
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER* out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        if (!clipper) {
            *out = nullptr;
            return DDERR_NOCLIPPERATTACHED;
        }
        clipper->AddRef();
        *out = clipper;
        return DD_OK;
    }
    STDMETHOD(GetColorKey)(DWORD flags, LPDDCOLORKEY out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        if (!(flags & DDCKEY_SRCBLT) || !hasKey) return DDERR_NOCOLORKEY;
        *out = key;
        return DD_OK;
    }
    STDMETHOD(GetDC)(HDC* out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        std::lock_guard<std::recursive_mutex> lock(g.lock);
        if (!PromoteToDib()) {
            *out = nullptr;
            return DDERR_GENERIC;
        }
        *out = st.dc;
        return DD_OK;
    }
    STDMETHOD(GetFlipStatus)(DWORD) override { return DD_OK; }
    STDMETHOD(GetOverlayPosition)(LPLONG, LPLONG) override { DDM_UNSUPPORTED("Surface::GetOverlayPosition"); }
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE*) override { return DDERR_NOPALETTEATTACHED; }
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        FillPixelFormat565(*out);
        return DD_OK;
    }
    STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC2 out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        FillDesc(out, out->dwSize >= sizeof(DDSURFACEDESC) ? out->dwSize : sizeof(DDSURFACEDESC2), false);
        return DD_OK;
    }
    STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC2) override { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(IsLost)() override { return DD_OK; }

    STDMETHOD(Lock)(LPRECT rect, LPDDSURFACEDESC2 desc, DWORD, HANDLE) override {
        Diag::Scope scope(Diag::C_DD_LOCK);
        if (!desc) return DDERR_INVALIDPARAMS;
        if (!st.bits) return DDERR_SURFACELOST;
        const DWORD size = (desc->dwSize == sizeof(DDSURFACEDESC)) ? sizeof(DDSURFACEDESC) : sizeof(DDSURFACEDESC2);
        FillDesc(desc, size, true);
        if (rect) {
            const LONG x = std::clamp<LONG>(rect->left, 0, w);
            const LONG y = std::clamp<LONG>(rect->top, 0, h);
            desc->lpSurface = st.bits + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 2;
        }
        ++lockCount;
        ++gen; // a lock hands out the pixels for writing
        if (rect) {
            DDM_TRACE("LOCK %p(%dx%d%s) rect=(%ld,%ld,%ld,%ld) by %s", static_cast<void*>(this), w, h, isFront() ? " front" : "",
                      rect->left, rect->top, rect->right, rect->bottom, GameCallers());
        } else {
            DDM_TRACE("LOCK %p(%dx%d%s) whole by %s", static_cast<void*>(this), w, h, isFront() ? " front" : "", GameCallers());
        }
        // remembered for the crash log: the game writes sprites straight into locked memory
        LockRecord& rec = g_lastLocks[g_lockSeq++ % kLockHistory];
        rec.bits = st.bits;
        rec.w = w;
        rec.h = h;
        rec.pitch = pitch;
        rec.virt = st.virt;
        rec.caps = caps;
        return DD_OK;
    }
    STDMETHOD(ReleaseDC)(HDC) override {
        GdiFlush();
        ++gen;
        ++genWrite; // GDI drew into the pixels
        if (isFront()) {
            PresentPrimary();
        }
        return DD_OK;
    }
    STDMETHOD(Restore)() override { return DD_OK; }
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER c) override {
        auto* nc = static_cast<Clipper*>(c);
        if (nc) nc->AddRef();
        if (clipper) clipper->Release();
        clipper = nc;
        return DD_OK;
    }
    STDMETHOD(SetColorKey)(DWORD flags, LPDDCOLORKEY ck) override {
        if (!(flags & DDCKEY_SRCBLT)) {
            static bool once = false;
            if (!once) {
                once = true;
                Logger::Warn("DDM: Surface::SetColorKey with flags 0x%08X ignored (only DDCKEY_SRCBLT is implemented).", flags);
            }
            return DD_OK;
        }
        if (ck) {
            key = *ck;
            hasKey = true;
        } else {
            hasKey = false;
        }
        return DD_OK;
    }
    STDMETHOD(SetOverlayPosition)(LONG, LONG) override { DDM_UNSUPPORTED("Surface::SetOverlayPosition"); }
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE) override { DDM_UNSUPPORTED("Surface::SetPalette"); }
    STDMETHOD(Unlock)(LPRECT) override {
        Diag::Scope scope(Diag::C_DD_UNLOCK);
        if (lockCount > 0) --lockCount;
        if (isFront()) {
            PresentPrimary();
        }
        return DD_OK;
    }
    STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDOVERLAYFX) override { DDM_UNSUPPORTED("Surface::UpdateOverlay"); }
    STDMETHOD(UpdateOverlayDisplay)(DWORD) override { DDM_UNSUPPORTED("Surface::UpdateOverlayDisplay"); }
    STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE7) override { DDM_UNSUPPORTED("Surface::UpdateOverlayZOrder"); }
    // ---- v2
    STDMETHOD(GetDDInterface)(LPVOID* out) override;
    STDMETHOD(PageLock)(DWORD) override { return DD_OK; }
    STDMETHOD(PageUnlock)(DWORD) override { return DD_OK; }
    // ---- v3
    STDMETHOD(SetSurfaceDesc)(LPDDSURFACEDESC2, DWORD) override { DDM_UNSUPPORTED("Surface::SetSurfaceDesc"); }
    // ---- v4
    STDMETHOD(SetPrivateData)(REFGUID, LPVOID, DWORD, DWORD) override { DDM_UNSUPPORTED("Surface::SetPrivateData"); }
    STDMETHOD(GetPrivateData)(REFGUID, LPVOID, LPDWORD) override { DDM_UNSUPPORTED("Surface::GetPrivateData"); }
    STDMETHOD(FreePrivateData)(REFGUID) override { DDM_UNSUPPORTED("Surface::FreePrivateData"); }
    STDMETHOD(GetUniquenessValue)(LPDWORD out) override {
        if (out) *out = 1;
        return DD_OK;
    }
    STDMETHOD(ChangeUniquenessValue)() override { return DD_OK; }
    // ---- v7
    STDMETHOD(SetPriority)(DWORD) override { return DD_OK; }
    STDMETHOD(GetPriority)(LPDWORD out) override {
        if (out) *out = 0;
        return DD_OK;
    }
    STDMETHOD(SetLOD)(DWORD) override { return DD_OK; }
    STDMETHOD(GetLOD)(LPDWORD out) override {
        if (out) *out = 0;
        return DD_OK;
    }
};

bool ClipToBounds(RECT& r, int w, int h) {
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > w) r.right = w;
    if (r.bottom > h) r.bottom = h;
    return r.right > r.left && r.bottom > r.top;
}

HRESULT Surface::DoBlt(const RECT* dstRect, Surface* src, const RECT* srcRect, DWORD flags, const DDBLTFX* fx) {
    if (!st.bits) return DDERR_SURFACELOST;
    ++gen; // this surface is being drawn into
    ++genWrite;
    if (w * h < 600000 && Diag::IsMainThread()) {
        HD::Sprites::OnSurfaceWritten(st.bits); // a sprite surface drawn into no longer holds the decoded frame
    }
    RECT d = dstRect ? *dstRect : RECT{ 0, 0, w, h };
    if (d.right <= d.left || d.bottom <= d.top) return DDERR_INVALIDRECT;
    // interface scale: the game's windows draw in coordinates of half the screen
    const bool uiScaled = dstRect && UiScale::Drawing() && g.primary && (this == g.primary || this == g.primary->back);
    if (uiScaled) UiScale::ScaleRect(d);

    if (flags & DDBLT_COLORFILL) {
        if (!fx) return DDERR_INVALIDPARAMS;
        DDM_TRACE("FILL %p(%dx%d%s) d=(%ld,%ld,%ld,%ld) color=%04X", static_cast<void*>(this), w, h, isFront() ? " front" : "",
                  d.left, d.top, d.right, d.bottom, static_cast<unsigned>(fx->dwFillColor) & 0xFFFF);
        Diag::Scope t(Diag::C_BLT_FILL);
        if (ClipToBounds(d, w, h)) {
            Fill16(buf(), d, static_cast<uint16_t>(fx->dwFillColor));
            Diag::AddValue(Diag::C_PX_FILL, static_cast<uint64_t>(d.right - d.left) * (d.bottom - d.top));
            HD::Patch::OnOverwrite(buf(), d);
        }
        return DD_OK;
    }

    if ((flags & DDBLT_ROP) && fx) {
        if (fx->dwROP == BLACKNESS || fx->dwROP == WHITENESS) {
            Diag::Scope t(Diag::C_BLT_FILL);
            if (ClipToBounds(d, w, h)) {
                Fill16(buf(), d, fx->dwROP == BLACKNESS ? 0x0000 : 0xFFFF);
                HD::Patch::OnOverwrite(buf(), d);
            }
            return DD_OK;
        }
        if (fx->dwROP != SRCCOPY) {
            static bool once = false;
            if (!once) {
                once = true;
                Logger::Warn("DDM: Blt ROP 0x%08X is not implemented, treated as SRCCOPY.", fx->dwROP);
            }
        }
    }

    if (!src) {
        static bool once = false;
        if (!once) {
            once = true;
            Logger::Warn("DDM: Blt without source and flags 0x%08X ignored.", flags);
        }
        return DD_OK;
    }
    if (!src->st.bits) return DDERR_SURFACELOST;

    RECT s = srcRect ? *srcRect : RECT{ 0, 0, src->w, src->h };
    const int sw = s.right - s.left;
    const int sh = s.bottom - s.top;
    const int dw = d.right - d.left;
    const int dh = d.bottom - d.top;
    if (sw <= 0 || sh <= 0) return DDERR_INVALIDRECT;

    // The mouse cursor with the interface scale: the game copies the screen under the cursor
    // twice, draws the cursor into the second copy and puts that back (colour-keyed). Enlarged,
    // the copied background would show as a square - the first copy tells which pixels are
    // background (see the uiScaled branch below).
    static unsigned s_cursorSerial = 0;
    static const Surface* s_cursorBg = nullptr;
    static RECT s_cursorBgRect{};
    if (UiScale::CursorDrawing()) {
        if (s_cursorSerial != UiScale::CursorSerial()) {
            s_cursorSerial = UiScale::CursorSerial();
            s_cursorBg = nullptr;
        }
        if (!s_cursorBg && g.primary && (src == g.primary || src == g.primary->back) && this != g.primary && this != g.primary->back &&
            sw == dw && sh == dh) {
            s_cursorBg = this;
            s_cursorBgRect = d;
        }
    }

    bool keyed = false;
    uint16_t keyColor = 0;
    if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) {
        keyed = true;
        keyColor = static_cast<uint16_t>(fx->ddckSrcColorkey.dwColorSpaceLowValue);
    } else if ((flags & DDBLT_KEYSRC) && src->hasKey) {
        keyed = true;
        keyColor = static_cast<uint16_t>(src->key.dwColorSpaceLowValue);
    }
    bool mirrorX = false, mirrorY = false;
    if ((flags & DDBLT_DDFX) && fx) {
        mirrorX = (fx->dwDDFX & DDBLTFX_MIRRORLEFTRIGHT) != 0;
        mirrorY = (fx->dwDDFX & DDBLTFX_MIRRORUPDOWN) != 0;
    }
    DDM_TRACE("BLT  %p(%dx%d%s) d=(%ld,%ld,%ld,%ld) <- %p(%dx%d) s=(%ld,%ld,%ld,%ld) %s%s%s%s by %s",
              static_cast<void*>(this), w, h, isFront() ? " front" : "", d.left, d.top, d.right, d.bottom,
              static_cast<void*>(src), src->w, src->h, s.left, s.top, s.right, s.bottom,
              (sw == dw && sh == dh) ? "1:1" : ((dw == sw * 2 && dh == sh * 2) ? "x2" : "stretch"),
              keyed ? " keyed" : "", mirrorX ? " mirrorX" : "", mirrorY ? " mirrorY" : "", GameCallers());

    if (sw == dw && sh == dh && !mirrorX && !mirrorY) {
        int dx = d.left, dy = d.top;
        RECT c = s;
        bool srcOutside = false;
        // keep the source rectangle inside the source surface
        if (c.left < 0)        { dx -= c.left; c.left = 0; srcOutside = true; }
        if (c.top < 0)         { dy -= c.top;  c.top = 0;  srcOutside = true; }
        if (c.right > src->w)  { c.right = src->w;  srcOutside = true; }
        if (c.bottom > src->h) { c.bottom = src->h; srcOutside = true; }
        // The game asks for more than the source holds when a map is smaller than the view.
        // Real DirectDraw rejects the whole blit; show black around the valid part instead of
        // leaving stale pixels of earlier frames.
        if (srcOutside && !keyed) {
            RECT whole = d;
            if (ClipToBounds(whole, w, h)) {
                Fill16(buf(), whole, 0);
            }
        }
        // keep the destination inside this surface
        if (dx < 0) { c.left -= dx; dx = 0; }
        if (dy < 0) { c.top -= dy;  dy = 0; }
        if (dx + (c.right - c.left) > w) c.right = c.left + (w - dx);
        if (dy + (c.bottom - c.top) > h) c.bottom = c.top + (h - dy);
        if (c.right <= c.left || c.bottom <= c.top) return DD_OK;

        const uint64_t px = static_cast<uint64_t>(c.right - c.left) * (c.bottom - c.top);
        if (keyed) {
            Diag::Scope t(Diag::C_BLT_KEY);
            // menus with the menu zoom: letters and pictures again in HD (ddm_hd_patch.h)
            static HD::Patch::Op op;    // main thread only
            bool patch = false;
            if (Diag::IsMainThread()) {
                op.Clear();
                patch = HD::Patch::BeginKeyed(buf(), dx, dy, src->buf(), c, keyColor, op);
            }
            CopyKey16(buf(), dx, dy, src->buf(), c, keyColor);
            if (patch) HD::Patch::End(op);
            Diag::AddValue(Diag::C_PX_KEY, px);
        } else {
            Diag::Scope t(Diag::C_BLT_COPY);
            Copy16(buf(), dx, dy, src->buf(), c);
            Diag::AddValue(Diag::C_PX_COPY, px);
            HD::Patch::OnCopy(buf(), dx, dy, src->buf(), c, g.primary && (this == g.primary || this == g.primary->back),
                              g.primary && (src == g.primary || src == g.primary->back));
            // a menu screen composed in a surface of the screen's size, copied to the screen
            if (g.primary && (this == g.primary || this == g.primary->back) && src != g.primary && src != g.primary->back &&
                src->w == w && src->h == h && Diag::IsMainThread()) {
                HD::Menu::OnScreenCopy(src->buf(), src->serial, src->gen, src->genWrite, c, dx, dy);
            } else if (g.primary && this != g.primary && this != g.primary->back && src != g.primary && src != g.primary->back &&
                       src->w == w && src->h == h && dx == 0 && dy == 0 && c.left == 0 && c.top == 0 && c.right == w &&
                       c.bottom == h && Diag::IsMainThread()) {
                // a whole copy between two surfaces of the screen's size (the credits' fixed forest into the
                // surface their scrolling text is mixed into)
                HD::Menu::OnSurfaceCopy(buf(), src->buf(), src->serial, src->gen, src->genWrite);
            }
        }
        return DD_OK;
    }

    // stretched and/or mirrored
    Diag::Scope t(Diag::C_BLT_STRETCH);
    {
        RECT oc = d;
        if (ClipToBounds(oc, w, h)) HD::Patch::OnOverwrite(buf(), oc);   // no longer what a shadow there shows
    }
    if (uiScaled) {
        // a window of the game, twice as large: filtered (colour key respected); none of the
        // HD substitutions below concern the interface
        RECT clip = d;
        if (ClipToBounds(clip, w, h)) {
            const bool inside = s.left >= 0 && s.top >= 0 && s.right <= src->w && s.bottom <= src->h;
            const bool cursor = UiScale::CursorDrawing() && keyed && inside && s_cursorSerial == UiScale::CursorSerial() &&
                                s_cursorBg && s_cursorBg != src && s_cursorBg->st.bits &&
                                s_cursorBgRect.right - s_cursorBgRect.left == sw && s_cursorBgRect.bottom - s_cursorBgRect.top == sh &&
                                s_cursorBgRect.left >= 0 && s_cursorBgRect.top >= 0 &&
                                s_cursorBgRect.right <= s_cursorBg->w && s_cursorBgRect.bottom <= s_cursorBg->h;
            if (cursor && dw == sw * 2 && dh == sh * 2 && !mirrorX && !mirrorY) {
                // only the cursor's own pixels: those that differ from the background copy
                static std::vector<uint16_t> tmp;
                tmp.resize(static_cast<size_t>(sw) * sh);
                const Buf sb = src->buf();
                const Buf bb = s_cursorBg->buf();
                for (int y = 0; y < sh; ++y) {
                    const auto* sp = reinterpret_cast<const uint16_t*>(sb.bits + static_cast<size_t>(s.top + y) * sb.pitch) + s.left;
                    const auto* bp = reinterpret_cast<const uint16_t*>(bb.bits + static_cast<size_t>(s_cursorBgRect.top + y) * bb.pitch) +
                                     s_cursorBgRect.left;
                    uint16_t* tp = tmp.data() + static_cast<size_t>(y) * sw;
                    for (int x = 0; x < sw; ++x) tp[x] = sp[x] == bp[x] ? keyColor : sp[x];
                }
                const Buf tb{ reinterpret_cast<uint8_t*>(tmp.data()), sw * 2, sw, sh };
                const RECT ts{ 0, 0, sw, sh };
                SmoothSprite2x16(buf(), d, tb, ts, clip, keyColor, kNoColor, 256, nullptr, false, false);
            } else if (keyed && dw == sw * 2 && dh == sh * 2 && !mirrorX && !mirrorY && inside &&
                       HD::Font::KeyedGlyph2x(buf(), d, clip, src->buf(), s, static_cast<uint16_t>(keyColor))) {
                // a letter of a colour-keyed font with an HD version (ddm_hd_font.h)
            } else if (dw == sw * 2 && dh == sh * 2 && !mirrorX && !mirrorY && inside) {
                // tiles of the HUD lie side by side: no fading at their edges (seams)
                SetSmoothEdgeClamp(true);
                if (Diag::IsMainThread()) {
                    SmoothPicture2x16(buf(), d, src->buf(), s, clip, keyed ? keyColor : kNoColor);    // kept between frames
                } else {
                    SmoothSprite2x16(buf(), d, src->buf(), s, clip, keyed ? keyColor : kNoColor, kNoColor, 256, nullptr, false, false);
                }
                SetSmoothEdgeClamp(false);
            } else {
                RECT sc = s;
                if (sc.left < 0) sc.left = 0;
                if (sc.top < 0) sc.top = 0;
                if (sc.right > src->w) sc.right = src->w;
                if (sc.bottom > src->h) sc.bottom = src->h;
                if (sc.right > sc.left && sc.bottom > sc.top) {
                    Stretch16(buf(), d, src->buf(), sc, clip, keyed, keyColor, mirrorX, mirrorY);
                }
            }
            Diag::AddValue(Diag::C_PX_STRETCH, static_cast<uint64_t>(clip.right - clip.left) * (clip.bottom - clip.top));
        }
        return DD_OK;
    }
    if (keyed && (HD::DebugFlags() & 8u)) return DD_OK; // test harness: see what these blits draw

    // exact 2x magnification of the level background: use the HD picture when there is one
    if (!keyed && !mirrorX && !mirrorY && dw == sw * 2 && dh == sh * 2) {
        RECT hdClip = d;
        if (ClipToBounds(hdClip, w, h) &&
            HD::TryBackgroundBlit(buf(), d, hdClip, src->buf(), s, reinterpret_cast<const void*>(static_cast<uintptr_t>(src->serial)),
                                  src->gen, src->genWrite)) {
            Diag::AddValue(Diag::C_PX_STRETCH, static_cast<uint64_t>(hdClip.right - hdClip.left) * (hdClip.bottom - hdClip.top));
            return DD_OK;
        }
    }

    // exact 2x magnification of a colour-keyed sprite (objects, water animations at the 2x zoom):
    // filtered instead of 2x2 blocks
    if (keyed && src != this && dw == sw * 2 && dh == sh * 2 && !mirrorX && !mirrorY &&
        s.left >= 0 && s.top >= 0 && s.right <= src->w && s.bottom <= src->h) {
        RECT groundClip = d;
        if (ClipToBounds(groundClip, w, h) && HD::DrawGroundAnimation(buf(), d, groundClip, src->buf(), s)) {
            return DD_OK; // water / chimney smoke on an HD background: drawn over the HD picture, or still HD water
        }
    }
    // ... or the upscaled version of the sprite frame the surface holds, when there is one
    if (keyed && src != this && dw == sw * 2 && dh == sh * 2 && !mirrorX && !mirrorY &&
        s.left >= 0 && s.top >= 0 && s.right <= src->w && s.bottom <= src->h) {
        RECT hdClip = d;
        if (ClipToBounds(hdClip, w, h) && HD::Sprites::TryStretch(buf(), d, hdClip, src->buf(), s, keyColor)) {
            Diag::AddValue(Diag::C_PX_STRETCH, static_cast<uint64_t>(hdClip.right - hdClip.left) * (hdClip.bottom - hdClip.top));
            return DD_OK;
        }
    }
    if (keyed && src != this && dw == sw * 2 && dh == sh * 2 && HD::Sprites::MissActive()) {
        HD::Sprites::NoteFallback(src->buf().bits, src->w, src->h, (mirrorX || mirrorY) ? 2 : 0); // test harness
    }
    if (keyed && src != this && dw == sw * 2 && dh == sh * 2 && SherwoodConfig::Get().smoothSprites && HD::VariantOn() &&
        s.left >= 0 && s.top >= 0 && s.right <= src->w && s.bottom <= src->h) {
        RECT clip2 = d;
        if (ClipToBounds(clip2, w, h)) {
            SmoothSprite2x16(buf(), d, src->buf(), s, clip2, keyColor, kNoColor, 256, nullptr, mirrorX, mirrorY);
            Diag::AddValue(Diag::C_PX_STRETCH, static_cast<uint64_t>(clip2.right - clip2.left) * (clip2.bottom - clip2.top));
        }
        return DD_OK;
    }

    if (s.left < 0) s.left = 0;
    if (s.top < 0) s.top = 0;
    if (s.right > src->w) s.right = src->w;
    if (s.bottom > src->h) s.bottom = src->h;
    RECT clip = d;
    if (s.right > s.left && s.bottom > s.top && ClipToBounds(clip, w, h)) {
        Stretch16(buf(), d, src->buf(), s, clip, keyed, keyColor, mirrorX, mirrorY);
        Diag::AddValue(Diag::C_PX_STRETCH, static_cast<uint64_t>(clip.right - clip.left) * (clip.bottom - clip.top));
    }
    return DD_OK;
}

void PresentPrimary() {
    Surface* p = g.primary;
    if (!p || !p->st.bits) return;
    Diag::Add(Diag::C_PRESENT);
    DDM_TRACE("PRESENT %p(%dx%d)", static_cast<void*>(p), p->w, p->h);
    {
        // a hitch between two frames (25..500 ms: longer ones are loading screens) - at most one line a second
        static LARGE_INTEGER s_freq{}, s_last{}, s_logged{};
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
        const double patchMs = HD::Patch::TakeTimeUs() / 1000.0;     // HD letters / pictures of menus in that time
        if (s_last.QuadPart) {
            const double ms = static_cast<double>(now.QuadPart - s_last.QuadPart) * 1000.0 / static_cast<double>(s_freq.QuadPart);
            if (ms >= 25.0 && ms < 500.0 && now.QuadPart - s_logged.QuadPart > s_freq.QuadPart) {
                s_logged = now;
                Logger::Info("Present: %.0f ms between two frames%s (HD menu patches %.1f ms of it).", ms,
                             UiScale::DialogLoopRunning() ? " (a dialog of the game)" : "", patchMs);
            }
        }
        s_last = now;
    }
    Present::Frame(p->st.bits, p->pitch, p->w, p->h);
    if (Diag::IsMainThread()) {
        Watchdog::Beat();   // a frame reached the screen: menus and videos count as alive too
        InputFix::Poll();   // menus have no game tick; retry a failed input acquire from here
    }

    static uint64_t s_lastCursorCheck = 0;
    const uint64_t now = Diag::Now();
    if (Diag::TicksToMs(now - s_lastCursorCheck) > 250.0) {
        s_lastCursorCheck = now;
        WindowManager::KeepCursorHidden();
    }
}

// ------------------------------------------------------------------------------------ IDirectDraw7
HRESULT CreateSurfaceImpl(const DDSURFACEDESC2* desc, Surface** out) {
    if (!desc || !out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    const DWORD flags = desc->dwFlags;
    DWORD caps = (flags & DDSD_CAPS) ? desc->ddsCaps.dwCaps : 0;
    const bool primary = (caps & DDSCAPS_PRIMARYSURFACE) != 0;

    if ((flags & DDSD_PIXELFORMAT) && desc->ddpfPixelFormat.dwRGBBitCount != 16) {
        Logger::Warn("DDM: CreateSurface with %u-bit pixel format requested - only 16-bit RGB565 is implemented.",
                     desc->ddpfPixelFormat.dwRGBBitCount);
        return DDERR_INVALIDPIXELFORMAT;
    }

    int w = 0, h = 0;
    if (primary) {
        CurrentMode(w, h);
    } else {
        if (!(flags & DDSD_WIDTH) || !(flags & DDSD_HEIGHT)) return DDERR_INVALIDPARAMS;
        w = static_cast<int>(desc->dwWidth);
        h = static_cast<int>(desc->dwHeight);
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return DDERR_INVALIDPARAMS;

    std::lock_guard<std::recursive_mutex> lock(g.lock);
    auto* s = new Surface(w, h, caps);
    if (!s->Alloc()) {
        delete s;
        Logger::Error("DDM: out of memory creating a %dx%d surface.", w, h);
        return DDERR_OUTOFMEMORY;
    }
    if ((flags & DDSD_CKSRCBLT)) {
        s->key = desc->ddckCKSrcBlt;
        s->hasKey = true;
    }
    g.surfaces.push_back(s);

    int backCount = 0;
    if ((flags & DDSD_BACKBUFFERCOUNT) && desc->dwBackBufferCount > 0) {
        backCount = static_cast<int>(std::min<DWORD>(desc->dwBackBufferCount, 3));
        s->backBuffers = backCount;
        Surface* tail = s;
        for (int i = 0; i < backCount; ++i) {
            DWORD bcaps = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER)) | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
            if (i == 0) bcaps |= DDSCAPS_BACKBUFFER;
            auto* b = new Surface(w, h, bcaps);
            if (!b->Alloc()) {
                delete b;
                s->Release();
                return DDERR_OUTOFMEMORY;
            }
            g.surfaces.push_back(b);
            tail->back = b;
            tail = b;
        }
        s->caps |= DDSCAPS_FRONTBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    }
    if (primary) {
        s->caps |= DDSCAPS_VISIBLE;
        g.primary = s;
    }

    ++g.created;
    if (g.created <= 60 || primary) {
        Logger::Info("DDM: CreateSurface #%u %dx%d caps=0x%08X flags=0x%08X backBuffers=%d%s",
                     g.created, w, h, caps, flags, backCount, primary ? " [PRIMARY]" : "");
    }
    *out = s;
    return DD_OK;
}

void FillCaps(LPDDCAPS out) {
    if (!out) return;
    const DWORD size = out->dwSize;
    if (size < 16 || size > sizeof(DDCAPS)) {
        return;
    }
    DDCAPS c{};
    c.dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_GDI | DDCAPS_CANBLTSYSMEM;
    c.dwCaps2 = DDCAPS2_WIDESURFACES | DDCAPS2_CANRENDERWINDOWED | DDCAPS2_FLIPNOVSYNC;
    c.dwCKeyCaps = DDCKEYCAPS_SRCBLT;
    c.dwFXCaps = DDFXCAPS_BLTMIRRORLEFTRIGHT | DDFXCAPS_BLTMIRRORUPDOWN | DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY |
                 DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKY;
    c.dwVidMemTotal = 256u * 1024u * 1024u;
    c.dwVidMemFree = 256u * 1024u * 1024u;
    c.ddsCaps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_OFFSCREENPLAIN |
                       DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    c.dwSize = size;
    std::memcpy(out, &c, size);
}

HRESULT SetCooperativeLevelImpl(HWND hwnd, DWORD flags) {
    std::lock_guard<std::recursive_mutex> lock(g.lock);
    Logger::Info("DDM: SetCooperativeLevel(hwnd=0x%08X, flags=0x%08X)", reinterpret_cast<uintptr_t>(hwnd), flags);
    g.coopFlags = flags;
    if (hwnd) {
        g.hwnd = hwnd;
        // Only the real game window is made fullscreen; the game first sets a normal
        // cooperative level on a helper window while it probes the hardware.
        if ((flags & DDSCL_FULLSCREEN) || hwnd == WindowManager::GetGameWindow()) {
            WindowManager::ApplyBorderless(hwnd);
        }
        Present::SetWindow(hwnd);
    }
    return DD_OK;
}

HRESULT SetDisplayModeImpl(DWORD w, DWORD h, DWORD bpp) {
    std::lock_guard<std::recursive_mutex> lock(g.lock);
    Logger::Info("DDM: SetDisplayMode(%u x %u, %u bpp) - internal resolution only, the desktop mode is untouched.", w, h, bpp);
    if (bpp != 16) {
        Logger::Warn("DDM: %u-bit display mode requested; surfaces stay 16-bit RGB565.", bpp);
    }
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return DDERR_INVALIDMODE;
    g.modeW = static_cast<int>(w);
    g.modeH = static_cast<int>(h);
    g.modeSet = true;
    WindowManager::SetInternalResolution(g.modeW, g.modeH);
    // Like a restored exclusive-mode primary, the flip chain follows the display mode.
    for (Surface* s = g.primary; s; s = s->back) {
        if (s->w != g.modeW || s->h != g.modeH) {
            s->Resize(g.modeW, g.modeH);
        }
    }
    return DD_OK;
}

class DDraw7 final : public IDirectDraw7 {
public:
    LONG refs = 1;

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv) override;
    STDMETHOD_(ULONG, AddRef)() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    STDMETHOD_(ULONG, Release)() override {
        const LONG r = InterlockedDecrement(&refs);
        if (r == 0) {
            {
                std::lock_guard<std::recursive_mutex> lock(g.lock);
                if (g.dd7 == this) g.dd7 = nullptr;
            }
            delete this;
        }
        return static_cast<ULONG>(r);
    }

    STDMETHOD(Compact)() override { return DD_OK; }
    STDMETHOD(CreateClipper)(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = new Clipper();
        return DD_OK;
    }
    STDMETHOD(CreatePalette)(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*) override { DDM_UNSUPPORTED("IDirectDraw7::CreatePalette"); }
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC2 desc, LPDIRECTDRAWSURFACE7* out, IUnknown*) override {
        if (!out) return DDERR_INVALIDPARAMS;
        Surface* s = nullptr;
        const HRESULT hr = CreateSurfaceImpl(desc, &s);
        *out = s;
        return hr;
    }
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE7, LPDIRECTDRAWSURFACE7*) override { DDM_UNSUPPORTED("IDirectDraw7::DuplicateSurface"); }
    STDMETHOD(EnumDisplayModes)(DWORD, LPDDSURFACEDESC2, LPVOID ctx, LPDDENUMMODESCALLBACK2 cb) override {
        if (!cb) return DDERR_INVALIDPARAMS;
        std::vector<SIZE> modes = { {640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1366, 768},
                                    {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160} };
        auto add = [&](int w, int h) {
            if (w <= 0 || h <= 0) return;
            for (const SIZE& m : modes) {
                if (m.cx == w && m.cy == h) return;
            }
            modes.push_back(SIZE{ w, h });
        };
        int mw = 0, mh = 0;
        WindowManager::GetMonitorResolution(mw, mh);
        add(mw, mh);
        add(SherwoodConfig::Get().width, SherwoodConfig::Get().height);
        for (const SIZE& m : modes) {
            DDSURFACEDESC2 d{};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            d.dwWidth = static_cast<DWORD>(m.cx);
            d.dwHeight = static_cast<DWORD>(m.cy);
            d.lPitch = (m.cx * 2 + 3) & ~3;
            d.dwRefreshRate = 60;
            FillPixelFormat565(d.ddpfPixelFormat);
            if (cb(&d, ctx) == DDENUMRET_CANCEL) break;
        }
        return DD_OK;
    }
    STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMSURFACESCALLBACK7) override { DDM_UNSUPPORTED("IDirectDraw7::EnumSurfaces"); }
    STDMETHOD(FlipToGDISurface)() override { return DD_OK; }
    STDMETHOD(GetCaps)(LPDDCAPS drv, LPDDCAPS hel) override {
        FillCaps(drv);
        FillCaps(hel);
        return DD_OK;
    }
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC2 out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        DWORD size = out->dwSize;
        if (size != sizeof(DDSURFACEDESC) && size != sizeof(DDSURFACEDESC2)) size = sizeof(DDSURFACEDESC2);
        int w = 0, h = 0;
        CurrentMode(w, h);
        DDSURFACEDESC2 d{};
        d.dwSize = size;
        d.dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        d.dwWidth = static_cast<DWORD>(w);
        d.dwHeight = static_cast<DWORD>(h);
        d.lPitch = (w * 2 + 3) & ~3;
        d.dwRefreshRate = 60;
        FillPixelFormat565(d.ddpfPixelFormat);
        std::memcpy(out, &d, size);
        return DD_OK;
    }
    STDMETHOD(GetFourCCCodes)(LPDWORD count, LPDWORD) override {
        if (count) *count = 0;
        return DD_OK;
    }
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE7* out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        std::lock_guard<std::recursive_mutex> lock(g.lock);
        if (!g.primary) {
            *out = nullptr;
            return DDERR_NOTFOUND;
        }
        g.primary->AddRef();
        *out = g.primary;
        return DD_OK;
    }
    STDMETHOD(GetMonitorFrequency)(LPDWORD out) override {
        if (out) *out = 60;
        return DD_OK;
    }
    STDMETHOD(GetScanLine)(LPDWORD out) override {
        if (out) *out = 0;
        return DD_OK;
    }
    STDMETHOD(GetVerticalBlankStatus)(LPBOOL out) override {
        if (out) *out = FALSE;
        return DD_OK;
    }
    STDMETHOD(Initialize)(GUID*) override { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(RestoreDisplayMode)() override {
        Logger::Info("DDM: RestoreDisplayMode()");
        return DD_OK;
    }
    STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD flags) override { return SetCooperativeLevelImpl(hwnd, flags); }
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp, DWORD, DWORD) override { return SetDisplayModeImpl(w, h, bpp); }
    STDMETHOD(WaitForVerticalBlank)(DWORD, HANDLE) override { return DD_OK; }
    STDMETHOD(GetAvailableVidMem)(LPDDSCAPS2, LPDWORD total, LPDWORD free) override {
        if (total) *total = 256u * 1024u * 1024u;
        if (free) *free = 256u * 1024u * 1024u;
        return DD_OK;
    }
    STDMETHOD(GetSurfaceFromDC)(HDC, LPDIRECTDRAWSURFACE7*) override { DDM_UNSUPPORTED("IDirectDraw7::GetSurfaceFromDC"); }
    STDMETHOD(RestoreAllSurfaces)() override { return DD_OK; }
    STDMETHOD(TestCooperativeLevel)() override { return DD_OK; }
    STDMETHOD(GetDeviceIdentifier)(LPDDDEVICEIDENTIFIER2 out, DWORD) override {
        if (!out) return DDERR_INVALIDPARAMS;
        std::memset(out, 0, sizeof(*out));
        std::strncpy(out->szDriver, "sherwoodfix", sizeof(out->szDriver) - 1);
        std::strncpy(out->szDescription, "SherwoodFix in-memory DirectDraw", sizeof(out->szDescription) - 1);
        return DD_OK;
    }
    STDMETHOD(StartModeTest)(LPSIZE, DWORD, DWORD) override { DDM_UNSUPPORTED("IDirectDraw7::StartModeTest"); }
    STDMETHOD(EvaluateMode)(DWORD, DWORD*) override { DDM_UNSUPPORTED("IDirectDraw7::EvaluateMode"); }
};

DDraw7* AcquireDD7() {
    std::lock_guard<std::recursive_mutex> lock(g.lock);
    if (!g.dd7) {
        g.dd7 = new DDraw7();
    } else {
        g.dd7->AddRef();
    }
    return g.dd7;
}

// ---- helpers shared by the pre-DirectX 7 interfaces (DDSURFACEDESC instead of DDSURFACEDESC2)
std::vector<SIZE> ModeList() {
    std::vector<SIZE> modes = { {640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1366, 768},
                                {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160} };
    auto add = [&](int w, int h) {
        if (w <= 0 || h <= 0) return;
        for (const SIZE& m : modes) {
            if (m.cx == w && m.cy == h) return;
        }
        modes.push_back(SIZE{ w, h });
    };
    int mw = 0, mh = 0;
    WindowManager::GetMonitorResolution(mw, mh);
    add(mw, mh);
    add(SherwoodConfig::Get().width, SherwoodConfig::Get().height);
    return modes;
}

HRESULT CreateSurfaceV1(const DDSURFACEDESC* d1, LPDIRECTDRAWSURFACE* out) {
    if (!d1 || !out) return DDERR_INVALIDPARAMS;
    // every field up to ddsCaps.dwCaps sits at the same offset in both structures
    DDSURFACEDESC2 d2{};
    std::memcpy(&d2, d1, std::min<size_t>(d1->dwSize, sizeof(DDSURFACEDESC)));
    d2.dwSize = sizeof(d2);
    Surface* s = nullptr;
    const HRESULT hr = CreateSurfaceImpl(&d2, &s);
    *out = reinterpret_cast<LPDIRECTDRAWSURFACE>(static_cast<IDirectDrawSurface7*>(s));
    return hr;
}

HRESULT EnumModesV1(LPVOID ctx, LPDDENUMMODESCALLBACK cb) {
    if (!cb) return DDERR_INVALIDPARAMS;
    for (const SIZE& m : ModeList()) {
        DDSURFACEDESC d{};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        d.dwWidth = static_cast<DWORD>(m.cx);
        d.dwHeight = static_cast<DWORD>(m.cy);
        d.lPitch = (m.cx * 2 + 3) & ~3;
        d.dwRefreshRate = 60;
        FillPixelFormat565(d.ddpfPixelFormat);
        if (cb(&d, ctx) == DDENUMRET_CANCEL) break;
    }
    return DD_OK;
}

HRESULT GetDisplayModeV1(LPDDSURFACEDESC out) {
    if (!out) return DDERR_INVALIDPARAMS;
    int w = 0, h = 0;
    CurrentMode(w, h);
    DDSURFACEDESC d{};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d.dwWidth = static_cast<DWORD>(w);
    d.dwHeight = static_cast<DWORD>(h);
    d.lPitch = (w * 2 + 3) & ~3;
    d.dwRefreshRate = 60;
    FillPixelFormat565(d.ddpfPixelFormat);
    *out = d;
    return DD_OK;
}

HRESULT QueryLegacy(REFIID riid, LPVOID* ppv, const char* from);

// IDirectDraw2: the game asks for it right after DirectDrawCreate (video memory query).
class DDraw2 final : public IDirectDraw2 {
public:
    LONG refs = 1;

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IDirectDraw2)) {
            *ppv = static_cast<IDirectDraw2*>(this);
            AddRef();
            return S_OK;
        }
        return QueryLegacy(riid, ppv, "IDirectDraw2");
    }
    STDMETHOD_(ULONG, AddRef)() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    STDMETHOD_(ULONG, Release)() override {
        const LONG r = InterlockedDecrement(&refs);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    STDMETHOD(Compact)() override { return DD_OK; }
    STDMETHOD(CreateClipper)(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = new Clipper();
        return DD_OK;
    }
    STDMETHOD(CreatePalette)(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*) override { DDM_UNSUPPORTED("IDirectDraw2::CreatePalette"); }
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE* out, IUnknown*) override { return CreateSurfaceV1(d, out); }
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*) override { DDM_UNSUPPORTED("IDirectDraw2::DuplicateSurface"); }
    STDMETHOD(EnumDisplayModes)(DWORD, LPDDSURFACEDESC, LPVOID ctx, LPDDENUMMODESCALLBACK cb) override { return EnumModesV1(ctx, cb); }
    STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override { DDM_UNSUPPORTED("IDirectDraw2::EnumSurfaces"); }
    STDMETHOD(FlipToGDISurface)() override { return DD_OK; }
    STDMETHOD(GetCaps)(LPDDCAPS drv, LPDDCAPS hel) override {
        FillCaps(drv);
        FillCaps(hel);
        return DD_OK;
    }
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC out) override { return GetDisplayModeV1(out); }
    STDMETHOD(GetFourCCCodes)(LPDWORD count, LPDWORD) override {
        if (count) *count = 0;
        return DD_OK;
    }
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE*) override { DDM_UNSUPPORTED("IDirectDraw2::GetGDISurface"); }
    STDMETHOD(GetMonitorFrequency)(LPDWORD out) override {
        if (out) *out = 60;
        return DD_OK;
    }
    STDMETHOD(GetScanLine)(LPDWORD out) override {
        if (out) *out = 0;
        return DD_OK;
    }
    STDMETHOD(GetVerticalBlankStatus)(LPBOOL out) override {
        if (out) *out = FALSE;
        return DD_OK;
    }
    STDMETHOD(Initialize)(GUID*) override { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(RestoreDisplayMode)() override { return DD_OK; }
    STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD flags) override { return SetCooperativeLevelImpl(hwnd, flags); }
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp, DWORD, DWORD) override { return SetDisplayModeImpl(w, h, bpp); }
    STDMETHOD(WaitForVerticalBlank)(DWORD, HANDLE) override { return DD_OK; }
    STDMETHOD(GetAvailableVidMem)(LPDDSCAPS, LPDWORD total, LPDWORD free) override {
        if (total) *total = 256u * 1024u * 1024u;
        if (free) *free = 256u * 1024u * 1024u;
        Logger::Info("DDM: IDirectDraw2::GetAvailableVidMem -> 256 MB.");
        return DD_OK;
    }
};

// The game calls DirectDrawCreate, which hands out the original IDirectDraw.
class DDraw1 final : public IDirectDraw {
public:
    LONG refs = 1;

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectDraw)) {
            *ppv = static_cast<IDirectDraw*>(this);
            AddRef();
            return S_OK;
        }
        return QueryLegacy(riid, ppv, "IDirectDraw");
    }
    STDMETHOD_(ULONG, AddRef)() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    STDMETHOD_(ULONG, Release)() override {
        const LONG r = InterlockedDecrement(&refs);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    STDMETHOD(Compact)() override { return DD_OK; }
    STDMETHOD(CreateClipper)(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = new Clipper();
        return DD_OK;
    }
    STDMETHOD(CreatePalette)(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*) override { DDM_UNSUPPORTED("IDirectDraw::CreatePalette"); }
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE* out, IUnknown*) override { return CreateSurfaceV1(d, out); }
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*) override { DDM_UNSUPPORTED("IDirectDraw::DuplicateSurface"); }
    STDMETHOD(EnumDisplayModes)(DWORD, LPDDSURFACEDESC, LPVOID ctx, LPDDENUMMODESCALLBACK cb) override { return EnumModesV1(ctx, cb); }
    STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override { DDM_UNSUPPORTED("IDirectDraw::EnumSurfaces"); }
    STDMETHOD(FlipToGDISurface)() override { return DD_OK; }
    STDMETHOD(GetCaps)(LPDDCAPS drv, LPDDCAPS hel) override {
        FillCaps(drv);
        FillCaps(hel);
        return DD_OK;
    }
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC out) override { return GetDisplayModeV1(out); }
    STDMETHOD(GetFourCCCodes)(LPDWORD count, LPDWORD) override {
        if (count) *count = 0;
        return DD_OK;
    }
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE*) override { DDM_UNSUPPORTED("IDirectDraw::GetGDISurface"); }
    STDMETHOD(GetMonitorFrequency)(LPDWORD out) override {
        if (out) *out = 60;
        return DD_OK;
    }
    STDMETHOD(GetScanLine)(LPDWORD out) override {
        if (out) *out = 0;
        return DD_OK;
    }
    STDMETHOD(GetVerticalBlankStatus)(LPBOOL out) override {
        if (out) *out = FALSE;
        return DD_OK;
    }
    STDMETHOD(Initialize)(GUID*) override { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(RestoreDisplayMode)() override { return DD_OK; }
    STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD flags) override { return SetCooperativeLevelImpl(hwnd, flags); }
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp) override { return SetDisplayModeImpl(w, h, bpp); }
    STDMETHOD(WaitForVerticalBlank)(DWORD, HANDLE) override { return DD_OK; }
};

// QueryInterface between the DirectDraw object versions (all share the global state).
HRESULT QueryLegacy(REFIID riid, LPVOID* ppv, const char* from) {
    char s[64];
    GuidToString(riid, s, sizeof(s));
    if (IsEqualGUID(riid, IID_IDirectDraw7)) {
        *ppv = static_cast<IDirectDraw7*>(AcquireDD7());
        Logger::Info("DDM: %s::QueryInterface(IID_IDirectDraw7)", from);
        return S_OK;
    }
    if (IsEqualGUID(riid, IID_IDirectDraw2)) {
        *ppv = static_cast<IDirectDraw2*>(new DDraw2());
        Logger::Info("DDM: %s::QueryInterface(IID_IDirectDraw2)", from);
        return S_OK;
    }
    if (IsEqualGUID(riid, IID_IDirectDraw) || IsEqualGUID(riid, IID_IUnknown)) {
        *ppv = static_cast<IDirectDraw*>(new DDraw1());
        Logger::Info("DDM: %s::QueryInterface(IID_IDirectDraw)", from);
        return S_OK;
    }
    Logger::Warn("DDM: %s::QueryInterface(%s) -> E_NOINTERFACE", from, s);
    *ppv = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE DDraw7::QueryInterface(REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectDraw7)) {
        *ppv = static_cast<IDirectDraw7*>(this);
        AddRef();
        return S_OK;
    }
    return QueryLegacy(riid, ppv, "IDirectDraw7");
}

HRESULT STDMETHODCALLTYPE Surface::GetDDInterface(LPVOID* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = static_cast<IDirectDraw7*>(AcquireDD7());
    return DD_OK;
}

// ------------------------------------------------------------------------------------ exports
HRESULT WINAPI Hook_DirectDrawCreate(GUID*, LPDIRECTDRAW* out, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;
    Logger::Info("DDM: DirectDrawCreate -> in-memory DirectDraw.");
    *out = new DDraw1();
    return DD_OK;
}

HRESULT WINAPI Hook_DirectDrawCreateEx(GUID*, LPVOID* out, REFIID iid, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (!IsEqualGUID(iid, IID_IDirectDraw7)) {
        *out = nullptr;
        return DDERR_INVALIDPARAMS;
    }
    Logger::Info("DDM: DirectDrawCreateEx -> in-memory DirectDraw.");
    *out = static_cast<IDirectDraw7*>(AcquireDD7());
    return DD_OK;
}

HRESULT WINAPI Hook_DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD) {
    if (!cb) return DDERR_INVALIDPARAMS;
    char desc[] = "Primary Display Driver";
    char name[] = "display";
    cb(nullptr, desc, name, ctx, nullptr);
    return DD_OK;
}

HRESULT WINAPI Hook_DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx) {
    if (!cb) return DDERR_INVALIDPARAMS;
    char desc[] = "Primary Display Driver";
    char name[] = "display";
    cb(nullptr, desc, name, ctx);
    return DD_OK;
}

void* g_origCreate = nullptr;
void* g_origCreateEx = nullptr;
void* g_origEnumExA = nullptr;
void* g_origEnumA = nullptr;

} // namespace

bool Install() {
    HMODULE hDDraw = GetModuleHandleA("ddraw.dll");
    if (!hDDraw) {
        hDDraw = LoadLibraryA("ddraw.dll");
    }
    if (!hDDraw) {
        Logger::Error("DDM: ddraw.dll is not loaded - cannot redirect DirectDrawCreate.");
        return false;
    }
    int hooked = 0;
    auto hook = [&](const char* name, LPVOID detour, void** orig) {
        FARPROC target = GetProcAddress(hDDraw, name);
        if (target && MH_CreateHook(reinterpret_cast<LPVOID>(target), detour, orig) == MH_OK &&
            MH_EnableHook(reinterpret_cast<LPVOID>(target)) == MH_OK) {
            ++hooked;
        }
    };
    hook("DirectDrawCreate",       reinterpret_cast<LPVOID>(&Hook_DirectDrawCreate),       &g_origCreate);
    hook("DirectDrawCreateEx",     reinterpret_cast<LPVOID>(&Hook_DirectDrawCreateEx),     &g_origCreateEx);
    hook("DirectDrawEnumerateExA", reinterpret_cast<LPVOID>(&Hook_DirectDrawEnumerateExA), &g_origEnumExA);
    hook("DirectDrawEnumerateA",   reinterpret_cast<LPVOID>(&Hook_DirectDrawEnumerateA),   &g_origEnumA);
    g.active = (g_origCreate != nullptr);
    Logger::Info("DDM: in-memory DirectDraw installed (%d ddraw.dll exports redirected).", hooked);
    return g.active;
}

void Shutdown() {
    if (!g.active) return;
    Logger::Info("DDM: shutdown - %u surfaces were created, %zu still alive, presenter: %s.",
                 g.created, g.surfaces.size(), Present::BackendName());
    // Deliberately no Present::Shutdown(): this runs inside DllMain(PROCESS_DETACH) under the
    // loader lock, where releasing a DXGI swap chain can deadlock. The process is ending anyway.
}

bool IsActive() {
    return g.active;
}

bool CopyFront(std::vector<uint16_t>& out, int& w, int& h) {
    std::lock_guard<std::recursive_mutex> lock(g.lock);
    if (!g.primary || !g.primary->st.bits) return false;
    const Buf b = g.primary->buf();
    out.resize(static_cast<size_t>(b.w) * b.h);
    for (int y = 0; y < b.h; ++y) {
        std::memcpy(out.data() + static_cast<size_t>(y) * b.w, b.bits + static_cast<size_t>(y) * b.pitch, static_cast<size_t>(b.w) * 2);
    }
    w = b.w;
    h = b.h;
    return true;
}

bool BackBits(const uint8_t*& bits, int& pitch, int& w, int& h) {
    std::lock_guard<std::recursive_mutex> lock(g.lock);
    if (!g.primary || !g.primary->back || !g.primary->back->st.bits) return false;
    const Buf b = g.primary->back->buf();
    bits = b.bits;
    pitch = b.pitch;
    w = b.w;
    h = b.h;
    return true;
}

bool CopyBack(std::vector<uint16_t>& out, int& w, int& h) {
    std::lock_guard<std::recursive_mutex> lock(g.lock);
    if (!g.primary || !g.primary->back || !g.primary->back->st.bits) return false;
    const Buf b = g.primary->back->buf();
    out.resize(static_cast<size_t>(b.w) * b.h);
    const size_t row = static_cast<size_t>(b.w) * 2;
    if (b.pitch == static_cast<int>(row)) {
        std::memcpy(out.data(), b.bits, row * b.h);
    } else {
        for (int y = 0; y < b.h; ++y) {
            std::memcpy(out.data() + static_cast<size_t>(y) * b.w, b.bits + static_cast<size_t>(y) * b.pitch, row);
        }
    }
    w = b.w;
    h = b.h;
    return true;
}

bool SurfaceAlive(uintptr_t serial, const uint8_t* bits) {
    std::lock_guard<std::recursive_mutex> lock(g.lock);
    for (const Surface* s : g.surfaces) {
        if (s->serial == serial) return s->st.bits == bits;
    }
    return false;
}

bool SurfaceOfDC(void* dc, Buf& out) {
    if (!dc) return false;
    std::lock_guard<std::recursive_mutex> lock(g.lock);
    for (Surface* s : g.surfaces) {
        if (s->st.dc == static_cast<HDC>(dc) && s->st.bits) {
            out = s->buf();
            return true;
        }
    }
    return false;
}

bool FindLockedBuf(const void* address, Buf& out) {
    const auto* p = static_cast<const uint8_t*>(address);
    for (unsigned i = 0; i < kLockHistory; ++i) {
        const LockRecord& rec = g_lastLocks[(g_lockSeq - 1 - i) % kLockHistory]; // newest first
        if (!rec.bits || rec.pitch <= 0) continue;
        if (p >= rec.bits && p < rec.bits + static_cast<size_t>(rec.pitch) * static_cast<size_t>(rec.h)) {
            out.bits = const_cast<uint8_t*>(rec.bits);
            out.pitch = rec.pitch;
            out.w = rec.w;
            out.h = rec.h;
            return true;
        }
    }
    return false;
}

void TraceCalls(const char* path, int count) {
    std::lock_guard<std::mutex> lock(g_traceLock);
    if (g_traceFile) {
        std::fclose(g_traceFile);
        g_traceFile = nullptr;
    }
    g_traceLeft.store(0);
    if (!path || count <= 0) return;
    g_traceFile = std::fopen(path, "w");
    if (g_traceFile) g_traceLeft.store(count);
}

bool TraceActive() {
    return g_traceLeft.load(std::memory_order_relaxed) > 0;
}

void TraceMark(const char* text) {
    DDM_TRACE("%s", text);
}

void LogFaultContext(uintptr_t faultAddress) {
    if (!g.active) return;
    // Newest first. A write fault just past the end of one of these means the game drew
    // something larger than the surface it had locked.
    const unsigned n = (g_lockSeq < kLockHistory) ? g_lockSeq : kLockHistory;
    for (unsigned i = 0; i < n; ++i) {
        const LockRecord& r = g_lastLocks[(g_lockSeq - 1 - i) % kLockHistory];
        const auto begin = reinterpret_cast<uintptr_t>(r.bits);
        const uintptr_t end = begin + static_cast<uintptr_t>(r.pitch) * static_cast<uintptr_t>(r.h);
        char where[64] = "";
        if (faultAddress >= begin && faultAddress < end) {
            std::snprintf(where, sizeof(where), " <- fault INSIDE (+%u)", static_cast<unsigned>(faultAddress - begin));
        } else if (faultAddress >= end && faultAddress - end < 64u * 1024u * 1024u) {
            std::snprintf(where, sizeof(where), " <- fault %u bytes PAST its end", static_cast<unsigned>(faultAddress - end));
        }
        Logger::Error("  Lock[-%u]: surface %dx%d pitch=%d caps=0x%08X bits=%08X..%08X (%s)%s",
                      i, r.w, r.h, r.pitch, r.caps, static_cast<uint32_t>(begin), static_cast<uint32_t>(end),
                      r.virt ? "VirtualAlloc" : "heap", where);
    }
}

} // namespace DDM
