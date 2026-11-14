# AI upscaling models for Robin Hood: The Legend of Sherwood (2x, 1280x720 -> 2560x1440)

Research date: 2026-11-14. Web-only research; nothing was downloaded or installed.
Content: pre-rendered 3D backgrounds, ~40 px sprites with hard alpha edges, RGB565 source (banding/dither, no JPEG).

Verification legend: **[API]** = file name/size/URL read from the GitHub Releases API (exact). **[ODB]** = URL taken from the model's openmodeldb.info page. **[HF]** = Hugging Face file listing. "Not verified" = from the author's/known convention, not seen this session.

## 0. Loader notes (spandrel)

- Install: `pip install spandrel` (PyPI, latest seen **0.4.2**, 2026-02-21, MIT, Python >=3.10; deps torch, torchvision, safetensors, numpy, einops). Import name `spandrel`. Source: https://pypi.org/pypi/spandrel/json , https://github.com/chaiNNer-org/spandrel
- `spandrel_extra_arches` (PyPI 0.2.0, 2024-09) is a **separate** package holding architectures with restrictive/non-commercial licenses (SRFormer, FeMaSR, CodeFormer, MAT, Restormer, M3SNet, MPRNet, MIRNet2, DDColor). Usage: `import spandrel_extra_arches; spandrel_extra_arches.install()`. **None of the candidates below need it**: ESRGAN/RRDB, Compact/SRVGG, SPAN, MoSR, DAT, HAT, RGT, RealPLKSR are all in the main package. The repo is MIT, but each architecture's code keeps its original license (the `__arch/` folders). Source: spandrel README.
- spandrel does no tiling itself; write your own tile loop (overlap 16-32 px, blend) for big models.

## 1. Caveats specific to this game

- **4x models used for 2x**: run at 4x, then downscale to 1/2 (Lanczos or area, in linear-ish float, not 8-bit round-trips). This acts as supersampling and usually looks cleaner than a native 2x model, but costs 4x the memory/time of a native 2x pass. Alternatively use native 2x models (group B, plus 2xAoMR_mosr).
- **Game textures/photos models "hallucinate" less when degradation-free**: models trained "on non-degraded input" (Nomos2, AoMR) are the faithful choice; "otf/real/web" models add denoise/deblur and can smooth fine texture (grass, thatch). Pre-rendered art has already clean edges, so prefer clean-input models, and run a 1x dedither first only if banding is visible.
- **Sprites with hard alpha**: do not feed RGBA. Bleed (dilate) edge colors into the transparent area, upscale RGB, upscale alpha separately (bicubic + threshold/soft clamp), then re-multiply. Otherwise dark/halo fringes appear.
- **RGB565**: expand to 8 bit with bit replication ((v<<3)|(v>>2)) before any model; quantisation noise is regular, so a 1x dedither/deband pass helps ESRGAN-type models that otherwise enlarge the dither pattern.
- **VRAM on a 12 GB card**: from general experience, not cited from a source: DAT2 / HAT-L need tiling; start at 192-256 px tiles with fp16/bf16 autocast (HAT-L ~165 MB weights, DAT2 ~140 MB) and reduce on OOM; ESRGAN-64nf23nb (64 MB) and SPAN/Compact/MoSR can run much bigger tiles or even a whole 1280x720 frame at 2x/4x in fp16. Window-attention models (DAT/HAT) show tile-seam artifacts if overlap is too small; use >=32 px overlap and test it on a flat gradient.
- **Licensing**: Phhofm releases are CC-BY-4.0 (some release bodies contain a typo "CC-BY-0.4"; openmodeldb says CC-BY-4.0). UltraSharp and Remacri are **CC-BY-NC-SA-4.0** (non-commercial). 4x SGI/FSDedither are **GPL-3.0**. Real-ESRGAN weights: BSD-3-Clause per repo (not re-verified this session).
- openmodeldb tag pages ("Pre-Rendered") could not be listed through the fetch tool (only the first ~12 of 672 shown), so the pre-rendered group was found via search; only the SGI model is explicitly about 90s pre-rendered game CGI. There is no 2x model explicitly trained on Commandos/BG style backgrounds that I could find.

## 2. Group A - best quality (offline)

| # | Model | Arch | Scale | Trained for | License | Format | Size | Direct URL |
|---|---|---|---|---|---|---|---|---|
| A1 | **4xNomos2_hq_dat2** (Phhofm/Helaman) | DAT2 | 4x | Clean (non-degraded) photographic input, quality-first, nomosv2 dataset 6k images, 140k iter | CC-BY-4.0 | .pth | 140.2 MB | https://github.com/Phhofm/models/releases/download/4xNomos2_hq_dat2/4xNomos2_hq_dat2.pth [API] |
| A2 | **4xNomos2_hq_mosr** | MoSR | 4x | Same as above, non-degraded input; small and fast | CC-BY-4.0 (typo "0.4" in release) | .pth | 17.2 MB | https://github.com/Phhofm/models/releases/download/4xNomos2_hq_mosr/4xNomos2_hq_mosr.pth [API] |
| A3 | **2xAoMR_mosr** | MoSR | **2x native** | Game textures (Age of Mythology Retold), 13.8k images, 510k iter, from Nomos2 pretrain; tags Game Textures, Restoration | CC-BY-4.0 (typo "0.4" in release) | .pth (+ .onnx) | 17.1 MB | https://github.com/Phhofm/models/releases/download/2xAoMR_mosr/2xAoMR_mosr.pth [API] |
| A4 | **4xNomos8kSCHAT-L** (also -S) | HAT-L | 4x | Photos, on-the-fly JPEG+blur, Nomos8k_sfw | CC-BY-4.0 | .pth and .safetensors | L 165.8 MB / S 40.6 MB | https://github.com/Phhofm/models/releases/download/4xNomos8kSCHAT/4xNomos8kSCHAT-L.safetensors (L, 165.3 MB); -S: .../4xNomos8kSCHAT-S.safetensors (40.3 MB) [API] |
| A5 | **4xTextures_GTAV_rgt-s_dither** | RGT-S | 4x | Game textures (GTA V), JPEG to q80, **trained with dithering** (explicit dither robustness) | release says "CC-BY-0.4" (typo; author's other releases CC-BY-4.0) | .pth and .safetensors | 136.0 MB | https://github.com/Phhofm/models/releases/download/4xTextures_GTAV_rgt-s_dither/4xTextures_GTAV_rgt-s_dither.safetensors [API] |
| A6 | **4xTextureDAT2_otf** | DAT2 | 4x | Game textures, Real-ESRGAN OTF degradations (JPEG, noise, slight blur), GTAV 30k images | CC-BY-4.0 | .pth/.safetensors | 140.3 MB | https://github.com/Phhofm/models/releases/download/4xTextureDAT2_otf/4xTextureDAT2_otf.safetensors [API] |
| A7 | **4x SGI** (ChrisNonyminus) | ESRGAN 64nf23nb | 4x | **Upscaling + dedithering pre-rendered sprites/images/textures of 90s-early-2000s games** (vintage CGI, SGI renders); tags CGI, Dedither | GPL-3.0-only | .pth | 63.9 MB | Only link on openmodeldb: https://drive.google.com/uc?export=download&confirm=1&id=1kNSz5f2_krdlsJ700DwH78OEPYkkcjW5 (Google Drive; no GitHub/HF primary found) |
| A8 | **4xNomos8kSC** | ESRGAN 64nf23nb | 4x | Photos, OTF JPEG+blur | CC-BY-4.0 | .pth | 63.9 MB | https://github.com/Phhofm/models/raw/main/4xNomos8kSC/4xNomos8kSC.pth [ODB] |
| A9 | **4xNomos8kDAT** | DAT | 4x | Photos, finetune of official DAT x4, OTF JPEG/blur | CC-BY-4.0 | .pth | 295.0 MB | Only Google Drive on openmodeldb: https://drive.google.com/uc?export=download&confirm=1&id=1JRwXYeuMBIsyeNfsTfeSs7gsHqCZD7xn . No GitHub/HF primary found. Substitute A1/A4. |
| A10 | **4x-UltraSharp** (Kim2091) | ESRGAN 64nf23nb | 4x | General, JPEG-heavy; adds a lot of detail/sharpness (not faithful, tends to over-sharpen) | CC-BY-NC-SA-4.0 | .pth and .safetensors | 67.0 / 66.9 MB | https://huggingface.co/Kim2091/UltraSharp/resolve/main/4x-UltraSharp.safetensors [HF] |
| A11 | **4x_foolhardy_Remacri** | ESRGAN 64nf23nb | 4x | BSRGAN-like with more detail, less smoothing (interpolation of Siax/Superscale etc.) | CC-BY-NC-SA-4.0 | .pth | 63.9 MB | https://huggingface.co/FacehugmanIII/4x_foolhardy_Remacri/resolve/main/4x_foolhardy_Remacri.pth [ODB] (third-party HF user, not author; author link is not primary) |
| A12 | **RealESRGAN_x2plus** | RRDBNet (23 blocks) | **2x native** | Real-world degradations (blur/noise/JPEG) | BSD-3-Clause (per repo, not re-verified) | .pth | 67.1 MB | https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.1/RealESRGAN_x2plus.pth [API] |
| A13 | **RealESRGAN_x4plus** | RRDBNet | 4x | General real-world photos | BSD-3-Clause | .pth | 67.0 MB | https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth [API] |
| A14 | 4xRealWebPhoto_v4_dat2 | DAT2 | 4x | Web photos (blur, noise, JPEG/WebP) - denoises/smooths, **not faithful** for clean art | CC-BY-4.0 | .pth/.safetensors | 140.3 / 139.8 MB | https://github.com/Phhofm/models/releases/download/4xRealWebPhoto_v4_dat2/4xRealWebPhoto_v4_dat2.safetensors [API] |
| A15 | 4xBHI_dat2_real | DAT2 | 4x | Web/realistic, noise+blur+WebP/JPG; finetune of Nomos2 dat2 | CC-BY-4.0 | .pth/.safetensors | 140.2 / 139.8 MB | https://github.com/Phhofm/models/releases/download/4xBHI_dat2_real/4xBHI_dat2_real.safetensors [API] |

Others noted but not evaluated further: 4x GameAI 2.0 (ESRGAN, WTFPL, PS2 compression + realistic/cartoon textures; Google Drive only, https://openmodeldb.info/models/4x-GameAI-2-0), 4x FSDedither (ESRGAN, GPL-3.0, dedither for realistic images; Google Drive only), ATD-based Phhofm models (4xRealWebPhoto_v3_atd) - not fetched.

Judgement on faithfulness (from the model descriptions, not tested): A1/A2/A3 are the only ones explicitly trained for clean input without added denoise/blur, so they should hallucinate least. A7 is the only one trained on pre-rendered game CGI. A10/A11 add detail aggressively and are NC-licensed: use for comparison only. Real-ESRGAN family tends to over-smooth to "painted" look on clean art.

## 3. Group B - fast 2x (real time candidates)

| # | Model | Arch | Scale | Trained for | License | Format | Size | Direct URL |
|---|---|---|---|---|---|---|---|---|
| B1 | **2xNomosUni_compact_multijpg** | Compact (SRVGG) | 2x | Universal fast; JPEG q40+ and multiscale downsample degradations | CC-BY-4.0 | .pth, .safetensors, .onnx fp16/fp32, ncnn | 1.22 MB (.pth) | https://github.com/Phhofm/models/releases/download/2xNomosUni_compact_multijpg/2xNomosUni_compact_multijpg.safetensors [API] |
| B2 | **2xNomosUni_span_multijpg** | SPAN | 2x | Same data, SPAN | CC-BY-4.0 | .pth, .safetensors, .onnx | 4.5 MB | https://github.com/Phhofm/models/releases/download/2xNomosUni_span_multijpg/2xNomosUni_span_multijpg.safetensors [API] |
| B3 | **2xBHI_small_span_fast_pretrain** | span_fast (32 ch, no bias; spandrel loads it as SPAN variant: verify) | 2x | L1+MS-SSIM only pretrain (no GAN: soft but faithful, no hallucination); ~2x faster than span on RTX2060 TensorRT per author | CC-BY-4.0 | .pth | 3.4 MB | https://github.com/Phhofm/models/releases/download/2xBHI_small_span_fast_pretrain/2xBHI_small_span_fast_pretrain.pth [API] |
| B4 | 2xBHI_small_span_pretrain | SPAN | 2x | Same, standard SPAN | CC-BY-4.0 | .pth | 8.9 MB | https://github.com/Phhofm/models/releases/download/2xBHI_small_span_pretrain/2xBHI_small_span_pretrain.pth [API] |
| B5 | 2xAoMR_mosr (see A3) | MoSR | 2x | Game textures; 17 MB, light compared with DAT/HAT, probably real-time-ish at 720p on a 12 GB card with fp16 (not measured) | CC-BY-4.0 | .pth | 17.1 MB | see A3 |
| B6 | 2x StarSample V2.0 Lite (.derpy.) | SPAN 48nf | 2x | Cartoon (MLP) - style mismatch, listed for completeness | CC-BY-NC-SA-4.0 | .safetensors | 8.9 MB | https://github.com/Derpiesaurus/models/releases/download/v2.0_Lite/2x_StarSample_V2.0_Lite.safetensors [API] |
| B7 | realesr-general-x4v3 | SRVGGNetCompact | 4x (downscale to 2x) | General real-world, small; has a denoise-strength knob via model interpolation in the original repo | BSD-3-Clause | .pth | 4.9 MB | https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-x4v3.pth [API] |

Fastest good ones: B1 (Compact, 1.2 MB) and B3 (span_fast). The quality ranking in the author's own claims is Compact < SPAN < MoSR for detail; pretrain (L1-only) variants are the most faithful but soft. Note: no RealPLKSR model suited to this content was found (90s Sonic LG is RealPLKSR but cartoon-specific).

## 4. Group C - 1x restoration (de-dither / de-band)

All three found 1x models are ESRGAN 64nf23nb (~64 MB) and trained on anime/cartoon images; none was trained on realistic art, so test carefully (they may flatten fine texture). No primary-source (GitHub/HF-by-author) link exists on openmodeldb; they are on Google Drive.

| # | Model | Arch | Scale | Trained for | License | Format | Size | URL |
|---|---|---|---|---|---|---|---|---|
| C1 | **1x DitherDeleterV3 Smooth** (Mutin Choler) | ESRGAN | 1x | Reverses 32-bit Riemersma dithering; 550-ish anime images, box+point downscale; tags Dedither | WTFPL | .pth | 63.6 MB (ODB) / 66.7 MB (HF) | Google Drive (ODB): https://drive.google.com/uc?export=download&confirm=1&id=1C6RTSFkBnidfBxBI6HjasgUEIgAzG3l3 . Community HF mirror (not author, no license stated, pickle-flagged): https://huggingface.co/sazoji/DitherDeleterV3-Smooth/resolve/main/1x_DitherDeleterV3-Smooth-115000_G.pth |
| C2 | **1x Bandage Smooth** (Mutin Choler) | ESRGAN | 1x | Repairs colour banding (64-bit banding applied to anime training set); tag Debanding | WTFPL | .pth | 63.6 MB | Google Drive (ODB): https://drive.google.com/uc?export=download&confirm=1&id=1Y-dQcOjUnftaAuhTtUoHJktpgRJWg2yQ |
| C3 | 1x DEPVR (CF2lter) | ESRGAN | 1x | PVRTC compression artefacts in game textures (not dither) | CC-BY-NC-SA-4.0 | .pth | 63.6 MB | Mega only: https://mega.nz/folder/O58BHDLL#q9clb4tfbh4_Jd2M6lA6ZA |
| C4 | 1x Spongebob De-Quantize, 1x ITF SkinDiffDDS v1 | ESRGAN | 1x | Colour quantization/dither of cartoon textures / DDS skin textures | CC-BY-NC-4.0 | .pth | n/a | not fetched; style mismatch |

Alternative to model-based C: the 4x models with dither robustness (A5, A7) already dedither inside the upscale pass. Also try a non-neural deband (shader gradient dither / bilateral on smooth regions) first; RGB565 banding is limited to 32/64 levels per channel and often needs only a mild blur on low-gradient areas.

## 5. Recommended shortlist (test in this order)

| Pri | Model | Group | Arch / scale | Size | URL |
|---|---|---|---|---|---|
| 1 | 4xNomos2_hq_dat2 (then downscale 0.5) | A | DAT2 4x | 140.2 MB | https://github.com/Phhofm/models/releases/download/4xNomos2_hq_dat2/4xNomos2_hq_dat2.pth |
| 2 | 2xAoMR_mosr | A/B | MoSR 2x native | 17.1 MB | https://github.com/Phhofm/models/releases/download/2xAoMR_mosr/2xAoMR_mosr.pth |
| 3 | 4x SGI (pre-rendered 90s game CGI + dedither; GPL-3.0) | A | ESRGAN 4x | 63.9 MB | https://drive.google.com/uc?export=download&confirm=1&id=1kNSz5f2_krdlsJ700DwH78OEPYkkcjW5 (Google Drive only) |
| 4 | 4xTextures_GTAV_rgt-s_dither | A | RGT-S 4x | 136.0 MB | https://github.com/Phhofm/models/releases/download/4xTextures_GTAV_rgt-s_dither/4xTextures_GTAV_rgt-s_dither.safetensors |
| 5 | 2xNomosUni_span_multijpg | B | SPAN 2x | 4.5 MB | https://github.com/Phhofm/models/releases/download/2xNomosUni_span_multijpg/2xNomosUni_span_multijpg.safetensors |
| 6 | 2xNomosUni_compact_multijpg (or 2xBHI_small_span_fast_pretrain, 3.4 MB) | B | Compact 2x | 1.2 MB | https://github.com/Phhofm/models/releases/download/2xNomosUni_compact_multijpg/2xNomosUni_compact_multijpg.safetensors |
| 7 | 1x DitherDeleterV3 Smooth (run only if banding/dither shows; compare with C2 Bandage Smooth) | C | ESRGAN 1x | 63.6 MB | https://drive.google.com/uc?export=download&confirm=1&id=1C6RTSFkBnidfBxBI6HjasgUEIgAzG3l3 (Google Drive; HF mirror listed in section 4) |

Baselines to add for comparison only (NC-licensed, not for shipping): 4x-UltraSharp (https://huggingface.co/Kim2091/UltraSharp/resolve/main/4x-UltraSharp.safetensors), RealESRGAN_x2plus (https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.1/RealESRGAN_x2plus.pth).

## 6. Pages used

- https://github.com/chaiNNer-org/spandrel (architectures, extra arches, licensing)
- https://pypi.org/pypi/spandrel/json , https://pypi.org/pypi/spandrel_extra_arches/json
- https://github.com/Phhofm/models (model index) and GitHub Releases API: https://api.github.com/repos/Phhofm/models/releases/tags/<tag> for 2xAoMR_mosr, 4xNomos2_hq_dat2, 4xNomos2_hq_mosr, 4xNomos8kSCHAT, 4xTextures_GTAV_rgt-s_dither, 4xTextureDAT2_otf, 4xRealWebPhoto_v4_dat2, 4xBHI_dat2_real, 2xNomosUni_span_multijpg, 2xNomosUni_compact_multijpg, 2xBHI_small_span_pretrain, 2xBHI_small_span_fast_pretrain
- https://api.github.com/repos/xinntao/Real-ESRGAN/releases and /releases/tags/v0.2.1
- https://api.github.com/repos/Derpiesaurus/models/releases/tags/v2.0_Lite
- openmodeldb.info model pages: /models/2x-AoMR-mosr, 4x-Nomos8kSC, 4x-Nomos8kSCHAT-L, 4x-Nomos8kDAT, 4x-Nomos2-hq-dat2, 4x-UltraSharp, 4x-Remacri, 4x-SGI, 4x-FSDedither, 4x-GameAI-2-0, 1x-DitherDeleterV3-Smooth, 1x-Bandage-Smooth, 1x-DEPVR, 2x-NomosUni-span-multijpg, 2x-NomosUni-compact-multijpg, 2x-StarSample-V2-Lite, 2x-90s-Sonic, /tags
- https://huggingface.co/Kim2091/UltraSharp/tree/main , https://huggingface.co/sazoji/DitherDeleterV3-Smooth/tree/main , https://huggingface.co/martin-rizzo/ESRGAN-4x
- Search results (openmodeldb dedither/pre-rendered queries), incl. https://openmodeldb.info/models/1x-Spongebob-De-Quantize
