# Notatki z dezasemblacji `Game.exe` (GOG, baza 0x00400000, bez ASLR)

Narzędzie: `python tools\rh.py <polecenie>` (dis / func / xrefs / callers / vcalls / str / find / imports / iatcalls).
Każdy adres poniżej został sprawdzony w dezasemblacji, chyba że oznaczono inaczej.

## Pliki
- `Game.exe` — silnik (2,9 MB). Jedyna różnica względem `Game.exe.bak`: bajt 0x116 (flaga Large Address Aware).
- `Robin Hood.exe` — tylko launcher. GOG uruchamia bezpośrednio `game.exe` z warstwami zgodności
  `DWM8And16BitMitigation 8And16BitAggregateBlts RUNASADMIN HIGHDPIAWARE` (rejestr `AppCompatFlags\Layers`).
- `ddraw.dll` — zamknięty „GOG.com DirectX 1-7 wrapper" 1.1.0.6018 (backend `d3d9.dll`), konfiguracja `dxcfg.ini`.
- `Fmod.dll` — FMOD 3.x, **spakowany UPX** (sekcje UPX0/UPX1), importuje `WINMM.dll` → nasz proxy inicjuje się
  PRZED rozpakowaniem FMOD-a. Hooki na FMOD wolno zakładać dopiero w LateInit (hook punktu wejścia `0x00643885`).
- Importy `Game.exe`: `DDRAW!DirectDrawCreate`, `DDRAW!DirectDrawEnumerateExA`, `DINPUT!DirectInputCreateEx`,
  `WINMM!timeGetTime`, 45 funkcji `fmod.dll`, 10 `binkw32.dll`. Brak Direct3D.

## Pętla główna `RHGame::Run`
- `0x0050F995` — początek ticka: `GetTickCount` → `esi = frameStartTick`.
- `0x0050F9B1..0x0050FC34` — **pętla zdarzeń wejścia**: `GetEventCount (0x005F0CE0)` → `PopEvent (0x005F1150)` →
  pełny potok obsługi (`0x00517AF0`, `0x00517900`, `0x006192A0`, `0x005107A0`, …) → `jmp 0x0050F9B1`.
  Wykonuje się **raz na każdy raport myszy w kolejce**, dopóki kolejka nie jest pusta.
- `0x00510114..0x00510125` — limiter 40 ms (25 Hz; wariant 0x190 = 400 ms przy `[ebx+0x18] != 0`).
  Zastąpiony przez `GameLoopLimiterDetour` → `PaceGameLoopTick` (`timing.cpp`).
- Po limiterze: `0x00547280`, `ShowCursor(0)`, `PumpMessages (0x005E1380)`.
- `0x005105D0` `RHGame::RenderFrame`: `RHEngine::Update (0x004C8380)` → `HudDraw (0x0055EE00)` → … →
  `0x005F1A90` → `EnterCS` → `CursorSetVisible (0x005F4900)` → `CursorDraw(0) (0x005F1CA0)` →
  `SBDrawMgr::Present (0x005E5260)` → `LeaveCS`.
- Logika i rendering są sprzężone w jednym ticku 25 Hz — gra nie ma osobnej pętli renderowania.

## Wejście (moduł SBInput)
- `0x005F09E0` `SBInputThreadProc` (wątek tworzony w `0x005F1467`). Pętla:
  `WaitForMultipleObjects(2, {mysz 0x6C1330, klawiatura 0x6C1334})` @ `0x005F0B0F` → `EnterCS(csInput 0x6C1230)` →
  gałąź myszy `0x005F0B22..0x005F0B80` / klawiatury `0x005F0BA0` → `LeaveCS`.
- Gałąź myszy (oryginał): `ReadMouseData (0x005FBDB0, thiscall, ret 4)` czyta **1 rekord** DirectInput
  (`GetDeviceData`, vtable+0x28, `cbObjectData=0x10`) → `ProcessInputEvent(0, rec) (0x005F0C20, cdecl)` dokłada węzeł
  0x1C bajtów do listy `[0x6C1264]`, licznik `[0x6C1268]` → jeśli `[0x6C12AC]==1`: `PeekEvent(count-1, A, B)
  (0x005F0D20, cdecl)` + `UpdateCursor(A) (0x005F45D0, cdecl)` dodaje deltę do pozycji kursora `0x6C1300` (float x,y).
- Ramka wątku na poziomie pętli: `[esp+0x10]` = struktura zdarzenia myszy A (3×word dx/dy/dz + bajty przycisków
  +6..+8, przyciski NIE są zerowane między iteracjami), `[esp+0x38]` = bufor rekordu, `[esp+0x50]` = tablica klawiszy B.
- `PopEvent (0x005F1150, cdecl(A, B))` — zdejmuje rekord; rekord X (ofs 0) + następny Y (ofs 4) skleja w jedno
  zdarzenie ruchu (i odwrotnie). Obsługiwane offsety myszy ≤ 0x0E (X, Y, kółko, 3 przyciski).
- `CursorDraw (0x005F1CA0, cdecl(int))`: dla argumentu **1 wraca od razu** (`0x005F1CF8: cmp [esp+0xD0],1 / je epilog`) —
  wątek wejścia NIE rysuje kursora. Kursor rysuje wyłącznie wątek główny, czyli 25 razy na sekundę.
- Błąd oryginału: gdy `ReadMouseData` nic nie odczyta, pętla i tak przetwarza stary rekord → podwójny ruch.
- Sekcje krytyczne: `csInput 0x006C1230`, `csCursorRender 0x006C12C8`. Oryginał ma odwróconą kolejność blokad
  (wątek główny: csCursor→csInput, wątek wejścia: csInput→csCursor) = możliwy deadlock; `cpu_optimizer.cpp`
  zamienia w `RenderFrame` (`0x005106F0`, `0x00510732`) csCursor na csInput.

## Grafika (SBDrawManager, `P:\Robin Hood\SBLibNG\SBDrawManager.cpp`)
> Pełna specyfikacja użycia DirectDraw (każde miejsce wywołania, flagi, struktury): [DDRAW_USAGE.md](DDRAW_USAGE.md).
> Poprawki względem tej sekcji: kod menedżera sięga do `0x005EF390` (funkcje tekstu/konsoli `0x005ED270..0x005EE620`
> wołają `Blt`/`GetDC`/`ReleaseDC` bezpośrednio); `GetCaps` nie jest wołane wcale (offset 0x2C to `Flip` powierzchni);
> flaga 8 przy tworzeniu powierzchni to caps `0x20004040` = NONLOCALVIDMEM|VIDEOMEMORY|OFFSCREENPLAIN, nie 3DDEVICE;
> test wersji DirectX w `0x005E1440` sam ładuje `DDRAW.DLL` i szuka `DirectDrawCreate`/`DirectDrawCreateEx` po nazwie —
> gdy zawiedzie, gra się zamyka; zaraz po `DirectDrawCreate` gra prosi o `IID_IDirectDraw2`, potem o `IID_IDirectDraw7`.
- Offsety vtable wywoływane w zakresie `0x005E3D00..0x005E6500`: 0x00, 0x08, 0x10, 0x14, 0x18, 0x20, 0x2C, 0x30, 0x44,
  0x50, 0x54, 0x64, 0x68, 0x6C, 0x70, 0x74, 0x80. Przypisanie do metod wg kontekstu (do potwierdzenia logiem wywołań
  przed pisaniem własnego DirectDraw): IDirectDraw7 — CreateClipper (0x10), CreateSurface (0x18), GetCaps (0x2C),
  GetDisplayMode (0x30), SetCooperativeLevel (0x50), SetDisplayMode (0x54); IDirectDrawClipper — SetHWnd (0x20);
  IDirectDrawSurface7 — Blt (0x14), Flip (0x2C), GetAttachedSurface (0x30), GetDC (0x44), GetPixelFormat (0x54),
  Lock (0x64), ReleaseDC (0x68), Restore (0x6C), SetClipper (0x70), SetColorKey (0x74), Unlock (0x80).
  **Brak BltFast (0x1C)**. Kod poza tym zakresem nie był jeszcze przeszukany pod kątem innych metod.
- `0x005E5A00` LockSurface (`EnterCS` per powierzchnia + `Lock`) — **66 miejsc wywołania** w silniku
  (sprite'y `0x005E69xx/0x005EAAxx/0x005EBCxx/0x005EC5xx`, RHSight/stożki widzenia `0x0058EA70`, `0x00590C90..0x005934B0`,
  kursor `0x005F27xx..0x005F44xx`, czcionki `0x0061B4C0`).
- `0x005E5D30` UnlockSurface, `0x005E61E0` Blt (46 wywołań), `0x005E6440` blit powierzchnia→powierzchnia,
  `0x005E5890`/`0x005E5BD0` GetDC/ReleaseDC, `0x005E5260` Present (Blt lub Flip na primary).
- `0x005E4F70` CreateSurface: `flags&8` → `3DDEVICE|VIDEOMEMORY|OFFSCREENPLAIN`, `flags&2` → `VIDEOMEMORY|OFFSCREENPLAIN`,
  `flags&1` → `SYSTEMMEMORY|OFFSCREENPLAIN`.
- Format roboczy: 16 bit RGB565.

## Rysowanie klatki i przewijanie mapy (zweryfikowane 2026-11-13)
- `RHGame*` globalnie w `[0x006C0B3C]`. `RHGame::RenderFrame (0x005105D0)` — `__thiscall(pGame, int showCursor, int doPresent)`,
  `ret 8`. Pętla główna woła `(1, 1)` w `0x0050FF05`; drugi wołający (`0x0053B6CD`) używa `(0, 0)`.
  Kolejność w środku: `RHEngine::Update(pGame+0x18)` → `HudDraw(pGame+0x196C)` → `0x006189E0(pGame+0x68, 0)` →
  `0x004C8140(pEngine)` → licznik komunikatu `dec [pGame+0x3574]` → `0x00511B90` → (gdy `pEngine[0xD1]==0` i stan
  `[pGame+0x3550]` nie jest 1/2/3) `0x005F1A90` → sekcja kursora → `Present`.
- `0x005F1A90` (`cdecl void(void)`) — **drżenie kursora**: co klatkę losuje ±1 px w X i w Y (po 50%), dopisuje sztuczne
  zdarzenie ruchu myszy do kolejki i przesuwa kursor. Stąd znane „trzęsienie się" kursora w tej grze.
- `RHEngine::Update (0x004C8380)` rysuje świat wg stanu kamery `[pEngine+0x48C0]`: 1 = mapa stoi (blit „powierzchni
  świata" `[+0x48CC]` na back buffer + obiekty `0x004D0A10`), 2 = przewijanie, 3/4 = zoom, 5 = pełne odrysowanie.
  Na końcu (`0x004C9413..0x004C942D`) zeruje flagi kierunków `[+0x6D]`,`[+0x6E]`,`[+0x6F]`,`[+0x70]` i ustawia stan 1.
- Przewijanie (stan 2, `0x004C86D6`): `ClampScrollDelta` → `SetCameraPosition(pozycja + prędkość [+0x48B8/+0x48BC])` →
  `0x004CEDE0`: przesuwa powierzchnię świata samą na siebie (`0x005E6440`), dorysowuje odsłonięte paski
  (do 4 blitów z powierzchni mapy) i kopiuje całość na back buffer.
- Prędkość przewijania: tablice `[+0x4798]` (X) i `[+0x4818]` (Y) indeksowane licznikami `[+0x4782]`/`[+0x4784]`
  (rosną, gdy kierunek jest trzymany — funkcje `0x004DB9xx..0x004DBDxx` ustawiają flagi kierunków i stan 2; po
  puszczeniu `Update` zmniejsza licznik co wywołanie = bezwładność). Wartości w pikselach na tick, dzielone przez zoom.
- Wniosek użyty w `timing.cpp`: `RenderFrame` można wołać między tickami; z ustawioną prędkością, flagami `[+0x6D]`,
  `[+0x6E]` = 1 i stanem 2 przesuwa kamerę o zadany ułamek dystansu, bez flag odrysowuje stojącą klatkę.

## Obiekty ruchome i ich pozycje
Pełny opis: [OBJECT_RENDER.md](OBJECT_RENDER.md). Skrót używany przez `src\motion_smooth.cpp`:
- Listy `vector<RHElement*>` w silniku: `E+0x49D8` (główna, sortowana), `E+0x49B8`, `E+0x4998`; begin `+4`, end `+8`.
- `S = [el+0x50]` (RHSprite), `P = S+4`; świat x,y,z = float `P+0x0C/10/14`, flagi ważności `P+0x04` (bit 0 = xyz ważne),
  id elementu `el+0x0C`. `PI::Update 0x0055F290 (thiscall P)` przelicza pozycję rzutowaną, punkt rysowania i bbox —
  gra sama robi „wpisz xyz, flagi = 1, Update, narysuj, przywróć" dla cieni obiektów (`0x004B722A..0x004B7243`).
- Pozycje zmienia tylko tick silnika (`0x004C6EF0`, wołany z `0x0050FEE6` przed `RenderFrame` w `0x0050FF05`).
- Liczone na klatkę, a nie na tick (ważne przy klatkach pośrednich): krok animacji scenerii `0x005B7F60` wołany
  z `0x005C6763`/`0x005C67C5`, licznik powidoku postaci gracza `dec word [el+0x580]` w `0x00491B5A`. Mod blokuje oba
  w klatkach pośrednich.

## Okno, aktywacja, Alt+Tab (zweryfikowane 2026-11-13)
- Flaga „aplikacja aktywna": `[0x006A6D27]`. WndProc `0x004094F0`: przy dezaktywacji (`0x004095B8`) zeruje flagę,
  zwalnia DirectInput (`0x005F9EA0(pInput, 0)`), `ShowCursor(1)`, pauzuje dźwięk (`0x005A7B40(0)`); przy aktywacji
  (`0x004095F0`) ustawia flagę, `ShowWindow(SW_RESTORE)`, **kręci się w pętli `while (IsIconic(hwnd))`** (`0x00409633`),
  potem `0x005E3F70` (odtworzenie trybu), ponowne przejęcie wejścia.
- Pętla główna (`0x0050F904..0x0050F946`): gdy flaga = 0 i w poprzednim obiegu była aktywna → `0x00513070` →
  **`CloseWindow(hwnd)` (gra sama minimalizuje okno)**; potem blokujące `GetMessage` w `0x005E1400` (`0x005101DB`) aż do
  aktywacji. `PumpMessages (0x005E1380)` przetwarza jeden komunikat na obieg.
- Mod: `IsIconic` zwraca dla okna gry FALSE (inaczej pętla `0x00409633` nie kończy się na Win 11), a `CloseWindow` jest
  ignorowane — okno bez ramek zostaje na miejscu. Bez tego drugiego gra po kilku Alt+Tab rysowała do zminimalizowanego okna.
- Kursor: gra woła `ShowCursor(0)` tylko w pętli rozgrywki (`0x0051012F`), `ShowCursor(1)` przy dezaktywacji.

### Zwiechy po Alt+Tab i proces-zombie (zweryfikowane testami 2026-11-14)
- WndProc obsługuje tylko: `WM_DESTROY` (`[0x6C06AC]=1` + `PostQuitMessage`), `WM_PAINT`, `WM_ERASEBKGND`,
  `WM_ACTIVATEAPP` (`0x0040959F`, test `LOWORD(wParam)`), `WM_SYSKEYDOWN/UP` (połykane). Flagę aktywności zerują też:
  pętla główna przy `IsIconic` (`0x0050F91B`), `0x00513080` (po `CloseWindow`), `PumpMessages` (`0x005E13AC`)
  i **detektor klawiszy `0x005E17A0`**.
- **`0x005E17A0` — `cdecl(uint8_t* stanKlawiszyDInput)`**, wołany co obieg pętli menu (`0x0052F695`), filmów Bink
  (`0x0040126F`), pokazu slajdów (`0x005E2A6D`) i pętli `0x0053C4A3` — NIE z pętli rozgrywki. Widząc Alt+Tab
  (`[0x0F]` + `[0x38]`/`[0xB8]`), Ctrl+Esc (`[0x01]` + `[0x1D]`/`[0x9D]`) albo Ctrl+Alt+Del: zeruje te klawisze, **zeruje
  flagę aktywności i woła `ShowWindow(hwnd, SW_MINIMIZE)`** (`0x005E1845..0x005E185A`). Zwraca 1 dla Alt+Tab/Ctrl+Esc.
  Po powrocie Alt+Tabem tablica klawiszy ma jeszcze „wciśnięte" Alt+Tab (log: detektor odpala 5 ms po aktywacji), więc
  gra sama się wyłączała; okno bez ramek nie daje się zminimalizować → gra na wierzchu, flaga 0, główny wątek stoi
  w `GetMessage` (`0x005E1400`) i czeka na `WM_ACTIVATEAPP`, którego Windows już nie wyśle. Mod: hook w `input_fix.cpp`
  (zeruje klawisze, flagi nie rusza) + strażnik w `watchdog.cpp` (okno na wierzchu i flaga 0 przez ~0,75 s →
  `PostMessage(WM_ACTIVATEAPP, TRUE)`).
- **Flaga startowa = wynik pierwszego przejęcia wejścia**: `0x0040AF3E: InputAcquire(pInput,1)` → `mov [0x6A6D27], al`.
  Gra uruchomiona bez fokusu ma flagę 0 i stoi na filmie wstępnym, dopóki nie dostanie aktywacji.
- **`0x005F9EA0` — `SBInput::Acquire(bool)`, `__thiscall`, `ret 4`, wynik w `al`.** Dla 1 i `[this+0x2A9]==0`: `Acquire`
  klawiatury (`[this+8]`, gdy `[+0x2A5]`), myszy (`[this+0xC]`, gdy `[+0x2A7]`), 16 joysticków (`+0x144`); błąd →
  `[+0x2AA]=2`, zwraca 0, nikt nie ponawia (wołający ignorują wynik). `[+0x2A9]` ustawiane już po udanej klawiaturze.
  Dla 0: `Unacquire` wszystkiego. Globalny wskaźnik: `[0x006C1838]`. Mod ponawia nieudane przejęcie co 100 ms.
- **Zombie**: zamknięcie okna (Alt+F4, pasek zadań) w pętli menu głównego (`0x0053C56D`) → `WM_ACTIVATEAPP(FALSE)` zeruje
  flagę, okno znika, a pętla wraca do blokującego `GetMessage(hwnd)` na nieistniejącym oknie — proces żyje bez okna
  z 0% CPU (potwierdzone zrzutem stosu strażnika). W menu w grze flaga wyjścia jest sprawdzana i gra kończy się sama.
  Wątki „Suspended" widoczne w takim procesie to wątki robocze sterownika NVIDIA (`nvwgf2um.dll+0xCB3D80`), nie objaw.
- Koniec gry (`WinMain`, `0x0040B8A3`): `SuspendThread` + `TerminateThread` wątku wejścia (`0x005F4840`/`0x005F4850`),
  potem destruktory i `ExitProcess`. Mod kończy proces w hooku `ExitProcess` (bez fazy `DLL_PROCESS_DETACH`).
- Wrapper GOG-a (`DDRAW.dll` w katalogu gry) mimo `Renderer=1` jest załadowany i **hookuje `GetMessageA`/`PeekMessageA`**
  (widać go na stosie głównego wątku); importuje też `SetWindowsHookExA`. Nie stwierdzono, by coś psuł.

### Liczone „na narysowaną klatkę" (ważne przy klatkach pośrednich; mod pilnuje, by szły raz na tick)
- `HudDraw 0x0055EE00` → `0x005D9180` na każdym panelu postaci: miganie 3 ikon akcji — licznik mignięć
  `word[panel+0xC98+2i]`, faza `word[panel+0xC9E+2i]` (5 klatek), widoczność `byte[panel+0xA54+0x104*i]`.
- `RenderFrame 0x0051068F`: `dec [pGame+0x3574]` — czas wyświetlania komunikatu na ekranie.
- Rysowanie kursora `0x005F22D9..0x005F239A`: przejście kształtu kursora — klatka `[0x6C12A0]`, opóźnienie `[0x6C133C]`,
  cel `[0x6C11E8]`, kierunek `[0x6C1212]`, start `[0x6C11EE]`.
- `0x006189E0` (GUI): `[this+0x17C8]++` — parzystość bufora przy `Flip`; zgodna, bo każda klatka pośrednia też robi `Flip`.
- Wcześniej znalezione: animacje scenerii `0x005B7F60` z `0x005C6763/0x005C67C5`, powidok `0x00491B5A`, drżenie kursora.

## Dźwięk (RHsound.cpp, zweryfikowane 2026-11-13)
- Menedżer dźwięku globalnie w `[0x006C1070]`; `Update (0x005A9420)` wołany z pętli głównej (`0x0050FF3E`) raz na tick —
  NIE z `RenderFrame`, więc klatki pośrednie nie dotykają dźwięku (licznik `inExtraFrame=0` w logu to potwierdza).
- FMOD wołany przez skoki pośrednie `0x005E39D6..0x005E3AD2`; start próbki: `0x005A72C0` → `FSOUND_PlaySoundEx(FREE, sample,
  NULL, paused)` → głośność/panorama `0x005A7410` → `SetPaused(false)`.
- Słyszalność dźwięków pozycyjnych: `0x005B2FF0` (zwraca 0 = nie graj). Zasięgi w obiekcie `soundMgr+0x138`, pola
  `+0xB8/+0xBC` (800/240 px przy zoomie 1) i `+0xC0/+0xC4` (640/240), ustawiane w `0x005B2F30(this, float zoom)`
  ze stałych 400/800/1200, 80/120/240/360, 320/640/960. Dobrane pod ekrany 640–1024 px: na 2560×1440 walki dalej od
  środka widoku były nieme. Mod mnoży te cztery pola przez szerokość/1024 (`HearingRangePercent`).

## Kamera / silnik (z notatek Argona, częściowo zweryfikowane)
- `0x006C0FA8` — globalny wskaźnik `RHEngine*`. Pola: `+0x4728/+0x472C` viewport W/H (float), `+0x4730/+0x4734` mapa W/H,
  `+0x4738/+0x473C` pozycja kamery, `+0x475C` zoom, `+0x48B8/+0x48BC` prędkość scrolla, `+0x48C0` stan kamery.
- `0x004CEC60` ClampScrollDelta — `__fastcall`, 0 argumentów na stosie (`ret`).
- `0x004C0340` ClampCameraPosition — `__thiscall`, 1 argument (`ret 4`). `0x004DB7F0` SetCameraPosition — `__thiscall`, `ret 4`.
- `0x005758D0` ClampCameraAndZoom — `__cdecl(outRet, inPos)`.
- `0x0052FCF7` limiter menu (20 ms).
- Pełny opis zoomu i ograniczania kamery: [ZOOM_CAMERA.md](ZOOM_CAMERA.md). Skrót: widok = `(vpW, vpH-80)/zoom`
  (dolne 80 px to pas HUD, stała `[0x677598]`, 23 odwołania); `CanZoomOut 0x004C7CF0` sam blokuje oddalenie, gdy mapa
  jest mniejsza niż podwojony widok; gdy widok jest szerszy niż mapa, kamera ląduje w 0 (mapa w lewym górnym rogu).
- Zmierzone: mapa Nottingham = **2304×3520 px** — węższa niż ekran 2560 px → czarny pas 256 px z prawej przy zoomie 1.
  Log wypisuje to przy każdej zmianie mapy/zoomu (`Map WxH px, view ...`).

## Dane gry (tylko rozpoznanie nazw i rozmiarów, formaty nieznane)
- `DATA\robinhood.bks` 565 MB + `robinhood.dic` 9 MB — bank sprite'ów (potwierdzone 2026-11-14). Gra otwiera `.bks`
  **na wyłączność** (share 0) i doczytuje klatki na żądanie: `0x00504600(bank, klatka)` → gdy brak danych, `0x005058A0`.
  Tabela klatek `[bank+0xC]`, wpis 20 bajtów: `+0` szer., `+2` wys., `+8` wskaźnik danych, `+0x10` typ.
  Dekoder RLE `0x005E3919` (wiersz: pierwszy x, ostatni x, piksele; `0xFFFF` = pusty wiersz) nie sprawdza zakresów.
  **Drugi `Game.exe` (np. zombie) nie otworzy `.bks`, dostaje śmieci i pada w `0x005E399B` (ACCESS_VIOLATION WRITE)
  ~2 s po starcie misji** — log pokazuje wtedy `File open FAILED (error 32 ...): Data\RobinHood.bks`.
- `DATA\Levels\{Day,Night,Fog,Custom1}\*.map` (20 plików, 93 MB) — tła poziomów. Nagłówek (odczytane 2026-11-14, tylko
  pierwsze 32 bajty): `u16 szerokość, u16 wysokość, u32 = 2, u32 rozmiar`, dalej strumień **bzip2** (`BZh91AY&SY`).
  To te dwa `u16` trafiają do `E+0x4730/+0x4734`. Obok `*.rhm`, `*.scb`, `*.min`, `*.rhp`.
  Rozmiary map (px): Croisement01/03 **1408×960**, Croisement02 **1792×1152**, Derby **1920×2752**, Sherwood (kryjówka)
  **1920×1088**, Nottingham **2304×3520**, Lincoln 2944×2176, Leicester 3136×1984, York 3136×2318.
  Widok to `(szer., wys.−80)/zoom`: przy 2560×1440 sześć z dziewięciu map jest węższych od ekranu (trzy także niższe),
  przy 1280×720 wszystkie wypełniają widok. Oddalenie (zoom 0,5) wymaga mapy ≥ 2× widok: przy 1280×720 działa na
  Lincoln/Leicester/York, przy 1152×648 także na Nottingham (jak w oryginale 1024×768).
- Grafika paska HUD ma łącznie ok. 800 px szerokości (lewy róg ~480, prawy ~320): przy każdej szerokości > 800 px
  w dolnych 80 px zostaje czarna przerwa pośrodku (1760 px przy 2560, 480 px przy 1280).
- `DATA\Characters\*.rhs`, `DATA\Animations\**\*.rhs` — małe pliki opisów.
- Brak publicznej dokumentacji i narzędzi do tych formatów (rozeznanie w sieci 2026-11-13).

## Uruchamianie testowe bez UAC (z konsoli nieuprzywilejowanej)
```powershell
$env:__COMPAT_LAYER = "RunAsInvoker DWM8And16BitMitigation 8And16BitAggregateBlts HighDpiAware"
Start-Process .\Game.exe -WorkingDirectory (Get-Location).Path
```

## Narzędzia testowe (`tools\`, stan 2026-11-14)
- `gamectl.ps1`: `-Start`, `-Focus` (bierze fokus tylko, gdy użytkownik nie dotykał komputera ≥ `-MinIdle` s; własne
  wstrzyknięcia nie liczą się jako aktywność), `-Unfocus`, `-AltTab [-AltHoldMs n] [-Cancel]` (prawdziwy Alt+Tab),
  `-Key/-MoveX/-MoveY/-Click` (tylko gdy gra jest na wierzchu), `-Shot`, `-Close` (jak Alt+F4, raportuje czy proces
  zniknął), `-Kill`, `-Status`, `-IgnoreLeftover`.
- `threaddump.ps1 [-ProcId n] [-Mem adr:rozmiar,...] [-Elevate]`: okna, moduły, uchwyty i stosy wszystkich wątków
  procesu 32-bitowego (adresy → `moduł!eksport+offset`), dekoduje na co wątek czeka. Egzemplarz uruchomiony przez
  `gamectl` nie jest podniesiony, więc działa bez UAC.
- `autotest_newgame.ps1`: start → pominięcie filmów → „Graj!" → obserwacja pierwszych sekund misji (zrzuty co ~1 s).
- `deploy.ps1 -ReplaceInUse`: wgrywa DLL także wtedy, gdy stary plik trzyma proces bez okna.
- W menu głównym kursor startuje na środku; „Graj!" jest 224 px w prawo i 40 px w górę (piksele wewnętrzne gry).
  Kursor gry przesuwa się o 1 piksel wewnętrzny na impuls myszy (sprawdzone przy 2560×1440: ruch o 224 impulsy trafił
  w przycisk). Przy `MouseSensitivity=0` i rozdzielczości wewnętrznej niższej od okna mod mnoży impulsy przez
  szer. wewnętrzna / szer. okna — skrypty muszą wtedy wysyłać odpowiednio więcej (`autotest_newgame.ps1 -MoveScale 2`).

## Tryb HD, skład klatki w powiększeniu 2,0, bank sprite'ów, tryb testowy w tle (2026-11-14)
Opisane w `docs\HD_MODE.md` (procedury rysujące ×2 `0x62261C/0x6220EC/0x62469C`, dekoder banku `0x504CD0`, wycinanie
zasłoniętych części sprite'a `0x5BBF20/0x5BC3C0`, stan kamery 5 = pełne odrysowanie, `SetCursorPos` w pętli misji
`0x50F98E`) oraz `docs\SPRITE_BANK.md` (format `.dic/.bks/.rhs`). Klawisze gry z instrukcji: Num+/Num− zoom, strzałki
przewijanie, F1 szybki zapis, F5 szybki odczyt, Alt pole widzenia, 1–5 bohaterowie — mod używa F6/F7/F8.

