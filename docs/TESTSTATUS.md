# Teststatus V74.3 – Vorabversion

Die Tests in diesem Repository prüfen Hostcode und extrahierte Firmwarefunktionen.
Sie ersetzen keinen vollständigen ESP32-Build oder Hardwaretest.

- Python: 12 Tests zu Protokoll, Archiv und Verbindungswechseln.
- C++: Journal, Funkquittungen und Wiederholung.
- C++: USB-Ausgabepuffer mit blockiertem Empfänger und Wiederaufnahme.
- C++: Stundenverlauf und Zeichenkoordinaten (96 Szenarien).

Noch offen: vollständiger V74.3-Build mit Core 3.3.11, gemessene Flash-/RAM-Belegung,
Upload, Betrieb ohne PC, Windows-GUI am Gerät, 24-Stunden-Lauf, Stromausfall und
mehrere reale Funk-Nodes. V74.1 wurde früher erfolgreich kompiliert; dieses Ergebnis
ist kein Buildnachweis für V74.3. Die öffentliche Fassung verwendet zudem einen
neutralen Beispielwert für PAIR_SECRET.

## Abnahme am Gerät

1. Mit Core 3.3.11, USB CDC und normaler Optimierung kompilieren; Größen notieren.
2. Ohne Löschen des Flashs hochladen; vorhandene Messdaten vorher exportieren.
3. GUI verbinden und trennen: Encoder, Display und Messung müssen weiterlaufen.
4. Direkt am Netzteil starten; SENSOR/HYBRID, NORMAL, Logging AN prüfen.
5. Temperatur/Druck und 1/6/12/24h umschalten; 24h Betrieb und CSV-Export prüfen.
6. Neustart: CSV bleibt erhalten, RAM-Verlauf beginnt neu.
