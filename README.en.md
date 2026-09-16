# ESP32-BMP280 Data Logger

[🇩🇪 Deutsch](README.md) | **🇬🇧 English**

Temperature and atmospheric pressure logger with ESP32-C3, SH1106 OLED, rotary encoder, CSV storage, ESP-NOW, web interface, OTA and Python desktop GUI.

**Status: V74.3 — prerelease.** Host tests pass; a complete build of this version, hardware testing and 24-hour acceptance testing are still pending. See [test status](docs/en/TESTSTATUS.md).

## Device overview

![ESP32 data logger in its enclosure](docs/images/datenlogger-produktansicht.jpg)

### Display views

![Dashboard, pressure and calibration](docs/images/geraet-menues.jpg)

Product and display previews with example values. These images show German display labels.
[More views: WebGUI and desktop GUI](docs/en/ANSICHTEN.md).

## Features

- Standalone measurement and recording in SENSOR/HYBRID mode.
- Eight OLED pages; only changed display tiles are updated.
- Temperature/pressure history for 1, 6, 12 and 24 hours during continuous operation.
- Buffered USB output prevents a PC that is not reading from blocking the logger.
- Local CSV files, wireless acknowledgements and retransmission of stored data.
- SQLite archive on the PC, charts and CSV export.
- WebGUI and OTA via device IP; MQTT/Home Assistant integration.

![V74 OLED views](docs/images/oled-v74-overview.png)

The preview uses simulated V74 measurements. The graph page was extended in V74.3; the image shows its earlier appearance, not a hardware photograph.

## Hardware

The control module combines an OLED, an EC11 rotary encoder and two buttons on one PCB.

![Wiring diagram](docs/images/wiring-en.svg)

**[Complete wiring and assembly instructions](docs/en/HARDWARE.md)**

| Component | Connection |
|---|---|
| ESP32-C3 SuperMini | 4 MB flash, native USB |
| SH1106 128 × 64 | I²C 0x3C |
| BMP280 | I²C 0x76 |
| SDA / SCL | GPIO8 / GPIO9 |
| Encoder A / B / push | GPIO0 / GPIO1 / GPIO3 |
| BAK / CONTR | GPIO4 / GPIO5 |

BMP280 measures temperature and pressure, not humidity.

### OLED/encoder module used

The blue PCB combines **OLED, EC11 rotary encoder and two buttons**.
The firmware uses **SH1106, 128 × 64 pixels, I²C 0x3C**.
The project owner confirmed the **ESP32-C3 SuperMini and BMP280 modules** used.
[Modules and purchasing sources](docs/en/HARDWARE.md#modules-and-purchasing-sources).

## Enclosure / 3D printing

The project owner identified this Hackster.io STL archive as the enclosure source:

**[Download enclosure STL files (ZIP)](https://hacksterio.s3.amazonaws.com/uploads/attachments/1914724/stl_files_MuvhP6NFu3.zip)**

The source was confirmed on September 15, 2026. The archive is linked as an external original source.

### Building your own

1. Download and extract the ZIP.
2. Open the STL files in a slicer.
3. Before printing, compare dimensions and cutouts with your ESP32-C3 board, OLED, encoder and buttons. Different module variants may require adjustments.

### STL archive contents

The download and ZIP integrity were checked on September 15, 2026.
The archive contains five binary STL files:

| File | X × Y × Z extent in STL units |
|---|---|
| `Estardyn - Back.stl` | 71 × 41 × 19 |
| `Estardyn - Front.stl` | 71 × 41 × 7 |
| `Estardyn - Buttons.stl` | 24 × 8 × 6.4 |
| `Estardyn - Knob.stl` | approx. 9.544 × 9.544 × 14 |
| `Estardyn - ESP32C3.stl` | 24.2 × 35 × 4.2 |

These extents were calculated from triangle coordinates. STL does not store a unit: importing as millimetres makes these numbers millimetres. They are outer extents of individual files, not internal dimensions or verified assembled enclosure dimensions.

Fit, screw sizes and print settings have not been verified. The original project page, designer and enclosure file licence are not yet documented; the ZIP contains only these STL files.

## Firmware installation

1. Download the repository as a ZIP and extract it.
2. Open `Esp32_LCD_Datalogger/Esp32_LCD_Datalogger.ino`. The complete sketch includes the USB console; no additional LoggerConsole.h is needed.
3. Install ESP32 board package **3.3.11** and libraries: **U8g2 2.36.19**, **Adafruit BMP280 Library 3.0.0**, **Adafruit Unified Sensor 1.1.15**, **Adafruit BusIO 1.17.4**, **PubSubClient 2.8**.
4. Select **ESP32C3 Dev Module**, **USB CDC On Boot: Enabled** and the actual board flash size. Keep the existing **Default 4 MB partition scheme**. Set **Erase All Flash: Disabled** and turn **Optimize for Debugging OFF**.
5. Replace `PAIR_SECRET` with the same private value on all your wireless nodes. The published value is a public example, not a secret key. When changing it from older firmware, update and pair all participating nodes again. Do not publish private settings.
6. Compile and upload. For standalone continuous operation, use SENSOR/HYBRID, NORMAL or ECO, with logging enabled.

No precompiled firmware BIN is included.

## Desktop GUI

![Desktop GUI preview](docs/images/pc-gui-vorschau.svg)

Schematic graph-tab preview based on the Python code, using example data and German UI labels.

Install Python 3.10 or newer with Tkinter. On Windows, run `install_requirements.bat`, then `start_gui.bat`. Alternatively:

```sh
python -m pip install -r requirements.txt
python bmp280_logger_v74_gui.py
```

Keep `logger_protocol.py` next to the GUI. Filenames and the archive path retain V74 for compatibility with existing archives. The compatible GUI reports V7.4.2; the firmware reports V7.4.3. On Windows, `build_exe_v74.bat` can build an EXE; a ready-made EXE is not included.

## Graph controls

| On the graph page | Action |
|---|---|
| Press encoder | Temperature ↔ pressure |
| Press BAK | 1h → 6h → 12h → 24h |
| Press CONTR | Open menu |
| Rotate encoder | Change display page |

The most recent reading of each minute is retained.
**RAM history starts again after a restart or deep sleep.**
CSV files remain stored and are not reloaded into this OLED graph.
[History details](docs/en/VERLAUF.md).

## Web, wireless and data

![WebGUI preview](docs/images/webgui-vorschau.svg)

Schematic preview based on the firmware webpage, using example data and German UI labels.

[Operation, archive, timekeeping, MQTT and wireless](docs/en/BEDIENUNG.md).
Open WebGUI/OTA using the IP address; mDNS/.local is not included.
Default web login: `admin` / `admin`; change it before use.
Fallback Wi-Fi: `BMP280-Logger-<ID>`, example password `adminadmin`.
Web and MQTT do not use TLS here; operate on a trusted local network only.
Do not expose WebGUI or OTA through router port forwarding.

## Tests

Python dependencies and g++ are required. From the repository directory:

```sh
python tests/test_v74.py
python tests/test_radio_host.py
python tests/test_console_host.py
python tests/test_graph.py
```

## Versions and licence

[Changelog](CHANGELOG.en.md). The project owner has not yet selected an open-source licence.
Libraries are obtained from their respective package sources and retain their own licences.
