# Robin Hood: Legenda Sherwood — mod 4K / HD (SherwoodEngineFix)

Nieoficjalna modyfikacja do **Robin Hood: Legenda Sherwood** (Spellbound, 2002) w wersji **GOG**. Mod to jedna
biblioteka `winmm.dll` kładziona obok `Game.exe` — gra ładuje ją sama, żadne pliki gry nie są zmieniane.

Cel: płynna i ostra gra na dzisiejszych komputerach i monitorach (1080p, 1440p, 4K, 144 Hz), bez zacięć oryginalnego
wrappera DirectDraw, z opcjonalną grafiką HD przygotowaną modelami AI z oryginalnych plików gry.

## Co robi

**Wydajność i stabilność**
- Własny DirectDraw w pamięci + prezentacja przez Direct3D 11 (flip-model) zamiast wolnego wrappera — koniec lagów
  w walce i przy przewijaniu kamery.
- Okno bez ramek na cały ekran, poprawne Alt+Tab (gra nie zawiesza się po powrocie), VSync, skalowanie obrazu
  shaderem (ostre piksele / sharp-bilinear / miękki).
- Klatki pośrednie między tickami gry (gra liczy logikę 25 razy na sekundę): płynne przewijanie mapy, płynny kursor,
  płynny ruch postaci i pocisków — z częstotliwością monitora; prędkość gry bez zmian.
- Poprawki wejścia (mysz o wysokiej częstotliwości, ponowne przejęcie myszy po Alt+Tab), dźwięku (zasięg słyszalności
  na szerokim ekranie), strażnik zawieszeń, usunięte zakleszczenie wątków przy przewijaniu krawędzią ekranu.

**Obraz**
- Wysokie rozdzielczości w menu gry (przyciski 1920×1080, 2560×1440, 3840×2160).
- **Tryb HD** (od szerokości 2048): gra składa obraz w pełnej rozdzielczości, a mapę pokazuje w powiększeniu 2,0
  (kadr jak przy 1280×720) — interfejs, napisy i kursor są ostre; mapa sięga do dołu ekranu.
- **Tła map HD**, **postacie i obiekty HD**, **ikony HD**, **tła menu HD** — z obrazów przygotowanych modelami AI
  (4 warianty do wyboru w locie), z kolorami dopasowanymi do oryginału. Te obrazy NIE są częścią repozytorium — patrz
  „Grafika HD” niżej.
- Opcjonalne klatki pośrednie animacji postaci (model RIFE) — animacje 2× płynniejsze.

**Interfejs**
- Interfejs misji 2× większy (każde okno rośnie od swojego rogu ekranu, kliknięcia trafiają tam, gdzie je widać).
- Minimapa w prawym górnym rogu; przeciąganie po niej przesuwa kamerę, okno mapy przesuwa się za brzegi.
- Menu i okna gry (odprawa, menu Esc, zapis, opcje) na cały ekran zamiast małego okienka 1024×768 na środku.
- **Panel ustawień moda w grze — klawisz F10** (grafika HD, interfejs, minimapa, menu, VSync, filtr, licznik FPS,
  prędkość kursora); zmiany działają od razu i zapisują się w `SherwoodFix.ini`.
- W opcjach gry prawdziwa nazwa procesora i ilość pamięci (gra pokazywała „GenuineIntel” i najwyżej 4095 MB).

## Wymagania

- Robin Hood: Legenda Sherwood z **GOG** (mod rozpoznaje kod `Game.exe` tej wersji; inne wydania nie są obsługiwane —
  wtedy łatki się nie zakładają, a log to opisuje).
- Windows 10 lub 11, karta graficzna z Direct3D 11.

## Instalacja

1. Pobierz archiwum z zakładki **Releases**.
2. Skopiuj `winmm.dll` i `SherwoodFix.ini` do folderu gry (tam, gdzie leży `Game.exe`).
3. Uruchom grę jak zwykle (GOG Galaxy albo skrót).

Odinstalowanie: usuń `winmm.dll` (i ewentualnie `SherwoodFix.ini`, `SherwoodFix.log`).

Domyślnie gra rysuje w 2560×1440 (tryb HD). Na monitorze 1080p ustaw w `SherwoodFix.ini` `Width=1920`, `Height=1080`
(albo `0` i `0` = rozdzielczość monitora). Wszystkie opcje są opisane po polsku w samym pliku INI.

## Klawisze moda

| Klawisz | Działanie |
|---|---|
| F10 | panel ustawień moda (w menu, w oknach gry i w misji) |
| F8 | tło HD ↔ oryginał (szybkie porównanie) |
| F7 / F6 | następny / poprzedni wariant grafiki HD |
| F9 | klatki pośrednie animacji postaci (gdy są wygenerowane) |

## Grafika HD

Obrazy HD powstają z oryginalnych plików gry, więc **nie są rozpowszechniane** (prawa autorskie do grafiki gry).
Każdy może je zrobić sam ze swojej kopii gry narzędziami z `tools\hd\` (Python 3 + PyTorch, karta NVIDIA zalecana):

- `extract_maps.py`, `upscale_variants.py` — tła map (warianty: `dat2`, `sgi`, `ultrasharp`, `aomr`),
- `extract_sprites.py`, `upscale_sprites.py`, `upscale_icons.py` — postacie, obiekty, ikony,
- `upscale_menu.py` — tła menu (mod sam zbiera je do `hd\work\menu_frames\`),
- `make_inbetweens.py` + `rife_net.py` — klatki pośrednie animacji.

Modele AI trzeba pobrać samemu ze stron ich autorów (licencje i źródła: `docs\UPSCALE_MODELS.md`). Układ folderu `hd\`
opisuje `hd\CZYTAJ.txt`, a cały tryb HD — `docs\HD_MODE.md`. Bez obrazów HD mod działa normalnie (oryginalna grafika,
ostro powiększona).

## Znane ograniczenia

- Silnik gry ma tylko trzy poziomy przybliżenia (0,5 / 1 / 2). Na poziomach węższych niż 2560 px (np. Nottingham)
  oddalenie do 1,0 zostawia czarny pas z prawej strony. Płynny zoom (lupa w prezenterze) jest w planach.
- Grafika HD działa w powiększeniu 2,0; przy 1,0 widać oryginalne piksele.
- Poziomy nocne i mgła nie są jeszcze sprawdzone z grafiką HD.

## Budowanie ze źródeł

Visual Studio 2022 lub nowsze (C++, CMake), konfiguracja **Win32 / Release**. MinHook jest pobierany przez CMake
(FetchContent).

```
cmake -S src -B src/build -A Win32
cmake --build src/build --config Release
```

Wynik: `src\build\Release\winmm.dll`. Skrypt `tools\deploy.ps1` buduje i kopiuje bibliotekę do folderu gry.

## Dokumentacja

- `docs\PLAN.md` — plan, przebieg prac i wyniki testów,
- `docs\HD_MODE.md` — tryb HD, interfejs ×2, menu, panel F10, tryb testowy w tle,
- `docs\RE_NOTES.md`, `docs\DDRAW_USAGE.md`, `docs\OBJECT_RENDER.md`, `docs\ZOOM_CAMERA.md`, `docs\HUD_STRIP.md`,
  `docs\SPRITE_BANK.md` — zweryfikowane adresy i budowa `Game.exe`,
- `tools\` — dezasembler (`rh.py`), sterowanie grą w trybie testowym w tle, narzędzia HD.

## Zastrzeżenia

Projekt fanowski, niezwiązany ze Spellbound Entertainment, GOG ani właścicielami praw do gry. Repozytorium nie zawiera
plików gry ani grafiki z niej pochodzącej. Do gry potrzebny jest legalny egzemplarz. Używasz na własną odpowiedzialność
— zrób kopię zapasową zapisów gry.
