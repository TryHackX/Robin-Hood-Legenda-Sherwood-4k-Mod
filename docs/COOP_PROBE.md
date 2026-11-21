# Co-op (lockstep) — wynik rozpoznania, 2026-11-19 (sesja poboczna)

Stan: **temat odłożony przez użytkownika 2026-11-19** („na razie zostawiamy, zachowaj wszystkie informacje i plan”).
Sonda i narzędzia są w repozytorium (commit e4cd553), działają tylko w teście w tle (`SHERWOODFIX_TEST=1`); zwykła gra
nie ma żadnych haków sondy.

## Ustalenia z rozmowy z użytkownikiem (2026-11-18/19)

**Czego chce użytkownik (wzór: Noita Entangled Worlds, mod kooperacji do jednoosobowej Noity):**
- każdy gracz ma własną kamerę i własne zaznaczenie postaci;
- gospodarz wybiera drużynę (w obozie);
- postaci, którą steruje jeden gracz, drugi nie może zaznaczyć — każdy gra swoimi, bez wchodzenia sobie w drogę;
- świat (wrogowie, skrypty misji) liczy gospodarz.

**Fakty o grze:**
- `Game.exe` nie ma żadnego kodu sieciowego (brak importów Winsock / DirectPlay; w pliku są tylko nazwy błędów
  DirectPlay z ogólnej tabeli błędów DirectX) — sieć trzeba dopisać w DLL od zera.
- Noita ma oficjalne API modów w Lua (Entangled Worlds na nim stoi); Robin Hood nie ma żadnego API — wszystko przez
  inżynierię wsteczną kodu maszynowego, jak grafika moda.
- Kamera nie wpływa na symulację (każdy gracz może mieć swoją), ALE „co jest pod kursorem” gra liczy przy rysowaniu
  (`0x004DDD50`), więc cel kliknięcia musi rozstrzygać komputer wydający polecenie i wysyłać jego identyfikator.
- Jedna lista zaznaczonych (`E+0x49F8`) i jeden stan tłumacza wejścia na grę → w co-op podstawiane na czas polecenia
  danego gracza; filtr „postać ma właściciela” przy zaznaczaniu (`SelectPC 0x004DBF20`).
- Okna dialogowe (odprawy, pytania, menu Esc) mają własną pętlę (`0x0053C390`) i zatrzymują grę; gra pauzuje, gdy jej
  okno traci fokus (pętla komunikatów `0x005E1380` / `GetMessage` przy fladze nieaktywności) — w co-op okno u jednego
  gracza nie może zamrażać drugiego.
- Dwie kopie gry na jednym komputerze: pierwszy `Game.exe` trzyma `Data\RobinHood.bks` na wyłączność, druga kopia pada
  przy starcie misji — do lokalnych testów trzeba to obejść (np. hak na tryb udostępniania pliku).
- Gesty mieczem to dane wejściowe jak każde inne — w lockstepie wysyłany jest wynik rozpoznania albo całe zdarzenie.

**Dwie drogi:**
1. Lockstep — oba komputery liczą tę samą grę, wymieniane są tylko polecenia graczy (z opóźnieniem wejścia, w
   ustalonym ticku). Wymaga powtarzalnej symulacji — sonda potwierdziła, że jest (warunki niżej). Zalecana.
2. Stan od gospodarza (host-authoritative, jak wrogowie w Entangled Worlds) — klient jako „marionetka” bez własnej
   logiki, gospodarz wysyła stan wszystkich elementów (~15 klas, SI, sekwencje, skrypty, pociski, HUD). Kilka razy
   większa i bardziej ryzykowna.

**Ocena w naszych warunkach (nie „miesiące programisty”):** raport niżej podaje lockstep ~2–4 mies. jako czas
programisty-człowieka na etat. U nas wąskim gardłem jest tygodniowy limit zużycia i testy we dwóch, nie godziny:
sonda w świeżej sesji (ponad tysiąc kroków) zużyła ok. 2–3% limitu tygodniowego; bardzo długa sesja jest droga (każdy
krok czyta cały kontekst). Lockstep to z grubsza kilkanaście–kilkadziesiąt świeżych sesji roboczych, czyli ok. 1–2
pełne tygodniowe limity; w kalendarzu kilka tygodni do ~2 miesięcy, zależnie od tego, jak często da się testować we
dwóch. Wersja z gospodarzem — kilka razy więcej, odradzana.

**Gotowy netcode? (pytanie użytkownika 2026-11-22: „gotowy netcode, a my tylko adresy i obsługa serwer–klient, jak w
Noicie”)** — tylko na warstwę transportu: wysyłanie pakietów, połączenie, niezawodność, NAT. Kandydaci: ENet (prosty
UDP z niezawodnością), GameNetworkingSockets (Valve, przekaźniki), Steam P2P odpada (gra z GOG); albo bez NAT-u — VPN
u graczy (Radmin VPN / ZeroTier / Tailscale) i zwykłe połączenie po IP. Biblioteki „całej synchronizacji” typu GGPO
(rollback) nie pasują: wymagają zapisu i cofnięcia całego stanu gry w każdej klatce i przeliczenia kilku ticków —
ta gra tego szybko nie umie (setki elementów, sekwencje, skrypty misji). Lockstep (wymiana poleceń z opóźnieniem 2–3
ticków = 80–120 ms) piszemy sami — mała pętla, symulacja jest powtarzalna (sonda). Entangled Worlds: Noita ma API
modów w Lua, a sieć robi osobny program-proxy połączony z grą lokalnie — tę architekturę da się powtórzyć (DLL ↔
proxy), ale to też tylko transport. Biblioteka skraca głównie kamień 7 (internet / NAT) i część 2 — razem może
10–15 % pracy; reszta (kamienie 1–6, 8) to praca na samej grze, jak w planie.

**Alternatywa bez pracy:** Parsec (darmowy) — jedna osoba uruchamia grę, druga łączy się i może sterować myszą i
klawiaturą (wspólne sterowanie, nie osobne kamery).

**Plan, gdyby temat wrócił (kolejne kamienie milowe):**
1. Dwie kopie gry na jednym komputerze (obejście blokady `RobinHood.bks`), połączone przez localhost; ta sama misja,
   ten sam ziarno losowania, osobny generator dla dźwięku.
2. Wymiana poleceń z opóźnieniem wejścia i harmonogram ticków; obie kopie ruszają się identycznie (kontrola haszu stanu
   co N ticków, narzędzia `tools\coop_run.ps1` / `tools\coop_compare.py`).
3. Zaznaczanie i tłumacz wejścia na gracza, filtr właściciela postaci, własna kamera.
4. Wspólne pauzy, okna dialogowe, odprawy i koniec misji; brak pauzy przy utracie fokusu w trybie co-op.
5. Obóz i kampania: drużynę wybiera gospodarz, start misji u wszystkich; zapisy.
6. Rozjazd → ponowna synchronizacja wczytaniem zapisu gospodarza (gra umie zapisać i wczytać stan, odczyt ustawia ziarno).
7. Sieć przez internet (NAT): pośrednik / przekierowanie portu; panel F10 lub menu: gospodarz / dołącz.
8. Testy we dwóch: logi, rundy poprawek.

---

Raport sondy (sesja poboczna, 2026-11-19):

Tylko badanie. Nic nie wydane, nic nie zacommitowane (stan z chwili raportu; potem zatwierdzone jako e4cd553).
Zwykła gra bez zmian zachowania: sonda działa wyłącznie przy `SHERWOODFIX_TEST=1` (test w tle). Wgrany `winmm.dll`
zawiera sondę, ale bez tej zmiennej nie zakłada żadnego haka.

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
