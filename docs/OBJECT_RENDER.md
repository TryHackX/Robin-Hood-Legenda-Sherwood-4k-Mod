# Dynamic objects: lists, positions, rendering (static RE, Game.exe)

Everything below was read from the disassembly unless marked *(inferred)*. `E` = `RHEngine*` (`[0x6C0FA8]`, same value as `[0x6C0A88]`; both are stored from `ebp` in the engine constructor at 0x4BED0F and 0x4BF092). `S` = `RHSprite*` = `[elem+0x50]`. `P` = `S+4`, the embedded `RHPositionInterface` (the sprite constructor 0x5B5890 calls the PI constructor 0x55EEE0 with `ecx=esi+4` at 0x5B58B7).

## 1. What 0x4D0A10 iterates

`0x4D0A10` is `__thiscall(E, int dstSurface)`, `ret 4` (0x4D162B). It runs in this order:

1. It builds the view rect `[esp+0x58] = Rect(cam, cam + (viewport + (0,-80)) / zoom)` (0x4D0A8E–0x4D0B3B; `cam = E+0x4738`, `viewport = E+0x4728`, `zoom = E+0x475C`).
2. It draws the ground FX list `E+0x49B8` (begin `+0x49BC`, end `+0x49C0`) at 0x4D0C7A–0x4D0CAD.
3. It draws the vision cone: `[E+0xD4]->vtbl[+0xA4]` at 0x4D0CEF, or the debug sight 0x4D7F40.
4. It draws the selection markers 0x581EE0 (0x4D10AF) for each entry of the selection container `E+0x49F8`.
5. **0x4D1D00 BuildDrawList** (call at 0x4D113B):
   - It `std::sort`s the master list `E+0x49D8` (begin `+0x49DC`, end `+0x49E0`) in place: `0x4E6690(first, last, 0x4D1800, 0)` at 0x4D1D33.
   - It clears the draw list `E+0x48F8` (0x4D1D51).
   - It refills the draw list from the sorted master list and inserts the "attached" FX list `E+0x4998` after the elements matched by `0x4AA980` (0x4D1E14–0x4D1EEE).
6. **Main loop** over the draw list (begin `E+0x48FC`, end `E+0x4900`) at 0x4D116D–0x4D126B. The element type is `RHElement*`.
   - For humans (`(elem+4)&7==4`) it first flushes the decor layer `E+0x4668` up to this element's sort key: `0x5C73E0(.., key=[S+0x1C4])` at 0x4D11A7.
   - It then calls the draw method `vtbl[+0x24]` (0x4D121D, or 0x4D1203 with flag `0x80000000` for a selected PC).
   - It passes the returned event list to `0x4D16B0(E, events, &view, 1.0f)` (positional sounds).
7. It flushes the rest of the decor layer (key 1e6) at 0x4D1289, then draws the per-PC X-ray silhouette `0x4913F0` (0x4D12CC).

**Sort key.** The key is a float at `S+0x1C4`. The comparator `0x462A30` (called through 0x4D1800) orders by key ascending, with ties broken by `elem+0xC` (a unique id taken from the counter `[0x6C081C]`, 0x462063). The key is written only by `SetSortKey 0x5BD560(S, relTo, before)`:
- With `relTo=0` it sets `key = [S+0x14]`, i.e. the world y (0x5BD568).
- Otherwise it sets the parent's key ± 0.001 (0x5BD57F / 0x5BD597).

The sort runs every frame, but the keys are computed in the logic.

**Draw method.** `vtbl[+0x24]` is `__thiscall(elem, int dst, float zoom, Rect* view, uint flags)`, `ret 0x10` (0x486F2D). It returns an event pointer or 0.

**Example chain down to a blit (NPC):**

`RenderFrame 0x5105D0` → `0x4C8380` (@0x4C86CC) → `0x4D0A10` → `[vtbl+0x24]` = `0x486A80` (@0x4D121D). Inside 0x486A80:
1. `0x5B9B60` cull test (@0x486BA9).
2. `0x5BBA00` decodes the current frame into the element surface `elem+0x54` (@0x486CD9; it locks the surface with `0x5E5A00` @0x5BBA50).
3. `0x5BA130` computes the source and screen rects (@0x486CED).
4. **`0x5EC5F0`** does the software blit (@0x486D38): `__thiscall(drawMgr=[elem+0x60], srcSurf, srcRect*, dst, dstRect*, w, w, 1)`, `ret 0x1C` (0x5ECBC1). The alternative is `0x5E6440` (@0x486D5D).

## 2. Position storage and the projection to screen

| Field | Type | Meaning |
|---|---|---|
| `P+0x04` (`S+0x08`) | u32 | Validity bits: 1 = 3D valid, 2 = projected valid, 4 = origin valid; 7 = all (0x55F536) |
| `P+0x0C/10/14` (`S+0x10/14/18`) | float | **World x, y, z (height)** |
| `P+0x18/1C` (`S+0x1C/20`) | float | Draw origin = `floor(projected - hotspot)`; integer-valued (0x55F343–0x55F390, floor = 0x6433AB with CW 0x173F) |
| `P+0x20/24` (`S+0x24/28`) | float | Projected = `(x, y - z)` (0x55F2C4–0x55F2D7) |
| `P+0x28..30`, `P+0x3C` | vec3, vec2 | Previous 3D / projected position; copied only by the actor commit (0x464249–0x46426C) |
| `P+0x80` (`S+0x84`) | vec2 | Hotspot *(inferred)* |
| `P+0x94` / `P+0xC0` | Rect | Local bbox / world bbox = local + projected (0x55F2F0–0x55F312) |
| `P+0xEC` | ptr | Ground plane: `z = a*x + b*y + c` (0x55F412–0x55F434) |

`PI::Update 0x55F290` (`__thiscall(P)`, plain `ret`) derives all the other fields from whichever representation is valid and sets the flags to 7.

**How the renderer reads the position:**
- `0x5B9B60` calls `0x55F6D0` when flag bit 2 is clear (0x5B9BDB). It then computes the world rect `S+0x1A0 = [origin + frameOffset, + frameSize]` (0x5B9BEF–0x5B9C71) and intersects it with the view (0x5B9CA5).
- `0x5BA130` recomputes the same top-left (0x5BA1CB). The size is taken from `S+0x19A/0x19C`, which `0x5BBA00` writes at 0x5BBACF/0x5BBADB.
- The result is clipped to the view (0x5BA271), then `screen = clipped - view.min` (0x5BA5F5/0x5BA60E).
- At zoom 0.5 the coordinates are halved (0x5BA338 onward); at zoom 2.0 they are doubled (0x5BA552 onward).

So `screenY = floor(y - z - hotY) + frameOffY - camY` (at zoom 1). The height z enters only through the projection.

## 3. Class hierarchy (no RTTI; the names come from the serializers' strings)

There is one hierarchy, rooted at `RHElement` (constructor 0x462010; `+4` = type, `+0xC` = id, `+0x50` = new `RHSprite`). The draw method is always slot `+0x24`.

| Class | vtable | +0x24 draw | Type `[elem+4]` |
|---|---|---|---|
| RHElement | 0x6776A0 | purecall | |
| RHElementActor → Human | 0x677710 → 0x6779D0 | purecall | `&3 == 0` |
| NPC / Civilian / Soldier | 0x677BF8 / 0x677824 / 0x677FF4 | 0x486A80 | 4 / 4 / 0x14 (0x470060, 0x4A1C22) |
| PC | 0x677DFC | 0x491570 | 0xC (0x48F2A6) |
| FX / FXMasked / Target | 0x6784A0 / 0x678508 / 0x6789A4 | 0x4A8300 / 0x4AACB0 / 0x4A8300 | 1 / 5 / 9 |
| Mobile | 0x678570 | 0x4AC850 (debug paths only) | 2 (0x4AB0AF); children vector `+0xB0` |
| Object family (arrow, projectile, purse, scroll, wasp…) | 0x678184–0x678AA0 | 0x4B7040, 0x4B2B60, 0x4A6CB0, 0x4BA580… | 3 / 7 / 0xB |

Non-element sprites drawn in the same pass: the decor layer `E+0x4668` (0x5C73E0 → 0x5C3FC0, which uses a shared sprite `[ebp+4]`) and the selection markers `E+0x4608`.

## 4. Position writers

The movement requests are:
- a pending 3D position at `elem+0x74` with flag byte `elem+0x70` (set, for example, at 0x496DCA);
- a pending projected position at `elem+0x80` with flag byte `elem+0x71` (set at 0x4985D2 and 0x498830).

They are applied by **`Actor::vtbl[+0x28]` = 0x464230**, which:
- saves the previous position;
- writes `P` (0x4642BE–0x4642CF), or calls `0x464530` for the projected path;
- calls `PI::Update` (0x4642E2) and `SetSortKey(S, 0, 1)` (0x4642ED).

Derived `Live` methods reach it: NPC 0x48A980, PC 0x4936F0, Soldier 0x4A5890. Non-actors (`Mobile::Live 0x4AB600`, projectile 0x4A6510) reach `0x5BD560` and `0x55F290` directly.

`Live` (`vtbl[+0x28]`) is called only from the `RHEngine::Tick 0x4C6EF0` loop over the update list `E+0x48D8` (0x4C75FA). That tick is called from `RHGame::Run` at 0x50FEE6, **before** `RenderFrame` at 0x50FF05.

**Static call graph from `RenderFrame`** (direct calls, CRT excluded): it does **not** reach 0x464230, 0x464530, 0x55F870 or 0x5BD560. It does reach `0x55F290` and `0x55F6D0`, which only derive fields.

Render-time exceptions, all temporary and restored:
- PC ghost: it overrides `P+0x20`, sets flags = 2 (0x491946–0x4919F7), then restores it and calls `0x55F870` (0x491B48/0x491B55).
- Object/projectile shadow: it writes the 3D position, sets flags = 1 and calls `0x55F290` (0x4B722A–0x4B7243), then restores (0x4B737B–0x4B73A2).
- The NPC draw clears `elem+0x70/71` when `elem+0x12 == 0` (0x486AE9).

## 5. Position-dependent caches

| Cache | When it is computed | Stale if only `P+0xC..14` is overwritten? |
|---|---|---|
| Origin `P+0x18`, projected `P+0x20`, bbox `P+0xC0` | Logic (`PI::Update`); render only if a flag bit is clear | **Yes**, unless flags = 1 and `0x55F290` is called |
| World rect `S+0x1A0` | Every draw (0x5B9C71) | No (it is recomputed from the origin) |
| Sort key `S+0x1C4` | Logic (0x5BD560) | Yes: the draw order stays at the tick position (an order swap up to 1 tick late) |
| Scenery masking | Render, `0x5BBA00` → 0x4EE120 (uses 3D `S+0x10`) / 0x4EDE00 (uses `S+0x24`) | No |
| Selection marker | Render, 0x581EE0 from `S+0x24` (0x4D107B) | No |
| Vision cone | Render, slot `+0xA4` (0x487B90); eye = `S+0x10` + posture height (0x478C20) | No *(inferred: the cone is not cached)* |
| Object shadow | Render (0x4B71AF) | No |
| PC X-ray silhouette | Render (0x4913F0) | No |
| Sound event position | Render (0x45ABF0 copies `S+0x24`) | No |
| Health bar | Not found in the world pass | — |

## 6. Enumeration

The containers are MSVC6 `vector<RHElement*>` blocks of 0x20 bytes: `+4` begin, `+8` end, `+0xC` capacity, and a dirty byte at `+0x14`.

| Offset | Contents |
|---|---|
| `E+0x49D8` | Master list of all sorted drawables (types 0, 2, 3, and FX with z ≠ 0) |
| `E+0x49B8` | Ground FX (type & 0xF == 1 and z == 0; 0x4D84B7–0x4D84E6) |
| `E+0x4998` | Attached FX (`0x610F30(elem+0xD4) != 0`) |
| `E+0x48D8` | Update ("Live") list: everything that can move by itself |
| `E+0x4918` / `+0x4938` / `+0x4958` / `+0x4978` | Actors / PCs / NPCs / Mobiles |

The lists are mutated only in the logic (AddElement 0x4D8460, Remove 0x4D88E0, called from the tick at 0x4C7611), plus the in-place sort and the draw-list rebuild in the render. They are stable between ticks on the main thread.

To tell moving objects from static ones: membership in `E+0x48D8`, or simply whether the position changed between snapshots. `elem+0x12 == 0` means the element is inactive or hidden.

## 7. Other per-tick state

These are all discrete (do not interpolate them):
- animation `S+0x184` (id), `S+0x186` (frame), `S+0x188` (sub-tick), advanced by 0x5B7820 from the logic;
- facing `P+0xD4` (0..15; turning code at 0x55F0F0–0x55F28F);
- posture `S+0x8C`.

**Render-side counters that already speed up with the extra frames:**
- Decor entries of type 1, 2 and 0xD step their animation on every `RenderFrame` (0x5C6763 → 0x5B7F60; jump table 0x5C724C).
- The PC ghost counter `elem+0x580` is decremented on every draw (0x491B5A).

Sound events are latched (`S+0x18A`, 0x5BB5F0), so they are not repeated.

## Recommended way to enumerate objects and override positions

```c
uint8_t* E = *(uint8_t**)0x6C0FA8;
static const int L[] = {0x49B8, 0x4998, 0x49D8};          // all lists that 0x4D0A10 draws
for (int k = 0; k < 3; k++) {
  uint8_t** it  = *(uint8_t***)(E + L[k] + 4);
  uint8_t** end = *(uint8_t***)(E + L[k] + 8);
  for (; it && it < end; ++it) {
    uint8_t* el = *it;                    if (!el) continue;
    uint32_t id = *(uint32_t*)(el + 0x0C);
    uint8_t* S  = *(uint8_t**)(el + 0x50); if (!S) continue;
    uint8_t* P  = S + 4;
    float* xyz  = (float*)(P + 0x0C);     // world x, y, z
  }
}
```

1. **Snapshot.** After `RHEngine::Tick` returns (0x50FEEB) and before the `RenderFrame` at 0x50FF05, set `prev = cur` and `cur = xyz`, keyed by `(el, id)`.
2. **Override, before every `RenderFrame`.** Do this for the tick's own frame too, with alpha ≈ 0; otherwise each tick shows `cur` and then jumps back to `prev`.
   - Save `P+0x04` (4 bytes), `P+0x0C..0x27` (0x1C bytes), `P+0xC0..0xD3` and `S+0x1A0..0x1B3`.
   - Write `xyz = prev + (cur - prev) * alpha`.
   - Write `*(uint32_t*)(P+4) = 1`.
   - Call `((void(__thiscall*)(void*))0x55F290)(P)`. The game itself does exactly this at 0x4B722A–0x4B7243.
3. **Restore** the saved bytes after `RenderFrame`.
4. **Skip** an element if it has no previous snapshot, if its id differs, or if `|cur - prev|` is large (a teleport).

Leave `S+0x1C4` (the sort key) unchanged at first: the sort runs in place on the master list, so changing the keys changes the list order that the next tick sees.

## Open questions / risks

- The general zoom formula `(world - cam) * zoom` is *(inferred)*; only the 0.5 and 2.0 branches are coded explicitly.
- Whether the logic reads `S+0x1A0` or `P+0xC0` (for example, mouse picking) is not verified, so restore both.
- The vision cone geometry is assumed to be recomputed at draw time (0x58E7F0), not checked.
- The exact semantics of `E+0x4998` and `0x4AA980` (draw order of the attached FX) are *(inferred)*.
- Possible writers through input handlers or scripts outside the tick were not exhaustively proven absent, only shown to be unreachable from `RenderFrame`.
- If the camera follows a moving character, it must use the same alpha.
- The decor animation and the PC ghost counter run at the `RenderFrame` rate, independently of positions.
