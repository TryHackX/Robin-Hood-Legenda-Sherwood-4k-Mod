# Sprite bank: `RobinHood.dic` + `RobinHood.bks`

[V] = verified by disassembly and by decoding the real files (`hd\work\` copies). [I] = inferred.
Code: `RHframeholder.cpp` (bank) and `RHSpriteScriptor.cpp` (.rhs), per the assert strings.

## Files
- **`.bks`** (592,261,466 B): raw frame data concatenated, no header. Frame 0 starts at offset 0 and the last
  frame ends exactly at EOF. [V]
- **`.dic`** (9,699,680 B): the whole directory. It is read once at start-up by the function containing 0x504040
  (reads at 0x504329–0x5044E4). [V]

```
u32  stamp            = 256969 (0x3EBC9) -> bank+0xB4. Every .rhs starts with the same value; the game
                        asserts "RHS file %s\%s was not generated with the current bank" (0x5BF97F) [V]
u16  nPal             = 134
nPal x { u16 nCodes; nCodes x 4 x u16 RGB565 }    read by 0x5051A0 -> 0x5069D0 [V]
                        (116 palettes have 4096 codes; 0x07E0 is rewritten to the key 0x07C0 on load;
                         converted to 555 if the screen is 15-bit; deduplicated by CRC (0x6183C0))
u32  nFrames          = 404,855
nFrames x 14 bytes { u16 w; u16 h; u32 offset; u32 size; u16 palIdx }   [V]
```
The dic is consumed exactly to EOF (checked by the extractor).

## In-memory frame table (`[bank+0xC]`, 20 B/entry) [V]
`+0 w, +2 h, +4 size, +8 data ptr (0 = not loaded), +0xC file offset, +0x10 u8 flag (always 1 at load,
0x504452), +0x12 palette index (0xFFFF = none)`. When the .bks is not mapped in memory (`bank+0xE8 = 1`), +0xC is
`base + (offset & ~1)` with base 0, i.e. a plain file offset.

**Lazy loader 0x5058A0** [V]: `malloc(size & ~1)`; file mode: seek(`bank+0x5C`, +0xC) (0x5F5370), read `size`
bytes (0x5F52B0). Memory mode: memcpy from +0xC. **No compression** besides the per-frame encodings below.

## Two frame encodings (chosen by palIdx, wrapper 0x504CD0) [V]
1. **palIdx = 0xFFFF: 16-bit row RLE** (10,134 frames). Per row: `u16 first, u16 last`, then `last-first+1`
   RGB565 pixels; pixels outside [first,last] are the key 0x07C0. Empty row: `last = 0xFFFF` (`first` is 0xFFFF
   or 0 in the data). All 10,134 frames decode with every word consumed. Decoders (same row format):
   0x5E3730 plain copy (flag +0x10 = 0), 0x5E3919 replaces 0x001F with a passed colour (flag = 1, the normal
   case), 0x5E37C0 / 0x5E3869 used by the masked wrappers 0x504EB0 / 0x504F70.
2. **palIdx != 0xFFFF: 4x1 vector quantisation** (394,721 frames, `size = h*ceil(w/4)*2`, w is always a multiple
   of 4). Each row is `w/4` u16 codes; code -> 4 consecutive RGB565 pixels from `pal[palIdx][code]`. Blitter
   0x5050A0 (args: src, dst, pitch-2w, w, h, palIdx, set). `set` 0/1/2 selects palette vectors at bank+0x1C /
   +0x2C / +0x3C (only +0x1C is filled from the dic; the others are probably recoloured copies) [I].
   All codes are in range for their palette (checked over the whole file).

Colours are **16-bit RGB565 only**; the "palettes" are VQ codebooks, not 8-bit palettes.
- **0x07C0** = colour key (transparent) in both encodings.
- **0x001F** = marker that the decoder replaces with a caller colour. In the decoded images it covers the
  **ground shadow** under characters and a 1-px fringe on edges [V by looking]. 4,206 of the RLE frames contain
  it, and so did every character VQ frame sampled. A pixel that already equals the replacement colour is bumped by +1 (0x5E3980).
- 98 frames are 4x1 placeholders of 2 bytes (frames 0, 100, 101, 272, ...). In the .rhs files they appear as the
  single frame of an "empty" animation (e.g. chariot02 anim 0).

Layout: frames are grouped in contiguous runs by palette (149 runs); 31 alternating VQ/RLE runs. Characters are VQ
(one 4096-code palette per character family, e.g. Robin = palette 110), objects/effects are mostly RLE.

## Sprites and animations: `DATA\**\*.rhs` (233 files) [I layout, V parses all files to EOF]
```
u32 stamp; u16 nProfiles
nProfiles x { char name[32]; u16 nAnims; u16 a, b; u32 canvasW, canvasH;
  nAnims x { u16 nFrames; u16 u1 (= nFrames-1 or 5..); u32 w, h; u16 u2;
    nFrames x 14 B { u32 bankFrame; u16 flags; s16 v; u16 x; u16 y; u16 extra } } }
```
1,076 profiles, 948,586 frame references, 404,807 distinct bank frames (48 never referenced). A bank frame is
referenced many times (Robin: 14,544 refs, 7,584 distinct frames). `extra` is non-zero on some frames (event /
sound id?). `a,b` look like the hotspot inside the canvas (Robin 74,98 in 150x150) and `x,y` like the per-frame
draw offset, but this is not confirmed [I]. At run time an anim struct is 100 B: `+0x18` u32 frame-id array (read
by 0x5BDBC0), `+0x48` 8-byte per-frame offset array added to the draw origin (0x5B9C01) [V]. So **hotspots /
offsets live in the .rhs, not in the bank**. 0x5BBA9B picks frame `idx + (flags & 1)` for one draw mode [V].

## Notes for 2x replacement
- Size comes from the frame table (`0x5046A0/0x5046C0` -> `S+0x19A/0x19C`); offsets from the .rhs anim data.
  A 2x frame needs both doubled, or a renderer hook that scales at blit time.
- Keep 0x07C0 transparent and 0x001F as the shadow marker (an upscaler will smear both; rebuild them from masks).
- VQ frames can be replaced by RLE16 frames (palIdx = 0xFFFF) so no codebook is needed.

## Tools
- `tools\hd\extract_sprites.py` — `--info`, `--frame N --out f.png [--keep-marker]`,
  `--range A B --outdir d [--rle-only|--vq-only]`. RGBA PNG; marker pixels go to `<name>_marker.png` (magenta).
- `tools\hd\rhs_parse.py` — checks all .rhs; `FILE [--dump]`; `--index hd\work\frame_index.csv` (one row per ref).
- `tools\hd\contact_sheet.py out.png scale in...` — preview sheet.
