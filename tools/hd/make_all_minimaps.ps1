# All HD pictures of the opened minimap, from scratch (1.2.5): run from anywhere, needs the game's DATA\Levels, the HD
# backgrounds' previews (hd\maps_png, hd\maps_hd\dat2) and the models (hd\models). See docs\HD_MODE.md (minimap).
#   1. the game's map pictures (DATA\Levels\*\*.min) as parts of the mod      -> hd\work\menu_parts\part_<key>.raw
#   2. "classic upscaled": the game's picture 2x, model dat2                  -> hd\menu_parts\<key>.hdp
#   3. the scroll for the new pictures: the game's picture 4x, model dat2     -> hd\work\minimap_x4\<key>.hdp
#   4. the Sherwood camp's own picture kept (MinimapCamp=0), 4x, model dat2   -> hd\minimap\classic\<key>.hdp
#   5. the new pictures from the levels' HD backgrounds                       -> hd\minimap\clean|parchment\<key>.hdp
# dat2 instead of the interface's sgi: the maps are paintings - sgi turned foliage into worm-like strokes.
$ErrorActionPreference = 'Stop'
$hd = $PSScriptRoot
$root = Split-Path (Split-Path $hd -Parent) -Parent
Push-Location $hd
try {
    $list = python -I minimap_pictures.py --write
    $list | Select-Object -Last 2
    $keys = ($list | Select-String -Pattern 'key ([0-9a-f]{16})' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique) -join ','
    $camp = @('26c1a218aa02cd8f', '5b3ced48c047e7f4')       # DATA\Levels\Day|Night\SHERWOOD.MIN
    python upscale_menu_parts.py --only $keys --model dat2 --redo
    python upscale_menu_parts.py --only $keys --model dat2 --redo --scale 4 --out (Join-Path $root 'hd\work\minimap_x4')
    $classic = Join-Path $root 'hd\minimap\classic'
    New-Item -ItemType Directory -Force $classic | Out-Null
    foreach ($k in $camp) { Copy-Item (Join-Path $root "hd\work\minimap_x4\$k.hdp") $classic -Force }
    python -I make_minimaps.py
} finally {
    Pop-Location
}
