# Zoom, camera clamping and the map size (static RE, Game.exe)

`E` = `RHEngine*` (`[0x6C0FA8]`). **[V]** = verified by disassembly, **[I]** = inferred.
Constants used below: `[0x677488]` = 0.0f, `[0x677598]` = 80.0f (HUD bar), `[0x67749C]` = 1.0f,
`[0x6774F4]` = 2.0f, `[0x6774B8]` = 0.5 (double). `0x642B7C` = CRT `_ftol` (truncation). `0x6433AB` = `floor`. [V]

## 1. Map size

| Field | Meaning |
|---|---|
| `E+0x4728/+0x472C` | viewport W/H (float), copied from the graphics config `+0x54/+0x58` by `OnResolutionChanged 0x4C0040` (0x4C007B/0x4C0085) [V] |
| `E+0x4730/+0x4734` | **map W/H in pixels (float)**. Only writer: level load `0x4C29DC..`, at 0x4C2EFB/0x4C2F05, from two `u16` of the map-file header (`[esp+0xB8]`, `[esp+0xBA]`) [V] |
| `E+0x48C4` | map surface, created with exactly mapW x mapH (`0x5E4ED0`, 0x4C2F31) [V] |
| `E+0x48CC` | world surface = viewport size (W x H, *not* minus 80), recreated in 0x4C0040 (0x4C00C7) [V] |
| `E+0x4738/+0x473C` | camera = world coords of the screen's top-left corner [V] |
| `E+0x4740` | camera at the last render (scroll delta source) [V] |
| `E+0x475C` | current zoom; `E+0x4760` zoom at last render [V] |
| `E+0x4898` (u16) | zoom index, `E+0x489C[3]` = {0.5, 1.0, 2.0} table (ctor 0x4BEF3B..0x4BEF59, index starts at 1) [V] |
| `E+0x4790` / `E+0x4791` | "zoom-in / zoom-out in progress" flags [V] |

The view in world pixels is always `(vpW, vpH-80) / zoom`. Every clamp reads mapW/mapH directly from `E+0x4730/4734`. [V]
`RHEngine::Serialize` (0x4C4676..0x4C603A, `ret 4`) saves/loads `E+0x4738` and `E+0x475C` (0x4C4780, 0x4C4790) but
**not** the index `E+0x4898` (no other access to 0x4898 exists). [V]

## 2. Camera clamping

### 2a. `ClampScrollDelta` 0x4CEC60 — `__thiscall(E)`, no stack args, plain `ret`, returns `al` = 1 if nothing was clamped [V]
Called from `Update` state 2 (0x4C86D8). It clamps the *velocity* `E+0x48B8/48BC`, not the camera:
```
vw = ftol(vpW/zoom) & 0xFFFF
if (cam.x + vel.x + vw > mapW) { vel.x = mapW - cam.x - vw; inertia.x(0x4782)=0 }   // 0x4CECAC  right/bottom first
if (cam.x + vel.x < 0)         { vel.x = -cam.x;           inertia.x=0 }            // 0x4CECE6  left/top LAST
vh = ftol((vpH-80)/zoom);  sumY = vh + ftol(cam.y) + ftol(vel.y)                   // integer sum
if (sumY > mapH)               { vel.y = mapH - vh - ftol(cam.y); 0x4784=0 }        // 0x4CED4B
if (cam.y + vel.y < 0)         { vel.y = -cam.y; 0x4784=0 }                         // 0x4CEDAD
```
**View larger than map: the second test always wins, so the camera is driven to 0 (map at top-left).** [V]

### 2b. Other clamps — all do left/top first, right/bottom last (=> negative camera when view > map) [V]
* **Zoom-out transition** (Update state 3, 0x4C8982..0x4C8D57): `cam = cam + view/(2*oldZoom) - view/(2*newZoom) + anchor`;
  then `if cam.x<0 → 0`, `if cam.x+vw>mapW → cam.x = mapW-vw` (0x4C8BCD), same for y (0x4C8C3B / 0x4C8CCA);
  overshoots stored in `E+0x48B0/48B4`; floor; `SetCameraPosition` (0x4C8D57).
* **Zoom-in transition** (0x4C8D7B..0x4C8FAB): no map clamp at all; two `SetCameraPosition` calls, the first
  (0x4C8EBC) with the *centre point*, the second (0x4C8FAB) with `centre - view/(2*newZoom) - anchor/2`.
* **`CenterCameraOn` 0x4C7F60** — `__thiscall(E, Vec2* worldPt)`, `ret 4`: `cam = pt - (vpW,vpH)/(2*zoom)`
  (note: full vpH), clamp x<0, then right (0x4C806C), y<0, then bottom (0x4C80A9), `SetCameraPosition`.
  Callers 0x4C812B, 0x54C15E.
* **0x5758D0 (not a "ClampCameraAndZoom" in the sense of a zoom veto)** — `__cdecl Vec2* f(Vec2* out, const Vec2* centre)`,
  plain `ret`, returns `out` [V]. Computes `p = trunc(centre - (vpW,vpH)/(2*zoom))`, then:
  - x<0 → x=0 and remember it; y<0 → y=0 and remember it;
  - `if x + vpW/zoom > mapW`: if x was already clamped (view wider than map) → **`zoom = 1.0`, `out = (0,0)`, return**
    (0x5759C9; `E+0x4898` is *not* updated) else `x = mapW - vpW/zoom` (can be negative);
  - same for y with `(vpH-80)/zoom` (0x575A31);
  - at zoom 0.5 rounds x,y down to even; `out = (x,y)`.
  Callers: script "set camera" 0x4CA646/0x4CA786 (write `E+0x4738` directly, no `SetCameraPosition`), script
  "travel to" 0x4CA6E9 and 0x5712D4/0x571394/0x571442 (write the travel target `E+0x4748`), resolution change
  0x4C00F7/0x4C0239, `Serialize` 0x4C5E8C/0x4C5F00.
* **`SetCameraPosition` 0x4DB7F0** — `__thiscall(E, Vec2*)`, `ret 4`: `cam = floor(pos)`; at zoom 0.5 odd values -1. No clamp. [V]
* **0x4C0340** — its *only* reference is 0x4DA49C inside 0x4DA394, the full-map screenshot routine (sets zoom 1,
  tiles the whole map). Hooking it (as `widescreen_patch.cpp` does) does not affect gameplay. [V]

## 3. Zoom input path

1. Engine input handler, action dispatch `0x4DCAA1` (jump table 0x4DCDAC/0x4DCDE8), `this = E+0x60`:
   zoom-in action → `0x4DBE40(E, evt)` (0x4DCAF9), zoom-out action → `0x4DBDB0(E, evt)` (0x4DCB07). [V]
   (Key vs. HUD button vs. wheel → action id mapping not traced. [I]: all go through this dispatch.)
2. **`ZoomOutRequest` 0x4DBDB0** — `__thiscall(E, Event* evt)`, `ret 4`:
   ```
   if (!CanZoomOut(E))  return;            // 0x4DBDB3 → je 0x4DBE34 (pop esi; ret 4)
   if (!CanStartZoom(E)) return;           // 0x4C7C90: !0x4768 && state!=4 && !0x4790 && !0x4791
   0x4793=0; 0x4769 = (evt+8 != 0)         // [I] 1 = zoom about screen centre instead of the cursor
   if (inertia 0x4782/0x4784 != 0 || state is 3/4) { 0x4792=0; 0x4793=1; return; }  // [I] deferred
   if (idx 0x4898 == 0) return;
   idx--; 0x4791=1; RaiseCameraState(3)    // 0x4DB8C0: state = max(state, arg), thiscall, ret 4
   ```
   `ZoomInRequest` 0x4DBE40 is symmetric with `CanZoomIn` 0x4C7CD0 (`zoom < 2.0`), `idx < 2`, `0x4790=1`.
3. **`CanZoomOut` 0x4C7CF0** — `__thiscall(E)`, no stack args, plain `ret`, result in `al` (eax upper bytes undefined),
   uses only eax and the FPU (stack balanced) [V]:
   ```
   return zoom > 0.5 && mapW >= vpW*2/zoom && mapH >= (vpH-80)*2/zoom;
   ```
   **So the original game already refuses a zoom-out whose view would be larger than the map.** [V]
   Other callers only use it to enable/grey the HUD zoom-out button: 0x4C84D0, 0x4C9159 (`Update`, message 9/0xA
   to `[E+0x68]`), 0x4CE879, 0x50E213 / 0x5143E2 (`HUD+0x1B7C → vtbl[+0x54](bool)`), and 0x4C01D8 (resolution change:
   `if zoom==0.5 && !CanZoomOut → idx++, zoom=1.0`, 0x4C01CF..0x4C01E8).
4. `Update` 0x4C8380 state machine (jump table 0x4C94A8): 1 → 0x4C929B (blit world surface + objects),
   2 → 0x4C86D6 (scroll), 3 → 0x4C8776 (zoom start: sets the new zoom, computes/clamps the camera, renders into
   temp surfaces `E+0x4788/478C`), 4 → 0x4C9129 (animation `0x4CF610` zoom-out / `0x4CFCE0` zoom-in; while it returns
   non-zero the state stays 4), 5 → 0x4C867C (full redraw). **Transition end**: 0x4C915E (`state = 1`, flags cleared,
   temp surface freed, HUD told `CanZoomIn<<16 | CanZoomOut`). [V]

**Best veto point: 0x4C7CF0 (`CanZoomOut`).** Returning `al=0` makes 0x4DBDB0 return before touching any state
(identical to the key not being pressed), and the HUD button greys out consistently because every HUD query uses the
same function. Original bytes: `D9 81 5C 47 00 00 DC 1D B8 74 67 00 DF E0 F6 C4 41 75 47` (19 bytes, no relative
branches/calls in the first 12, so a 5-byte JMP detour/MinHook is safe). Wrapper: `bool __fastcall(E*, edx)`;
call the original and AND it with the extra condition. [V]

## 4. Is a negative camera origin safe? (fix A)

* **Background** `RenderBackground 0x4C9B30` (`__thiscall(E, dstSurf)`, `ret 4`): `src = Rect(cam, cam + (vpW,vpH-80)/zoom)`
  on the map surface `E+0x48C4`, `dst = Rect(0,0,vpW,vpH-80)`, via `0x5E6440`. [V]
* **Blit** `0x5E6440` — `__thiscall(drawMgr, srcSurf, Rect* src, dstSurf, Rect* dst, flags)`, `ret 0x14`: converts the
  float rects with `_ftol` to a `RECT` and calls `IDirectDrawSurface7::Blt` (vtbl+0x14) **with no clipping**
  (stretch is done by Blt: at zoom 0.5 src is 2x dst). On failure it logs "Error during the blit" and returns 0. [V]
  Real DirectDraw rejects a source rect outside the source surface (DDERR_INVALIDRECT) [I, wrapper behaviour unknown].
  So with a negative camera, or with any view larger than the map even at camera 0, the original code issues an
  out-of-bounds source rect. What the user currently sees (map top-left, black bands) is the GOG wrapper's handling of
  that rect or the world surface's old contents [I].
* **Scroll** `0x4CEDE0`: self-blit of the world surface by `(cam - lastCam)*zoom`, then strips from the map surface whose
  source rects are again `cam + offset/zoom` (same `0x5E6440`, no clipping). [V]
* **Objects / picking**: `0x4D0A10` view rect and sprite clipping are float (`screen = clipped - view.min`, halved at 0.5,
  see OBJECT_RENDER.md). `ScreenToWorld 0x4DEE70` (`__thiscall(E, Vec2* out)`, `ret 4`) = `cursor/zoom + cam`, pure float. [V]
  A cursor over a band therefore yields world coordinates outside the map (negative or > mapW); what pathfinding /
  grid lookups do with them was **not** checked. [I: risky]
* Conclusion: **fix A is feasible only with several changes**: (1) hook `0x5E6440` when `srcSurf == E+0x48C4` and clip
  the source rect to `[0,mapW]x[0,mapH]`, shrinking the destination rect by the same amount x zoom; (2) clear the world
  surface bands (black fill) before `RenderBackground` and after scroll self-blits; (3) make `ClampScrollDelta`, 0x5758D0,
  `CenterCameraOn` and the zoom-out clamp produce `cam = (mapW - vw)/2` on the axis where the view is larger;
  (4) reject clicks outside the map. Not recommended.

## 5. Why bands can still appear, and the recommended patch

Because of `CanZoomOut`, the zoom key alone cannot produce a 0.5 view larger than the map [V]. Paths that can:
* **Savegame** made at a lower resolution with zoom 0.5, loaded at 2560x1440: `Serialize` restores zoom 0.5 [V]; the
  post-load clamp 0x5758D0 (0x4C5E8C) resets the zoom only when the view overflows on *both* sides of the centre,
  otherwise it returns a negative camera and keeps 0.5 [V]. The mod's `EnforceSafeCameraBounds` then pins it to 0 → bands
  right/bottom.
* **Zoom 1.0 on a map smaller than 2560x1360** (e.g. small interiors): no zoom can fix it; camera pinned to 0 by
  `ClampScrollDelta` and the mod. Needs fix A.
* If bands are seen right after pressing zoom-out on a map whose real size is < 5120x2720, then `E+0x4730/4734` must be
  larger than the visible image (black padding inside the map surface). Log mapW/mapH, viewport and zoom at 0x4DBDB0
  to confirm. [I — could not be checked statically; DATA was not read]

**Recommended smallest safe patch (fix B, completes the original veto):**
1. Keep the original veto (nothing to patch at 0x4C7CF0 unless a margin is wanted).
2. In the existing hook of `0x5758D0` (`__cdecl(out, centre)`), **before** calling the original: if
   `zoom < 1.0 && (mapW < vpW/zoom || mapH < (vpH-80)/zoom)` then `zoom = 1.0; idx(E+0x4898) = 1; E+0x48D0 = 0`.
   The original then centres on `centre` with zoom 1.0. `centre` is zoom-independent, so this is correct for all callers. [V for
   the arithmetic, I for the visual result]
3. The HUD zoom buttons are refreshed only at the end of a zoom transition (0x4C9159) and in the `E+0x48D1 == 0`
   re-init branch of `Update` (0x4C84B1, and there only when a zoom was in progress), so after a forced reset the
   buttons may show a stale state until the next zoom. Cosmetic. [V for the call sites, I for the effect]
4. Optional cleanup: the hook on 0x4C0340 is dead for gameplay; the `SetCameraPosition` clamp should allow a camera
   < 0 only if fix A is ever implemented.
