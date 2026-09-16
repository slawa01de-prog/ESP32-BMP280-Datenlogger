# V74.3 test status — prerelease

[🇩🇪 Deutsch](../TESTSTATUS.md) | **🇬🇧 English**

Repository tests check host code and extracted firmware functions.
They do not replace a full ESP32 build or hardware testing.

- Python: 12 tests covering protocol, archive and connection changes.
- C++: journal, wireless acknowledgements and retries.
- C++: USB output buffer with a blocked receiver and recovery.
- C++: hourly history and drawing coordinates (96 scenarios).

Still pending: full V74.3 build with Core 3.3.11, measured flash/RAM usage, upload, operation without a PC, Windows GUI connected to hardware, a 24-hour run, power-loss testing and multiple real wireless nodes.
V74.1 compiled successfully earlier; that is not evidence of a V74.3 build.
The public version also uses a neutral example `PAIR_SECRET`.

## Hardware acceptance

1. Compile with Core 3.3.11, USB CDC and normal optimization; record sizes.
2. Upload without erasing flash; export existing readings first.
3. Connect/disconnect the GUI: encoder, display and measurement must keep running.
4. Start directly from a power supply; check SENSOR/HYBRID, NORMAL and logging ON.
5. Switch temperature/pressure and 1/6/12/24h; test 24-hour operation and CSV export.
6. Restart: CSV remains stored, RAM history starts over.
