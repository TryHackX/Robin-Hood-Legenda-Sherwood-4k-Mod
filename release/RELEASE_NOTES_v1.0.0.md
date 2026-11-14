Pierwsze publiczne wydanie moda do **Robin Hood: Legenda Sherwood** (wersja GOG).

## Instalacja
1. Rozpakuj archiwum.
2. Skopiuj `winmm.dll` i `SherwoodFix.ini` do folderu gry (obok `Game.exe`).
3. Uruchom grę jak zwykle. Ustawienia moda w grze: **F10**.

Odinstalowanie: usuń `winmm.dll`. Na monitorze 1080p ustaw w `SherwoodFix.ini` `Width=1920` i `Height=1080`.

## Najważniejsze
- Własny DirectDraw w pamięci + Direct3D 11: bez lagów wrappera, okno bez ramek, poprawne Alt+Tab, VSync.
- Płynne przewijanie mapy, kursor i ruch postaci z częstotliwością monitora (logika gry nadal 25 Hz).
- Tryb HD (2560×1440): mapa w powiększeniu 2,0, ostry interfejs, mapa do dołu ekranu.
- Obsługa grafiki HD (tła map, postacie, obiekty, ikony, tła menu) i klatek pośrednich animacji — obrazy robi się
  samemu z własnej kopii gry narzędziami z `tools\hd\` (nie są dołączone: grafika gry jest chroniona prawem autorskim).
- Interfejs misji ×2, minimapa w rogu (przeciąganie po niej przesuwa kamerę, okno za brzegi — lewym lub prawym),
  menu i okna gry na cały ekran, panel ustawień F10.
- Poprawne informacje o procesorze i pamięci w opcjach gry.

## Znane ograniczenia
- Zoom: silnik ma tylko poziomy 0,5 / 1 / 2; na poziomach węższych niż 2560 px oddalenie do 1,0 zostawia czarny pas.
- Grafika HD działa w powiększeniu 2,0. Poziomy nocne i mgła nie są jeszcze sprawdzone z HD.

Szczegóły: `README.md` i `docs\` w repozytorium.
