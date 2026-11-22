# Tryb HD — jak działa i jak go budować (stan 2026-11-22, wersja 1.3.0)

Legenda: [Z] = zweryfikowane pomiarem/testem w tle, [K] = z kodu gry (dezasemblacja), [N] = niesprawdzone.

## 1. Pomysł
Gra przy rozdzielczości wewnętrznej 2560×1440 i **własnym powiększeniu 2,0** pokazuje ten sam kadr co 1280×720, ale
obraz składa natywnie: interfejs, napisy, kursor i stożki widzenia są ostre, a powiększane ×2 są tylko bitmapy
(tło mapy i sprite'y). Każde takie powiększenie jest miejscem podmiany na grafikę HD. Nie ma skalowania całej klatki.

Ustawienia (`SherwoodFix.ini`, sekcja `[Display]`): `Width=2560 Height=1440 HDView=1 FullHeightView=1 HDBackgrounds=1
HDVariant=<folder> HDKeyToggle/Next/Prev (kody VK dziesiętnie) HDWater=1 SmoothSprites=1 HDSprites=1 HDInBetween=1
HDKeyBetween=120 (F9)`.

## 2. Z czego gra składa klatkę w powiększeniu 2,0 [Z: ślad rysowania `tools\hctl.ps1 -Cmd "trace N nazwa"`]
Kolejność w jednej klatce (`RHGame::RenderFrame`):
1. `Blt` 1:1 powierzchni świata (2560×1440, `E+0x48CC`) na bufor tylny. Powierzchnia świata jest wypełniana
   rozciąganiem ×2 z **powierzchni mapy** (`E+0x48C4`, cały poziom, RGB565) — przy pełnym odrysowaniu (stan kamery 5)
   i paskami po przewinięciu. → **podmiana na tło HD** (`DDM::HD::TryBackgroundBlit`).
2. Animacje tła i obiekty „na ziemi" (`0x4D0A10` → `0x4A8670`): `Blt` z kluczem ×2 z małej powierzchni, do której gra
   tuż przedtem zdekodowała klatkę banku. Tu są m.in. **animacje wody** i **dachy budynków** (zdejmowane, gdy postać
   wchodzi do środka), duże drzewa.
3. Wielokąty z barwieniem (`0x5EEC00`, jedyne wywołanie `0x461D98`): czyta piksele wprost z powierzchni mapy i miesza
   z kolorem; w 2,0 procedura `0x62469C` zapisuje bloki 2×2. Hak czyta zamiast tego 4 piksele z obrazu HD.
4. Postacie i obiekty z cieniem (`0x5EC5F0` → dla skali 2,0 `0x62261C` z kluczem `0x07C0` / `0x6220EC` bez):
   `(src, dst, szerDst, wysDst, pitchSrc, pitchDst, współczynnik, znacznik)`, cdecl. Piksel równy znacznikowi (cień,
   `0x001F`) przyciemnia cel (`dst*wsp/256`), inne są kopiowane; oryginał czytał JEDEN piksel celu na blok 2×2 —
   na tle HD każdy cień robił z ziemi klocki. Hak przyciemnia 4 piksele osobno; z `SmoothSprites=1` filtruje (bilinear
   z pokryciem z klucza → miękka krawędź sylwetki).
5. HUD i napisy: `Blt` 1:1 z kluczem; kursor: zapis tła 55×55, rysunek 21×37.
6. `Flip` → prezentacja D3D11.
- Stałe skali: `[0x6774F4] = 2.0`, `[0x6775D8] = 0.5`; maski kanałów procedur MMX: `0x6C11C8` = `1F00 1F80 1F00 1F00`.
- Stan kamery `E+0x48C0`: 1 spoczynek, 2 przewijanie, 3/4 animacja zoomu, 5 pełne odrysowanie (silnik sam ustawia 5 po
  wczytaniu). Wymuszenie odświeżenia tła po zmianie wariantu = zapis 5, gdy stan to 1 lub 2. [Z]

## 3. Tła HD
- `tools\hd\extract_maps.py` → `hd\maps_png\<mapa>_<wariant>.png` (20 plików; format `.map`: `u16 w, u16 h, u32 2,
  u32 rozmiar` + bzip2 surowego RGB565).
- `tools\hd\upscale_variants.py [--variants …] [--only …] [--redo] [--png] [--colorfix σ]` →
  `hd\maps\<wariant>\<klucz>.hdmap`. Klucz = FNV-1a 64 po co 7. pikselu oryginału (to samo liczy DLL po powierzchni mapy).
  `.hdmap`: `"HDMP"`, u32 wersja=1, u32 szer, u32 wys, u32 0, potem RGB565 (z ditheringiem uporządkowanym).
- `tools\hd\hdmap_to_png.py [--variants … | all] [--only …] [--redo]` → `hd\maps_hd\<wariant>\<mapa>.png`: podglądy PNG
  robione WPROST z plików `.hdmap` (te same piksele; bez modelu i karty graficznej; 20 map = 680 MB w ~50 s). Folder
  `hd\maps_hd` służy tylko do oglądania — gra go nie czyta. Po każdym ponownym generowaniu map odświeżyć podglądy
  (`--redo`): użytkownik ocenia grafikę także po plikach w `hd\` (2026-11-14 leżała tam jako jedyny plik stara próba
  sprzed korekty kolorów — wyglądała na „jedyną mapę HD" i to ją porównywał z oryginałem; teraz w `hd\maps_hd\_stare\`).
  Pomiar podglądu nottingham_day: średnie RGB 113,0/86,7/50,3, nasycenie 60,2 % (oryginał 112,7/86,5/50,0; 60,1 %;
  stary plik 110,7/87,0/52,4; 56,4 %). Opis folderów dla użytkownika: `hd\CZYTAJ.txt`.
- Warianty (folder → model w `hd\models`): `aomr` 2xAoMR_mosr (0,7 Mpx/s), `sgi` 4x_SGI (0,28), `ultrasharp`
  4x-UltraSharp (0,27, licencja niekomercyjna), `dat2` 4xNomos2_hq_dat2 (0,04; całość ~45 min). Modele 4× są
  zmniejszane do 2× filtrem Lanczos. Półprecyzja tylko gdy model ją deklaruje i wynik próbny się zgadza.
- **Korekta kolorów** (`color_fix`, domyślnie σ=3 px oryginału): `HD += powiększ(rozmyj(oryginał − zmniejsz(HD)))`.
  Pomiar na nottingham_day przed korektą (średnie RGB; nasycenie): oryginał 112,7/86,5/50,0; 60,1 % — dat2
  110,2/86,4/51,8; 57,7 % — sgi 112,2/87,0/50,2; 59,7 % — ultrasharp 110,9/86,6/49,5; 60,2 % — aomr 110,3/83,7/44,5;
  64,8 %. Po korekcie (croisement01): różnica średnich ≤0,15, bloki 32 px: 0,1–0,2. [Z]
- DLL (`src\ddm\ddm_hd.cpp`): warianty = podfoldery `hd\maps`; pliki mapowane w pamięć (najwyżej 3 naraz); powierzchnia
  mapy identyfikowana numerem seryjnym; gra blokuje ją co klatkę do odczytu, więc zawartość jest sprawdzana haszem
  najwyżej co 0,5 s, a po `Blt` do niej — od razu. Zmiana zawartości w trakcie gry → ostrzeżenie w logu i powrót do
  oryginalnych pikseli dla tego stanu (nie zaobserwowano). [Z]

## 4. Woda i dym — animacje „na ziemi” [Z]
Staw w Nottingham = animacja 8 klatek 272×178 (klatki banku 4579–4586, paleta 16), nieprzezroczysta, każda klatka to
cały staw; dane są kodowane blokami 4×1, więc powiększone dają mozaikę. Dym z kominów w Lincoln (klatki 4242–4250
52×106 i 4251–4259 52×111) to tak samo pełne prostokąty: skała i dach + cienki dym, w niskiej jakości. Rozpoznanie
(`DrawGroundAnimation`, raz na miejsce): duży sprite (≥4000 px, ≥3000 nieprzezroczystych), którego piksele różnią się
od mapy pod spodem średnio o <25/255 — „zamalowuje ziemię”. Pomiar: woda 8,0–13,7, dym 12,8–18,2, reszta 45–82.
- **Nakładka** (pokrywa ≥90 % prostokąta — dym; woda ma 39–67 %): różnica klatka − mapa pod spodem, wygładzona 3×3;
  od jej wielkości (największy kanał, 0..255) zależy krycie: 0 do 8 (szum kompresji, mediana 9,5), pełne od 36 (dym:
  percentyle 90/99 = 30/75). Kolor = wygładzona klatka. Mieszane na obraz HD (A·klatka + (1−A)·HD), więc skała HD
  zostaje ostra, a dym jest miękką smugą. (Samo dodanie różnicy do HD było za słabe: dym ginął na szczegółach skały.)
  Wcześniej dym albo był uznany za wodę i pomijany (zgłoszenie 2026-11-15: „po Alt+Tab widać dymy, a u nas nie”),
  albo rysowany wprost — rozmyta łata wokół komina.
- **Woda** (2026-11-15, prośba użytkownika „rób wodę”, „remake dla realizmu”): w klatkach gry nie ma fal, tylko szum
  ±3/255 zmieniający się z klatką (`tools\hd\water_motion_study.py`) — z nich nic nie da się wziąć. `HDWater=2`
  (domyślnie) / `3` (mocniej): woda z obrazu HD poruszana falami. Cztery pociągi fal (sinusoidy, długość 17–64 px HD,
  kierunki co ~90°, pionowa długość fali o połowę krótsza — rzut izometryczny), okresowe w pętli 4 s (24 klatki,
  przenikane), przesuwają **gładką część** obrazu (Gauss σ 1,2 px HD) o 1–3 px, a najdrobniejsza faktura zostaje
  w miejscu — ostrość 99 % (przy przesuwaniu całego obrazu: dwuliniowo 37–52 %, Catmull-Rom 52–77 %, Lanczos 69–83 %
  energii laplasjanu przy przesunięciu 0,5/0,25 px). Do tego jasność ±7 % (±9 %) z nachyleniem fal do światła z lewej
  góry i słaby odblask. Woda = piksele, gdzie sprite gry jest nieprzezroczysty; efekt narasta przez 7 px od brzegu
  (trzciny w stawie lekko się kołyszą — odróżnienie ich kolorem dawało poszarpane krawędzie). Pętlę liczy osobny wątek
  (niski priorytet; ~0,12 s na 22 tys. pikseli, staw Nottingham ~0,6 s i 5,4 MB), do tego czasu i przez 0,6 s
  przenikania widać nieruchomą wodę HD; rysowanie = przenikanie dwóch klatek SSE2 tylko po pikselach wody (tych samych,
  które zamalowałby sprite gry — postacie rysowane później zasłaniają ją normalnie). Pamięć: najwyżej 40 MB pętli,
  najdawniej rysowane są zwalniane i liczone od nowa przy powrocie; zmiana wariantu HD liczy pętlę od nowa.
  Prototyp z tą samą arytmetyką: `tools\hd\water_anim_proto.py` (presety `waves`, `waves_strong`; podglądy i GIF
  w `hd\compare\water_anim\`, porównanie: `porownanie_woda.gif`). Test w tle (Lincoln nie ma animacji wody):
  `dbg 32` = dym traktowany jak woda — poza prostokątem obraz = tło HD co do piksela (0,00), na brzegu 0,4/255, wewnątrz
  średnio 7,2/255 jak w prototypie. `HDWater=1` — nieruchoma woda z obrazu HD (wybór z 2026-11-14), `0` — klatki gry.
- Płynność: różnica liczona przy zmianie klatki animacji (hasz pikseli), powiększana 2× raz; między klatkami obrazu
  przenikanie poprzedniej i bieżącej przez czas trwania klatki (średnia z pomiarów) — dym nie skacze co klatkę
  animacji (gra zmienia ją kilka razy na sekundę, obraz ma 144). Opóźnienie o jedną klatkę animacji jest niewidoczne.
- Koszt: złożenie SSE2 w zakresach wierszy, gdzie coś jest (dwa kominy: rozciąganie 2,1 ms na tik zamiast 5,6 ms
  w zwykłym kodzie). Studium z podglądami: `tools\hd\ground_anim_study.py` → `hd\compare\ground_anim\`.
- Nie sprawdzone w grze na prawdziwym stawie: w teście w tle dostępna jest tylko pierwsza misja (Lincoln) — wygląd
  oceniony na prototypie z danymi stawu z Nottingham, mechanika w grze przez `dbg 32`.

## 5. Sprite'y HD (wdrożone 2026-11-14, `src\ddm\ddm_hd_sprites.cpp`, `HDSprites=1`)
- **Skąd wiadomo, co jest na powierzchni** [Z]: hak na dekoder banku `0x504CD0` zapisuje „wskaźnik pikseli powierzchni →
  numer klatki, tryb barwienia, rozmiar klatki". Dekodery z maską `0x504EB0/0x504F70` kasują wpis (powierzchnia ma
  wtedy tylko część klatki). Numery klatek ≥ 404 855 to klatki tworzone w locie (małe ikony) — bez wersji HD.
- **Zasłanianie** [K+Z]: po dekodowaniu obiekt sprite'a (`0x5BBA00`, gałąź `[ebp+0x10]==1`) wycina z powierzchni to, co
  zasłaniają mury/drzewa — procedury `0x5BBF20(rect*, blokada*, poz*)` `ret 0xC` i `0x5BC3C0(dane*, blokada*, poz*,
  word)` `ret 0x10` wpisują klucz w zasłonięte piksele. Haki oznaczają wpis jako „wycięty"; klatka HD jest wtedy
  rysowana tylko tam, gdzie piksel powierzchni (hx/2, hy/2) nie jest kluczem.
- **Cień** [Z]: w banku znacznik `0x001F`; gra zamienia go przy dekodowaniu na własny kolor (w dzień `0x2964`) i ten
  kolor podaje procedurze `0x62261C` jako „znacznik" (współczynnik przyciemnienia 102/256). W klatkach HD cień zostaje
  `0x001F`; przy rysowaniu przez procedurę gry przyciemnia cel, przy rozciąganiu DirectDraw dostaje kolor z powierzchni.
- **Kontrola zgodności**: po każdym dekodowaniu, przed pierwszym użyciem, porównanie powierzchni z klatką HD w siatce
  ~12×12 punktów: kształt (przezroczystość) i **kolory** (średnia różnica na kanał ≤ 36/255; w teście na Nottingham
  największa wśród przyjętych klatek: 9,5). Niezgodność → rysowanie po staremu (raz w logu na klatkę). To jest też
  bezpiecznik dla wszystkiego, czego nie znam (inne palety, efekty): zły kolor nigdy nie trafi na ekran.
- **Noc i mgła** [K; sprawdzone w grze 2026-11-15, v1.1.6 — patrz §5f]: tryb barwienia 1/2 z dekodera. Mod barwi klatkę HD tym samym wzorem co
  `0x5046E0`: `kanał = kanał*p/100 + kolor*(100-p)/100` (zieleń liczona na 5 bitach, klucz i kolor cienia bez zmian),
  noc p=50 z kolorem `0x00E7` (= `0x5EF320(0,0x1E,0x3C)`), mgła p=60 z bielą. Dla klatek blokowych gra używa
  gotowych zestawów palet 1/2 (`bank+0x2C/+0x3C`), budowanych tymi samymi parametrami: `0x506930(paleta, wzór, tryb)`
  → `0x504830(paleta, 50, kolor(0,0x1E,0x3C))` albo `(60, 0xFFFF)` [K]. Znacznik cienia w paletach wszystkich trzech
  zestawów jest zamieniany na kolor cienia poziomu przez `0x5055B0` → `0x504B80(paleta, kolor(0xF))` [K]. Gdyby
  zaokrąglenia się różniły, kontrola kolorów odrzuci klatki i będą rysowane po staremu.
- **Magazyny**: `hd\sprites\<wariant>\` (nazwa jak wariant tła) i `hd\sprites\_common\`; `sprites.idx` (16 B na klatkę
  banku: offset u64, rozmiar u32, szer u16, wys u16) + `sprites.dat` (klatka = surowe RGB565 albo wiersze
  „u16 pierwszy, u16 liczba, piksele"). Odczyt `ReadFile` przy pierwszym użyciu, pamięć podręczna 128 MB (LRU).
- **Podział** (arkusz `hd\compare\postacie_sprites.png`): klatki bezstratne (dachy, drzewa, obiekty; 10 134 szt.) —
  każdym modelem wariantu; klatki blokowe (postacie, animacje; 394 721 szt.) — tylko SGI do `_common`, bo DAT2 i AoMR
  zamieniają szum kompresji w fakturę, a HAT jest ~10× wolniejszy.
- Narzędzia: `tools\hd\upscale_sprites.py` (`--kind rle|vq`, `--store`, `--palettes-of plik`, `--ids-file plik`,
  `--sheet`, `--shard I/N`, `--batch-pixels`, `--skip-store`), `tools\hd\merge_sprite_stores.py` (scalanie magazynów
  zrobionych równolegle), `tools\hd\sprite_model_test.py` (arkusz porównawczy modeli), `tools\hd\variant_sheet.py`
  (warianty tła obok siebie + zgodność kolorów), polecenie testowe `frames NAZWA` (które klatki są teraz na
  powierzchniach sprite'ów).
- Czasy na karcie 12 GB (2026-11-14): klatki bezstratne (63 Mpx) — sgi/ultrasharp ~8 min, aomr ~12 min, dat2 ~65 min
  (większość przy zajętej karcie); klatki blokowe (1 051 Mpx) modelem sgi — 4 procesy równolegle, ~80–90 min.
  Przy małych klatkach wąskim gardłem jest procesor (stąd `--shard`). **Modele z uwagą (DAT, HAT) muszą mieć małe
  partie** (20 000 px): przy 280 000 px DAT2 zapchał 12 GB karty i przestał się posuwać.
- **Nie uruchamiać generatora podczas gry**: zajęta karta wydłuża `Present` (pomiar: przy zapchanej pamięci karty gra
  spadła z 25 do ~8 tików/s; po zwolnieniu wróciła do 25).
- **Nie zrobione / nie sprawdzone** (stan z 2026-11-14; noc i mgła, interfejs i animacja wody są już zrobione, patrz §4, §5f, §7): sprite'y rysowane lustrzanie przez
  DirectDraw (idą po staremu), pozostałe lokacje (test wtedy tylko na Nottingham w dzień).

### Ikony i znaczniki — klatki tworzone przez grę w locie (2026-11-14) [Z]
Zgłoszenie użytkownika: „ikonka nad głową Robina jest pixeloza, gdy ma kaptur". Przyczyna: portret w kółku nad wybranym
bohaterem, „Z" nad śpiącym, gwiazdki, znaki zapytania, ikony przedmiotów, pierścień pod postacią itd. NIE są w pliku
banku — gra buduje je przy wczytywaniu obrazków interfejsu (`0x00505CE0` dokłada zestaw obrazków na koniec wektora klatek
`bank+8`: `_First` = `bank+0xC`, `_Last` = `bank+0x10`, 20 B na wpis; `0x00505980` przycina obrazek do zawartości i koduje
wierszami RLE16). Numery ≥ 404 855 zależą od kolejności wczytywania, więc taka klatka jest rozpoznawana **po treści**:
FNV-1a 64 po „u16 szer, u16 wys" + dane wierszy (`ContentKey` w `ddm_hd_sprites.cpp`).
- Magazyn: `hd\sprites\_icons\icons.idx` („HDIC", wersja 1, liczba, 0; wpisy 24 B: klucz u64, offset u64, rozmiar u32,
  szer u16, wys u16) + `icons.dat` (klatki jak w `sprites.dat`). Wspólny dla wszystkich wariantów.
- Narzędzie: `tools\hd\upscale_icons.py --model ultrasharp [--sheet …]` (porównanie modeli: `--compare sgi ultrasharp dat2
  aomr`; arkusze `hd\compare\ikony_oryginalne.png`, `ikony_modele.png`, `ikony_przed_po.png`, `ikony_w_grze_przed_po.png`).
  Wybrany **UltraSharp**: na portretach 21×28 i ikonach najczystszy; SGI podobny, DAT2 i AoMR dodają szum. Czysta czerń
  z czarnym otoczeniem zostaje czernią (`keep_black`) — pierścienie i efekty świetlne są mieszane wg jasności.
- Skąd dane: polecenie testowe `rtdump NAZWA` zapisuje wszystkie klatki „w locie" z pamięci gry (Nottingham: 439 klatek,
  382 różne); **oraz sam mod podczas zwykłej gry**: klatka „w locie" bez wersji HD jest raz dopisywana do
  `hd\work\runtime_frames\play_<data>_<czas>.bin` — po kolejnym uruchomieniu `upscale_icons.py` ikony z misji, których
  nikt nie zrzucał ręcznie, też są w HD. (Ikony nieznane rysowane są jak dotąd — wygładzone ×2.)
- **Pierścień zaznaczenia i efekty świetlne** idą inną drogą: ogólny blitter gry `0x005E6900` (thiscall, 6 arg.:
  `srcId, srcRect*, dstId, dstRect*, src2Id, flagi`; tryb = flagi | `[this+0x70]`: `0x20000` 16 bitów, `0x8000`
  rozciąganie, `0x100/0x200/0x400` rodzaj mieszania, `1` klucz) → dla `0x28201` procedura `0x005E8B10(src, dst, alphaSrc,
  klucz, szerDst, wysDst, pitchSrc, pitchDst, skalaX, skalaY)`, `ret 0x28`: alfa = min(255, 5 górnych bitów zieleni
  drugiego źródła × 16), `wynik = src·α/256 + dst·(256−α)/256` na kanał. Hak rysuje klatkę HD tym samym wzorem na każdy
  piksel HD (oryginał mieszał jeden piksel celu na blok 2×2). Inne tryby rozciągane (`0x28100/0x28101/0x28200/0x284xx`:
  `0x5E92F0`, `0x5E8F20`, `0x5E8740`, `0x5E84E0`) nie pojawiły się w teście — do obserwacji w śladzie (`GBLT …`).
- **Raport braków** (polecenie testowe `miss NAZWA [TIKI]`): lista wszystkiego, co w tym czasie zostało powiększone ×2
  BEZ klatki HD (numer klatki, droga rysowania, powód). Nottingham po zmianach: pusty przy ~10 000 rysowań HD na 50 tików.

### Notatki z rozpoznania (stan sprzed wdrożenia)
- Bank: `docs\SPRITE_BANK.md`, narzędzia `tools\hd\extract_sprites.py`, `rhs_parse.py`. 404 855 klatek, 1 114 Mpx:
  bezstratne RLE 10 134 szt. / 63 Mpx (obiekty, dachy, efekty), VQ 4×1 394 721 szt. / 1 051 Mpx (postacie; z tego
  879 Mpx to klatki <8000 px).
- Dekoder klatek [K]: `0x504CD0` `__thiscall(bank, blokada*, nrKlatki, kolor/zestaw)`, `ret 0xC`; blokada: `+4` szer,
  `+6` wys (u16), `+8` pitch, `+0x10` wskaźnik pikseli. RLE: `0x5E3919` (zamienia znacznik `0x001F` na podany kolor)
  albo `0x5E3730`; VQ: `0x5050A0(src, dst, pitch−2w, w, h, paleta, zestaw)`. Po dekodowaniu, zależnie od trybu poziomu
  (`0x4D2340(0xF)`: 1 noc, 2 mgła) barwienie `0x5046E0(piksele, liczba, procent, kolor)`. Warianty z maską:
  `0x504EB0`, `0x504F70` (wołane z `0x5BBAF9/0x5BBB19`).
- Zamiar: hak na `0x504CD0` zapamiętuje „wskaźnik pikseli → numer klatki”; przy rysowaniu ×2 (krok 2 i 4 wyżej) mod
  bierze klatkę HD z `hd\sprites\…` i kopiuje 1:1 z kluczem/znacznikiem; barwienie nocy/mgły tą samą funkcją gry.
- Upscaling sprite'ów: kolory krawędzi rozlane w przezroczystość, alfa i znacznik cienia skalowane osobno, na końcu
  korekta kolorów jak dla map. Czas: cały bank szybkim modelem ~30–70 min, DAT2 ~8 h.

## 5a. Klatki pośrednie animacji — eksperyment na obrazkach (2026-11-14; wdrożenie w grze: §5b)
Pytanie użytkownika: czy AI może dorysować klatki między klatkami animacji postaci. Odpowiedź: tak, modelem RIFE.
- **Model**: RIFE v4.26 (hzwer, MIT). Pobrane za zgodą użytkownika: `RIFEv4.26_0921.zip`, 22 869 906 B, z konta autora
  `huggingface.co/hzwer/RIFE` → `hd\models_vfi\rife_v4.26_dl\` (sha256 `1fa9b9cd…3f760e`). Użyty jest tylko plik wag
  `flownet.pkl`, wczytywany `torch.load(weights_only=True)`; kod z archiwum NIE jest uruchamiany — sieć jest napisana
  od nowa w `tools\hd\rife_net.py` (5,66 mln parametrów, 158 tensorów z pliku; 40 tensorów „teacher/caltime" służy
  tylko do uczenia). Próby kontrolne: identyczne klatki → wynik bez zmian; kwadrat przesunięty o 16 px → błąd 0,0001
  (przenikanie 0,0195). Skrypty czytające wagi uruchamiać przez `python -I`.
- **Jak**: obie klatki HD na wspólnym płótnie (przesunięcia klatek z `.rhs`), nałożone na jednolite tło i powiększone
  ×4 (sieć uczono na filmach, a postać ma ~56×114 px); sieć liczy pole ruchu i maskę, a kolor, krycie i cień sprite'a
  są przesuwane tym samym polem → wynik jest znowu klatką sprite'a (figura / cień / przezroczyste).
- **Test z prawdziwą odpowiedzią** (`interp_survey.py`: co druga klatka animacji usunięta i odtworzona; błąd na figurze,
  na 255; dolna granica = różnica dwóch klatek tej samej pozy = 9,2):

  | animacja Robina | ruch px | hold (gra) | przenikanie | przepływ OpenCV | RIFE ×2 | RIFE ×4 | RIFE ×8 |
  |---|---|---|---|---|---|---|---|
  | 112 chód gęsty | 0,5 | 15,4 | 13,1 | 12,8 | 12,5 | 12,3 | 12,2 |
  | 128 chód | 0,5 | 15,9 | 14,3 | 14,0 | 13,9 | 13,6 | 13,5 |
  | 224 | 2,7 | 25,1 | 21,8 | 15,6 | 15,1 | 14,7 | 14,6 |
  | 320 | 1,1 | 20,6 | 17,5 | 16,2 | 15,6 | 15,5 | 15,3 |
  | 432 | 3,3 | 28,3 | 24,6 | 20,2 | 19,0 | 18,3 | 18,4 |
  | 672 | 1,7 | 21,3 | 18,0 | 16,5 | 16,3 | 15,9 | 16,0 |
  | 752 skok | 12,1 | 31,0 | 29,4 | 27,7 | 23,9 | 23,5 | 23,3 |
  | 1264 | 6,5 | 25,9 | 24,4 | 22,9 | 21,2 | 20,8 | 20,4 |

  Test jest trudniejszy niż użycie w grze (tam klatka pośrednia leży między DWIEMA KOLEJNYMI klatkami, czyli przy
  połowie tego ruchu). Wzrokowo: RIFE daje jedną ostrą sylwetkę także przy dużym ruchu, przenikanie i przepływ OpenCV
  dają podwójne „duchy". ×4 jest prawie tak dobre jak ×8 przy 55 ms zamiast 137 ms na parę (×2: 39 ms).
- **Zastrzeżenia**: (1) przejście „ostatnia → pierwsza klatka" animacji, która się nie zapętla (np. lądowanie → start
  skoku), daje bezkształtną plamę — przy generowaniu potrzebny próg jakości (np. niezgodność obu przesuniętych klatek)
  i/lub generowanie tylko par, które w grze naprawdę po sobie następują; (2) ruch nieliniowy (przykucnięcie) daje
  wiarygodną, ale inną niż w grze pozę; (3) klatki wygenerowane są nieco gładsze od oryginalnych.
- **Co to dałoby w grze**: chód i większość animacji zmieniają klatkę co tik (25 kl./s; pole `flags` w `.rhs` to czas
  trwania kroku w tikach, 0 = jeden tik; `s16` = przesunięcie na krok). Mod rysuje już klatki między tikami (pozycje są
  interpolowane) — jedna klatka pośrednia na tik to animacja postaci w 50 kl./s. Animacje z długimi krokami (bezczynność:
  2–15 tików) zyskałyby najmniej z jednej klatki.
- **Koszt pełnego wdrożenia**: 615 622 różnych par kolejnych klatek we wszystkich animacjach (474 904 po złożeniu par
  A→B i B→A); ~5–6 GB na dysku; generowanie ok. 1–3 h w kilku procesach (karta ~25 min, reszta procesor); w modzie:
  pamiętanie poprzedniej klatki każdego sprite'a, magazyn par, podmiana w klatkach między tikami, przełącznik w INI.
- Narzędzia i obrazy: `tools\hd\rife_net.py`, `interp_test.py` (arkusz + animacja porównawcza), `interp_survey.py`
  (tabela wyżej, `--sheet`), `interp_demo.py` (animacja 25 vs 50 kl./s i arkusz wszystkich klatek);
  `hd\compare\chod_25_vs_50.gif`, `skok_25_vs_50.gif`, `chod_wszystkie_klatki.png`, `skok_wszystkie_klatki.png`,
  `klatki_posrednie*.png`, `chod_klatki_posrednie.gif`.

## 5b. Klatki pośrednie w grze — najpierw próba na Robinie (2026-11-14, `HDInBetween=1`, klawisz F9) [Z w tle]
Użytkownik po obejrzeniu arkuszy zapytał, czy „będzie gorzej przez ghosty i blury w ruchu" → mała próba zamiast całości.
- **Dane**: `tools\hd\make_inbetweens.py --files RobinTown.rhs` (potem `--loops`) → `hd\sprites\_between\between.idx`
  („HDBT", wersja 1, liczba, 0; wpisy 32 B posortowane po (a, b), a < b: `u32 a, u32 b, u64 offset, u32 rozmiar, u16 szer,
  u16 wys, i16 dxA, dyA, dxB, dyB`; rozmiar 0 = para odrzucona) + `between.dat` (klatki jak w `sprites.dat`). Jedna klatka
  na parę, używana w obu kierunkach; `dx/dy` = lewy górny róg klatki pośredniej względem lewego górnego rogu klatki A / B.
  RIFE na obrazach ×4, partiami wg rozmiaru płótna: **22 pary/s** (RobinTown: 9040 par w 6,9 min).
- **Kontrola jakości** (pomiar na powiększeniu ×4): obie klatki źródłowe przesunięte „do środka" muszą się zgadzać —
  `colour` = średnia różnica barw na figurze (na 255), `shape` = udział sylwetki, na którym się różnią. Progi z oglądu
  240 par (`hd\compare\posrednie_proba.png`): zwykłe pary `colour ≤ 32` i `shape ≤ 0,30` (do ~17 czyste, 27–36 miękkie, ale
  jedna spójna figura; dalej rozmazane). Pary widziane tylko jako „ostatnia → pierwsza klatka" to najczęściej cięcia:
  próg `≤ 20 / ≤ 0,10`, a w drugim przebiegu (`--loops`) przyjmowane są te, których obie wartości nie przekraczają
  1,25 × mediany kroków WEWNĄTRZ tej samej animacji (zamknięcie cyklu chodu 293635→293616 miało 20,01 / 0,057).
  Wynik: RobinTown 6461 par użytecznych z 9040 (53,9 MB); pomiary w `hd\work\between_log.csv`.
- **Skąd wiadomo, co pokazać** [K+Z]: metoda obiektu sprite'a `0x005BBA00` (`this` = sprite, `ret 4`; pola: `+0x184` nr
  animacji, `+0x186` nr kroku, `+0x1D0` bank) woła dekoder banku — hak zapamiętuje, który obiekt dekoduje; dla każdego
  obiektu pamiętana jest klatka z poprzedniego tiku (licznik tików = własne klatki tików, `Timing` → `DDM::HD::OnTickFrame`).
  **Sprawdzone śladem**: dla stojących postaci krok pozycji rysowania między kolejnymi klatkami = dokładnie 2 × różnica
  przesunięć `x,y` z `.rhs` (reszta (0,0) we wszystkich 13 sprite'ach; u idącego Robina reszta = jego ruch).
- **Kiedy** : mod rysuje ~6 obrazów na tik (144 Hz); `Timing::RenderPhase()` = położenie obrazu w tiku (0..1; −1, gdy nie
  ma klatek dodatkowych). Sprite, który w tym tiku zmienił klatkę A→B i ma parę w magazynie, jest w obrazach o fazie
  < 0,45 rysowany klatką pośrednią, potem klatką B (pomiar: 3/3 obrazy; przy progu 0,6 było 4/2). Czyli B, M, C, M…
  po ~20 ms — animacja 50 kroków/s. Pozycje i tak są interpolowane (`SmoothMotion`).
- **Zasłanianie** : gra wycina z powierzchni sprite'a to, co zasłaniają mury/drzewa (w mieście Robin ma `cut=1` prawie
  zawsze). Z powierzchni i klatki HD powstaje mapa „zasłonięte/widoczne" (pewna tam, gdzie klatka HD jest pełna;
  rozszerzana o 6 pikseli na boki) i jest nakładana na klatkę pośrednią (`HiddenMap`).
- **Wynik w tle**: 938 klatek pośrednich w 25 s chodzenia — 25,0 Hz, 7,0 ms pracy na tik, 143,9 kl./s (bez zmian);
  test ciągły 3 min PASS. Obraz: `hd\compare\posrednie_w_grze.png` (klatka gry / pośrednia / gry / pośrednia).
- **Ograniczenia**: tylko pary z plików podanych generatorowi (Robin: `RobinTown.rhs`, `RobinHood.rhs`); przejścia między
  animacjami i pary odrzucone idą po staremu (jeden krok 40 ms); kroki trwające kilka tików dostają jedną klatkę
  pośrednią na początku; klatki pośrednie są miększe (szum kompresji obu klatek się uśrednia — najbardziej widać to na
  płaszczu z kapturem); sprite'y rysowane przez DirectDraw (obiekty, animacje tła) — bez klatek pośrednich.
- **Całość** — 2026-11-14 użytkownik: „Tak, wszystkie postacie" → patrz §5c.

## 5c. Klatki pośrednie dla wszystkich postaci (2026-11-14)
- Zakres: 117 plików `DATA\Characters\*.rhs` (`--files Characters/`; animacje z `DATA\Animations\…` to tła i efekty,
  rysowane przez DirectDraw — klatek pośrednich tam nie ma): 148 577 animacji, **457 552 par** (59 579 tylko jako
  „ostatnia → pierwsza").
- **Generator przepisany na kartę graficzną** (`Maker.run`): powiększenie ×4 dwusześcienne (`F.interpolate`), obraz dla
  sieci, RIFE, przesunięcie płaszczyzn, miary jakości i zmniejszenie ×4 (`avg_pool2d` = średnia z obszaru) — na kartę idzie
  tylko małe płótno. `--selftest 240` (losowe pary postaci): stara droga (OpenCV na procesorze) 15,9 pary/s, nowa 111 par/s
  przy **identycznym wyniku** (różnica klatek 0,01 na 255, te same decyzje 240/240). Półprecyzja (`--half`, autocast fp16)
  była wolniejsza (31–92 pary/s) i różniła się o 0,35 — nieużywana. Pamięć karty: 1,3 GB na proces.
- **Nowa miara: cienkie elementy** (`thin_kept`): przy szybkim zamachu mieczem/włócznią klatka pośrednia potrafiła zgubić
  ostrze (została sama rękojeść), a kolor/kształt tego nie widzą — ostrze to mały ułamek sylwetki. Cienkie części = sylwetka
  minus jej otwarcie elipsą 5×5 (pas węższy niż ~5 px HD); klatka pośrednia musi zachować ≥ 50 % średniej z obu klatek
  (gdy mają ich ≥ 40 px). Próba 800 par wszystkich postaci (`--survey 800 --sheet-thin 0.7`): 105 par poniżej 0,5, z czego
  38 (4,8 %) przechodziło pozostałe progi — to były właśnie znikające miecze, włócznie, halabardy. Przy 0,6–0,7 klatki
  wyglądają dobrze. Próg koloru 32 sprawdzony na tej samej próbie: odrzucane pary tuż nad progiem są zwykle jeszcze
  poprawne — próg zostaje ostrożny.
- **Uruchomienie**: `python -I tools\hd\make_inbetweens.py --files Characters/ --jobs 3 --redo` — 3 procesy (części
  `_between_part0..2`, podział po `(a·31 + b) mod 3`); potem scalenie w `_between_new` → zamiana z `_between` (stary
  magazyn działa w grze do samego końca), drugi przebieg `--loops`. Przerwane? To samo polecenie kontynuuje (gotowe pary
  w częściach zostają). Podczas działania gry (`Game.exe`) generator czeka i oddaje pamięć karty.
- **Pułapka: pamięć karty przy kilku procesach.** Pierwsze podejście (bez limitu): po 5 min razem 11,5 z 12 GB (każdy
  proces trzymał zapas po płótnach różnych rozmiarów), Windows zaczął przenosić pamięć karty do RAM i dwa procesy stanęły
  na 8 minut. Teraz każdy proces ma limit `torch.cuda.set_per_process_memory_fraction(0,7/N)` (`--card-share`) i
  `PYTORCH_CUDA_ALLOC_CONF=garbage_collection_threshold:0.6,max_split_size_mb:64` → razem 5,8–7,8 GB, bez zastojów.
- **Wynik (2026-11-14, 14:00–14:58)**: 3 procesy po 45–47 min, 47–50 par/s każdy (na początku 73, potem większe płótna);
  scalenie + drugi przebieg 2 min. **457 198 par, 314 756 użytecznych (69 %), 2,98 GB**; 354 pary pominięte (płótno
  > 400×400 px albo brak klatki HD).
- **Sylwetki z samego cienia** (`Blip00.rhs`, 2672 klatki: postacie poza zasięgiem wzroku rysowane jako ciemne
  sylwetki): pierwszy przebieg odrzucił wszystkie 3168 par (wymagana była kolorowa figura). Teraz miara kształtu porównuje
  wtedy cienie, a klatka może być samym cieniem → `--retry-shadow`: 3032 z 3168 użytecznych.
- **Sprawdzone w tle** (Nottingham, ustawienia gracza): ślad rysowania — klatka pośrednia narysowana w 518 z 531 obrazów,
  w których jej się należała (98 %; Robin, żołnierze, strażnicy, oficerowie, kusznicy, żebrak, mieszczanka, sylwetki,
  obracające się przedmioty do podniesienia); test ciągły 3 min: 25,0 Hz, 7,0 ms pracy na tik (najgorszy tik 23 ms),
  143,5 kl./s, 48 199 klatek pośrednich, 0 błędów. Nie sprawdzone: inne poziomy, noc/mgła (barwienie jak dla klatek HD).
- Narzędzia: `--stats`, `--selftest N`, `--survey N --sheet PLIK [--sheet-range LO HI | --sheet-thin X]`; logi
  `hd\work\make_inbetweens_all.log`, `make_inbetweens__between_partN.log`, pomiary `hd\work\between_log*.csv`
  (kolumna `thin_kept`).
- **Próg jakości w grze** (2026-11-15, zgłoszenie: „animacja Robina wygląda dziwnie, jakby raz było HD, raz nie”, zrzut
  z „duchem” w animacji spoczynku; „ruchy walki jakby w innym miejscu” przy zoomie): generator przepuszczał różnicę
  koloru do 32/255, a sam opisał klatki do ~17 jako czyste, 27–36 jako miękkie. Rozkład użytecznych: mediana 19,9;
  > 17 — 68 %, > 24 — 27 %; u Robina 73 % ponad 20. Miękkie klatki przeplatały się z ostrymi klatkami HD.
  `tools\hd\between_quality.py` zapisuje pomiar każdej pary w kolejności indeksu: `hd\sprites\_between\between.q`
  (`"HDBQ"`, u32 1, u32 liczba, u32 CRC-32 pliku `between.idx`, potem bajt na parę — 457 KB, idzie w paczce moda);
  DLL pokazuje tylko pary do `HDInBetweenQuality` (domyślnie 17 → 100 722 z 314 756; 24 → 230 821; 32 = jak w 1.1.x).
  Zły CRC (inny magazyn) → wszystkie, z ostrzeżeniem w logu. Sprawdzone w tle: w walce (szybkie ruchy) klatek pośrednich
  nie ma w ogóle, u spokojnych postaci są.
- **Ostre klatki pośrednie** (2026-11-15, v1.1.6, „więcej płynnych animacji”): miękkość brała się z jednego kroku —
  `mid = wa·mask + wb·(1−mask)`, mieszania dwóch przesuniętych klatek; gdzie się nie zgadzają, mieszanina jest rozmyta
  albo podwójna. Tryb `--sharp`: każdy piksel (na powiększeniu ×4) z JEDNEJ z dwóch klatek — tej, której sieć tam ufa
  bardziej (`mask > 0,5`); ostatnie zmniejszenie ×4 (średnia z obszaru) wygładza styk. Porównanie
  `tools\hd\inbetween_sharp_test.py` na 60 parach z miary 17–32 (`hd\compare\posrednie_ostre.png`: A | mieszanie | ostre | B):
  kolczuga i ręce rycerza (31,7) bez podwójnego wzoru, cienkie elementy lepiej (mediana zachowania 0,98 zamiast 0,92).
  Całość od nowa: `make_inbetweens.py --files Characters/ --jobs 3 --redo --sharp` — 61 min (3 procesy), **457 198 par,
  320 646 przyjętych** (dawniej 314 756), 3,04 GB; magazyn oznaczony w nagłówku `between.idx` (ostatnie słowo = 1).
  `between_quality.py` skaluje miarę ×17/32 → przy domyślnym `HDInBetweenQuality=17` gra pokazuje **wszystkie
  320 646** (dawniej 100 722). Stara lista jako `between.v1.q` (DLL bierze listę z pasującym CRC: gracze ze starą
  paczką mają dalej tylko czyste klatki). Test ciągły z walkami: 37 286 klatek pośrednich w 4 min (dawniej ~18 tys.),
  0 błędów; seria zrzutów walki przy zoomie 3×: postacie na swoich miejscach, ostre. Nowa paczka
  `SherwoodHD-3-klatki-posrednie.zip` (1,62 GiB) zastąpiła starą w wydaniu `hd-pack-1`.
- Generator wstrzymuje się tylko przy grze z WIDOCZNYM oknem (ukryta instancja testowa też ma tytuł okna — pierwsza
  wersja tego sprawdzenia zatrzymała generowanie na czas testu).
- **Spoczynek i długie kroki** (2026-11-15/16, v1.1.7; zgłoszenie: Robin w spoczynku „jakby ubranie się ciągle
  zmieniało”, „kucanie niskoklatkowe”, wyjście spod płaszcza i część zamachów nie płynne). Pomiary
  (`tools\hd\between_view.py --stats`: zmiana klatek 1×, „boil” = różnice HD tam, gdzie 1× stoi, łaciatość klatki
  pośredniej, przepływ optyczny): w animacji spoczynku klatki 1× różnią się w ¼ pikseli postaci, ale tylko cieniowaniem
  — przepływ p90 0,1–0,2 px; ostra klatka pośrednia to wtedy łatanina obu klatek (30 % sąsiednich pikseli z różnych
  klatek), błysk przy każdej zmianie. Ślad rysowania (`trace`) pokazał też, że sprite jest dekodowany w KAŻDYM tiku, a
  krok animacji trwa `flags + 1` tików (rekord .rhs; flags 2 → klatka 3 tiki = 120 ms) — kroków 2-tikowych jest
  najwięcej (175 tys.; 1-tikowych 96 tys.), a klatka pośrednia była tylko w pierwszych ~45 % pierwszego tiku (~20 ms z
  80–120 ms). Teraz: `tools\hd\between_motion.py` (DIS na klatkach 1× ×4, 6 procesów, 10 min) → `hd\work\between_motion.npz`;
  `between_quality.py` pomija pary z p90 < 1,25 px (35 597, q = 255; zostaje 285 049) i pisze `between.t`
  (`"HDBS"`, u32 1, liczba, CRC jak `between.q`, potem bajt na parę = tiki klatki b przy a→b, bajt = tiki a przy b→a;
  0 = nieznane, 65 % znanych). DLL (`FindBetween`): klatka pośrednia przez pierwszą połowę kroku — okno
  `tiki/2 − 0,05` (1 tik: 0,45 jak dawniej, 2: 0,95, 3: 1,45, max 4 tiki = 1,95); długość z `between.t`, inaczej z tego,
  ile trwała poprzednia klatka (śledzenie per obiekt: `since`, `held`). Sprawdzone w tle: kroki 2-tikowe → klatka
  pośrednia w 6 z 11 obrazów (było 3), animacje spoczynku bez klatek pośrednich. Próg sprawdzony na arkuszu
  `hd\compare\posrednie_prog_ruchu.png` (pary 0,99–1,18 prawie identyczne, 1,41–1,58 widoczny ruch rąk/broni).

## 5d. Czcionka HD (2026-11-15, „Tak, czcionka HD”; `src\ddm\ddm_hd_font.cpp`)
- Fonty gry to bitmapy `DATA\Interface\Fonts\*.bfn` / `Dialog.fnt` („SBFONT”, krój Lydian i inne; pliki TTF wymienione
  w `Game.exe` nie istnieją, `*.tfn` = Arial przez GDI dla list w menu). Plik: nagłówek (nazwa, wysokość, liczba znaków
  180, pierwszy znak 32), tabela 18 bajtów na znak (kod, x w pasku, szerokość, odstępy), potem bitmapy. `MANAGER.CFG`
  przypisuje fonty do zastosowań (Tooltips/Default/MenuText = `tooltips.bfn`, Dialogue = `Dialog.fnt` …). W grze każdy
  font to pasek komórek o stałej szerokości (ToolTips: 180 × 16 px = 2880×20); fonty „Lydian” mają drugi pasek
  pokrycia (kanał niebieski, waga 2·b z 64 — procedura `0x00622FD4`), pozostałe są z kluczem (zieleń `0x07E0`).
  Eksplorator formatu: `tools\hd\bfn_explore.py`.
- W misji (interfejs ×2) litery rysują: tryb `0x28401` → `0x00622FD4` (z pokryciem: dymki, komunikaty) i rozciąganie
  DirectDraw ×2 z kluczem (napis z pieniędzmi, imiona). Mod rozpoznaje pasek po treści (FNV-1a 64 pikseli koloru
  i pokrycia; niskie 12 bitów = wysokość), raz na powierzchnię (próbka 64 pikseli wykrywa podmianę), i rysuje literę
  z `hd\fonts\<klucz>.hdfont` (`"HDFT"`, u32 1, szer, wys = 2× paska, RGB565, bajt pokrycia na piksel) z mieszaniem
  po pokryciu (5 bitów; wcześniejsza wersja z 8 bitami „przelewała” czerwień w niebieski — niebieski obrys).
- Paski bez wersji HD mod sam zapisuje do `hd\work\fonts\font_<klucz>.raw` (tylko gdy istnieje `hd\work`), jak ikony;
  polecenie testowe `fonts dump|stop`. Konwersja/podgląd: `tools\hd\font_strips.py show`; powiększanie:
  `tools\hd\upscale_fonts.py --compare ultrasharp sgi dat2` (arkusz `hd\compare\czcionki_modele.png`) i
  `--model sgi` → `hd\fonts\`. Kolor (przezroczyste miejsca wypełnione od liter) i pokrycie (jako szary obraz) idą przez
  model osobno, modele 4× zmniejszane polem do 2×. Wybrany **SGI**: ostry i najwierniejszy grubością (UltraSharp
  pogrubia, DAT2 zostawia drobne ślady). Pliki fontów HD (2 × 0,7 MB) idą w paczce moda.
- Sprawdzone w tle: dymek „Pokaż / ukryj pole widzenia nieprzyjaciela” przy zoomie 3× i napis z pieniędzmi — ostre,
  ciemny obrys jak w oryginale (`tools\out\tooltip_hdfont_cmp.png`, `money_hdfont_x2.png`); test ciągły bez błędów.
- Menu i okna dialogowe: §5e.

## 5e. Napisy i obrazki menu w HD — „cienie” powierzchni (2026-11-15, v1.1.5; `src\ddm\ddm_hd_patch.cpp`)
Przy `MenuZoom` gra rysuje menu i okna w skali 1× (środek klatki), a prezenter powiększa tę część ×1,875. Każda litera
i każdy obrazek, który gra tam rysuje, jest rysowany drugi raz, 2× większy, do **cienia** powierzchni (tylko nad
pokazywaną częścią 1365×768: `ref` = piksel klatki, do którego należy blok HD, 0 = brak; `hd` = 2× BGRA):
- litery: procedura 1:1 z pokryciem `0x00622EB4` (brat `0x00622FD4`; tryb GBLT `0x20401` → `0x005E8410`) i fonty
  z kluczem kopiowane DirectDraw 1:1 — z fontu HD (§5d; nowe paski zbierane same, `upscale_fonts.py --model sgi`);
- obrazki: `BlendBlit` 1:1 (`0x00622550` z kluczem `0x07C0` / `0x00622028` bez; piksele „znacznika” przyciemniają tło
  — cień przycisku) i kopie DirectDraw 1:1 z kluczem (pergamin odprawy 496×463, portrety, pieczęcie). Wersje HD:
  `hd\menu_parts\<klucz>.hdp` („HDMP”, 2w×2h BGRA z pokryciem + bajt cienia na piksel), robi je
  `tools\hd\upscale_menu_parts.py` (SGI; aomr dawał pergaminowi „robaczkową” fakturę — `menu_parts_compare.py`)
  z obrazków zbieranych przez moda do `hd\work\menu_parts\` (tylko gdy jest `hd\work`);
- tło pod literą/obrazkiem w HD: cień (gdy jego `ref` zgadza się z pikselem „przed”), obraz HD menu (warstwa 1,
  `Menu::HdUnder`) albo stare piksele powiększone dwuliniowo; brzeg litery HD może sięgać piksela, którego wersja 1×
  nie ruszyła (gładki kontur), obrazka — tylko nad tłem HD;
- kopie 1:1 między powierzchniami wielkości klatki przenoszą cienie (gra składa okna w osobnych powierzchniach);
  kopia z małej powierzchni (tło spod kursora) zostawia blok tylko tam, gdzie piksel znów jest „swój”; wypełnienia
  i rozciągania kasują; kursor gry (`0x005F1700`) nie robi łatek. Cienie są per pamięć powierzchni (flip zamienia
  pamięci), do 5 × 21 MB, kasowane po wyjściu z pętli okien. Cała kopia ekranu do innej powierzchni = składanie okna
  (odprawa rysuje pergamin, zanim ruszy pętla okien) — taka powierzchnia przyjmuje łatki od razu.
- Prezenter (`ddm_present.cpp`): tekstury t6 (ref, R32_UINT) i t7 (HD) na każdą z dwóch pamięci, wysyłane prostokątem
  zmian; shader pokazuje blok HD (próbkowany tylko wewnątrz swojego bloku 2×2) przed warstwami 1–2, gdy piksel klatki
  = `ref`, a z 8 sąsiadów co najwyżej 4 *mających* łatkę już się nie zgadza (cienkie litery — sąsiedzi bez łatki się
  nie liczą). Polecenie testowe `patch` = liczniki; `dbg 64/128/256` = bez łatek / warstwy 1 / tła dialogu.
- Wynik (test w tle): odprawa, menu Esc, opcje (także z misji), wczytywanie, menu główne — litery, przyciski,
  pergamin, pieczęć, portret ostre; kursor nad napisami nie zostawia śladów (0 różnych pikseli).
- **Tekst GDI** (v1.1.6: lista klawiszy, lista zapisów — Arial, `*.tfn`): gra pisze go `TextOutA`/`TextOutW`
  (sloty importów `0x00677068` / `0x0067707C`) do DC powierzchni (`GetDC` → sekcja DIB, `DDM::SurfaceOfDC`),
  **znak po znaku**, przezroczyście, wyrównany do lewej i góry. Hak liczy pudełko znaku (`GetTextExtentPoint32`,
  `GetTextMetrics`, `GetTextAlign`; zapas pół szerokości znaku na boki — ogon „j” sięga pod poprzednią literę — i pół
  dolnego wydłużenia; przycięte do `GetClipBox`), pozwala grze narysować znak, a potem rysuje go drugi raz tą samą
  czcionką 2× większą z wygładzaniem (`ANTIALIASED_QUALITY`, biały na czarnym = pokrycie, gamma 0,75 — nie
  wygładzone litery gry wyglądają grubiej) i składa jak literę fontu HD (to, co narysowano, = piksele „przed” ≠ „po”).
  Tło `OPAQUE` wchodzi do rysunku. Oryginał miał kolorowe obwódki ClearType — wersja HD jest czysta.

## 5f. Noc i mgła (sprawdzone 2026-11-15, v1.1.6)
- Rodzaj mapy to flaga w polu silnika `+0x4AD0` (1 dzień, 2 mgła, 4 noc, 8 atak, 16..128 mapy własne), czytana
  z nagłówka poziomu przez `0x004C1AB0` (thiscall, `ret 0x10`); mapa idzie z `Data\Levels\<rodzaj>\`, kod nocy / mgły
  sprawdza `== 4` / `== 2`, a `+0x4CC0` (300 dla mgły i nocy, 400 inaczej) ustawia ta sama funkcja. Polecenie testowe
  **`ambiance N`** ustawia rodzaj dla następnego startu misji (tylko instancja testowa) — pierwsza misja kampanii
  biegnie wtedy na nocnej albo mglistej Lincoln. Zapisy: `tools\save_backup.ps1 -Restore -All` (także `Restart*`).
- Wszystkie 20 map (dzień, mgła, noc, własna) ma tła HD. Noc: postacie i zwierzęta NIE są przyciemniane — w oryginale
  też nie (porównanie `hd off`: świnia tak samo jasna); raport `miss` pokazał 9 klatek tworzonych w locie bez HD
  (ikona 21×28, 8 klatek 32×32 rysowanych mieszaniem — światło nocą) → `rtdump` + `upscale_icons.py --model ultrasharp`
  dorobiło 17 klatek (magazyn ikon 397 klatek; idzie w archiwum moda). Mgła: wszystko w HD (raport `miss` jak w dzień —
  tylko róg paska 29×95), postacie przygaszone mgłą jak w grze.
- Niesprawdzalne w tle: misje z drużyną (kilka portretów) i prawdziwy staw (Nottingham) — potrzebny postęp kampanii.

## 5g. Klatki pośrednie animacji poziomu i druga próba postaci (2026-11-22, v1.2.9)
Prośby użytkownika: „dla innych animacji też zrób klatki pośrednie”, „wszystko co jest animowane”, „i ta druga próba
odrzuconych”; kolejka na kartę graficzną `tools\hd\queue_between.ps1` (czeka, gdy okno gry jest otwarte).
- **Animacje poziomu** — `DATA\Animations\<pora>\*.rhs` (116 plików: gołębie, wrony, kaczki, kury, flagi, dym, muchy,
  krowy, świnie…), rysowane rozciągnięciami DirectDraw (`TryStretch`), nie ścieżką postaci. Osobny magazyn
  `hd\sprites\_between_anim\`: `make_inbetweens.py --files Animations/ --store _between_anim --sharp --frames-from
  _common dat2` (klatki HD części animacji są tylko w magazynie wariantu `dat2` — `FrameChain` bierze pierwszy, który
  ma klatkę), potem `between_motion.py --store _between_anim` i `between_quality.py --store _between_anim`. 15 714 par,
  13 223 przyjętych, **7 636 pokazywanych** (5 587 bez ruchu — p90 przepływu < 1,25 px, falowanie w miejscu, ostra klatka
  pośrednia byłaby łataniną obu jak u postaci w spoczynku). Klatki mniejsze niż 12 px po powiększeniu ×4 (iskry,
  krople) nie są mierzone (DIS ich nie liczy). Sieć RIFE wymaga boków podzielnych przez `64·max(skal)/16` — małe płótna
  dostają obrzeże z powtórzonych krawędzi, obcinane po obliczeniu.
- **Płynny tor** (`AnimateLevelSprite`, `ddm_hd_sprites.cpp`): ruch tych animacji jest w przesunięciach klatek, nie w
  pozycji elementu. DLL śledzi miejsce figury per obiekt (`g_places`): środek jej nieprzezroczystych pikseli w poziomie
  HD = `LevelOrigin` − przesunięcie rysowania + środek figury w klatce (nie róg — klatki siedzącego ptaka są różnie
  przycięte, przesuwanie rogów dawało drżenie). Gdy przy zmianie kroku figura skoczyła o 4–80 px HD, rysunek jedzie
  od poprzedniego miejsca do nowego przez cały krok (o krok później — tymi animacjami nikt nie steruje); w pierwszej
  połowie kroku klatka pośrednia (jeśli jest), przesunięta tam, gdzie figura jest w danej chwili. Tylko dla kroków,
  które zna magazyn `_between_anim` (także bez klatki) — postacie i reszta bez zmian. Ślad (`trace`): `ABT` = klatka
  pośrednia animacji poziomu, `AGL` = przesuw.
- **Druga próba odrzuconych par postaci** (`hd\sprites\_between_more\`): 136 552 pary, których pierwszy magazyn nie
  przyjął (upadki, szybkie ciosy, schylanie), od nowa z innymi skalami przepływu sieci. Ustawienia próbowane na
  losowych 1500 parach (`--survey`; pierwsza próba w kolejce stanęła na błędzie rozmiaru płótna — naprawione obrzeżem,
  patrz wyżej), najlepsze `--rife-scales 8,4,2,1,1` (~10 % odzyskanych w próbie). Całość (`make_inbetweens.py --files
  Characters/ --rejected-from _between --store _between_more --sharp --rife-scales 8,4,2,1,1`, 26 min):
  **11 400 przyjętych (8,3 %), 11 386 z ruchem**. Pokrycie kroków
  animacji postaci (`tools\hd\between_coverage.py --file Characters/`): 65 % → 68 % (Robin 67 % → 69 %).
- DLL: trzy magazyny w jednej przestrzeni indeksów (`SetOf`, `kBetweenSets`): `FindBetween` bierze pierwszy, który ma
  klatkę dla pary — `_between`, potem `_between_more`, potem `_between_anim`; każdy z własnym `between.q` / `between.t`
  pod CRC swojego indeksu. Log przy starcie: `HD sprites: N in-between frames for the characters (second try) / the
  level's animations available`.
- **Odchudzenie** (`tools\hd\compact_between.py`): generator trzyma każdą wypróbowaną parę; zostają tylko klatki
  pokazywane przy domyślnym `HDInBetweenQuality=17` (magazyny ostre — wszystkie przyjęte mają q ≤ 17, poza parami bez
  ruchu). `_between_anim --keep-all`: indeks cały (płynny tor musi znać każdy krok), dane 277,7 → 131,3 MB;
  `_between_more`: tylko pokazywane wpisy, indeks 4,37 → 0,36 MB, dane 102,8 MB. `between.q`, `between.t` i
  `hd\work\between_motion<magazyn>.npz` przepisane pod nowy CRC (`between_quality.py` działa dalej na odchudzonym).
- Sprawdzone w tle (Nottingham, kamera strzałkami, `trace` w kilku miejscach mapy): magazyny wczytane (7 636 i 11 386
  klatek, listy jakości i długości kroków pasują); klatki pośrednie flag, świni, krowy, kury, much i gołębia 03, przesuw
  much i gołębia (skoki 4–10 px HD, w pierwszej połowie kroku klatka pośrednia przesuwana o ~1 px na klatkę obrazu,
  potem przesuw do końca kroku — ciągle); zrzuty flagi co 60 ms ostre, bez podwójnego rysunku. Lot gołębi 01/02 (duże
  skoki) się nie trafił — pauzy między lotami 4–20 s.
- Wydanie: oba magazyny w archiwum `SherwoodHD-4-klatki-posrednie-2.zip` wydania `hd-pack-1` (`tools\make_hd_packs.py
  --only 4`) — dane zmieniają się rzadko, archiwum moda zostaje ~150 MB. Bez niego mod działa jak w 1.2.8.

## 5h. Warstwa poziomu poniżej zoomu 2,0 (1.3.0, 2026-11-22, `[Display] HDLevelLayer=1`; `src\ddm\ddm_present.cpp`)
- Życzenie użytkownika: „oddalone do końca jakość nie jest HD”. Poniżej 2,0 silnik rysuje poziom w 1,0 w oryginalnych pikselach, a płynny zoom go powiększał [Z].
- Teraz tam, gdzie kopia świata nadal pokazuje własny piksel obrazu poziomu (kamera + piksel świata), wyjście pokazuje obraz HD poziomu (przy `HDMaps4k=2` lub silnym powiększeniu — obraz 4K, §5i). Postacie i obiekty zostają takie, jak silnik rysuje je w 1,0 (powiększone) — silnik zna tylko 0,5 / 1 / 2 [K].
- `HD::LevelOrigin` = piksel HD w pikselu (0,0) powierzchni świata = 2 × kamera.

## 5i. Natywne mapy 4K (1.3.0, `[Display] HDMaps4k`, `hd\maps4k\<wariant>\<klucz>.hdm4`)
- **Format** `.hdm4`: „HDM4”, u32 wersja 1, u32 szerokość, u32 wysokość, u32 0, potem wiersze RGB565 od góry do dołu (z ditheringiem). Klucz = ten sam 64-bitowy FNV-1a obrazu poziomu co w `.hdmap` (§3).
- **Wytwarzanie**: `tools\hd\upscale_4k.py` (4× model 4xNomos2_hq_dat2 + dopasowanie kolorów do oryginału → `hd\maps_png\4k\<mapa>_<wariant>.png`) i `tools\hd\make_maps4k.py`. Wszystkie 20 map, tylko wariant dat2, 3,3 GB.
- **Kiedy**: `HDMaps4k=1` (domyślnie) — tam, gdzie ekran pokazuje poziom większy niż jego obraz HD (2×): zoom powyżej ok. 2,2 na 2560×1440 (`ZoomMax` = 400 %), na monitorze 4K już od 2,0. `2` = także poniżej (obraz 4K zmniejszony, do porównań). `0` = wyłączone.
- **Wczytywanie**: prezenter czyta plik w tle (wątek czytający, dwa bufory) paskami po 4 MB, najwyżej jeden pasek na prezentowaną klatkę, do tekstury DEFAULT B5G6R5; ok. 1 s po starcie poziomu. Tylko pokazywany wariant (F6/F7 porównują warianty; 4K ma tylko dat2).
- **Gdzie widać 4K**: tam, gdzie świat nadal pokazuje własny piksel tła HD (dokładne porównanie RGB565 z obrazem HD poziomu w `HD::LevelOrigin` + piksel świata; reguła sąsiadów: różni się najwyżej 3 z 8) — wyjście próbkuje obraz 4K (4 próbki dwuliniowe co ćwierć stopy piksela wyjścia). Postacie, drzewa (to sprite'y), cienie, stożki widzenia i interfejs zostają takie, jak rysuje gra.
- **Test** [Z, w tle]: mapa powozu (Croisement02): zoom 3,0 i 4,0 wyraźnie więcej szczegółu (źdźbła trawy); wnętrze 2048×1152 z wyjściem 2560×1440 (jak ekran 4K) przy zoomie 2,0 prosto z klatki. Harness: `dbg 4096` barwi piksele z warstwy poziomu, `dbg 2048` wyłącza warstwę poziomu.
- **Wydanie**: `SherwoodHD-5-mapy-4k-a.zip` (skrzyżowanie, obóz w Sherwood, Derby, Leicester, Lincoln; 1,16 GB) i `SherwoodHD-5-mapy-4k-b.zip` (Nottingham, York; 1,19 GB) w wydaniu `hd-pack-1`, opcjonalne; `tools\make_hd_packs.py` pakuje 5 / 5b; `tools\upload_hd_pack.ps1 -Only <nazwy>` wysyła tylko nowe paczki.
- **Zmiana wariantu przy zmienionym obrazie poziomu**: misja powozu rysuje powóz w obrazie poziomu; po F6/F7 poziom zostawał w oryginalnych pikselach (klucz zmienionego obrazu nie ma pliku). Teraz wariant szukany po kluczu własnego obrazu poziomu; kopia oryginału zostaje (zmieniona część pokazana z obrazu gry).

## 5j. Przytrzymanie klatek przejściowych przy dialogach (1.3.0)
- Zgłoszenie: na końcu rajdu zwój pojawiał się na ułamek sekundy ucięty z prawej, z jasnym poziomem, potem cały.
- Dziennik prezentera (`presentlog N` — nowa komenda harnessu, loguje następne N prezentowanych klatek) pokazał dwie klatki przejściowe [Z]: lupa menu nadal włączona po końcu pętli dialogu pytania, bez dialogu w klatce (jasny poziom powiększony 1,875×), oraz pierwsza klatka następnego dialogu narysowana przed kopią świata płynnego zoomu (poziom 1,0, niepowiększony).
- Takie klatki nie są prezentowane — zostaje poprzednia, najwyżej 6 pod rząd.

## 5k. Pauza (1.3.0)
- W pauzie (flaga translatora wejścia, singleton `[0x006C0C4C]` + 0x1B) `RHGame::Run` pomija tik silnika, przeskakuje ogranicznik klatek (`0x005100DB..0x005100E0` → `0x00510126`) i nie rysuje żadnej klatki: pętla kręciła ok. 47 000 przebiegów na sekundę na pełnym rdzeniu (także przy `VSync=1`) [Z].
- Teraz hak w tym miejscu robi najwyżej jeden przebieg na odświeżenie i wykonuje pracę moda na tik: F6–F9 i przełączniki panelu F10, przełączenie poziomu silnika przez płynny zoom, watchdog. Klatka liczy się jako tik gry tylko gdy zmienił się licznik tików silnika `[silnik + 0x4A94]` (zwiększany w `0x004C7254`). Harness: `pause 1 | 0`. Szczegóły adresów: `docs\RE_NOTES.md`.

## 6. Tryb testowy w tle (żeby nie zabierać użytkownikowi ekranu)
- `tools\htest.ps1 [-Ini plik] [-MenuOnly]` — start ukrytej gry (`SHERWOODFIX_TEST=1`), pominięcie filmów, „Graj!".
- `tools\hctl.ps1 -Cmd "a; b" [-Shot nazwa | -ShotOut nazwa] [-Crop x,y,w,h] [-MaxWidth n] [-Wait ms]`.
  Polecenia: `move`, `moveto`, `click [L|R]`, `down`, `up`, `wheel`, `key DIK [ms]`, `zoomin`, `zoomout`, `shot`,
  `shotout`, `mem ADR LEN`, `stat`, `hd [next|prev|toggle|NAZWA|off]`, `dbg MASKA` (pomijanie procedur rysujących:
  1 sprite'y z kluczem, 2 bez, 4 wielokąty, 8 rozciąganie DirectDraw z kluczem, 16 klatka pośrednia w każdym obrazie
  tiku), `trace N NAZWA` (ślad rysowania z adresami wywołujących; od 2026-11-14 także `GBLT …` = wywołania ogólnego
  blittera gry z flagami, `SPR …` = każdy sprite z klatką, poprzednią klatką, pozycją i fazą tiku, `BTW …` = narysowana
  klatka pośrednia), `frames NAZWA`, `miss NAZWA [TIKI]` (co zostało powiększone bez klatki HD), `rtdump NAZWA` (klatki
  tworzone przez grę w locie → plik dla `upscale_icons.py`), `between on|off|toggle`, `quit`. `stat` pokazuje też
  `between=` i `hdBetween=` (liczba narysowanych klatek pośrednich). Od 2026-11-14 (§7): `uiwins` (okna misji:
  vtable, prostokąt, co każde narysowało → `tools\out\harness\uiwins.txt`), `settings open|close|up|down|left|right|
  click|state` (panel F10; `state` = wartości wszystkich opcji). Ślad ma też `UI DRAW begin/end` (skąd wywołany
  `WindowManager::Draw`), `UI BLEND`, `POINTER`, `SETCURSOR`.
- `tools\hsoak.ps1 [-Minutes n] [-Ui]` — test ciągły z werdyktem (tempo tików, błędy w logu); `-Ui` dodaje minimapę,
  portret, menu Esc i panel ustawień.
- Zrzut `shot` = klatka gry bez nakładek moda; `shotout` = to, co trafia na ekran (z licznikiem i napisami).
- **Izolacja pulpitu**: w trybie testowym `SetCursorPos`, `ClipCursor`, `SetCursor`, `ShowCursor`, `SendInput`,
  `mouse_event`, `keybd_event`, `SetForegroundWindow`, `SetActiveWindow`, `BringWindowToTop`, `SetCapture` są podmienione
  dla całego procesu (plus import gry). Bez tego `RHGame::Run` (`0x50F967..0x50F98E`: `GetWindowRect` + `SetCursorPos`
  na środek okna w KAŻDYM obiegu pętli misji) przypinał kursor użytkownika do środka ekranu. `stat` pokazuje
  `desktopBlocked=`; `htest.ps1` kończy instancję, jeśli tego pola nie ma albo kursor stoi na środku mimo ruchu myszy.
- Urządzenia DirectInput gra otwiera z `DISCL_FOREGROUND` (klawiatura 0x16/0x15, mysz 5/6), więc ukryte okno nigdy
  ich nie dostaje — klawiatura i mysz użytkownika nie trafiają do instancji testowej. [K]

## 7. Interfejs ×2, menu na cały ekran, panel ustawień (2026-11-14, `src\ui_scale.cpp`, `src\settings_menu.cpp`)

### Interfejs misji ×2 (`UIScale=2`, szerokość wewnętrzna ≥ 2048)
Gra rozmieszcza okna w pikselach ekranu z wielu miejsc (konfiguracja grafiki czytana w ~60 miejscach, kopia w RHGame),
więc układanie ich dla mniejszego ekranu odpadło. Zamiast tego każdy obrazek okna jest rysowany 2× większy **od swojego
rogu ekranu** (siatka 25/50/25 %): okno w lewym dolnym polu rośnie od lewego dolnego rogu, w górnym środkowym od górnej
krawędzi wokół własnego środka, w środkowym wokół własnego środka. Pozycja myszy dla okien jest przeliczana wstecz
kotwicą obrazka, nad którym jest kursor (lista obrazków z ostatniej klatki), więc kliknięcie trafia w małe okno gry.
- Kotwica okna: pętla `WindowManager::Draw` (0x006189E0) — hak w środku funkcji na 0x00618A34 (bieżące okno) i
  0x00618A3F (koniec okna). Prostokąt okna: floaty l,t,r,b na +0x70, bajt ważności +0x80. Okna średniej wielkości
  (≤ 50 % ekranu) mają jedną kotwicę dla wszystkich obrazków; okna tej samej klasy (vtable) — wspólny prostokąt (rząd
  portretów rośnie razem od lewego rogu). Dwa okna HUD na cały ekran (vtable 0x0067AD9C) — obrazek po obrazku.
- Mapa: to samo okno co przycisk w prawym górnym rogu (vtable 0x00679B70); otwarta ma 158×246 przy 1039,90.
  `MapPosition=1` przesuwa ją ×2 pod przycisk (prawy górny róg, 8 px od krawędzi, od y = 104·2).
- Wskaźnik okien misji: RHGame+0x68+0x1758, pozycja floaty +0x80; `Pointer::Update` (0x00628210) dodaje ruch z
  InputState (s16 dx,dy) — przy skali ruch jest zerowany na czas wywołania, a pozycja ustawiana z przeliczenia.
- Rysowane poza blitami (piszą piksele same, ich prostokąty też przeliczane): `DrawRect` 0x005ECDC0 (ramka widoku
  kamery na mapie; linia pogrubiana do 2 px), `BlendBlit` 0x005EC5F0 (z cieniem 50 %, np. 29×95 w prawym dolnym rogu;
  sam rozciąga, nie przycina → przycinanie po stronie moda; przy ×2 kończy w procedurze sprite'ów 2× `0x0062261C`,
  od 1.3.1 obraz z `hd\menu_parts` — `Sprite2xWindowHd`).
- Poza menedżerem okien, z `RHGame::RenderFrame`: napis z pieniędzmi 0x004C8140 (od lewego górnego rogu), zegar misji
  0x00511B90 (jedna kotwica dla całego napisu), komunikat 0x005C84A0 (od dołu, środek), kursor 0x005F1CA0 (wokół
  „czubka"). Kursor gra składa w powierzchni 55×55 (kopia tła + kursor) — przy powiększeniu piksele równe pierwszej
  kopii tła są pomijane (`ddm.cpp`), inaczej widać kwadrat tła.
- Przełączanie w locie (panel F10): przy wyłączeniu wskaźnik okien jest raz ustawiany pod prawdziwy kursor.
- Poprawki po pierwszej sesji użytkownika (2026-11-14 wieczór):
  - Kursor nad światem (pod nim żaden powiększony obrazek) → okna widzą prawdziwą pozycję (przedtem przeliczenie
    względem środka ekranu: gra nie widziała kursora przy krawędzi, przewijanie krawędziowe nie działało). Wyjątek:
    miejsce, gdzie dla gry leży przeniesiona do rogu mapa — tam nadal przeliczenie od środka (bez „duchów”).
  - Wskaźnik dostaje ruch zgodny z przeliczeniem (od poprzedniej pozycji do nowej), nie zero — `Pointer::Update`
    zapisuje ruch w +0xB0, z niego korzysta m.in. przeciąganie.
  - Okno zachowuje kotwicę z chwili pojawienia się (przy tym samym rozmiarze): mapę gra pozwala przeciągać —
    kotwica liczona co klatkę od nowa z przesuwanego prostokąta dawała szarpanie w miejscu.
  - Klik lewym na zwiniętych końcach zwoju (górne i dolne 8,5 % wysokości) idzie do gry jako prawy = zamyka mapę
    (`input_fix.cpp` `Emit`, prostokąt mapy publikowany przez `UiScale::OnMapScrollEnd`).
  - Okna dialogowe przy przybliżeniu menu: kursor gry jest trzymany w pokazanej części, a wskaźnik okna tylko w
    ekranie — po dojechaniu do krawędzi rozjeżdżały się i kliknięcia nie trafiały w przyciski. Teraz wskaźnik
    okien dialogowych jest w każdym `Pointer::Update` ustawiany dokładnie pod kursor.
  - Ramka zaznaczenia: `RHEngine::DrawSelection` 0x004D0400 rysuje 4 linie (0x005ECBD0) między punktami świata
    E+0x78 (początek) i E+0x80 (koniec), aktywna gdy E+0x74; koniec gra przesuwa tylko na tikach (25 Hz). W klatkach
    między tikami koniec jest na czas rysowania ustawiany pod kursor (kamera E+0x4738 + kursor / zoom E+0x475C).
  - Podpowiedź „F10 - USTAWIENIA MODA" w prawym dolnym rogu menu i okien gry.
  - Narzędzia: `memfind F1 [F2]` (gdzie gra trzyma dane wartości float), w śladzie `LINE by …` (linie gry,
    tylko w trybie testowym), `tools\memfloats.ps1 -Addr … -Len …` (pamięć jako liczby float).
- Minimapa — przeciąganie (2026-11-14 noc, `MapDragCamera=1`, pozycja w panelu F10) [Z w tle]:
  - Obsługa wskaźnika okna mapy: 0x0054A640 (flagi wskaźnika z `Pointer::Update` +0xA4: 0x01 klik lewym przy
    puszczeniu, 0x08 dwuklik, 0x40 lewy trzymany, 0x02 klik prawym, 0x80 prawy trzymany). Klik lewym na obrazku
    poziomu → 0x0054BFB0 (centruje kamerę: `CenterCameraOn` 0x004C7F60 na `(mapW·u, mapH·v)`), lewy trzymany →
    0x0054C1E0 (przeciąganie okna: mapa+0xB0 = start, +0xB4 wskaźnik na starcie, +0xBC pozycja okna na starcie,
    +0xB1 = przesunięte; okno rusza dopiero po 15 px), prawy klik zamyka. Prostokąt otwartej mapy: mapa+0xE4,
    ustawiany przez 0x0054A0F0 (thiscall(mapa, Vec2* lewy górny), trzyma w ekranie) i oddawany oknu przez 0x0061AD20
    (thiscall(okno, Rect*)). Obrazek poziomu = prostokąt okna (+0x70) minus margines z 0x006C0C58 (14, 24).
  - Zmiany: przeciąganie zaczęte na obrazku przesuwa kamerę (ciągłe centrowanie, poza mapą dociśnięte do krawędzi);
    okno przesuwa się za brzegi lewym albo **prawym** (`input_fix.cpp`: prawy wciśnięty na brzegu idzie do gry jako
    lewy; prawy klik bez ruchu = lewy + prawy klik, czyli zamknięcie jak dotąd); przeciągane okno jest ustawiane pod
    kursor w każdej rysowanej klatce (gra robi to na tikach 25 Hz i po 15 px — przy ×2 skok o 30 px ekranu); w czasie
    przeciągania wskaźnik jest przeliczany kotwicą mapy (przy szybkim ruchu kursor zjeżdżał z obrazka, przeliczenie
    zmieniało kotwicę i okno skakało w tę i z powrotem); przeciąganie kończy się z przyciskiem (puszczony poza oknem
    zostawiał w grze „trwające” przeciąganie i następne przeciąganie gdziekolwiek ruszało mapę).
  - Sprawdzone w tle (mapa 2944×2176): przeciąganie po obrazku 0,0 → 1437,1075 (zgodnie z wyliczeniem), poza mapą
    x = 0; prawy za brzeg: okno −150/+90 px przy ruchu myszy −300/+180; prawy klik na brzegu zamyka; lewy za brzeg
    przesuwa okno; klik na obrazku centruje (764,705); zaznaczanie w świecie nie rusza kamery.
- Wydajność (2026-11-15, zgłoszenie „spadki do 112 FPS”): każdy obrazek interfejsu ×2 szedł co klatkę przez filtr
  wygładzający (`SmoothSprite2x16`) — sam HUD to ~0,6 Mpx na klatkę, ~5 ms, połowa czasu klatki (linie `Blit(5s)`:
  „stretch 2,5 Mpx 20 ms na tik” przy ~4 klatkach na tik). Teraz `SmoothPicture2x16` (`ddm_blit.cpp`) trzyma wynik
  filtra: piksele w pełni kryjące jako gotowe kolory (kopiowane `memcpy` w zakresach wierszy), miękką krawędź jako sumy
  kolorów z pokryciem (mieszane z tym, co pod spodem — te same piksele co filtr). Wpis jest używany tylko, gdy piksele
  źródła czytane przez filtr (z ramką 1 px) są dokładnie takie jak przy jego tworzeniu (`memcmp`); obrazek zmieniający
  się co chwilę idzie bez pamięci. Wynik (w tle, 2560×1440, zoom 2,0): rozciąganie z 20 ms do ~2 ms na tik, klatka
  z ~10 ms do ~4,5 ms; z VSync gra trzyma odświeżanie ekranu (tu 144 Hz), bez VSync ~145–150 kl./s przy innym
  procesie obciążającym CPU w tle.
- Odprawa z kilkoma stronami (2026-11-14 noc): przy każdej stronie zaczyna się nowa pętla okna dialogowego, a „zdjęcie
  poziomu” do rozpoznania tła (`NoteDialogStart`) było robione z ekranu, na którym leżał już pergamin poprzedniej
  strony — pergamin liczył się jako poziom (99 %) i zostawał mały, a powiększony tekst wychodził poza niego. Teraz
  zdjęcie jest robione tylko wtedy, gdy od poprzedniego okna gra narysowała klatkę misji (strony 1–4: 94 %).
- Opcje gry — procesor i pamięć (2026-11-14 noc, `cpu_optimizer.cpp`): menu 0x00538F90 składa
  `L"%s : %S, %i %s\n%s : %i %s"`; procesor = producent z CPUID („GenuineIntel”, 0x006C1894 kopiowany przez
  0x0060F2B0), pamięć = `GlobalMemoryStatus` (0x0060F8F0, do 4 GB → „4095 Mb”; menu bierze 16 bitów). Teraz nazwa
  z CPUID 0x80000002..4 (skrócona: bez (R), (TM), CPU, „@ …GHz”; łata `mov ebp, nazwa` na 0x00539066) i pamięć z
  `GlobalMemoryStatusEx` (najwyżej 65535 MB). Sprawdzone: pełna nazwa procesora (np. „Intel Core i…, 3600 MHz”) i pamięć ponad 4 GB.

### Płynny zoom — lupa (`SmoothZoom=1`, `ZoomStep=115`, `ZoomMax=400`; `src\view_zoom.cpp`) [Z w tle]
Silnik rysuje postacie tylko w powiększeniu 0,5 / 1 / 2 (osobne gałęzie kodu), a oddalenie do 1,0 zostawiało czarny pas
na poziomach węższych niż ekran. Teraz zoom gracza to dowolna wartość od „cały poziom wypełnia ekran” (min =
max(1, 2560/szer. poziomu, 1440/wys.)) do `ZoomMax`; silnik rysuje w 1,0 (poniżej 2,0) albo 2,0 (grafika HD, od 2,0
w górę), resztę powiększa prezenter — **tylko świat**:
- Zdjęcie świata: `ViewZoom::OnWorldDrawn` z haka `WindowManager::Draw` misji (0x00510618), zanim narysują się okna —
  bufor tylny prosto do tekstury D3D11 (`DDM::BackBits` → `Present::UploadWorld`, `UpdateSubresource` do tekstury
  DEFAULT, bez kopii na CPU). Klatki, w których `Update` nic nie narysował (`E+0xD1` i `E+0x4A94 & 0x1F`), zostawiają
  poprzednią teksturę. Tło okien dialogowych czyta ją z powrotem przez `Present::ReadWorld` (tekstura staging).
- Shader (warstwa 3): piksel klatki równy pikselowi zdjęcia = świat → próbka zdjęcia w powiększeniu (filtr wg
  `ShaderFilter`); różny = interfejs (HUD, kursor, napisy, panel F10, licznik FPS) → bez zmian, w swoim miejscu.
- Zmiana poziomu silnika bez animacji gry: zoom `E+0x475C`, indeks `E+0x4898`, kamera przez `SetCameraPosition`
  0x004DB7F0 wokół tego samego środka widoku, `E+0x48D0 = 0` (następna aktualizacja rysuje wszystko od nowa).
  Robione na tiku gry (`OnGameTick`), gdy pokazywany zoom przekroczy 2,0; kadr bierze kamerę i zoom ostatniego
  renderu (`E+0x4740`, `E+0x4760`), więc przejście jest bez skoku.
- Haki: `ZoomIn/OutRequest` 0x004DBE40 / 0x004DBDB0 (klawisze, kółko myszy, przyciski — kroki × `ZoomStep`, przystanki
  na 1,0 i 2,0), `CanZoomIn` 0x004C7CD0 i `CanZoomOut` (widescreen_patch) — przyciski HUD szare tylko na końcach
  zakresu; odświeżenie przycisków komunikatem 9 jak w `Update` (0x005490A0 / 0x005460C0 / 0x00549110, cel `[E+0x68]`).
- Myszka: `ScreenToWorld` 0x004DEE70 (16 wywołań — wybór postaci, cel marszu, kształt kursora, start ramki
  zaznaczenia) przelicza pozycję przez powiększenie; koniec ramki zaznaczenia w klatkach pośrednich też.
- Brzegi poziomu: pokazany fragment klatki ma przesunięcie (świat px). Przewijanie, którego kamera nie może już zrobić
  (`ClampScrollDelta`: chciane − dozwolone, w `timing.cpp`), przesuwa fragment; `CenterCameraOn` 0x004C7F60 (klik
  na minimapie, centrowanie na postaci) ustawia przesunięcie tak, by punkt był na środku. Fragment zawsze w obrębie
  poziomu w klatce (bez pasów).
- Minimapa: prostokąt kamery (`DrawRect` z 0x0054B3A5) przeliczany na pokazany fragment, w pełnych pikselach
  (z ułamkowymi współrzędnymi procedura gry nic nie rysowała).
- Start poziomu: 2,0 od razu (zastępuje wymuszanie z `WidescreenPatch::OnGameTick`).
- Sprawdzone w tle (2944×2176): 2,0 → 2,65 → 4,0 → 1,5 (silnik 1,0) → 1,0; interfejs zawsze 1×; klik przy 3,0 —
  Robin doszedł dokładnie w kliknięte miejsce; przewijanie do lewego brzegu — kamera 0, fragment do krawędzi;
  minimapa — mniejszy prostokąt, klik centruje; kółko myszy = kroki zoomu (gra sama mapuje kółko na zoom).
- Kółko myszy zoomuje względem kursora (kotwica: punkt świata pod kursorem zostaje pod nim). Kamera idzie za kotwicą
  przez `SetCameraPosition`; w stanie spoczynku (1) silnik kopiuje jednak powierzchnię świata bez zmian i tylko
  zapamiętuje nową kamerę — tło zostawało przy starej kamerze, a postacie, dachy i podpowiedzi rysowały się przy nowej
  („obiekty rozjeżdżają się przy zoomie, Alt+Tab naprawia” — zgłoszenie 2026-11-15). Po ruchu kamery stan → 2
  (przewijanie): silnik sam przesuwa powierzchnię świata o (kamera − kamera ostatniego renderu) i dorysowuje pasy.
- Zoom ustawiony przez grę (skrypty pokazujące coś z animacją oddalenia, wczytanie stanu): w stanie 3/4 lupa się nie
  wtrąca, a po animacji przejmuje zoom silnika (`ViewZoom: the game set the zoom … itself - followed`). Wcześniej
  lupa powiększała obraz silnika 1,0 z powrotem do starego zoomu — „rozdzielczość robi się natywna”. Test w tle:
  `lupa gameout` / `lupa gamein` = oryginalna animacja gry z pominięciem haków (jak skrypt).
- Napisy ×2 (dymki podpowiedzi, komunikaty): tryb blittera 0x28401 (klucz, alfa z drugiego obrazka, rozciąganie) przy
  skali 2,0 woła procedurę MMX 0x00622FD4 `(src, dst, alfa, szerDst, wysDst, pitchSrc, pitchDst, pitchAlfa)`, cdecl.
  Liczyła mieszanie RAZ na blok 2×2 (z lewym górnym pikselem celu) i zapisywała cały blok — pod każdą literą świat
  zamieniał się w bloki 2×2 (alfa = 0 dookoła liter). Przy lupie te piksele różniły się od zdjęcia świata, więc szły
  jako interfejs: „kropkowane” prostokąty i „stara mapa” pod napisem. Hak liczy to samo dla każdego piksela celu
  (waga = 2 × niebieski z obrazka alfa, z 64; maski kanałów z 0x006C11C8); piksele z alfą 0 zostają nietknięte.
  Analogicznie tryb 0x28201 (0x005E8B10, pierścienie, światła bez klatki HD) przy skali 2,0 — per piksel.
- Zdjęcia porównawcze i pomiary: `tools\out\tooltip_fixed_x2.png` (po), `tooltip_zoom.png` (przed).
- **Reguła sąsiadów** (2026-11-15, zgłoszenie: plamy w ciemnym dole paska przy zoomie i w menu): piksel
  półprzezroczystego obrazka interfejsu bywa przypadkiem równy światu (czarny zostaje czarny po przyciemnieniu) — sam
  wśród pikseli interfejsu pokazywał kropkę powiększonego świata. W shaderze (`WorldAt`) piksel liczy się jako świat tylko,
  gdy co najwyżej 4 z 8 sąsiadów nie są światem. To samo dla warstwy 1 (obrazy HD menu, `PictureAt`) i 2 (tło okna
  dialogowego, `BackAt`). Koszt: 18 odczytów tekstur na piksel — dla karty bez znaczenia.
- **Panel F10** przy powiększeniu (lupa albo powiększone tło okna dialogowego) ma jednolite ciemne tło: jego „obraz
  pod spodem” z klatki gry jest w skali silnika i prześwitywał w innej skali niż wokół (zgłoszenie 2026-11-15). Licznik
  FPS w czasie pauzy (menu Esc) pokazuje klatki samej gry, która rysuje tylko przy zmianach — „8 FPS” na zrzucie
  użytkownika to nie spadek obrazu (log: ~135–140 pokazanych kl./s).
- **Otwieranie menu Esc przy lupie** (zgłoszenie „lekki lag, zwiecha”): powiększone przyciemnione tło liczyło się na
  procesorze w liczbach zmiennoprzecinkowych — ~150–170 ms. Teraz tablica przyciemnionych kolorów (65 536 pozycji), wagi
  całkowite liczone raz na kolumnę/wiersz, 4 wątki: 15 ms; razem z kopią świata (16 ms) i analizą (~25 ms) ~55 ms zamiast
  ~200 ms (log: `HD menu: dialog start … (N ms)`, `… magnified xM … (N ms)`).
  **v1.1.5:** pomiar całej przerwy między klatkami (log `Present: N ms between two frames … (HD menu patches M ms)`,
  ślad rysowania z czasem `@ms` w każdej linii) pokazał ~390 ms przy otwarciu menu — 290 ms z tego to procedura gry
  `0x0053B330` (cdecl: id powierzchni, odcień, współczynnik), która barwi tło okna na zielono: na każdy piksel RGB565 →
  liczby (czerwony ×2/32, zielony /64, niebieski /32, × współczynnik), `0x0053CBC0` (RGB→HSV), `0x0053CA30` (HSV→RGB
  z nowym odcieniem), trzy `_ftol`. Hak liczy tablicę 65 536 kolorów raz na (odcień, współczynnik) tymi samymi
  krokami (pomocnicze funkcje gry, x87 jak w oryginale) i mapuje powierzchnię: **3 ms**, sprawdzone w trybie testowym
  z procedurą gry — 0 różnych pikseli na 3 686 400. Analiza tła tylko w pokazywanej części (reszta co 4. piksel do
  nauki barw): 10–15 ms zamiast ~30. Menu Esc: **~70–90 ms zamiast ~390 ms** (resztę zajmuje gra: fonty okna z plików).
- **Filtr powiększenia** (2026-11-16, v1.1.7; zgłoszenie: „na oddaleniu robi się blur… na murze, śwince, płaszczu
  Robina”): poniżej zoomu 2 silnik rysuje świat w 1,0 (bez grafiki HD), a lupa powiększała go dwuliniowo. Dla
  powiększenia 1 < m < 2 (także zoom 2–4 na klatce HD) shader bierze Catmull-Rom z 16 tekseli (`WorldMagnified`,
  przycięte przeregulowanie); m ≥ 2 jak dotąd. Porównanie `hd\compare\lupa15_bilinear_vs_catrom.png`. ✔ Prawdziwe HD
  poniżej zoomu 2: zrobione w 1.3.0 jako warstwa poziomu (§5h); sprite'y zostają 1× (powiększone).
- **Linia zamachu — przyczyna i poprawka** (2026-11-17, v1.2.0; youtuber: „rysowanie walki zależy od zoomu”). Gest:
  przytrzymany lewy przycisk = flaga wskaźnika `0x40` (`Pointer::Update 0x00628210`: 1 = kliknięcie przy puszczeniu,
  8 = podwójne, 0x40 = trzymany; prawy 2/0x10/0x80) → menedżer zdarzeń `[0x6C0C4C]` (= `RHEngine+0x68`) zaczyna zapis
  (`+0x1F`, czyszczenie listy `+0x5C`), każdy ruch dopisuje pozycję ekranową (`+0x84`) — tylko gdy tryb kursora `+0xA2`
  = 0 i walczy zaznaczona postać (`0x004E1890`: ktoś z listy `E+0x49F8` ma przeciwnika `+0x268`); maks. 350 punktów,
  intensywność 112,5 na punkt (`+0x2C`). Ślad rysuje silnik na końcu renderowania obiektów (`0x004D0A10` →
  `0x00548800`: blokada powierzchni 0 = bufor tylny, pędzel `0x00548480` grubości ∝ intensywności) — RAZEM ZE ŚWIATEM,
  więc lupa powiększała go i przesuwała. Rozpoznanie przy puszczeniu (`0x004DE290` → `0x004E1A40` → `0x00547600`)
  porównuje kształt z pozycją walczącego z WorldToScreen (piksele klatki). Hak (`ui_scale.cpp` `TrailInFrame`): na czas
  `0x00548800` i `0x00547600` punkty listy przeliczone `ViewZoom::ScreenToFrame`, potem przywrócone. W teście w tle
  procedura śladu działa (blokady powierzchni), ale pędzel nic nie rysuje przy wstrzykiwanym ruchu — potwierdzenie na
  prawdziwej myszy u gracza.
- **Żywopłot na dole** (2026-11-17, v1.2.0; youtuber: „rośliny z portretami mają dziurę w środku i przy oddaleniu
  zanikają”). Pasek = lewy koniec 320×165, prawy koniec, środek 160×110 raz na postać drużyny; w oryginale pas 80 px pod
  światem był czarny. Z `FullHeightView=1` dziura i prześwity liści pokazywały świat, a lupa w prześwitach pokazywała
  świat powiększony (piksel klatki = kopia świata). Teraz (`ddm.cpp` `HedgeStrip`, gałąź powiększonego interfejsu): przy
  lewym końcu pas 80·2 px czarny na całą szerokość + środek (kopia z poprzedniej klatki) od lewego do prawego końca;
  prezenter nie traktuje tego pasa jako świata (`DDM::TakeHudBandTop` → `lupaWorldH`; wcześniej czarne piksele pasa
  trafiały na czarną ziemię i pokazywały pomarańczowe smugi). Portrety: decyzja „każdy od swojego miejsca / cały rząd od
  rogu” trzymana dla tej samej liczby okien klasy (zaznaczony portret jest szerszy — rząd przeskakiwał).
- **Okno mniejsze niż ekran** (2026-11-17, v1.2.0; prośba youtubera): `WindowWidth/WindowHeight` (0 = cały monitor),
  `WindowX/WindowY` (-1 = środek); `window_manager.cpp`: `g_monitorWidth/Height` = rozmiar okna, `g_winX/g_winY` = jego
  miejsce (tworzenie, `SetWindowPos`, `MoveWindow`, `ApplyBorderless`, `ClipCursor`). Sprawdzone w tle: 1600×900 na środku
  monitora 2560×1440, prezenter rysuje 1600×900.
- **Okna dialogowe otwierane z okna misji — „zamarzanie” w obozie** (2026-11-17, v1.2.1; zrzut youtubera 3.jpg:
  kwadraty pergaminu za kursorem, rozmazany świat z prawej). Odtworzone w tle komendą `win` (konsola gry: singleton
  `0x0045BBE0` → `[0x6C0808]`, komenda WIN `0x0045CDA0` thiscall(console,int,int) ret 8; pierwsze pole konsoli = obiekt
  gry od startu misji `0x0050F850`): po 1. i 2. misji obóz Sherwood (poziom 1920×1088), mapa kampanii, „Chcesz
  rozpocząć wybraną misję?”. Przyczyny i poprawki:
  - Pytanie otwiera okno misji (klasa `0x006796D4`, samo rysuje pergamin 400×200) z wnętrza swojego Draw, czyli w
    trakcie `WindowManager::Draw` misji (powiększanie ×2 włączone): `ShowDialog 0x0053BB80` (thiscall(manager, okno,
    pętla, tło), ret 0xC, jedyne wywołanie z `DoModal 0x00544B80`) — hak `Hook_ShowDialog` odkłada stan powiększania
    (`g_drawDepth`, `g_curWindow`, kotwica) na czas okna. Wcześniej każdy blit okna rósł ×2 wokół środka, a
    przywracanie tła spod kursora (zapis 1:1, przywrócenie przez `0x005F1A32`) lądowało ×2 gdzie indziej — kursor
    zostawiał kwadraty. Dodatkowo `UiScale::Drawing()` jest fałszywe przy działającej pętli okna.
  - Konstruktor menedżera okna (`0x0053B6B0`) renderuje misję raz na tło: `RenderFrame(0,0)` z `0x0053B6CD` (gra woła
    `RenderFrame` tylko stąd i z tiku `0x0050FF05`). `Timing::InBackdropRender()`: bez skutków „klatki misji”
    (`g_missionFramed`, wyłączanie przybliżenia menu, kopia świata lupy, przeciągana mapa); okna ze środkowej komórki
    siatki są w nim pomijane (puste prostokąty w `Transform`) — nie ma już pergaminu ×2 wystającego spod okna.
  - Poziom do tła okna (`NoteDialogStart`) bierze się z tej samej klatki co kopia świata lupy: okno otwarte z okna
    misji → tylny bufor w `Hook_ShowDialog` (przód trzymał starszą klatkę pośrednią — animowana woda, korony drzew i
    krzaki różniły się, tło było sklejone ze świata powiększonego i niepowiększonego); po renderze tła też tylny bufor.
  - Na poziomie mniejszym niż widok przy zoomie 1,0 silnik rysuje tylko poziom, reszta klatki zostawała z klatek przy
    zoomie 2 (powiększony strumień z prawej). `ViewZoom::ClearOutsideLevel()` czyści ją na czarno po narysowaniu świata
    (`DDM::FillBack`), w każdej klatce misji.
  - Przyciemnione tło: zamiast zgadywania koloru przyciemnienia (głosowanie — rzadkie kolory spod pergaminu dawały
    kropki w kształcie postaci na 2. stronie podsumowania) dokładna tablica ostatniego barwienia gry
    (`HD::LastBackdropColouring`, ta sama co przy szybkim menu Esc) albo „bez zmiany”, gdy któraś pasuje do ≥ 30 % tła.
  - Podświetlenie pieczęci V/X pod kursorem to osobny obrazek rysowany tą samą procedurą (`0x00622550`); brakowało jego
    wersji HD. Zebrane obrazki z kampanii (pieczęcie, portrety odpraw 158×154, pergamin pytania, mapa kampanii
    629×480, znaczniki i podświetlony zamek po 3. misji) powiększone `upscale_menu_parts.py` — 53 pliki `.hdp`
    (sprawdzone w grze: mapa kampanii 1258×960, pergamin pytania, portrety, pieczęcie). Zbieranie: bez animacji (ta sama
    szerokość, inna wysokość w ciągu 2 s — napisy końcowe dały 5160 plików / 5,5 GB, przeniesione do
    `hd\work\menu_parts_napisy_koncowe\`) i najwyżej 300 na sesję.
- **Portrety, dymek umiejętności, wczytywanie** (2026-11-17, v1.2.2; drugie zgłoszenie youtubera):
  - Portrety zaznaczonych postaci (klasa okna `0x0067A348`, 112×131 przy dolnej krawędzi): gra rozkłada N portretów
    równo na całą szerokość (5 postaci: x = 225, 724, 1223, 1722, 2221). W trybie „każde okno od siebie” rosły od
    tercji ekranu (lewa krawędź / własny środek / prawa krawędź) — nierówne odstępy, dwa ostatnie stykały się. Teraz
    przy dolnej krawędzi każdy rośnie od własnego środka (`PerWindowClass`), trzymany na ekranie. Przy okazji błąd od
    1.0.0 w `CollectWindows`: nowa grupa klasy dostawała tylko klasę i prostokąt — licznik okien, najmniejszy odstęp i
    najszersze okno zostawały z poprzednich klatek (licznik rósł bez końca, więc ochrona decyzji z 1.2.0 nie działała).
  - Zapamiętana kotwica okna (`KeptAnchor`) tylko dla minimapy (gracz ją przeciąga). Dymek przy kursorze (klasa
    `0x00678D18`, gra przestawia go za kursorem) brał kotwicę z pierwszego pokazania i rysował się gdzie indziej albo
    poza ekranem („migają albo z przesunięciem”); teraz rośnie w prawo i w dół od rogu przy kursorze, jak w grze.
  - Wczytywanie zapisu: gra wypełnia cały tylny bufor czernią (`FILL d=(0,0,2560,1440)`) i pokazuje czarne klatki.
    Warstwa HD menu zostawała z obrazem ekranu wczytywania i pokazywała go tam, gdzie czarny piksel równał się
    pikselowi starego obrazu (poszarpany czarny prostokąt). `HD::Menu::OnScreenFill`: pełne wypełnienie ekranu
    kończy obraz menu.
  - Klawisz Windows: `SetCooperativeLevel` klawiatury z `0x16`/`0x15` (`DISCL_NOWINKEY`) w `0x005FA376`/`0x005FA5FE` →
    `0x06`/`0x05` (`WindowsKey=1`, `input_fix.cpp`).
  - OBS: flagi zgodności GOG dla `game.exe` (HKLM/HKCU `AppCompatFlags\Layers`: `DWM8And16BitMitigation
    8And16BitAggregateBlts RUNASADMIN HIGHDPIAWARE`) — gra działa jako administrator, OBS bez uprawnień nie wstrzykuje
    przechwytywania gry. Rozwiązanie po stronie gracza (README): przechwytywanie okna „Windows 10 (1903+)” albo OBS
    jako administrator. (1.2.5: u youtubera przechwytywanie okna też nie działało, a skróty OBS tylko po Alt+Tab —
    README zaleca już tylko OBS jako administrator, niżej.)
- **Tło pytań w obozie, pierwsza odprawa, portrety, mapa kampanii** (2026-11-17, v1.2.3; trzecie zgłoszenie youtubera,
  zrzuty 2.1–2.5 z 1.2.1, sprawdzone w tle na jego zapisach — kopia w `tools\out\yt_savegame_2026-11-17\`, profil po
  teście w `backups\yt_profile_004_after_test_2026-11-17\`; w jego obozie V „Wyślij wybranych bohaterów na misję” jest
  dopiero, gdy Robin w drużynie ma ✓ — zapis „Sherwood” ma taki stan):
  - „Krzaczki się rozpadają, obrazy nachodzą” (2.2): tło okna = render `RenderFrame(0,0)` z konstruktora menedżera okna
    `0x0053B6B0`, wywołany z Draw okna misji, czyli przy `g_drawDepth = 1` — korony drzew (`BlendBlit 0x005EC5F0`, 40 %)
    szły przez `Transform` jak obrazki okna (przesunięte ×2 albo wycięte jako „środkowa komórka”). Do tego okno otwiera
    się czasem w naszej klatce pośredniej: render tła nie był rozpoznawany (`!g_inExtraFrame`), a cały dialog działał
    jako klatka pośrednia (zamrożone `AnimAdvance`, przesunięte obiekty `MotionSmooth`). Teraz: `UiScale::Begin/
    EndBackdropRender` odkłada i przywraca cały stan rysowania okien (głębokość, okno, kotwice, `g_pieces`, grupy,
    minimapa); w klatce pośredniej `MotionSmooth::Restore()` i `g_inExtraFrame = false` przed renderem tła (log
    „a window opened a dialog during an extra frame”). Kopia świata do tła lupy pochodzi z TEGO SAMEGO renderu
    (`ViewZoom::OnBackdropWorldDrawn` w haku Draw `0x00510618` przy `InBackdropRender`, numer renderu =
    `Timing::BackdropRenderCount()`; `NoteDialogStart` bierze ją przez `BackdropWorld`) — poziom i świat równe piksel w
    piksel poza oknami. Pas żywopłotu (`DDM::LastHudBandTop`) nie należy do świata powiększanego w tle.
  - Czarny pas obok pierwszej odprawy (2.1) i mignięcie pierwszej klatki poziomu: `ViewZoom::OnGameTick` szedł na końcu
    tiku, po pierwszym renderze nowego poziomu (silnik jeszcze przy zoomie 1,0, poziom 1920 px na ekranie 2560).
    `ViewZoom::PrepareLevel()` przed renderem tiku i przed renderem tła — start poziomu przed pierwszą klatką. Hak
    `RenderFrame` (`timing.cpp`) jest teraz zakładany także przy `SmoothMotion=0` (wcześniej bez niego znikały poprawki
    tła okien z 1.2.1).
  - Portrety (2.4): ukryte okna portretów w obozie (`+0x5C` = pokazane) leżały stosem w x=225 i liczyły się do grupy
    (odstęp 0 → cały rząd od rogu, część poza ekranem: „4 zaznaczone, 3 portrety”) — `CollectWindows` je pomija.
    Rząd przy dolnej krawędzi: miejsca gry (225 + 499·i przy 2560) ściśnięte współczynnikiem min(1, 2048/szer.) od
    pierwszego portretu — krok ~400 px, równy rząd bez przerw między powiększonymi portretami.
  - Wczytywanie (2.5): zapis z menu Esc i start misji — czarne klatki gry, bez warstwy menu (sprawdzone śladem: start
    misji wypełnia bufor czernią `FILL (0,0,2560,1440)` → `OnScreenFill` z 1.2.2).
  - Mapa kampanii w HD w każdym stanie kampanii: plik `DATA\Interface\Default.res` („SRES”: wpisy `BTTN`/`PICC`/
    `PIC `/`CUR `…, obrazki `u16 w, u16 h, u32 format (0 = RGB565, 1 = zlib), u32 rozmiar`) trzyma wszystkie obrazki
    menu jako RGB565 — klucz moda (`Fnv` w `ddm_hd_patch.cpp`) da się policzyć bez gry: `tools\hd\res_pictures.py`
    (58 z 63 zebranych przez grę obrazków ma klucz z pliku), `--write --sizes …` zapisuje je jako `part_*.raw` dla
    `upscale_menu_parts.py`; przegląd grup: `tools\hd\res_sheet.py`, struktura: `tools\hd\res_parse.py`. Dodane: 5 zamków
    × 4 (szkic + 3 klatki poświaty), 12 znaczników ścieżek, 10 strzałek wojsk szeryfa, czerwona flaga (niebieskiej
    flagi nie ma w pliku jako osobnego obrazka). W pliku jest 1126 obrazków — resztę zrobiła v1.2.4 (niżej).
- **Interfejs misji z obrazków HD** (2026-11-17, v1.2.4; „Tak, zrób” + „te małe też”): wszystkie obrazki z
  `DATA\Interface\Default.res` o bokach ≥ 4 (bez 6 pełnoekranowych teł menu, które mają własne HD) powiększone
  `upscale_menu_parts.py` (sgi; 631 + 216 nowych, razem 939 plików `.hdp`, 89 MB; małe obrazki: korekta kolorów
  liczona na wersji z ramką, bo jej rozmycie potrzebuje ≥ 6 pikseli). Interfejs ×2 rysuje obrazki przez
  `SmoothPicture2x16` (`ddm_blit.cpp`, pamięć podręczna powiększeń): przy budowie wpisu `BuildFromHd` pyta
  `HD::Patch::InterfacePicture` (klucz treści `Fnv` jak w menu, `LoadPicture` z pamięcią odpowiedzi) i robi przebiegi
  wpisu z obrazka HD — pełne piksele (alfa ≥ ~248) jako kolor, brzeg jako sumy z pokryciem w szesnastkach, sam cień
  jako przyciemnienie do 50 %. Tylko dla klucza koloru 0x07C0 albo rysowania bez klucza i obrazka bez przezroczystości.
  Obrazki zmienne (zdrowie, liczby) dostają nowy klucz — bez wersji HD zostaje filtr. Polecenie testowe `patch`
  pokazuje `ui=znalezione/zapytane`. W obozie: 18/18 obrazków interfejsu z HD, żywopłot bez szwów.
- **Tło okna wczytywania po wczytaniu gry** (2026-11-17, v1.2.4; błąd od 1.2.2): `OnScreenFill` (pełne wypełnienie
  ekranu przy wczytywaniu innego poziomu) zeruje płótna i kopię źródła `g_srcCrop`, ale skrót „ten sam obrazek co
  ostatnio — tylko zmienione piksele” w `OnScreenCopy` (statyczne `lastPic`/`lastBase`) zostawał: czarne otoczenie
  obrazka menu równało się wyzerowanej kopii, więc wypełniał się tylko środek — remake okna wczytywania / zapisu był
  widoczny tylko w ramce 1024×485. Teraz `g_fullRefill` po `OnScreenFill` = pełne wypełnienie płócien przy następnej
  kopii. Sprawdzone: Lincoln → wczytanie obozu → okno wczytywania na cały ekran.
- **Pasy przy wczytywaniu poziomów** (2026-11-17, pytanie użytkownika „czy wszystkie mapy wczytują się bez pasa”):
  pas może dać tylko poziom węższy / niższy od ekranu przy zoomie 1 — obóz Sherwood 1920×1088, Derby 1920×2752,
  Nottingham 2304×3520, rozstaje 1408×960 i 1792×1152 (Leicester, Lincoln i York zawsze wypełniają ekran; rozmiary z
  nagłówków `.map`). `tools\check_level_bands.ps1` wczytuje po kolei zapisy z listy (`-Scroll N` = starsze) i mierzy
  czarny pasek przy prawej krawędzi i nad żywopłotem: na zapisach youtubera (cała kampania) bez pasa po wczytaniu.
- **Czwarte zgłoszenie youtubera** (2026-11-18, v1.2.5; film z 1.2.4, jego log i zapisy w
  `tools\out\zgloszenie_yt_2026-11-18\`, automatyczne napisy `transcript.txt` do szukania miejsc w filmie):
  - Czarny pas z prawej przy pierwszej odprawie (5:04): krok kółka myszy sprzed poziomu (`g_wheel`, zebrany w menu albo
    przy wczytywaniu) wykonywał się na starcie poziomu i lupa oddalała poziom 1792×1152 poniżej „poziom wypełnia
    ekran”. `ViewZoom::PrepareLevel()` wyrzuca kroki kółka sprzed poziomu (log „mouse wheel step(s) from before the
    level ignored”).
  - Ucięty dół przy oddaleniu (7:40, „dolny krzaczek”): z `FullHeightView=1` kamera gry kończyła się na dole poziomu
    (`0x677488` = 0 w sześciu miejscach ograniczenia kamery: `CenterCameraOn` `0x4C808C`/`0x4C80B5`, `ClampScrollDelta`
    `0x4CED05`/`0x4CED62`, ograniczenie `0x5758D0` w `0x575A11`/`0x575A58`), więc ostatnie 160 px poziomu (interfejs ×2)
    leżało pod żywopłotem. Teraz te miejsca czytają `g_cameraStrip` = 80 · współczynnik interfejsu
    (`WidescreenPatch::CameraStrip()`, ustawiany w `OnGameTick`; bez `FullHeightView` zostaje 80 gry) — dół poziomu
    kończy się na górze żywopłotu; lupa (`OnWorldDrawn`, `OnBackdropWorldDrawn`) liczy `m ≥ (H − pas) / dół treści` i
    `ClampShownY` z tym samym pasem. Sprawdzone w tle: zoom 2 — kamera y do 512 (było 432); oddalenie — wycinek (0,256),
    dół poziomu na górze żywopłotu. Ciemne drzewo „w kratkę” przy dole poziomu rysuje sama gra (tak samo z F8).
  - Walka (18:44, 20:25: gesty nie działają, zwykłe ciosy wymagają dużo machania, obszarowy działa): rozpoznawanie
    `0x00547600` (z `0x004E1A40`; pozycja walczącego `0x004DEEB0`, kierunek `0x004135B0`) sprawdza kształt — koło
    `0x00547F00`, krzywizna 0,3 `0x00548050`, kąty ±π/4 i ±3π/4 `0x00548100`, `0x00548280` — bez progów w pikselach poza
    10 px dla ≤ 2 punktów, więc skala nie zmienia wyniku, a punkty idą przez `ScreenToFrame` (v1.2.0). Mod nie jest
    udowodnioną przyczyną. Log „Swing: N points, WxH px, drawn L px … fighter at (x,y) -> result R” (pierwsze 60
    zamachów sesji, `Hook_SwingRecognise` w `ui_scale.cpp`) pozwoli porównać z grą bez moda. Instrukcja s. 27: linie
    względem walczącego (przód / tył / lewo / prawo), półkola, koło (obrót), ∞ (cios kończący).
  - OBS — ciąg dalszy (v1.2.6, test na OBS 33 u użytkownika; gra i OBS bez uprawnień administratora, gra w oknie 1280×720
    przez `gamectl.ps1 -Start -Quiet`, tymczasowa kolekcja scen z „Przechwytywaniem gry” na `Robin Hood - Legend Of
    Sherwood:SBDRAW:Game.exe`, wynik z dziennika OBS i nagrania): przechwytywanie gry działa („d3d11 shared texture capture
    successful”, nagranie z obrazem intro). Przeszkody: (1) gra pauzuje, gdy jej okno traci fokus (`WindowManager: game
    window deactivated`, pętla gry staje w `GetMessage`) — gdy OBS jest na wierzchu, podgląd/nagranie są czarne;
    (2) „hak zgodności z anty-cheatem” (domyślny) wstrzykuje przez `SetWindowsHookEx(WH_GETMESSAGE)` i budzi wątek
    `PostThreadMessage` — pętla gry (`0x005E1380`: `PeekMessageA(&msg, okno gry, 0, 0, PM_REMOVE)`, jeden komunikat na
    obieg; `0x005E1400` `GetMessageA(…, okno gry, …)`) nie odbiera komunikatów wątku, więc hak łapie dopiero przy
    komunikacie okna (w testach: raz po 2 s, raz wcale przez 25 s intra); bez niego (wstrzyknięcie bezpośrednie) — od razu
    w każdej próbie; (3) tryb SLI/Crossfire („memory capture”) działa — na różne karty. Prezenter: karta z DXGI 1.6
    `EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`, karta monitora gry, gdy to ta sama / ten sam model (u użytkownika
    RTX 3080 Ti widziany dwa razy — zdalny pulpit), log kart. Ustawienia OBS użytkownika przywrócone z
    `backups\obs_config_before_test_2026-11-18\`.
  - OBS (21:55: skróty OBS dopiero po Alt+Tab; przechwytywanie gry i okna czarne, działa tylko ekran): gra działa jako
    administrator (`RUNASADMIN` od instalatora GOG), a Windows (UIPI) nie pozwala programowi bez uprawnień czytać
    klawiszy, gdy na wierzchu jest okno z wyższymi uprawnieniami, ani wstrzyknąć przechwytywania. Rozwiązanie: OBS jako
    administrator. Log mówi na starcie „Process: runs as administrator …” (`dllmain.cpp`, `TokenElevation`).
  - Klawisze szybkiego zapisu i odczytu (prośba użytkownika, „z możliwością usunięcia przycisku”): `QuickSaveKey` /
    `QuickLoadKey` w `[Input]` (puste = jak w opcjach gry, `none` = brak, nazwa klawisza albo kod DirectInput `0x..`)
    i panel F10. Ustawienia klawiszy gry: 28 kodów DirectInput (u16) w obiekcie `[0x006C0B54] + 0x14C` w kolejności
    ekranu Opcje → Klawiatura; 26 = szybki zapis (F1), 27 = szybki odczyt (F5) → komendy 11 / 12 (`0x00512840` →
    `0x00512CB0` / `0x00512EB0`). `InputFix::Poll` wpisuje klawisze moda w czasie poziomu; pętla okna gry
    (`Hook_DialogLoop` → `InputFix::OnDialogLoop`) i zamknięcie przywracają wartości gry, więc jej ekran klawiatury
    pokazuje i zapisuje własne. Plik `DATA\Savegame\Profiles` (dwa zestawy po 76 B, klawisze od 0x4E i 0x9A) ma po
    szybkim zapisie i odczycie dalej F1/F5 gry. Panel pomija klawisze moda (F6–F10), drugi klawisz szybki i klawisze
    innych akcji gry; kolizję wpisaną w INI log ostrzega (`InputFix::QuickKeyClash`). Sprawdzone w tle: F9 = zapis, F1 i
    F5 nic; w menu Esc F1/F5 gry, po zamknięciu znowu F9/brak; F11 zapis, F1 odczyt (obraz wraca do stanu z zapisu).
  - Minimapa — wielkość i obraz (prośby użytkownika: „wybór wielkości minimapy i upscale”, „3 wielkości i klasyk”,
    „x5”, „klasyczna / klasyczna upscalowana / nowa / nowa z efektem pergaminu, domyślnie pergamin”, „gotowe pliki, bez
    generowania w grze”):
    - Obraz otwartej mapy NIE powstaje w grze: każdy poziom ma gotowy obrazek całego zwoju w
      `DATA\Levels\<Day|Night|Fog|Custom1>\<poziom>.min` (u16 w, u16 h, u32 format 2 = bzip2, u32 rozmiar, RGB565;
      158×246, 168×246 albo 225×165..195; 20 różnych, mgła rozstajów = dzień, `Custom1\Nottingham` = dzień). To tło
      poziomu zmniejszone w prostokąt okna minus marginesy (14, 24) — ten sam, w który gra rzutuje kamerę i znaczniki
      (w obrazku poziom leży 1 px w lewo i w dół) — przyciemnione „na pergamin” i wtopione w papier na brzegach. Wyjątki:
      mgła = bezbarwny szkic, obóz Sherwood = inny obraz (las ze ścieżką, nie obóz). `tools\hd\minimap_pictures.py`
      (klucze `Fnv` jak w interfejsie, `--write` → `part_*.raw`, `--png`).
    - Pliki: `hd\menu_parts\<klucz>.hdp` — obrazek gry ×2 z AI (sgi); `hd\minimap\clean|parchment\<klucz>.hdp` ×4 z
      `tools\hd\make_minimaps.py`: tło HD poziomu (`hd\maps_hd\dat2\`) zmniejszone w prostokąt mapy, zwój i brzegi z
      obrazka gry ×4 z AI (`upscale_menu_parts.py --scale 4 --out hd\work\minimap_x4`), wtopienie na 5 px obrazka;
      `parchment` = kolory obrazka gry (przekształcenie Monge-Kantorovicha średniej i kowariancji, potem kwantyle
      kanałów — najmniejsze kwadraty spłaszczały kontrast mgły), lekko „malowany” (`edgePreservingFilter`) i z fakturą
      papieru; mgła: kolory szkicu z samej jasności tła (sepia, bez kolorowych plam), obóz: kolory rozstajów / Leicester
      nocą. 40 plików, 125 MB (69 MB w ZIP-ie). Obrazy gry powiększa model dat2, nie sgi interfejsu (mapy to malowane
      obrazy — sgi robił z liści „robaczki”, `tools\out\minimap_sgi_vs_dat2_x2.png`); całość od zera:
      `tools\hd\make_all_minimaps.ps1`.
    - Obóz Sherwood (prośba użytkownika: „inne tło mogło być celowo — niech obóz dostaje klasyczny widok”): jego obrazek
      gry (`Day|Night\SHERWOOD.MIN`, klucze `26c1a218aa02cd8f` / `5b3ced48c047e7f4`) to leśna ścieżka, nie obóz.
      `MinimapCamp=0` (domyślnie, tylko INI): ten obrazek ×4 z dat2 z `hd\minimap\classic\` (`MapPicture` sprawdza ten
      folder najpierw — trzyma tylko obrazki, dla których zostaje obraz gry), 1 = jak inne poziomy (prawdziwy obóz z tła
      HD). Sprawdzone w tle (stan „Kontynuuj w obozie”): w obozie gra minimapy nie pokazuje — w rogu jest mapa kampanii,
      klik w miejsce przycisku zwoju nic nie otwiera; mapę obozu używają tylko `DATA\Levels\Sherwood.rhm` (obóz) i
      `SherwoodOutro.rhm` (zakończenie), więc obrazek leśnej ścieżki jest pewnie nieużywany — dlatego opcja nie ma
      miejsca w panelu F10. GPT / remake obrazu możliwy później jak tła menu.
    - Wielkość (`MapScale`, 150..500 %, panel: 2×, 2,5×, 3×, 5×, 1,5×): w `UiScale::Transform` otwarta mapa ma własny
      współczynnik (`g_mapFactor`; wysoka mapa przy 5× tyle, ile mieści się nad żywopłotem: ~4,36×, `g_mapFactorUsed`),
      kotwica w rogu z ×2 marginesu przycisków (`x0 = floor(W − 8·2 − f·w)`, `y0 = 104·2`), `Piece` z czynnikiem
      (mysz cofana czynnikiem obrazka pod nią; dymek po mapie dostaje kotwicę równoważną dla ×2), zapamiętana kotwica
      z czynnikiem, przeciąganie okna (`FollowMapDrag`) i obramowanie kamery (`DrawRect`, grubość = czynnik) — tym samym.
      `ScaleRect` dla mapy zaokrągla róg i rozmiar osobno (ten sam rozmiar przy przeciąganiu).
    - Rysowanie: blity okna mapy (`UiScale::LastScaledWasMap`) idą przez `DDM::ScaledPicture16` (`ddm_blit.cpp`) —
      skalowanie do dowolnego rozmiaru filtrem namiotowym o szerokości zmniejszenia, w kolorze mnożonym przez pokrycie,
      z pamięcią jak `SmoothPicture2x16` (wpis na obrazek i rozmiar: przebiegi pełnych kolorów i miękkiego brzegu).
      Źródło: `HD::Patch::MapPicture` (`MinimapHD`: 3 parchment → clean → AI ×2; 2 clean → parchment → AI; 1 AI ×2
      (`InterfacePicture`, gdy `HDMenu=1`); 0 piksele gry). Zmiana w panelu → `DDM::DropScaledPictures()`. Koszt: plik
      czytany raz przy pierwszym otwarciu mapy na poziomie, skalowanie raz na rozmiar (~20 ms przy 5×), potem kopiowanie
      jak reszta interfejsu.
    - Sprawdzone w tle (rozstaje 1792×1152, zapisy youtubera): wszystkie 5 wielkości i 4 obrazy, znaczniki i obramowanie
      kamery na miejscach; przy zoomie lupy 3 obramowanie = widoczna część nad żywopłotem (47 % × 37 %); klik w rogi mapy
      przy 3× i 5× przesuwa widok do rogów (crop 0,0 / 853,427), przeciąganie po mapie przy 5× też. Porównania:
      `tools\out\minimapa_obrazy.png`, `minimapa_wielkosci.png`, `minimapa_inne_mapy.png`.
- **Gesty miecza — przyczyna i poprawka** (2026-11-19, v1.2.7; piąte zgłoszenie youtubera: nagrania z modem i bez, na
  manekinie treningowym, log 1.2.6 — prawie każdy zamach `-> result 1`, także proste kreski 1129×49 px czy 29×481 px).
  - Przyczyna: pętla główna (`0x0050FB33`) daje tłumaczowi jedno zdarzenie ruchu wskaźnika na obieg, a z klatkami
    pośrednimi moda obiegów jest kilka na tick, pozycja w zdarzeniu zmienia się tylko z tickiem. Gałąź ruchu
    (`0x00546A3D`) dopisuje do śladu (`tr+0x5C`) poprzednią pozycję `[tr+0x84]` i jasność 112,5 (`tr+0x2C`), więc ten
    sam punkt trafiał tam 4–5 razy z rzędu. Test przecięcia gry (`0x006107E0`: czy dwa niesąsiednie odcinki się
    spotykają; `0x0060A990` liczy styk końców jako przecięcie) widział, że sąsiedzi odcinka zerowej długości się
    stykają, uznawał kreskę za pętlę i rozpoznawanie (`0x00547600`) dawało zwykły cios (1) każdej linii. Bez moda jest
    jeden obieg na tick i powtórzeń nie ma.
  - Poprawka (`ui_scale.cpp` `TrailAppendStub` na `0x00546A68`): punkt równy ostatniemu w śladzie nie jest dopisywany
    (ani jego jasność) — lista jak w grze bez moda; ostatni punkt dostaje świeżą jasność, którą przyniosłoby powtórzenie
    (smuga rysowana z jasnością przechodzącą od punktu do punktu, `0x00548800`), więc smuga i jej płynne wygasanie są
    jak wcześniej. Log: `Swing: N points (R repeated in a row) … crossing C … -> result X. Repeats left out so far: K`,
    punkty pierwszych 10 zamachów (`Swing n points:`), żeby kreskę z logu gracza dało się odtworzyć.
  - Sprawdzone w tle (`tools\swing_test.ps1`: Lincoln, walka ze strażnikiem, kreski w prawo / lewo / górę / dół):
    przed — 55 punktów, 38 powtórzeń, przecięcie 1, wynik 1 dla każdej kreski; po — 14–17 punktów, 0 powtórzeń,
    przecięcie 0, wyniki 2 / 3 / 5 / 6 (kierunek liczony względem tego, gdzie patrzy walczący); smuga widoczna za
    kursorem w trakcie kreski (`moveshot` w harnessie). Także przy zoomie (`-Wheel n`, krok kółka po kroku): 2,3× i
    maksymalne 4× (wszystkie punkty przez `ScreenToFrame`, wyniki 2 / 3 / 5), maksymalne oddalenie (zoom gry 1,0,
    wyniki 5 / 5) — smuga zawsze tuż za kursorem (`tools\out\gesty_zoom_1.2.7.png`). Obraz w walce: `Present=143.9Hz`
    przy 25 tickach/s, jak w 1.2.6 u youtubera (144 Hz) — poprawka zmienia tylko listę punktów gestu.
- **Szóste zgłoszenie youtubera** (2026-11-21, 1.2.7; trzy zrzuty, bez logu i zapisów):
  - Pasek przy pytaniu w Nottingham (Marian, „Nagrodę? Ryzykujesz tyle dla srebrnej strzały?”): z prawej, na całej
    wysokości, poziom NIEprzyciemniony i większy niż przyciemnione tło obok. Prezenter przy dialogu (warstwa 2,
    `ddm_present.cpp`): o tym, czy piksel wyjścia to tło, decyduje piksel środkowej części klatki pod nim (`BackAt`,
    współrzędne powiększonego wycinka 1365×768), a samo tło bierze z pozycji w całej klatce (w swojej wielkości albo
    powiększone jak lupa, `backShow`). Większy, jasny poziom = albo tło z pikselami, których gra nie przyciemniła, poza
    wycinkiem (pokazane takie, jakie są), albo w wycinku poziom nieprzyciemniony, uznany za „obrazek dialogu” —
    pokazany powiększony ×1,875 z dialogiem. Odtworzyć się nie dało (odprawa w Nottingham, menu Esc przy zoomie,
    dialogi obozu youtubera: zawsze 100 % przyciemnionego poziomu); poprawka (`BuildBackdrop`, `ddm_hd_menu.cpp`) przy
    znanej tablicy barwienia gry i tle będącym przyciemnionym poziomem: w wycinku piksel = nieprzyciemniony poziom →
    poziom (tło trzyma kolor klatki, wersja pokazywana — przyciemniony); poza wycinkiem wszystko, co nie jest
    przyciemnionym poziomem z chwili otwarcia dialogu → przyciemniony poziom; log „level the game left undarkened in
    the shown part: … outside it, not the darkened level: …” z prostokątami. Na zwykłych dialogach nie zadziałała.
  - Zielone ośmiokąty na leśnej mapie („jakby na pień dało się wskoczyć”, tylko na jednym zapisie): to podgląd
    samej gry pod LEWYM SHIFTEM (instrukcja s. 16: miejsca zasłonięte przeszkodą / wyżej, do których drużyna może
    dojść — płaskie wierzchołki skał, pnie). Sprawdzone w tle na zapisie youtubera `z110` (= `Savegame_104`, las z
    tarczami): Shift rysuje te pola, na grafice oryginalnej (F8) w tych samych miejscach i kształtach; w HD wnętrze
    to tekstura HD zabarwiona na zielono. Nie błąd moda.
    Ciąg dalszy (2026-11-22, użytkownik: „nad kamieniem przesunięte, obwódka wokół pni za duża; mapa youtubera to inna
    mapa”): jego mapa to Rozdroża 01 (`croisement01`, misje FoA: Emb01/04/08, Tac01/04/18; FoB = 02, FoC = 03 — nazwy
    map w `.rhm`). Żaden jego zapis z 17.11 nie jest na niej, a mapa kampanii z jego obozu nie daje teraz misji FoA.
    Sprawdzone: (1) obraz HD Rozdroży 01 leży na obrazie gry co do piksela (`tools\hd\map_alignment.py croisement01 day`:
    korelacja fazowa całości i kafli 256×256 — wszędzie +0,+0); (2) na jego zrzutach (oddalenie / przybliżenie) pola
    i pnie rosną razem ze światem (×1,63–1,67 przy ×1,65) i stoją w tym samym miejscu względem pni; (3) na Rozdrożach
    03 pola są te same przy interfejsie ×2 i ×1 (`UIScale=0`) i na grafice oryginalnej (F8) — rysuje je świat gry, nie
    interfejs. Kształty (ośmiokąty wokół pni, wierzchołki skał, kawałki trawy przy drodze) to dane gry. Lewy Shift to
    też zaznaczanie kilku postaci (instrukcja s. 15), stąd pola widać „tylko czasem”. Do potwierdzenia na Rozdrożach 01:
    jego zapis z tej mapy albo Shift + F8 u niego.
  - „Zmiana typu upscale działa tylko na postacie, a nie na mapę” (pytanie użytkownika): F7/F6 przełącza tło i
    postacie razem (`HD: background variant switched to 'ultrasharp'`, plik `hd\maps\ultrasharp\…hdmap`); na zrzutach
    dat2 → sgi → ultrasharp zmienia się ~40 % pikseli świata (różnica > 12/255), głównie faktura ziemi i dróg — budynki
    wychodzą z modeli bardzo podobnie, postacie widać bardziej. Minimapa się nie zmienia (gotowe obrazy z dat2).
    Porównanie: `tools\out\var_compare.png`.
  - „4K to 4K, a nie 1440p?” (stan sprzed 1.3.0, uzupełnienie niżej): tryb HD liczy obraz w 2560×1440; grafika HD ma 2× piksele oryginału (np. Nottingham
    4608×7040 dla poziomu 2304×3520), czyli przy domyślnym zoomie 1:1 na 1440p; na monitorze 4K prezenter skaluje
    ×1,5 (sharp-bilinear). Od 1.3.0 tło mapy może być w natywnym 4K (mapy 4× oryginału, §5i); postacie, obiekty i interfejs
    nadal są z klatki 2× (prawdziwe 4K także dla nich = grafika HD ×3, kilka razy więcej danych i generowania).
  - Ptaki (pytanie użytkownika 2026-11-22: „czy są w HD i mają klatki pośrednie, nie wyglądają płynnie”): to animacje
    otoczenia z `DATA\Animations\<pora>\` (`not_birds.rhs` gołębie Nottingham, `York_crow` / `York_pigeon`,
    `Lei_duck` kaczki, `der_henns` kury). HD — tak: klatki są w magazynach (`_common`: gołębie Nottingham 191/199,
    kaczki 98/98; `dat2`: kury 119/120; York ok. połowa w każdym). Klatek pośrednich — nie (generator robił tylko 117
    plików `Characters\*.rhs`; animacje otoczenia rysuje DirectDraw, tam ich nie ma). Krok animacji (`flags + 1` tików,
    `hd\work\frame_index.csv`): gołębie Nottingham 80 ms (12,5 kl./s), wrony 120 ms, kaczki 160 ms, do tego pauzy po
    4–20 s; lot (gołąb do ~250×270 px poziomu, kaczka 213×153) jest zapisany w PRZESUNIĘCIACH klatek, nie w pozycji
    elementu — `MotionSmooth` go nie widzi, więc ptak skacze co 80–160 ms obok płynnych postaci. Zrobione w 1.2.9:
    płynny tor między krokami i klatki pośrednie RIFE dla wszystkich animacji poziomu (§5g).
  - Narzędzia: `tools\save_test.ps1` (test w tle na wybranym zapisie: `-Profile` cudzy folder zapisów na czas
    testu, `-NoPlay` + kroki w menu „Wczytaj” — lista: nazwa z `Profiles`, pozycja w kolumnie), `tools\save_thumbs.py`
    (arkusz miniatur zapisów `<zapis>_t`), harness `hold DIK` / `release DIK`.
- **Zgłoszenia użytkownika w trakcie gry** (2026-11-22, 1.2.8 → poprawki w 1.2.9):
  - **Wywalenie gry** (znalezione w jego logu z 2026-11-21, nie zgłoszone): access violation w `0x0058C88B` —
    procedura pikselowa z testem głębi, która czyta obraz poziomu pod tym, co rysuje (wywołanie z `0x005911C5`); w
    Nottingham 8014 bajtów za końcem powierzchni poziomu 2304×3520. Powierzchnie DDM miały dokładnie swój rozmiar
    (+32 B) — DirectDraw trzymał powierzchnie systemowe z pamięcią wokół, takie odczyty przechodziły niezauważone.
    Teraz każda powierzchnia ma przed sobą i za sobą zerowany margines 128 wierszy + 4 KB (`Surface::Alloc`, `ddm.cpp`).
    **Drugi raz** (2026-11-22 01:01, gra 1.2.9, Rozdroża 1408×960 przy 1.0): 369 654 B = 131 wierszy za końcem — za
    marginesem. Prawdziwa przyczyna: `0x0058C7A0` to jedna z 10 procedur pasków pionowych wypełniacza wielokątów
    (`0x00590C90..0x005934B0`) — **stożki widzenia** przeciwników (oko w prawym górnym rogu „Pokaż / ukryj pole
    widzenia nieprzyjaciela” + klik na żołnierza). Czyta obraz poziomu (powierzchnia mapy) w miejscu ekran + kamera,
    bez sprawdzania jego rozmiaru; widoki gry nigdy nie wychodziły poza poziom, widok HD przy 1.0 na małym poziomie i
    widok pełnej wysokości — tak (czarny pas), a stożek nad pasem czytał za mapą. Poprawka (1.2.10,
    `widescreen_patch.cpp` `PatchSpanClip`): haki na wszystkie 10 procedur (`[obj+0x20C]` 1:1, `+0x210` ×2, `+0x214`
    pół; obiekt: blokada celu `+0x08` — szer./wys. `+0x0C/+0x0E`, blokada mapy `+0x2C` — `+0x30/+0x32`, kamera
    `+0x90/+0x94`), każdy pasek przycięty do wierszy i kolumn mapy i celu (początek przesunięty z poprawką wartości
    `+= wiersze × przyrost`). Sprawdzone w tle: stożek żołnierza przy 2.0 (procedura 4) i 1.0 (procedura 0) rysuje
    się jak dawniej; `dbg 1024` = poziom liczony o 400 wierszy niższy (do oglądania przycięcia). Samego stożka za
    dolną krawędzią nie udało się złapać (żołnierze idą ścieżką u góry).
  - **Kursor i ikony akcji z „obwódką wycięcia”** (wiązanie, zamki, podnoszenie, krzyżyk): kursor misji
    (`0x005F1CA0`, ślad z 2026-11-22) kopiuje ekran pod sobą do dwóch powierzchni 55×55, blokuje swój obrazek
    (powierzchnia jego rozmiaru, cała, np. 35×43) i programowo składa go z cieniem na jednej z kopii; ta kopia idzie
    na ekran ×2 z kluczem. Mod brał z niej piksele różne od tła i wygładzał obrazek 1× do ×2 — miękki brzeg obrazka,
    zmieszany przez grę z tłem 1×, został wokół jako obwódka; plików HD interfejsu kursor w ogóle nie pytał. Teraz
    powierzchnie zablokowane w czasie rysowania kursora są zapamiętane (`g_cursorPics`), ta o rozmiarze kursora jest
    szukana po treści wśród obrazków interfejsu HD (`HD::Patch::InterfacePicture`, `hd\menu_parts\`) i rysowana z
    alfą, cień = co najwyżej połowa ciemności jak u gry (`HdCursor`); bez niej — wydobyte piksele z kluczem 0x07C0
    też szukane, dopiero potem wygładzenie jak dawniej. Sprawdzone w tle: strzałka i pięści ostre, bez obwódki.
    Obrazków w `DATA\Interface\Default.res`: 1126, z wersją HD 1118 (`tools\hd\res_pictures.py`).
    **Kursor w menu i oknach** (1.2.10; użytkownik: „kursor na zwoju jest w starej wersji, a w grze już nowy”): tam
    rysuje go inna procedura — wskaźnik `0x005F1700`: kopia tła spod kursora do jednej z dwóch powierzchni
    (`[0x6C125C]` / `[0x6C12FC]`, na zmianę licznikiem `[esi+0x10]` — dwie pamięci łańcucha flip), potem `BlendBlit`
    obrazka kursora 1:1 prosto do klatki (klucz cienia 0x001F, 50 %) → `0x00622550`. Jako łatka cienia (§5e) nie
    może iść: po przejściu kursora tło HD pod nim by przepadło. Teraz `BeginShadowPicture` przy rysowaniu wskaźnika
    zapamiętuje kursor (miejsce, piksele „przed” i „po”, obrazek HD po treści) zamiast łatki, a prezenter, gdy
    pokazywana pamięć wciąż ma w tym miejscu kursor dokładnie taki, jak narysowany (`Patch::GetCursor`): wgrywa do
    tekstury klatki piksele „przed” (wszystkie warstwy HD widzą tam tło, jakby kursora nie było) i na końcu shadera
    kładzie obrazek HD (t8, 2× z przezroczystą ramką 1 teksela, kolor przemnożony przez pokrycie, alfa = ile znika z
    tego, co pod spodem, razem z cieniem — tak jak `End()` składa łatkę). `dbg 512` = bez tego (do porównań).
    Shadery prezentera sprawdza przed grą `tools\check_shader.py` (fxc z Windows SDK; DLL kompiluje je dopiero w grze).
  - **„Brew Robina i inne części są przezroczyste”** (zrzut rogu z okiem). Pierwsza ocena była BŁĘDNA („tak wygląda
    oryginalny obrazek”) — porównany był plik obrazka, nie to, co gra pokazuje; użytkownik dopytał 2026-11-22.
    Porównanie w grze (ten sam róg z `HDMenu=1` i `HDMenu=0`, `tools\testcfg\user_copy_nohdui.ini`) i piksel po
    pikselu z plikiem HD (`9e2d5bdbf05af92b.hdp`, twarz 74×60): plik jest pełny, a w grze w miejscu CZARNYCH pikseli
    (brew, kontury liści) prześwitywała ziemia. Przyczyna w prezenterze (lupa, warstwa 3): piksel klatki równy kopii
    świata = świat, pokazany powiększony; na małym poziomie przy 1.0 pas za krawędzią poziomu (`ClearOutsideLevel`)
    był czarny, więc czarne piksele interfejsu nad nim liczyły się jako świat. Poprawka (1.2.10): pas wypełniany
    kolorem 0x0801 (RGB 8,0,8 — wygląda jak czerń; w obrazkach interfejsu tylko pojedyncze piksele: 245 z 8,1 mln
    w 1×, 349 z 17 mln w HD — reguła sąsiadów zostawia je interfejsowi). Sprawdzone w tle: brew i liście pełne,
    zwoje z cieniem jak w oryginale (`tools\out\eye_compare2.png`: przed | po | oryginał 1×).
  - **Pytania ze starym tłem 1×**: „Czy rzeczywiście chcesz wyjść z gry…?” w MENU GŁÓWNYM (po misji) i „Na pewno
    chcesz zapisać grę na już istniejącej?” na ekranie zapisu. Ścieżka (A) w `OnScreenCopy` (ostatni rozpoznany
    ekran menu z czymś narysowanym na nim → obraz HD wokół) była wyłączona „w misji”, a wskaźnik silnika
    `[0x006C0FA8]` zostaje ustawiony po misji — menu główne po misji liczyło się jako misja; ekran zapisu w misji to
    obrazek menu na całość, pytanie rysowane jak w menu. (A) działa teraz zawsze. Sprawdzone w tle: misja → Wyjdź →
    menu główne → „Wyjdź z gry” (log: `a screen without a picture of its own - 77 % still the last menu picture`) i
    zapis na istniejący → tło HD wokół obu pytań (`tools\out\ex3_q2.png`, `sv1_q.png`).
  - Klatki pośrednie animacji poziomu i druga próba postaci: §5g.
- **Wydajność sprite'ów HD** (2026-11-17, v1.2.1): `Sprites::TryShadowBlit` (główna ścieżka sprite'ów HD przez procedury
  gry `0x0062261C/0x006220EC`) liczona po 8 pikseli SSE2 (przezroczyste/ukryte zostają, cień przyciemnia pojedynczo, reszta
  = piksel HD). Las misji z obozu: klatka pośrednia 11,2 → 7,1 ms, 86 → 140 kl./s. Do diagnozy: plik mapy linkera
  (`/MAP`, `src\build\Release\winmm.map`) — adresy z `tools\stack_sample.ps1` („WINMM.dll(game dir)+0x…”) daje się nazwać.
- **Linia zamachu** (zgłoszenie 2026-11-15: przy max zoomie pomarańczowa linia rysuje się gdzie indziej niż kursor,
  po zoomie „rysowanie zamachów chyba nie działa”): gest (wciśnięcie i ruch myszą w walce, `Manual.pdf` s. 27) nie
  daje się wywołać w teście w tle — test wstrzykuje tylko dane buforowane DirectInput. Od v1.1.7 hak `DrawLine`
  (`0x005ECBD0`) działa zawsze i zapisuje do logu pierwszą linię każdego rodzaju (kolor, kto rysuje, końce, kursor i
  jego miejsce w klatce): `Lines: colour … by …`. Kandydaci z kodu: `0x004DAE10` (silnik, lista obiektów `E+0x499C`,
  kolor `0xFA00`/`0xFFFF`, rysowanie łamanej `0x00460F60`), `0x004A8300` i `0x004AC850` (metody wirtualne, `0xFA00`).

### Menu i okna gry na cały ekran (`MenuZoom=1`, Direct3D 11, wysokość wewnętrzna ≥ 960)
Menu i okna (odprawa, menu Esc, wczytywanie/zapis, opcje) rysuje jedna pętla okien dialogowych gry
(`WindowManager::Draw` wywołany z 0x0053C565). Ułożone są dla 1024×768 na środku klatki. Gdy ta pętla rysuje, prezenter
pokazuje środkową część klatki 1365×768 (proporcje ekranu) na całe okno (×1,875 przy 1440p), granice kursora gry
(floaty 0x006C12E8..F4, przycinane przez wątek wejścia w 0x005F4700) są ustawiane na tę część, a ruch myszy skalowany
768/1440 (ta sama prędkość na ekranie). Klatka misji (z 0x00510618) przywraca widok i granice.

### Tła menu w HD (`HDMenu=1`, przy `MenuZoom=1`; `src\ddm\ddm_hd_menu.cpp`, `tools\hd\upscale_menu.py`)
Gra składa każdy ekran menu w powierzchni wielkości ekranu (2560×1440) i kopiuje ją na ekran (w całości przy
wejściu, potem kawałkami — naprawa miejsc pod przyciskami, napisami i kursorem); przyciski (`BlendBlit`, 168×39) i
napisy (`GBLT`) dorysowuje na wierzchu. Taka złożona ramka jest rozpoznawana po treści (FNV-1a 64 po pokazywanej
części 1365×768), w menu poza misją. Bez wersji HD zapisywana jest raz do `hd\work\menu_frames\<klucz>.mnf`;
`upscale_menu.py` wycina ramkę wokół obrazu (bez czarnego), powiększa 2× modelem wariantu (aomr, sgi, ultrasharp,
dat2), koryguje kolory do oryginału i zapisuje `hd\menu\<wariant>\<klucz>.mhd` (BGRA) + podglądy `hd\menu_png\`.
Prezenter: dwie tekstury (R32_UINT oryginalne piksele, BGRA 2× HD) aktualizowane prostokątem zmian; shader pokazuje
piksel HD tam, gdzie piksel klatki jest nadal oryginalnym pikselem tła (porównanie RGB565), resztę jak dotąd.
Zbieranie: `tools\collect_menus.ps1` (przechodzi ekrany menu ukrytej gry). Polecenie testowe `menuhd` = liczniki.
- **Menu otwarte w misji** (opcje, wczytaj/zapisz z menu Esc): wokół obrazu menu jest przyciemniony poziom, więc klucz
  całej części nie pasuje. Klucz „wnętrza” (`InnerKeyOf`: piksele 173..1193 × 144..622 części 1365×768) i pliki
  `<klucz wnętrza>.alias` (16 cyfr = klucz obrazu) obok obrazów (`upscale_menu.py` pisze je zawsze, `--aliases`
  same). Tło takiego ekranu nie jest już „tłem dialogu”: udział przyciemnionego poziomu liczony tylko w pokazywanej
  części i bez czarnych pikseli (wcześniej 87 % → w czarnych miejscach obrazu przebijał poziom: kropkowany pierścień).
- **Tła z remake'u** (`HDMenuRemake=1`, F10 „MENU W HD” → „TAK, TŁA Z REMAKE GDY SĄ”): `hd\menu\remake\<klucz>.mhd`
  ma pierwszeństwo przed wariantem, pole = cała część 16:9 (bez czarnych pasów). Obieg: `tools\hd\menu_remake.py export`
  → `hd\remake\` (obraz HD, zrzut z układem przycisków: `tools\menu_layout_shots.ps1`, `PROMPTY.txt` z promptami),
  obrazy z AI (Gemini/ChatGPT) do `hd\remake\gotowe\NN_*.jpg|png`, `menu_remake.py import` → dopasowanie do 16:9,
  mniejsze powiększane modelem (SGI) do 2730×1536, alias. Pierwszy komplet (5 teł z Gemini, 2752×1536 JPG, czyste) —
  2026-11-15, opublikowany za zgodą użytkownika („Tak, dodaj”) jako osobny plik wydania v1.1.5:
  `SherwoodEngineFix-menu-remake-1.zip` (45 MB, `tools\make_remake_pack.ps1`). Zmiana w F10 działa od razu (warstwa
  odbudowana z zapamiętanego obrazu menu).
- **Ekran twórców** (2026-11-15): las z winietą, na nim napisy przewijane od dołu (pasek 400×10500 mieszany 50 % do
  powierzchni składania, potem kopia całości na ekran). Każda klatka miała inny klucz → mod zapisywał KAŻDĄ do
  `hd\work\menu_frames` (2 MB, ~40/s, 1 GB) i ekran szedł 38 kl./s. Teraz: trzeci nowy obraz w ciągu sekundy wstrzymuje
  zapis (animacja, nie tło), limit 64 plików na sesję; puste wpisy pamięci obrazów usuwane najpierw (inaczej obraz menu
  głównego czytany od nowa po napisach). Tło twórców do remake'u: `menu_remake.py` ekran `06_tworcy` (klucz pierwszej
  klatki bez napisów `2ef7a848bc09b885`).
- **Obraz pod tym, co gra na nim narysowała** (2026-11-16, v1.1.8; „czekam aż dasz mi tło na wyjściu”). Pytanie
  „wyjść z gry?”: ślad rysowania — gra NIE barwi tła (żółtawy wygląd to stary obraz 1×): kopiuje powierzchnię składania
  menu na bok, kopiuje do niej ekran (z kursorem i wciśniętym przyciskiem), dorysowuje pergamin 400×200 i oddaje całość
  na ekran — nowy klucz, bez obrazu → 1×. Teraz dwie drogi w `OnScreenCopy` (poza misją):
  (A) ostatni rozpoznany ekran („baza”: jego piksele 1×, `g_baseCrop`) — kopia bez własnego obrazu, w której ≥ 50 %
  NIECZARNYCH pikseli bazy jest dalej na miejscu (czarne nic nie dowodzą — prawie każde menu ma czarne pasy), pokazuje
  obraz bazy tam, gdzie piksel = baza (pytanie o wyjście: 77–90 %);
  (B) `OnSurfaceCopy` (DoBlt: cała kopia między dwiema powierzchniami wielkości ekranu, żadna nie jest ekranem) —
  twórcy kopiują co klatkę stały las do powierzchni, w którą gra miesza pasek napisów 400×10500; las rozpoznany po
  kluczu (raz na stan źródła), zapamiętany dla celu na 50 ms → kopia celu na ekran bierze obraz od razu, bez kluczy
  całej klatki. `FillCanvases(…, base)`: oryginał tylko tam, gdzie źródło = baza; płótno HD wypełniane raz na obraz
  (`g_canvasPic`), osobny prostokąt zmian HD (`Layer::hdDirty`) — co klatkę idzie do karty tylko kolumna napisów
  (płótno oryginałów, prostokąt zmienionych pikseli). Twórcy: 70–90 kl./s (było 40–50). Klucz wnętrza szukany teraz
  także poza misją. Sprawdzone w tle: pytanie o wyjście na tle remake'u menu głównego, twórcy na remake'u lasu od
  użytkownika (2752×1536 z Gemini, `hd\remake\gotowe\06_tworcy.jpg` → `menu_remake.py import --only 06`), opcje,
  wczytywanie, menu Esc i opcje z misji bez zmian. Wersje HD lasu (4 warianty) idą w archiwum moda.
- **Wejście w Opcje** (2026-11-15, „przez chwilę się przycina”): ślad rysowania — 1069 ms bez klatki; próbki stosu
  (`tools\stack_sample.ps1`: polecenie testowe + migawki wątków co 50 ms) — pętla `timeGetTime` w `0x0060F250`: konstruktor
  obiektu informacji o procesorze (`0x0060F2B0`, wołany przy starcie gry i przy KAŻDYM otwarciu opcji) mierzy taktowanie
  czekając 1000 ms między dwoma `rdtsc`. Hak (`cpu_optimizer.cpp`): pomiar raz, 25 ms licznika wydajności (3600 MHz jak
  gra). Teraz ~85 ms (z tego ~23 ms analiza tła dialogu w modzie, reszta gra: paski fontów okna).

### Panel ustawień moda (F10, `SettingsKey=121`)
Rysowany w prezenterze do tekstury klatki (składany w zwykłej pamięci z obrazu źródłowego — czytanie zmapowanej
tekstury jest bardzo wolne: pierwsza wersja spadała do 27 kl./s), czcionka 5×7 skalowana. 15 opcji: wariant tła HD
(tylko w misji: zmiana na tiku), postacie HD, klatki pośrednie (w misji), wygładzanie, woda, interfejs ×2, minimapa,
przeciąganie minimapy (kamera / okno), menu, tła menu HD, VSync, filtr, płynne przewijanie, licznik FPS, prędkość kursora — zmiana od razu, zapis `WritePrivateProfileString`.
Gdy panel jest otwarty: kliknięcia myszy nie idą do gry (`input_fix.cpp` `Emit`), wciśnięcia klawiszy też nie
(hak `ProcessInputEvent` 0x005F0C20, typ 1; puszczenia przechodzą). Klawisze panelu: `GetAsyncKeyState` w wątku głównym.
Gra w misji się nie zatrzymuje (pauza: Esc, potem F10).
