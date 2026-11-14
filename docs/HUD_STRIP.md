# The 80 px HUD strip: where it is baked in, and how to remove it (static RE, Game.exe)

`E` = `RHEngine*` `[0x6C0FA8]`, `G` = `RHGame*` `[0x6C0B3C]`. **[V]** = verified by disassembly, **[I]** = inferred.
Constants: `[0x677598]` = 80.0f, `[0x677488]` = 0.0f (.rdata, read-only). Vec2 helpers: `0x5FC3C0` = `Vec2::Vec2(x,y)`
(thiscall, `ret 8`, args pushed y first), `0x5FC190` = `out = this + other` (thiscall `(out*, other*)`), `0x5FC580` =
`out = v * s` (cdecl), `0x5FC5D0` = `out = this / s`. Site checker: `python tools\hud_strip_sites.py` (verifies original bytes).

## 1. Encodings of the strip

There are exactly three encodings in engine code, plus the HUD layout in `RHGame`:

* **A. `fsub dword ptr [0x677598]`** after `fld [E+0x472C]` (vpH - 80). 18 sites, all `D8 25 98 75 67 00`. [V]
* **B. `push 0xC2A00000`** = y of `Vec2(x, -80.0f)` that is then added to `E+0x4728` (`0x5FC190`) -> `(vpW, vpH-80)`.
  24 sites, all `68 00 00 A0 C2`. [V] (In 23 sites x = `push 0`/`push ebx`/`push ebp`, unchanged by the patch.)
* **C. Integer `-0x50` / float 80 in `RHGame` HUD layout** (`G+4/+6` = screen W/H as u16). Positions the HUD windows. Keep.

No float/double -80 or double 80 constant exists in data; all other `0x42A00000` (80.0f) immediates are unrelated (below). [V]

### A. References to `[0x677598]` (23)

| Address | Function | Class | Evidence |
|---|---|---|---|
| 0x4C0414, 0x4C046F | 0x4C0340 (full-map screenshot tiler, only ref 0x4DA49C) | world render size (screenshot) | `fld [edi+0x472C]; fsub` [V] |
| 0x4C7D2C | `CanZoomOut` 0x4C7CF0 | zoom check | `mapH >= (vpH-80)*2/zoom` [V] |
| 0x4C808C, 0x4C80B5 | `CenterCameraOn` 0x4C7F60 | camera clamp (bottom) | centre uses full vpH, clamp uses vpH-80 [V] |
| 0x4CED05, 0x4CED62 | `ClampScrollDelta` 0x4CEC60 | camera clamp | `vh = ftol((vpH-80)/zoom)` [V] |
| 0x4CEEFB, 0x4CEF21, 0x4CEF3D, 0x4CEF61 | `Scroll` 0x4CEDE0 | scroll (self-blit src/dst rect heights of world surface `E+0x48CC`) | rect fed to `0x5E6440` at 0x4CEF87 [V] |
| 0x4CF715, 0x4CF723 | zoom-out animation 0x4CF610 | world render size | `fld [esi+0x472C]` x2, `fsub` x2 [V] |
| 0x4D01CD, 0x4D01DD, 0x4D0229 | zoom-in animation 0x4CFCE0 (part 0x4D003E) | world render size | same pattern [V] |
| 0x575A11, 0x575A58 | camera clamp 0x5758D0 | camera clamp / zoom reset | `(vpH-80)/zoom` vs mapH [V] |
| 0x4456FA | 0x4456D7 (character code, switch on distance class) | **unrelated** (distance threshold 80) | `fld [0x677598]` in a jump-table of range constants [V] |
| 0x44C1BF, 0x44C1D0, 0x44C1D6 | 0x44BFFD (character movement) | **unrelated** | `while (len > 80) len -= 80` loop [V] — **would hang if the constant became 0** |
| 0x5B2FB9 | 0x5B2F30 (zoom-dependent parameter interpolation, called by 0x5A8650) | **unrelated** | `(1-z)*80 + c` [V] |

### B. `Vec2(x,-80)` sites (24)

| Address | Function | Class / use [V unless noted] |
|---|---|---|
| 0x4BEDFD | engine init 0x4BEA58 | view centre `cam + (cfgW,cfgH-80)/2/zoom` -> `E+0x45E0->0x5A8650` (listener/"view centre" object [I]) |
| 0x4C16F2 | level load 0x4C11E3.. | stored to a local that is only destroyed (0x4C19AC) — dead [V] |
| 0x4C37C7 | 0x4C3740 (called from main loop 0x50F855) | view centre -> `E+0x45E0->0x5A8650` |
| 0x4C748E | 0x4C7385 (tick) | world view rect `cam .. cam+(vp-80)/zoom` -> 0x4D23D0 (visible-area logic [I]) |
| 0x4C87A1 | `Update` state 3 (zoom start) | zoom camera math |
| 0x4C8994 | `Update` zoom-out start | zoom camera math / clamp |
| 0x4C8DD9 | `Update` zoom-in start | zoom camera math |
| 0x4C92E7 | `Update` state 1 after the world blit | view centre -> `0x5A8650` |
| 0x4C94E7 | 0x4C94C0 (called 0x4C940E, state 1) | world view rect for overlay drawing [I] |
| 0x4C9B67, 0x4C9C38 | `RenderBackground` 0x4C9B30 | world render size: src rect `(vp-80)/zoom`, dst rect `(0,0)-(vp-80)` |
| 0x4CEF8C | `Scroll` 0x4CEDE0 | scroll strips (view size) |
| 0x4CF851, 0x4CF92C, 0x4CFA5B | zoom-out animation 0x4CF610 | world render size |
| 0x4CFD7E, 0x4CFDED, 0x4CFFF4, 0x4D00CC | zoom-in animation 0x4CFCE0 | world render size |
| 0x4D0A8E | object render 0x4D0A10 | world render size (view rect for sprites) |
| 0x4D910E | 0x4D90B0 (select character; callers HUD/hotkeys/scripts) | visibility test: if char not in `cam..cam+(vp-80)/zoom` -> centre camera |
| 0x4DA4AF, 0x4DA570, 0x4DA6A2 | screenshot routine 0x4DA394.. | world render size (screenshot) |

### C. HUD layout in `RHGame` (do NOT change)

| Address | Function | Evidence |
|---|---|---|
| 0x50A6DD (`push 80.0f`), 0x50A717 (`add eax,-0x50`), 0x50A835 (`sub edx,0x50`) | 0x50A70F.. (resolution set-up, writes `G+4/+6`) | `G+0x1970->vtbl[0x2C](Rect(0,H-80, W,H))` (0x50A791) and the same rect for `G+0x1A10` (0x50A87E) [V] |
| 0x50C008, 0x50C03E, 0x50C079, 0x50C215 | 0x50BD33 (HUD creation) | creates `G+0x1970` / `G+0x1A10` (`vtbl[4]` at 0x50C0C4 / 0x50C279) [V] |
| 0x50CEE9, 0x50CF2A, 0x50D08A, 0x50D0CB, 0x50D236, 0x50D277 | 0x50CD91, 0x50D02D, 0x50D11F | other HUD windows anchored at H-80 (e.g. `G+0x2118`) [V for the arithmetic, I for meaning] |

Unrelated 80.0f immediates [V]: 0x44BF9C/0x44C2F6/0x44C370 (vector scale, char code), 0x45018B/0x45019F (AI table),
0x4813D7/0x48148E, 0x4C0BC0 + 0x4C185B (loading progress 80 %, `[0x6C0A58]`), 0x52C8DD/0x5370CC/0x540C5E/0x540FA2 (menu rects),
0x54B291 [I menu]. Other `[E+0x472C]` reads use the **full** height: 0x4C8D94/0x4C907B (zoom temp surfaces `E+0x4788/478C`
created vpW x vpH), 0x4DB9FE/0x4DBD58 (`|vel| > vpH*zoom` -> full redraw), 0x4DEF64 (`0x4DEEB0` WorldToScreen +
"on screen" test, y < vpH). [V]

## 2. Input path

1. Main loop 0x50F9AF..: `hit = GUI(G+0x68)::ProcessInput 0x6192A0(state G+0x1960, ..., flag)` — thiscall, `ret 0xC`,
   returns the head of the hit-widget chain (0 = cursor over no GUI window) (0x50FAC3 -> `esi`). [V]
2. Mouse move: `Event(type 1, sub 1, pos)` (ctor 0x549120) is posted to the event manager `[0x6C0C4C]` (0x5460C0,
   thiscall, `ret 4`) **unconditionally** (0x50FB7C). Buttons: `Event(1, 3, pos, buttons)` (ctor 0x5491A0) is posted only
   `if (buttons != 0 && hit == 0)` (0x50FBB1..0x50FC08). [V]
3. 0x5460C0 stores the cursor in `mgr+0x84`, then broadcasts to every listener (`call [vtbl+0]`, 0x546C4E). [V]
4. Engine listener `E+0x60` (vtbl 0x678B34) -> `OnEvent 0x4DC2C0` (thiscall on `E+0x60`, `ret 4`). Type 1 is handled at
   0x4DC964: gated only by `E+0x6C` (world input enabled); `ScreenToWorld 0x4DEE70` (reads `mgr+0x84`, no bounds), hover
   `0x4DEBF0`, button handlers 0x4DE290/0x4DE5E0/0x4DDFB0 (down), 0x4DE1E0/0x4D9330/0x4DE840 (up). **No coordinate test.** [V]
5. GUI hit test: for each visible top-level window `vtbl[0xC](state)`. `G+0x1970` and `G+0x1A10` are `0x619FA0` containers
   (vtbl 0x67AD9C) registered as top-level windows (0x50D3B4, 0x50D3BD; `G+0x1970` holds `G+0x1AB0` and the zoom-button
   group `G+0x1B7C`, 0x50D399/0x50D3A7). Container hit test `0x61A6E0` (thiscall, `ret 4`): if visible (`+0x5C`) and
   enabled (`+0x5D`) and `Rect(+0x70).Contains(pos)` (0x608400, flag `+0x80` = rect valid) **the container itself is a hit**
   (`+0x90`), then children `vtbl[0x18]` are appended. Both rects are `(0,H-80)-(W,H)`. [V]

**So the strip is excluded only because the two full-width HUD containers consume every button event there, not by a
coordinate test.** Hover still reaches the world (with y > view bottom). [V]
Edge-scrolling: not found. Scroll actions 1-4 come from keyboard (0x517900, 0x518159) and a joystick POV (0x518C6F,
angles 13500/22500/27000/31500); no code compares the cursor with `vpH-80`. [V for what was searched, I that no edge scroll exists]

## 3. Surfaces and blits

* World surface `E+0x48CC` = full viewport (created 0x4BEDF8 / 0x4C00C7). [V] Zoom temp surfaces full viewport. [V]
* World -> back buffer (Update state 1, 0x4C92AC): `0x5E6440(src=E+0x48CC, srcRect=0, dst=0, dstRect=0, 0)`; handle 0
  is replaced by 1 (default/back buffer [I]) and NULL rects = whole surface [I]. **Already full height; no blit rect to
  change.** The bottom 80 rows are copied today too; they are just never rendered into. Every other rect is computed from
  the sites in section 1, so zeroing them makes all rects full height consistently. [V for the sites]

## 4. Patch plan

**P1 (world size, 42 instructions):**
* 18 x `D8 25 98 75 67 00` -> `D8 25 88 74 67 00` at 0x4C0414, 0x4C046F, 0x4C7D2C, 0x4C808C, 0x4C80B5, 0x4CED05, 0x4CED62,
  0x4CEEFB, 0x4CEF21, 0x4CEF3D, 0x4CEF61, 0x4CF715, 0x4CF723, 0x4D01CD, 0x4D01DD, 0x4D0229, 0x575A11, 0x575A58
  (bytes +2..+5 only).
* 24 x `68 00 00 A0 C2` -> `68 00 00 00 00` at 0x4BEDFD, 0x4C16F2, 0x4C37C7, 0x4C748E, 0x4C87A1, 0x4C8994, 0x4C8DD9,
  0x4C92E7, 0x4C94E7, 0x4C9B67, 0x4C9C38, 0x4CEF8C, 0x4CF851, 0x4CF92C, 0x4CFA5B, 0x4CFD7E, 0x4CFDED, 0x4CFFF4, 0x4D00CC,
  0x4D0A8E, 0x4D910E, 0x4DA4AF, 0x4DA570, 0x4DA6A2 (bytes +3..+4).
* Apply all or none (scroll strips, render rects and clamps must agree). Do **not** write 0.0 into `0x677598`
  (0x44C1D0 loop would never terminate) and do not touch 0x4456FA, 0x44C1BF/D0/D6, 0x5B2FB9 or section C.

**P2 (clicks in the strip):** make the two HUD containers not count as hits themselves, keeping children hits.
Hook `0x61A6E0` (`int __fastcall(win, edx, state*)`): if `win == G+0x1970 || win == G+0x1A10`, save byte `win+0x80`, set it
to 0, call original, restore. With the rect "invalid", 0x608400 returns 0, the self-hit is skipped, the child loop still
runs (0x61A727 -> 0x61A72F). [V for the control flow] First bytes `83 EC 08 56 8B F1 8A 46 5C 84 C0` (no relative
operands in the first 11 bytes; 5-byte detour safe). `G` = `[0x6C0B3C]`.
Alternative if decorative corner art must keep blocking clicks: in the hook, clear the flag only when
`x >= 480 && x < W-320` (art widths from the user, [I]).

## 5. Risks / not determined

* Corner art: whether the left/right pieces are child widgets with their own hit test (so clicks on them stay in the
  HUD after P2) was not determined. [I]
* Whether a container/HUD draw fills the strip with an opaque colour was not checked; the user's observation (black
  middle, world never drawn) suggests not. [I]
* P1 makes the view 80 px taller in world units: `CanZoomOut`/0x5758D0 refuse zoom-out (or reset zoom) on maps a bit
  more often; small maps reach "view larger than map" 80 px earlier (bands, see ZOOM_CAMERA.md §4-5). [V for the formulas]
* The bottom 80 world rows are now under the HUD art: units there are hidden behind the corners; `0x4D90B0` will
  consider a character "visible" when it is behind the HUD and not re-centre the camera. [V for the test, I for the UX]
* Saved camera positions: `Serialize` restores `E+0x4738`, then 0x5758D0 re-clamps with the new height (0x4C5E8C). [V]
* Edge scrolling and the hover cursor shape over HUD windows were not traced further. Pathfinding with clicks below
  the old view bottom is unaffected (those points are now inside the view). [I]
