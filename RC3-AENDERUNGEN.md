# RC3 – saubere Display-/Touchbasis und echte PWR-Abschaltung

Änderungen gegenüber dem gelieferten RC2.3-Paket:
- Standby-/Lockscreen-Oberfläche vollständig entfernt.
- PWR: neuer Hold ab 2 Sekunden zeigt OFF; Abschaltung nach Loslassen.
- Einschalten per PWR: Hold wird geprüft, ON kurz angezeigt; der Einschalthold löst kein OFF aus.
- SYS_EN wird beim Ausschalten deaktiviert; bei USB-Versorgung folgt zusätzlich Deep Sleep mit PWR-Wakeup.
- Kurzer BOOT-Druck führt ins Hauptmenü und beendet den Reader.
- Tastenerkennung bleibt auch bei fehlender Akku-ADC-Telemetrie verfügbar.
- Menü-Erstellung wartet auf den LVGL-Mutex (kein Abbruch durch lock(0)-Race).
- Touch-Kommunikation nach offiziellem Espressif-Treiber: 14-Byte-Abfrage und Antwort für zwei Kontaktberichte, getrennte I2C-Schreib-/Lesetransaktionen, 300 kHz, 10-ms-Abfrage. Byte 0 ist ein Gestenfeld; kein fester C0-Filter.
- UP-Ereignis und Überbrückung ungültiger Abfragen bis höchstens 60 ms aus letzter Touch-Auswertung erhalten. BSP liefert native Koordinaten (raw_y, 639-raw_x). LVGL 9.5 dreht Eingabepunkte automatisch um 270°; daraus entstehen genau einmal die Bildschirmkoordinaten (639-raw_x, 171-raw_y). Die vorherige doppelte Drehung erzeugte negative Y-Koordinaten und verhinderte die Bedienung. Nach der Korrektur der doppelten Drehung wurde Tippen am Gerät bestätigt, aber mit ungenauer/verzögerter Reaktion. Wiederholt gemeldete Zweikontaktpakete werden jetzt über den ersten Kontakt ausgewertet; Zuverlässigkeit dieser Folgekorrektur noch zu bestätigen.
- LVGL-Software-Rotator statt eigener Pixelrotation; DMA-Puffer wird erst nach Transferende überschrieben.
- Display-On/Off-Boolsemantik im AXS15231B-Treiber korrigiert.
- Nachmeldung zur fehlenden Anzeige: GPIO42-PWM ist aktiv-low. Die Werksdefinition LCD_PWM_MODE_255 ergibt 0, nicht 255. Helligkeitsumrechnung entsprechend korrigiert; RC-FAST-Taktquelle übernommen.
- Startdiagnose vom Benutzer bestätigt: grünes QSPI-Testbild und Menü sichtbar. Testbild anschließend aus dem Startablauf entfernt; die ersten fünf LVGL-Bilder protokollieren weiterhin Format und gefüllte Pixel.
- Reader: Schrift bis 64 px, WPM und Batterie räumlich getrennt, Kopfzeile von Readergesten ausgeschlossen.
- Readergesten starten ausschließlich dort, wo die Berührung begonnen hat; pro Berührung eine Aktion.
- Stromsparen: dynamischer CPU-Takt 80–240 MHz; WLAN ausschließlich beim Upload; Display aus bei Inaktivität gemäß Einstellung.

Prüfung:
- Aktueller Stand: vollständiger ESP-IDF-5.5.2-Build und Partition-/Größenprüfung. Passende Flashdateien in firmware-idf55; build/ enthält ein älteres IDF-6-Image.
- Physische Touch-, Ausrichtungs- und PWR-Prüfung nach Flash erforderlich; eine Akkulaufzeitmessung wurde nicht durchgeführt.
- Anzeige der Diagnoseversion am 16.09.2026 physisch bestätigt. Rückfall-App: RC3-display-confirmed-diagnostic.bin (gehört zu den RC3-Bootloader-/Partition-Dateien).
- ON/OFF und Menü vom Benutzer bestätigt. Beleuchtung beim Start erst nach vollständigem ON-Bildtransfer einschalten, damit kein altes Panelbild sichtbar wird.

Arbeitsverzeichnis: Simple-RSVP-Reader-3.49B-V2-RC3-CLEAN
Die unveränderte Waveshare-Werksfirmware bleibt als Rückfall-Binärdatei in waveshare-v2/Firmware erhalten.

## Scroll-Verzögerung

Touch-Timer von 30 auf 10 ms verkürzt. LVGL-Task respektiert Timerfristen mit 2 statt 10 ms Mindestpause. CPU-Frequenz während des LVGL-Handlers per PM-Lock auf Maximum gesetzt; vor der Taskpause wieder freigegeben. Native Koordinaten und Zweikontakt-Auswertung bleiben erhalten. Laufzeitprotokoll meldet alle fünf Sekunden die maximale Handlerdauer. Build und Flash-Hash geprüft; physische Wirkung auf Scrollen noch zu bestätigen. Vorheriger Stand in `versions/rc3-touch-before-scroll-latency` gesichert. Keine Akkulaufzeitmessung.

## Reader-Gesten über direkte Touch-Ereignisse

Scrollen im Hauptmenü wurde vom Nutzer bestätigt. Reader-Gesten werden jetzt direkt über PRESSED/PRESSING/RELEASED am Readerbereich verarbeitet; der separate 12-ms-Zustandsabfragetimer wurde entfernt. Auch der letzte Punkt im Loslassereignis zählt für schnelle Wischbewegungen. Die Gestenfläche beginnt bei y=57; der Zurückbutton endet bei y=54 und ist getrennt. PRESS_LOCK hält eine begonnene Readerberührung auf ihrer ursprünglichen Fläche. Fokusmarkierungen sind nicht anklickbar.

Tippen: Start/Pause beim Loslassen, maximal 16 Pixel Bewegung, 20 bis 700 ms Dauer. Wischen: mindestens 22 Pixel und dominante Achse (Verhältnis mindestens 1,5), pro Berührung einmal. Links: nächstes Wort. Rechts: vorheriges Wort. Oben: +25 WPM. Unten: -25 WPM. Ein Wischen kann nicht beim Loslassen zusätzlich Start/Pause auslösen. Unklare diagonale Bewegungen und längeres Halten lösen kein Tippen aus. Physische Reader-Prüfung nach dem Flash steht noch aus.


## Video IMG_9397 – Touch, Reader und EPUB-Kapitel

Die 40,56-s-Aufnahme wurde in 2-s-Kontaktbogen und Einzelbilder zerlegt. Sichtbar: Ein Finger berührt links die Kapitelliste, während der rechts liegende Slider nacheinander ungefähr 69 %, 89 % und 0 % meldet. Der Touchpunkt springt damit während einer zusammenhängenden Berührung zwischen weit entfernten Zielen.

- AXS15231B: Bei zwei gemeldeten Kontakten wird während einer laufenden Berührung der Punkt gewählt, der dem letzten gültigen Punkt am nächsten liegt. Sprünge von mehr als 48 nativen X- bzw. 96 nativen Y-Pixeln innerhalb eines 10-ms-Kontakts werden verworfen. Ein UP behält den letzten gültigen Punkt.
- Reader: Der Zustands-Abfragetimer mit den engen Bedingungen (7 Messungen, 90–400 ms, maximal 7 Pixel) wurde entfernt. Direkte LVGL-Ereignisse akzeptieren Taps von 20–1200 ms und bis 18 Pixel Bewegung. Vertikales Wischen ab 24 Pixel ändert WPM einmal; Wortnavigation bleibt auf den großen Tasten. PRESS_LOCK hält die begonnene Readerberührung auf der Readerfläche. Fokusmarkierungen fangen keine Eingaben ab.
- Display: PARTIAL statt FULL; 40 logische Zeilen pro Zeichenpuffer. Rotation und QSPI-Übertragung erfassen nur die ungültige Teilfläche. Damit entfällt die bisherige Vollbildverarbeitung von 110080 Pixeln bei jedem Wort, Button oder Sliderwert (zuvor im Mitschnitt bis etwa 125 ms Handlerzeit).
- EPUB: Kapitel werden aus EPUB-3-nav oder EPUB-2-NCX samt Ankern und Titeln erzeugt. Spine-Dateigrenzen dienen nur noch als Fallback. Metadatenformat `RSVPCH2`; ältere Cachedateien werden beim nächsten Öffnen automatisch neu aufgebaut. Der Kapitelslider ist 28 statt 18 Pixel hoch.

Sicherung vor diesen Änderungen: `versions/rc3-video-9397-before-fix`. Build-, Flash- und physische Geräteprüfung werden separat protokolliert.
