# Co-op (lockstep) — wynik rozpoznania, 2026-11-19 (sesja poboczna)

Tylko badanie. Nic nie wydane, nic nie zacommitowane. Zwykła gra bez zmian zachowania: sonda działa wyłącznie przy
`SHERWOODFIX_TEST=1` (test w tle). Wgrany `winmm.dll` zawiera sondę, ale bez tej zmiennej nie zakłada żadnego haka.

## Gdzie co jest
- Sonda: `src\coop_probe.cpp/.h` (+ punkty zaczepienia: `view_zoom.cpp` ScreenToWorld, `ui_scale.cpp` gest miecza,
  `timing.cpp` `InRenderFrame()` i `OnLoopEnd`, `test_harness.cpp` polecenie `coop`, `dllmain.cpp` Install).
- Skrypty: `tools\coop_run.ps1` (nagranie/powtórka w tle, sam przywraca zapisy gracza), `tools\coop_compare.py`
  (porównanie przebiegów tick po ticku), `tools\fninfo.py`, `tools\serfields.py`, `tools\asmctx.py`.
- Adresy i wyniki: `docs\RE_NOTES.md`, sekcja „Co-op”. Dane przebiegów: `tools\out\harness\coop_s*_*`.

## 1. Warstwa rozkazów
Wejście → `RHGameInputTranslator::HandleEvent 0x005460C0` (thiscall(tr, Event*), ret 4) → rozesłanie do dwóch
słuchaczy: silnik `0x004DC2C0` (this = E+0x60) i RHGame `0x00512580`. Wszystko, co gracz zleca światu, to zdarzenia
dla słuchacza silnika: kat. 1 (wskaźnik, flagi przycisków) → `OnWorldClick 0x004DE290` → gest `0x004E1A40`
(rozpoznawanie `0x00547600`) → tryb kursora `[tr+0xA2]` 0..20 → funkcja akcji `thiscall(E, Vec2*, u8[,0[,0]])`;
kat. 2 (kody gry; 13–16 = `SelectPC 0x004DBF20`, argument = wskaźnik postaci). Rozkazy `RHOrder` / sekwencje
powstają dopiero w ticku logiki (`RHEngine::Tick 0x004C6EF0`), wszystkie (sprawdzone sondą) w kontekście logiki.
Jedno zaznaczenie (`E+0x49F8`) i jeden stan tłumacza na grę → w co-op podstawiane na czas polecenia gracza.

## 2. Powtarzalność
- Jedyny generator: CRT `rand()` (`0x00642A7D`, stan na wątek). `srand(time)` przy każdym zapisie (też `Restart`
  na starcie misji) → ziarno musi ustawiać host. Moduł dźwięku (0x005A6000..0x005B5800) zużywa ten sam generator,
  także z rysowania (wariant dźwięku `0x005AE380`), a ile — zależy od kamery, zegara FMOD i włączonego dźwięku →
  potrzebny osobny generator (sonda: `coop isolate 64`).
- Klatki pośrednie moda: 0 wywołań `rand()`. Ale zanik podświetlenia postaci (`+0x480..+0x48C`, pola zapisywane w
  save) liczony jest na każdą narysowaną klatkę — kosmetyczny, różnicuje hasz.
- Wyniki (nagranie vs powtórki tych samych zdarzeń): bez klatek pośrednich (s4) — 3 przebiegi identyczne w 100 %
  (1504 ticki, 2794 losowania, koniec misji w tym samym ticku); z klatkami (s3, s5) — identyczne poza zanikiem
  (s5: 3141 losowań i 2591 rozkazów 1:1); powtórka BEZ klatek nagrania Z klatkami — logika identyczna → liczba
  klatek nie wpływa na symulację.
- Bez osobnego generatora dla dźwięku (s6) rozjazd już po ~2 s gry (tick 53) — izolacja jest konieczna.
- Kamera wpływa: powtórka z kamerą trzymaną daleko rozjechała się w ticku 679 (start gestu miecza, inna gałąź walki).
  Gra liczy „co jest pod kursorem” w rysowaniu (`0x004DDD50` → `+0x92/+0xA0` elementu) i obsługa myszy z tego
  korzysta → w co-op cel akcji musi rozstrzygać maszyna wydająca polecenie i wysyłać id. Rzadkie rozjazdy (2 na 8
  powtórek z kamerą z logu) pasują do tego mechanizmu.

## 3. Ocena
- Lockstep (wymiana samych poleceń) — realny. Brakuje: sieć (sockety w DLL), harmonogram ticków z opóźnieniem
  wejścia, podmiana zaznaczenia/tłumacza per gracz i filtr własności postaci, osobny generator dźwięku, ziarno i
  `srand` przy zapisach, okna modalne/pauza/odprawy/koniec misji wspólne dla wszystkich, obóz (drużyna wybierana
  przez hosta), wykrywanie rozjazdu haszem + ponowna synchronizacja wczytaniem zapisu hosta. Szacunek: ~2–4 mies.
- Stan od hosta (host-authoritative) — replikacja ~15 klas elementów, AI, sekwencji, skryptów, pocisków, HUD-u
  i tryb „marionetki” bez ticku na klientach: ~6–12 mies., duże ryzyko.
- Zalecenie: lockstep + kontrola haszu co N ticków + resync z zapisu gry (gra umie zapisać i wczytać stan, a odczyt
  ustawia ziarno z pliku).
