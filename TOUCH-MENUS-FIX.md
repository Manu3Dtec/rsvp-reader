# Touch und Reader-Menüs — 2026-09-21

WPM und Kapitelübersicht öffnen über LV_EVENT_CLICKED ohne die bisherige
Haltezeit von 1000 ms. Die Halte- und Freigabetimer wurden entfernt.

Hauptmenü-Karten verwerfen Klicks, sobald sich der Finger während der Geste
mehr als 10 Pixel vom Startpunkt entfernt hat, auch nach Rückkehr zum Start.
Der Scrollstart liegt bei 6 Pixeln, um kleine Koordinatenschwankungen zu tolerieren.

Neue Kontakte und große Koordinatensprünge benötigen eine zweite nahe Messung
innerhalb von 40 ms. Kurze Kontaktlücken werden bis 25 ms überbrückt.
Die erlaubte Bewegung berücksichtigt längere Abfrageintervalle während
eines vollständigen Bildschirmaufbaus.
Verworfene Sprünge verlängern den letzten gültigen Kontakt nicht mehr endlos.
I2C-Timeouts sind auf jeweils 8 ms begrenzt, damit Fehler die GUI nicht für
bis zu 100 ms blockieren.

Build über den vorhandenen IDF-5.5.2-Buildordner in DISPLAY-TEST. Komponenten,
Konfiguration und übrige Hauptquellen stimmen mit RC3-CLEAN überein.
Die aktuelle main.cpp aus RC3-CLEAN wurde übernommen. Die vorherige Datei
ist unter main/main.cpp.before-rc3-sync.bak gesichert. Der separate vollständige
Neubau in RC3-CLEAN/build-idf55 wurde zugunsten dieses inkrementellen Builds
abgebrochen.

## Prüfung am Gerät (noch offen)

- WPM und Kapitelanzeige kurz antippen: direkt nach Loslassen öffnen.
- Hauptmenü langsam und schnell in beide Richtungen scrollen.
- Am Listenende weiterwischen und zum Startpunkt zurückwischen: keine Aktion.
- Während eines Wischens den Kontakt kurz verlieren: keine neue Menüauswahl.
- Normaler kurzer Tipp auf jede Hauptmenü-Karte: Aktion genau einmal.
- Gerät ohne Berührung liegen lassen: keine selbsttätigen Aktionen.

Build-Ergebnis und Firmware-Hash stehen in firmware-idf55/STATUS.txt.

## Nachbesserung Start/Stop

Die I2C-Abfrage und Kontaktfilterung laufen nun in einer eigenen FreeRTOS-Task
auf Core 1. Eine Queue mit 64 Einträgen übergibt Zustandswechsel und Bewegungen
an LVGL. Das verhindert, dass ein kompletter kurzer Tipp während eines
Vollbildaufbaus (in bisherigen Logs etwa 140 ms) unbemerkt bleibt. Nur die
GUI-Task führt LVGL-Aufrufe aus. Bei überlanger Blockierung wird der älteste
Queue-Eintrag verworfen, damit die aktuelle Freigabe weiterhin ankommt.

Start/Stop reagiert einmal auf LV_EVENT_PRESSED nach der Kontaktbestätigung.
Die bisherige CLICKED-Registrierung wird entfernt, damit Loslassen keine zweite
Aktion auslöst. Die unsichtbare Trefferfläche wächst um 16 Pixel pro Seite.
Die serielle Ausgabe protokolliert Start/Pause und die Dauer der Aktion.

Zusätzliche Gerätetests: Einschalten → Weiterlesen → sofort kurz Start antippen;
bei laufender Wortanzeige kurz pausieren; länger gedrückt halten (nur eine
Aktion); nahe am Rand des Startbuttons tippen; Hauptmenü weiterhin scrollen.
