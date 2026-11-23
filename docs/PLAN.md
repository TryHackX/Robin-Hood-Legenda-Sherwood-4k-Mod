# Plan naprawy i odświeżenia — Robin Hood: Legenda Sherwood (GOG) na Windows 11

Stan na 2026-11-22 (wersja 1.3.0). Sprzęt testowy: 8-rdzeniowy procesor, karta graficzna 12 GB, monitor 2560×1440 / 144 Hz.
Szczegóły techniczne (adresy, ABI): [RE_NOTES.md](RE_NOTES.md). Narzędzia: `tools\rh.py`.

## 0. Wynik pomiaru z 2026-11-13 (test nr 1, wrapper GOG-a) — to on wyznacza kierunek

- Hipoteza „mysz 1000 Hz dławi pętlę zdarzeń" jest **obalona**: obsługa zdarzenia kosztuje 0,01 ms.
- Profil głównego wątku w zwykłej grze: `ddraw.dll` GOG-a 85%, sterownik NVIDIA 6–7%, **kod gry 3–4%**.
- Tick 19 ms w spokoju: `Flip` 8,7 ms + `Blt` 2,7 ms we wrapperze. W walce `Blt` rośnie do **94 ms** na tick
  (o 14 wywołań więcej, każde czeka na GPU), przy przewijaniu kamery skoki `Blt` do ~50 ms.
- Wniosek: wolny jest wrapper, nie gra. Naprawa = własny DirectDraw w pamięci (`src\ddm\`), zrobiony tego samego dnia;
  gra startuje z nim do menu, obraz poprawny. **Czekało na test nr 2 w rozgrywce — nieaktualne: potwierdzone w kolejnych testach (2026-11-14 i dalej).**

### Test nr 2 (własny DirectDraw, 2026-11-13)
- Relacja użytkownika: 24 kl./s, walka nie laguje jak wcześniej, przesuwanie mapy nadal „laguje".
- Log (`backups\SherwoodFix_2026-11-13_test2_ddm.log`): tick **2,2–3,2 ms** przy 2560×1440 (wrapper: 19–23 ms, w walce
  113 ms), `Blt` 0,8 ms, `Flip` 0,62 ms, zero linii `Spike` w całej sesji. Przewijanie dokłada jedną pełnoekranową
  kopię (+0,4 ms). Czyli „lag" przewijania to nie wydajność, tylko 25 kroków kamery na sekundę.

### Test nr 3 (klatki pośrednie, 2026-11-13, podgląd logu na żywo podczas gry użytkownika)
- `SmoothCameraScroll` + `SmoothCursor`: własna funkcja rysująca gry (`RHGame::RenderFrame`) wołana między tickami;
  kod w `src\timing.cpp`, mechanizm w RE_NOTES. Przy 144 Hz: 5 klatek na tick.
- Na żywo: `Present=124–125 Hz`, tick 2,3–2,6 ms, klatka pośrednia 1,8–2,7 ms, bez błędów i ostrzeżeń, obraz poprawny
  na zrzutach (także przy zoomie i w walce). Do potwierdzenia: odczucie użytkownika, czyste zamknięcie gry.
- Logger otwiera plik do współdzielonego odczytu (log można czytać w trakcie gry). Profiler próbkuje tylko, gdy płyną
  ticki, i jest zatrzymywany przed `ExitProcess` (wcześniej mógł zawiesić proces przy wyjściu).

### Po teście nr 3 — uwagi użytkownika i co z nimi zrobiono (2026-11-13)
| Uwaga | Ustalenie | Zmiana |
|---|---|---|
| Tylko 100–120 kl./s, „czemu nie VSync 144" | Schemat „5 klatek na tick" dawał 125 kl./s na monitorze 144 Hz (dudnienie = szarpanie). | Zwykły `VSync=1`: jedna klatka na odświeżenie, krok kamery proporcjonalny do czasu, ticki gry na idealnym harmonogramie 40 ms. Zmierzone: 143,7–143,9 kl./s. |
| FPS spada w walce | Te same kopie pamięci bywały 2–3× wolniejsze w niektórych oknach czasu (0,16 → 0,5 ms/Mpx), niezależnie od ilości rysowania. Podejrzane przypinanie wątku głównego do jednego rdzenia. | `ThreadOptimization=0`. Do potwierdzenia w walce. |
| Przewijanie lewo/prawo szarpie, wolne | 125 kl./s na 144 Hz + prędkości przewijania dobrane pod 640–1024 px. | VSync jak wyżej; `EdgeScrollSpeed=0` = prędkość × szerokość/1024 (×2,5 przy 2560). |
| Brak dźwięków walki | Zasięg słyszalności 800 px od środka widoku wpisany na sztywno. | `HearingRangePercent=0` = zasięg × szerokość/1024. Do potwierdzenia uchem. |
| Po kilku Alt+Tab gra nie wraca | Gra sama minimalizuje okno (`CloseWindow`), a mod kłamał jej, że nie jest zminimalizowane. | `CloseWindow` ignorowane; przywracanie okna przy aktywacji. Do potwierdzenia. |

### Test nr 4 (VSync, 2026-11-13, sesja użytkownika ~13 min) — `backups\SherwoodFix_2026-11-13_test4_vsync.log`
- Użytkownik: „spora poprawa". 143,7–143,9 kl./s w większości okien, zero ostrzeżeń, 2 linie `Spike` (~0,95 s, przejścia
  z wygaszaniem ekranu, teraz ograniczone VSync-em).
- Alt+Tab: 3 cykle dezaktywacja/aktywacja w logu, po każdym gra wróciła. Wyjście Alt+F4: czyste (`detached cleanly`).
- Dźwięk: 2238 startów próbek, 0 nieudanych, 0 w klatkach pośrednich. Zasięg ×2,5 zastosowany; brak jeszcze oceny uchem.
- Spadki do 131–137 kl./s pokrywają się z animacją zoomu (klatki pośrednie są wtedy celowo wstrzymane) — kosmetyka.
- Kopie pamięci stabilnie ~0,21 ms/Mpx (bez 2–3× spowolnień z testu 3) po wyłączeniu przypinania wątków.

### Wygładzanie ruchu obiektów (`SmoothMotion=1`, 2026-11-13) — wdrożone, NIEPRZETESTOWANE w rozgrywce
- `src\motion_smooth.cpp` + hook na `RenderFrame` w `src\timing.cpp`: po ticku zapamiętuje pozycje (poprzednia/bieżąca),
  przed każdą klatką wpisuje pozycję pośrednią i woła `PI::Update`, po klatce przywraca oryginał. Obiekty pokazywane
  do 40 ms za pozycją logiczną; skok > 120 px na tick traktowany jako teleport. Błąd dostępu do pamięci wyłącza funkcję
  na resztę sesji i trafia do logu (`MotionSmooth: access violation ...`).
- Przy okazji naprawione dwa błędy klatek pośrednich znalezione w rozpoznaniu: animacje scenerii (dym, flagi) i licznik
  powidoku postaci gracza szły tyle razy szybciej, ile klatek przypadało na tick.
- Sprawdzone tylko przy starcie gry: hooki założone (`animation step guarded=1`, `afterimage counter guarded=1`,
  `MotionSmooth: ... enabled`). Test w rozgrywce się nie odbył (potwierdzone 2026-11-14, nieaktualne) — użytkownik pracował przy komputerze, a okno gry bez
  fokusu nie przyjmuje sterowania.
- Do obserwacji: drganie postaci, gdy kamera sama za nią podąża; kolejność rysowania nakładających się postaci
  (może się zamienić o jeden tick za późno); linia `smoothed objects/tick` w `Input(5s)`.
- 2026-11-14: użytkownik potwierdził, że ruch postaci jest płynniejszy; dźwięki walki są, biały kursor zniknął.

### Test nr 5 (2026-11-14, zgłoszenia użytkownika + testy automatyczne w rozgrywce) — logi `backups\SherwoodFix_2026-11-14_test5*.log`
| Zgłoszenie | Przyczyna (potwierdzona czym) | Stan |
|---|---|---|
| Ikony akcji migają bardzo szybko | panel postaci odlicza miganie „na narysowaną klatkę" (`0x005D9180`); przy 144 kl./s ~6× za szybko (dezasemblacja) | poprawione (hook `0x0055EE00`), NIE potwierdzone wzrokowo |
| „Zwiechy" po powrocie z Alt+Tab | detektor Alt+Tab w grze (`0x005E17A0`) sam wyłącza grę 5 ms po aktywacji, a okno bez ramek zostaje na wierzchu (log testu 5d; 3 z 7 przełączeń w teście 5c) | poprawione u źródła + strażnik przywracający aktywność; 6 cykli w menu po poprawce bez zwiechy |
| `Game.exe` zostaje w tle bez okna | pętla menu głównego czeka na komunikat nieistniejącego okna (stos ze strażnika, test 5a) | proces kończony w hooku `ExitProcess`, a gdy sam nie wyjdzie — 2 s po zniknięciu okna |
| Gra pada ~2 s po starcie misji | TYLKO gdy w tle wisi inny `Game.exe`: trzyma `Data\RobinHood.bks` na wyłączność (log: błąd 32) | start gry kończy pozostawiony proces bez okna albo wyświetla komunikat |
| Obraz „ucieka w lewo i w górę" | mapa Nottingham ma 2304 px szerokości, ekran 2560 → pas 256 px z prawej; dół to pas HUD 80 px, którego grafika nie zakrywa na szerokim ekranie (log `Map ...`) | NIE naprawione — patrz Etap 4 |
- Poza tym: czas komunikatów na ekranie i animacja zmiany kursora szły za szybko przy klatkach pośrednich — poprawione
  (zapis/odtworzenie stanu wokół klatki pośredniej). Ponawianie przejęcia klawiatury/myszy, gdy nie uda się przy aktywacji.
- Profiler domyślnie wyłączony (`Profiler=0`); nowy klucz `HangWatchdog=1`.
- Sprawdzone automatycznie na wersji końcowej (175 616 B): start misji, 25 Hz logiki / 143,9 kl./s, zamknięcie z menu
  w grze w 0,1–0,2 s. Alt+Tab w rozgrywce i w menu sprawdzony na wersji o jedną kompilację wcześniejszej (ten sam kod wejścia).

### Test nr 6 i tryb HD (2026-11-14, noc) — szczegóły techniczne w `docs\HD_MODE.md`
Uwagi użytkownika po przełączeniu na 1280×720 ×2: ruch płynniejszy, ale gra „mocno rozciągnięta"; zgoda na upscaling AI
(tła, postacie, animacje — wszystko) i na testowanie wielu modeli; pomysł: kilka wariantów map do wyboru i przełączanie
w locie; woda na tle HD wygląda słabo; wersja HD mapy jest „mniej ciepła, mniej nasycona" niż oryginał.

| Co | Stan |
|---|---|
| **Tryb HD**: gra składa obraz w 2560×1440, mapa w powiększeniu 2,0 (`HDView=1`: start misji od razu w 2,0) — kadr jak 1280×720, ale interfejs/napisy/kursor ostre | wdrożone, sprawdzone w tle (start misji, zoom, przewijanie, 3 min testu ciągłego: 25,0 Hz, ~132 kl./s przy zajętym GPU) |
| **Tła HD**: rozciąganie ×2 tła mapy podmieniane na obraz z `hd\maps\<wariant>\<hash>.hdmap` (poziom rozpoznawany po zawartości) | wdrożone; 20 map × 4 warianty (dat2, sgi, ultrasharp, aomr) |
| **Warianty w locie**: F8 oryginał↔HD, F7/F6 następny/poprzedni; napis na ekranie; wybór zapisywany w INI | wdrożone, sprawdzone poleceniami testowymi (klawisze fizyczne — do potwierdzenia przez użytkownika) |
| **Kolory map HD** | zmierzone: dat2 R−2,5 B+1,9 i nasycenie 60,1→57,7 %; aomr ciemniejszy i przesycony. Poprawka: niskie częstotliwości (σ=3 px) z oryginału → różnica średnich ≤0,2 poziomu. Wszystkie warianty przegenerowane |
| **Woda**: animacje wody to duże sprite'y (np. staw 272×178, 8 klatek, mocno skompresowane), które co klatkę zamalowują tło | na tle HD pomijane (`HDWater=1`) — nieruchoma woda z obrazu HD (wybór użytkownika); rozpoznawane po podobieństwie do tła pod spodem (woda 8–14/255, reszta 45–82) |
| **Pełna wysokość widoku** (`FullHeightView=1`): 42 łatki + przepuszczanie kliknięć między elementami HUD | sprawdzone: klik w dawnym pasie HUD dociera do świata (postać idzie w to miejsce) |
| **Sprite'y HD** (`HDSprites=1`): postacie, dachy, drzewa, obiekty rysowane z klatek upscalowanych AI; rozpoznawanie klatki przez hak na dekoder banku; obsługa zasłaniania (maski) i cienia | wdrożone i sprawdzone w tle na Nottingham (dzień). Magazyny: 4 × 10 072 klatki bezstratne (dachy, obiekty — w stylu wariantu) + 394 614 klatek postaci i animacji (`_common`, model SGI, 4,1 GB); razem `hd\` = 11 GB. **Test końcowy z ustawieniami użytkownika i wolną kartą (4 min, ukryty): 25,0 Hz logiki (min 25,0), praca 7,0 ms/tik (najgorszy tik 20 ms), 143,8 kl./s (min 143,0), 576 tys. narysowanych klatek HD, 0 odrzuconych, 0 błędów.** Noc/mgła: barwienie klatek HD zaimplementowane wg kodu gry, NIE sprawdzone w grze (bezpiecznik: kontrola kolorów) |
| Sprite'y bez wersji HD: powiększanie z filtrem zamiast klocków 2×2 (`SmoothSprites=1`); cienie przyciemniają każdy piksel tła HD osobno | wdrożone |
| **Klatki pośrednie animacji** (eksperyment; opis i tabela w `docs\HD_MODE.md` §5a) | tania metoda (przepływ OpenCV): rozmyte klatki z „duchami". **Model neuronowy RIFE v4.26** (wagi pobrane za zgodą użytkownika, sieć napisana od nowa w `tools\hd\rife_net.py`): ostre, wiarygodne klatki także przy dużym ruchu; w teście z prawdziwą klatką najlepszy we wszystkich 8 animacjach (np. skok: 31,0 → 23,3 przy 29,4 dla przenikania; dolna granica 9,2). NIE wdrożone w grze — czeka na decyzję (615 tys. par, ~5–6 GB, 1–3 h generowania + praca w modzie) |
| **Kursor Windows przypięty do środka ekranu podczas testów w tle** (zgłoszenie użytkownika) | przyczyna: `RHGame::Run` co obieg woła `SetCursorPos(środek okna)`; ukryta instancja testowa trafiała tym w pulpit. Tryb testowy odcina teraz SetCursorPos/ClipCursor/SendInput/SetForegroundWindow… dla całego procesu (pomiar: 25 połkniętych wywołań/s, kursor wolny) |
| Start gry za innym oknem (29 s „zwiechy" 2026-11-14) | `BringToFrontAtStart` (4 metody + wysunięcie okna nad inne) — wdrożone; próba sprawdzenia nocą nieważna, bo komputer był ZABLOKOWANY (ekran blokady odrzuca każdą zmianę pierwszego planu). NIE sprawdzone w zwykłym starcie |
| Klawisze F6/F7/F8 fizycznie, zwykłe uruchomienie na ekranie | NIE sprawdzone (ekran blokady) — do potwierdzenia przez użytkownika; przełączanie sprawdzone poleceniami testowymi, napis na ekranie widoczny na zrzucie |

Jeszcze nie w HD (kolejność prac; NIEAKTUALNE — stan z 2026-11-14, pozycje 1–4 zrobione do 2026-11-22, patrz Etap 5): 1) sprite'y na poziomach nocnych i we mgle (barwienie klatek HD tak jak robi to gra);
2) animacja wody w HD (teraz woda stoi); 3) interfejs/menu ×2 (HUD w 1440p jest mały); 4) sprawdzenie pozostałych 8 lokacji
(tła są rozpoznawane po zawartości — w logu ma być `HD: upscaled background ... in use`, a nie `no picture for this level`);
5) klatki pośrednie animacji modelem neuronowym, jeśli tania metoda nie wystarczy.

## 1. Co naprawdę jest nie tak (ustalone z logu i dezasemblacji)

| Objaw | Przyczyna | Status |
|---|---|---|
| Lagi w walce i przy przewijaniu kamery (tick 110–166 ms zamiast ~20 ms) | Wrapper DirectDraw GOG-a: `Blt` do 94 ms na tick, `Flip` 9–12 ms zawsze (patrz sekcja 0). | Własny DirectDraw w pamięci + D3D11 wdrożony (`Renderer=1`), **przetestowane w grze (nieaktualne: „czeka na test")** |
| Biały, zawieszony kursor Windows w menu | Gra działa w oknie, a chowa kursor (`ShowCursor(0)`) tylko w pętli rozgrywki; mysz trzyma DirectInput. | Poprawka wdrożona (okno nigdy nie ustawia kursora), potwierdzone (nieaktualne: „czeka") |
| Mysz 1000 Hz+ | Pętla główna robi jeden przebieg na raport myszy, ale przebieg jest tani (0,01 ms). | Scalanie do 125 Hz zostaje jako zabezpieczenie; bez mierzalnego wpływu na wydajność |
| „Animacje klatkują" | Silnik ma jedną pętlę 25 Hz na logikę i obraz. Animacje mają najwyżej 25 klatek/s z założenia. Rozdzielczość tego nie zmienia. | Wymaga interpolacji klatek (etap 3) |
| „Topornie", kursor się ciągnie | Kursor jest rysowany tylko przez wątek główny, czyli 25 razy na sekundę, i staje razem z grą przy każdym przycięciu. | Etap 2 (kursor rysowany przez nasz renderer z częstotliwością monitora) |
| Wyższa rozdzielczość „niewiele dała" | 2560×1440 to 4,7× więcej pikseli do narysowania programowo niż maksymalne 1024×768 gry, a postacie robią się malutkie. Obraz nie jest ostrzejszy, bo grafiki mają rozdzielczość z 2002 r. | Etap 2 (osobna rozdzielczość wewnętrzna i wyjściowa) |
| Poprawki audio Argona | `Fmod.dll` jest spakowany UPX-em; hooki zakładane przy starcie były kasowane przez rozpakowanie i psuły FMOD przy wyjściu. | Naprawione (LateInit), potwierdzone w logu |
| Mod zmieniał systemową akcelerację myszy | `RawMouseInput` wyłączał ją dla całego pulpitu; po crashu zostawała wyłączona. Gra i tak czyta mysz przez DirectInput. | Usunięte |
| Telemetria „Locks/f=0" | Licznik był zerowany tuż przed odczytem. | Zastąpione nową diagnostyką |

Nie wiadomo jeszcze: ile czasu ticka zjada wrapper DirectDraw GOG-a (`Lock`/`Blt`), ile dźwięk, a ile kod gry.
Zmierzy to nowa diagnostyka przy pierwszym teście (linie `Perf(5s)`, `Spike`, `Prof.*` w `SherwoodFix.log`).

## 2. Odpowiedzi na pomysły

- **DLSS** — nie da się. DLSS potrzebuje z silnika 3D głębi i wektorów ruchu; ta gra to płaski obraz 2D.
  Odpowiedniki, które mają sens: skalowanie przestrzenne shaderem (FSR 1 / NIS / CAS) oraz interpolacja klatek.
- **Upscaling plików gry (Real-ESRGAN itp.)** — poprawi ostrość, **nie poprawi płynności**. Do tego silnik rysuje
  sprite'y 1:1 w pikselach świata, więc podmiana plików na 2× większe nie zadziała bez przejęcia rysowania.
  Realna droga to „paczka HD jak w emulatorach": zrzut grafik z pamięci gry → AI upscaling offline → nasz renderer
  rysuje wersje HD na GPU. Duża praca, dopiero po etapach 1–3 (etap 5).
- **Płynniejsze animacje** — dorobienie klatek pośrednich w plikach wymagałoby przestawienia całej logiki gry z 25 na
  50 Hz (prędkości, liczniki, AI liczone w tickach), co jest nierealne łatkami. Wykonalne: interpolacja gotowych
  klatek obrazu po stronie GPU (jak w telewizorach / Lossless Scaling), z kursorem i przewijaniem kamery liczonymi
  dokładnie, bez opóźnienia.

## 3. Etapy

### Etap 0 — zrobione 2026-11-13
- `tools\rh.py`, kopie skryptów Argona w `tools\argon_scratch\`, kopie zapasowe w `backups\`.
- `src\input_fix.cpp`: scalanie raportów myszy do 125 Hz (przyciski i kółko bez opóźnień).
- `src\diag.cpp`: rozbicie czasu ticka (zdarzenia wejścia, Lock/Unlock/Blt/Flip, dźwięk) + profiler próbkujący 1 kHz.
- `src\audio_fix.cpp` + LateInit: działające hooki FMOD (na razie tylko pomiar i log).
- Test startu: gra wstaje do menu, wszystkie łatki założone, zero błędów w logu.

### Etap 1 — grywalność (najpierw to)
1. **Test w grze** (patrz sekcja 5) i odczyt logu: czy tick w walce wrócił do ~20 ms.
2. Analiza `Prof.*`: lista 5 najgorętszych miejsc, każde z nazwą funkcji i decyzją (łatka / pominąć).
3. **Własny DirectDraw w pamięci** (`src\ddm\`) — ZROBIONE 2026-11-13: powierzchnie jako zwykłe bufory RAM,
   `Lock` za darmo, `Blt` programowo w SSE2, prezentacja przez Direct3D 11 (flip-model, sharp-bilinear),
   okno bez ramek. Wrapper GOG-a zostaje załadowany, ale nie jest wołany. Specyfikacja użycia: DDRAW_USAGE.md.
   Do potwierdzenia testem w rozgrywce (linie `Perf(5s)` i `Blit(5s)` w logu).
4. Przegląd „optymalizacji" Argona pomiarem A/B: hook `memcpy`, przypinanie wątków do rdzeni, `SmoothCameraScroll`.
   Co nie daje mierzalnego zysku albo psuje stabilność — domyślnie wyłączone.

Gotowe, gdy: praca ticka < 15 ms w walce, zero linii `Spike` poza wczytywaniem, 30 min gry bez crasha.

### Etap 2 — własny renderer wyjściowy (Direct3D 11, flip-model)
1. Prezentacja bufora gry przez D3D11 (`FLIP_DISCARD`, tearing/VRR), okno bez ramek.
2. **Rozdzielczość wewnętrzna ≠ wyjściowa**: np. gra rysuje 1280×720, GPU skaluje ×2 do 2560×1440.
   Postacie mają rozsądną wielkość, a procesor rysuje 4× mniej pikseli.
3. Shadery: `UpscalerMode` 0 = najbliższy sąsiad, 1 = sharp-bilinear, 2 = FSR 1 (EASU + RCAS), plus wygładzanie
   pasm 16-bit (deband). Ustawienia w `SherwoodFix.ini`.
4. **Kursor rysowany przez renderer** z częstotliwością monitora (144 Hz), niezależnie od ticków gry.
5. Równe tempo klatek: każda klatka gry pokazywana dokładnie co 40 ms.

Gotowe, gdy: kursor płynny przy 144 Hz, obraz ostry przy skalowaniu ×2, przełączanie Alt+Tab bez błędów.

### Etap 3 — płynność ruchu
1. Przewijanie kamery i kursor z częstotliwością bliską monitorowi — ZROBIONE 2026-11-13 klatkami pośrednimi
   (`RHGame::RenderFrame` między tickami; rysowanie klatki kosztuje ~2 ms, więc nie trzeba sztuczek po stronie GPU).
   Zostaje: równe odstępy klatek względem odświeżania (VRR / `AllowTearing`), ocena wizualna użytkownika.
2. Interpolacja klatek świata 25 → 50/75 Hz (przepływ optyczny w compute shaderze), HUD i kursor poza interpolacją
   (świat przechwytywany przed `HudDraw 0x0055EE00`).
3. Szybki sprawdzian przed pisaniem punktu 2: Lossless Scaling (niedrogi, płatny program na Steamie) robi to samo
   dla dowolnej gry w oknie. Pozwala tanio ocenić, czy interpolowane 50–75 kl./s w tej grze Ci się podoba.

Gotowe, gdy: scroll bez szarpania, ruch postaci wizualnie płynny, brak smużenia interfejsu.

### Etap 4 — błędy
Potrzebna lista od Ciebie: co, gdzie, po jakiej akcji. Każdy błąd: kroki odtworzenia → log (handler crashy już
zapisuje moduł+adres i stos) → poprawka → test. Znane podejrzane miejsca: hack sub-klatek kamery, małe mapy
w wysokiej rozdzielczości (kryjówka), zoom.

Otwarte po teście nr 5 — **czarny pas z prawej i u dołu przy 2560×1440** (decyzja użytkownika, trzy drogi):
1. Zostać przy 2560×1440 i wyśrodkować mapę węższą od ekranu (pasy po obu stronach). Wymaga ujemnej pozycji kamery:
   zmiana `EnforceSafeCameraBounds`, przycięcie `ScreenToWorld 0x004DEE70` do granic mapy, przegląd ~31 funkcji
   czytających kamerę (ryzyko indeksowania poza zakresem — Argon miał tu crashe). Nieprzetestowane, nie wdrożone.
2. Wypełnić dolny pas HUD mapą (widok o pełnej wysokości): 23 odwołania do stałej 80.0 + osobne miejsca z `(0,-80)`.
3. Rozdzielczość wewnętrzna 1280×720 skalowana ×2 (sama zmiana w INI): znikają oba pasy na każdej mapie, wszystko
   jest 2× większe (jak w oryginale), menu czytelne, koszt klatki 4× mniejszy.
Zmierzone 2026-11-14 z nagłówków plików map (rozmiary w RE_NOTES, „Dane gry"): przy 2560×1440 czarne pasy ma **6 z 9
map** (Croisement 1408 px szerokości — prawie połowa ekranu czarna), przy 1280×720 żadna. To przesądza na korzyść
drogi 3; droga 1 (wyśrodkowanie) zostawia ogromne pasy. Przerwa w dolnym pasku HUD zostaje przy każdej szerokości
powyżej 800 px, więc droga 2 jest potrzebna niezależnie. 1280×720 nie było jeszcze ani razu uruchomione — do sprawdzenia:
menu przy wysokości 720, czułość kursora (1 impuls myszy = 1 piksel wewnętrzny, czyli 2× szybciej na ekranie).
DLSS odpada (wymaga głębi, wektorów ruchu i jittera z renderera 3D; gra oddaje gotowy obraz 2D). Realna droga do
jakości: tła map to strumienie bzip2 z 12-bajtowym nagłówkiem → jednorazowy upscaling AI teł i rysowanie ich w 2560×1440
pod sprite'ami (Etap 5); ewentualnie eksperyment z RTX Video Super Resolution (SDK NVIDII, pod skompresowane wideo).
**2026-11-14 02:16 — przełączono na 1280×720 ×2 na życzenie użytkownika (próba).** `SherwoodFix.ini`: `Width=1280`,
`Height=720`; poprzedni plik w `backups\SherwoodFix_2026-11-14_2560x1440.ini`. Nowy klucz `[Input] MouseSensitivity=0`
(`src\input_fix.cpp`: impulsy myszy × szer. wewnętrzna / szer. okna, z przenoszeniem ułamka) — bez tego kursor byłby 2×
szybszy. Wgrana wersja 176 128 B. **Stan: NIEPRZETESTOWANE** — użytkownik był przy komputerze, więc nie uruchamiałem gry;
do potwierdzenia przez niego: menu przy wysokości 720, prędkość kursora, brak pasów, płynność.
Do sprawdzenia przy okazji: czy miganie ikon akcji ma teraz właściwe tempo; pozostałe liczniki „na klatkę"
(metoda: zrzut pamięci przed i po klatce pośredniej albo pułapka sprzętowa na zapis).

### Etap 5 — paczka HD (w toku od 2026-11-14; opis: `docs\HD_MODE.md`)
1. ✔ Tła map: wyciągnięte z plików `.map`, 4 warianty AI, podmiana w locie, przełączanie klawiszem, korekta kolorów.
2. ✔ Woda (pomijana na tle HD), cienie, wygładzanie sprite'ów jako rozwiązanie przejściowe.
3. ✔ Sprite'y HD: hak na dekoder klatek banku (`0x00504CD0`: klatka → powierzchnia), magazyny `hd\sprites\<wariant>\`
   (klatki bezstratne: dachy, drzewa, obiekty — modelem wariantu) i `hd\sprites\_common\` (postacie i animacje — SGI),
   podmiana w rozciąganiu z kluczem (DirectDraw) i w procedurach gry `0x0062261C/0x006220EC`, maski zasłaniania, cień.
   ✔ poziomy nocne/mgła (zrobione 2026-11-15, v1.1.6).
   ✔ 2026-11-14 (zgłoszenie użytkownika: „ikonka nad głową Robina jest pixeloza"): **ikony i znaczniki w HD** — klatki
   tworzone przez grę w locie rozpoznawane po treści, magazyn `hd\sprites\_icons\` (381 ikon, UltraSharp), pierścień
   zaznaczenia przez hak na `0x005E8B10`; brakujące ikony z innych misji mod sam zbiera do `hd\work\runtime_frames\`
   (dorabia je `tools\hd\upscale_icons.py --model ultrasharp`). Polecenie testowe `miss` = raport tego, co jeszcze
   idzie bez HD (Nottingham: pusto). Opis: `docs\HD_MODE.md` §5 „Ikony i znaczniki".
4. ✔ Interfejs i menu (zgłoszenia użytkownika 2026-11-14 po pierwszej sesji, opis techniczny: `docs\HD_MODE.md` §7):
   - **Crash „Opcje → Grafika"** (zgłoszone jako „zacięcie"): hak testu trafienia HUD czytał wiszący wskaźnik RHGame po
     wyjściu z misji → sprawdzanie pamięci (`VirtualQuery`) i vtable. Sprawdzone w tle: okno grafiki otwiera się.
   - **Migający kursor** (prawy górny róg, akcje z lewej): klatki między tikami ustawiały kształt kursora świata →
     `SetCursor` (0x005F14C0) pomijany w klatkach dodatkowych, liczniki pulsowania zapisywane/przywracane.
   - **Przyciski zoomu** (prawy górny róg): `CanZoomOut` (0x004C7CF0) nie pozwalał oddalić z 2,0 przy mapie węższej od
     widoku, a mod wymuszał 2,0 → teraz oddalenie do 1,0 i z powrotem; wymuszanie tylko 3 s po starcie poziomu.
   - **Interfejs ×2** (`UIScale=2`): wszystkie okna misji, minimapa (×2, w prawym górnym rogu, `MapPosition=1`),
     ramka widoku na mapie, półprzezroczyste elementy, napis z pieniędzmi, zegar misji, dymki, kursor ×2.
   - **Menu i okna gry na cały ekran** (`MenuZoom=1`): menu główne, odprawa, menu Esc, wczytywanie, opcje.
   - **Panel ustawień moda w grze** (F10, `SettingsKey=121`): 15 opcji, zmiana od razu, zapis do INI.
   Sprawdzone w tle (zrzuty, kliknięcia, soak). ☐ Kilka portretów naraz (misje z drużyną) — grupowanie okon tej samej
   klasy jest zrobione, ale nieoglądane; ☐ ocena użytkownika na prawdziwym ekranie.
   - Zgłoszenia po pierwszej sesji z interfejsem ×2 (2026-11-14 ~17:10) i poprawki tego samego wieczoru: przewijanie
     przy górnej krawędzi, przeciąganie okna minimapy (szarpało w miejscu), zamykanie mapy klikiem w zwinięty koniec
     zwoju, ramka zaznaczenia wlecze się za kursorem (25 Hz) → dociągana w klatkach pośrednich, podpowiedź F10 w menu
     (użytkownik szukał ustawień moda w opcjach grafiki gry). Przy okazji znaleziony błąd przybliżenia menu: po
     dojechaniu kursorem do krawędzi kliknięcia w menu nie trafiały (rozjazd wskaźnika okna i kursora) — poprawiony.
   - ✔ **Tła menu w HD** (prośba „menu też upscalować", zgoda 2026-11-14): 5 teł menu zebranych w locie, 4 warianty AI,
     podmiana w prezenterze tam, gdzie piksel klatki jest oryginalnym pikselem tła (`docs\HD_MODE.md` §7).
     ✔ Przyciski menu (168×39) i pergamin odprawy — zrobione w v1.1.5.
   - Zgłoszenia z sesji 2026-11-14 ~18:30 i poprawki tej nocy: tło menu Esc/odprawy w misji ostre (bez powiększania
     tła), Robin na portrecie przy swoim marginesie, szwy przy ikonach akcji, wnętrza budynków (porównanie z obrazem
     poziomu — budynków gra nie pokazuje — pozycja nieaktualna), ✔ **minimapa**: przeciąganie po obrazku przesuwa kamerę, okno za brzegi
     lewym lub prawym, płynnie w każdej klatce (`MapDragCamera=1`, panel F10), ✔ odprawa z kilkoma stronami (pergamin
     zostawał mały od 2. strony), ✔ **opcje gry: prawdziwy procesor i pamięć** (pełna nazwa procesora
     i pamięć ponad 4 GB zamiast „GenuineIntel” i „4095 Mb”).
   - ✔ **Płynny zoom — lupa** (zgłoszenie 2026-11-14 ~18:30: czarny pas przy oddaleniu, brak przybliżenia ponad
     2,0, tylko dwa kroki; „Tak, rób zoom”): `SmoothZoom=1`, kroki 115 %, od „poziom wypełnia ekran” do 4,0; świat
     powiększany w prezenterze, interfejs 1×, myszka przeliczana, brzegi poziomu osiągalne, minimapa pokazuje
     fragment. Opis: `docs\HD_MODE.md` §7 „Płynny zoom”. ✔ zoom kółkiem względem kursora, ✔ tło menu Esc przy
     powiększeniu lupy.
   - ✔ **v1.1.1 — zgłoszenie 2026-11-15** (5 zdjęć): obiekty „rozjeżdżały się” z tłem przy zoomie kółkiem (silnik
     w spoczynku nie przesuwał tła za kamerą kotwicy → stan przewijania), „kropkowane” tło pod napisami dymków (procedura
     MMX 0x00622FD4 mieszała raz na blok 2×2 → per piksel), zoom ze scenek dawał „natywną rozdzielczość” (lupa przejmuje
     zoom gry), brak dymu z kominów (uznany za wodę → nakładka na obraz HD z płynnym przenikaniem klatek), spadki FPS
     (HUD ×2 filtrowany co klatkę → pamięć podręczna; klatka ~10 → ~4,5 ms), wywrotka przy wyjściu z misji (nieaktualny
     RHGame*). Woda: nowa opcja `HDWater=2` (obraz HD + migotanie z gry), domyślnie zostaje nieruchoma (wybór
     użytkownika). Opis: `docs\HD_MODE.md` §4 i §7. ✔ czcionka HD zamiast ×2 (zrobione w v1.1.4).
   - ✔ **v1.1.2 — woda z falami** (2026-11-15, „Tak, rób wodę”): `HDWater=2` (domyślnie) / `3` — woda z obrazu HD
     poruszana falami (przesuwana tylko gładka część obrazu, faktura zostaje ostra), pętla liczona w wątku w tle.
     Opis: `docs\HD_MODE.md` §4. ☐ Ocena użytkownika na prawdziwym stawie (test w tle ma tylko Lincoln, bez wody).
   - ✔ **v1.1.3 — zgłoszenie 2026-11-15 wieczorem** (3 zrzuty): klatki pośrednie tylko czyste (`HDInBetweenQuality=17`,
     plik `between.q`; „Robin raz HD, raz nie”, duchy, ruchy walki „w innym miejscu”), reguła sąsiadów w shaderze (plamy
     w dole paska i w menu), panel F10 z jednolitym tłem przy powiększeniu, menu Esc ~55 ms zamiast ~200 ms. Przedmioty
     na ziemi są w HD (raport `miss`: wszystko poza rogiem paska 29×95). Opis: `docs\HD_MODE.md` §5c i §7.
   - ✔ **v1.1.4 — czcionka HD w interfejsie misji** („Tak, czcionka HD”): paski fontów gry rozpoznawane po treści,
     litery z wersji ×2 zrobionych modelem SGI (`tools\hd\upscale_fonts.py`), nowe fonty zbierane same do
     `hd\work\fonts`. Opis: `docs\HD_MODE.md` §5d. ✔ Tekst w menu i oknach dialogowych — v1.1.5.
   - ✔ **v1.1.5 — menu i okna w HD, nowe tła, menu Esc bez zwiechy** (2026-11-15, „lecisz te wszystkie rzeczy co
     zostały”, „pixelozy na krawędziach czarnych”, „tło w menu… wygenerować nowe”): gładki czarny obrys fontu HD;
     napisy, przyciski, pergamin odprawy, pieczęcie i portrety w menu i oknach rysowane drugi raz w HD do „cieni”
     powierzchni (`docs\HD_MODE.md` §5e); tła menu w HD także w menu otwartych w misji (aliasy), błąd „pierścienia”
     w opcjach z misji poprawiony; tła z remake'u (`hd\menu\remake\`, `tools\hd\menu_remake.py` export/import, prompty;
     pierwszy komplet 5 teł z Gemini od użytkownika), F10 z trzema stanami; menu Esc ~80 ms zamiast ~390 ms (barwienie
     tła przez tablicę kolorów, bit w bit jak gra). Wnętrza budynków sprawdzone: postać w domu jest ukryta, dach zostaje
     — gra nie pokazuje wnętrz, nic do poprawy. ✔ Listy GDI — v1.1.6.
   - ✔ **v1.1.6 — listy w HD, noc i mgła, ostre klatki pośrednie** (2026-11-15, „leć wszystko i listy w hd i klatki
     pośrednie i cała reszta”): tekst GDI (lista klawiszy, zapisów) rysowany drugi raz czcionką 2× z wygładzaniem;
     noc i mgła sprawdzone w tle poleceniem `ambiance` (`docs\HD_MODE.md` §5f; nocą 17 nowych klatek tworzonych w locie
     dorobionych w HD); klatki pośrednie od nowa w trybie ostrym — 320 646 pokazywanych zamiast 100 722, nowa paczka 3.
     ☐ Misje z drużyną i staw w Nottingham — po postępie kampanii gracza (test w tle ma tylko pierwszą misję).
   - ✔ **v1.1.7 — opcje bez zwiechy, spokojny spoczynek, płynniejsze długie kroki, ostrzejsza lupa** (2026-11-16;
     zgłoszenie z 4 zdjęciami + „wejście w opcje się przycina”): gra przy każdym otwarciu opcji mierzyła taktowanie
     procesora czekając 1 s (`0x0060F250`) — pomiar 25 ms raz (opcje ~85 ms zamiast ~1,1 s, start gry o 1 s krótszy);
     klatki pośrednie: pary bez ruchu pominięte (spoczynek Robina — „zmieniające się ubranie”), klatka pośrednia przez
     połowę kroku z `between.t` zamiast 20 ms (kucanie, wyjście spod płaszcza, zamachy); lupa 1–2× i 2–4× filtrem
     Catmull-Rom; ekran twórców nie zapisuje już każdej klatki (1 GB w `hd\work\menu_frames`); dziennik linii pod
     diagnozę linii zamachu; eksport tła twórców do remake'u (`06_tworcy`). Dźwięk: złe urządzenie wyjścia u gracza
     (nie mod). Opis: `docs\HD_MODE.md` §5c, §7.
     ✔ Linia zamachu przy zoomie ≠ 2 (potwierdzona v1.2.3, przyczyna naprawiona v1.2.7). ✔ HD poniżej zoomu 2 — zrobione w 1.3.0 (warstwa poziomu).
   - ✔ **v1.1.8 — tło pod pytaniem o wyjście i twórcy w HD/remake** (2026-11-16, „Tak, rób”, „twórcy dodane, czekam aż
     dasz mi tło na wyjściu”): ekran bez własnego obrazu, który jest w większości ostatnim menu z czymś narysowanym na
     wierzchu, zostaje z obrazem HD/remake tego menu (pytanie o wyjście — tło menu głównego z remake'u; nowy obraz
     niepotrzebny); twórcy rozpoznawani po stałej powierzchni lasu (remake lasu od użytkownika); twórcy 70–90 kl./s.
     Opis: `docs\HD_MODE.md` §7 (tła menu).
   - ✔ **v1.1.9 — porządki i pewna instalacja** (2026-11-16; youtuber zgłosił, że „zrobił wszystko wg instrukcji i nie
     działa”): CRT wbudowany (`/MT`; wcześniej `winmm.dll` wymagał MSVCP140/VCRUNTIME140 x86 — bez pakietu VC++ gra
     w ogóle nie startowała), sprawdzenie wydania na starcie (11 wzorców procedur GOG `Game.exe` 2 899 968 B, znacznik
     3E257C94; inne → komunikat PL/EN, mod wyłączony, log), brakujący `SherwoodFix.ini` zapisywany z INI wydania
     osadzonego w DLL (`src\winmm.rc`; wcześniej 1920×1080 = tryb HD wyłączony), domyślne wartości w `config.h` jak w
     INI wydania. README: pułapka „Wyodrębnij wszystkie” (dopisuje nazwę archiwum do ścieżki), rozdzielczość (stara rada
     „1920×1080 na monitorze 1080p” wyłączała HD — obraz i tak jest skalowany), przycisk 1920*1080 w opcjach gry, sekcja
     „If it does not work”. GitHub: zostają tylko najnowsze — v1.1.9 (mod), `hd-pack-1`, `menu-remake-2` (6 teł).
   - ✔ **v1.2.0 — zgłoszenie youtubera** (2026-11-17; zrzuty mailem, komentarze pod filmem): żywopłot na dole na całą
     szerokość z czarną podstawą (dziura i „znikające krzaczki” przy zoomie), linia i rozpoznanie zamachu przy każdym
     zoomie (punkty śladu w pikselach klatki), portrety bez przeskakiwania (decyzja układu trzymana dla tej samej liczby
     okien), okno bez ramek mniejsze niż ekran (`WindowWidth/Height/X/Y`). Audyt: test ciągły 4 min z interfejsem —
     0 błędów, 0 ostrzeżeń, ~143 kl./s (długie „tiki” = okna gry otwarte w teście). Opis: `docs\HD_MODE.md` §7.
     ✔ Ekran „zamarza” po wyborze misji w obozie Sherwood (3.jpg) — v1.2.1.
     ✔ Linia zamachu na prawdziwej myszy (potwierdzone v1.2.3, przyczyna naprawiona v1.2.7). ✔ HD poniżej zoomu 2 (1.3.0).
   - ✔ **v1.2.1 — audyt kodu i gry** (2026-11-17, „zrób jeszcze raz audyt… chcemy mieć porządny remake”): dwa przeglądy
     kodu (renderer; haki/wejście/okno — wszystkie ~45 haków na `Game.exe` zgodne z konwencją i `ret N`) + kampania
     w tle do obozu komendą konsoli gry `win` (odtworzenie zgłoszenia youtubera). Poprawione: okna dialogowe otwierane
     z okna misji (kwadraty za kursorem, pergamin ×2, tło sklejone z dwóch skal, śmieci poza poziomem przy zoomie 1),
     kropki w przyciemnionym tle podsumowania misji, podświetlone pieczęcie i 30 nowych obrazków menu w HD (mapa
     kampanii, portrety odpraw, pergamin pytania), sprite'y HD po 8 pikseli (las: 86 → 140 kl./s), powrót Direct3D 11
     po resecie sterownika, kursor tej samej prędkości co w Windows także przy 2560 na 1080p i w mniejszym oknie, Esc
     zamykające F10 nie otwiera menu gry, przeciągana minimapa po końcu misji, zasięg słyszenia po zmianie poziomu,
     `GetDC` na zablokowanej powierzchni, wypełnienie przy kopii w jednej powierzchni, pamięć podręczna powiększeń
     (limit i brak pamięci), rejestrator wyjątków (nie zużywa jedynego raportu na wyjątki obsłużone). Test ciągły 4 min
     z interfejsem: PASS, 0 błędów, 143,5 kl./s. Opis: `docs\HD_MODE.md` §7. Kopia stanu kampanii z obozu do testów:
     `backups\savegame_camp_test_2026-11-17\` (zapisy gracza przywrócone).
     ✔ Pierwsza odprawa bez lupy — naprawione w v1.2.3/v1.2.5. ☐ Pozostałe z przeglądu
     (niski priorytet): cienie łatek menu rozpoznawane tylko po adresie, stan lupy przy szybkim wczytaniu tego samego
     poziomu, panel F10 w menu głównym po misji uważa, że misja trwa.
   - ✔ **v1.2.2 — drugie zgłoszenie youtubera** (2026-11-17 wieczorem; zrzuty 1.1–1.3 i film z 1.2.0 —
     `tools\out\zgloszenie_yt_2026-11-17b\`): portrety kilku zaznaczonych postaci w równych odstępach (gra rozkłada je
     na całą szerokość; rosły od tercji ekranu) + błąd od 1.0.0: grupy klas okien nie były zerowane co klatkę (licznik
     rósł bez końca, decyzja układu zależała od historii); dymek umiejętności w obozie przy kursorze (kotwica
     zapamiętana tylko dla minimapy, dymek rośnie od rogu przy kursorze); wczytywanie zapisu = czarny ekran (pełne
     wypełnienie ekranu kończy obraz menu HD); klawisz Windows (gra otwiera klawiaturę z `DISCL_NOWINKEY` —
     `WindowsKey=1` usuwa flagę); OBS: instalator GOG ustawia `game.exe` RUNASADMIN (flagi zgodności w rejestrze) —
     przechwytywanie gry działa tylko z OBS jako administrator, przechwytywanie okna metodą „Windows 10 (1903+)” zawsze
     (README). Film: wyruszenie z obozu z 1.2.0 = błąd naprawiony w 1.2.1. Test ciągły: PASS, 0 błędów, 142,9 kl./s.
   - ✔ **v1.2.3 — trzecie zgłoszenie youtubera** (2026-11-17 w nocy; zrzuty 2.1–2.5 z 1.2.1 i jego zapisy —
     `tools\out\zgloszenie_yt_2026-11-17c\`, `tools\out\yt_savegame_2026-11-17\`; linia zamachu potwierdzona przez niego
     przy przybliżeniu i oddaleniu): tło pytania „Chcesz rozpocząć wybraną misję?” przy oddaleniu (korony drzew szły
     przez powiększanie okien — render tła odbywa się wewnątrz Draw okna misji, czasem w klatce pośredniej), pierwsza
     odprawa i pierwsza klatka poziomu bez czarnego pasa (start poziomu w lupie przed pierwszym renderem), portrety w
     równym rzędzie i wszystkie (ukryte okna portretów obozu liczyły się do grupy), mapa kampanii w HD we wszystkich
     stanach (obrazki prosto z `DATA\Interface\Default.res`, `tools\hd\res_pictures.py`), hak `RenderFrame` także przy
     `SmoothMotion=0`. Opis: `docs\HD_MODE.md` §7. Zapisy gracza po teście zgodne z kopią sprzed testów (hasze).
   - ✔ **v1.2.4 — cały interfejs z obrazków HD** (2026-11-17 w nocy, „Tak, zrób” i „te małe też”): wszystkie obrazki
     `Default.res` o bokach ≥ 4 w HD (939 plików, 89 MB w `hd\menu_parts\`), interfejs misji ×2 bierze je zamiast
     filtra (`BuildFromHd` w `ddm_blit.cpp`). Sprawdzone pasy przy wczytywaniu zapisów na kolejnych poziomach
     (`tools\check_level_bands.ps1`).
   - ✔ **v1.2.5 — czwarte zgłoszenie youtubera** (2026-11-18; film z 1.2.4, log, zapisy — `tools\out\zgloszenie_yt_2026-11-18\`):
     czarny pas przy pierwszej odprawie po kółku myszy w menu (kroki kółka sprzed poziomu wyrzucane), dół poziomu
     schowany pod żywopłotem przy `FullHeightView` (kamera i lupa dochodzą do dołu poziomu nad żywopłotem — 6 miejsc
     ograniczenia kamery czyta `g_cameraStrip`), log zamachów w walce (`Swing:`; mod nie jest udowodnioną przyczyną
     nierozpoznanych gestów — czekam na jego log i porównanie bez moda), log uprawnień procesu (OBS musi działać jako
     administrator — skróty i przechwytywanie), klawisze szybkiego zapisu / odczytu (`QuickSaveKey` / `QuickLoadKey`,
     panel F10, także „brak”; prośba użytkownika), minimapa: wielkość 1,5× / 2× (klasyczna) / 2,5× / 3× / 5×
     (`MapScale`) i obraz klasyczny / klasyczny powiększony / nowy z tła HD poziomu / nowy na pergaminie — domyślny
     (`MinimapHD`; gotowe pliki `hd\minimap\`, `tools\hd\make_minimaps.py`). Opis: `docs\HD_MODE.md` §7.
   - ✔ **v1.2.6 — karta graficzna i OBS** (2026-11-18; youtuber: „OBS jako administrator działa, ale przechwytywanie gry
     dalej tylko jako ekran”, jego Ryzen 7 9800X3D ma własną grafikę): prezenter tworzy Direct3D 11 na najmocniejszej
     karcie (DXGI 1.6 „high performance”, bez tego największa pamięć; karta monitora gry, gdy to ta sama / ten sam model),
     wszystkie karty w logu, `GPU=1` = wybór Windows. Test OBS 33 u użytkownika: przechwytywanie gry działa (nagranie z
     obrazem); przeszkody opisane w README (OBS jako administrator, gra pauzuje bez fokusu, hak zgodności z anty-cheatem
     czeka na komunikat wątku, którego gra nie odbiera — pętla `0x005E1380` czyta tylko komunikaty swojego okna, tryb
     SLI/Crossfire przy dwóch kartach).
   - ✔ **v1.2.7 — gesty miecza** (2026-11-19; youtuber: nagrania z modem i bez na manekinie, log — prawie każdy zamach
     to zwykły cios): z klatkami pośrednimi pętla gry dawała kilka zdarzeń ruchu na tick z tą samą pozycją, ślad gestu
     dostawał po 4–5 identycznych punktów, a test przecięcia gry (`0x006107E0`, styk końców = przecięcie) brał każdą
     kreskę za pętlę → wynik 1. Punkt równy ostatniemu nie jest już dopisywany (ostatni dostaje świeżą jasność, smuga
     bez zmian). Sprawdzone w tle (`tools\swing_test.ps1`): kreski dają ciosy kierunkowe (2/3/5/6). Opis:
     `docs\HD_MODE.md` („Gesty miecza — przyczyna i poprawka”), adresy: `docs\RE_NOTES.md` („Ślad gestu”).
   - ◐ **v1.2.8 — tło dialogu** (2026-11-21; szóste zgłoszenie: w Nottingham przy pytaniu z prawej pasek poziomu
     nieprzyciemniony i większy): tło dialogu pokazuje teraz przyciemniony poziom także tam, gdzie gra go nie
     przyciemniła (w środkowej części i poza nią); dokładny przypadek nieodtworzony — log ma linię z prostokątami, gdyby
     wróciło. Zielone ośmiokąty w lesie = podgląd gry pod lewym Shiftem (sprawdzone na zapisie youtubera, tak samo na
     oryginalnej grafice). F7/F6 zmienia tło i postacie. Opis: `docs\HD_MODE.md` („Szóste zgłoszenie youtubera”).
   - ✔ **v1.2.9 — wywalenie, kursor HD, pytania w menu, animacje poziomu** (2026-11-22; użytkownik w trakcie gry:
     „leć wszystko… kolejkuj”): (1) access violation z jego logu (`0x0058C88B`, odczyt za końcem powierzchni poziomu) —
     marginesy wokół każdej powierzchni; (2) kursor i ikony akcji z obwódką → obrazek kursora z jego wersji HD
     (`hd\menu_parts`), sprawdzone strzałka i pięści; (3) „wyjść z gry?” w menu głównym po misji i „nadpisać?” przy
     zapisie z tłem HD (ścieżka (A) także „w misji”); (4) klatki pośrednie animacji poziomu (ptaki, kury, flagi, dym,
     muchy…; 7 636) + płynny tor lotu, druga próba odrzuconych par postaci (+11 386, pokrycie 65 → 68 %), magazyny
     odchudzone (`tools\hd\compact_between.py`) i wydane jako `SherwoodHD-4-klatki-posrednie-2.zip` w `hd-pack-1`.
     Opis: `docs\HD_MODE.md` („Zgłoszenia użytkownika w trakcie gry”, §5g).
   - ✔ **v1.2.10 — wywalenie przy stożkach widzenia, kursor HD w oknach, brew Robina** (2026-11-22; użytkownik:
     „miałem crasha”, „kursor na zwoju w starej wersji”, „czemu oko nie jest poprawione”): (1) to samo `0x0058C88B`
     drugi raz, 131 wierszy za mapą — stożki widzenia czytały mapę za jej krawędzią, gdy widok jest większy od poziomu
     → paski przycinane do mapy (10 procedur); (2) kursor w menu i oknach z obrazka HD (prezenter: klatka bez kursora
     1× + nakładka HD); (3) czarne piksele interfejsu nad pasem za poziomem brane za świat (dziury w brwi i liściach)
     → pas w kolorze 0x0801. Pierwsza ocena brwi („jak w oryginale”) była błędna. Opis: `docs\HD_MODE.md` (ten sam rozdział).
   - ◐ **Kooperacja — rozpoznanie** (2026-11-19, pytanie użytkownika o multiplayer w stylu Noita Entangled Worlds: każdy
     swoja kamera i zaznaczenie, gospodarz wybiera drużynę, bez podbierania postaci): sonda tylko w teście w tle
     (`src\coop_probe.cpp`, `tools\coop_run.ps1`, `tools\coop_compare.py`). Warstwa rozkazów znaleziona (tłumacz wejścia
     `0x005460C0`), symulacja powtarzalna przy osobnym generatorze losowym dla dźwięku i celu kliknięcia rozstrzyganym
     przez wydającego polecenie. Ocena: lockstep ~2–4 mies., stan od gospodarza ~6–12 mies. Raport:
     `docs\COOP_PROBE.md`, adresy: `docs\RE_NOTES.md` („Co-op”). ☐ Decyzja użytkownika, czy robić.
   - ✔ **Wydanie na GitHubie** (2026-11-14): v1.0.0 (README po angielsku), paczki grafiki HD jako osobne wydanie
     (7 plików ZIP < 2 GB; magazyn postaci w dwóch częściach — mod czyta `sprites.dat` + `sprites.dat.2`).
5. ✔ (zrobione 2026-11-22, poza własną oceną gracza) Klatki pośrednie animacji postaci (prośba użytkownika; „jak się nie uda, trudno"): eksperyment zakończony
   powodzeniem — RIFE v4.26 daje dobre klatki (`docs\HD_MODE.md` §5a). ☐ Wdrożenie w grze jako przełącznik
   (generowanie par + podmiana w klatkach między tikami) — po decyzji użytkownika. 2026-11-14 użytkownik zapytał, czy
   „będzie gorzej przez ghosty i blury w ruchu" → zaproponowana MAŁA PRÓBA zamiast całości: tylko animacje Robina,
   włączane klawiszem (ocena w ruchu w grze; klatki RIFE są gładsze od oryginalnych, pary koniec→początek trzeba odsiać).
   ◐ **Próba wdrożona 2026-11-14** („już nie gram, możesz robić co tam chcesz"): `HDInBetween=1`, klawisz F9, dane dla
   Robina (`RobinTown.rhs` + `RobinHood.rhs`) w `hd\sprites\_between\`; opis i pomiary: `docs\HD_MODE.md` §5b. Sprawdzone
   w tle (pozycje co do piksela, zasłanianie, 25 Hz / 7 ms / 144 kl./s).
   ✔ **Wszystkie postacie** (decyzja użytkownika 2026-11-14: „Tak, wszystkie postacie"): 117 plików `Characters\*.rhs`,
   457 198 par, 314 756 użytecznych, 2,98 GB w `hd\sprites\_between\`; generator przeniesiony na kartę (7× szybciej),
   nowa miara „cienkie elementy" (znikające miecze/włócznie odrzucane), sylwetki z samego cienia. Opis: `docs\HD_MODE.md`
   §5c. ☐ Ocena użytkownika na prawdziwym ekranie (F9 = porównanie), inne poziomy, noc/mgła.

**Pierwsza prawdziwa sesja użytkownika w trybie HD** (2026-11-14 od 11:54, Nottingham dzień; z `SherwoodFix.log`, bez
mojego udziału): 25,0 Hz, 143,9 kl./s, 7,0 ms pracy na tik (max 7–27 ms w zwykłej grze), 0 ostrzeżeń i błędów, tła `dat2`
i oba magazyny sprite'ów wczytane, woda pomijana. Przerwy 1,2–3,8 s: po starcie poziomu (11:55:04), przy ponownym
wczytaniu (11:56:21, 11:56:26) i jedna bez przypisanej przyczyny (11:56:52–57; w tle kończył się wtedy eksport PNG na
jednym rdzeniu — przez wcześniejsze 45 s eksportu gra szła równo). Do obserwacji. Klawisze F6/F7/F8 nadal
niepotwierdzone (w logu brak „background variant switched").

### 1.3.6 (2026-11-23) — klawisze moda w Opcje → Klawiatura i w F10, wersja w menu
- Prośba użytkownika: klawisze moda do przypisania w natywnym menu gry i w panelu F10, przy informacji o F10 wersja
  moda i przypisany klawisz, przycisk resetu do domyślnych.
- **Klawisze moda** (`ModKeys`, `mod_keys.cpp`): panel, tło HD / oryginał, następny i poprzedni wariant, klatki pośrednie
  jako kody DirectInput (`SherwoodConfig::modKeys`), sprawdzane dalej jako klawisze wirtualne (`VkFromDik`). INI zapisuje
  nazwy (`HDKeyToggle=F8`); stare kody dziesiętne Windows (`119`) czytane jak dawniej (`DikFromVk`).
- **Natywny ekran** (`key_dialog.cpp`, RE w RE_NOTES): lista gry bierze liczbę akcji z RHKeyConfig::Count (stałe 28),
  klawisze z KeyAt, a nazwy z tekstów 0x97+i. Dla roboczej kopii otwartego ekranu Count = 28+5, KeyAt 28.. = edytowane
  klawisze moda, FindKey widzi je też (gra czyści duplikat — „Puste!”), a nazwy wierszy wpisuje hak tekstów
  WidescreenPatch (`KeyDialog::ApplyRowLabel`, tylko dla wywołania z `0x005CEE5D`: te same numery tekstów nazywają
  strzałki i lewy Shift). Wiersze moda przyjmują też F10, F11, Home i ScrollLock (gra trzyma je z dala od swoich akcji).
  OK zapisuje (panel zawsze ma klawisz), Domyślne 1/2 przywracają też klawisze moda, Wybór — zapisane, Anuluj porzuca.
  Polskie litery: kompilator zrobił z `L"\u0144"` dwa znaki (U+0139 U+201E), więc etykiety są w UTF-8 i zamieniane
  przy pierwszym użyciu (`MultiByteToWideChar`).
- **Panel F10**: wiersze „KLAWISZ: …” (Enter/klik → następny klawisz z hooka DirectInput; Esc anuluje, Delete = brak,
  strzałka w lewo = domyślny), odmowa dla klawiszy akcji gry (w menu z profilu gry `[0x006C0E48+0x28]+0x34`) i innych
  klawiszy moda, wiersz „KLAWISZE - PRZYWROC DOMYSLNE”. Tytuł z wersją, podpowiedzi z przypisanym klawiszem, litery
  mniejsze, gdy 28 wierszy się nie mieści.
- **Wersja**: `SHERWOODFIX_VERSION` z CMake (pierwsza linia logu, panel, podpis w prawym dolnym rogu menu: „SHERWOODENGINEFIX
  1.3.6   F10 - USTAWIENIA MODA” z klawiszem, jaki panel naprawdę ma). Komunikat startowy misji z przypisanymi klawiszami
  (i w logu).
- Test w tle na kopii INI (`htest.ps1 -Ini` — domyślnie test pisze do INI gracza!): wiersze z polskimi literami, przypisanie K →
  OK → `HDKeyBetween=K`, ponowne otwarcie, Domyślne 1, duplikat (Działanie 2 = F8 → „Mod: tło HD” Puste!), F11 na wierszu
  moda, Anuluj; panel: przechwycenie, odmowy (q, F1, F8), Esc, reset; podpis w menu; komunikat w misji. Polecenie testu
  `text ID` (kody znaków tekstu gry).

### 1.3.5 (2026-11-22) — widoczny interfejs blokuje kliknięcie w świat
- Pytanie użytkownika: czy przezroczyste części przez skalowanie zachodzą gdzieś jeszcze. Pomiar całego ekranu (polecenie
  testu `pointergrid`, `tools\hd\plot_pointer.py`, 14 400 punktów): poprawka 1.3.4 działa wszędzie (ok. 640 punktów
  przezroczystych części — oba końce żywopłotu, środkowe kawałki, ozdoby rogów), ale był stary błąd odwrotny. Widoczne
  piksele interfejsu poza oknami gry (zawinięte liście w obu dolnych rogach, górny rząd liści nad czarnym paskiem, ozdoby
  górnych rogów, czerwona pieczęć przy prawej krawędzi) i czarny pasek żywopłotu między widżetami oddawały kliknięcie
  światu, i to w inny punkt: przy kamerze u góry mapy klik w liście wysyłał postać 60 px niżej, za żywopłot, a ze
  środkowego kawałka 228 px w lewo; w pasku — w ukryte miejsce pod nim.
- Teraz (prośba „blokuj”) interfejs przejmuje taki wskaźnik. `UiScale::Resolve` oznacza punkt jako interfejs, gdy piksel
  powiększonego obrazka jest nieprzezroczysty, a przeliczony punkt nie leży na oknie gry, albo gdy kursor jest w wierszach
  paska od `DDM::LastHudBandTop`. Po testach trafienia menedżera okien (koniec pętli `0x006194AF`, wstawka `LoopEndStub`),
  gdy żadne okno nie zostało trafione, okno paska HUD bierze wskaźnik jak w niezmienionej grze (`ClaimForInterface`:
  węzeł +0x90). Pozycja wskaźnika się nie zmienia: przewijanie przy krawędziach (gra przewija dopiero przy samym brzegu)
  i ramka zaznaczania działają jak dotąd. Okna gry (portrety, zwój mapy) wygrywają zawsze — blokada działa tylko wtedy,
  gdy nic nie zostało trafione. Pierwsza próba z wstawką w `0x006194B4` wywróciła grę
  (`0x006194B8` jest celem skoków — RE_NOTES).
- Sprawdzone w teście w tle: klik w liście (lewy i prawy koniec, środek) i w pasek — postać stoi (pod liśćmi zwykła
  ziemia); klik w trawę — idzie; portret — zaznacza; przewijanie w dół, w lewo, w prawo i w obu górnych rogach nad
  zablokowanym interfejsem; ramka zaznaczania przeciągnięta na żywopłot kończy się normalnie.

### 1.3.4 (2026-11-22) — trawa pod bramką na mapie wozu
- **Róg mapy wozu (Croisement02)** (zgłoszenie: „w jednym miejscu wciąż nie mogę wejść, mimo że tam zwykła trawa”,
  „przez bramkę po trawie pod nią powinno się dać chodzić”, „przy samej krawędzi mapy nie pozwalaj”). Diagnoza z 1.3.3
  („pas niedostępny dla gracza”) była błędna: trawa leży w głównej strefie ziemi gry (`zoneq`: strefa id 0), krzyżyk
  robił mod. Przy interfejsie 2× prawy koniec żywopłotu (obrazek 320×165, powiększony od rogu do 1920..2560 ×
  1110..1440) sięga wysoko nad swoje liście; wskaźnik nad jego przezroczystą górą był przeliczany jak nad
  interfejsem — świat dostawał punkt (kursor + (2560, 1440)) / 2, za dolną krawędzią mapy, i gra pokazywała „nie da
  się”. Teraz (`UiScale::PieceUnder`) piksel klucza powiększonego obrazka nie należy do obrazka, gdy przeliczony punkt
  nie leży na żadnym oknie gry (gra trafia w okna prostokątami — tam bez zmian): wskaźnik idzie wtedy do świata
  z prawdziwą pozycją. Źródło obrazka zapisuje DDM przy powiększaniu (`UiScale::NoteSource`, `DDM::PixelIsKey`).
  Liście żywopłotu działają jak dotąd — sam brzeg mapy za nimi nadal niedostępny.
- Rozpoznanie stref chodzenia i wyboru kursora: RE_NOTES („Strefy chodzenia i kursor misji”); polecenia testu
  `zones`, `zoneq`, `zonelog`, `pointer`; `tools\probe_walk.ps1` zapisuje też, czy wskaźnik trafia w okno gry,
  `tools\hd\plot_walk.py` rysuje wynik na zrzucie klatki. Usunięty podgląd krzaków (`mock_corner_bushes.py`).

### 1.3.3 (2026-11-22) — korony drzew z obrazu HD poziomu
- **Korony drzew** (zgłoszenie: „na mapie liście wyglądają dobrze, a to, co gra rysuje na nich, jest złe”): gra
  rysuje korony drugi raz nad poziomem jako sprite z „sitem” — co drugi piksel kluczem (szachownica), reszta to
  lekko poruszona kopia liści z obrazu poziomu (postać za drzewem prześwituje, liście drgają). W 2× sito dawało
  klocki 2×2, a skalowanie AI dziurawych klatek — kanciaste liście (zrzuty 1× i różnice: `dbg 65536` →
  `tools\out\harness\cutout_*.raw`, `tools\hd\analyse_cutouts.py`: 1–5 % pikseli identycznych z poziomem, 55–97 %
  blisko, 150–180 % „dziur szachownicy”). Rozpoznanie (`OverlayAt`, ddm_hd.cpp): na siatce poziomu, ≥ 55 % pikseli
  blisko obrazu poziomu, ≥ 40 % dziur; pokrycie z sąsiedztwa (szachownica = ½, pełne = 1), kolor z obrazu HD poziomu
  (2×, `LevelOverlay2x`) albo z obrazu poziomu (1×, `LevelOverlay1x` w procedurze 1× `0x00622550` — warstwa poziomu
  prezentera pokazuje tam HD). Nad gołą ziemią korona = mapa (przy 4× też), postać za drzewem prześwituje gładko.
  Piksele, gdzie klatka już ma obraz HD, pomijane po 8 (SSE2): koszt ok. 0,7 ms na klatkę. `dbg 32768` wyłącza.
- **Ciche testy w tle naprawdę ciche**: FMOD dopisywał dźwięk do już oddanych buforów (zerowanie przy waveOutWrite nie
  wystarczało — gracz słyszał ukrytą grę). Teraz urządzenie waveOut jest w trybie testowym udawane (proxy: bufory
  „grane” w czasie rzeczywistym, nic nie idzie do Windows). Sprawdzenie: `tools\audio_check.ps1` (sesje audio procesu
  i ich szczyt — dla testu: brak sesji).
- Narzędzia: polecenie testu `cursor` (który obrazek kursora gra narysowała), `tools\probe_walk.ps1` (siatka punktów:
  gdzie postać może iść). Róg mapy wozu: pas pod bramką (poziom ok. x 1470–1792, y ≥ 1065) niedostępny dla gracza,
  a żołnierze tamtędy chodzą; podgląd krzaków jako przeszkody: `tools\hd\mock_corner_bushes.py`.

### 1.3.2 (2026-11-22) — celownik łuku, skalowanie w dół
- **Celownik łuku** (zgłoszenie: „lupa łuku nie pokazuje postaci”, potem „czarne kółko”): to kursor gry —
  pierścień ze strzałkami i duży cień w środku. Gra kopiuje tło spod kursora, rysuje w kopii kursor i oddaje ją
  (mod: ×2). Cień przyciemniał piksele klatki w miejscu kursora, a przy płynnym zoomie to nie jest miejsce, które
  pokazuje wyjście (inny fragment poziomu albo czarny pas za nim przy silniku 1,0). Teraz `HdCursor` (ddm.cpp)
  zapisuje kursor misji (`Patch::NoteMissionCursor`: prostokąt, piksele przed, obraz HD), prezenter oddaje klatce
  piksele spod kursora i kładzie jego obraz na wyjściu (jak kursor w menu, t8) — cień przyciemnia powiększony świat,
  cel widać w środku [Z]. `dbg 512` wyłącza też tę nakładkę.
- **Gra większa niż ekran** (3840×2160 w opcjach gry na monitorze 1440p, 2560×1440 na 1080p): prezenter zmniejszał
  jedną próbką na piksel — liście migotały i były poszarpane. Teraz cztery próbki na ślad piksela (`Minified`).
- Wyjaśnione: mapa 4K jest ostra (`tools\hd\compare_4k_sharpness.py`: 3,4× więcej drobnych szczegółów niż
  powiększony obraz HD, 11× niż oryginał) — wrażenie miękkości to „malarski” charakter skalowania AI. Czarne tło
  opcji tuż po zmianie rozdzielczości w grze — do pierwszego wejścia na poziom (gra nie przerysowuje ekranu po zmianie
  trybu). Niedostępny pas trawy przy dolnej krawędzi poziomu: w oryginale zasłaniał go pasek interfejsu (80 px),
  projektanci nie dali tam ścieżek; `FullHeightView=1` go pokazuje.

### 1.3.1 (2026-11-22) — ostatnie dwa elementy interfejsu w HD
- Róg ekranu z rogiem myśliwskim (29×95, prawy dolny) i czerwona pieczęć po prawej (41×41) gra powiększa własną
  procedurą sprite'ów 2× (`0x0062261C`, z cieniem: kolor klucza 0x07C0, marker 0x001F, czynnik 128). Gdy rysują się okna
  (`UiScale::Drawing()`), `Sprite2xWindowHd` bierze je z `hd\menu_parts` po zawartości (pliki były już, z
  `Default.res`). Obrazy trzyma menedżer okien poza ostatnio zablokowanymi powierzchniami — czytane spod wskaźnika
  procedury. Stara ścieżka wygładzania pokazywała róg „wyblakły”; obraz gry jest w pełni kryjący [Z]. `dbg 8192` = bez
  tego, `dbg 16384` (test) zapisuje obraz 1× do `tools\out\harness\sprite2x_<w>x<h>.raw`.

### 1.3.0 (2026-11-22) — natywne mapy 4K, warstwa poziomu, pauza
- **Natywne mapy 4K** (`HDMaps4k=1`): `hd\maps4k\<wariant>\<klucz>.hdm4` (4× model 4xNomos2_hq_dat2 + dopasowanie kolorów; `tools\hd\upscale_4k.py`, `tools\hd\make_maps4k.py`). Wszystkie 20 map, wariant dat2, 3,3 GB. Używane przy zoomie powyżej ok. 2,2 na 2560×1440 i od 2,0 na monitorze 4K. Tylko tło mapy; postacie, drzewa, cienie, stożki widzenia i interfejs zostają takie, jak rysuje gra. Zweryfikowane [Z] w teście w tle (Croisement02). Paczki opcjonalne: `SherwoodHD-5-mapy-4k-a.zip` i `-b.zip` (wydanie `hd-pack-1`). Opis: `docs\HD_MODE.md`.
- **Warstwa poziomu poniżej 2,0** (`HDLevelLayer=1`): zamiast powiększonych oryginalnych pikseli tło z obrazu HD poziomu.
- **Błysk przy przejściu do dialogu** naprawiony: klatki przejściowe (lupa menu bez dialogu, pierwsza klatka dialogu przed kopią świata) nie są prezentowane (poprzednia zostaje, najwyżej 6 pod rząd). Nowa komenda `presentlog N`.
- **Pauza**: w pauzie gra nie rysowała klatek i kręciła pętlę ok. 47 000 razy na sekundę na pełnym rdzeniu; teraz hak robi najwyżej jeden przebieg na odświeżenie (`pause 1|0` w teście).
- **Zmiana wariantu przy zmienionym obrazie poziomu** (powóz): wariant szukany po kluczu własnego obrazu poziomu.
- **Dźwięk w trybie testowym**: FMOD gra przez WINMM, a `waveOutWrite` w DLL zamienia bufory w ciszę; żadnych zmian ustawień Windows (test z 2026-11-22 wyciszył sesję audio Game.exe na stałe — cofnięte).
- **Poprawki z przeglądu kodu**: pozycje obiektów przywracane przed każdym dialogiem; estymata czasu dodatkowych klatek ignoruje próbki dłuższe niż tik; log awarii obejmuje błędy w modzie; klawisze szybkiego zapisu przywracane przy WM_CLOSE/WM_DESTROY; brak kopii `Game.exe.bak` i zapisu flagi large-address do działającego exe; kolektory obrazów bez HD piszą tylko tam, gdzie istnieje `hd\work` (dwa tworzyły go u każdego gracza); prawdziwy LRU pamięci podręcznej sprite'ów HD; zapasowy prezenter GDI pokazuje FPS i panel F10; i inne.
- **Odpowiedzi / znane ograniczenia**: postacie, obiekty i interfejs nie mogą być w natywnym 4K (gra rysuje je w klatce 2×). Niedostępny prawy dolny róg przy płocie na mapie skrzyżowania to dane nawigacji poziomu (chunk FACE pliku `.rhp`) — nie zmieniane (zbyt ryzykowne bez możliwości testu AI i zapisów). Białe tarcze na ekranie Esc to własne przyciemnienie gry. Koło lupy przy celowaniu łukiem niepokazujące postaci — jeszcze nie odtworzone.

## 4. Podział pracy (oszczędzanie tokenów: najwyżej 1–2 agentów naraz)

| Kto | Co |
|---|---|
| Fable (główna sesja) | dezasemblacja, hooki silnika, rdzeń DirectDraw w pamięci, architektura renderera, interpolacja |
| Opus, średni effort | porty shaderów HLSL (FSR 1, deband), testy pikselowe blitterów, przeglądy kodu |
| Sonnet, niski effort | szablony COM (vtable IDirectDraw7/Surface7), obsługa INI, dokumentacja, rozeznanie w sieci, streszczanie logów |

## 5. Test, którego teraz potrzebuję (5 minut)
1. Uruchom grę normalnie (GOG / skrót).
2. Wczytaj zapis z przeciwnikami. Walcz 1–2 minuty, machając myszą tak jak zwykle.
3. Przewiń kamerę w 4 strony, użyj zoomu, wyjdź z gry przez menu.
4. Napisz, jak było w odczuciu. Log `SherwoodFix.log` przeczytam sam.

Porównanie „przed": ustaw w `SherwoodFix.ini` `MouseReportRateHz=0` i powtórz tę samą walkę.
Powrót do wersji Argona: skopiuj `backups\winmm_argon_2026-11-13.dll` jako `winmm.dll`.
