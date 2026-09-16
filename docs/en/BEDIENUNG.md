# Operation and hardware

[🇩🇪 Deutsch](../BEDIENUNG.md) | **🇬🇧 English**

## Hardware as in V73

| Part | Connection |
|---|---|
| ESP32-C3 | 4 MB flash, USB CDC |
| SH1106 OLED | 128 × 64, I²C 0x3C |
| BMP280 | I²C 0x76 |
| SDA / SCL | GPIO8 / GPIO9 |
| Encoder A / B | GPIO0 / GPIO1 |
| Encoder push | GPIO3 |
| BAK / CONTR | GPIO4 / GPIO5 |

The sketch uses the BMP280 library. A BME280 is not a tested drop-in replacement; humidity is not recorded.
Do not mix this wiring with pin assignments from other ESP32 projects.
[Detailed wiring diagram](HARDWARE.md).

## Eight OLED pages

1. **Dashboard:** large temperature with °C, pressure and time until the next stored reading.
2. **Temperature:** minimum, maximum and trend. “Ruhig: 60 Werte” means a range of at most 0.2 °C across the last 60 sensor readings, not a standards-compliant stability test.
3. **History:** automatically scaled temperature/pressure history with min/max scale. V74.3 adds 1/6/12/24 hours; see [history](VERLAUF.md).
4. **Pressure:** large hPa reading and trend.
5. **Recording:** number of locally stored samples, free flash and fill bar.
6. **Calibration:** raw value and configured temperature/pressure corrections.
7. **Connections:** Wi-Fi signal level, MQTT connection, radio channel and active nodes.
8. **Diagnostics:** sensor status, dropped receive packets and timebase status.

Rotate the encoder to change pages. Press the encoder or CONTR to open the menu, except on the V74.3 graph page, which uses the special [graph controls](VERLAUF.md).
In the menu, BAK goes back. “Screen Auto” changes pages every 8 seconds; rotating the encoder resets this delay. A fixed screen remains selected.
The existing DE/EN mode is retained.

After the first frame, the OLED sends only changed 8-pixel tiles. Unchanged pages are not continuously retransmitted.
During deep sleep, the three pushbuttons can wake the device. Encoder A/B were removed from wake sources so their resting positions cannot cause a continuous reboot loop.

[OLED overview](../images/oled-v74-overview.png) shows the actual drawing logic using the selected U8g2 fonts and simulated readings, enlarged for viewing; it is not a hardware photograph.

## Fixes and behaviour changes

- Malformed, excessively long or incomplete serial lines no longer stop the GUI.
- Receive errors close the affected port. A connection identifier separates events from an old thread from a new connection.
- One periodic chart refresh; clicking does not create additional timers.
- The Wi-Fi callback only copies received packets into a queue. File access, output and MQTT are handled later in the main loop.
- All documented `set mqtt...` commands are processed. MQTT web buttons invoke the appropriate commands, and PubSubClient is explicitly included.
- MQTT does not publish invented 0 °C readings for invalid samples. Device failure is reported through Last Will; gateway failure also affects its nodes.
- Remote logging status is read from the sensor packet, not copied from the gateway.
- TCP/MQTT connection waits are bounded. Attempts remain synchronous; this firmware is not a hard real-time logger.
- Commas in Wi-Fi/MQTT passwords are no longer converted to periods. Credentials sent through the GUI are masked in its terminal.
- Storage reserve and complete write results are checked. A failure does not incorrectly increment the local log counter. No automatic formatting.
- New journal lines include an FNV-1a checksum to detect corrupted records; this is not cryptographic authentication.
- Stored data is transmitted wirelessly with acknowledgements and retries. The gateway acknowledges stored packets after writing them to a file. On failure they remain in the local journal and are retransmitted when the gateway becomes reachable.
- A sensor-only device without a router connection can search for the gateway radio channel. When connected to a router, its channel applies; both devices must use the same channel.
- Local records and records received by the gateway can be downloaded over USB.
- Sensor errors are indicated, and sensor initialization is retried.

## Measurement archive and time

New files on the ESP32:

- `/local_v74.csv`: local V74 measurements.
- `/v74_<NODE-ID>.csv`: V74 measurements stored by the gateway.

Old `/local_log.csv` and `/node_<ID>.csv` files are not automatically rewritten or deleted.
The old local file can be retrieved using the “Alte V73 CSV” web link or `dump legacy`.
Export old gateway files before updating; the new download buttons target V74 files.

PC archive: `%LOCALAPPDATA%\BMP280_Logger_V74\measurements.sqlite3`.
It persistently stores valid received values. “Archiv CSV exportieren” exports all archived records.
Charts hold up to **20,000 values per node**. After restarting, the initial load contains the last 20,000 archived records in total.
Older measurements remain in the archive.

V74 includes device ID, boot session, sequence number, relative uptime and, when available, UTC measurement time.
The GUI sets the time on the directly connected device; Wi-Fi devices can also obtain it through NTP.
Without clock synchronization, absolute timestamps are **estimated** and labelled accordingly, especially for old offline measurements downloaded later.
A deep-sleep cycle retains session and sequence; complete power loss begins a new session.

Curves use measurement times rather than sample numbers; long gaps and new sessions interrupt the curve.
A subsequent clock jump may affect chronological display. Synchronize all devices before measuring when accurate time comparisons are required.

## Storage and recovery

“Max Tage” (maximum days) mathematically limits the sample count. It does not guarantee enough flash for seven days.
Capacity depends on interval, file size, old files and node count.
Old measurements are not automatically overwritten.
When storage is full, export files first and then selectively delete them.
Status reports the error; the requested logging setting alone does not prove that writes are succeeding.

A new/unformatted device may report FS FEHLER.
**Only if it contains no files you need**, run `formatfs confirm` in the terminal.
This formats the entire measurement-file area, including old files and gateway files.
Do not use formatting as the first troubleshooting step on an existing device.

Wireless retransmission is limited to local files that still exist.
Deleting a local file also discards its unacknowledged wireless records.
Live readings sent while local recording is stopped are not kept as a retransmittable archive.
The number of encrypted ESP-NOW peers depends on the ESP32 core and may be lower than the 20 manageable trust entries.
