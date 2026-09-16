# Bedienung und Hardware

**🇩🇪 Deutsch** | [🇬🇧 English](en/BEDIENUNG.md)


## Hardware wie in V73

| Teil | Anschluss |
|---|---|
| ESP32-C3 | 4 MB Flash, USB CDC |
| SH1106 OLED | 128 × 64, I²C 0x3C |
| BMP280 | I²C 0x76 |
| SDA / SCL | GPIO8 / GPIO9 |
| Encoder A / B | GPIO0 / GPIO1 |
| Encoder drücken | GPIO3 |
| BAK / CONTR | GPIO4 / GPIO5 |

Der Sketch verwendet die BMP280-Bibliothek. Ein BME280 ist kein ungeprüfter
Austausch: Feuchte wird nicht erfasst. Die vorhandene Verdrahtung nicht mit
Pinbelegungen aus anderen ESP32-Projekten vermischen.

## Acht neue OLED-Seiten

1. **Dashboard:** große Temperatur mit °C, Luftdruck und Zeit bis zur nächsten Speicherung.
2. **Temperatur:** Minimum, Maximum und Trend. „Ruhig: 60 Werte“ bedeutet maximal
   0,2 °C Spannweite in den letzten 60 Sensorablesungen; keine normgerechte Stabilitätsprüfung.
3. **Verlauf:** automatisch skalierter Temperatur-/Druckverlauf mit Min-/Max-Skala.
   V74.3 ergänzt 1/6/12/24 Stunden; siehe VERLAUF.md.
4. **Luftdruck:** große Zahl mit hPa und Trend.
5. **Aufzeichnung:** Anzahl gespeicherter lokaler Messpunkte, freier Flash und Füllbalken.
6. **Kalibrierung:** Rohwert und eingestellte Temperatur-/Druckkorrekturen.
7. **Verbindungen:** WLAN-Pegel, MQTT-Verbindung, Funkkanal und aktive Nodes.
8. **Diagnose:** Sensorstatus, verworfene Empfangspakete und Zustand der Zeitbasis.

Encoder drehen: Seite wechseln. Encoder oder CONTR drücken: Menü. Auf der Grafikseite in V74.3 gilt die Sonderbedienung aus VERLAUF.md.
Im Menü BAK zurück. „Screen Auto“ wechselt alle 8 Sekunden die Seite;
Drehen setzt diese Wartezeit zurück. Ein fester Screen bleibt stehen.
Der bisherige DE/EN-Modus bleibt erhalten.

Das OLED überträgt nach dem ersten Bild nur veränderte 8-Pixel-Kacheln. Unveränderte
Seiten werden nicht fortwährend neu gesendet. Im Deep-Sleep wecken die drei Taster;
die Encoder-A/B-Leitungen wurden aus den Weckquellen entfernt, damit deren
Ruheposition keine dauernde Neustartschleife auslöst.

`images/oled-v74-overview.png` zeigt die echte Zeichenlogik mit den verwendeten
U8g2-Schriften und simulierten Messwerten, vergrößert dargestellt. Kein Hardwarefoto.

## Behobene Fehler und neues Verhalten

- Fehlerhafte, zu lange oder unvollständige serielle Zeilen stoppen die GUI nicht mehr.
- Empfangsfehler schließen den betroffenen Port; alte Thread-Ereignisse werden durch
  eine Verbindungskennung von der neuen Verbindung getrennt.
- Nur eine periodische Diagrammaktualisierung; keine zusätzlichen Timer bei jedem Klick.
- Empfangspakete werden im WLAN-Callback nur in eine Warteschlange kopiert.
  Dateizugriffe, Ausgaben und MQTT laufen anschließend in der Hauptschleife.
- Alle dokumentierten `set mqtt...`-Befehle werden verarbeitet; die MQTT-Webbuttons
  sind mit den passenden Befehlen verbunden. PubSubClient wird ausdrücklich eingebunden.
- MQTT meldet keine erfundenen 0-°C-Werte für ungültige Sensorablesungen.
  Geräteausfall über Last Will; Gateway-Ausfall beeinflusst auch seine Nodes.
- Fern-Loggingstatus wird aus dem Sensorpaket gelesen, nicht aus dem Gateway übernommen.
- TCP/MQTT-Verbindungswartezeiten begrenzt. Verbindungsversuche sind weiterhin
  synchron; die Firmware ist kein harter Echtzeitlogger.
- Kommas in WLAN-/MQTT-Passwörtern werden nicht mehr in Punkte umgewandelt.
  Zugangsdaten werden beim Senden über die GUI im Terminal maskiert.
- Speicherreserve und vollständige Schreibresultate werden geprüft. Bei einem
  Fehler steigt der lokale Logzähler nicht fälschlich weiter. Keine automatische Formatierung.
- Neue Journalzeilen tragen eine FNV-1a-Prüfsumme zur Erkennung beschädigter Datensätze
  (keine kryptografische Authentifizierung).
- Funkübertragung gespeicherter Daten mit Quittung und Wiederholungen. Der Gateway
  bestätigt gespeicherte Pakete nach dem Dateischreiben. Bei Ausfall bleiben sie im
  lokalen Journal; Nachlieferung erfolgt bei wieder erreichbarem Gateway.
- Ein reiner Sensor kann ohne Routerverbindung nach dem Gateway-Funkkanal suchen.
  Bei Routerverbindung gilt dessen Kanal; beide Geräte müssen denselben Kanal verwenden.
- Lokale Datensätze und am Gateway empfangene Datensätze können über USB nachgeladen werden.
- Sensorfehler werden kenntlich gemacht; die Sensorinitialisierung wird wiederholt.

## Messarchiv und Zeit

Neue Dateien auf dem ESP32:
- `/local_v74.csv`: lokale V74-Messungen.
- `/v74_<NODE-ID>.csv`: am Gateway gespeicherte V74-Messungen.

Die alten `/local_log.csv`- und `/node_<ID>.csv`-Dateien werden nicht automatisch
umgeschrieben oder gelöscht. Die alte lokale Datei lässt sich über den Web-Link
„Alte V73 CSV“ oder `dump legacy` ausgeben. Alte Gatewaydateien vor dem Update
exportieren; die neuen Downloadbuttons adressieren V74-Dateien.

PC-Archiv: `%LOCALAPPDATA%\BMP280_Logger_V74\measurements.sqlite3`.
Es speichert empfangene gültige Werte dauerhaft. „Archiv CSV exportieren“ gibt
alle archivierten Datensätze aus. In den Diagrammen stehen bis zu **20.000 Werte
pro Node**, nach Neustart zunächst die letzten 20.000 archivierten Datensätze
insgesamt. Das Archiv enthält darüber hinausgehende Messungen weiterhin.

V74 führt Geräte-ID, Boot-Sitzung, Sequenz, relative Laufzeit und – wenn vorhanden –
UTC-Messzeit mit. Die GUI setzt die Zeit des direkt verbundenen Geräts; WLAN-Geräte
können sie zusätzlich per NTP beziehen. Ohne Uhrsynchronisierung sind absolute
Zeitzuordnungen **geschätzt** und entsprechend beschriftet. Das gilt insbesondere
für ältere, erst später ausgelesene Offline-Messungen. Ein Deep-Sleep-Zyklus behält
Sitzung und Sequenz; ein vollständiger Stromverlust beginnt eine neue Sitzung.

Kurven verwenden Messzeiten statt Punktnummern; lange Zeitlücken und neue Sitzungen
werden unterbrochen dargestellt. Ein nachträglicher Uhrsprung kann die chronologische
Darstellung beeinflussen. Für zeitlich genaue Vergleiche alle Geräte vor der Messung synchronisieren.

## Speicher und Wiederherstellung

Die Einstellung „Max Tage“ begrenzt die Messpunktzahl rechnerisch. Sie garantiert
nicht, dass der Flash für sieben Tage reicht: Intervall, Dateigröße, alte Dateien und
Anzahl der Nodes bestimmen die Kapazität. Kein automatisches Überschreiben alter Messungen.
Bei vollem Speicher zuerst Dateien exportieren und anschließend gezielt löschen.
Der Status zeigt den Fehler an; die gewünschte Logging-Einstellung allein ist kein
Nachweis, dass gerade geschrieben werden kann.

Ein neues/unformatiertes Gerät meldet gegebenenfalls FS FEHLER. **Nur wenn dort keine
benötigten Dateien liegen**, im Terminal `formatfs confirm` ausführen. Dieser Befehl
formatiert den gesamten Messdateibereich einschließlich alter und Gateway-Dateien.
Bei einem vorhandenen Gerät nicht als erste Fehlerbehebung formatieren.

Funk-Nachlieferung ist auf noch vorhandene lokale Dateien begrenzt. Ein Löschen der
lokalen Datei verwirft auch ihre noch nicht bestätigten Funkdaten. Livewerte bei
angehaltener lokaler Aufzeichnung werden nicht als nachlieferbares Archiv geführt.
Die Zahl verschlüsselter ESP-NOW-Peers hängt vom verwendeten ESP32-Core ab und kann
unter den 20 verwaltbaren Vertrauenseinträgen liegen.

