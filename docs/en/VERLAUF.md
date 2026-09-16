# V74.3 OLED history — 1/6/12/24 hours

[🇩🇪 Deutsch](../VERLAUF.md) | **🇬🇧 English**

## Installation

Extract the ZIP and open `Esp32_LCD_Datalogger/Esp32_LCD_Datalogger.ino`.
The complete sketch is one file; LoggerConsole.h is not required.
The V74.2 GUI remains compatible. Use ESP32-C3, Core 3.3.11, USB CDC enabled and normal optimization (-Os).
Keep partitioning unchanged and Erase All Flash disabled. No precompiled firmware image is included.

## Graph page controls (page 3)

- Encoder push (OK): switch temperature in C / pressure in hPa.
- Short BAK press: 1h → 6h → 12h → 24h → 1h.
- CONTR: open menu.
- Encoder rotation: change display page.

After changing quantity or time window, the graph page remains selected.
Quantity and time-window preferences are saved.
Temperature/pressure can also be selected through the existing Graph menu item.
Terminal commands: `set graph temp`, `set graph press`;
`set graph_hours 1`, `set graph_hours 6`, `set graph_hours 12`, `set graph_hours 24`.

## Display and data

The time axis is fixed from -1/-6/-12/-24h to now; the left scale is automatic.
The most recent reading per minute is retained for each quantity.
For longer windows, minimum and maximum minute values are drawn per display column.
Brief peaks between minute readings may be missed; this is not a full raw-data plot.
Intervals without data remain blank; new values appear on the right.
The previous 96-reading short-term statistics buffer is retained separately.

Hourly history is a RAM display buffer for the current run.
It starts over after a restart, power loss, deep-sleep restart or statistics reset.
Older CSV readings are not loaded into the OLED graph and remain stored unchanged.
For continuous 24-hour history, use NORMAL or ECO. In ECO, press a button to wake the display.

## Verification

The actual history class and drawing function were host-tested with a simulated OLED:
96 combinations covering empty/short/24-hour data, temperature/pressure, four time windows, DE/EN, negative values and gaps.
The V74.2 USB fix is included.
A full ESP32 build and physical display test are still pending.
Test output is included; check memory usage when compiling.
