# DirectDraw usage in `Game.exe` (static RE)

All addresses are VAs in `Game.exe`. "(inferred)" marks conclusions that were not read directly. HRESULT names come from the DX7 SDK values (`0x887601C2` SURFACELOST, `0x8876017C` OUTOFVIDEOMEMORY, `0x8876024B` WRONGMODE).
**Correction to RE_NOTES:** `SBDrawManager.cpp` code covers **0x005E3B10..0x005EF390**, not just ..0x005E6500. The console/text functions at 0x005ED270..0x005EE620 also call DirectDraw. There is also a DX-version probe at 0x005E1440 that loads `DDRAW.DLL` dynamically.

## 1. Creation
- **DX version probe** `0x005E1440`, called from WinMain at `0x0040A893` before the manager exists. A result below `0x700` shows "Please Install a newer version of DirectX" and exits (`0x0040A898`). Steps:
  1. `LoadLibrary("DDRAW.DLL")`, `GetProcAddress("DirectDrawCreate")`, `DirectDrawCreate(NULL,&dd,NULL)` (`0x005E14B8`).
  2. `dd->QI(IID_IDirectDraw2 {B3A6F3E0-2B43-11CF-A2DE-00AA00B93356} @0x67A688)` (`0x005E14EE`), released at once.
  3. `dd->SetCooperativeLevel(NULL, DDSCL_NORMAL)` (`0x005E15D3`), then `CreateSurface(DDSURFACEDESC{0x6C, DDSD_CAPS, PRIMARYSURFACE})` (`0x005E160D`).
  4. On that surface: QI `IID_IDirectDrawSurface3 {DA044E00-69B2-11D0-A1D5-00AA00B8DFBB}` (`0x005E164A`) and `IID_IDirectDrawSurface4 {0B2B8630-AD35-11D0-8EA6-00609797EA5B}` (`0x005E167E`). Both results are leaked.
  5. `GetProcAddress("DirectDrawCreateEx")`, `DirectDrawCreateEx(NULL,&p,IID_IDirectDraw7,NULL)` (`0x005E1739`), released.
  6. If `D3D8.DLL` loads the result is 0x800, otherwise 0x700. Any failed step returns a version below 0x700.
- **Manager ctor** `0x005E3C20` (called `0x0040A96E`). After creating the window it calls `DirectDrawEnumerateExA(0x005E3B10, &mgr->adapterList(+0x18), DDENUM_ATTACHEDSECONDARYDEVICES=1)` (`0x005E3E99`). The return value is ignored. The callback copies only `lpDriverDescription` into a `std::list<std::string>` and returns 1 (`0x005E3B2F`, `0x005E3C00`). GUID, name and HMONITOR are ignored. The list is never read again inside the manager (inferred: no other references to +0x18..+0x20).
- **Init** `0x005E41D0(width, height, flags)` (called `0x0040AA11` with `flags = esi = 1`, inferred from `0x0040A266`). It calls `DirectDrawCreate(NULL, &mgr+0x84, NULL)` (`0x005E4268`, result unchecked). On v1 it calls only `SetCooperativeLevel`, then `QueryInterface(IID_IDirectDraw7 {15E65EC0-3B9C-11D2-B92F-00609797EA5B} @0x67A788, &mgr+0x88)` (`0x005E42B0` windowed, `0x005E444F` fullscreen; failure is only logged). v1 is kept until the destructor releases it (`0x005E4D87`).

## 2. SetCooperativeLevel (always called on the **v1** pointer +0x84)
| Site | Condition | hwnd | Flags |
|---|---|---|---|
| `0x005E429C` | `mgr+0x80 == 0` (windowed) | +0x100 | `DDSCL_NORMAL` (8), unchecked |
| `0x005E43C8` | fullscreen | +0x100 | `0x413` = FULLSCREEN\|ALLOWREBOOT\|EXCLUSIVE\|MULTITHREADED; failure logged |
| `0x005E3FF4` | reset, after `RestoreAllSurfaces` returns WRONGMODE | +0x100 | `0x413` (no windowed check) |
| `0x005E15D3` | DX probe | NULL | `DDSCL_NORMAL` |

The fullscreen byte `0x6C0684` is set to 1 at `0x0040A347`, and no other write to it was found. The windowed path is probably dead (inferred).

## 3. Other IDirectDraw7 methods
- `SetDisplayMode(w, h, 16, 0, 0)` (v7, 5 args): init fullscreen (`0x005E44DD`, failure logged), reset loop (`0x005E4088`, failure is fatal), SetResolution `0x005E4810` (`0x005E4930`, unchecked, fullscreen only). SetResolution is called with (640, 480, 16) (`0x0040B019`, `0x00537437`) and with the config size and 16.
- `RestoreAllSurfaces`: at the end of SetResolution (`0x005E4C7F`), and in the reset loop `0x005E3F70` (`0x005E3FC9`), which is called after window re-activation (`0x00409642`, `0x005E197C`). The loop returns true on DD_OK. On WRONGMODE it calls SetCooperativeLevel and SetDisplayMode, then retries. Any other error logs "Unable to restore the primary surface" and returns false.
- **Never called** (no vtable call at those slots anywhere): GetDisplayMode, RestoreDisplayMode, EnumDisplayModes, GetCaps, GetAvailableVidMem, TestCooperativeLevel, WaitForVerticalBlank, GetDeviceIdentifier, FlipToGDISurface, CreatePalette. (RE_NOTES' "GetCaps 0x2C" is really surface Flip.)

## 4. CreateSurface (dd7 unless noted)
| Site | dwFlags | ddsCaps | Other | On failure |
|---|---|---|---|---|
| `0x005E4328` init windowed primary | 1 | 0x200 PRIMARY | – | unchecked |
| `0x005E45D3` init fullscreen, flags&1 | 0x21 | 0x218 PRIMARY\|FLIP\|COMPLEX | BackBufferCount=1 | unchecked |
| `0x005E45D3` init fullscreen, !(flags&1) | 1 | 0x200 | then offscreen back buffer via `0x005E4ED0(w,h,1)` | unchecked |
| `0x005E4AEE` SetResolution, flags&1 | 0x21 | 0x218 | BBC=1; both buffers then colour-filled black | unchecked |
| `0x005E49DB` SetResolution, !(flags&1) | 1 | 0x200 | – | unchecked |
| `0x005E5068` offscreen wrapper `0x005E4F70(id,w,h,f)` | 7 (CAPS\|HEIGHT\|WIDTH) | f=0 is treated as 1. f&1: 0x840 SYSTEMMEMORY\|OFFSCREENPLAIN; else f&2: 0x4040 VIDEOMEMORY\|OFFSCREENPLAIN; else f&8: **0x20004040** NONLOCALVIDMEM\|VIDEOMEMORY\|OFFSCREENPLAIN (not 3DDEVICE); f==4 gives caps 0 | No pixel format, so the display format is used | OUTOFVIDEOMEMORY: recursive retry with f=1. Other errors: log, return false |
| `0x005E160D` DX probe (v1, DDSURFACEDESC 0x6C) | 1 | 0x200 | – | returns version 0 |

Width and height are the low 16 bits of the arguments. In windowed mode the "back buffer" (id 1) is an offscreen surface of type f=2 if flags&1, else f=1 (`0x005E43A0`, `0x005E4C49`). The console surface (`0x005ED29F`) uses f=0, i.e. system memory, at screen size.

## 5. Attachments
`GetAttachedSurface(&DDSCAPS2{DDSCAPS_BACKBUFFER=4,0,0,0})` on the primary: `0x005E4626` and `0x005E4B4E`. Failures are logged. AddAttachedSurface, DeleteAttachedSurface and EnumAttachedSurfaces are never used.

## 6. Lock (`0x005E5AC1`, the only Lock)
`Lock(NULL, &ddsd(dwSize=0x7C), DDLOCK_WAIT=1, NULL)`. The wrapper `0x005E5A00(id, vp*, flags)` uses `flags` only to remap the id. `id 0` becomes 1 (back buffer) unless `mgr+0x112 != 0` or `(flags & 0x18) == 0x18`. +0x112 is only ever written as 0 (`0x005E3D71`). It enters the per-surface CS first. **SURFACELOST**: `Restore()`, leave CS, return false. Other errors: leave CS, log, return false. On success the CS stays held and `rec.locked=1`. Fields read and the viewport filled:

| vp offset | value |
|---|---|
| +0x00 | remapped id |
| +0x04 (word) | `ddsd.dwWidth` |
| +0x06 (word) | `ddsd.dwHeight` |
| +0x08 | `ddsd.lPitch` (bytes) |
| +0x0C (word) | `mgr+0x10C` (15 or 16) |
| +0x0E (word) | `rec.colorKey` |
| +0x10 | `ddsd.lpSurface` |
| +0x14 (byte) | 1 |

The pixel format in the DDSD is not checked (`0x005E5B64..0x005E5BA9`). Bink decodes straight into a locked back buffer (`0x004012AC`, `0x004012D8`).

## 7. Unlock
`Unlock(NULL)` (`0x005E5DD5`). Wrapper `0x005E5D30(id)` remaps id 0 to 1 **unconditionally**. On success it clears `locked` and leaves the CS. On failure it logs and does not leave the CS. `0x005E5EA0(vp)` unlocks `vp->id`.

## 8. Blt
| Site | dest, destRect | src, srcRect | dwFlags | FX | Failure |
|---|---|---|---|---|---|
| `0x005E4B08`, `0x005E4BDE` | primary / back buffer, NULL | – | COLORFILL\|WAIT `0x1000400` | dwFillColor=0 | unchecked |
| `0x005E5324` (console+flip) | back buffer, NULL | console, NULL | WAIT `0x1000000` | NULL | log, `primary->Restore()` |
| `0x005E5405` (console, no flip) | primary, NULL | console, NULL | WAIT | NULL | log, Restore |
| `0x005E54FA` (fullscreen, no flip) | primary, NULL | id 1, NULL | WAIT | NULL | unchecked |
| `0x005E55C3`, `0x005E561A` (windowed) | primary, client rect in **screen** coordinates | console or id 1, NULL | WAIT | NULL | log, Restore. **Stretches** when the client size differs from the surface |
| `0x005E63AE` fill wrapper `0x005E61E0(id, floatRect*, color, flags)` | id (0 maps to 1 unless flags&8), NULL or `{ftol(l),ftol(t),ftol(r),ftol(b)}`; empty rect returns false without a Blt | – | COLORFILL\|WAIT | dwFillColor=color&0xFFFF (raw 16-bit, e.g. 0xF800) | log, return false |
| `0x005E67D1` blit `0x005E6440(srcId, srcRectF, dstId, dstRectF, flags)` | dst (0 maps to 1 unless flags&8) | src (0 maps to 1 unless flags&0x10) | flags&1 ? `0x8200` ASYNC\|KEYSRC : `0x200` ASYNC (**no WAIT**) | **NULL** | SURFACELOST: `dest->Restore()`, false. Else log, false |
| `0x005EDC25`, `0x005EE1EB` console scroll | console, `{x1,y1,x2,y2-3h}` | **same surface**, `{x1,y1+h,x2,y2-2h}` (overlapping) | WAIT | NULL | log |
| `0x005EDCB7`, `0x005EE287` | console, last line | – | COLORFILL\|WAIT | 0 | unchecked |

In 0x005E6440 the two rects are independent, so **stretching is possible**. With KEYSRC a NULL rect is replaced by the full surface rect (`0x005E6594`, `0x005E6622`). The blit holds the dest CS only. In windowed mode, rects on id 0 are offset by the client origin. `0x005E277A` blits id X to the real primary (flags 8, NULL rects). DDBLT_DDFX, ROP and mirroring are never used.

## 9. Flip and Present (`0x005E5260`)
- Fullscreen: if console mode (`+0x113==1`), blit the console (id `+0x78`) as in §8. Then, if `+0x7C & 1`, call `primary->Flip(NULL, DDFLIP_WAIT=1)` (`0x005E538C`, `0x005E547D`); on failure log and `primary->Restore()`. Otherwise Blt id 1 to the primary.
- Windowed ("simulated flip"): `GetClientRect` and `ClientToScreen`, then Blt to the primary with that rect (`0x005E554B..0x005E561A`).

## 10. Colour key
- Creation (`0x005E510A`) and wrapper `0x005E5EC0(id,color)` (`0x005E5F4E`; one caller, `0x0061B605`). Both call `SetColorKey(DDCKEY_SRCBLT=8, &rec+0x08)`. The low value is `color & 0xFFFF`; the default is `0x7C0` for 565 and `0x3E0` for 555 (`0x005E5033..0x005E5057`). **The high value is uninitialized stack memory**, so a replacement must use only the low value when DDCKEY_COLORSPACE is not set. The result is ignored. GetColorKey is never called. `0x005E5F60` returns the cached value.

## 11. GetPixelFormat (`0x005E471A`, init only, on the primary)
Only `dwGBitMask` (pf+0x14) is read:
- `0x7E0`: `+0x10C=16`, `+0x70=0x20000`, MMX masks `0x6C11C8=0x1F801F00` and `0x6C11CC=0x1F001F00`.
- `0x3E0`: `+0x10C=15`, `+0x70=0x10000`, `0x6C11B8` and `0x6C11BC = 0x1F001F00`.
- Anything else leaves `+0x10C=0xFFFF`.

`+0x10C==16` also selects BINKSURFACE565 (10) instead of 555 (9) (`0x004011E4`). `+0x70` and the masks select the software/MMX blitters (`0x005E6953`, `0x006220A8`).

## 12. GetDC / ReleaseDC
- Wrapper `0x005E5890(id, HDC*, flags)` (one caller, `0x0061B367`, flags 0): id 0 maps to 1 unless flags&8. It enters the surface CS and never leaves it. ReleaseDC wrapper `0x005E5BD0` (`0x005E5C71`) remaps 0 to 1 unconditionally and does **not** leave the CS.
- Direct calls from the text functions: back buffer id 1 (`0x005ED3AC`, `0x005ED5BD`) and the console surface (`0x005ED860`, `0x005EDCC5`, `0x005EDF03`, `0x005EE295`), each with matching ReleaseDC. The primary is never targeted.

## 13. Clipper (windowed only)
`dd7->CreateClipper(0, &mgr+0x8C, NULL)` (`0x005E433B`), `clipper->SetHWnd(0, hwnd)` (`0x005E434C`), `primary->SetClipper(clipper)` (`0x005E4359`). All unchecked. The clipper is released in the destructor (`0x005E4D67`).

## 14. IsLost / Restore
IsLost is never called. `Restore()` is called after Present failures (`0x005E537F`, `0x005E53E8`, `0x005E5465`, `0x005E54DD`, `0x005E569D`), after a SURFACELOST Lock (`0x005E5ADC`), and after a SURFACELOST blit on the dest (`0x005E67F2`). The result is ignored.

## 15. Surface QueryInterface
Only in the DX probe (Surface3 and Surface4 on a v1 surface). No gamma, colour control, Direct3D or palettes.

## 16. Completeness
1. Each DDRAW import has one reference. `"DDRAW.DLL"` and `"DirectDrawCreateEx"` are used only in 0x005E1440. IID_IDirectDraw7 is used only at `0x005E1733`, `0x005E42A9` and `0x005E4448`.
2. `vcalls` over 0x005E3000..0x005EF400: every COM call is listed above.
3. After each of the ~150 loads of `g_DrawManager` (`0x6C11D0`), the next 60 instructions were scanned. Outside code reads only +0x100, +0x10C and +0x115, never the map (+0x54/+0x58) or +0x84/+0x88/+0x8C.
4. Whole-exe scan for COM-style calls (`mov rV,[rO]; push rO; call [rV+d]`) and for calls on objects loaded from `[x+0x84/0x88/0x8C]`. Hits outside the two ranges were thiscall C++ or DInput (checked `0x004273D2`, `0x004CD733`, `0x004D9D96`, `0x00618B2A`, `0x005E2758`).

**Conclusion: no DirectDraw calls outside 0x005E1440..0x005E1795 and 0x005E3B10..0x005EF390** (heuristic scan, not proof).

## 17. Manager layout (`new 0x150`, global `0x6C11D0`)
| Off | Meaning |
|---|---|
| +0x00 | CRITICAL_SECTION (manager) |
| +0x18 | `std::list<string>` adapter descriptions (+0x1C head, +0x20 size) |
| +0x48/+0x4C | text-output disable flags |
| +0x50 | next surface id |
| +0x54 | `std::map<int,SurfRec>` (+0x58 head node, +0x60 size). Node: +0x0C key, +0x10 record |
| +0x64 | `std::list<int>` tracked ids |
| +0x70 | software-blit pixel flag (0x10000 = 555, 0x20000 = 565) |
| +0x74, +0x7C | init flags (bit0 = flip / video memory) |
| +0x78 | console surface id |
| +0x80 | fullscreen (byte) |
| +0x84 | IDirectDraw (v1) |
| +0x88 | IDirectDraw7 |
| +0x8C | IDirectDrawClipper |
| +0xFC | HINSTANCE |
| +0x100 | HWND |
| +0x10C | word 15/16/0xFFFF |
| +0x10E/+0x110 | word width/height |
| +0x112 | "address real primary" (always 0) |
| +0x113 | console active |
| +0x114 | console created |
| +0x115 | DD initialised |
| +0x118..+0x124 | console rect (floats) |
| +0x12C | console line height |

Ids: **0 = primary**, **1 = back buffer**, 2.. = offscreen surfaces.

SurfRec (0x18 bytes): +0x00 `IDirectDrawSurface7*`, +0x04 word w, +0x06 word h, +0x08 DDCOLORKEY (low = key, high = garbage), +0x10 byte locked, +0x14 `CRITICAL_SECTION*` (heap, 0x18 bytes). In fullscreen the destructor does not Release id 1 (it is attached to the primary) (`0x005E4CDB`).

## Minimum method set a replacement must implement
| Interface | Method | Required behaviour |
|---|---|---|
| exports | DirectDrawCreate | Return IDirectDraw v1 (static import and GetProcAddress) |
| exports | DirectDrawCreateEx | Must exist and succeed for IID_IDirectDraw7, or the game exits |
| exports | DirectDrawEnumerateExA | Call the callback at least once (description string), return DD_OK |
| IDirectDraw | QueryInterface | IID_IDirectDraw2 (released at once) and IID_IDirectDraw7 |
| IDirectDraw | SetCooperativeLevel | Accept NULL/NORMAL, hwnd/NORMAL, hwnd/0x413 |
| IDirectDraw | CreateSurface | v1 DDSURFACEDESC (0x6C) primary, returning a surface that QIs to Surface3 and Surface4 |
| IDirectDraw | Release | Normal release |
| IDirectDraw7 | SetDisplayMode | (w, h, 16, 0, 0) |
| IDirectDraw7 | CreateSurface | Primary (with and without 1 attached back buffer) and offscreen (flags 7); 16-bit RGB in the display format; may return OUTOFVIDEOMEMORY for video memory |
| IDirectDraw7 | CreateClipper | Return a clipper |
| IDirectDraw7 | RestoreAllSurfaces | DD_OK, or WRONGMODE to force a mode reset |
| IDirectDraw7 | Release | Normal release |
| IDirectDrawSurface7 | Lock | NULL rect, DDLOCK_WAIT; fill lpSurface, lPitch, dwWidth, dwHeight |
| IDirectDrawSurface7 | Unlock | NULL rect; tolerate an Unlock of a surface that is not locked |
| IDirectDrawSurface7 | Blt | COLORFILL, plain copy, KEYSRC; NULL or arbitrary rects; stretching; overlapping self-blit; ASYNC must be completed synchronously with DD_OK |
| IDirectDrawSurface7 | Flip | (NULL, DDFLIP_WAIT) on the primary |
| IDirectDrawSurface7 | GetAttachedSurface | DDSCAPS_BACKBUFFER |
| IDirectDrawSurface7 | GetPixelFormat | dwGBitMask 0x7E0 (565) or 0x3E0 |
| IDirectDrawSurface7 | SetColorKey | DDCKEY_SRCBLT, low value only |
| IDirectDrawSurface7 | GetDC / ReleaseDC | GDI TextOut on the back buffer and offscreen surfaces |
| IDirectDrawSurface7 | Restore | Return DD_OK |
| IDirectDrawSurface7 | SetClipper | Windowed only |
| IDirectDrawSurface7 | Release | Normal release |
| IDirectDrawSurface | QueryInterface, Release | Surface3/Surface4 results are leaked and never used |
| IDirectDrawClipper | SetHWnd, Release | Normal behaviour |

## Open questions
1. Lock, Fill and Blit call sites with register-valued flags (about 15 Lock sites, e.g. cursor `0x005F30EE`, `0x005BB893`) were not resolved. It is not settled whether game code locks the real primary with `flags&0x18==0x18`; it seems rare (inferred).
2. The init flags argument is `esi` (= 1, inferred). Fullscreen = 1 because only one write to `0x6C0684` exists. Confirm with a call log.
3. The configured resolution (floats at `[0x55DBB0()]+0x54/+0x58`) was not resolved. 640x480 is also used.
4. CS imbalance: GetDC/ReleaseDC and failed Unlocks keep the per-surface CS held. Harmless on one thread (inferred), but a replacement must not block on it.
5. Unlocking via the viewport passes id 0 as 1, so a locked primary would never be unlocked. It is unknown whether this ever happens at runtime.
