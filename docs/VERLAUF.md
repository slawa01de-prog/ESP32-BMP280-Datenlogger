BMP280 Logger V74.3 – OLED-Verlauf 1/6/12/24 Stunden

Installation:
ZIP entpacken. Esp32_LCD_Datalogger/Esp32_LCD_Datalogger.ino öffnen.
Vollständiger Sketch in einer Datei, keine LoggerConsole.h erforderlich.
Bisherige GUI der V74.2 bleibt verwendbar.
ESP32-C3, Core 3.3.11, USB CDC aktiviert, normale Optimierung (-Os).
Partitionierung unverändert lassen; Erase All Flash deaktiviert lassen.
Kein fertig kompiliertes Firmware-Abbild enthalten.

Bedienung auf der Grafikseite (Seite 3):
- Encoder drücken (OK): Temperatur in C / Luftdruck in hPa wechseln.
- BAK kurz drücken: 1h -> 6h -> 12h -> 24h -> 1h.
- CONTR drücken: Menü öffnen.
- Encoder drehen: weiterhin die Anzeigeseite wechseln.
Nach Wechsel von Messgröße oder Zeitraum bleibt die Grafikseite fest stehen.
Messgröße und Zeitfenster werden gespeichert.
Temperatur/Druck sind zusätzlich über den bestehenden Menüpunkt Graph wählbar.
Terminal: set graph temp / set graph press; set graph_hours 1/6/12/24.

Darstellung und Daten:
Feste Zeitachse von -1/-6/-12/-24h bis jetzt. Linke Skala automatisch.
Ein letzter Messwert je Minute wird für beide Messgrößen vorgehalten.
Bei langen Zeitfenstern werden die Minima/Maxima dieser Minutenwerte je
Displayspalte gezeichnet. Sekundenkurze Spitzen zwischen Minutenwerten können
entfallen. Es handelt sich nicht um einen vollständigen Rohdatenplot.
Ohne Daten bleibt der entsprechende Zeitabschnitt leer; neue Werte stehen rechts.
Die bisherigen 96 Ablesungen für die Kurzzeitstatistik bleiben zusätzlich erhalten.
Der Stundenverlauf ist ein RAM-Anzeigespeicher des laufenden Betriebs:
Nach Neustart, Stromverlust, Deep-Sleep-Neustart oder Statistik-Reset beginnt er neu.
Alte CSV-Messungen werden nicht in diese OLED-Grafik nachgeladen und bleiben
unverändert gespeichert. Für einen durchgehenden 24h-Verlauf NORMAL oder ECO
verwenden. Bei ECO lässt sich das abgeschaltete Display per Taste wecken.

Prüfung:
Die tatsächliche History-Klasse und Zeichenfunktion wurden auf dem Host mit
simuliertem OLED getestet: 96 Kombinationen inklusive leerer/kurzer/24h-Daten,
Temperatur/Druck, 4 Zeitfenster, DE/EN, negative Werte und Datenlücken.
Die USB-Korrektur aus V74.2 ist enthalten.
Der vollständige ESP32-Build und ein Test am physischen Display sind noch offen.
Die Testausgabe ist beigelegt. Speicherbelegung bitte beim Kompilieren prüfen.
